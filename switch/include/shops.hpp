#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sgb {
struct ShopEntry {
    std::string name;
    std::string url;
    std::string shop;
    std::uint64_t size = 0;
};
struct ShopSearchProgress {
    std::atomic<std::size_t> shopsTotal{0};
    std::atomic<std::size_t> shopsDone{0};
    std::atomic<bool> running{false};
    mutable std::mutex mutex;
    std::vector<ShopEntry> matches;
    std::string message;
    void snapshot(std::vector<ShopEntry>& out, std::string& detail) const {
        std::lock_guard<std::mutex> lock(mutex);
        out = matches;
        detail = message;
    }
};
struct ShopSearchResult {
    std::vector<ShopEntry> matches;
    std::string message;
    bool success = false;
};
// Read OpenNX's Tinfoil index plus linked public JSON shop indexes.
// Private credentials, Tinfoil-specific protocols, and custom headers
// are not transferred automatically.
ShopSearchResult searchOpenNxShops(
    const std::string& title,
    ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel);
bool isShopPackage(const std::string& name);
} // namespace sgb
