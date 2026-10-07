#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_U3C3 final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:u3c3";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "u3c3",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_U3C3);