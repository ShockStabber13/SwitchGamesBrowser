#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_BangumiMoe final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:bangumi-moe";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "bangumi-moe",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_BangumiMoe);