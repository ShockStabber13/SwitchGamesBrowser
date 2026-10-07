#pragma once

#include "provider.hpp"
#include "provider_api_utils.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sgb_jackett {

struct Config {
    std::string url;
    std::string apiKey;
};

inline Config loadConfig()
{
    constexpr const char* path =
        "sdmc:/switch/SwitchGamesBrowser/jackett.json";

    std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error(
            "Missing SD:/switch/SwitchGamesBrowser/jackett.json");
    }

    nlohmann::json root;
    file >> root;

    Config config;
    config.url =
        sgb_api::trim(
            root.value("url", std::string{}));
    config.apiKey =
        sgb_api::trim(
            root.value("apiKey", std::string{}));

    while (!config.url.empty() && config.url.back() == '/')
        config.url.pop_back();

    if (config.url.empty())
        throw std::runtime_error("Jackett URL is empty");

    if (config.apiKey.empty())
        throw std::runtime_error("Jackett API key is empty");

    if (
        config.url.rfind("http://", 0) != 0 &&
        config.url.rfind("https://", 0) != 0
    ) {
        throw std::runtime_error(
            "Jackett URL must start with http:// or https://");
    }

    return config;
}

inline std::vector<sgb::ProviderResult> search(
    const std::string& indexerId,
    const std::string& query)
{
    std::vector<sgb::ProviderResult> results;

    const std::string clean = sgb_api::trim(query);

    if (clean.empty())
        return results;

    const Config config = loadConfig();

    const std::string url =
        config.url +
        "/api/v2.0/indexers/" +
        sgb_api::urlEncode(indexerId) +
        "/results/torznab/api?apikey=" +
        sgb_api::urlEncode(config.apiKey) +
        "&t=search&q=" +
        sgb_api::urlEncode(clean);

    const std::string xml =
        sgb_api::httpGet(
            url,
            "application/rss+xml, application/xml, text/xml, */*");

    static const std::regex itemRe(
        R"(<item\b[\s\S]*?</item>)",
        std::regex::icase);

    std::unordered_set<std::string> seen;

    for (
        auto it =
            std::sregex_iterator(
                xml.begin(),
                xml.end(),
                itemRe);
        it != std::sregex_iterator();
        ++it
    ) {
        const std::string item = it->str();

        const std::string title =
            sgb_api::trim(
                sgb_api::tagValue(item, "title"));

        std::string magnet =
            sgb_api::trim(
                sgb_api::torznabAttr(
                    item,
                    "magneturl"));

        if (magnet.empty()) {
            magnet =
                sgb_api::trim(
                    sgb_api::torznabAttr(
                        item,
                        "magnet"));
        }

        if (magnet.empty()) {
            const std::string link =
                sgb_api::trim(
                    sgb_api::tagValue(
                        item,
                        "link"));

            if (link.rfind("magnet:", 0) == 0)
                magnet = link;
        }

        if (magnet.empty()) {
            const std::string guid =
                sgb_api::trim(
                    sgb_api::tagValue(
                        item,
                        "guid"));

            if (guid.rfind("magnet:", 0) == 0)
                magnet = guid;
        }

        std::string hash =
            sgb_api::lowerAscii(
                sgb_api::trim(
                    sgb_api::torznabAttr(
                        item,
                        "infohash")));

        if (hash.empty() && !magnet.empty())
            hash = sgb_api::hashFromMagnet(magnet);

        if (
            title.empty() ||
            !sgb_api::validInfoHash(hash)
        ) {
            continue;
        }

        if (!seen.insert(hash).second)
            continue;

        if (magnet.empty()) {
            magnet =
                sgb_api::magnetFromHash(
                    hash,
                    title);
        }

        sgb::ProviderResult row;
        row.title = title;
        row.magnet = magnet;
        row.infoHash = hash;
        results.push_back(std::move(row));
    }

    return results;
}

} // namespace sgb_jackett