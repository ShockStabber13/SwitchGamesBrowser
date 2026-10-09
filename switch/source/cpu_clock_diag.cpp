#include "cpu_clock_diag.hpp"

#include <switch.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace sgb {
namespace {

std::string resultLine(const char* operation, Result rc) {
    char message[160]{};
    std::snprintf(message, sizeof(message), "%s: 0x%08lX",
                  operation, static_cast<unsigned long>(rc));
    return message;
}

} // namespace

std::vector<std::string> probeCpuClockReadOnly() {
    std::vector<std::string> lines;
    lines.emplace_back("Clkrst CPU read-only capability test");
    lines.emplace_back("SwitchGamesBrowser already uses FastLoad during installs.");

    // Service access can be restricted to privileged processes on some
    // firmware/configurations. An error is a useful test result, not a crash.
    const Result init = clkrstInitialize();
    if (R_FAILED(init)) {
        lines.push_back(resultLine("clkrstInitialize failed", init));
        lines.emplace_back("Clock service unavailable or permission denied.");
        lines.emplace_back("No clocks or voltages were changed.");
        return lines;
    }
    lines.emplace_back("clkrstInitialize: OK");

    ClkrstSession session{};
    const Result opened = clkrstOpenSession(
        &session, PcvModuleId_CpuBus, 3);
    if (R_FAILED(opened)) {
        lines.push_back(resultLine("clkrstOpenSession failed", opened));
        lines.emplace_back("CPU clock session cannot be opened.");
        clkrstExit();
        return lines;
    }
    lines.emplace_back("CPU clock session: OPEN");

    u32 currentHz = 0;
    const Result read = clkrstGetClockRate(&session, &currentHz);
    if (R_SUCCEEDED(read)) {
        lines.emplace_back("Current CPU: " +
                           std::to_string(currentHz / 1000000) + " MHz");
    } else {
        lines.push_back(resultLine("clkrstGetClockRate failed", read));
    }

    std::array<u32, 64> frequencies{};
    PcvClockRatesListType listType = PcvClockRatesListType_Invalid;
    s32 frequencyCount = 0;
    const Result listed = clkrstGetPossibleClockRates(
        &session, frequencies.data(),
        static_cast<s32>(frequencies.size()),
        &listType, &frequencyCount);

    if (R_SUCCEEDED(listed)) {
        if (frequencyCount < 0 ||
            static_cast<size_t>(frequencyCount) > frequencies.size()) {
            lines.emplace_back("Clock service returned invalid list size.");
        } else {
            lines.emplace_back("Available clocks: " +
                               std::to_string(frequencyCount) +
                               (listType == PcvClockRatesListType_Range
                                ? " (range)" : " (discrete)"));
            const size_t shown = std::min<size_t>(
                static_cast<size_t>(frequencyCount), 5);
            for (size_t i = 0; i < shown; ++i) {
                lines.emplace_back("  " + std::to_string(i + 1) +
                                   ": " + std::to_string(
                                       frequencies[i] / 1000000) + " MHz");
            }
            if (static_cast<size_t>(frequencyCount) > shown)
                lines.emplace_back("  ... additional rates omitted");
        }
    } else {
        lines.push_back(resultLine(
            "clkrstGetPossibleClockRates failed", listed));
    }

    clkrstCloseSession(&session);
    clkrstExit();
    lines.emplace_back("Read-only test finished; no overrides applied.");
    return lines;
}
