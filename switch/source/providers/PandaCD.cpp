#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_PandaCD final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:pandacd";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "pandacd",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_PandaCD);