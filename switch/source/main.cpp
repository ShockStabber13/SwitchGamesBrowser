#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include "catalog.hpp"
#include "debrid.hpp"
#include "provider.hpp"
#include "live_search.hpp"
#include "scrape_cache.hpp"
#include "provider_diagnostics.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <sys/stat.h>
#include <set>
#include <thread>
#include <vector>

static const std::string root = "sdmc:/switch/SwitchGamesBrowser/";
static std::atomic<bool> stopping{false};
static SDL_Color green{21,190,25,255}, white{245,245,245,255}, muted{155,155,155,255};

struct Download { std::string bytes; size_t limit; };
static size_t receive(char* data, size_t size, size_t count, void* opaque) {
    auto* d = static_cast<Download*>(opaque);
    if (size && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    if (n > d->limit - d->bytes.size()) return 0;
    d->bytes.append(data, n); return n;
}
static std::string get(const std::string& url, size_t maxBytes) {
    if (url.rfind("https://", 0) != 0) throw std::runtime_error("HTTPS URL required");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Network initialization failed");
    Download d{{}, maxBytes};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "SwitchGamesBrowser/0.4");
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    #if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return stopping.load() ? 1 : 0; });
    auto result = curl_easy_perform(curl.get());
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK || status < 200 || status >= 300) throw std::runtime_error("HTTPS fetch failed; cached index retained");
    return d.bytes;
}
static std::string digest(const std::string& bytes) {
    unsigned char hash[32]; sha256CalculateHash(hash, bytes.data(), bytes.size());
    std::string hex; static const char* digits = "0123456789abcdef";
    for (auto b : hash) { hex += digits[b >> 4]; hex += digits[b & 15]; } return hex;
}
static void atomicWrite(const std::string& path, const std::string& bytes) {
    std::string temp = path + ".tmp", backup = path + ".bak";
    { std::ofstream f(temp, std::ios::binary | std::ios::trunc); f.write(bytes.data(), bytes.size()); f.flush(); if (!f) throw std::runtime_error("SD write failed"); }
    std::ifstream existing(path); bool exists = existing.good(); existing.close();
    if (exists) { std::remove(backup.c_str()); if (std::rename(path.c_str(), backup.c_str())) throw std::runtime_error("Cannot preserve old cache"); }
    if (std::rename(temp.c_str(), path.c_str())) { if (exists) std::rename(backup.c_str(), path.c_str()); throw std::runtime_error("Cache replacement failed"); }
    std::remove(backup.c_str());
}
struct Refresh {
    std::vector<sgb::Game> games;
    std::string message;
};

static std::string siblingUrl(
    const std::string& indexUrl,
    const std::string& filename
) {
    if (
        indexUrl.rfind(
            "https://raw.githubusercontent.com/",
            0
        ) != 0 ||
        indexUrl.find('?') != std::string::npos
    ) {
        throw std::runtime_error(
            "Configure a raw GitHub indexUrl first"
        );
    }

    auto slash = indexUrl.find_last_of('/');

    if (slash == std::string::npos)
        throw std::runtime_error("Invalid indexUrl");

    return indexUrl.substr(0, slash + 1) + filename;
}


struct CoverCacheStats {
    size_t cached = 0;
    size_t total = 0;
};

static std::atomic<int> catalogRefreshPhase{0};
static std::atomic<size_t> catalogCoverDone{0};
static std::atomic<size_t> catalogCoverTotal{0};
static std::atomic<size_t> catalogCoverCached{0};

static std::vector<std::string> offlineCoverCandidates(
    const std::string& url
) {
    std::vector<std::string> urls;

    auto add = [&](const std::string& value) {
        if (
            !value.empty() &&
            std::find(urls.begin(), urls.end(), value) == urls.end()
        ) {
            urls.push_back(value);
        }
    };

    const std::string big2x = "/t_cover_big_2x/";
    const std::string big = "/t_cover_big/";

    if (url.find(big2x) != std::string::npos) {
        add(url);

        std::string fallback = url;
        fallback.replace(
            fallback.find(big2x),
            big2x.size(),
            big
        );

        add(fallback);
    } else {
        add(url);
    }

    return urls;
}

static bool validImageBytes(const std::string& bytes) {
    if (bytes.size() < 12)
        return false;

    const auto* p =
        reinterpret_cast<const unsigned char*>(bytes.data());

    // JPEG
    if (p[0] == 0xff && p[1] == 0xd8)
        return true;

    // PNG
    if (
        p[0] == 0x89 &&
        p[1] == 0x50 &&
        p[2] == 0x4e &&
        p[3] == 0x47
    )
        return true;

    // WEBP
    if (
        bytes.compare(0, 4, "RIFF") == 0 &&
        bytes.compare(8, 4, "WEBP") == 0
    )
        return true;

    return false;
}

static bool cachedImageValid(const std::string& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file)
        return false;

    char header[12]{};
    file.read(header, sizeof(header));

    if (file.gcount() < 12)
        return false;

    return validImageBytes(
        std::string(header, sizeof(header))
    );
}

static CoverCacheStats cacheCatalogCovers(
    const std::vector<sgb::Game>& games
) {
    struct Job {
        std::string url;
        std::string path;
    };

    std::vector<Job> jobs;
    std::set<std::string> seen;

    mkdir((root + "covers").c_str(), 0777);

    for (const auto& game : games) {
        if (game.cover.empty())
            continue;

        std::string path =
            root +
            "covers/" +
            digest(game.cover) +
            ".img";

        if (!seen.insert(path).second)
            continue;

        jobs.push_back({
            game.cover,
            path
        });
    }

    catalogCoverTotal.store(jobs.size());
    catalogCoverDone.store(0);
    catalogCoverCached.store(0);

    std::atomic<size_t> next{0};
    std::atomic<size_t> cached{0};

    auto worker = [&]() {
        while (true) {
            size_t i = next.fetch_add(1);

            if (i >= jobs.size())
                break;

            const auto& job = jobs[i];

            if (cachedImageValid(job.path)) {
                cached.fetch_add(1);
                catalogCoverCached.fetch_add(1);
                catalogCoverDone.fetch_add(1);
                continue;
            }

            std::remove(job.path.c_str());

            for (
                const auto& candidate :
                offlineCoverCandidates(job.url)
            ) {
                try {
                    auto bytes = get(
                        candidate,
                        8 * 1024 * 1024
                    );

                    if (!validImageBytes(bytes))
                        continue;

                    atomicWrite(
                        job.path,
                        bytes
                    );

                    cached.fetch_add(1);
                    catalogCoverCached.fetch_add(1);
                    break;
                }
                catch (...) {
                    // Try fallback URL.
                }
            }

            catalogCoverDone.fetch_add(1);
        }
    };

    size_t workerCount =
        std::min<size_t>(8, jobs.size());

    std::vector<std::thread> workers;

    for (size_t i = 0; i < workerCount; ++i)
        workers.emplace_back(worker);

    for (auto& thread : workers)
        thread.join();

    return {
        cached.load(),
        jobs.size()
    };
}

static size_t downloadCoverPacks(
    const std::string& indexUrl
);

static Refresh refreshCatalog(
    const std::string& indexUrl,
    const std::vector<sgb::Game>& current
) {
    try {
        catalogRefreshPhase.store(1);
        catalogCoverDone.store(0);
        catalogCoverTotal.store(0);
        catalogCoverCached.store(0);

        auto url = siblingUrl(
            indexUrl,
            "igdb-switch-games.json"
        );

        auto bytes = get(
            url,
            SIZE_MAX
        );

        auto catalog =
            sgb::parseIgdbCatalog(bytes);

        sgb::mergeReleases(
            catalog,
            current
        );

        atomicWrite(
            root + "igdb-switch-games.json",
            bytes
        );

        catalogRefreshPhase.store(2);

        auto packCount =
            downloadCoverPacks(indexUrl);

        return {
            std::move(catalog),
            "Catalog refreshed; " +
            std::to_string(packCount) +
            " cover packs ready"
        };
    }
    catch (const std::exception& e) {
        return {{}, e.what()};
    }
}

static bool mergeCachedTorrentShards(
    std::vector<sgb::Game>& games
) {
    try {
        if (games.empty())
            return false;

        auto manifestBytes =
            sgb::read(
                root + "torrent-manifest.json"
            );

        auto manifest =
            sgb::Json::parse(
                manifestBytes
            );

        if (
            manifest.value("schemaVersion", 0) != 1 ||
            !manifest.contains("shards") ||
            !manifest["shards"].is_array()
        ) {
            return false;
        }

        // Remove stale release data before applying
        // the current shard set.
        for (auto& game : games)
            game.releases.clear();

        for (
            const auto& info :
            manifest["shards"]
        ) {
            if (!info.is_object())
                continue;

            std::string filename =
                info.value("file", "");

            size_t expectedBytes =
                info.value("bytes", 0u);

            std::string expectedHash =
                info.value("sha256", "");

            if (
                filename.empty() ||
                expectedBytes == 0 ||
                expectedHash.empty()
            ) {
                return false;
            }

            auto bytes =
                sgb::read(root + filename);

            if (
                bytes.size() != expectedBytes ||
                digest(bytes) != expectedHash
            ) {
                return false;
            }

            auto shard =
                sgb::parseTorrentShard(
                    bytes
                );

            sgb::mergeReleases(
                games,
                shard
            );
        }

        return true;
    }
    catch (...) {
        return false;
    }
}

static Refresh refreshTorrents(
    const std::string& indexUrl,
    const std::vector<sgb::Game>& current
) {
    try {
        if (current.empty()) {
            throw std::runtime_error(
                "Refresh Catalog Index first"
            );
        }

        auto manifestUrl =
            siblingUrl(
                indexUrl,
                "torrent-manifest.json"
            );

        auto manifestBytes =
            get(
                manifestUrl,
                2 * 1024 * 1024
            );

        auto manifest =
            sgb::Json::parse(
                manifestBytes
            );

        if (
            manifest.value("schemaVersion", 0) != 1 ||
            !manifest.contains("shards") ||
            !manifest["shards"].is_array()
        ) {
            throw std::runtime_error(
                "Unsupported torrent manifest"
            );
        }

        const auto& shards =
            manifest["shards"];

        std::vector<sgb::Game> merged =
            current;

        // Important: remove old torrent results.
        // Anything absent from the new shards should
        // disappear instead of remaining stale.
        for (auto& game : merged)
            game.releases.clear();

        size_t done = 0;
        size_t cached = 0;

        for (const auto& info : shards) {
            if (!info.is_object()) {
                throw std::runtime_error(
                    "Invalid torrent shard entry"
                );
            }

            std::string filename =
                info.value("file", "");

            size_t expectedBytes =
                info.value("bytes", 0u);

            std::string expectedHash =
                info.value("sha256", "");

            if (
                filename.empty() ||
                expectedBytes == 0 ||
                expectedHash.empty()
            ) {
                throw std::runtime_error(
                    "Invalid torrent shard metadata"
                );
            }

            std::string localPath =
                root + filename;

            std::string bytes;
            bool localGood = false;

            try {
                bytes =
                    sgb::read(localPath);

                localGood =
                    bytes.size() == expectedBytes &&
                    digest(bytes) == expectedHash;
            }
            catch (...) {
                localGood = false;
            }

            if (localGood) {
                ++cached;
            }
            else {
                bytes =
                    get(
                        siblingUrl(
                            indexUrl,
                            filename
                        ),
                        SIZE_MAX
                    );

                if (
                    bytes.size() != expectedBytes ||
                    digest(bytes) != expectedHash
                ) {
                    throw std::runtime_error(
                        "Torrent shard verification failed: " +
                        filename
                    );
                }

                atomicWrite(
                    localPath,
                    bytes
                );
            }

            auto shard =
                sgb::parseTorrentShard(
                    bytes
                );

            sgb::mergeReleases(
                merged,
                shard
            );

            ++done;
        }

        // Write manifest last so an interrupted refresh
        // never points to incomplete shard files.
        atomicWrite(
            root + "torrent-manifest.json",
            manifestBytes
        );

        return {
            std::move(merged),
            "Torrent index refreshed; " +
            std::to_string(done) +
            " shards ready (" +
            std::to_string(cached) +
            " cached)"
        };
    }
    catch (const std::exception& e) {
        return {{}, e.what()};
    }
}

static std::string downloadLangegenCatalog(
    const std::string& sourceUrl
) {
    try {
        if (sourceUrl.empty())
            throw std::runtime_error(
                "Configure Langegen catalog URL first");

        auto bytes = get(
            sourceUrl,
            SIZE_MAX
        );

        auto json = sgb::Json::parse(
            bytes,
            nullptr,
            false
        );

        if (json.is_discarded())
            throw std::runtime_error(
                "Downloaded Langegen catalog is invalid JSON");

        const sgb::Json* rows = nullptr;

        if (json.is_array()) {
            rows = &json;
        }
        else if (json.is_object()) {
            if (
                json.contains("data") &&
                json["data"].is_array()
            ) {
                rows = &json["data"];
            }
            else if (
                json.contains("games") &&
                json["games"].is_array()
            ) {
                rows = &json["games"];
            }
        }

        if (!rows || rows->empty())
            throw std::runtime_error(
                "Downloaded Langegen catalog has no rows");

        size_t usable = 0;

        for (const auto& row : *rows) {
            if (!row.is_object())
                continue;

            std::string title;

            if (
                row.contains("title") &&
                row["title"].is_string()
            ) {
                title =
                    row["title"].get<std::string>();
            }
            else if (
                row.contains("name") &&
                row["name"].is_string()
            ) {
                title =
                    row["name"].get<std::string>();
            }

            if (
                !title.empty() &&
                row.contains("magnet") &&
                row["magnet"].is_string() &&
                row["magnet"]
                    .get<std::string>()
                    .rfind("magnet:?", 0) == 0
            ) {
                ++usable;
            }
        }

        if (usable == 0)
            throw std::runtime_error(
                "Langegen catalog contains no usable torrent rows");

        // atomicWrite preserves the previous working cache
        // until the replacement has been written successfully.
        atomicWrite(
            root + "langegen.json",
            bytes
        );

        return
            "Langegen catalog updated: " +
            std::to_string(usable) +
            " rows";
    }
    catch (const std::exception& e) {
        return e.what();
    }
}
static std::string keyboard(const char* label, const std::string& initial) {
    SwkbdConfig config; char out[256]{};
    if (R_FAILED(swkbdCreate(&config, 0))) return initial;
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, label); swkbdConfigSetInitialText(&config, initial.c_str());
    swkbdConfigSetStringLenMax(&config, sizeof(out)-1);
    auto result = swkbdShow(&config, out, sizeof(out)); swkbdClose(&config);
    return R_SUCCEEDED(result) ? std::string(out) : initial;
}
static void rect(SDL_Renderer* r, SDL_Rect box, SDL_Color colour, bool outline = false) {
    SDL_SetRenderDrawColor(r, colour.r, colour.g, colour.b, colour.a);
    if (outline) SDL_RenderDrawRect(r, &box); else SDL_RenderFillRect(r, &box);
}
static void label(SDL_Renderer* renderer, TTF_Font* font, std::string text, int x, int y, int width, SDL_Color colour = white, bool wrapped = false) {
    if (text.empty()) return;
    // Clip without splitting UTF-8 continuation bytes.
    int w = 0, h = 0;
    if (!wrapped) while (!text.empty() && TTF_SizeUTF8(font, text.c_str(), &w, &h) == 0 && w > width) {
        text.pop_back(); while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
        // Remove the leading byte of the final multibyte character if necessary.
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xc0) text.pop_back();
    }
    SDL_Surface* surface = wrapped ? TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), colour, width) : TTF_RenderUTF8_Blended(font, text.c_str(), colour);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect target{x,y,surface->w,surface->h}; SDL_FreeSurface(surface);
    if (texture) { SDL_RenderCopy(renderer, texture, nullptr, &target); SDL_DestroyTexture(texture); }
}
static std::string score(const sgb::Game& g) {
    if (g.rating < 0) return "Unrated";
    return std::to_string(static_cast<int>(g.rating + .5)) + "/100  (" + std::to_string(g.ratingCount) + " votes)";
}


struct CoverResult { std::string id, path; };

struct PackedCoverEntry {
    int pack = 0;
    size_t offset = 0;
    size_t size = 0;
};

static std::map<std::string, PackedCoverEntry> packedCovers;

static bool loadCoverPackManifest() {
    try {
        auto manifest = sgb::Json::parse(
            sgb::read(root + "covers-manifest.json")
        );

        if (
            manifest.value("schemaVersion", 0) != 1 ||
            !manifest.contains("entries") ||
            !manifest["entries"].is_object()
        ) {
            return false;
        }

        std::map<std::string, PackedCoverEntry> next;

        for (
            auto it = manifest["entries"].begin();
            it != manifest["entries"].end();
            ++it
        ) {
            const auto& value = it.value();

            if (!value.is_object())
                continue;

            PackedCoverEntry entry;
            entry.pack = value.value("pack", -1);
            entry.offset = value.value("offset", 0u);
            entry.size = value.value("size", 0u);

            if (
                entry.pack < 0 ||
                entry.pack > 15 ||
                entry.size == 0
            ) {
                continue;
            }

            next[it.key()] = entry;
        }

        packedCovers = std::move(next);
        return !packedCovers.empty();
    }
    catch (...) {
        packedCovers.clear();
        return false;
    }
}

static SDL_Texture* loadPackedCoverTexture(
    SDL_Renderer* renderer,
    const std::string& gameId
) {
    auto found = packedCovers.find(gameId);

    if (found == packedCovers.end())
        return nullptr;

    const auto& entry = found->second;

    static const char hex[] = "0123456789abcdef";

    std::string path =
        root +
        "covers-0" +
        std::string(1, hex[entry.pack]) +
        ".pack";

    std::ifstream file(
        path,
        std::ios::binary
    );

    if (!file)
        return nullptr;

    file.seekg(
        static_cast<std::streamoff>(entry.offset)
    );

    if (!file)
        return nullptr;

    std::vector<unsigned char> bytes(
        entry.size
    );

    file.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );

    if (!file)
        return nullptr;

    SDL_RWops* rw = SDL_RWFromConstMem(
        bytes.data(),
        static_cast<int>(bytes.size())
    );

    if (!rw)
        return nullptr;

    SDL_Surface* source =
        IMG_Load_RW(rw, 1);

    if (!source)
        return nullptr;

    SDL_Surface* portrait =
        SDL_CreateRGBSurfaceWithFormat(
            0,
            150,
            212,
            32,
            SDL_PIXELFORMAT_RGBA32
        );

    if (!portrait) {
        SDL_FreeSurface(source);
        return nullptr;
    }

    SDL_BlitScaled(
        source,
        nullptr,
        portrait,
        nullptr
    );

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(
            renderer,
            portrait
        );

    SDL_FreeSurface(portrait);
    SDL_FreeSurface(source);

    return texture;
}

static size_t downloadCoverPacks(
    const std::string& indexUrl
) {
    auto manifestUrl = siblingUrl(
        indexUrl,
        "covers-manifest.json"
    );

    auto manifestBytes = get(
        manifestUrl,
        8 * 1024 * 1024
    );

    auto manifest =
        sgb::Json::parse(manifestBytes);

    if (
        manifest.value("schemaVersion", 0) != 1 ||
        !manifest.contains("packs") ||
        !manifest["packs"].is_object()
    ) {
        throw std::runtime_error(
            "Invalid cover manifest"
        );
    }

    const auto& packs = manifest["packs"];

    catalogCoverTotal.store(packs.size());
    catalogCoverDone.store(0);
    catalogCoverCached.store(0);

    size_t ready = 0;

    for (
        auto it = packs.begin();
        it != packs.end();
        ++it
    ) {
        const auto& info = it.value();

        std::string filename =
            info.value("file", "");

        size_t expectedBytes =
            info.value("bytes", 0u);

        std::string expectedHash =
            info.value("sha256", "");

        if (
            filename.empty() ||
            expectedBytes == 0 ||
            expectedHash.empty()
        ) {
            throw std::runtime_error(
                "Invalid cover pack metadata"
            );
        }

        std::string localPath =
            root + filename;

        bool alreadyGood = false;

        try {
            auto local =
                sgb::read(localPath);

            alreadyGood =
                local.size() == expectedBytes &&
                digest(local) == expectedHash;
        }
        catch (...) {}

        if (alreadyGood) {
            ++ready;
            catalogCoverCached.fetch_add(1);
            catalogCoverDone.fetch_add(1);
            continue;
        }

        auto packBytes = get(
            siblingUrl(indexUrl, filename),
            SIZE_MAX
        );

        if (
            packBytes.size() != expectedBytes ||
            digest(packBytes) != expectedHash
        ) {
            throw std::runtime_error(
                "Cover pack verification failed: " +
                filename
            );
        }

        atomicWrite(
            localPath,
            packBytes
        );

        ++ready;
        catalogCoverDone.fetch_add(1);
    }

    // Manifest is written last so it never points to
    // partially-downloaded packs.
    atomicWrite(
        root + "covers-manifest.json",
        manifestBytes
    );

    return ready;
}


static std::vector<std::string> coverCandidates(
    const std::string& url
) {
    std::vector<std::string> result;

    auto add = [&](const std::string& value) {
        if (
            !value.empty() &&
            std::find(result.begin(), result.end(), value) ==
                result.end()
        ) {
            result.push_back(value);
        }
    };

    const std::string big =
        "/t_cover_big/";

    const std::string big2x =
        "/t_cover_big_2x/";

    if (url.find(big2x) != std::string::npos) {
        add(url);

        std::string fallback = url;
        fallback.replace(
            fallback.find(big2x),
            big2x.size(),
            big
        );

        add(fallback);
    }
    else if (url.find(big) != std::string::npos) {
        std::string better = url;
        better.replace(
            better.find(big),
            big.size(),
            big2x
        );

        add(better);
        add(url);
    }
    else {
        add(url);
    }

    return result;
}

struct DebridCheckResult { std::map<std::string, sgb::DebridTorrentStatus> rows; std::string message; };
struct DebridAddResult { std::string hash; sgb::DebridTorrentStatus state; std::string message; };
struct DebridManagerResult { std::vector<sgb::DebridAccountTorrent> rows; std::string message; };
enum class Page { Browse, Detail, Torrents, Files, Settings, Providers, SearchProgress, Options, ScrapeChoice, DebridManager, DebridManagerFiles };

static DebridCheckResult liveDebridCheck(
    const sgb::DebridConfig& config,
    const std::vector<sgb::DebridCandidate>& candidates
) {
    DebridCheckResult result;
    for (const auto& c : candidates) result.rows[c.infoHash] = {};
    try {
        auto backend = sgb::createDebridBackend(config);
        if (!backend) throw std::runtime_error("Configure debridService and debridApiKey in config.json");
        auto checked = backend->check(candidates);
        for (auto& pair : checked) result.rows[pair.first] = std::move(pair.second);
        result.message = sgb::debridServiceName(config.service) + " status refreshed";
    } catch (const std::exception& e) {
        result.message = e.what();
    }
    return result;
}

static DebridAddResult liveDebridAdd(
    const sgb::DebridConfig& config,
    const std::string& hash,
    const std::string& magnet
) {
    DebridAddResult result; result.hash = hash;
    try {
        auto backend = sgb::createDebridBackend(config);
        if (!backend) throw std::runtime_error("Configure debridService and debridApiKey in config.json");
        result.state = backend->add(hash, magnet);
        result.message = "Added to " + sgb::debridServiceName(config.service);
    } catch (const std::exception& e) {
        result.message = e.what();
        result.hash.clear();
    }
    return result;
}

static DebridManagerResult loadDebridManager(
    const sgb::DebridConfig& config
) {
    DebridManagerResult result;

    try {
        auto backend =
            sgb::createDebridBackend(config);

        if (!backend) {
            throw std::runtime_error(
                "Authorize a debrid service first");
        }

        result.rows =
            backend->accountTorrents();

        result.message =
            sgb::debridServiceName(
                config.service) +
            " manager refreshed";
    }
    catch (const std::exception& e) {
        result.message = e.what();
    }

    return result;
}
int main(int, char**) {
    if (appletGetAppletType() != AppletType_Application) {
        consoleInit(nullptr); printf("Launch in application mode: hold R while opening a game.\nPress + to exit.\n");
        PadState p; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&p);
    while (appletMainLoop()) { padUpdate(&p); if (padGetButtonsDown(&p) & HidNpadButton_Plus) break; consoleUpdate(nullptr); }
        consoleExit(nullptr); return 0;
    }
    socketInitializeDefault(); romfsInit(); curl_global_init(CURL_GLOBAL_DEFAULT);
    mkdir(root.c_str(), 0777); mkdir((root + "covers").c_str(), 0777);
    if (R_FAILED(plInitialize(PlServiceType_User))) return 1;
    PlFontData fontData{};
    if (R_FAILED(plGetSharedFontByType(&fontData, PlSharedFontType_Standard))) { plExit(); return 1; }
    if (SDL_Init(SDL_INIT_VIDEO) || TTF_Init()) return 1;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    SDL_Window* window = SDL_CreateWindow("Switch Games Browser", 0, 0, 1280, 720, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    TTF_Font* small = TTF_OpenFontRW(SDL_RWFromConstMem(fontData.address, fontData.size), 1, 20);
    TTF_Font* big = TTF_OpenFontRW(SDL_RWFromConstMem(fontData.address, fontData.size), 1, 32);
    if (!renderer || !small || !big) return 1;
    SDL_RenderSetLogicalSize(renderer, 1280, 720);
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    std::vector<sgb::Game> games; std::set<std::string> favourites;
    sgb::Filter filter;
    std::string url, langegenUrl, status = "Press X for Filters";
    sgb::DebridConfig debridConfig;
    std::map<std::string, sgb::DebridTorrentStatus> debridStatuses;

    std::set<std::string> enabledProviders;
    bool providerSelectionConfigured = false;
    try {
        games = sgb::parseIgdbCatalog(
            sgb::read(root + "igdb-switch-games.json")
        );

        try {
            auto torrents = sgb::parse(
                sgb::read(root + "switch-index.json")
            );

            sgb::mergeReleases(
                games,
                torrents
            );

            status = "Loaded cached catalog + torrents";
        }
        catch (...) {
            status = "Loaded cached catalog";
        }
    }
    catch (...) {
        try {
            games = sgb::parse(
                sgb::read(root + "switch-index.json")
            );

            status = "Loaded cached torrent index";
        }
        catch (...) {}
    }
    try {
        auto config = sgb::Json::parse(
            sgb::read(root + "config.json", 65536)
        );

        url = sgb::field(config, "indexUrl", 2048);
        langegenUrl = "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/EN_catalog.json";

        debridConfig.service = sgb::parseDebridService(
            config.value("debridService", "")
        );

        debridConfig.apiKey =
            config.value("debridApiKey", "");

        if (
            config.contains("enabledProviders") &&
            config["enabledProviders"].is_array()
        ) {
            providerSelectionConfigured = true;

            for (const auto& item : config["enabledProviders"]) {
                if (item.is_string())
                    enabledProviders.insert(
                        item.get<std::string>()
                    );
            }
        }

    } catch (...) {}
    try { debridStatuses = sgb::parseDebridStatusJson(sgb::read(root + "debrid-status.json", 8 * 1024 * 1024)); } catch (...) {}
    try {
        auto state = sgb::Json::parse(sgb::read(root + "state.json", 1024 * 1024));
        filter.search = state.value("search", ""); filter.genre = state.value("genre", "");
        filter.minRating = state.value("minRating", 0.0); filter.minReviews = state.value("minReviews", 0);
        filter.favouritesOnly = state.value("favouritesOnly", false);
        filter.sort = static_cast<sgb::Sort>(std::clamp(state.value("sort", 0), 0, 2));
        for (const auto& f : state.value("favourites", sgb::Json::array())) if (f.is_string()) favourites.insert(f.get<std::string>());
    } catch (...) {}
    auto saveState = [&]() {
        try { atomicWrite(root + "state.json", sgb::Json{{"search",filter.search},{"genre",filter.genre},{"minRating",filter.minRating},{"minReviews",filter.minReviews},{"favouritesOnly",filter.favouritesOnly},{"sort",static_cast<int>(filter.sort)},{"favourites",favourites}}.dump()); }
        catch (const std::exception& e) { status = e.what(); }
    };

    auto saveConfig = [&]() {
        try {
            std::string service =
                debridConfig.service == sgb::DebridService::TorBox ? "torbox" :
                debridConfig.service == sgb::DebridService::AllDebrid ? "alldebrid" : "";

            sgb::Json providerArray =
                sgb::Json::array();

            for (const auto& id : enabledProviders)
                providerArray.push_back(id);

            atomicWrite(
                root + "config.json",
                sgb::Json{
                    {"indexUrl", url},
                    {"debridService", service},
                    {"debridApiKey", debridConfig.apiKey},
                    {"enabledProviders", providerArray}
                }.dump(2)
            );
            status = "Settings saved";
        } catch (const std::exception& e) {
            status = e.what();
        }
    };
    std::vector<size_t> rows = sgb::browse(games, filter, favourites);
    size_t cursor = 0, releaseIndex = 0, torrentCursor = 0, fileCursor = 0;
    size_t settingsCursor = 0, providerCursor = 0;
    size_t optionsCursor = 0;
    std::string torrentTextFilter;
    std::optional<sgb::TorBoxDeviceAuth> torBoxDeviceAuth;
    std::optional<sgb::AllDebridPinAuth> allDebridPin;
    loadCoverPackManifest();

    Page page = Page::Browse; bool dirty = false;
    std::set<size_t> selectedFiles;
    std::future<Refresh> pending;
    std::future<std::string> pendingLangegen; 
    std::future<DebridCheckResult> pendingDebridCheck;
    std::future<DebridAddResult> pendingDebridAdd;
    std::future<sgb::LiveSearchResult> pendingLiveSearch;
    std::future<DebridManagerResult> pendingDebridManager;
    sgb::LiveSearchProgress liveSearchProgress;

    std::shared_ptr<std::atomic<bool>>
        liveSearchCancel;

    std::optional<size_t>
        queuedLiveSearchGameIndex;

    std::map<
        std::string,
        sgb::DebridAccountTorrent
    > debridAccountByRemoteId;

    std::vector<sgb::DebridAccountTorrent>
        debridManagerRows;

    size_t debridManagerCursor = 0;
    size_t debridManagerFileCursor = 0;
    size_t debridManagerSelectedTorrent = 0;

    std::set<size_t>
        debridManagerSelectedFiles;

    auto nextDebridManagerRefresh =
        std::chrono::steady_clock::now();
    size_t liveSearchGameIndex = 0;

    size_t scrapeChoiceGameIndex = 0;
    size_t scrapeChoiceCursor = 0;
    bool scrapeChoiceHasCache = false;
    std::time_t scrapeChoiceSavedAt = 0;
    size_t scrapeChoiceReleaseCount = 0;
    std::map<std::string, SDL_Texture*> covers; std::set<std::string> attempted;
    auto rebuild = [&]() { rows = sgb::browse(games, filter, favourites); cursor = std::min(cursor, rows.empty() ? size_t(0) : rows.size()-1); dirty = true; };

    auto torrentRowsFor =
        [&](const sgb::Game& game)
    {
        std::vector<size_t> visible;

        const std::string needle =
            sgb::lower(
                torrentTextFilter);

        for (
            size_t i = 0;
            i < game.releases.size();
            ++i
        ) {
            if (needle.empty()) {
                visible.push_back(i);
                continue;
            }

            const auto& release =
                game.releases[i];

            const std::string searchable =
                sgb::lower(
                    release.title +
                    " " +
                    release.source);

            if (
                searchable.find(needle) !=
                std::string::npos
            ) {
                visible.push_back(i);
            }
        }

        return visible;
    };
    auto filesFor = [&](const sgb::Release& release) {
        std::vector<std::string> files;
        auto found = debridStatuses.find(release.infoHash);
        if (found != debridStatuses.end() && !found->second.files.empty()) {
            for (const auto& file : found->second.files) if (!file.name.empty()) files.push_back(file.name);
        }
        if (files.empty()) files = release.files;
        return files;
    };
    auto queueSelectedFiles = [&](const sgb::Game& g, const sgb::Release& release, const std::vector<std::string>& files) {
        if (selectedFiles.empty()) { status = "Select at least one file"; return; }
        auto queue = sgb::Json::array();
        try { queue = sgb::Json::parse(sgb::read(root + "install-queue.json", 4 * 1024 * 1024)); } catch (...) {}
        if (!queue.is_array()) throw std::runtime_error("Install queue format invalid");
        sgb::Json chosen = sgb::Json::array();
        for (size_t i : selectedFiles) if (i < files.size()) chosen.push_back(files[i]);
        queue.push_back({{"gameId",g.id},{"gameTitle",g.title},{"releaseTitle",release.title},{"magnet",release.magnet},{"infoHash",release.infoHash},{"source",release.source},{"files",chosen}});
        atomicWrite(root + "install-queue.json", queue.dump(2));
        status = "Queued " + std::to_string(chosen.size()) + " selected file(s)";
    };
    auto queueDebridManagerFiles =
        [&](const sgb::DebridAccountTorrent& torrent)
    {
        if (debridManagerSelectedFiles.empty()) {
            status = "Select at least one file";
            return;
        }

        auto queue =
            sgb::Json::array();

        try {
            queue =
                sgb::Json::parse(
                    sgb::read(
                        root + "install-queue.json",
                        4 * 1024 * 1024));
        }
        catch (...) {}

        if (!queue.is_array()) {
            throw std::runtime_error(
                "Install queue format invalid");
        }

        sgb::Json chosen =
            sgb::Json::array();

        for (
            size_t i :
                debridManagerSelectedFiles
        ) {
            if (
                i < torrent.files.size() &&
                !torrent.files[i].name.empty()
            ) {
                chosen.push_back(
                    torrent.files[i].name);
            }
        }

        if (chosen.empty()) {
            status = "Select at least one file";
            return;
        }

        queue.push_back({
            {"gameId", ""},
            {"gameTitle", torrent.name},
            {"releaseTitle", torrent.name},
            {"magnet", ""},
            {"infoHash", torrent.infoHash},
            {"source", sgb::debridServiceName(
                debridConfig.service)},
            {"remoteId", torrent.remoteId},
            {"files", chosen}
        });

        atomicWrite(
            root + "install-queue.json",
            queue.dump(2));

        status =
            "Queued " +
            std::to_string(chosen.size()) +
            " selected file(s) for install";
    };

    auto saveDebridStatuses = [&]() {
        try { atomicWrite(root + "debrid-status.json", sgb::serializeDebridStatusJson(debridStatuses)); }
        catch (const std::exception& e) { status = e.what(); }
    };
    auto startDebridCheck = [&](const sgb::Game& g) {
        if (debridConfig.service == sgb::DebridService::None || debridConfig.apiKey.empty()) {
            status = "Configure debridService and debridApiKey in config.json";
            return;
        }
        if (pendingDebridCheck.valid() || pendingDebridAdd.valid()) { status = "Debrid request already running"; return; }
        std::vector<sgb::DebridCandidate> candidates;
        candidates.reserve(g.releases.size());
        for (const auto& rel : g.releases) candidates.push_back({rel.infoHash, rel.magnet});
        status = "Checking " + sgb::debridServiceName(debridConfig.service) + "...";
        auto configCopy = debridConfig;
        pendingDebridCheck = std::async(std::launch::async, [configCopy, candidates]() {
            return liveDebridCheck(configCopy, candidates);
        });
    };
    auto startDebridAdd = [&](const sgb::Release& release) {
        if (debridConfig.service == sgb::DebridService::None || debridConfig.apiKey.empty()) {
            status = "Configure debridService and debridApiKey in config.json";
            return;
        }
        if (pendingDebridCheck.valid() || pendingDebridAdd.valid()) { status = "Debrid request already running"; return; }
        auto known = debridStatuses.find(release.infoHash);
        if (known != debridStatuses.end() && known->second.downloaded) { status = "Already in debrid account"; return; }
        status = "Adding to " + sgb::debridServiceName(debridConfig.service) + "...";
        auto configCopy = debridConfig;
        auto hash = release.infoHash, magnet = release.magnet;
        pendingDebridAdd = std::async(std::launch::async, [configCopy, hash, magnet]() {
            return liveDebridAdd(configCopy, hash, magnet);
        });
    };
    auto startDebridManagerRefresh = [&]() {
        if (
            debridConfig.service ==
                sgb::DebridService::None ||
            debridConfig.apiKey.empty()
        ) {
            status =
                "Authorize a debrid service first";
            return;
        }

        if (pendingDebridManager.valid())
            return;

        auto configCopy =
            debridConfig;

        pendingDebridManager =
            std::async(
                std::launch::async,
                [configCopy]() {
                    return loadDebridManager(
                        configCopy);
                });

        nextDebridManagerRefresh =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
    };

    auto openScrapeChoice = [&](size_t gameIndex) {
        if (gameIndex >= games.size())
            return;

        scrapeChoiceGameIndex = gameIndex;
        scrapeChoiceCursor = 0;

        const auto cache =
            sgb::scrapeCacheInfo(
                root,
                games[gameIndex].title,
                debridConfig.service);

        scrapeChoiceHasCache =
            cache.available;

        scrapeChoiceSavedAt =
            cache.savedAt;

        scrapeChoiceReleaseCount =
            cache.releaseCount;

        page = Page::ScrapeChoice;
    };
    auto launchLiveSearch = [&](size_t gameIndex) {
        liveSearchGameIndex = gameIndex;
        torrentCursor = 0;
        selectedFiles.clear();
        torrentTextFilter.clear();

        games[gameIndex].releases.clear();

        page = Page::SearchProgress;

        const std::string query =
            games[gameIndex].title;

        const std::string configPath =
            root + "config.json";

        auto configCopy =
            debridConfig;

        liveSearchCancel =
            std::make_shared<
                std::atomic<bool>>(false);

        auto cancelCopy =
            liveSearchCancel;

        pendingLiveSearch =
            std::async(
                std::launch::async,
                [
                    query,
                    configPath,
                    configCopy,
                    cancelCopy,
                    &liveSearchProgress
                ]() {
                    return sgb::runLiveSearch(
                        query,
                        configPath,
                        configCopy,
                        liveSearchProgress,
                        cancelCopy
                    );
                }
            );
    };

    auto cancelLiveSearch = [&]() {
        if (
            pendingLiveSearch.valid() &&
            liveSearchCancel
        ) {
            liveSearchCancel->store(true);
            status =
                "Stopping live search...";
        }
    };

    auto startLiveSearch = [&](size_t gameIndex) {
        if (gameIndex >= games.size())
            return;

        if (
            debridConfig.service ==
                sgb::DebridService::None ||
            debridConfig.apiKey.empty()
        ) {
            status =
                "Authorize TorBox or AllDebrid first";
            return;
        }

        if (pendingLiveSearch.valid()) {
            queuedLiveSearchGameIndex =
                gameIndex;

            cancelLiveSearch();

            page =
                Page::SearchProgress;

            return;
        }

        launchLiveSearch(
            gameIndex);
    };
        

    while (appletMainLoop()) {
        SDL_Event event; while (SDL_PollEvent(&event)) {} // libnx handles controller input below.
        padUpdate(&pad); u64 keys = padGetButtonsDown(&pad);
        if (keys & HidNpadButton_Plus) break;
        if (pending.valid() && pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = pending.get();
            catalogRefreshPhase.store(0); status = result.message;
            if (!result.games.empty()) {
                for (auto& item : covers)
                    SDL_DestroyTexture(item.second);

                covers.clear();
                attempted.clear();

                loadCoverPackManifest();

                games = std::move(result.games);
                page = Page::Browse;
                releaseIndex = torrentCursor = fileCursor = 0;
                selectedFiles.clear();
                rebuild();
            }
        }


        if (
            pendingLangegen.valid() &&
            pendingLangegen.wait_for(
                std::chrono::milliseconds(0)
            ) == std::future_status::ready
        ) {
            status = pendingLangegen.get();
        }
        if (pendingDebridCheck.valid() && pendingDebridCheck.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = pendingDebridCheck.get();
            for (auto& pair : result.rows) debridStatuses[pair.first] = std::move(pair.second);
            saveDebridStatuses();
            status = result.message;
        }
        if (
            pendingDebridAdd.valid() &&
            pendingDebridAdd.wait_for(
                std::chrono::milliseconds(0)) ==
                std::future_status::ready
        ) {
            auto result =
                pendingDebridAdd.get();

            if (!result.hash.empty()) {
                debridStatuses[
                    result.hash] =
                        std::move(
                            result.state);

                saveDebridStatuses();

                nextDebridManagerRefresh =
                    std::chrono::steady_clock::now();
            }

            status =
                result.message;
        }
        if (
            pendingDebridManager.valid() &&
            pendingDebridManager.wait_for(
                std::chrono::milliseconds(0)) ==
                std::future_status::ready
        ) {
            auto result =
                pendingDebridManager.get();

            debridManagerRows =
                std::move(result.rows);

            debridAccountByRemoteId.clear();

            for (
                const auto& torrent :
                    debridManagerRows
            ) {
                if (!torrent.remoteId.empty()) {
                    debridAccountByRemoteId[
                        torrent.remoteId] =
                            torrent;
                }
            }

            if (
                debridManagerCursor >=
                    debridManagerRows.size()
            ) {
                debridManagerCursor =
                    debridManagerRows.empty()
                        ? 0
                        : debridManagerRows.size() - 1;
            }

            if (
                page ==
                    Page::DebridManager
            ) {
                status =
                    result.message;
            }
        }

        if (
            (
                page == Page::DebridManager ||
                page == Page::Torrents
            ) &&
            !pendingDebridManager.valid() &&
            std::chrono::steady_clock::now() >=
                nextDebridManagerRefresh
        ) {
            bool shouldRefresh =
                page == Page::DebridManager;

            if (
                !shouldRefresh &&
                page == Page::Torrents &&
                !rows.empty()
            ) {
                const auto& game =
                    games[rows[cursor]];

                for (const auto& release :
                     game.releases) {
                    auto state =
                        debridStatuses.find(
                            release.infoHash);

                    if (
                        state ==
                            debridStatuses.end() ||
                        !state->second.downloaded ||
                        state->second.remoteId.empty()
                    ) {
                        continue;
                    }

                    auto account =
                        debridAccountByRemoteId.find(
                            state->second.remoteId);

                    if (
                        account ==
                            debridAccountByRemoteId.end() ||
                        !account->second.complete
                    ) {
                        shouldRefresh = true;
                        break;
                    }
                }
            }

            if (shouldRefresh)
                startDebridManagerRefresh();
        }

        // Stream each debrid-checked result into the list immediately.
        if (
            pendingLiveSearch.valid() &&
            liveSearchGameIndex < games.size() &&
            (
                !liveSearchCancel ||
                !liveSearchCancel->load()
            )
        ) {
            std::vector<sgb::Release>
                liveRows;

            std::map<
                std::string,
                sgb::DebridTorrentStatus
            > liveStatuses;

            liveSearchProgress.snapshotResults(
                liveRows,
                liveStatuses);

            auto& liveGame =
                games[liveSearchGameIndex];

            bool addedAny = false;

            for (const auto& release : liveRows) {
                bool alreadyPresent = false;

                for (
                    const auto& existing :
                        liveGame.releases
                ) {
                    if (
                        !release.infoHash.empty() &&
                        existing.infoHash ==
                            release.infoHash
                    ) {
                        alreadyPresent = true;
                        break;
                    }

                    if (
                        release.infoHash.empty() &&
                        existing.magnet ==
                            release.magnet &&
                        existing.title ==
                            release.title
                    ) {
                        alreadyPresent = true;
                        break;
                    }
                }

                if (!alreadyPresent) {
                    liveGame.releases.push_back(
                        release);

                    addedAny = true;
                }
            }

            for (
                const auto& pair :
                    liveStatuses
            ) {
                debridStatuses[
                    pair.first] =
                        pair.second;
            }

            if (
                addedAny &&
                page ==
                    Page::SearchProgress
            ) {
                page =
                    Page::Torrents;

                status =
                    "Debrid-checked results "
                    "appear as they arrive";
            }
        }

        if (
            pendingLiveSearch.valid() &&
            pendingLiveSearch.wait_for(
                std::chrono::milliseconds(0)) ==
                std::future_status::ready
        ) {
            const bool wasCancelled =
                liveSearchCancel &&
                liveSearchCancel->load();

            auto result =
                pendingLiveSearch.get();

            liveSearchCancel.reset();

            if (
                queuedLiveSearchGameIndex
                    .has_value()
            ) {
                const size_t nextGame =
                    *queuedLiveSearchGameIndex;

                queuedLiveSearchGameIndex.reset();

                status =
                    "Starting new live search...";

                launchLiveSearch(
                    nextGame);
            }
            else if (wasCancelled) {
                status =
                    "Live search stopped";
            }
            else if (
                liveSearchGameIndex <
                    games.size()
            ) {
                if (result.success) {
                    (void)sgb::saveScrapeCache(
                        root,
                        games[
                            liveSearchGameIndex
                        ].title,
                        debridConfig.service,
                        result.releases,
                        result.statuses);
                }

                auto& finishedGame =
                    games[
                        liveSearchGameIndex];

                for (
                    const auto& release :
                        result.releases
                ) {
                    bool alreadyPresent = false;

                    for (
                        const auto& existing :
                            finishedGame.releases
                    ) {
                        if (
                            !release.infoHash.empty() &&
                            existing.infoHash ==
                                release.infoHash
                        ) {
                            alreadyPresent = true;
                            break;
                        }

                        if (
                            release.infoHash.empty() &&
                            existing.magnet ==
                                release.magnet &&
                            existing.title ==
                                release.title
                        ) {
                            alreadyPresent = true;
                            break;
                        }
                    }

                    if (!alreadyPresent) {
                        finishedGame.releases.push_back(
                            release);
                    }
                }

                for (
                    const auto& pair :
                        result.statuses
                ) {
                    debridStatuses[
                        pair.first] =
                            pair.second;
                }

                saveDebridStatuses();

                selectedFiles.clear();
                status = result.message;

                if (
                    !finishedGame.releases.empty()
                ) {
                    page =
                        Page::Torrents;
                }
            }
        }

        if (page == Page::SearchProgress) {
            if (keys & HidNpadButton_B) {
                cancelLiveSearch();
                page = Page::Detail;
            }

        } else if (page == Page::ScrapeChoice) {
            if (keys & HidNpadButton_B) {
                page = Page::Detail;
            }

            if (keys & HidNpadButton_AnyUp) {
                scrapeChoiceCursor =
                    scrapeChoiceCursor == 0
                        ? 1
                        : 0;
            }

            if (keys & HidNpadButton_AnyDown) {
                scrapeChoiceCursor =
                    scrapeChoiceCursor == 0
                        ? 1
                        : 0;
            }

            if (keys & HidNpadButton_A) {
                if (
                    scrapeChoiceGameIndex >=
                    games.size()
                ) {
                    status =
                        "Selected game is no longer available";
                    page = Page::Browse;
                }
                else if (scrapeChoiceCursor == 0) {
                    std::vector<sgb::Release> cached;
                    std::map<
                        std::string,
                        sgb::DebridTorrentStatus
                    > cachedStatuses;
                    std::time_t savedAt = 0;

                    if (
                        !sgb::loadScrapeCache(
                            root,
                            games[
                                scrapeChoiceGameIndex
                            ].title,
                            debridConfig.service,
                            cached,
                            cachedStatuses,
                            &savedAt)
                    ) {
                        status =
                            "No cached scrape available for this game";
                    }
                    else {
                        games[
                            scrapeChoiceGameIndex
                        ].releases =
                            std::move(cached);

                        for (auto& pair : cachedStatuses) {
                            debridStatuses[pair.first] =
                                std::move(pair.second);
                        }

                        torrentCursor = 0;
                        selectedFiles.clear();

                        status =
                            "Loaded cached scrape (" +
                            sgb::scrapeCacheAgeText(
                                savedAt) +
                            ")";

                        page = Page::Torrents;
                    }
                }
                else {
                    startLiveSearch(
                        scrapeChoiceGameIndex);
                }
            }
        } else if (page == Page::Browse) {
            if ((keys & HidNpadButton_Left) && cursor) --cursor;
            if ((keys & HidNpadButton_Right) && cursor+1 < rows.size()) ++cursor;
            if ((keys & HidNpadButton_Up) && cursor >= 5) cursor -= 5;
            if ((keys & HidNpadButton_Down) && cursor+5 < rows.size()) cursor += 5;
            if (keys & HidNpadButton_L) cursor = cursor >= 10 ? cursor-10 : 0;
            if (keys & HidNpadButton_R) cursor = rows.empty() ? 0 : std::min(cursor+10, rows.size()-1);
            if ((keys & HidNpadButton_A) && !rows.empty()) { page = Page::Detail; releaseIndex = torrentCursor = fileCursor = 0; selectedFiles.clear(); }
            if (keys & HidNpadButton_X) {
                optionsCursor = 0;
                page = Page::Options;
            }
            if (keys & HidNpadButton_Y) {
                settingsCursor = 0;
                page = Page::Settings;
            }
        } else if (page == Page::Options) {
            if ((keys & HidNpadButton_Up) && optionsCursor > 0)
                --optionsCursor;

            if ((keys & HidNpadButton_Down) && optionsCursor < 7)
                ++optionsCursor;

            if (keys & HidNpadButton_B) {
                page = Page::Browse;
            }

            if (keys & HidNpadButton_A) {
                if (optionsCursor == 0) {
                    filter.search = keyboard(
                        "Search Switch games",
                        filter.search
                    );
                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 1) {
                    filter.sort = static_cast<sgb::Sort>(
                        (static_cast<int>(filter.sort) + 1) % 3
                    );
                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 2) {
                    std::set<std::string> genres;
                    for (const auto& game : games) {
                        for (const auto& genre : game.genres)
                            genres.insert(genre);
                    }

                    if (filter.genre.empty()) {
                        if (!genres.empty())
                            filter.genre = *genres.begin();
                    }
                    else {
                        auto next = genres.upper_bound(filter.genre);
                        filter.genre =
                            next == genres.end()
                                ? ""
                                : *next;
                    }

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 3) {
                    filter.minRating =
                        filter.minRating == 0 ? 70 :
                        filter.minRating == 70 ? 80 :
                        filter.minRating == 80 ? 90 : 0;

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 4) {
                    filter.minReviews =
                        filter.minReviews == 0 ? 10 :
                        filter.minReviews == 10 ? 50 :
                        filter.minReviews == 50 ? 100 : 0;

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 5) {
                    filter.favouritesOnly =
                        !filter.favouritesOnly;

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 6) {
                    filter = {};
                    cursor = 0;
                    rebuild();
                    status = "Filters reset";
                }
                else {
                    page = Page::Browse;
                }
            }        } else if (page == Page::Settings) {
            if ((keys & HidNpadButton_Up) && settingsCursor > 0)
                --settingsCursor;

            if ((keys & HidNpadButton_Down) && settingsCursor < 5)
                ++settingsCursor;

            if (keys & HidNpadButton_B)
                page = Page::Browse;

            if (keys & HidNpadButton_A) {
                if (settingsCursor == 0) {
                    debridConfig.service =
                        debridConfig.service == sgb::DebridService::TorBox
                            ? sgb::DebridService::AllDebrid
                            : sgb::DebridService::TorBox;

                    debridConfig.apiKey.clear();
                    torBoxDeviceAuth.reset();
                    allDebridPin.reset();
                    debridStatuses.clear();

                    try {
                        atomicWrite(root + "debrid-status.json", "{}");
                    } catch (...) {}

                    saveConfig();
                }

                else if (settingsCursor == 1) {
                    if (debridConfig.service == sgb::DebridService::TorBox) {
                        try {
                            if (!torBoxDeviceAuth.has_value()) {
                                torBoxDeviceAuth =
                                    sgb::beginTorBoxDeviceAuth();

                                status =
                                    "TorBox code: " +
                                    torBoxDeviceAuth->code;
                            }
                            else {
                                auto result =
                                    sgb::checkTorBoxDeviceAuth(
                                        *torBoxDeviceAuth
                                    );

                                if (
                                    result.activated &&
                                    !result.apiKey.empty()
                                ) {
                                    debridConfig.apiKey =
                                        result.apiKey;

                                    debridStatuses.clear();

                                    try {
                                        atomicWrite(
                                            root +
                                            "debrid-status.json",
                                            "{}"
                                        );
                                    } catch (...) {}

                                    saveConfig();

                                    torBoxDeviceAuth.reset();

                                    status =
                                        "TorBox authorized";
                                }
                                else {
                                    status =
                                        result.message.empty()
                                            ? "Waiting for TorBox approval"
                                            : result.message;
                                }
                            }
                        }
                        catch (const std::exception& e) {
                            status = e.what();
                            torBoxDeviceAuth.reset();
                        }
                    }

                    else if (
                        debridConfig.service ==
                        sgb::DebridService::AllDebrid
                    ) {
                        try {
                            if (!allDebridPin.has_value()) {
                                allDebridPin =
                                    sgb::beginAllDebridPinAuth();

                                status =
                                    "Enter PIN " +
                                    allDebridPin->pin +
                                    " at alldebrid.com/pin";
                            } else {
                                auto result =
                                    sgb::checkAllDebridPinAuth(
                                        *allDebridPin
                                    );

                                if (result.activated &&
                                    !result.apiKey.empty()) {

                                    debridConfig.apiKey =
                                        result.apiKey;

                                    debridStatuses.clear();

                                    try {
                                        atomicWrite(
                                            root +
                                            "debrid-status.json",
                                            "{}"
                                        );
                                    } catch (...) {}

                                    saveConfig();
                                    allDebridPin.reset();
                                    status =
                                        "AllDebrid authorized";
                                } else {
                                    allDebridPin->expiresIn =
                                        result.expiresIn;

                                    status =
                                        "Waiting for PIN " +
                                        allDebridPin->pin;
                                }
                            }
                        } catch (const std::exception& e) {
                            status = e.what();
                        }
                    }
                }

                else if (settingsCursor == 2) {
                    if (pending.valid()) {
                        status = "Refresh already running";
                    }
                    else if (url.empty()) {
                        status = "Configure indexUrl first";
                    }
                    else {
                        status = "Refreshing catalog index...";

                        auto current = games;

                        pending = std::async(
                            std::launch::async,
                            [url, current]() {
                                return refreshCatalog(
                                    url,
                                    current
                                );
                            }
                        );
                    }
                }
                else if (settingsCursor == 3) {
                    providerCursor = 0;
                    page = Page::Providers;
                }
                else if (settingsCursor == 4) {
                    if (pendingLangegen.valid()) {
                        status =
                            "Langegen catalog download already running";
                    }
                    else {
                        

                        if (langegenUrl.empty()) {
                            status =
                                "Langegen catalog URL not configured";
                        }
                        else {
                            status =
                                "Downloading Langegen catalog...";

                            const std::string sourceUrl =
                                langegenUrl;

                            pendingLangegen = std::async(
                                std::launch::async,
                                [sourceUrl]() {
                                    return downloadLangegenCatalog(
                                        sourceUrl
                                    );
                                }
                            );
                        }
                    }
                }
                else {
                    debridManagerRows.clear();
                    debridManagerCursor = 0;
                    debridManagerFileCursor = 0;
                    debridManagerSelectedFiles.clear();
                    page = Page::DebridManager;
                    status = "Loading debrid manager...";
                    startDebridManagerRefresh();
                }
            }

        } else if (page == Page::DebridManager) {
            if (keys & HidNpadButton_B) {
                settingsCursor = 5;
                page = Page::Settings;
            }

            if (!debridManagerRows.empty()) {
                if (
                    (keys & HidNpadButton_Up) &&
                    debridManagerCursor > 0
                ) {
                    --debridManagerCursor;
                }

                if (
                    (keys & HidNpadButton_Down) &&
                    debridManagerCursor + 1 <
                        debridManagerRows.size()
                ) {
                    ++debridManagerCursor;
                }

                if (keys & HidNpadButton_L) {
                    debridManagerCursor =
                        debridManagerCursor >= 8
                            ? debridManagerCursor - 8
                            : 0;
                }

                if (keys & HidNpadButton_R) {
                    debridManagerCursor =
                        std::min(
                            debridManagerCursor + 8,
                            debridManagerRows.size() - 1);
                }

                if (keys & HidNpadButton_A) {
                    const auto& torrent =
                        debridManagerRows[
                            debridManagerCursor];

                    if (!torrent.complete) {
                        status =
                            "Download is not complete";
                    }
                    else {
                        debridManagerSelectedTorrent =
                            debridManagerCursor;

                        debridManagerFileCursor = 0;
                        debridManagerSelectedFiles.clear();

                        page =
                            Page::DebridManagerFiles;
                    }
                }
            }

            if (keys & HidNpadButton_Y) {
                if (pendingDebridManager.valid()) {
                    status =
                        "Debrid refresh already running";
                }
                else {
                    status =
                        "Refreshing debrid manager...";
                    startDebridManagerRefresh();
                }
            }

        } else if (page == Page::DebridManagerFiles) {
            if (
                debridManagerSelectedTorrent >=
                    debridManagerRows.size()
            ) {
                page = Page::DebridManager;
            }
            else {
                const auto& torrent =
                    debridManagerRows[
                        debridManagerSelectedTorrent];

                if (keys & HidNpadButton_B) {
                    debridManagerSelectedFiles.clear();
                    page = Page::DebridManager;
                }

                if (!torrent.complete) {
                    status =
                        "Download is not complete";
                    page = Page::DebridManager;
                }
                else {
                    const auto& files =
                        torrent.files;

                    if (
                        (keys & HidNpadButton_Up) &&
                        debridManagerFileCursor > 0
                    ) {
                        --debridManagerFileCursor;
                    }

                    if (
                        (keys & HidNpadButton_Down) &&
                        debridManagerFileCursor + 1 <
                            files.size()
                    ) {
                        ++debridManagerFileCursor;
                    }

                    if (keys & HidNpadButton_L) {
                        debridManagerFileCursor =
                            debridManagerFileCursor >= 10
                                ? debridManagerFileCursor - 10
                                : 0;
                    }

                    if (
                        (keys & HidNpadButton_R) &&
                        !files.empty()
                    ) {
                        debridManagerFileCursor =
                            std::min(
                                debridManagerFileCursor + 10,
                                files.size() - 1);
                    }

                    if (
                        (keys & HidNpadButton_A) &&
                        !files.empty()
                    ) {
                        if (
                            debridManagerSelectedFiles.count(
                                debridManagerFileCursor)
                        ) {
                            debridManagerSelectedFiles.erase(
                                debridManagerFileCursor);
                        }
                        else {
                            debridManagerSelectedFiles.insert(
                                debridManagerFileCursor);
                        }
                    }

                    if (
                        (keys & HidNpadButton_X) &&
                        !files.empty()
                    ) {
                        if (
                            debridManagerSelectedFiles.size() ==
                                files.size()
                        ) {
                            debridManagerSelectedFiles.clear();
                        }
                        else {
                            debridManagerSelectedFiles.clear();

                            for (
                                size_t i = 0;
                                i < files.size();
                                ++i
                            ) {
                                debridManagerSelectedFiles.insert(i);
                            }
                        }
                    }

                    if (keys & HidNpadButton_Y) {
                        try {
                            queueDebridManagerFiles(
                                torrent);
                        }
                        catch (const std::exception& e) {
                            status = e.what();
                        }
                    }
                }
            }

        } else if (page == Page::Providers) {
            const auto& providers =
                sgb::providerRegistry();

            if (keys & HidNpadButton_B) {
                settingsCursor = 3;
                page = Page::Settings;
            }

            if (!providers.empty()) {
                if (
                    (keys & HidNpadButton_Up) &&
                    providerCursor > 0
                ) {
                    --providerCursor;
                }

                if (
                    (keys & HidNpadButton_Down) &&
                    providerCursor + 1 < providers.size()
                ) {
                    ++providerCursor;
                }

                if (keys & HidNpadButton_L) {
                    providerCursor =
                        providerCursor >= 8
                            ? providerCursor - 8
                            : 0;
                }

                if (keys & HidNpadButton_R) {
                    providerCursor =
                        std::min(
                            providerCursor + 8,
                            providers.size() - 1
                        );
                }

                if (keys & HidNpadButton_A) {
                    const auto& id =
                        providers[providerCursor].id;

                    if (enabledProviders.count(id))
                        enabledProviders.erase(id);
                    else
                        enabledProviders.insert(id);

                    saveConfig();
                }

                if (keys & HidNpadButton_X) {
                    enabledProviders.clear();

                    for (const auto& provider : providers)
                        enabledProviders.insert(provider.id);

                    saveConfig();
                }

                if (keys & HidNpadButton_Y) {
                    enabledProviders.clear();
                    saveConfig();
                }
            }

        } else if (!rows.empty()) {
            const auto& g = games[rows[cursor]];
            if (page == Page::Detail) {
                if (keys & HidNpadButton_B) page = Page::Browse;
                if (keys & HidNpadButton_X) { if (favourites.count(g.id)) favourites.erase(g.id); else favourites.insert(g.id); dirty = true; }
                if (keys & HidNpadButton_A) {
                    openScrapeChoice(rows[cursor]);
                }
            } else if (page == Page::Torrents) {
                if (keys & HidNpadButton_B) {
                    if (
                        pendingLiveSearch.valid() &&
                        liveSearchGameIndex ==
                            rows[cursor]
                    ) {
                        cancelLiveSearch();
                    }

                    page = Page::Detail;
                }

                auto visible =
                    torrentRowsFor(g);

                size_t visiblePos = 0;

                if (!visible.empty()) {
                    auto found =
                        std::find(
                            visible.begin(),
                            visible.end(),
                            torrentCursor);

                    if (found == visible.end()) {
                        torrentCursor =
                            visible.front();
                    }
                    else {
                        visiblePos =
                            static_cast<size_t>(
                                found - visible.begin());
                    }

                    if (
                        (keys & HidNpadButton_Up) &&
                        visiblePos > 0
                    ) {
                        --visiblePos;
                        torrentCursor =
                            visible[visiblePos];
                    }

                    if (
                        (keys & HidNpadButton_Down) &&
                        visiblePos + 1 <
                            visible.size()
                    ) {
                        ++visiblePos;
                        torrentCursor =
                            visible[visiblePos];
                    }

                    if (keys & HidNpadButton_L) {
                        visiblePos =
                            visiblePos >= 8
                                ? visiblePos - 8
                                : 0;

                        torrentCursor =
                            visible[visiblePos];
                    }

                    if (keys & HidNpadButton_R) {
                        visiblePos =
                            std::min(
                                visiblePos + 8,
                                visible.size() - 1);

                        torrentCursor =
                            visible[visiblePos];
                    }

                    if (keys & HidNpadButton_A) {
                        startDebridAdd(
                            g.releases[
                                torrentCursor]);
                    }

                    if (keys & HidNpadButton_X) {
                        releaseIndex =
                            torrentCursor;

                        fileCursor = 0;
                        selectedFiles.clear();
                        page = Page::Files;
                    }
                }

                if (keys & HidNpadButton_Y)
                    startDebridCheck(g);

                if (keys & HidNpadButton_Minus) {
                    torrentTextFilter =
                        keyboard(
                            "Filter torrent results",
                            torrentTextFilter);

                    auto filtered =
                        torrentRowsFor(g);

                    if (!filtered.empty())
                        torrentCursor =
                            filtered.front();
                }
            } else if (page == Page::Files && releaseIndex < g.releases.size()) {
                const auto& release = g.releases[releaseIndex];
                auto files = filesFor(release);
                if (keys & HidNpadButton_B) { page = Page::Torrents; selectedFiles.clear(); }
                if ((keys & HidNpadButton_Up) && fileCursor) --fileCursor;
                if ((keys & HidNpadButton_Down) && fileCursor+1 < files.size()) ++fileCursor;
                if (keys & HidNpadButton_L) fileCursor = fileCursor >= 10 ? fileCursor-10 : 0;
                if (keys & HidNpadButton_R) fileCursor = files.empty() ? 0 : std::min(fileCursor+10, files.size()-1);
                if ((keys & HidNpadButton_A) && !files.empty()) {
                    if (selectedFiles.count(fileCursor)) selectedFiles.erase(fileCursor); else selectedFiles.insert(fileCursor);
                }
                if ((keys & HidNpadButton_X) && !files.empty()) {
                    if (selectedFiles.size() == files.size()) selectedFiles.clear();
                    else { selectedFiles.clear(); for (size_t i = 0; i < files.size(); ++i) selectedFiles.insert(i); }
                }
                if (keys & HidNpadButton_Y) {
                    try { queueSelectedFiles(g, release, files); }
                    catch (const std::exception& e) { status = e.what(); }
                }
            }
        }
        if ((keys & HidNpadButton_B) && page == Page::Browse) rebuild();
        if (dirty) { saveState(); dirty = false; }
        SDL_SetRenderDrawColor(renderer, 0,0,0,255); SDL_RenderClear(renderer);
        label(renderer, big, "SWITCH GAMES", 32,22,850,green);
        label(renderer, small, std::to_string(rows.size()) + " games", 1050,32,200,muted);
        if (page == Page::Options) {
            static const char* optionSortLabels[] = {
                "Title A-Z",
                "Highest rated",
                "Newest"
            };

            label(
                renderer,big,
                "FILTER / SORT OPTIONS",
                32,70,1200,green
            );

            label(
                renderer,small,
                "A Change / Select  |  B Back",
                32,112,1200,muted
            );

            std::vector<std::string> optionRows{
                "Search: " +
                    (filter.search.empty() ? std::string("Any") : filter.search),

                "Sort: " +
                    std::string(optionSortLabels[static_cast<int>(filter.sort)]),

                "Genre: " +
                    (filter.genre.empty() ? std::string("All genres") : filter.genre),

                "Minimum rating: " +
                    (filter.minRating == 0
                        ? std::string("Any")
                        : std::to_string(static_cast<int>(filter.minRating)) + "+"),

                "Minimum votes: " +
                    (filter.minReviews == 0
                        ? std::string("Any")
                        : std::to_string(filter.minReviews) + "+"),

                "Favourites only: " +
                    std::string(filter.favouritesOnly ? "On" : "Off"),

                "Reset filters",
                "Back"
            };

            for (size_t i = 0; i < optionRows.size(); ++i) {
                int y = 150 + static_cast<int>(i) * 61;
                SDL_Rect box{32,y,1216,53};

                rect(
                    renderer,
                    box,
                    SDL_Color{18,18,18,255}
                );

                if (i == optionsCursor) {
                    rect(renderer,box,green,true);
                    rect(
                        renderer,
                        {33,y+1,1214,51},
                        green,
                        true
                    );
                }

                label(
                    renderer,small,
                    optionRows[i],
                    52,y+14,1160,
                    i == optionsCursor ? green : white
                );
            }
        } else if (page == Page::SearchProgress) {
            const std::string gameTitle =
                liveSearchGameIndex < games.size()
                    ? games[liveSearchGameIndex].title
                    : "";

            const size_t providersDone =
                liveSearchProgress.providersDone.load();
            const size_t providersTotal =
                liveSearchProgress.providersTotal.load();
            const size_t candidates =
                liveSearchProgress.candidatesFound.load();
            const size_t debridDone =
                liveSearchProgress.debridChecked.load();
            const size_t debridTotal =
                liveSearchProgress.debridTotal.load();
            const std::string stage =
                liveSearchProgress.stageText();
            const std::string provider =
                liveSearchProgress.providerText();

            label(renderer,big,"LIVE SEARCH",32,72,1200,green);
            label(renderer,big,gameTitle,32,118,1200,white);
            label(renderer,small,"Stage: " + stage,32,175,1200,green);

            label(
                renderer,small,
                "Providers: " + std::to_string(providersDone) +
                " / " + std::to_string(providersTotal) + " complete",
                32,225,1200,white
            );

            label(
                renderer,small,
                "Candidates found: " + std::to_string(candidates),
                32,270,1200,white
            );

            label(
                renderer,small,
                "Debrid checked: " + std::to_string(debridDone) +
                " / " + std::to_string(debridTotal),
                32,315,1200,white
            );

            if (!provider.empty()) {
                label(
                    renderer,small,
                    "Latest provider: " + provider,
                    32,360,1200,muted
                );
            }

            const auto providerDiagnostics =
                sgb::providerDiagnosticsSnapshot();

            const sgb::ProviderDiagnostic*
                currentDiagnostic = nullptr;

            if (!provider.empty()) {
                for (const auto& diagnostic :
                     providerDiagnostics) {
                    if (diagnostic.id == provider) {
                        currentDiagnostic =
                            &diagnostic;
                        break;
                    }
                }
            }

            if (
                !currentDiagnostic &&
                !providerDiagnostics.empty()
            ) {
                currentDiagnostic =
                    &providerDiagnostics.back();
            }

            if (currentDiagnostic) {
                const std::string httpText =
                    currentDiagnostic->httpStatus
                        ? std::to_string(
                            currentDiagnostic->httpStatus
                        )
                        : "-";

                label(
                    renderer,
                    small,
                    "HTTP: " + httpText +
                    " | Requests: " +
                    std::to_string(
                        currentDiagnostic->requests
                    ) +
                    " | Bytes: " +
                    std::to_string(
                        currentDiagnostic->responseBytes
                    ) +
                    " | Candidates: " +
                    std::to_string(
                        currentDiagnostic->candidates
                    ),
                    32,610,1200,white
                );

                std::string finalUrl =
                    currentDiagnostic->finalUrl;

                if (finalUrl.size() > 110) {
                    finalUrl =
                        finalUrl.substr(0,107) +
                        "...";
                }

                if (!finalUrl.empty()) {
                    label(
                        renderer,
                        small,
                        "Final URL: " + finalUrl,
                        32,640,1200,muted
                    );
                }

                if (!currentDiagnostic->error.empty()) {
                    std::string errorText =
                        currentDiagnostic->error;

                    if (errorText.size() > 110) {
                        errorText =
                            errorText.substr(0,107) +
                            "...";
                    }

                    label(
                        renderer,
                        small,
                        "Error: " + errorText,
                        32,670,1200,white
                    );
                }
            }

            size_t percent = 0;

            if (stage == "Searching providers") {
                percent = providersTotal
                    ? (providersDone * 50 / providersTotal)
                    : 0;
            }
            else if (stage == "Checking debrid contents") {
                percent = 50 + (
                    debridTotal
                        ? (debridDone * 50 / debridTotal)
                        : 0
                );
            }
            else if (stage == "Complete") {
                percent = 100;
            }

            if (percent > 100)
                percent = 100;

            SDL_Rect progressBg{32,470,1216,28};
            rect(renderer,progressBg,SDL_Color{18,18,18,255});

            SDL_Rect progressFill{
                32,
                470,
                static_cast<int>(1216 * percent / 100),
                28
            };

            if (progressFill.w > 0)
                rect(renderer,progressFill,green);

            label(
                renderer,small,
                "Progress: " + std::to_string(percent) + "%",
                32,515,1200,white
            );

            label(
                renderer,small,
                "Each result appears as soon as its debrid check finishes.",
                32,570,1200,muted
            );
        } else if (page == Page::Providers) {
            const auto& providers =
                sgb::providerRegistry();

            const std::string providerHeader =
                "SCRAPE PROVIDERS (" +
                std::to_string(providers.size()) +
                " total)";

            const std::string providerControls =
                providers.empty()
                    ? "0 / 0  |  A Toggle  |  L/R Page  |  X All  |  Y None  |  B Back"
                    : std::to_string(providerCursor + 1) +
                        " / " +
                        std::to_string(providers.size()) +
                        "  |  A Toggle  |  L/R Page  |  X All  |  Y None  |  B Back";

            label(
                renderer,big,
                providerHeader,
                32,70,1200,green
            );

            label(
                renderer,small,
                providerControls,
                32,112,1200,muted
            );

            if (providers.empty()) {
                label(
                    renderer,big,
                    "No providers compiled",
                    32,250,1200,muted
                );
            }
            else {
                size_t start =
                    (providerCursor / 8) * 8;

                for (
                    size_t slot = 0;
                    slot < 8 &&
                    start + slot < providers.size();
                    ++slot
                ) {
                    size_t index =
                        start + slot;

                    const auto& provider =
                        providers[index];

                    int y =
                        155 +
                        static_cast<int>(slot) * 62;

                    SDL_Rect box{
                        32,y,1216,54
                    };

                    rect(
                        renderer,
                        box,
                        SDL_Color{18,18,18,255}
                    );

                    if (index == providerCursor)
                        rect(
                            renderer,
                            box,
                            green,
                            true
                        );

                    bool enabled =
                        enabledProviders.count(
                            provider.id
                        ) != 0;

                    std::string text =
                        std::string(
                            enabled
                                ? "[x] "
                                : "[ ] "
                        ) +
                        provider.id;

                    label(
                        renderer,
                        small,
                        text,
                        52,
                        y + 15,
                        1100,
                        enabled
                            ? green
                            : white
                    );
                }
            }

        } else if (page == Page::DebridManager) {
            label(
                renderer,big,
                "DEBRID MANAGER",
                32,70,1200,green
            );

            label(
                renderer,small,
                "A Open Files  |  Y Refresh  |  L/R Page  |  B Back",
                32,112,1200,muted
            );

            if (debridManagerRows.empty()) {
                label(
                    renderer,big,
                    pendingDebridManager.valid()
                        ? "Loading..."
                        : "No torrents in debrid account",
                    32,280,1200,muted
                );
            }
            else {
                size_t start =
                    (debridManagerCursor / 8) * 8;

                for (
                    size_t slot = 0;
                    slot < 8 &&
                    start + slot <
                        debridManagerRows.size();
                    ++slot
                ) {
                    size_t index =
                        start + slot;

                    const auto& torrent =
                        debridManagerRows[index];

                    int y =
                        150 +
                        static_cast<int>(slot) * 62;

                    SDL_Rect box{
                        32,y,1216,54
                    };

                    rect(
                        renderer,
                        box,
                        SDL_Color{18,18,18,255}
                    );

                    if (index == debridManagerCursor)
                        rect(
                            renderer,
                            box,
                            green,
                            true
                        );

                    label(
                        renderer,small,
                        torrent.name,
                        48,y+6,1160,
                        white
                    );

                    const std::string downloadText =
                        torrent.complete
                            ? "Download completed"
                            : "Downloading: " +
                                std::to_string(
                                    torrent.progress) +
                                "%";

                    label(
                        renderer,small,
                        downloadText,
                        48,y+30,1160,
                        torrent.complete
                            ? green
                            : muted
                    );
                }
            }

        } else if (page == Page::DebridManagerFiles) {
            label(
                renderer,big,
                "DEBRID FILES",
                32,70,1200,green
            );

            if (
                debridManagerSelectedTorrent <
                    debridManagerRows.size()
            ) {
                const auto& torrent =
                    debridManagerRows[
                        debridManagerSelectedTorrent];

                label(
                    renderer,small,
                    torrent.name,
                    32,112,1200,white
                );

                label(
                    renderer,small,
                    std::to_string(
                        debridManagerSelectedFiles.size()) +
                    " selected  |  A Toggle  |  X Select All  |  Y Queue Install  |  B Back",
                    32,142,1216,muted
                );

                const auto& files =
                    torrent.files;

                size_t start =
                    (debridManagerFileCursor / 9) * 9;

                for (
                    size_t slot = 0;
                    slot < 9 &&
                    start + slot < files.size();
                    ++slot
                ) {
                    size_t index =
                        start + slot;

                    int y =
                        180 +
                        static_cast<int>(slot) * 50;

                    SDL_Rect box{
                        32,y,1216,43
                    };

                    rect(
                        renderer,
                        box,
                        SDL_Color{18,18,18,255}
                    );

                    if (
                        index ==
                        debridManagerFileCursor
                    ) {
                        rect(
                            renderer,
                            box,
                            green,
                            true
                        );
                    }

                    const bool selected =
                        debridManagerSelectedFiles.count(
                            index) != 0;

                    label(
                        renderer,small,
                        std::string(
                            selected
                                ? "[x] "
                                : "[ ] ") +
                            files[index].name,
                        48,y+10,1150,
                        selected
                            ? green
                            : white
                    );
                }

                if (files.empty()) {
                    label(
                        renderer,big,
                        "No files available",
                        32,300,1200,muted
                    );
                }
            }

        } else if (page == Page::Settings) {
            label(
                renderer,big,
                "SETTINGS",
                32,70,1200,green
            );

            label(
                renderer,small,
                "A Select  |  B Back",
                32,112,1200,muted
            );

            SDL_Rect serviceBox{
                32,150,1216,65
            };

            rect(
                renderer,
                serviceBox,
                SDL_Color{18,18,18,255}
            );

            if (settingsCursor == 0)
                rect(
                    renderer,
                    serviceBox,
                    green,
                    true
                );

            label(
                renderer,small,
                "Debrid Service",
                52,170,500
            );

            label(
                renderer,big,
                sgb::debridServiceName(
                    debridConfig.service
                ),
                650,163,550,green
            );


            SDL_Rect authBox{
                32,225,1216,65
            };

            rect(
                renderer,
                authBox,
                SDL_Color{18,18,18,255}
            );

            if (settingsCursor == 1)
                rect(
                    renderer,
                    authBox,
                    green,
                    true
                );

            std::string authLabel =
                "Authorize";

            if (
                debridConfig.service ==
                sgb::DebridService::TorBox
            ) {
                authLabel =
                    torBoxDeviceAuth.has_value()
                        ? "Check TorBox Authorization"
                        : "Authorize TorBox";
            }

            if (
                debridConfig.service ==
                sgb::DebridService::AllDebrid
            ) {
                authLabel =
                    allDebridPin.has_value()
                    ? "Check AllDebrid PIN"
                    : "Authorize with AllDebrid PIN";
            }

            label(
                renderer,small,
                authLabel,
                52,246,780
            );

            label(
                renderer,small,
                debridConfig.apiKey.empty()
                    ? "Not authorized"
                    : "Authorized",
                970,246,230,
                debridConfig.apiKey.empty()
                    ? muted
                    : green
            );


            SDL_Rect catalogBox{
                32,300,1216,65
            };

            rect(
                renderer,
                catalogBox,
                SDL_Color{18,18,18,255}
            );

            if (settingsCursor == 2)
                rect(
                    renderer,
                    catalogBox,
                    green,
                    true
                );

            label(
                renderer,small,
                "Refresh Catalog Index",
                52,321,1100
            );


            SDL_Rect torrentBox{
                32,375,1216,65
            };

            rect(
                renderer,
                torrentBox,
                SDL_Color{18,18,18,255}
            );

            if (settingsCursor == 3)
                rect(
                    renderer,
                    torrentBox,
                    green,
                    true
                );

            label(
                renderer,small,
                "Scrape Providers",
                52,396,1100
            );
            SDL_Rect langegenBox{
                32,450,1216,65
            };

            rect(
                renderer,
                langegenBox,
                SDL_Color{18,18,18,255}
            );

            if (settingsCursor == 4)
                rect(
                    renderer,
                    langegenBox,
                    green,
                    true
                );

            label(
                renderer,small,
                "Download / Update Langegen Catalog",
                52,471,760
            );

            {
                std::ifstream catalogFile(
                    root + "langegen.json",
                    std::ios::binary
                );

                label(
                    renderer,
                    small,
                    catalogFile.good()
                        ? "Installed"
                        : "Not downloaded",
                    990,
                    471,
                    220,
                    catalogFile.good()
                        ? green
                        : muted
                );
            }

            SDL_Rect backBox{
                32,525,1216,65
            };

            rect(
                renderer,
                backBox,
                SDL_Color{18,18,18,255}
            );

            if (settingsCursor == 5)
                rect(
                    renderer,
                    backBox,
                    green,
                    true
                );

            label(
                renderer,small,
                "Debrid Manager",
                52,546,1100
            );


            if (torBoxDeviceAuth.has_value()) {
                label(
                    renderer,
                    big,
                    "TorBox Code: " +
                        torBoxDeviceAuth->code,
                    32,
                    600,
                    700,
                    green
                );

                label(
                    renderer,
                    small,
                    "Visit: " +
                        torBoxDeviceAuth
                            ->friendlyVerificationUrl,
                    32,
                    638,
                    1216,
                    white
                );
            }
            else if (allDebridPin.has_value()) {
                label(
                    renderer,big,
                    "PIN: " + allDebridPin->pin,
                    32,605,500,green
                );

                label(
                    renderer,small,
                    "Enter PIN at alldebrid.com/pin, then select Authorize again.",
                    32,635,1216,white
                );
            }
            else {
                label(
                    renderer,small,
                    "Catalog and torrent indexes are cached separately on the SD card.",
                    32,610,1216,muted
                );
            }
            int refreshPhase =
                catalogRefreshPhase.load();

            if (refreshPhase == 1) {
                label(
                    renderer,
                    small,
                    "Catalog: downloading metadata...",
                    32,650,1216,green
                );
            }
            else if (refreshPhase == 2) {
                size_t done =
                    catalogCoverDone.load();

                size_t total =
                    catalogCoverTotal.load();

                size_t cached =
                    catalogCoverCached.load();

                size_t percent =
                    total ? (done * 100 / total) : 0;

                std::string progress =
                    "Cover packs: " +
                    std::to_string(done) +
                    "/" +
                    std::to_string(total) +
                    " (" +
                    std::to_string(percent) +
                    "%)  cached: " +
                    std::to_string(cached);

                label(
                    renderer,
                    small,
                    progress,
                    32,650,1216,green
                );
            }

        } else if (page == Page::ScrapeChoice) {
            // SCRAPE OPTIONS RENDERER - correct main render chain.

            const std::string gameTitle =
                scrapeChoiceGameIndex < games.size()
                    ? games[scrapeChoiceGameIndex].title
                    : "";

            label(
                renderer,
                big,
                "SCRAPE OPTIONS",
                32,
                72,
                1200,
                green
            );

            label(
                renderer,
                big,
                gameTitle,
                32,
                118,
                1200,
                white
            );

            label(
                renderer,
                small,
                "Debrid: " +
                    sgb::debridServiceName(
                        debridConfig.service
                    ),
                32,
                165,
                1200,
                muted
            );

            SDL_Rect cachedRow{
                32,
                215,
                1216,
                105
            };

            SDL_Rect newRow{
                32,
                345,
                1216,
                105
            };

            rect(
                renderer,
                cachedRow,
                SDL_Color{18,18,18,255}
            );

            rect(
                renderer,
                newRow,
                SDL_Color{18,18,18,255}
            );

            if (scrapeChoiceCursor == 0) {
                rect(
                    renderer,
                    cachedRow,
                    green,
                    true
                );
            }

            if (scrapeChoiceCursor == 1) {
                rect(
                    renderer,
                    newRow,
                    green,
                    true
                );
            }

            label(
                renderer,
                big,
                "Cached Scrape",
                52,
                230,
                1140,
                scrapeChoiceCursor == 0
                    ? green
                    : white
            );

            if (scrapeChoiceHasCache) {
                label(
                    renderer,
                    small,
                    std::to_string(
                        scrapeChoiceReleaseCount
                    ) +
                    " torrent(s)  |  " +
                    sgb::scrapeCacheAgeText(
                        scrapeChoiceSavedAt
                    ) +
                    "  |  availability cached",
                    52,
                    278,
                    1140,
                    muted
                );
            }
            else {
                label(
                    renderer,
                    small,
                    "No cached scrape for this debrid service",
                    52,
                    278,
                    1140,
                    muted
                );
            }

            label(
                renderer,
                big,
                "New Scrape",
                52,
                360,
                1140,
                scrapeChoiceCursor == 1
                    ? green
                    : white
            );

            label(
                renderer,
                small,
                "Search providers and refresh debrid availability",
                52,
                408,
                1140,
                muted
            );

            label(
                renderer,
                small,
                "Up/Down Select  |  A Confirm  |  B Back",
                32,
                620,
                1216,
                green
            );
        } else if (page == Page::Detail && !rows.empty()) {
            const auto& g = games[rows[cursor]];
            label(renderer,big,g.title,32,85,1200);
            label(renderer,small,score(g) + "  " + g.ratingSource,32,140,1200,green);
            label(renderer,small,"Released: " + (g.date.empty() ? "Unknown" : g.date) + "  Torrents: " + std::to_string(g.releases.size()),32,178,1200,muted);
            label(renderer,small,g.ratingUrl,32,215,1200,muted);
            SDL_RenderSetClipRect(renderer,nullptr);
            SDL_Rect summaryBox{32,260,1200,250}; SDL_RenderSetClipRect(renderer,&summaryBox);
            label(renderer,small,g.summary.empty() ? "No description available" : g.summary,32,260,1200,white,true);
            SDL_RenderSetClipRect(renderer,nullptr);
            label(renderer,small,"Debrid: " + sgb::debridServiceName(debridConfig.service),32,535,1200,muted);
            label(renderer,small,favourites.count(g.id) ? "Saved to favourites" : "X: Add to favourites",32,575,1200,green);
            label(renderer,small,"A Scrape | X Favourite | B Back",32,620,1200);
        } else if (page == Page::Torrents && !rows.empty()) {
            const auto& g =
                games[rows[cursor]];

            label(
                renderer,big,
                g.title,
                32,78,1200
            );

            label(
                renderer,small,
                "SEARCH RESULTS  |  A Download / Add to Debrid  |  X View Files  |  - Filter  |  Y Refresh  |  B Back",
                32,122,1200,muted
            );

            auto visible =
                torrentRowsFor(g);

            if (!visible.empty()) {
                auto found =
                    std::find(
                        visible.begin(),
                        visible.end(),
                        torrentCursor);

                size_t visiblePos = 0;

                if (found == visible.end()) {
                    torrentCursor =
                        visible.front();
                }
                else {
                    visiblePos =
                        static_cast<size_t>(
                            found - visible.begin());
                }

                size_t startRow =
                    (visiblePos / 7) * 7;

                for (
                    size_t slot = 0;
                    slot < 7 &&
                    startRow + slot <
                        visible.size();
                    ++slot
                ) {
                    size_t index =
                        visible[
                            startRow + slot];

                    const auto& rel =
                        g.releases[index];

                    int y =
                        158 +
                        static_cast<int>(slot) * 70;

                    SDL_Rect box{
                        32,y,1216,62
                    };

                    rect(
                        renderer,
                        box,
                        SDL_Color{18,18,18,255}
                    );

                    if (index == torrentCursor) {
                        rect(
                            renderer,
                            box,
                            green,
                            true
                        );

                        rect(
                            renderer,
                            {33,y+1,1214,60},
                            green,
                            true
                        );
                    }

                    label(
                        renderer,small,
                        rel.title,
                        48,y+7,1150
                    );

                    auto st =
                        debridStatuses.find(
                            rel.infoHash);

                    std::string state =
                        "Debrid checked";

                    SDL_Color stateColour =
                        muted;

                    if (
                        st !=
                        debridStatuses.end()
                    ) {
                        if (
                            st->second.downloaded &&
                            !st->second.remoteId.empty()
                        ) {
                            auto account =
                                debridAccountByRemoteId.find(
                                    st->second.remoteId);

                            if (
                                account !=
                                    debridAccountByRemoteId.end()
                            ) {
                                if (
                                    account->second.complete
                                ) {
                                    state =
                                        "Download completed";

                                    stateColour =
                                        green;
                                }
                                else {
                                    state =
                                        "Downloading: " +
                                        std::to_string(
                                            account->second.progress) +
                                        "%";
                                }
                            }
                            else {
                                state =
                                    "Added to Debrid";
                            }
                        }
                        else if (st->second.cached) {
                            state =
                                "Debrid checked: Cached";

                            stateColour =
                                green;
                        }
                        else {
                            state =
                                "Debrid checked: Uncached";
                        }
                    }

                    label(
                        renderer,small,
                        rel.source +
                            "  " +
                            rel.size +
                            "  " +
                            state,
                        48,y+34,1150,
                        stateColour
                    );
                }
            }
            else {
                label(
                    renderer,big,
                    torrentTextFilter.empty()
                        ? "No torrents available"
                        : "No torrents match filter",
                    32,300,1200,muted
                );
            }

            if (!torrentTextFilter.empty()) {
                label(
                    renderer,small,
                    "Filter: " +
                        torrentTextFilter,
                    32,650,1200,green
                );
            }
        } else if (page == Page::Files && !rows.empty()) {
            const auto& g = games[rows[cursor]];
            const auto& rel = g.releases[releaseIndex];
            auto files = filesFor(rel);
            label(renderer,big,"FILES",32,75,300,green);
            label(renderer,small,rel.title,190,82,1030);
            label(renderer,small,std::to_string(selectedFiles.size()) + " selected  |  A Toggle  |  X Select All  |  Y Queue Selected  |  B Back",32,120,1216,muted);
            size_t startRow = (fileCursor / 9) * 9;
            for (size_t slot = 0; slot < 9 && startRow + slot < files.size(); ++slot) {
                size_t index = startRow + slot;
                int y = 158 + static_cast<int>(slot) * 53;
                SDL_Rect box{32,y,1216,46};
                rect(renderer,box,SDL_Color{18,18,18,255});
                if (index == fileCursor) { rect(renderer,box,green,true); rect(renderer,{33,y+1,1214,44},green,true); }
                std::string mark = selectedFiles.count(index) ? "[x] " : "[ ] ";
                label(renderer,small,mark + files[index],48,y+11,1150,selectedFiles.count(index) ? green : white);
            }
            if (files.empty()) label(renderer,big,"No file list available for this torrent",32,300,1200,muted);
        } else {
            static const char* sortLabels[] = {"Title A-Z","Highest rated","Newest"};
            label(renderer,small,std::string(sortLabels[static_cast<int>(filter.sort)]) + " | " + (filter.genre.empty() ? "All genres" : filter.genre) + " | Min " + std::to_string(static_cast<int>(filter.minRating)) + " | Votes " + std::to_string(filter.minReviews) + (filter.favouritesOnly ? " | Favourites" : "") + (filter.search.empty() ? "" : " | " + filter.search),32,68,1216,muted);
            size_t start =
                (cursor / 10) * 10;

            for (
                size_t slot = 0;
                slot < 10 &&
                start + slot < rows.size();
                ++slot
            ) {
                const auto& g =
                    games[rows[start + slot]];

                int x =
                    32 +
                    (slot % 5) * 244;

                int y =
                    102 +
                    (slot / 5) * 272;

                SDL_Rect box{
                    x,y,232,262
                };

                rect(
                    renderer,
                    box,
                    SDL_Color{18,18,18,255}
                );

                if (
                    !covers.count(g.id) &&
                    !attempted.count(g.id)
                ) {
                    attempted.insert(g.id);

                    if (
                        SDL_Texture* texture =
                            loadPackedCoverTexture(
                                renderer,
                                g.id
                            )
                    ) {
                        covers[g.id] = texture;
                    }
                }

                auto found =
                    covers.find(g.id);

                if (
                    found != covers.end() &&
                    found->second
                ) {
                    SDL_Rect pic{
                        x + 41,
                        y + 6,
                        150,
                        212
                    };

                    SDL_RenderCopy(
                        renderer,
                        found->second,
                        nullptr,
                        &pic
                    );
                }
                else {
                    rect(
                        renderer,
                        {
                            x + 41,
                            y + 6,
                            150,
                            212
                        },
                        SDL_Color{
                            9,40,12,255
                        }
                    );

                    label(
                        renderer,big,
                        "SWITCH",
                        x + 55,
                        y + 92,
                        125,
                        green
                    );
                }


                label(
                    renderer,small,
                    g.title,
                    x + 10,
                    y + 220,
                    212
                );

                label(
                    renderer,small,
                    score(g),
                    x + 10,
                    y + 240,
                    212,
                    green
                );

                if (
                    start + slot ==
                    cursor
                ) {
                    rect(
                        renderer,
                        box,
                        green,
                        true
                    );

                    rect(
                        renderer,
                        {
                            x + 1,
                            y + 1,
                            230,
                            260
                        },
                        green,
                        true
                    );
                }
            }
            if (rows.empty()) label(renderer,big,games.empty() ? "Press Y to load your GitHub index" : "No games match these filters",32,260,1200,muted);
            label(renderer,small,"X Filters  |  A Details  |  Y Settings  |  + Exit",32,648,1216,muted);

        }
        label(renderer,small,status,32,675,1216,green);
        SDL_RenderPresent(renderer);
    }
    stopping = true;

    if (
        liveSearchCancel &&
        pendingLiveSearch.valid()
    ) {
        liveSearchCancel->store(true);
    }

    if (pending.valid()) pending.wait();
    if (pendingLangegen.valid()) pendingLangegen.wait();
    if (pendingLiveSearch.valid()) pendingLiveSearch.wait(); 
    if (pendingDebridCheck.valid()) pendingDebridCheck.wait();
    if (pendingDebridAdd.valid()) pendingDebridAdd.wait();
    if (pendingDebridManager.valid()) pendingDebridManager.wait();
    saveState(); for (auto& p : covers) SDL_DestroyTexture(p.second);
    TTF_CloseFont(small); TTF_CloseFont(big); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    IMG_Quit(); TTF_Quit(); SDL_Quit(); plExit(); curl_global_cleanup(); romfsExit(); socketExit();
    return 0;
}

