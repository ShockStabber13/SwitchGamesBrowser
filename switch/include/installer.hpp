#pragma once

#include "debrid.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
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

    std::atomic<std::uint64_t>
        bytesDone{0};

    std::atomic<std::uint64_t>
        bytesTotal{0};

    std::atomic<std::uint64_t>
        bytesPerSecond{0};

    mutable std::mutex mutex;
    std::string stage = "Queued";
    std::string detail;

    void set(
        const std::string& newStage,
        int newPercent,
        const std::string& newDetail = "");

    void beginTransfer(
        std::uint64_t totalBytes);

    void addTransferBytes(
        std::uint64_t bytes);

    void snapshot(
        std::string& outStage,
        int& outPercent,
        std::string& outDetail) const;

    void snapshotTransfer(
        std::uint64_t& outDone,
        std::uint64_t& outTotal,
        std::uint64_t& outBytesPerSecond) const;

private:
    std::chrono::steady_clock::time_point
        transferSampleStarted_{};

    std::uint64_t
        transferSampleBytes_ = 0;
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
