#include "provider.hpp"
#include "provider_api_utils.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;
}

class BitSearchProvider final : public sgb::Provider {
public:
    std::string id() const override { return "bitsearch"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;
        const std::string clean = sgb_api::trim(query);
        if (clean.empty())
            return results;

        const std::string url =
            "https://bitsearch.eu/api/v1/search?q=" +
            sgb_api::urlEncode(clean) +
            "&sort=seeders&order=desc&limit=100&page=1";

        const std::string body =
            sgb_api::httpGet(url, "application/json");

        const Json root = Json::parse(body, nullptr, false);
        if (root.is_discarded() || !root.is_object())
            throw std::runtime_error("BitSearch returned invalid JSON");

        if (!root.value("success", false))
            throw std::runtime_error("BitSearch API success=false");

        if (!root.contains("results") || !root["results"].is_array())
            throw std::runtime_error("BitSearch API missing results");

        std::unordered_set<std::string> seen;

        for (const auto& item : root["results"]) {
            if (!item.is_object())
                continue;

            const std::string title =
                sgb_api::trim(item.value("title", std::string{}));
            std::string hash = sgb_api::lowerAscii(
                sgb_api::trim(item.value("infohash", std::string{})));

            if (title.empty() || !sgb_api::validInfoHash(hash))
                continue;

            if (!seen.insert(hash).second)
                continue;

            sgb::ProviderResult result;
            result.title = title;
            result.infoHash = hash;
            result.magnet = sgb_api::magnetFromHash(hash, title);
            results.push_back(std::move(result));
        }

        return results;
    }
};

SGB_REGISTER_PROVIDER(BitSearchProvider);