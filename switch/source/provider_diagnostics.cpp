#include "provider_diagnostics.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sgb {
namespace {

std::mutex diagnosticsMutex;

std::map<std::string, ProviderDiagnostic>
diagnostics;

thread_local std::string
currentProviderId;

} // namespace

void resetProviderDiagnostics() {
    std::lock_guard<std::mutex> lock(
        diagnosticsMutex
    );

    diagnostics.clear();
}

void beginProviderDiagnostics(
    const std::string& providerId
) {
    currentProviderId = providerId;

    std::lock_guard<std::mutex> lock(
        diagnosticsMutex
    );

    ProviderDiagnostic diagnostic;
    diagnostic.id = providerId;

    diagnostics[providerId] =
        std::move(diagnostic);
}

void recordProviderHttp(
    long httpStatus,
    const std::string& finalUrl,
    size_t responseBytes,
    const std::string& error
) {
    if (currentProviderId.empty())
        return;

    std::lock_guard<std::mutex> lock(
        diagnosticsMutex
    );

    auto& diagnostic =
        diagnostics[currentProviderId];

    diagnostic.id =
        currentProviderId;

    diagnostic.httpStatus =
        httpStatus;

    diagnostic.finalUrl =
        finalUrl;

    ++diagnostic.requests;

    diagnostic.responseBytes +=
        responseBytes;

    if (!error.empty())
        diagnostic.error = error;
}

void endProviderDiagnostics(
    size_t candidates,
    const std::string& error
) {
    if (currentProviderId.empty())
        return;

    std::lock_guard<std::mutex> lock(
        diagnosticsMutex
    );

    auto& diagnostic =
        diagnostics[currentProviderId];

    diagnostic.id =
        currentProviderId;

    diagnostic.candidates =
        candidates;

    diagnostic.completed = true;

    if (!error.empty())
        diagnostic.error = error;

    currentProviderId.clear();
}

std::vector<ProviderDiagnostic>
providerDiagnosticsSnapshot() {
    std::lock_guard<std::mutex> lock(
        diagnosticsMutex
    );

    std::vector<ProviderDiagnostic> out;
    out.reserve(diagnostics.size());

    for (const auto& pair : diagnostics)
        out.push_back(pair.second);

    return out;
}

} // namespace sgb