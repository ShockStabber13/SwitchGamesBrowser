#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_MagnetDownload final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:magnetdownload";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "magnetdownload",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_MagnetDownload);