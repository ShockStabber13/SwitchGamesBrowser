#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_Byrutor final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:byrutor";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "byrutor",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_Byrutor);