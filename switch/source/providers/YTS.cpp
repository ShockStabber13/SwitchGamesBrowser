#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_YTS final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:yts";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "yts",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_YTS);