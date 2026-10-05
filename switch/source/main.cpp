#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include "catalog.hpp"
#include "debrid.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <sys/stat.h>

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
struct Refresh { std::vector<sgb::Game> games; std::string message; };
static Refresh refresh(const std::string& url) {
    try {
        if (url.rfind("https://raw.githubusercontent.com/", 0) != 0 || url.find('?') != std::string::npos || url.substr(url.find_last_of('/') + 1) != "switch-index.json")
            throw std::runtime_error("Set a raw GitHub switch-index.json URL in config.json");
        auto manifest = sgb::Json::parse(get(url.substr(0, url.find_last_of('/') + 1) + "manifest.json", 64 * 1024));
        if (manifest.at("schemaVersion") != 1) throw std::runtime_error("Unsupported manifest");
        auto bytes = get(url, 48 * 1024 * 1024);
        if (manifest.at("bytes") != bytes.size() || manifest.at("sha256") != digest(bytes)) throw std::runtime_error("Index changed during refresh; try again");
        auto games = sgb::parse(bytes);
        if (manifest.at("gameCount") != games.size()) throw std::runtime_error("Manifest game count mismatch");
        atomicWrite(root + "switch-index.json", bytes);
        return {std::move(games), "Index refreshed"};
    } catch (const std::exception& e) { return {{}, e.what()}; }
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
struct DebridCheckResult { std::map<std::string, sgb::DebridTorrentStatus> rows; std::string message; };
struct DebridAddResult { std::string hash; sgb::DebridTorrentStatus state; std::string message; };
enum class Page { Browse, Detail, Torrents, Files, Settings };

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
    std::string url, status = "Press Y for Settings";
    sgb::DebridConfig debridConfig;
    std::map<std::string, sgb::DebridTorrentStatus> debridStatuses;
    try { games = sgb::parse(sgb::read(root + "switch-index.json")); status = "Loaded cached index"; }
    catch (...) { try { games = sgb::parse(sgb::read(root + "switch-index.json.bak")); status = "Recovered previous cache"; } catch (...) {} }
    try {
        auto config = sgb::Json::parse(
            sgb::read(root + "config.json", 65536)
        );

        url = sgb::field(config, "indexUrl", 2048);

        debridConfig.service = sgb::parseDebridService(
            config.value("debridService", "")
        );

        debridConfig.apiKey =
            config.value("debridApiKey", "");

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

            atomicWrite(
                root + "config.json",
                sgb::Json{
                    {"indexUrl", url},
                    {"debridService", service},
                    {"debridApiKey", debridConfig.apiKey}
                }.dump(2)
            );
            status = "Settings saved";
        } catch (const std::exception& e) {
            status = e.what();
        }
    };
    std::vector<size_t> rows = sgb::browse(games, filter, favourites);
    size_t cursor = 0, releaseIndex = 0, torrentCursor = 0, fileCursor = 0;
    size_t settingsCursor = 0;
    Page page = Page::Browse; bool dirty = false;
    std::set<size_t> selectedFiles;
    std::future<Refresh> pending; std::future<CoverResult> pendingCover;
    std::future<DebridCheckResult> pendingDebridCheck;
    std::future<DebridAddResult> pendingDebridAdd;
    std::map<std::string, SDL_Texture*> covers; std::set<std::string> attempted;
    auto rebuild = [&]() { rows = sgb::browse(games, filter, favourites); cursor = std::min(cursor, rows.empty() ? size_t(0) : rows.size()-1); dirty = true; };
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
    while (appletMainLoop()) {
        SDL_Event event; while (SDL_PollEvent(&event)) {} // libnx handles controller input below.
        padUpdate(&pad); u64 keys = padGetButtonsDown(&pad);
        if (keys & HidNpadButton_Plus) break;
        if (pending.valid() && pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = pending.get(); status = result.message;
            if (!result.games.empty()) { games = std::move(result.games); page = Page::Browse; releaseIndex = torrentCursor = fileCursor = 0; selectedFiles.clear(); rebuild(); }
        }
        if (pendingCover.valid() && pendingCover.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto c = pendingCover.get();
            if (!c.path.empty()) { SDL_Surface* s = IMG_Load(c.path.c_str()); if (s) {
                    SDL_Surface* thumb = SDL_CreateRGBSurfaceWithFormat(0, 288, 120, 32, SDL_PIXELFORMAT_RGBA32);
                    if (thumb) { SDL_BlitScaled(s, nullptr, thumb, nullptr); covers[c.id] = SDL_CreateTextureFromSurface(renderer, thumb); SDL_FreeSurface(thumb); }
                    SDL_FreeSurface(s);
                } }
            // Bound decoded textures; disk cache remains reusable.
            if (covers.size() > 24) { auto i = covers.begin(); SDL_DestroyTexture(i->second); attempted.erase(i->first); covers.erase(i); }
        }
        if (pendingDebridCheck.valid() && pendingDebridCheck.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = pendingDebridCheck.get();
            for (auto& pair : result.rows) debridStatuses[pair.first] = std::move(pair.second);
            saveDebridStatuses();
            status = result.message;
        }
        if (pendingDebridAdd.valid() && pendingDebridAdd.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = pendingDebridAdd.get();
            if (!result.hash.empty()) { debridStatuses[result.hash] = std::move(result.state); saveDebridStatuses(); }
            status = result.message;
        }
        if (page == Page::Browse) {
            if ((keys & HidNpadButton_Left) && cursor) --cursor;
            if ((keys & HidNpadButton_Right) && cursor+1 < rows.size()) ++cursor;
            if ((keys & HidNpadButton_Up) && cursor >= 4) cursor -= 4;
            if ((keys & HidNpadButton_Down) && cursor+4 < rows.size()) cursor += 4;
            if (keys & HidNpadButton_L) cursor = cursor >= 8 ? cursor-8 : 0;
            if (keys & HidNpadButton_R) cursor = rows.empty() ? 0 : std::min(cursor+8, rows.size()-1);
            if ((keys & HidNpadButton_A) && !rows.empty()) { page = Page::Detail; releaseIndex = torrentCursor = fileCursor = 0; selectedFiles.clear(); }
            if (keys & HidNpadButton_X) { filter.search = keyboard("Search Switch games", filter.search); cursor = 0; rebuild(); }
            if (keys & HidNpadButton_B) { filter = {}; cursor = 0; rebuild(); }
            if (keys & HidNpadButton_ZL) { filter.sort = static_cast<sgb::Sort>((static_cast<int>(filter.sort)+1)%3); cursor = 0; rebuild(); }
            if (keys & HidNpadButton_ZR) { filter.favouritesOnly = !filter.favouritesOnly; cursor = 0; rebuild(); }
            if (keys & HidNpadButton_Minus) {
                filter.minRating = filter.minRating == 0 ? 70 : filter.minRating == 70 ? 80 : filter.minRating == 80 ? 90 : 0;
                cursor = 0; rebuild();
            }
            if (keys & HidNpadButton_StickL) {
                std::set<std::string> genres; for (const auto& g : games) for (const auto& genre : g.genres) genres.insert(genre);
                if (filter.genre.empty()) { if (!genres.empty()) filter.genre = *genres.begin(); }
                else { auto next = genres.upper_bound(filter.genre); filter.genre = next == genres.end() ? "" : *next; }
                cursor = 0; rebuild();
            }
            if (keys & HidNpadButton_StickR) { filter.minReviews = filter.minReviews == 0 ? 10 : filter.minReviews == 10 ? 50 : filter.minReviews == 50 ? 100 : 0; cursor = 0; rebuild(); }
            if (keys & HidNpadButton_Y) {
                settingsCursor = 0;
                page = Page::Settings;
            }
        } else if (page == Page::Settings) {
            if ((keys & HidNpadButton_Up) && settingsCursor > 0)
                --settingsCursor;

            if ((keys & HidNpadButton_Down) && settingsCursor < 2)
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
                    debridStatuses.clear();

                    try {
                        atomicWrite(root + "debrid-status.json", "{}");
                    } catch (...) {}

                    saveConfig();
                } else if (settingsCursor == 1) {
                    if (pending.valid()) {
                        status = "Refresh already running";
                    } else if (url.empty()) {
                        status = "Configure indexUrl first";
                    } else {
                        status = "Refreshing index...";
                        pending = std::async(
                            std::launch::async,
                            refresh,
                            url
                        );
                    }
                } else {
                    page = Page::Browse;
                }
            }
        } else if (!rows.empty()) {
            const auto& g = games[rows[cursor]];
            if (page == Page::Detail) {
                if (keys & HidNpadButton_B) page = Page::Browse;
                if (keys & HidNpadButton_X) { if (favourites.count(g.id)) favourites.erase(g.id); else favourites.insert(g.id); dirty = true; }
                if (keys & HidNpadButton_A) {
                    if (g.releases.empty()) status = "No torrents available for this game";
                    else { page = Page::Torrents; torrentCursor = 0; selectedFiles.clear(); startDebridCheck(g); }
                }
            } else if (page == Page::Torrents) {
                if (keys & HidNpadButton_B) page = Page::Detail;
                if ((keys & HidNpadButton_Up) && torrentCursor) --torrentCursor;
                if ((keys & HidNpadButton_Down) && torrentCursor+1 < g.releases.size()) ++torrentCursor;
                if (keys & HidNpadButton_L) torrentCursor = torrentCursor >= 8 ? torrentCursor-8 : 0;
                if (keys & HidNpadButton_R) torrentCursor = g.releases.empty() ? 0 : std::min(torrentCursor+8, g.releases.size()-1);
                if ((keys & HidNpadButton_A) && !g.releases.empty()) startDebridAdd(g.releases[torrentCursor]);
                if (keys & HidNpadButton_Y) startDebridCheck(g);
                if ((keys & HidNpadButton_X) && !g.releases.empty()) {
                    releaseIndex = torrentCursor; fileCursor = 0; selectedFiles.clear(); page = Page::Files;
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
        if (page == Page::Settings) {
            label(renderer,big,"SETTINGS",32,85,1200,green);
            label(renderer,small,"A Select  |  B Back",32,130,1200,muted);

            SDL_Rect serviceBox{32,180,1216,80};
            rect(renderer,serviceBox,SDL_Color{18,18,18,255});
            if (settingsCursor == 0) rect(renderer,serviceBox,green,true);
            label(renderer,small,"Debrid Service",52,194,500);
            label(renderer,big,sgb::debridServiceName(debridConfig.service),650,190,550,green);

            SDL_Rect refreshBox{32,280,1216,80};
            rect(renderer,refreshBox,SDL_Color{18,18,18,255});
            if (settingsCursor == 1) rect(renderer,refreshBox,green,true);
            label(renderer,small,"Refresh Cached Index",52,305,1100);

            SDL_Rect backBox{32,380,1216,80};
            rect(renderer,backBox,SDL_Color{18,18,18,255});
            if (settingsCursor == 2) rect(renderer,backBox,green,true);
            label(renderer,small,"Back",52,405,1100);

            label(
                renderer,
                small,
                "Changing debrid service clears the old service credentials/status cache.",
                32,500,1216,muted
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
            label(renderer,small,"A Torrents | X Favourite | B Back",32,620,1200);
        } else if (page == Page::Torrents && !rows.empty()) {
            const auto& g = games[rows[cursor]];
            label(renderer,big,g.title,32,78,1200);
            label(renderer,small,"TORRENTS  |  A Add to Debrid  |  X View Files  |  Y Refresh Status  |  B Back",32,122,1200,muted);
            size_t startRow = (torrentCursor / 7) * 7;
            for (size_t slot = 0; slot < 7 && startRow + slot < g.releases.size(); ++slot) {
                size_t index = startRow + slot;
                const auto& rel = g.releases[index];
                int y = 158 + static_cast<int>(slot) * 70;
                SDL_Rect box{32,y,1216,62};
                rect(renderer,box,SDL_Color{18,18,18,255});
                if (index == torrentCursor) { rect(renderer,box,green,true); rect(renderer,{33,y+1,1214,60},green,true); }
                label(renderer,small,rel.title,48,y+7,1150);
                auto st = debridStatuses.find(rel.infoHash);
                std::string state = "Cached: Unknown  Downloaded: Unknown";
                SDL_Color stateColour = muted;
                if (st != debridStatuses.end()) {
                    state = std::string("Cached: ") + (st->second.cached ? "True" : "False") +
                        "  Downloaded: " + (st->second.downloaded ? "True" : "False");
                    if (st->second.cached) stateColour = green;
                }
                label(renderer,small,rel.source + "  " + rel.size + "  " + state,48,y+34,1150,stateColour);
            }
            if (g.releases.empty()) label(renderer,big,"No torrents available",32,300,1200,muted);
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
            size_t start = (cursor / 8) * 8;
            for (size_t slot = 0; slot < 8 && start+slot < rows.size(); ++slot) {
                const auto& g = games[rows[start+slot]];
                int x = 32 + (slot%4)*308, y = 112 + (slot/4)*228;
                SDL_Rect box{x,y,292,210}; rect(renderer,box,SDL_Color{18,18,18,255});
                auto found = covers.find(g.id);
                if (found != covers.end() && found->second) { SDL_Rect pic{x+2,y+2,288,120}; SDL_RenderCopy(renderer,found->second,nullptr,&pic); }
                else { rect(renderer,{x+2,y+2,288,120},SDL_Color{9,40,12,255}); label(renderer,big,"SWITCH",x+18,y+43,250,green); }
                if (!pendingCover.valid() && !attempted.count(g.id) && !g.cover.empty()) {
                    attempted.insert(g.id); std::string path = root + "covers/" + digest(g.cover) + ".img";
                    std::string coverUrl = g.cover, id = g.id;
                    pendingCover = std::async(std::launch::async,[path,coverUrl,id]() -> CoverResult {
                        try { std::ifstream cached(path); bool exists = cached.good(); cached.close(); if (!exists) atomicWrite(path,get(coverUrl,4*1024*1024)); return {id,path}; } catch (...) { return {id,""}; }
                    });
                }
                label(renderer,small,g.title,x+12,y+133,268);
                label(renderer,small,score(g),x+12,y+166,268,green);
                if (start+slot == cursor) { rect(renderer,box,green,true); rect(renderer,{x+1,y+1,290,208},green,true); }
            }
            if (rows.empty()) label(renderer,big,games.empty() ? "Press Y to load your GitHub index" : "No games match these filters",32,260,1200,muted);
            label(renderer,small,"A Details | X Search | Y Settings | ZL Sort | ZR Favourites | - Rating",32,580,1216);
            label(renderer,small,"L/R Page | Left stick Genre | Right stick Votes | B Reset | + Exit",32,614,1216,muted);
        }
        label(renderer,small,status,32,675,1216,green);
        SDL_RenderPresent(renderer);
    }
    stopping = true; if (pending.valid()) pending.wait(); if (pendingCover.valid()) pendingCover.wait();
    if (pendingDebridCheck.valid()) pendingDebridCheck.wait();
    if (pendingDebridAdd.valid()) pendingDebridAdd.wait();
    saveState(); for (auto& p : covers) SDL_DestroyTexture(p.second);
    TTF_CloseFont(small); TTF_CloseFont(big); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    IMG_Quit(); TTF_Quit(); SDL_Quit(); plExit(); curl_global_cleanup(); romfsExit(); socketExit();
    return 0;
}
