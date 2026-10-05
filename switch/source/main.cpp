#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include "catalog.hpp"
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
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "SwitchGamesBrowser/0.1");
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
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
    sgb::Filter filter; std::string url, status = "Set indexUrl in config.json, then press Y";
    try { games = sgb::parse(sgb::read(root + "switch-index.json")); status = "Loaded cached index"; }
    catch (...) { try { games = sgb::parse(sgb::read(root + "switch-index.json.bak")); status = "Recovered previous cache"; } catch (...) {} }
    try { url = sgb::field(sgb::Json::parse(sgb::read(root + "config.json", 65536)), "indexUrl", 2048); } catch (...) {}
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
    std::vector<size_t> rows = sgb::browse(games, filter, favourites);
    size_t cursor = 0, releaseIndex = 0; bool detail = false, dirty = false;
    std::future<Refresh> pending; std::future<CoverResult> pendingCover;
    std::map<std::string, SDL_Texture*> covers; std::set<std::string> attempted;
    auto rebuild = [&]() { rows = sgb::browse(games, filter, favourites); cursor = std::min(cursor, rows.empty() ? size_t(0) : rows.size()-1); dirty = true; };
    while (appletMainLoop()) {
        SDL_Event event; while (SDL_PollEvent(&event)) {} // libnx handles controller input below.
        padUpdate(&pad); u64 keys = padGetButtonsDown(&pad);
        if (keys & HidNpadButton_Plus) break;
        if (pending.valid() && pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = pending.get(); status = result.message;
            if (!result.games.empty()) { games = std::move(result.games); detail = false; releaseIndex = 0; rebuild(); }
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
        if (!detail) {
            if ((keys & HidNpadButton_Left) && cursor) --cursor;
            if ((keys & HidNpadButton_Right) && cursor+1 < rows.size()) ++cursor;
            if ((keys & HidNpadButton_Up) && cursor >= 4) cursor -= 4;
            if ((keys & HidNpadButton_Down) && cursor+4 < rows.size()) cursor += 4;
            if (keys & HidNpadButton_L) cursor = cursor >= 8 ? cursor-8 : 0;
            if (keys & HidNpadButton_R) cursor = rows.empty() ? 0 : std::min(cursor+8, rows.size()-1);
            if ((keys & HidNpadButton_A) && !rows.empty()) { detail = true; releaseIndex = 0; }
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
        } else if (!rows.empty()) {
            const auto& g = games[rows[cursor]];
            if (keys & HidNpadButton_B) detail = false;
            if (keys & HidNpadButton_X) { if (favourites.count(g.id)) favourites.erase(g.id); else favourites.insert(g.id); dirty = true; }
            if ((keys & HidNpadButton_Left) && releaseIndex) --releaseIndex;
            if ((keys & HidNpadButton_Right) && releaseIndex+1 < g.releases.size()) ++releaseIndex;
            if (keys & HidNpadButton_A) {
                try {
                    auto queue = sgb::Json::array();
                    try { queue = sgb::Json::parse(sgb::read(root + "magnet-queue.json", 4 * 1024 * 1024)); } catch (...) {}
                    if (!queue.is_array()) throw std::runtime_error("Queue format invalid");
                    bool exists = false; const auto& release = g.releases[releaseIndex];
                    for (const auto& entry : queue) if (entry.value("magnet", "") == release.magnet) exists = true;
                    if (!exists) queue.push_back({{"gameId",g.id},{"title",release.title},{"magnet",release.magnet}});
                    atomicWrite(root + "magnet-queue.json", queue.dump(2)); status = "Magnet saved to SD (downloader integration pending)";
                } catch (const std::exception& e) { status = e.what(); }
            }
        }
        if ((keys & HidNpadButton_B) && !detail) rebuild();
        if ((keys & HidNpadButton_Y) && !pending.valid()) {
            if (url.empty()) status = "Configure indexUrl on SD first";
            else { status = "Refreshing index..."; pending = std::async(std::launch::async, refresh, url); }
        }
        if (dirty) { saveState(); dirty = false; }
        SDL_SetRenderDrawColor(renderer, 0,0,0,255); SDL_RenderClear(renderer);
        label(renderer, big, "SWITCH GAMES", 32,22,850,green);
        label(renderer, small, std::to_string(rows.size()) + " games", 1050,32,200,muted);
        if (detail && !rows.empty()) {
            const auto& g = games[rows[cursor]];
            label(renderer,big,g.title,32,85,1200);
            label(renderer,small,score(g) + "  " + g.ratingSource,32,140,1200,green);
            label(renderer,small,"Released: " + (g.date.empty() ? "Unknown" : g.date) + "  Title ID: " + g.titleId,32,178,1200,muted);
            label(renderer,small,g.ratingUrl,32,215,1200,muted);
            SDL_RenderSetClipRect(renderer,nullptr);
            SDL_Rect summaryBox{32,260,1200,180}; SDL_RenderSetClipRect(renderer,&summaryBox);
            label(renderer,small,g.summary.empty() ? "No description available" : g.summary,32,260,1200,white,true);
            SDL_RenderSetClipRect(renderer,nullptr);
            const auto& rel = g.releases[releaseIndex];
            label(renderer,small,"Release " + std::to_string(releaseIndex+1) + "/" + std::to_string(g.releases.size()) + ": " + rel.title,32,470,1200);
            label(renderer,small,"Size: " + rel.size,32,510,1200,muted);
            label(renderer,small,favourites.count(g.id) ? "Saved to favourites" : "X: Add to favourites",32,550,1200,green);
            label(renderer,small,"A Save magnet | Left/Right Release | X Favourite | B Back",32,610,1200);
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
            label(renderer,small,"A Details | X Search | Y Refresh | ZL Sort | ZR Favourites | - Rating",32,580,1216);
            label(renderer,small,"L/R Page | Left stick Genre | Right stick Votes | B Reset | + Exit",32,614,1216,muted);
        }
        label(renderer,small,status,32,675,1216,green);
        SDL_RenderPresent(renderer);
    }
    stopping = true; if (pending.valid()) pending.wait(); if (pendingCover.valid()) pendingCover.wait();
    saveState(); for (auto& p : covers) SDL_DestroyTexture(p.second);
    TTF_CloseFont(small); TTF_CloseFont(big); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    IMG_Quit(); TTF_Quit(); SDL_Quit(); plExit(); curl_global_cleanup(); romfsExit(); socketExit();
    return 0;
}
