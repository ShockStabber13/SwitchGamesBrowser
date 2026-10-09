#include "provider.hpp"
#include "provider_api_utils.hpp"
#include "provider_stats.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

struct BitmagnetConfig {
    std::string url;
};

BitmagnetConfig loadBitmagnetConfig()
{
    constexpr const char* path =
        "sdmc:/switch/SwitchGamesBrowser/bitmagnet.json";

    std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error(
            "Missing SD:/switch/SwitchGamesBrowser/bitmagnet.json");
    }

    nlohmann::json root;
    file >> root;

    BitmagnetConfig config;
    config.url =
        sgb_api::trim(
            root.value("url", std::string{}));

    while (!config.url.empty() && config.url.back() == '/')
        config.url.pop_back();

    if (config.url.empty())
        throw std::runtime_error("Bitmagnet URL is empty");

    if (
        config.url.rfind("http://", 0) != 0 &&
        config.url.rfind("https://", 0) != 0
    ) {
        throw std::runtime_error(
            "Bitmagnet URL must start with http:// or https://");
    }

    return config;
}

class BitmagnetProvider final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "bitmagnet";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;

        const std::string clean =
            sgb_api::trim(query);

        if (clean.empty())
            return results;

        const BitmagnetConfig config =
            loadBitmagnetConfig();

        // Bitmagnet exposes a Torznab-compatible API at /torznab.
        const std::string url =
            config.url +
            "/torznab/api?t=search&q=" +
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
            auto it = std::sregex_iterator(
                xml.begin(),
                xml.end(),
                itemRe);
            it != std::sregex_iterator();
            ++it
        ) {
            const std::string item =
                it->str();

            const std::string title =
                sgb_api::trim(
                    sgb_api::tagValue(
                        item,
                        "title"));

            std::string hash =
                sgb_api::lowerAscii(
                    sgb_api::trim(
                        sgb_api::torznabAttr(
                            item,
                            "infohash")));

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

            const std::string link =
                sgb_api::trim(
                    sgb_api::tagValue(
                        item,
                        "link"));

            const std::string guid =
                sgb_api::trim(
                    sgb_api::tagValue(
                        item,
                        "guid"));

            // Bitmagnet sets <guid> to the v1 info hash.
            if (
                !sgb_api::validInfoHash(hash) &&
                sgb_api::validInfoHash(guid)
            ) {
                hash =
                    sgb_api::lowerAscii(guid);
            }

            if (
                magnet.empty() &&
                link.rfind("magnet:", 0) == 0
            ) {
                magnet = link;
            }

            // Bitmagnet also places its magnet URI in the enclosure.
            if (magnet.empty()) {
                static const std::regex enclosureRe(
                    R"(<enclosure\b[^>]*\burl=["']([^"']+)["'][^>]*>)",
                    std::regex::icase);

                std::smatch enclosureMatch;

                if (
                    std::regex_search(
                        item,
                        enclosureMatch,
                        enclosureRe)
                ) {
                    magnet =
                        sgb_api::xmlDecode(
                            enclosureMatch[1].str());
                }
            }

            if (
                hash.empty() &&
                !magnet.empty()
            ) {
                hash =
                    sgb_api::hashFromMagnet(
                        magnet);
            }

            if (
                title.empty() ||
                !sgb_api::validInfoHash(hash)
            ) {
                continue;
            }

            hash = sgb_api::lowerAscii(hash);

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
            row.infoHash = hash;
            row.magnet = magnet;
            row.seeders = sgb_stats::seeders(item);
            row.leechers = sgb_stats::leechers(item);

            results.push_back(
                std::move(row));
        }

        return results;
    }
};

} // namespace

SGB_REGISTER_PROVIDER(BitmagnetProvider);

