#pragma once

#include "catalog.hpp"
#include "debrid.hpp"

#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace sgb {

struct ScrapeCacheInfo {
    bool available = false;
    std::time_t savedAt = 0;
    size_t releaseCount = 0;
};

bool saveScrapeCache(
    const std::string& root,
    const std::string& gameTitle,
    DebridService debridService,
    const std::vector<Release>& releases,
    const std::map<std::string, DebridTorrentStatus>& statuses);

bool loadScrapeCache(
    const std::string& root,
    const std::string& gameTitle,
    DebridService debridService,
    std::vector<Release>& releases,
    std::map<std::string, DebridTorrentStatus>& statuses,
    std::time_t* savedAt = nullptr);

ScrapeCacheInfo scrapeCacheInfo(
    const std::string& root,
    const std::string& gameTitle,
    DebridService debridService);

std::string scrapeCacheAgeText(std::time_t savedAt);

} // namespace sgb