#pragma once

#include "provider_api_utils.hpp"

#include <nlohmann/json.hpp>
#include <climits>
#include <initializer_list>
#include <string>

namespace sgb_stats {

// -1 denotes unavailable. Zero means the provider explicitly reported zero.
inline int nonnegativeCount(const std::string& text)
{
    const std::string value = sgb_api::trim(text);
    if (value.empty())
        return -1;
    int count = 0;
    for (char c : value) {
        if (c < '0' || c > '9')
            return -1;
        const int digit = c - '0';
        if (count > (INT_MAX - digit) / 10)
            return -1;
        count = count * 10 + digit;
    }
    return count;
}

inline int jsonCount(
    const nlohmann::json& item,
    std::initializer_list<const char*> keys)
{
    if (!item.is_object())
        return -1;
    for (const char* key : keys) {
        auto found = item.find(key);
        if (found == item.end() || found->is_null())
            continue;
        if (found->is_number_integer()) {
            const auto number = found->get<std::int64_t>();
            if (number >= 0 && number <= INT_MAX)
                return static_cast<int>(number);
        } else if (found->is_number_unsigned()) {
            const auto number = found->get<std::uint64_t>();
            if (number <= INT_MAX)
                return static_cast<int>(number);
        } else if (found->is_string()) {
            const int count = nonnegativeCount(found->get<std::string>());
            if (count >= 0)
                return count;
        }
    }
    return -1;
}

inline int xmlCount(
    const std::string& item,
    std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        int count = nonnegativeCount(sgb_api::torznabAttr(item, key));
        if (count >= 0)
            return count;
        count = nonnegativeCount(sgb_api::tagValue(item, key));
        if (count >= 0)
            return count;
    }
    return -1;
}

inline int seeders(const nlohmann::json& item)
{
    return jsonCount(item, {"seeders", "seeds", "seed", "seed_count",
                            "seedersCount", "num_seeders", "Seeders"});
}

inline int leechers(const nlohmann::json& item)
{
    return jsonCount(item, {"leechers", "leeches", "leech", "leech_count",
                            "leechersCount", "num_leechers", "Leechers"});
}

inline int seeders(const std::string& xml)
{
    return xmlCount(xml, {"seeders", "seeds", "nyaa:seeders"});
}

inline int leechers(const std::string& xml)
{
    return xmlCount(xml, {"leechers", "leeches", "nyaa:leechers"});
}

} // namespace sgb_stats
