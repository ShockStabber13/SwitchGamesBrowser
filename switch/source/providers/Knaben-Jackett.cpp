#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_Knaben_Jackett final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:knaben";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "knaben",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_Knaben_Jackett);