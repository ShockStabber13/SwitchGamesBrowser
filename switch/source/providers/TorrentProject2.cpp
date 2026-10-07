#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_TorrentProject2 final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:torrentproject2";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "torrentproject2",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_TorrentProject2);