#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_EZTV final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:eztv";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "eztv",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_EZTV);