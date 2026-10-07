#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_1337x final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:1337x";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "1337x",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_1337x);