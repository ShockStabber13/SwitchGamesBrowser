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
}

class ThePirateBayProvider final : public sgb::Provider {
public:
    std::string id() const override { return "the_pirate_bay"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;
        const std::string clean = sgb_api::trim(query);
        if (clean.empty())
            return results;

        const std::string url =
            "https://apibay.org/q.php?q=" + sgb_api::urlEncode(clean) +
            "&cat=0";

        const std::string body =
            sgb_api::httpGet(url, "application/json");

        const Json root = Json::parse(body, nullptr, false);
        if (root.is_discarded() || !root.is_array())
            throw std::runtime_error("APIBay returned invalid JSON");

        std::unordered_set<std::string> seen;

        for (const auto& item : root) {
            if (!item.is_object())
                continue;

            const std::string title = sgb_api::trim(
                item.value("name", std::string{}));

            std::string hash = sgb_api::lowerAscii(sgb_api::trim(
                item.value("info_hash", std::string{})));

            if (hash == "0000000000000000000000000000000000000000")
                continue;

            if (title.empty() || !sgb_api::validInfoHash(hash))
                continue;

            if (!seen.insert(hash).second)
                continue;

            sgb::ProviderResult result;
            result.title = title;
            result.infoHash = hash;
            result.magnet = sgb_api::magnetFromHash(hash, title);
            result.seeders = sgb_stats::seeders(item);
            result.leechers = sgb_stats::leechers(item);
            results.push_back(std::move(result));
        }

        return results;
    }
};

SGB_REGISTER_PROVIDER(ThePirateBayProvider);