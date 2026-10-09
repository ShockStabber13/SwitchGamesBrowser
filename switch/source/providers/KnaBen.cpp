#include "provider.hpp"
#include "provider_api_utils.hpp"
#include "provider_stats.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;

std::string stringField(
    const Json& item,
    const char* a,
    const char* b = nullptr,
    const char* c = nullptr)
{
    if (item.contains(a) && item[a].is_string())
        return item[a].get<std::string>();
    if (b && item.contains(b) && item[b].is_string())
        return item[b].get<std::string>();
    if (c && item.contains(c) && item[c].is_string())
        return item[c].get<std::string>();
    return "";
}
}

class KnabenProvider final : public sgb::Provider {
public:
    std::string id() const override { return "knaben"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;
        const std::string clean = sgb_api::trim(query);
        if (clean.empty())
            return results;

        constexpr size_t pageSize = 300;
        size_t offset = 0;
        std::unordered_set<std::string> seen;

        while (true) {
            const std::string url =
                "https://api.knaben.org/v2/search?q=" +
                sgb_api::urlEncode(clean) +
                "&sf=title&o=seeders&d=desc&s=" +
                std::to_string(pageSize) +
                "&f=" + std::to_string(offset);

            const std::string body =
                sgb_api::httpGet(url, "application/json");

            const Json root = Json::parse(body, nullptr, false);
            if (root.is_discarded())
                throw std::runtime_error("Knaben returned invalid JSON");

            const Json* items = nullptr;
            if (root.is_array()) {
                items = &root;
            }
            else if (root.is_object()) {
                if (root.contains("hits") && root["hits"].is_array())
                    items = &root["hits"];
                else if (root.contains("results") && root["results"].is_array())
                    items = &root["results"];
                else if (root.contains("data") && root["data"].is_array())
                    items = &root["data"];
            }

            if (!items)
                throw std::runtime_error("Knaben API missing result array");

            const size_t batchSize = items->size();
            if (batchSize == 0)
                break;

            size_t newResults = 0;

            for (const auto& item : *items) {
                if (!item.is_object())
                    continue;

                const std::string title = sgb_api::trim(
                    stringField(item, "title", "name"));

                std::string magnet = sgb_api::trim(
                    stringField(item, "magnetUrl", "magnetUri", "magnet"));

                std::string hash = sgb_api::lowerAscii(sgb_api::trim(
                    stringField(item, "hash", "infohash", "infoHash")));

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
                results.push_back(std::move(result));
                ++newResults;
            }

            if (batchSize < pageSize || newResults == 0)
                break;

            offset += pageSize;
        }

        return results;
    }
};

SGB_REGISTER_PROVIDER(KnabenProvider);
