#include "scrape_cache.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace sgb {
namespace {

using Json = nlohmann::json;

uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    for (unsigned char c : value) {
        hash ^= static_cast<uint64_t>(c);
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

std::string cacheKey(const std::string& title)
{
    std::ostringstream out;
    out << std::hex
        << std::setfill('0')
        << std::setw(16)
        << fnv1a64(title);

    return out.str();
}

bool ensureDir(const std::string& path)
{
    if (::mkdir(path.c_str(), 0777) == 0)
        return true;

    return errno == EEXIST;
}

std::string serviceFolder(DebridService service)
{
    switch (service) {
        case DebridService::TorBox:
            return "torbox";
        case DebridService::AllDebrid:
            return "alldebrid";
        default:
            return "none";
    }
}

std::string serviceDirectory(
    const std::string& root,
    DebridService service)
{
    return root + "cache/" + serviceFolder(service);
}

std::string cacheDirectory(
    const std::string& root,
    DebridService service)
{
    return serviceDirectory(root, service) + "/scrapes";
}

std::string cachePath(
    const std::string& root,
    const std::string& title,
    DebridService service)
{
    return cacheDirectory(root, service) +
        "/" +
        cacheKey(title) +
        ".json";
}

bool prepareCacheDirectory(
    const std::string& root,
    DebridService service)
{
    if (!ensureDir(root + "cache"))
        return false;

    if (!ensureDir(serviceDirectory(root, service)))
        return false;

    return ensureDir(cacheDirectory(root, service));
}

Json releaseToJson(const Release& release)
{
    return Json{
        {"title", release.title},
        {"magnet", release.magnet},
        {"infoHash", release.infoHash},
        {"source", release.source}
    };
}

bool releaseFromJson(
    const Json& item,
    Release& release)
{
    if (!item.is_object())
        return false;

    release.title =
        item.value(
            "title",
            std::string{});

    release.magnet =
        item.value(
            "magnet",
            std::string{});

    release.infoHash =
        item.value(
            "infoHash",
            std::string{});

    release.source =
        item.value(
            "source",
            std::string{});

    return
        !release.title.empty() &&
        (
            !release.infoHash.empty() ||
            !release.magnet.empty()
        );
}

bool readCache(
    const std::string& root,
    const std::string& gameTitle,
    DebridService debridService,
    Json& json)
{
    std::ifstream file(
        cachePath(
            root,
            gameTitle,
            debridService),
        std::ios::binary);

    if (!file)
        return false;

    json =
        Json::parse(
            file,
            nullptr,
            false);

    if (
        json.is_discarded() ||
        !json.is_object()
    ) {
        return false;
    }

    if (json.value("version", 0) != 2)
        return false;

    if (
        json.value(
            "gameTitle",
            std::string{}) !=
        gameTitle
    ) {
        return false;
    }

    if (
        json.value(
            "debridService",
            std::string{}) !=
        serviceFolder(debridService)
    ) {
        return false;
    }

    return
        json.contains("releases") &&
        json["releases"].is_array() &&
        json.contains("statuses") &&
        json["statuses"].is_object();
}

} // namespace

bool saveScrapeCache(
    const std::string& root,
    const std::string& gameTitle,
    DebridService debridService,
    const std::vector<Release>& releases,
    const std::map<std::string, DebridTorrentStatus>& statuses)
{
    if (!prepareCacheDirectory(root, debridService))
        return false;

    Json rows = Json::array();

    for (const auto& release : releases)
        rows.push_back(releaseToJson(release));

    Json statusJson =
        Json::parse(
            serializeDebridStatusJson(statuses),
            nullptr,
            false);

    if (
        statusJson.is_discarded() ||
        !statusJson.is_object()
    ) {
        statusJson = Json::object();
    }

    Json rootJson{
        {"version", 2},
        {"gameTitle", gameTitle},
        {"debridService", serviceFolder(debridService)},
        {
            "savedAt",
            static_cast<long long>(
                std::time(nullptr))
        },
        {"releases", std::move(rows)},
        {"statuses", std::move(statusJson)}
    };

    const std::string target =
        cachePath(
            root,
            gameTitle,
            debridService);

    const std::string temporary =
        target + ".tmp";

    {
        std::ofstream file(
            temporary,
            std::ios::binary |
            std::ios::trunc);

        if (!file)
            return false;

        const std::string payload =
            rootJson.dump();

        file.write(
            payload.data(),
            static_cast<std::streamsize>(
                payload.size()));

        file.flush();

        if (!file)
            return false;
    }

    std::remove(target.c_str());

    if (
        std::rename(
            temporary.c_str(),
            target.c_str()) != 0
    ) {
        std::remove(
            temporary.c_str());

        return false;
    }

    return true;
}

bool loadScrapeCache(
    const std::string& root,
    const std::string& gameTitle,
    DebridService debridService,
    std::vector<Release>& releases,
    std::map<std::string, DebridTorrentStatus>& statuses,
    std::time_t* savedAt)
{
    Json json;

    if (
        !readCache(
            root,
            gameTitle,
            debridService,
            json)
    ) {
        return false;
    }

    std::vector<Release> loaded;

    for (const auto& item : json["releases"]) {
        Release release;

        if (releaseFromJson(item, release))
            loaded.push_back(
                std::move(release));
    }

    statuses =
        parseDebridStatusJson(
            json["statuses"].dump());

    if (savedAt) {
        *savedAt =
            static_cast<std::time_t>(
                json.value(
                    "savedAt",
                    0LL));
    }

    releases =
        std::move(loaded);

    return true;
}

ScrapeCacheInfo scrapeCacheInfo(
    const std::string& root,
    const std::string& gameTitle,
    DebridService debridService)
{
    ScrapeCacheInfo info;

    Json json;

    if (
        !readCache(
            root,
            gameTitle,
            debridService,
            json)
    ) {
        return info;
    }

    info.available = true;

    info.savedAt =
        static_cast<std::time_t>(
            json.value(
                "savedAt",
                0LL));

    info.releaseCount =
        json["releases"].size();

    return info;
}

std::string scrapeCacheAgeText(
    std::time_t savedAt)
{
    if (savedAt <= 0)
        return "unknown age";

    const std::time_t now =
        std::time(nullptr);

    if (now <= savedAt)
        return "just now";

    const long long seconds =
        static_cast<long long>(
            now - savedAt);

    if (seconds < 60)
        return "just now";

    const long long minutes =
        seconds / 60;

    if (minutes < 60)
        return
            std::to_string(minutes) +
            "m ago";

    const long long hours =
        minutes / 60;

    if (hours < 48)
        return
            std::to_string(hours) +
            "h ago";

    const long long days =
        hours / 24;

    return
        std::to_string(days) +
        "d ago";
}

} // namespace sgb