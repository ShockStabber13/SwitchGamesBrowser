#pragma once
#include "installer.hpp"

namespace sgb {

// SwitchGamesBrowser keeps the proven installer as the default.
// A real YATI port must implement runSphairaYatiInstallJob with the same
// contract, using Sphaira's original source and storage pipeline.
enum class InstallerBackend {
    Original,
    SphairaYati,
};

bool isInstallerBackendAvailable(InstallerBackend backend);

// The only call site in Install Manager is routed through this interface.
// No YATI backend is advertised or selected until it compiles and links.
InstallResult dispatchInstallJob(
    InstallerBackend backend,
    const DebridConfig& config,
    const InstallJob& job,
    const std::string& cacheDirectory,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancelRequested
);

} // namespace sgb
