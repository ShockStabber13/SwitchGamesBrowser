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
// Shop option #3: scrape the NotUltraNX public website for Base,
// Update and DLC download options. The API is used ONLY for the official
// short-lived download redirect, never for catalog search.
ShopSearchResult searchNotUltraNxWebsite(
    const std::string& title,
    const std::string& titleId,
    const std::string& catalogPath,
    ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel);

// Download a local, offline-searchable website catalog into the app folder.
// No download URLs or credentials are saved, only game names and title IDs.
std::string downloadNotUltraNxCatalog(
    const std::string& catalogPath,
    ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel);

// Legacy relay entry point, retained for source compatibility.
ShopSearchResult searchNotUltraNxRelay(
    const std::string& title,
    ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel);

ShopSearchResult searchOpenNxShops(
    const std::string& title,
    ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel);
bool isShopPackage(const std::string& name);
} // namespace sgb
