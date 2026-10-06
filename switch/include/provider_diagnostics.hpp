#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sgb {

struct ProviderDiagnostic {
    std::string id;
    long httpStatus = 0;
    std::string finalUrl;
    size_t requests = 0;
    size_t responseBytes = 0;
    size_t candidates = 0;
    std::string error;
    bool completed = false;
};

void resetProviderDiagnostics();

void beginProviderDiagnostics(
    const std::string& providerId
);

void recordProviderHttp(
    long httpStatus,
    const std::string& finalUrl,
    size_t responseBytes,
    const std::string& error
);

void endProviderDiagnostics(
    size_t candidates,
    const std::string& error
);

std::vector<ProviderDiagnostic>
providerDiagnosticsSnapshot();

} // namespace sgb