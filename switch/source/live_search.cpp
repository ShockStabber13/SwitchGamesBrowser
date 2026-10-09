#include "live_search.hpp"
#include "provider.hpp"
#include "provider_diagnostics.hpp"
#include "provider_api_utils.hpp"
#include "torrent_metainfo.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>
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

std::string normalizeInfoHash(std::string hash) {
    hash = lowerAscii(std::move(hash));

    if (hash.size() == 40) {
        for (unsigned char ch : hash) {
            if (!std::isxdigit(ch))
                return "";
        }

        return hash;
    }

    // BitTorrent v1 hashes may also be supplied as 32-character
    // Base32 BTIH values. Convert them to the 40-character hex form
    // expected by the debrid backends.
    if (hash.size() != 32)
        return "";

    static const char* hex =
        "0123456789abcdef";

    std::string out;
    out.reserve(40);

    unsigned int buffer = 0;
    int bits = 0;

    for (unsigned char ch : hash) {
        int value = -1;

        if (ch >= 'a' && ch <= 'z')
            value = ch - 'a';
        else if (ch >= '2' && ch <= '7')
            value = 26 + (ch - '2');
        else
            return "";

        buffer =
            (buffer << 5) |
            static_cast<unsigned int>(value);

        bits += 5;

        while (bits >= 8) {
            bits -= 8;

            const unsigned int byte =
                (buffer >> bits) & 0xffU;

            out.push_back(hex[(byte >> 4) & 0x0fU]);
            out.push_back(hex[byte & 0x0fU]);

            if (bits == 0) {
                buffer = 0;
            }
            else {
                buffer &=
                    (1U << bits) - 1U;
            }
        }
    }

    return out.size() == 40
        ? out
        : "";
}

std::string hashFromMagnet(const std::string& magnet) {
    const std::string needle = "xt=urn:btih:";
    const std::string lowered = lowerAscii(magnet);
    auto pos = lowered.find(needle);

    if (pos == std::string::npos)
        return "";

    pos += needle.size();
    auto end = magnet.find('&', pos);

    return normalizeInfoHash(
        magnet.substr(
            pos,
            end == std::string::npos
                ? std::string::npos
                : end - pos
        )
    );
}

struct Candidate {
    ProviderResult provider;
    std::string source;
};


// ============================================================
// Global .torrent download limiter
// ============================================================

constexpr size_t maxConcurrentTorrentDownloads = 10;

std::mutex torrentDownloadMutex;
std::condition_variable torrentDownloadCv;
size_t activeTorrentDownloads = 0;

class TorrentDownloadSlot {
public:
    TorrentDownloadSlot()
    {
        std::unique_lock<std::mutex> lock(
            torrentDownloadMutex);

        torrentDownloadCv.wait(
            lock,
            []() {
                return activeTorrentDownloads <
                    maxConcurrentTorrentDownloads;
            });

        ++activeTorrentDownloads;
    }

    ~TorrentDownloadSlot()
    {
        {
            std::lock_guard<std::mutex> lock(
                torrentDownloadMutex);

            if (activeTorrentDownloads > 0)
                --activeTorrentDownloads;
        }

        torrentDownloadCv.notify_one();
    }

    TorrentDownloadSlot(
        const TorrentDownloadSlot&) = delete;

    TorrentDownloadSlot& operator=(
        const TorrentDownloadSlot&) = delete;
};


constexpr size_t maxConcurrentTorrentProcessing = 10;

std::mutex torrentProcessingMutex;
std::condition_variable torrentProcessingCv;
size_t activeTorrentProcessing = 0;

class TorrentProcessingSlot {
public:
    TorrentProcessingSlot()
    {
        std::unique_lock<std::mutex> lock(
            torrentProcessingMutex);

        torrentProcessingCv.wait(
            lock,
            []() {
                return activeTorrentProcessing <
                    maxConcurrentTorrentProcessing;
            });

        ++activeTorrentProcessing;
    }

    ~TorrentProcessingSlot()
    {
        {
            std::lock_guard<std::mutex> lock(
                torrentProcessingMutex);

            if (activeTorrentProcessing > 0)
                --activeTorrentProcessing;
        }

        torrentProcessingCv.notify_one();
    }

    TorrentProcessingSlot(
        const TorrentProcessingSlot&) = delete;

    TorrentProcessingSlot& operator=(
        const TorrentProcessingSlot&) = delete;
};


bool resolveExistingHashOrMagnet(
    ProviderResult& item)
{
    std::string hash =
        normalizeInfoHash(
            item.infoHash);

    if (hash.empty()) {
        hash =
            hashFromMagnet(
                item.magnet);
    }

    if (hash.empty())
        return false;

    item.infoHash = hash;

    if (item.magnet.empty()) {
        item.magnet =
            sgb_api::magnetFromHash(
                hash,
                item.title);
    }

    return !item.magnet.empty();
}


bool downloadTorrentPayload(
    const ProviderResult& item,
    std::string& torrent)
{
    if (item.torrentUrl.empty())
        return false;

    try {
        // Maximum 10 simultaneous .torrent downloads
        // across the live search.
        TorrentDownloadSlot downloadSlot;

        torrent =
            sgb_api::httpGet(
                item.torrentUrl,
                "application/x-bittorrent, application/octet-stream, */*",
                0L,
                30L);

        constexpr size_t maxTorrentBytes =
            8 * 1024 * 1024;

        if (
            torrent.empty() ||
            torrent.size() > maxTorrentBytes
        ) {
            torrent.clear();
            return false;
        }

        return true;
    }
    catch (...) {
        torrent.clear();
        return false;
    }
}


bool processTorrentPayload(
    ProviderResult& item,
    const std::string& torrent)
{
    if (torrent.empty())
        return false;

    try {
        // Separate processing stage:
        // maximum 10 simultaneous hash calculations.
        TorrentProcessingSlot processingSlot;

        const std::string hash =
            lowerAscii(
                sgb_torrent::infoHashV1(
                    torrent));

        if (hash.size() != 40)
            return false;

        item.infoHash = hash;

        if (item.magnet.empty()) {
            item.magnet =
                sgb_api::magnetFromHash(
                    hash,
                    item.title);
        }

        return !item.magnet.empty();
    }
    catch (...) {
        return false;
    }
}

std::string candidateKey(const Candidate& candidate) {
    std::string hash =
        normalizeInfoHash(
            candidate.provider.infoHash);

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

    {
        std::lock_guard<std::mutex> lock(resultMutex);
        liveReleases.clear();
        liveStatuses.clear();
    }

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
void LiveSearchProgress::publishResult(
    const Release& release,
    const std::string& hash,
    const DebridTorrentStatus& status)
{
    std::lock_guard<std::mutex> lock(resultMutex);
    // Several providers may find the same hash. Preserve the first
    // listing, but enrich previously unavailable peer counts.
    for (auto& existing : liveReleases) {
        if (existing.infoHash != hash)
            continue;
        if (existing.seeders < 0 && release.seeders >= 0)
            existing.seeders = release.seeders;
        if (existing.leechers < 0 && release.leechers >= 0)
            existing.leechers = release.leechers;
        return;
    }
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
    LiveSearchProgress& progress,
    const std::shared_ptr<
        std::atomic<bool>>& cancelRequested
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
        constexpr size_t debridBatchSize = 10;
        constexpr size_t torrentWorkers = 10;

        std::atomic<size_t> nextJob{0};
        std::mutex dedupeMutex;
        std::set<std::string> seen;

        auto worker = [&]() {
            sgb_api::setHttpCancelFlag(
                cancelRequested.get());

            while (true) {
                if (
                    cancelRequested &&
                    cancelRequested->load()
                ) {
                    break;
                }

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

                        if (
                            cancelRequested &&
                            cancelRequested->load()
                        ) {
                            break;
                        }

                        providerRows = found.size();

                        // "Candidates" means exactly what the
                        // provider/indexer returned.
                        progress.candidatesFound.fetch_add(
                            providerRows);

                        fresh.reserve(found.size());

                        for (auto& item : found) {
                            if (item.title.empty())
                                continue;

                            if (
                                item.infoHash.empty() &&
                                item.magnet.empty() &&
                                item.torrentUrl.empty()
                            ) {
                                continue;
                            }

                            fresh.push_back({
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
                    providerRows,
                    providerError);

                if (
                    cancelRequested &&
                    cancelRequested->load()
                ) {
                    break;
                }

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

                        // =================================================
                        // Ready candidates feed straight into this queue.
                        //
                        // MoviesAndSeries behaviour is preserved:
                        // debrid checks are chunks of up to 10.
                        //
                        // Difference:
                        // hash/magnet results do NOT wait for torrent-only
                        // results to finish downloading.
                        // =================================================

                        std::deque<size_t> readyQueue;

                        std::mutex readyMutex;
                        std::condition_variable readyCv;

                        bool resolvingDone = false;


                        auto enqueueReady =
                            [&](size_t index)
                        {
                            {
                                std::lock_guard<std::mutex>
                                    lock(readyMutex);

                                readyQueue.push_back(
                                    index);
                            }

                            readyCv.notify_one();
                        };


                        // =================================================
                        // Debrid consumer
                        //
                        // Runs concurrently with .torrent downloading /
                        // hashing.
                        // =================================================

                        auto debridWorker = [&]() {
                            setDebridCancelFlag(
                                cancelRequested.get());

                            auto publishChecked =
                                [&](size_t freshIndex,
                                    const std::map<
                                        std::string,
                                        DebridTorrentStatus
                                    >& checked)
                            {
                                const std::string hash =
                                    normalizeInfoHash(
                                        fresh[freshIndex]
                                            .provider
                                            .infoHash);

                                if (hash.empty())
                                    return;

                                auto state =
                                    checked.find(hash);

                                if (
                                    state ==
                                        checked.end()
                                ) {
                                    return;
                                }

                                bool firstResult = false;

                                {
                                    std::lock_guard<std::mutex>
                                        lock(dedupeMutex);

                                    firstResult =
                                        seen.insert(
                                            "h:" + hash)
                                            .second;
                                }

                                // Duplicate hashes can still contribute
                                // missing seed/leech counts to the first row.
                                Release release;

                                release.title =
                                    fresh[freshIndex]
                                        .provider
                                        .title;

                                release.magnet =
                                    fresh[freshIndex]
                                        .provider
                                        .magnet;

                                release.infoHash =
                                    hash;

                                release.source =
                                    fresh[freshIndex]
                                        .source;
                                release.seeders =
                                    fresh[freshIndex].provider.seeders;
                                release.leechers =
                                    fresh[freshIndex].provider.leechers;

                                progress.publishResult(
                                    release,
                                    hash,
                                    state->second);

                                if (firstResult)
                                    progress.validFound.fetch_add(1);
                            };

                            while (true) {

                                std::vector<size_t>
                                    indexes;

                                {
                                    std::unique_lock<std::mutex>
                                        lock(readyMutex);

                                    readyCv.wait_for(
                                        lock,
                                        std::chrono::milliseconds(100),
                                        [&]() {
                                            return
                                                !readyQueue.empty() ||
                                                resolvingDone ||
                                                (
                                                    cancelRequested &&
                                                    cancelRequested->load()
                                                );
                                        });

                                    if (
                                        cancelRequested &&
                                        cancelRequested->load()
                                    ) {
                                        break;
                                    }

                                    if (
                                        readyQueue.empty() &&
                                        resolvingDone
                                    ) {
                                        break;
                                    }

                                    const size_t take =
                                        std::min(
                                            debridBatchSize,
                                            readyQueue.size());

                                    if (take == 0)
                                        continue;

                                    indexes.reserve(take);

                                    for (
                                        size_t i = 0;
                                        i < take;
                                        ++i
                                    ) {
                                        indexes.push_back(
                                            readyQueue.front());

                                        readyQueue.pop_front();
                                    }
                                }


                                std::vector<DebridCandidate>
                                    batch;

                                batch.reserve(
                                    indexes.size());

                                for (
                                    size_t freshIndex :
                                        indexes
                                ) {
                                    batch.push_back({
                                        fresh[freshIndex]
                                            .provider
                                            .infoHash,

                                        fresh[freshIndex]
                                            .provider
                                            .magnet
                                    });
                                }


                                std::map<
                                    std::string,
                                    DebridTorrentStatus
                                > checked;

                                bool batchFailed = false;
                                std::string batchError;

                                try {
                                    checked =
                                        backend->check(
                                            batch);
                                }
                                catch (const std::exception& e) {
                                    batchFailed = true;
                                    batchError = e.what();
                                }
                                catch (...) {
                                    batchFailed = true;
                                    batchError =
                                        "Unknown debrid error";
                                }

                                if (
                                    cancelRequested &&
                                    cancelRequested->load()
                                ) {
                                    break;
                                }

                                if (batchFailed) {
                                    const bool systemicFailure =
                                        batchError.find(
                                            "rate limit") !=
                                                std::string::npos ||
                                        batchError.find(
                                            "HTTP 429") !=
                                                std::string::npos ||
                                        batchError.find(
                                            "HTTP 503") !=
                                                std::string::npos ||
                                        batchError.find(
                                            "network request failed") !=
                                                std::string::npos;

                                    progress.setText(
                                        "Debrid check failed",
                                        job.id +
                                            ": " +
                                            batchError);

                                    if (systemicFailure) {
                                        progress.debridChecked
                                            .fetch_add(
                                                indexes.size());

                                        continue;
                                    }

                                    // A malformed/rejected candidate must not
                                    // discard the other candidates in its batch.
                                    // Retry the batch one item at a time so good
                                    // torrents can still be published.
                                    for (
                                        size_t freshIndex :
                                            indexes
                                    ) {
                                        if (
                                            cancelRequested &&
                                            cancelRequested->load()
                                        ) {
                                            break;
                                        }

                                        std::vector<DebridCandidate>
                                            single;

                                        single.push_back({
                                            fresh[freshIndex]
                                                .provider
                                                .infoHash,
                                            fresh[freshIndex]
                                                .provider
                                                .magnet
                                        });

                                        try {
                                            auto one =
                                                backend->check(
                                                    single);

                                            publishChecked(
                                                freshIndex,
                                                one);
                                        }
                                        catch (const std::exception& e) {
                                            progress.setText(
                                                "Debrid item failed",
                                                job.id +
                                                    ": " +
                                                    e.what());
                                        }
                                        catch (...) {
                                            progress.setText(
                                                "Debrid item failed",
                                                job.id);
                                        }

                                        progress.debridChecked
                                            .fetch_add(1);
                                    }

                                    continue;
                                }

                                for (
                                    size_t freshIndex :
                                        indexes
                                ) {
                                    publishChecked(
                                        freshIndex,
                                        checked);
                                }

                                progress.debridChecked
                                    .fetch_add(
                                        indexes.size());
                            }
                        };


                        std::thread debridThread(
                            debridWorker);


                        // =================================================
                        // First send existing hashes/magnets straight
                        // into the debrid queue.
                        //
                        // Torrent-only candidates are separated out.
                        // =================================================

                        std::vector<size_t>
                            torrentIndexes;

                        torrentIndexes.reserve(
                            fresh.size());


                        for (
                            size_t i = 0;
                            i < fresh.size();
                            ++i
                        ) {
                            if (
                                cancelRequested &&
                                cancelRequested->load()
                            ) {
                                break;
                            }

                            if (
                                resolveExistingHashOrMagnet(
                                    fresh[i].provider)
                            ) {
                                enqueueReady(i);
                                continue;
                            }


                            if (
                                !fresh[i]
                                    .provider
                                    .torrentUrl
                                    .empty()
                            ) {
                                torrentIndexes.push_back(i);
                                continue;
                            }


                            // Candidate could not be resolved.
                            progress.debridChecked
                                .fetch_add(1);
                        }


                        // =================================================
                        // Torrent-only workers
                        //
                        // Each worker does:
                        //
                        // download ONE torrent
                        //       ↓
                        // immediately hash THAT torrent
                        //       ↓
                        // generate magnet
                        //       ↓
                        // immediately feed debrid queue
                        //
                        // There is NO download-all -> wait -> hash-all
                        // barrier anymore.
                        // =================================================

                        std::atomic<size_t>
                            nextTorrent{0};


                        auto torrentWorker = [&]() {
                            sgb_api::setHttpCancelFlag(
                                cancelRequested.get());

                            while (true) {
                                if (
                                    cancelRequested &&
                                    cancelRequested->load()
                                ) {
                                    break;
                                }

                                const size_t position =
                                    nextTorrent.fetch_add(1);

                                if (
                                    position >=
                                    torrentIndexes.size()
                                ) {
                                    break;
                                }


                                const size_t i =
                                    torrentIndexes[
                                        position
                                    ];


                                std::string torrent;


                                if (
                                    !downloadTorrentPayload(
                                        fresh[i].provider,
                                        torrent)
                                ) {
                                    progress.debridChecked
                                        .fetch_add(1);

                                    continue;
                                }


                                // Hash this torrent immediately after
                                // this individual download finishes.
                                if (
                                    !processTorrentPayload(
                                        fresh[i].provider,
                                        torrent)
                                ) {
                                    progress.debridChecked
                                        .fetch_add(1);

                                    continue;
                                }


                                // It can now enter the exact same
                                // debrid path as native hash/magnet
                                // results.
                                enqueueReady(i);
                            }
                        };


                        const size_t torrentWorkerCount =
                            std::min(
                                torrentWorkers,
                                torrentIndexes.size());


                        std::vector<std::thread>
                            torrentThreads;

                        torrentThreads.reserve(
                            torrentWorkerCount);


                        for (
                            size_t i = 0;
                            i < torrentWorkerCount;
                            ++i
                        ) {
                            torrentThreads.emplace_back(
                                torrentWorker);
                        }


                        for (
                            auto& thread :
                                torrentThreads
                        ) {
                            thread.join();
                        }


                        // All candidates have now either:
                        //
                        // - entered the ready queue, or
                        // - failed torrent resolution.
                        //
                        // Allow the final partial MoviesAndSeries
                        // batch (<10) to flush.

                        {
                            std::lock_guard<std::mutex>
                                lock(readyMutex);

                            resolvingDone = true;
                        }

                        readyCv.notify_all();

                        debridThread.join();
                    }
                    else {
                        progress.debridChecked
                            .fetch_add(
                                fresh.size());
                    }
                }
                if (
                    cancelRequested &&
                    cancelRequested->load()
                ) {
                    break;
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

        if (
            cancelRequested &&
            cancelRequested->load()
        ) {
            output.message =
                "Search cancelled";

            output.success = false;

            progress.setText(
                "Cancelled",
                "");
        }
        else {
            output.message =
                "Search complete: " +
                std::to_string(
                    output.releases.size()) +
                " torrent(s)";

            output.success = true;

            progress.setText(
                "Complete",
                "");
        }
    }
    catch (const std::exception& e) {
        if (
            cancelRequested &&
            cancelRequested->load()
        ) {
            output.message =
                "Search cancelled";

            progress.setText(
                "Cancelled",
                "");
        }
        else {
            output.message = e.what();

            progress.setText(
                "Error",
                "");
        }
    }

    progress.running.store(false);
    return output;
}
} // namespace sgb












