#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_kickasstorrentsto final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:kickasstorrents-to";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "kickasstorrents-to",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_kickasstorrentsto);