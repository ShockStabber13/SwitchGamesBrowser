#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_ThePirateBay_Jackett final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:thepiratebay";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "thepiratebay",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_ThePirateBay_Jackett);