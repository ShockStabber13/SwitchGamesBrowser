param(
    [string]$ProjectRoot = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'

function Write-Utf8NoBom {
    param([string]$Path, [string]$Content)
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [System.IO.File]::WriteAllText(
        $Path,
        $Content,
        (New-Object System.Text.UTF8Encoding($false))
    )
}

function Backup-Once {
    param([string]$Path)
    $backup = "$Path.pre-jackett-streaming.bak"
    if (-not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $Path -Destination $backup
    }
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$includeDir  = Join-Path $ProjectRoot 'switch\include'
$sourceDir   = Join-Path $ProjectRoot 'switch\source'
$providerDir = Join-Path $sourceDir 'providers'
$makefile    = Join-Path $ProjectRoot 'switch\Makefile'
$liveHpp     = Join-Path $includeDir 'live_search.hpp'
$liveCpp     = Join-Path $sourceDir 'live_search.cpp'
$mainCpp     = Join-Path $sourceDir 'main.cpp'

foreach ($required in @($makefile, $liveHpp, $liveCpp, $mainCpp)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required file not found: $required"
    }
}

New-Item -ItemType Directory -Force -Path $providerDir | Out-Null

foreach ($path in @($makefile, $liveHpp, $liveCpp, $mainCpp)) {
    Backup-Once $path
}

# ----------------------------------------------------------------------
# Shared Jackett provider helper (header-only so every generated .cpp
# remains a normal provider file and the filename stays the UI ID).
# ----------------------------------------------------------------------
$jackettHeader = @'
#pragma once

#include "provider.hpp"
#include "provider_api_utils.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sgb_jackett {

struct Config {
    std::string url;
    std::string apiKey;
};

inline Config loadConfig()
{
    constexpr const char* path =
        "sdmc:/switch/SwitchGamesBrowser/jackett.json";

    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error(
            "Missing SD:/switch/SwitchGamesBrowser/jackett.json");

    nlohmann::json root;
    file >> root;

    Config config;
    config.url = sgb_api::trim(
        root.value("url", std::string{}));
    config.apiKey = sgb_api::trim(
        root.value("apiKey", std::string{}));

    while (!config.url.empty() && config.url.back() == '/')
        config.url.pop_back();

    if (config.url.empty())
        throw std::runtime_error("Jackett URL is empty");
    if (config.apiKey.empty())
        throw std::runtime_error("Jackett API key is empty");

    if (
        config.url.rfind("http://", 0) != 0 &&
        config.url.rfind("https://", 0) != 0
    ) {
        throw std::runtime_error(
            "Jackett URL must start with http:// or https://");
    }

    return config;
}

inline std::vector<std::string> itemBlocks(
    const std::string& xml)
{
    std::vector<std::string> out;
    size_t pos = 0;

    while (true) {
        const size_t start = xml.find("<item", pos);
        if (start == std::string::npos)
            break;

        const size_t openEnd = xml.find('>', start);
        if (openEnd == std::string::npos)
            break;

        const size_t end = xml.find("</item>", openEnd + 1);
        if (end == std::string::npos)
            break;

        out.push_back(
            xml.substr(start, end + 7 - start));

        pos = end + 7;
    }

    return out;
}

inline std::vector<sgb::ProviderResult> search(
    const std::string& indexerId,
    const std::string& query)
{
    std::vector<sgb::ProviderResult> results;

    const std::string clean = sgb_api::trim(query);
    if (clean.empty())
        return results;

    const Config config = loadConfig();

    const std::string url =
        config.url +
        "/api/v2.0/indexers/" +
        sgb_api::urlEncode(indexerId) +
        "/results/torznab/api?apikey=" +
        sgb_api::urlEncode(config.apiKey) +
        "&t=search&q=" +
        sgb_api::urlEncode(clean);

    const std::string body =
        sgb_api::httpGet(
            url,
            "application/rss+xml, application/xml, text/xml, */*");

    std::unordered_set<std::string> seen;

    for (const auto& item : itemBlocks(body)) {
        const std::string title =
            sgb_api::trim(
                sgb_api::tagValue(item, "title"));

        if (title.empty())
            continue;

        std::string magnet =
            sgb_api::trim(
                sgb_api::torznabAttr(item, "magneturl"));

        std::string hash =
            sgb_api::lowerAscii(
                sgb_api::trim(
                    sgb_api::torznabAttr(item, "infohash")));

        if (magnet.empty()) {
            const std::string link =
                sgb_api::trim(
                    sgb_api::tagValue(item, "link"));

            if (link.rfind("magnet:?", 0) == 0)
                magnet = link;
        }

        if (hash.empty() && !magnet.empty())
            hash = sgb_api::hashFromMagnet(magnet);

        if (!hash.empty() && !sgb_api::validInfoHash(hash))
            hash.clear();

        if (magnet.empty() && !hash.empty())
            magnet = sgb_api::magnetFromHash(hash, title);

        if (hash.empty() && magnet.empty())
            continue;

        const std::string key =
            !hash.empty()
                ? ("h:" + hash)
                : ("m:" + magnet);

        if (!seen.insert(key).second)
            continue;

        sgb::ProviderResult row;
        row.title = title;
        row.infoHash = hash;
        row.magnet = magnet;
        results.push_back(std::move(row));
    }

    return results;
}

} // namespace sgb_jackett
'@

Write-Utf8NoBom `
    (Join-Path $includeDir 'jackett_provider.hpp') `
    $jackettHeader

# ----------------------------------------------------------------------
# Make sure provider .cpp files are compiled.
# ----------------------------------------------------------------------
$make = [System.IO.File]::ReadAllText($makefile)

if ($make -notmatch '(?m)^\s*SOURCES\s*:=.*source/providers') {
    $patched = [regex]::Replace(
        $make,
        '(?m)^(\s*SOURCES\s*:=\s*)source\s*$',
        '${1}source source/providers',
        1
    )

    if ($patched -eq $make) {
        throw "Could not patch switch/Makefile SOURCES line."
    }

    Write-Utf8NoBom $makefile $patched
}

# ----------------------------------------------------------------------
# Extend LiveSearchProgress with a thread-safe incremental result snapshot.
# ----------------------------------------------------------------------
$hpp = [System.IO.File]::ReadAllText($liveHpp)

if ($hpp -notmatch 'snapshotResults\s*\(') {
    $needle = '    std::string currentProvider;'
    if (-not $hpp.Contains($needle)) {
        throw "live_search.hpp layout is different than expected."
    }

    $replacement = @'
    std::string currentProvider;

    // Validated results are published here while providers are still
    // searching. The SDL/main thread only reads snapshots.
    mutable std::mutex resultMutex;
    std::vector<Release> liveReleases;
    std::map<std::string, DebridTorrentStatus> liveStatuses;
'@
    $hpp = $hpp.Replace($needle, $replacement.TrimEnd())

    $needle2 = '    std::string providerText() const;'
    $replacement2 = @'
    std::string providerText() const;

    void publishResult(
        const Release& release,
        const std::string& hash,
        const DebridTorrentStatus& status);

    void snapshotResults(
        std::vector<Release>& releases,
        std::map<std::string, DebridTorrentStatus>& statuses) const;
'@
    if (-not $hpp.Contains($needle2)) {
        throw "Could not find providerText declaration in live_search.hpp."
    }

    $hpp = $hpp.Replace($needle2, $replacement2.TrimEnd())
    Write-Utf8NoBom $liveHpp $hpp
}

# ----------------------------------------------------------------------
# Replace runLiveSearch with a bounded worker model:
# - max 4 provider workers
# - each provider is checked by debrid as soon as it finishes
# - validated Switch torrents are published immediately
# ----------------------------------------------------------------------
$cpp = [System.IO.File]::ReadAllText($liveCpp)

if ($cpp -notmatch '#include <thread>') {
    $cpp = $cpp.Replace(
        '#include <stdexcept>',
        "#include <stdexcept>`r`n#include <thread>"
    )
}

# Replace reset() so old streamed results do not leak into a new search.
$resetPattern =
    '(?s)void LiveSearchProgress::reset\(\)\s*\{.*?\n\}'
$resetReplacement = @'
void LiveSearchProgress::reset() {
    providersTotal.store(0);
    providersDone.store(0);
    candidatesFound.store(0);
    debridTotal.store(0);
    debridChecked.store(0);
    validFound.store(0);
    running.store(true);

    {
        std::lock_guard<std::mutex> lock(resultMutex);
        liveReleases.clear();
        liveStatuses.clear();
    }

    setText("Starting", "");
}
'@

$newCpp = [regex]::Replace(
    $cpp,
    $resetPattern,
    $resetReplacement.TrimEnd(),
    1
)

if ($newCpp -eq $cpp -and $cpp -notmatch 'liveReleases\.clear') {
    throw "Could not patch LiveSearchProgress::reset()."
}
$cpp = $newCpp

if ($cpp -notmatch 'void LiveSearchProgress::snapshotResults') {
    $providerTextPattern = @'
(?s)std::string LiveSearchProgress::providerText\(\) const\s*\{
.*?
\}
'@

    $match = [regex]::Match($cpp, $providerTextPattern)
    if (-not $match.Success) {
        throw "Could not locate LiveSearchProgress::providerText()."
    }

    $extraMethods = @'

void LiveSearchProgress::publishResult(
    const Release& release,
    const std::string& hash,
    const DebridTorrentStatus& status)
{
    std::lock_guard<std::mutex> lock(resultMutex);
    liveReleases.push_back(release);
    liveStatuses[hash] = status;
}

void LiveSearchProgress::snapshotResults(
    std::vector<Release>& releases,
    std::map<std::string, DebridTorrentStatus>& statuses) const
{
    std::lock_guard<std::mutex> lock(resultMutex);
    releases = liveReleases;
    statuses = liveStatuses;
}
'@

    $insertAt = $match.Index + $match.Length
    $cpp =
        $cpp.Substring(0, $insertAt) +
        $extraMethods +
        $cpp.Substring($insertAt)
}

$runStart = $cpp.IndexOf('LiveSearchResult runLiveSearch(')
$namespaceEnd = $cpp.LastIndexOf("} // namespace sgb")

if ($runStart -lt 0 -or $namespaceEnd -lt 0 -or $namespaceEnd -le $runStart) {
    throw "Could not locate runLiveSearch() boundaries."
}

$newRunFunction = @'
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
            throw std::runtime_error(
                "Configure a debrid service first");
        }

        const auto enabled =
            enabledProviderIds(configPath);

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
            throw std::runtime_error(
                "No scrape providers enabled");

        progress.setText(
            "Searching providers",
            "");

        // The Switch does not benefit from one OS thread per provider,
        // especially after adding many Jackett-backed providers.
        // Four workers keeps RAM/socket pressure predictable.
        constexpr size_t maxWorkers = 4;
        constexpr size_t debridBatchSize = 16;

        std::atomic<size_t> nextJob{0};
        std::mutex dedupeMutex;
        std::set<std::string> seen;

        auto worker = [&]() {
            while (true) {
                const size_t jobIndex =
                    nextJob.fetch_add(1);

                if (jobIndex >= jobs.size())
                    break;

                const Job job = jobs[jobIndex];

                progress.setText(
                    "Searching providers",
                    job.id);

                beginProviderDiagnostics(job.id);

                std::vector<Candidate> fresh;
                size_t providerRows = 0;
                std::string providerError;

                try {
                    auto provider = job.create();

                    if (provider) {
                        auto found =
                            provider->search(query);

                        providerRows = found.size();
                        fresh.reserve(found.size());

                        for (auto& item : found) {
                            if (item.title.empty())
                                continue;

                            if (item.infoHash.empty()) {
                                item.infoHash =
                                    hashFromMagnet(
                                        item.magnet);
                            }

                            if (
                                item.infoHash.empty() &&
                                item.magnet.empty()
                            ) {
                                continue;
                            }

                            Candidate candidate{
                                std::move(item),
                                job.id
                            };

                            const std::string key =
                                candidateKey(candidate);

                            if (key == "m:")
                                continue;

                            bool inserted = false;

                            {
                                std::lock_guard<std::mutex>
                                    lock(dedupeMutex);

                                inserted =
                                    seen.insert(key).second;
                            }

                            if (inserted)
                                fresh.push_back(
                                    std::move(candidate));
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
                    providerRows,
                    providerError);

                progress.candidatesFound.fetch_add(
                    fresh.size());

                progress.debridTotal.fetch_add(
                    fresh.size());

                if (!fresh.empty()) {
                    progress.setText(
                        "Checking debrid contents",
                        job.id);

                    auto backend =
                        createDebridBackend(
                            debridConfig);

                    if (backend) {
                        for (
                            size_t offset = 0;
                            offset < fresh.size();
                            offset += debridBatchSize
                        ) {
                            const size_t end =
                                std::min(
                                    offset +
                                        debridBatchSize,
                                    fresh.size());

                            std::vector<DebridCandidate>
                                batch;

                            batch.reserve(
                                end - offset);

                            for (
                                size_t i = offset;
                                i < end;
                                ++i
                            ) {
                                std::string hash =
                                    lowerAscii(
                                        fresh[i]
                                            .provider
                                            .infoHash);

                                if (hash.empty()) {
                                    hash =
                                        hashFromMagnet(
                                            fresh[i]
                                                .provider
                                                .magnet);
                                }

                                batch.push_back({
                                    hash,
                                    fresh[i]
                                        .provider
                                        .magnet
                                });
                            }

                            std::map<
                                std::string,
                                DebridTorrentStatus
                            > checked;

                            try {
                                checked =
                                    backend->check(
                                        batch);
                            }
                            catch (...) {
                                progress.debridChecked
                                    .fetch_add(
                                        end - offset);
                                continue;
                            }

                            for (
                                size_t i = offset;
                                i < end;
                                ++i
                            ) {
                                std::string hash =
                                    lowerAscii(
                                        fresh[i]
                                            .provider
                                            .infoHash);

                                if (hash.empty()) {
                                    hash =
                                        hashFromMagnet(
                                            fresh[i]
                                                .provider
                                                .magnet);
                                }

                                auto state =
                                    checked.find(hash);

                                if (
                                    state ==
                                        checked.end() ||
                                    !hasSwitchExtension(
                                        state->second.files)
                                ) {
                                    continue;
                                }

                                Release release;
                                release.title =
                                    fresh[i]
                                        .provider
                                        .title;
                                release.magnet =
                                    fresh[i]
                                        .provider
                                        .magnet;
                                release.infoHash =
                                    hash;
                                release.source =
                                    fresh[i].source;

                                progress.publishResult(
                                    release,
                                    hash,
                                    state->second);

                                progress.validFound
                                    .fetch_add(1);
                            }

                            progress.debridChecked
                                .fetch_add(
                                    end - offset);
                        }
                    }
                    else {
                        progress.debridChecked
                            .fetch_add(
                                fresh.size());
                    }
                }

                progress.providersDone.fetch_add(1);
            }
        };

        const size_t workerCount =
            std::min(
                maxWorkers,
                jobs.size());

        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (size_t i = 0; i < workerCount; ++i)
            workers.emplace_back(worker);

        for (auto& thread : workers)
            thread.join();

        progress.snapshotResults(
            output.releases,
            output.statuses);

        output.message =
            "Search complete: " +
            std::to_string(
                output.releases.size()) +
            " valid Switch torrent(s)";

        progress.setText(
            "Complete",
            "");
    }
    catch (const std::exception& e) {
        output.message = e.what();
        progress.setText(
            "Error",
            "");
    }

    progress.running.store(false);
    return output;
}

'@

$cpp =
    $cpp.Substring(0, $runStart) +
    $newRunFunction +
    $cpp.Substring($namespaceEnd)

Write-Utf8NoBom $liveCpp $cpp

# ----------------------------------------------------------------------
# Main loop: consume snapshots while the future is still running.
# The torrent list becomes interactive after the first VALID result arrives.
# ----------------------------------------------------------------------
$main = [System.IO.File]::ReadAllText($mainCpp)

if ($main -notmatch 'Stream validated live-search results') {
    $readyNeedle = @'
        if (
            pendingLiveSearch.valid() &&
            pendingLiveSearch.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready
        ) {
'@

    $readyIndex = $main.IndexOf($readyNeedle)

    if ($readyIndex -lt 0) {
        # CRLF-normalized fallback.
        $normalizedNeedle =
            $readyNeedle -replace "`n", "`r`n"
        $readyIndex =
            $main.IndexOf($normalizedNeedle)
    }

    if ($readyIndex -lt 0) {
        throw "Could not locate pendingLiveSearch completion block in main.cpp."
    }

    $streamBlock = @'
        // Stream validated live-search results into the torrent page while
        // provider/debrid workers continue in the background.
        if (
            pendingLiveSearch.valid() &&
            liveSearchGameIndex < games.size()
        ) {
            std::vector<sgb::Release> liveRows;
            std::map<
                std::string,
                sgb::DebridTorrentStatus
            > liveStatuses;

            liveSearchProgress.snapshotResults(
                liveRows,
                liveStatuses);

            auto& liveGame =
                games[liveSearchGameIndex];

            if (
                liveRows.size() >
                    liveGame.releases.size()
            ) {
                liveGame.releases =
                    std::move(liveRows);

                for (auto& pair : liveStatuses) {
                    debridStatuses[pair.first] =
                        std::move(pair.second);
                }

                if (
                    torrentCursor >=
                        liveGame.releases.size()
                ) {
                    torrentCursor =
                        liveGame.releases.empty()
                            ? 0
                            : liveGame.releases.size() - 1;
                }

                // First usable torrent: show the normal torrent screen.
                // A/X/Up/Down work immediately while more rows append.
                if (page == Page::SearchProgress) {
                    page = Page::Torrents;
                    status =
                        "Live search still running; "
                        "more torrents may appear";
                }
            }
        }

'@

    $main =
        $main.Substring(0, $readyIndex) +
        $streamBlock +
        $main.Substring($readyIndex)
}

$main = $main.Replace(
    'Results are buffered until the search finishes.',
    'Validated results become clickable while the search continues.'
)

Write-Utf8NoBom $mainCpp $main

Write-Host ''
Write-Host 'Jackett + streaming-results patch applied.' -ForegroundColor Green
Write-Host ''
Write-Host 'Next:' -ForegroundColor Cyan
Write-Host '  1. Run Generate-Jackett-Providers.ps1'
Write-Host '  2. Put jackett.json on the Switch SD card'
Write-Host '  3. Build the Switch project'
