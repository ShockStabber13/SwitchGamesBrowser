#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sgb {
struct ShopEntry {
    std::string name;
    std::string url;
    bool isDirectory = false;
    std::uint64_t size = 0;
};
struct ShopListing {
    std::string url;
    std::vector<ShopEntry> entries;
    std::string error;
    bool success = false;
};
// Tinfoil-format JSON shops and simple HTTPS directory indexes.
// Only explicitly linked HTTPS packages are installable; private shop
// authentication and shop-specific protocols are deliberately excluded.
ShopListing loadShopListing(const std::string& url);
bool isShopPackage(const std::string& name);
} // namespace sgb
