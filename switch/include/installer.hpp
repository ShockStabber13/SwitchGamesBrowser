#pragma once

#include "debrid.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <utility>
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

    // Measured independently of content-storage writes.
    std::atomic<std::uint64_t> networkBytesPerSecond{0};
    std::atomic<std::uint64_t> networkBytesDone{0};

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

    void addNetworkBytes(
        std::uint64_t bytes);

    void snapshot(
        std::string& outStage,
        int& outPercent,
        std::string& outDetail) const;

    void snapshotTransfer(
        std::uint64_t& outDone,
        std::uint64_t& outTotal,
        std::uint64_t& outBytesPerSecond,
        std::uint64_t& outNetworkBytesPerSecond) const;

private:
    std::chrono::steady_clock::time_point
        transferSampleStarted_{};

    std::uint64_t
        transferSampleBytes_ = 0;

    std::chrono::steady_clock::time_point networkSampleStarted_{};
    std::uint64_t networkSampleBytes_ = 0;

    // Time-stamped totals for a real-time, sliding-window speed display.
    mutable std::deque<std::pair<
        std::chrono::steady_clock::time_point,
        std::uint64_t>> liveSpeedSamples_;
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
