#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sgb {

struct ProviderResult {
    std::string title;
    std::string magnet;
    std::string infoHash;
    std::string torrentUrl;
    int seeders = -1;  // -1: unknown; zero: no seeders reported
    int leechers = -1;
};

class Provider {
public:
    virtual ~Provider() = default;

    virtual std::string id() const = 0;

    virtual std::vector<ProviderResult>
    search(const std::string& query) = 0;
};


// ============================================================
// Provider registry
// ============================================================

using ProviderFactory =
    std::unique_ptr<Provider>(*)();

struct ProviderEntry {
    // Automatically derived from the .cpp filename.
    std::string id;

    ProviderFactory create;
};

const std::vector<ProviderEntry>&
providerRegistry();

class ProviderRegistrar {
public:
    ProviderRegistrar(
        const char* sourceFile,
        ProviderFactory factory
    );
};

} // namespace sgb


// ============================================================
// Put this at the BOTTOM of each provider .cpp:
//
// SGB_REGISTER_PROVIDER(MyProvider);
//
// The registry ID comes from the filename:
//
// my_provider.cpp -> my_provider
// ============================================================

#define SGB_REGISTER_PROVIDER(CLASS)                  \
    namespace {                                       \
        std::unique_ptr<sgb::Provider>                \
        sgbCreateProvider_##CLASS() {                 \
            return std::make_unique<CLASS>();         \
        }                                             \
                                                      \
        const sgb::ProviderRegistrar                  \
        sgbProviderRegistrar_##CLASS(                 \
            __FILE__,                                 \
            &sgbCreateProvider_##CLASS                \
        );                                            \
    }

