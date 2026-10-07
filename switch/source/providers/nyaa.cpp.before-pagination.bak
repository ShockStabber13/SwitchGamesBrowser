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

class NyaaProvider final : public sgb::Provider {
public:
    std::string id() const override { return "nyaa"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;
        const std::string clean = sgb_api::trim(query);
        if (clean.empty())
            return results;

        const std::string url =
            "https://nyaaapi.onrender.com/nyaa?q=" +
            sgb_api::urlEncode(clean) +
            "&category=software&sort=seeders&order=desc";

        const std::string body =
            sgb_api::httpGet(url, "application/json");

        const Json root = Json::parse(body, nullptr, false);
        if (root.is_discarded() || !root.is_object())
            throw std::runtime_error("Nyaa API returned invalid JSON");

        if (!root.contains("data") || !root["data"].is_array())
            throw std::runtime_error("Nyaa API missing data array");

        std::unordered_set<std::string> seen;

        for (const auto& item : root["data"]) {
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
            results.push_back(std::move(result));
        }

        return results;
    }
};

SGB_REGISTER_PROVIDER(NyaaProvider);