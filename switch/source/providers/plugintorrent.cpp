#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_plugintorrent final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:plugintorrent";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "plugintorrent",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_plugintorrent);