#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_AnimeTosho final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:animetosho-xyz";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "animetosho-xyz",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_AnimeTosho);