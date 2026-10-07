#include "provider.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace sgb {

namespace {

std::vector<ProviderEntry>&
mutableRegistry() {
    static std::vector<ProviderEntry> registry;
    return registry;
}


std::string providerIdFromFile(
    std::string path
) {
    // Support both:
    // source/providers/foo.cpp
    // source\providers\foo.cpp

    const auto slash =
        path.find_last_of("/\\");

    if (slash != std::string::npos) {
        path =
            path.substr(slash + 1);
    }

    const auto dot =
        path.find_last_of('.');

    if (dot != std::string::npos) {
        path =
            path.substr(0, dot);
    }

    // Normalize provider IDs.
    std::transform(
        path.begin(),
        path.end(),
        path.begin(),
        [](unsigned char c) {
            return static_cast<char>(
                std::tolower(c)
            );
        }
    );

    return path;
}

} // namespace


ProviderRegistrar::ProviderRegistrar(
    const char* sourceFile,
    ProviderFactory factory
) {
    if (!sourceFile || !factory)
        return;

    std::string id =
        providerIdFromFile(sourceFile);

    if (id.empty())
        return;

    auto& registry =
        mutableRegistry();

    // Avoid duplicate filenames / IDs.
    auto existing =
        std::find_if(
            registry.begin(),
            registry.end(),
            [&](const ProviderEntry& entry) {
                return entry.id == id;
            }
        );

    if (existing != registry.end())
        return;

    registry.push_back({
        std::move(id),
        factory
    });
}


const std::vector<ProviderEntry>&
providerRegistry() {
    return mutableRegistry();
}

} // namespace sgb
