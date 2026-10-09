#include "provider.hpp"
#include "provider_api_utils.hpp"
#include "provider_stats.hpp"

#include <regex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct TorznabPage {
    std::vector<sgb::ProviderResult> results;
    size_t rawItems = 0;
};

TorznabPage parseTorznab(
    const std::string& xml,
    std::unordered_set<std::string>& seen)
{
    TorznabPage page;

    static const std::regex itemRe(
        R"(<item\b[\s\S]*?</item>)",
        std::regex::icase);

    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), itemRe);
         it != std::sregex_iterator(); ++it) {
        ++page.rawItems;
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
        result.seeders = sgb_stats::seeders(item);
        result.leechers = sgb_stats::leechers(item);
        page.results.push_back(std::move(result));
    }

    return page;
}

} // namespace

class TorlockProvider final : public sgb::Provider {
public:
    std::string id() const override { return "torlock"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;
        const std::string clean = sgb_api::trim(query);
        if (clean.empty())
            return results;

        constexpr size_t pageSize = 100;
        size_t offset = 0;
        std::unordered_set<std::string> seen;

        while (true) {
            const std::string url =
                "https://www.torlock.com/torznab/api?t=search&q=" +
                sgb_api::urlEncode(clean) +
                "&limit=" + std::to_string(pageSize) +
                "&offset=" + std::to_string(offset);

            const std::string xml = sgb_api::httpGet(
                url,
                "application/rss+xml, application/xml, text/xml");

            TorznabPage page = parseTorznab(xml, seen);

            if (page.rawItems == 0)
                break;

            const size_t newResults = page.results.size();

            results.insert(
                results.end(),
                std::make_move_iterator(page.results.begin()),
                std::make_move_iterator(page.results.end()));

            if (page.rawItems < pageSize || newResults == 0)
                break;

            offset += pageSize;
        }

        return results;
    }
};

SGB_REGISTER_PROVIDER(TorlockProvider);
