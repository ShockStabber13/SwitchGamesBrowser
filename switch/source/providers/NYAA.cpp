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

bool nyaaHasNextPage(
    const Json& root,
    size_t currentPage,
    size_t batchSize)
{
    if (root.contains("nextPage") && root["nextPage"].is_boolean())
        return root["nextPage"].get<bool>();

    if (root.contains("totalPage") && root["totalPage"].is_number_integer())
        return currentPage < root["totalPage"].get<size_t>();

    if (root.contains("totalPages") && root["totalPages"].is_number_integer())
        return currentPage < root["totalPages"].get<size_t>();

    // If this API build does not expose paging metadata, keep requesting
    // pages until it returns an empty page. A repeated page is caught by
    // the no-new-results guard in search().
    return batchSize != 0;
}
}

class NyaaProvider final : public sgb::Provider {
public:
    std::string id() const override { return "nyaa"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;
        const std::string clean = sgb_api::trim(query);
        if (clean.empty())
            return results;

        size_t page = 1;
        std::unordered_set<std::string> seen;

        while (true) {
            const std::string url =
                "https://nyaaapi.onrender.com/nyaa?q=" +
                sgb_api::urlEncode(clean) +
                "&category=software&sort=seeders&order=desc&page=" +
                std::to_string(page);

            const std::string body =
                sgb_api::httpGet(url, "application/json");

            const Json root = Json::parse(body, nullptr, false);
            if (root.is_discarded() || !root.is_object())
                throw std::runtime_error("Nyaa API returned invalid JSON");

            if (!root.contains("data") || !root["data"].is_array())
                throw std::runtime_error("Nyaa API missing data array");

            const Json& items = root["data"];
            const size_t batchSize = items.size();
            if (batchSize == 0)
                break;

            size_t newResults = 0;

            for (const auto& item : items) {
                if (!item.is_object())
                    continue;

                const std::string title = sgb_api::trim(
                    item.value("title", std::string{}));

                const std::string magnet = sgb_api::trim(
                    item.value("magnet", std::string{}));

                const std::string hash =
                    sgb_api::hashFromMagnet(magnet);

                if (title.empty() || magnet.empty() ||
                    !sgb_api::validInfoHash(hash)) {
                    continue;
                }

                if (!seen.insert(hash).second)
                    continue;

                sgb::ProviderResult result;
                result.title = title;
                result.magnet = magnet;
                result.infoHash = hash;
                result.seeders = sgb_stats::seeders(item);
                result.leechers = sgb_stats::leechers(item);
                results.push_back(std::move(result));
                ++newResults;
            }

            if (newResults == 0 ||
                !nyaaHasNextPage(root, page, batchSize)) {
                break;
            }

            ++page;
        }

        return results;
    }
};

SGB_REGISTER_PROVIDER(NyaaProvider);
