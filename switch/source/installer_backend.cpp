#include "installer_backend.hpp"

namespace sgb {

#ifdef SGB_YATI_LINKED
// Implemented by the genuine Sphaira/YATI backend after its dependencies,
// progress bridge, and license integration are complete.
InstallResult runSphairaYatiInstallJob(
    const DebridConfig& config,
    const InstallJob& job,
    const std::string& cacheDirectory,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancelRequested);
#endif

bool isInstallerBackendAvailable(InstallerBackend backend) {
    switch (backend) {
    case InstallerBackend::Original:
        return true;
    case InstallerBackend::SphairaYati:
#ifdef SGB_YATI_LINKED
        return true;
#else
        return false;
#endif
    }
    return false;
}

InstallResult dispatchInstallJob(
    InstallerBackend backend,
    const DebridConfig& config,
    const InstallJob& job,
    const std::string& cacheDirectory,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancelRequested)
{
    switch (backend) {
    case InstallerBackend::Original:
        return runInstallJob(
            config, job, cacheDirectory, progress, cancelRequested);
    case InstallerBackend::SphairaYati:
#ifdef SGB_YATI_LINKED
        return runSphairaYatiInstallJob(
            config, job, cacheDirectory, progress, cancelRequested);
#else
        return {false, false, "Sphaira YATI backend is not linked"};
#endif
    }
    return {false, false, "Unknown installer backend"};
}

} // namespace sgb
