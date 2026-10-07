#include "provider.hpp"
#include "provider_api_utils.hpp"

#include <regex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

std::vector<sgb::ProviderResult> parseTorznab(const std::string& xml)
{
    std::vector<sgb::ProviderResult> results;
    std::unordered_set<std::string> seen;

    static const std::regex itemRe(
        R"(<item\b[\s\S]*?</item>)",
        std::regex::icase);

    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), itemRe);
         it != std::sregex_iterator(); ++it) {
        const std::string item = it->str();

        const std::string title = sgb_api::trim(
            sgb_api::tagValue(item, "title"));

        std::string magnet = sgb_api::torznabAttr(item, "magneturl");
        if (magnet.empty())
            magnet = sgb_api::torznabAttr(item, "magnet");

        if (magnet.empty()) {
            const std::string link = sgb_api::trim(
                sgb_api::tagValue(item, "link"));
            if (link.rfind("magnet:", 0) == 0)
                magnet = link;
        }

        if (magnet.empty()) {
            const std::string guid = sgb_api::trim(
                sgb_api::tagValue(item, "guid"));
            if (guid.rfind("magnet:", 0) == 0)
                magnet = guid;
        }

        std::string hash = sgb_api::lowerAscii(sgb_api::trim(
            sgb_api::torznabAttr(item, "infohash")));

        if (hash.empty() && !magnet.empty())
            hash = sgb_api::hashFromMagnet(magnet);

        if (title.empty() || !sgb_api::validInfoHash(hash))
            continue;

        if (!seen.insert(hash).second)
            continue;

        if (magnet.empty())
            magnet = sgb_api::magnetFromHash(hash, title);

        sgb::ProviderResult result;
        result.title = title;
        result.magnet = magnet;
        result.infoHash = hash;
        results.push_back(std::move(result));
    }

    return results;
}

} // namespace

class TorlockProvider final : public sgb::Provider {
public:
    std::string id() const override { return "torlock"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        const std::string clean = sgb_api::trim(query);
        if (clean.empty())
            return {};

        const std::string url =
            "https://www.torlock.com/torznab/api?t=search&q=" +
            sgb_api::urlEncode(clean) +
            "&limit=100&offset=0";

        return parseTorznab(
            sgb_api::httpGet(url, "application/rss+xml, application/xml, text/xml"));
    }
};

SGB_REGISTER_PROVIDER(TorlockProvider);