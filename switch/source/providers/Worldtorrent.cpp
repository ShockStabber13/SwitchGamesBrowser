#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_Worldtorrent final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:world-torrent";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "world-torrent",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_Worldtorrent);