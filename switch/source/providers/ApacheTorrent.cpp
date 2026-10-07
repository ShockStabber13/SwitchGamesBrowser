#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_ApacheTorrent final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:apachetorrent";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "apachetorrent",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_ApacheTorrent);