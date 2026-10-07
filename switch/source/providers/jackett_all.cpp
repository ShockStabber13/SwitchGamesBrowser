#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettAllProvider final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett_all";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "all",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettAllProvider);
