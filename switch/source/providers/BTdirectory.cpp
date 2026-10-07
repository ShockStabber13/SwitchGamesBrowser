#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_BTdirectory final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:btdirectory";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "btdirectory",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_BTdirectory);