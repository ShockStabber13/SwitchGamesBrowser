#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class JackettProvider_RuTrackerRU final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:rutracker-ru";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "rutracker-ru",
            query);
    }
};

SGB_REGISTER_PROVIDER(JackettProvider_RuTrackerRU);