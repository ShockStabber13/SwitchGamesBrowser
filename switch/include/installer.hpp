#pragma once

#include "debrid.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace sgb {

struct InstallJob {
    std::string id;
    std::string gameTitle;
    std::string releaseTitle;
    std::string infoHash;
    std::string source;
    std::string remoteId;
    DebridFile file;
};

struct InstallProgress {
    std::atomic<int> percent{0};
    std::atomic<bool> running{false};

    mutable std::mutex mutex;
    std::string stage = "Queued";
    std::string detail;

    void set(
        const std::string& newStage,
        int newPercent,
        const std::string& newDetail = "");

    void snapshot(
        std::string& outStage,
        int& outPercent,
        std::string& outDetail) const;
};

struct InstallResult {
    bool success = false;
    bool cancelled = false;
    std::string message;
};

InstallResult runInstallJob(
    const DebridConfig& config,
    const InstallJob& job,
    const std::string& cacheDirectory,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancelRequested
);

} // namespace sgb
