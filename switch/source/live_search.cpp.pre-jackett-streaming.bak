#include "live_search.hpp"
#include "provider.hpp"
#include "provider_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <future>
#include <set>
#include <stdexcept>
#include <utility>

namespace sgb {
namespace {

std::string lowerAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );
    return value;
}

bool hasSwitchExtension(const std::vector<DebridFile>& files) {
    static const char* extensions[] = {
        ".nsp", ".nsz", ".xci", ".xcz"
    };

    for (const auto& file : files) {
        std::string name = lowerAscii(file.name);

        for (const char* ext : extensions) {
            const size_t n = std::char_traits<char>::length(ext);

            if (
                name.size() >= n &&
                name.compare(name.size() - n, n, ext) == 0
            ) {
                return true;
            }
        }
    }

    return false;
}

std::string hashFromMagnet(const std::string& magnet) {
    const std::string needle = "xt=urn:btih:";
    const std::string lowered = lowerAscii(magnet);
    auto pos = lowered.find(needle);

    if (pos == std::string::npos)
        return "";

    pos += needle.size();
    auto end = magnet.find('&', pos);

    std::string hash = magnet.substr(
        pos,
        end == std::string::npos
            ? std::string::npos
            : end - pos
    );

    // Current debrid path expects the normal 40-character info hash.
    if (hash.size() == 40)
        return lowerAscii(hash);

    return "";
}

struct Candidate {
    ProviderResult provider;
    std::string source;
};

std::string candidateKey(const Candidate& candidate) {
    std::string hash = lowerAscii(candidate.provider.infoHash);

    if (hash.empty())
        hash = hashFromMagnet(candidate.provider.magnet);

    if (!hash.empty())
        return "h:" + hash;

    return "m:" + candidate.provider.magnet;
}

} // namespace

void LiveSearchProgress::reset() {
    providersTotal.store(0);
    providersDone.store(0);
    candidatesFound.store(0);
    debridTotal.store(0);
    debridChecked.store(0);
    validFound.store(0);
    running.store(true);
    setText("Starting", "");
}

void LiveSearchProgress::setText(
    const std::string& newStage,
    const std::string& provider
) {
    std::lock_guard<std::mutex> lock(textMutex);
    stage = newStage;
    currentProvider = provider;
}

std::string LiveSearchProgress::stageText() const {
    std::lock_guard<std::mutex> lock(textMutex);
    return stage;
}

std::string LiveSearchProgress::providerText() const {
    std::lock_guard<std::mutex> lock(textMutex);
    return currentProvider;
}

std::set<std::string> enabledProviderIds(
    const std::string& configPath
) {
    std::set<std::string> out;
    bool configured = false;

    try {
        auto config = Json::parse(read(configPath, 65536));
        auto it = config.find("enabledProviders");

        if (it != config.end() && it->is_array()) {
            configured = true;

            for (const auto& item : *it) {
                if (item.is_string())
                    out.insert(item.get<std::string>());
            }
        }
    } catch (...) {}

    // If the setting has never been written, default to all compiled providers.
    if (!configured) {
        for (const auto& entry : providerRegistry())
            out.insert(entry.id);
    }

    return out;
}

LiveSearchResult runLiveSearch(
    const std::string& query,
    const std::string& configPath,
    const DebridConfig& debridConfig,
    LiveSearchProgress& progress
) {
    LiveSearchResult output;
    progress.reset();
    resetProviderDiagnostics();

    try {
        if (
            debridConfig.service == DebridService::None ||
            debridConfig.apiKey.empty()
        ) {
            throw std::runtime_error("Configure a debrid service first");
        }

        const auto enabled = enabledProviderIds(configPath);

        struct Job {
            std::string id;
            ProviderFactory create = nullptr;
        };

        std::vector<Job> jobs;

        for (const auto& entry : providerRegistry()) {
            if (enabled.count(entry.id))
                jobs.push_back({entry.id, entry.create});
        }

        progress.providersTotal.store(jobs.size());

        if (jobs.empty())
            throw std::runtime_error("No scrape providers enabled");

        progress.setText("Searching providers", "");

        // All enabled providers run concurrently.
        std::vector<std::future<std::vector<Candidate>>> futures;
        futures.reserve(jobs.size());

        for (const auto& job : jobs) {
            futures.push_back(std::async(
                std::launch::async,
                [job, query, &progress]() {
                    std::vector<Candidate> rows;
                    progress.setText("Searching providers", job.id);

                    beginProviderDiagnostics(job.id);

                    std::string providerError;

                    try {
                        auto provider = job.create();

                        if (provider) {
                            auto found = provider->search(query);
                            rows.reserve(found.size());

                            for (auto& item : found) {
                                if (item.title.empty())
                                    continue;

                                if (item.infoHash.empty())
                                    item.infoHash = hashFromMagnet(item.magnet);

                                if (
                                    item.infoHash.empty() &&
                                    item.magnet.empty()
                                ) {
                                    continue;
                                }

                                rows.push_back({
                                    std::move(item),
                                    job.id
                                });
                            }
                        }
                    }
                    catch (const std::exception& e) {
                        providerError = e.what();
                    }
                    catch (...) {
                        providerError =
                            "Unknown provider exception";
                    }

                    endProviderDiagnostics(
                        rows.size(),
                        providerError
                    );

                    progress.candidatesFound.fetch_add(rows.size());
                    progress.providersDone.fetch_add(1);
                    return rows;
                }
            ));
        }

        std::vector<Candidate> candidates;
        std::set<std::string> seen;

        for (auto& future : futures) {
            auto rows = future.get();

            for (auto& row : rows) {
                const std::string key = candidateKey(row);

                if (key == "m:" || !seen.insert(key).second)
                    continue;

                candidates.push_back(std::move(row));
            }
        }

        // Show the deduped count once provider collection is complete.
        progress.candidatesFound.store(candidates.size());
        progress.debridTotal.store(candidates.size());
        progress.setText("Checking debrid contents", "");

        if (candidates.empty()) {
            output.message = "Search complete: no candidates found";
            progress.setText("Complete", "");
            progress.running.store(false);
            return output;
        }

        auto backend = createDebridBackend(debridConfig);

        if (!backend)
            throw std::runtime_error("Debrid backend unavailable");

        // Small batches let the UI progress counter move while debrid checking runs.
        constexpr size_t batchSize = 25;

        for (
            size_t offset = 0;
            offset < candidates.size();
            offset += batchSize
        ) {
            const size_t end = std::min(
                offset + batchSize,
                candidates.size()
            );

            std::vector<DebridCandidate> batch;
            batch.reserve(end - offset);

            for (size_t i = offset; i < end; ++i) {
                batch.push_back({
                    lowerAscii(candidates[i].provider.infoHash),
                    candidates[i].provider.magnet
                });
            }

            std::map<std::string, DebridTorrentStatus> checked;

            try {
                checked = backend->check(batch);
            } catch (...) {
                progress.debridChecked.fetch_add(end - offset);
                continue;
            }

            for (auto& pair : checked) {
                output.statuses[lowerAscii(pair.first)] =
                    std::move(pair.second);
            }

            progress.debridChecked.fetch_add(end - offset);
        }

        // Providers do NOT inspect file extensions.
        // Only the file list returned by the selected debrid service is used.
        for (const auto& candidate : candidates) {
            std::string hash = lowerAscii(candidate.provider.infoHash);

            if (hash.empty())
                hash = hashFromMagnet(candidate.provider.magnet);

            auto state = output.statuses.find(hash);

            if (
                state == output.statuses.end() ||
                !hasSwitchExtension(state->second.files)
            ) {
                continue;
            }

            Release release;
            release.title = candidate.provider.title;
            release.magnet = candidate.provider.magnet;
            release.infoHash = hash;
            release.source = candidate.source;

            output.releases.push_back(std::move(release));
            progress.validFound.fetch_add(1);
        }

        output.message =
            "Search complete: " +
            std::to_string(output.releases.size()) +
            " valid Switch torrent(s)";

        progress.setText("Complete", "");
    }
    catch (const std::exception& e) {
        output.message = e.what();
        progress.setText("Error", "");
    }

    progress.running.store(false);
    return output;
}

} // namespace sgb