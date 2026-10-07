#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_AudioBookBay final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:audiobookbay";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "audiobookbay",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_AudioBookBay);