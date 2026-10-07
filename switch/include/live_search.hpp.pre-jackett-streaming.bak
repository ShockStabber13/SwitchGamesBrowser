#pragma once

#include "catalog.hpp"
#include "debrid.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace sgb {

struct LiveSearchProgress {
    std::atomic<size_t> providersTotal{0};
    std::atomic<size_t> providersDone{0};
    std::atomic<size_t> candidatesFound{0};
    std::atomic<size_t> debridTotal{0};
    std::atomic<size_t> debridChecked{0};
    std::atomic<size_t> validFound{0};
    std::atomic<bool> running{false};

    mutable std::mutex textMutex;
    std::string stage;
    std::string currentProvider;

    void reset();
    void setText(const std::string& newStage, const std::string& provider = "");
    std::string stageText() const;
    std::string providerText() const;
};

struct LiveSearchResult {
    std::vector<Release> releases;
    std::map<std::string, DebridTorrentStatus> statuses;
    std::string message;
};

std::set<std::string> enabledProviderIds(const std::string& configPath);

LiveSearchResult runLiveSearch(
    const std::string& query,
    const std::string& configPath,
    const DebridConfig& debridConfig,
    LiveSearchProgress& progress
);

} // namespace sgb