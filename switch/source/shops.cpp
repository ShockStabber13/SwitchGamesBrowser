#include "shops.hpp"
#include "json.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <cstring>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sgb {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxIndexBytes = 8 * 1024 * 1024;
constexpr const char* kNotUltraNxCatalog =
    "http://127.0.0.1:8080/cyberfoil/base-games";
constexpr const char* kNotUltraNxDownload =
    "http://127.0.0.1:8080/raw?u=";
constexpr std::size_t kMaxMatches = 400;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
bool https(const std::string& s) {
    return s.rfind("https://", 0) == 0 && s.size() < 4096 &&
        s.find('\r') == std::string::npos && s.find('\n') == std::string::npos;
}
std::string stripFragment(std::string url) {
    auto n = url.find('#');
    if (n != std::string::npos) url.erase(n);
    return url;
}
std::string stripQuery(std::string url) {
    url = stripFragment(std::move(url));
    auto n = url.find('?');
    if (n != std::string::npos) url.erase(n);
    return url;
}
std::string urlResolve(const std::string& base, const std::string& path) {
    if (https(path)) return path;
    if (path.empty() || path[0] == '#' ||
        path.find("://") != std::string::npos ||
        path.rfind("//", 0) == 0 ||
        path.find('\n') != std::string::npos)
        return "";
    const auto start = base.find("://");
    if (start == std::string::npos) return "";
    const auto hostStart = start + 3;
    const auto hostEnd = base.find('/', hostStart);
    const std::string origin = hostEnd == std::string::npos
        ? base : base.substr(0, hostEnd);
    if (path[0] == '/') return origin + path;
    const std::string clean = stripQuery(base);
    const auto lastSlash = clean.rfind('/');
    return (lastSlash == std::string::npos || lastSlash < hostStart)
        ? origin + "/" + path : clean.substr(0, lastSlash + 1) + path;
}
std::string displayName(const std::string& link) {
    auto hash = link.find('#');
    if (hash != std::string::npos && hash + 1 < link.size())
        return link.substr(hash + 1, 240);
    const std::string path = stripQuery(link);
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string shopName(const std::string& link) {
    const auto scheme = link.find("://");
    const auto start = scheme == std::string::npos ? 0 : scheme + 3;
    const auto end = link.find_first_of("/?#", start);
    return link.substr(start, end == std::string::npos
        ? std::string::npos : end - start);
}
struct Body { std::string text; };
size_t appendBody(char* ptr, size_t size, size_t count, void* ctx) {
    auto* b = static_cast<Body*>(ctx);
    if (size && count > SIZE_MAX / size) return 0;
    const std::size_t n = size * count;
    if (b->text.size() > kMaxIndexBytes ||
        n > kMaxIndexBytes - b->text.size()) return 0;
    b->text.append(ptr, n);
    return n;
}
int requestProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancelled = static_cast<std::atomic<bool>*>(context);
    return cancelled && cancelled->load() ? 1 : 0;
}
std::string fetch(const std::string& url, const std::atomic<bool>* cancel) {
    const bool localRelay = (url == kNotUltraNxCatalog);
    if (!localRelay && !https(url))
        throw std::runtime_error("Shop index requires HTTPS");
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Shop connection unavailable");
    Body body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchGamesBrowser/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, localRelay ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, requestProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     localRelay ? CURLPROTO_HTTP : CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     localRelay ? CURLPROTO_HTTP : CURLPROTO_HTTPS);
#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    const auto code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status < 200 || status >= 300) {
        if (localRelay) {
            if (status == 503 || status == 502)
                throw std::runtime_error("NotUltraNX relay needs DBI priming / catalog");
            throw std::runtime_error("NotUltraNX relay unavailable (HTTP " +
                                     std::to_string(status) + ")");
        }
        throw std::runtime_error("Shop index unavailable or requires authentication");
    }
    return std::move(body.text);
}
std::string stringField(const Json& object, const char* key) {
    if (!object.is_object()) return "";
    auto it = object.find(key);
    return it == object.end() || !it->is_string()
        ? "" : it->get<std::string>();
}
std::uint64_t sizeField(const Json& object) {
    if (!object.is_object()) return 0;
    auto it = object.find("size");
    if (it == object.end()) return 0;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>();
    if (it->is_number_integer()) {
        auto n = it->get<std::int64_t>();
        return n > 0 ? static_cast<std::uint64_t>(n) : 0;
    }
    return 0;
}
// Canonicalize titles for search: the catalog contains symbols like
// Mario Kart™ 8 and Crash™ Team Racing that IGDB titles omit.
// Ignore punctuation, whitespace and UTF-8 symbol bytes consistently.
std::string comparable(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (unsigned char byte : text) {
        if (byte >= 'A' && byte <= 'Z')
            result.push_back(static_cast<char>(byte + ('a' - 'A')));
        else if ((byte >= 'a' && byte <= 'z') ||
                 (byte >= '0' && byte <= '9'))
            result.push_back(static_cast<char>(byte));
    }
    return result;
}

// Matches MoviesAndSeries Api.kt isLikePattern() behavior:
// put '*' between title words, then match each normalized part in order.
// '*' represents any number of characters, not the regex token ".*".
std::string wildcardPattern(const std::string& title) {
    std::string pattern = "*";
    bool hasText = false;
    for (unsigned char c : title) {
        if (std::isspace(c)) {
            if (pattern.back() != '*') pattern.push_back('*');
        } else if (c >= 'A' && c <= 'Z') {
            pattern.push_back(static_cast<char>(c - 'A' + 'a'));
            hasText = true;
        } else if ((c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9')) {
            pattern.push_back(static_cast<char>(c));
            hasText = true;
        }
    }
    if (!hasText) return "";
    if (pattern.back() != '*') pattern.push_back('*');
    return pattern;
}

bool wildcardMatches(const std::string& text, const std::string& pattern) {
    if (pattern.empty()) return false;
    std::size_t current = 0;
    std::size_t start = 0;
    while (start < pattern.size()) {
        const auto end = pattern.find('*', start);
        const std::string part = pattern.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (!part.empty()) {
            const auto index = text.find(part, current);
            if (index == std::string::npos) return false;
            current = index + part.size();
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
struct Directory { std::string url; std::size_t depth = 0; };
std::string entryLink(const Json& obj) {
    return obj.is_string() ? obj.get<std::string>() : stringField(obj, "url");
}
void addUnique(std::vector<ShopEntry>& entries,
               std::set<std::string>& seen,
               const ShopEntry& item) {
    if (entries.size() < kMaxMatches && seen.insert(item.url).second)
        entries.push_back(item);
}
} // namespace

bool isShopPackage(const std::string& name) {
    const std::string value = lower(name);
    for (const char* ext : {".nsp", ".nsz", ".xci", ".xcz"}) {
        const std::size_t length = std::char_traits<char>::length(ext);
        if (value.size() >= length &&
            value.compare(value.size() - length, length, ext) == 0)
            return true;
    }
    return false;
}

ShopSearchResult searchOpenNxShops(
    const std::string& title, ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel)
{
    ShopSearchResult result;
    progress.running.store(true);
    progress.shopsDone.store(0);
    progress.shopsTotal.store(0);
    { std::lock_guard<std::mutex> lock(progress.mutex);
      progress.matches.clear(); progress.message = "Loading OpenNX shops"; }
    try {
        const auto root = Json::parse(fetch(
            "https://opennx.github.io/tinfoil.json", cancel.get()),
            nullptr, false);
        if (!root.is_object() || !root.contains("directories") ||
            !root["directories"].is_array())
            throw std::runtime_error("OpenNX shop directory format not supported");

        std::vector<std::string> shops;
        std::set<std::string> unique;
        for (const auto& row : root["directories"]) {
            const std::string link = entryLink(row);
            if (https(link) && unique.insert(link).second && shops.size() < 64)
                shops.push_back(link);
        }
        progress.shopsTotal.store(shops.size());
        if (shops.empty()) throw std::runtime_error("OpenNX listed no HTTPS shops");

        const std::string search = comparable(title);
        std::atomic<std::size_t> nextShop{0};
        std::atomic<std::size_t> unavailable{0};
        auto worker = [&]() {
            while (!cancel->load()) {
                const auto index = nextShop.fetch_add(1);
                if (index >= shops.size()) break;
                const std::string rootUrl = shops[index];
                const std::string origin = shopName(rootUrl);
                std::deque<Directory> queue;
                queue.push_back({rootUrl, 0});
                std::set<std::string> visited;
                std::set<std::string> foundLinks;
                std::vector<ShopEntry> matches;
                bool parsedAtLeastOne = false;
                std::size_t pages = 0;
                // One shop cannot monopolize the search on a Switch.
                while (!queue.empty() && !cancel->load() && pages < 8) {
                    const auto current = std::move(queue.front());
                    queue.pop_front();
                    const auto indexUrl = stripFragment(current.url);
                    if (!https(indexUrl) || !visited.insert(indexUrl).second)
                        continue;
                    ++pages;
                    try {
                        const auto json = Json::parse(fetch(indexUrl, cancel.get()),
                                                     nullptr, false);
                        if (!json.is_object() || (!json.contains("files") &&
                                                 !json.contains("directories")))
                            continue;
                        parsedAtLeastOne = true;
                        if (json.contains("files") && json["files"].is_array()) {
                            for (const auto& row : json["files"]) {
                                const std::string link = urlResolve(
                                    indexUrl, entryLink(row));
                                if (!https(link)) continue;
                                std::string name = stringField(row, "name");
                                if (name.empty()) name = stringField(row, "title");
                                if (name.empty()) name = displayName(link);
                                if (!isShopPackage(name)) continue;
                                if (comparable(name).find(search) == std::string::npos)
                                    continue;
                                addUnique(matches, foundLinks,
                                    {name, link, origin, sizeField(row)});
                            }
                        }
                        if (current.depth < 2 &&
                            json.contains("directories") &&
                            json["directories"].is_array()) {
                            for (const auto& row : json["directories"]) {
                                const std::string link = urlResolve(
                                    indexUrl, entryLink(row));
                                if (https(link) && queue.size() < 80)
                                    queue.push_back({link, current.depth + 1});
                            }
                        }
                    } catch (...) {
                        // A private or offline shop should not abort others.
                    }
                }
                if (!parsedAtLeastOne) unavailable.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(progress.mutex);
                    for (const auto& item : matches) {
                        if (progress.matches.size() >= kMaxMatches) break;
                        progress.matches.push_back(item);
                    }
                    progress.message = "Checked " +
                        std::to_string(progress.shopsDone.load() + 1) + "/" +
                        std::to_string(shops.size()) + " shops";
                }
                progress.shopsDone.fetch_add(1);
            }
        };
        std::vector<std::thread> workers;
        constexpr std::size_t kConcurrentShops = 3;
        try {
            for (std::size_t i = 0; i < std::min(kConcurrentShops, shops.size()); ++i)
                workers.emplace_back(worker);
        } catch (...) {
            cancel->store(true);
            for (auto& workerThread : workers)
                if (workerThread.joinable()) workerThread.join();
            throw;
        }
        for (auto& workerThread : workers) workerThread.join();
        if (cancel->load()) throw std::runtime_error("Shop search cancelled");
        {
            std::lock_guard<std::mutex> lock(progress.mutex);
            result.matches = progress.matches;
        }
        result.success = true;
        result.message = "Found " + std::to_string(result.matches.size()) +
            " direct package(s); " + std::to_string(unavailable.load()) +
            " shop(s) unavailable or require login";
    } catch (const std::exception& e) {
        result.message = e.what();
    }
    { std::lock_guard<std::mutex> lock(progress.mutex);
      progress.message = result.message; }
    progress.running.store(false);
    return result;
}


namespace {

// A deliberately small HTML scanner: the site's catalog and its public
// download buttons are ordinary anchors, so no JSON/API catalog is needed.
struct WebsiteAnchor {
    std::string href;
    std::string caption;
};
std::string htmlDecode(std::string value) {
    for (const auto& mapping : {
        std::pair<const char*, const char*>{"&amp;", "&"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
        {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}
    }) {
        std::size_t p = 0;
        while ((p = value.find(mapping.first, p)) != std::string::npos) {
            value.replace(p, std::strlen(mapping.first), mapping.second);
            p += std::strlen(mapping.second);
        }
    }
    return value;
}
std::string htmlText(const std::string& markup) {
    std::string text;
    bool tag = false;
    for (char c : markup) {
        if (c == '<') tag = true;
        else if (c == '>') { tag = false; text.push_back(' '); }
        else if (!tag) text.push_back(c);
    }
    return htmlDecode(std::move(text));
}
std::vector<WebsiteAnchor> websiteAnchors(const std::string& markup) {
    std::vector<WebsiteAnchor> links;
    std::size_t p = 0;
    while ((p = markup.find("<a", p)) != std::string::npos) {
        const auto tagEnd = markup.find('>', p + 2);
        if (tagEnd == std::string::npos) break;
        const auto end = markup.find("</a>", tagEnd + 1);
        if (end == std::string::npos) break;
        const auto tag = markup.substr(p, tagEnd - p);
        const auto key = tag.find("href");
        std::string href;
        if (key != std::string::npos) {
            auto eq = tag.find('=', key + 4);
            if (eq != std::string::npos) {
                ++eq;
                while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq])))
                    ++eq;
                if (eq < tag.size() && (tag[eq] == '\'' || tag[eq] == '"')) {
                    const char quote = tag[eq++];
                    const auto last = tag.find(quote, eq);
                    if (last != std::string::npos)
                        href = htmlDecode(tag.substr(eq, last - eq));
                }
            }
        }
        if (!href.empty()) {
            WebsiteAnchor link;
            link.href = std::move(href);
            // Game cards occasionally put titles beside a linked image.
            // This short context also covers nested <span> title markup.
            const auto after = std::min(markup.size(), end + 5 + 260);
            link.caption = htmlText(markup.substr(
                tagEnd + 1, after - (tagEnd + 1)));
            links.push_back(std::move(link));
        }
        p = end + 4;
    }
    return links;
}
std::string websiteQueryEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : value) {
        if (std::isalnum(c) && c < 128) output.push_back(static_cast<char>(c));
        else if (c == '-' || c == '_' || c == '.') output.push_back(c);
        else {
            output.push_back('%');
            output.push_back(hex[c >> 4]);
            output.push_back(hex[c & 15]);
        }
    }
    return output;
}
bool validTitleId(const std::string& id) {
    if (id.size() != 16) return false;
    for (unsigned char c : id)
        if (!std::isxdigit(c)) return false;
    return true;
}
std::string anchorGameId(const std::string& href) {
    auto p = href.find("/game/");
    if (p == std::string::npos) return "";
    p += 6;
    const auto id = href.substr(p, 16);
    return validTitleId(id) ? id : "";
}
bool websiteTitleMatches(const std::string& title, const std::string& html) {
    const auto beginTag = html.find("<h1");
    const auto begin = beginTag == std::string::npos
        ? std::string::npos : html.find('>', beginTag);
    const auto end = begin == std::string::npos
        ? std::string::npos : html.find("</h1>", begin);
    if (end == std::string::npos) return false;
    const auto actual = comparable(htmlText(
        html.substr(begin + 1, end - begin - 1)));
    const auto expected = comparable(title);
    return !actual.empty() && !expected.empty() &&
        (actual == expected ||
         (expected.size() >= 8 &&
          actual.find(expected) != std::string::npos &&
          expected.size() * 5 >= actual.size() * 4));
}

} // namespace

ShopSearchResult searchNotUltraNxWebsite(
    const std::string& title,
    const std::string& titleId,
    ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel)
{
    ShopSearchResult result;
    progress.running.store(true);
    progress.shopsDone.store(0);
    progress.shopsTotal.store(1);
    {
        std::lock_guard<std::mutex> guard(progress.mutex);
        progress.matches.clear();
        progress.message = "Finding game on NotUltraNX website";
    }
    try {
        if (!cancel || cancel->load())
            throw std::runtime_error("Shop search cancelled");
        std::string id = validTitleId(titleId) ? titleId : "";
        if (id.empty()) {
            // IGDB's catalog does not always carry a Nintendo title ID.
            // Scan the website's paginated HTML directory without using
            // NotUltraNX's catalog API. Four fetchers keep this bounded.
            // Try the site's own HTML search before paginated browsing.
            // Its public pagination links use ?p= and ?s= parameters.
            constexpr std::size_t maxPages = 48;
            const std::string searchUrl = "https://not.ultranx.ru/en?s=" +
                websiteQueryEncode(title);
            progress.shopsTotal.store(maxPages);
            std::atomic<std::size_t> nextPage{1};
            std::atomic<std::size_t> finished{0};
            std::atomic<bool> found{false};
            std::mutex foundMutex;
            const std::string normalized = comparable(title);
            auto scan = [&]() {
                while (!found.load() && !cancel->load()) {
                    const std::size_t page = nextPage.fetch_add(1);
                    if (page > maxPages) break;
                    try {
                        const std::string listing = fetch(
                            page == 1 ? searchUrl :
                            "https://not.ultranx.ru/en?p=" +
                                std::to_string(page - 1), cancel.get());
                        std::set<std::string> candidates;
                        const auto pattern = wildcardPattern(title);
                        for (const auto& link : websiteAnchors(listing)) {
                            const std::string candidate = anchorGameId(link.href);
                            const auto caption = comparable(link.caption);
                            if (!candidate.empty() && normalized.size() >= 3 &&
                                (caption.find(normalized) != std::string::npos ||
                                 wildcardMatches(caption, pattern)))
                                candidates.insert(candidate);
                        }
                        // Some card templates use a clickable container
                        // instead of an <a>. Match the game path in those
                        // HTML attributes too, then verify its h1 title.
                        std::size_t pos = 0;
                        while ((pos = listing.find("/game/", pos)) != std::string::npos) {
                            const auto candidate = anchorGameId(listing.substr(pos, 22));
                            const auto start = pos > 400 ? pos - 400 : 0;
                            const auto end = std::min(listing.size(), pos + 600);
                            const auto surrounding = comparable(htmlText(
                                listing.substr(start, end - start)));
                            if (!candidate.empty() && normalized.size() >= 3 &&
                                (surrounding.find(normalized) != std::string::npos ||
                                 wildcardMatches(surrounding, pattern)))
                                candidates.insert(candidate);
                            pos += 6;
                        }
                        for (const auto& candidate : candidates) {
                            if (found.load() || cancel->load()) break;
                            try {
                                const auto gamePage = fetch(
                                    "https://not.ultranx.ru/en/game/" + candidate,
                                    cancel.get());
                                if (!websiteTitleMatches(title, gamePage)) continue;
                                std::lock_guard<std::mutex> lock(foundMutex);
                                if (!found.exchange(true)) id = candidate;
                                break;
                            } catch (...) {}
                        }
                    } catch (...) {
                        // A failed page should not hide the rest of the site.
                    }
                    const auto count = finished.fetch_add(1) + 1;
                    progress.shopsDone.store(count);
                    std::lock_guard<std::mutex> guard(progress.mutex);
                    progress.message = "Checked " + std::to_string(count) +
                        "/" + std::to_string(maxPages) + " website pages";
                }
            };
            std::vector<std::thread> workers;
            for (int i = 0; i < 4; ++i) workers.emplace_back(scan);
            for (auto& worker : workers) worker.join();
        }
        if (cancel->load())
            throw std::runtime_error("Shop search cancelled");
        if (id.empty())
            throw std::runtime_error(
                "Game not found on NotUltraNX website (try a title with an ID)");

        const auto html = fetch(
            "https://not.ultranx.ru/en/game/" + id, cancel.get());
        // Never present a different game's packages after a name search.
        if (!validTitleId(titleId) && !websiteTitleMatches(title, html))
            throw std::runtime_error(
                "NotUltraNX result title does not match selected game");
        const auto links = websiteAnchors(html);
        std::set<std::string> seen;
        std::vector<ShopEntry> matches;
        for (const auto& a : links) {
            const auto url = urlResolve(
                "https://not.ultranx.ru/en/game/" + id, a.href);
            // Follow only links embedded on the selected game's real
            // HTML page, to the official API redirect host. Do not use the
            // DBI/CyberFoil catalog or local relay.
            if (url.rfind("https://api.ultranx.ru/", 0) != 0 ||
                url.find("/download/" + id + "/") == std::string::npos)
                continue;
            const auto path = stripQuery(url);
            std::string name;
            if (path.size() >= 5 && path.compare(path.size()-5, 5, "/base") == 0)
                name = title + " [BASE].nsz";
            else if (path.size() >= 7 &&
                     path.compare(path.size()-7, 7, "/update") == 0)
                name = title + " [UPDATE].nsz";
            else if (path.size() >= 5 &&
                     path.compare(path.size()-5, 5, "/dlcs") == 0)
                name = title + " [DLCs].zip";
            else
                continue;
            if (seen.insert(url).second)
                matches.push_back({name, url, "NotUltraNX Website", 0});
        }
        if (matches.empty())
            throw std::runtime_error(
                "Website game found but no Base/Update/DLC download buttons");
        {
            std::lock_guard<std::mutex> guard(progress.mutex);
            progress.matches = matches;
        }
        result.matches = std::move(matches);
        result.success = true;
        result.message = "NotUltraNX website: " +
            std::to_string(result.matches.size()) +
            " Base/Update/DLC download option(s)";
    } catch (const std::exception& e) {
        result.message = e.what();
    }
    progress.shopsDone.store(progress.shopsTotal.load());
    {
        std::lock_guard<std::mutex> guard(progress.mutex);
        progress.message = result.message;
    }
    progress.running.store(false);
    return result;
}

ShopSearchResult searchNotUltraNxRelay(
    const std::string& title, ShopSearchProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel)
{
    ShopSearchResult result;
    progress.running.store(true);
    progress.shopsDone.store(0);
    progress.shopsTotal.store(1);
    {
        std::lock_guard<std::mutex> guard(progress.mutex);
        progress.matches.clear();
        progress.message = "Reading NotUltraNX relay catalog...";
    }
    try {
        if (!cancel || cancel->load())
            throw std::runtime_error("Shop search cancelled");

        const Json catalog = Json::parse(fetch(kNotUltraNxCatalog, cancel.get()),
                                        nullptr, false);
        if (!catalog.is_object() || !catalog.contains("files") ||
            !catalog["files"].is_array()) {
            throw std::runtime_error("NotUltraNX relay returned invalid catalog JSON");
        }

        const std::string query = wildcardPattern(title);
        std::set<std::string> seen;
        std::vector<ShopEntry> matches;
        for (const auto& row : catalog["files"]) {
            if (cancel->load())
                throw std::runtime_error("Shop search cancelled");
            if (!row.is_object()) continue;
            const std::string name = stringField(row, "name");
            const std::string link = stringField(row, "url");
            if (!isShopPackage(name) ||
                link.rfind(kNotUltraNxDownload, 0) != 0 ||
                link.find('\r') != std::string::npos ||
                link.find('\n') != std::string::npos ||
                !wildcardMatches(comparable(name), query))
                continue;
            addUnique(matches, seen, {name, link, "NotUltraNX", sizeField(row)});
        }

        {
            std::lock_guard<std::mutex> guard(progress.mutex);
            progress.matches = matches;
        }
        result.matches = std::move(matches);
        result.message = "NotUltraNX: " +
            std::to_string(result.matches.size()) +
            " matching file(s) from " +
            std::to_string(catalog["files"].size()) + " base games";
        result.success = true;
    } catch (const std::exception& e) {
        result.message = e.what();
    }
    progress.shopsDone.store(1);
    {
        std::lock_guard<std::mutex> guard(progress.mutex);
        progress.message = result.message;
    }
    progress.running.store(false);
    return result;
}
} // namespace sgb
