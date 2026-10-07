#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_showRSS final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:showrss";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "showrss",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_showRSS);