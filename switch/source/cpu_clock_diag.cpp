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


namespace {
constexpr u32 kBoostHz = 1224000000u;
constexpr std::array<u32, 4> kAllowedCpuPresets{{
    1224000000u, 1326000000u, 1428000000u, 1581000000u
}};
constexpr auto kTrialDuration = std::chrono::seconds(10);

std::string mhz(u32 hz) {
    char value[64]{};
    std::snprintf(value, sizeof(value), "%.1f MHz",
                  static_cast<double>(hz) / 1000000.0);
    return value;
}
} // namespace

void CpuClockBoostTrial::close() {
    if (sessionOpen_) {
        clkrstCloseSession(&session_);
        sessionOpen_ = false;
    }
    if (initialized_) {
        clkrstExit();
        initialized_ = false;
    }
    active_ = false;
    holdUntilChanged_ = false;
    targetHz_ = 0;
}

CpuClockBoostTrial::~CpuClockBoostTrial() {
    if (active_) {
        // Best effort when app exits through unexpected C++ control flow.
        stop("Shutdown");
    } else {
        close();
    }
}

std::vector<std::string> CpuClockBoostTrial::begin(u32 requestedHz, bool holdUntilChanged) {
    if (active_) {
        return {"Test already running: please wait for auto-restore."};
    }

    if (std::find(kAllowedCpuPresets.begin(), kAllowedCpuPresets.end(),
                  requestedHz) == kAllowedCpuPresets.end()) {
        return {"Unsupported CPU preset. No change applied."};
    }
    std::vector<std::string> result{
        std::string("CPU target: ") + mhz(requestedHz) +
        (holdUntilChanged ? " (saved app-session mode)" : " (10-second test)")
    };

    const Result initialized = clkrstInitialize();
    if (R_FAILED(initialized)) {
        result.push_back(resultLine("clkrstInitialize", initialized));
        result.emplace_back("Not applied. No CPU settings changed.");
        return result;
    }
    initialized_ = true;
    const Result opened = clkrstOpenSession(
        &session_, PcvModuleId_CpuBus, 3);
    if (R_FAILED(opened)) {
        result.push_back(resultLine("clkrstOpenSession", opened));
        close();
        return result;
    }
    sessionOpen_ = true;

    const Result read = clkrstGetClockRate(&session_, &originalHz_);
    if (R_FAILED(read)) {
        result.push_back(resultLine("Get original CPU rate", read));
        result.emplace_back("Not applied: original clock is unknown.");
        close();
        return result;
    }
    result.emplace_back("Original clock: " + mhz(originalHz_));

    // Do not downclock an already boosted CPU for this test.
    if (!holdUntilChanged && originalHz_ >= requestedHz) {
        result.emplace_back("Already at/above requested CPU rate; no change made.");
        close();
        return result;
    }

    std::array<u32, 64> rates{};
    PcvClockRatesListType listType = PcvClockRatesListType_Invalid;
    s32 count = 0;
    const Result listed = clkrstGetPossibleClockRates(
        &session_, rates.data(), static_cast<s32>(rates.size()),
        &listType, &count);
    if (R_FAILED(listed)) {
        result.push_back(resultLine("Get allowed CPU rates", listed));
        result.emplace_back("Not applied: cannot validate requested rate.");
        close();
        return result;
    }
    if (count < 0 || static_cast<size_t>(count) > rates.size() ||
        listType != PcvClockRatesListType_Discrete ||
        std::find(rates.begin(), rates.begin() + count, requestedHz) ==
            rates.begin() + count) {
        result.emplace_back("Requested MHz absent from discrete CPU clock list.");
        result.emplace_back("Not applied. No CPU settings changed.");
        close();
        return result;
    }

    const Result set = clkrstSetClockRate(&session_, requestedHz);
    u32 actualHz = 0;
    const Result verify = clkrstGetClockRate(&session_, &actualHz);
    if (R_FAILED(set)) {
        result.push_back(resultLine("Set CPU clock failed", set));
        if (R_FAILED(verify) || actualHz != originalHz_) {
            result.emplace_back("Clock unverified / changed; restoring now.");
            const Result restore = clkrstSetClockRate(
                &session_, originalHz_);
            result.emplace_back(R_SUCCEEDED(restore)
                ? "Restore request accepted."
                : resultLine("Restore failed", restore));
        } else {
            result.emplace_back("Request rejected; no boost confirmed.");
        }
        close();
        return result;
    }

    // Even if a readback fails, an accepted write must be restored.
    active_ = true;
    deadline_ = std::chrono::steady_clock::now() + kTrialDuration;
    targetHz_ = requestedHz;
    holdUntilChanged_ = holdUntilChanged;
    result.emplace_back("Set CPU clock: accepted by service.");
    if (R_SUCCEEDED(verify)) {
        result.emplace_back("CPU clock readback: " + mhz(actualHz));
        if (actualHz != requestedHz)
            result.emplace_back("Warning: requested and actual clocks differ.");
    } else {
        result.push_back(resultLine("Clock readback failed", verify));
    }
    result.emplace_back(holdUntilChanged
        ? "Held while app runs; restores on exit or next change."
        : "Auto-restore in 10s; B/+/exit restores early.");
    return result;
}

unsigned CpuClockBoostTrial::secondsRemaining() const {
    if (!active_ || holdUntilChanged_) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline_) return 0;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline_ - now).count();
    return static_cast<unsigned>((left + 999) / 1000);
}

std::vector<std::string> CpuClockBoostTrial::tick() {
    if (!active_ || holdUntilChanged_ || std::chrono::steady_clock::now() < deadline_)
        return {};
    return stop("10-second timer finished");
}

std::vector<std::string> CpuClockBoostTrial::stop(const char* reason) {
    if (!active_) return {};
    std::vector<std::string> result{
        std::string("Restoring CPU: ") + reason
    };
    const Result restored = clkrstSetClockRate(&session_, originalHz_);
    if (R_SUCCEEDED(restored))
        result.emplace_back("Restore request: accepted");
    else
        result.push_back(resultLine("Restore request FAILED", restored));
    u32 actualHz = 0;
    const Result verify = clkrstGetClockRate(&session_, &actualHz);
    if (R_SUCCEEDED(verify)) {
        result.emplace_back("CPU clock readback: " + mhz(actualHz));
        result.emplace_back(actualHz == originalHz_
            ? "Original CPU clock verified."
            : "WARNING: original CPU clock NOT verified!");
    } else {
        result.push_back(resultLine("Restore verification FAILED", verify));
    }
    close();
    return result;
}

} // namespace sgb
