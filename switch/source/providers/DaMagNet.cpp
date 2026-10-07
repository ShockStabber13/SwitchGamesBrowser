#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_DaMagNet final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:damagnet";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "damagnet",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_DaMagNet);