#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_GamesTorrents final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:gamestorrents";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "gamestorrents",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_GamesTorrents);