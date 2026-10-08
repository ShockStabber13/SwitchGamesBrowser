#include "debrid.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace sgb {

thread_local const std::atomic<bool>*
    gDebridCancelFlag = nullptr;

void setDebridCancelFlag(
    const std::atomic<bool>* flag)
{
    gDebridCancelFlag = flag;
}

namespace {

using Json = nlohmann::json;

int debridProgressCallback(
    void*,
    curl_off_t,
    curl_off_t,
    curl_off_t,
    curl_off_t)
{
    return (
        gDebridCancelFlag &&
        gDebridCancelFlag->load()
    ) ? 1 : 0;
}

struct Buffer {
    std::string bytes;
    size_t limit = 0;
    long retryAfter = 0;
};

struct Response {
    long status = 0;
    std::string body;
    long retryAfter = 0;
};

size_t writeBody(char* data, size_t size, size_t count, void* opaque) {
    auto* b = static_cast<Buffer*>(opaque);
    if (size && count > SIZE_MAX / size) return 0;
    const size_t n = size * count;
    if (n > b->limit - b->bytes.size()) return 0;
    b->bytes.append(data, n);
    return n;
}

size_t readHeader(char* data, size_t size, size_t count, void* opaque) {
    auto* b = static_cast<Buffer*>(opaque);
    const size_t n = size * count;
    std::string line(data, n), lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.rfind("retry-after:", 0) == 0) {
        try { b->retryAfter = std::stol(line.substr(12)); } catch (...) {}
    }
    return n;
}

std::string lowerHash(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string valueString(const Json& object, const char* key) {
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}

std::string encode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("URL encoding failed");
    char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!escaped) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("URL encoding failed");
    }
    std::string out(escaped);
    curl_free(escaped);
    curl_easy_cleanup(curl);
    return out;
}

Response request(
    const std::string& url,
    bool post,
    const std::vector<std::string>& headers,
    const std::string& body = ""
) {
    if (url.rfind("https://", 0) != 0) throw std::runtime_error("Debrid endpoint must use HTTPS");
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Debrid network initialization failed");

    Buffer buffer{{}, 16 * 1024 * 1024, 0};
    curl_slist* headerList = nullptr;
    for (const auto& h : headers) headerList = curl_slist_append(headerList, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchGamesBrowser/0.4");
    #if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, debridProgressCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, readHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buffer);
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    if (post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const auto rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        if (
            rc == CURLE_ABORTED_BY_CALLBACK &&
            gDebridCancelFlag &&
            gDebridCancelFlag->load()
        ) {
            throw std::runtime_error(
                "Search cancelled");
        }

        throw std::runtime_error(
            "Debrid network request failed");
    }

    return {
        status,
        std::move(buffer.bytes),
        buffer.retryAfter
    };
}

Response requestRetry(
    const std::string& url,
    bool post,
    const std::vector<std::string>& headers,
    const std::string& body = ""
) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto response = request(url, post, headers, body);
        if (response.status != 429 && response.status != 503) return response;
        const long waitSeconds =
            response.retryAfter > 0
                ? response.retryAfter
                : std::min<long>(
                    8,
                    1L << attempt);

        for (
            long tick = 0;
            tick < waitSeconds * 10;
            ++tick
        ) {
            if (
                gDebridCancelFlag &&
                gDebridCancelFlag->load()
            ) {
                throw std::runtime_error(
                    "Search cancelled");
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));
        }
    }
    throw std::runtime_error("Debrid service rate limit");
}

Json jsonResponse(const Response& response, const char* label) {
    if (response.status < 200 || response.status >= 300)
        throw std::runtime_error(std::string(label) + " HTTP " + std::to_string(response.status));
    try { return Json::parse(response.body); }
    catch (...) { throw std::runtime_error(std::string(label) + " returned invalid JSON"); }
}

void appendTorBoxFiles(const Json& files, std::vector<DebridFile>& out) {
    if (!files.is_array()) return;
    for (const auto& f : files) {
        if (f.is_string()) {
            out.push_back({f.get<std::string>(), "", false});
            continue;
        }
        if (!f.is_object()) continue;
        std::string name = valueString(f, "name");
        if (name.empty()) name = valueString(f, "path");
        std::string id = valueString(f, "file_id");
        if (id.empty()) id = valueString(f, "id");
        if (!name.empty()) {
            std::uint64_t size = 0;
            auto sizeIt = f.find("size");

            if (
                sizeIt != f.end() &&
                sizeIt->is_number_unsigned()
            ) {
                size = sizeIt->get<std::uint64_t>();
            }
            else if (
                sizeIt != f.end() &&
                sizeIt->is_number_integer()
            ) {
                const auto raw =
                    sizeIt->get<long long>();

                if (raw > 0)
                    size =
                        static_cast<std::uint64_t>(
                            raw);
            }

            out.push_back({
                name,
                id,
                false,
                "",
                size
            });
        }
    }
}

void appendAllDebridFiles(
    const Json& files,
    std::vector<DebridFile>& out,
    const std::string& prefix = "")
{
    if (!files.is_array())
        return;

    for (const auto& node : files) {
        if (!node.is_object())
            continue;

        const std::string name =
            valueString(node, "n");

        auto children =
            node.find("e");

        if (
            children != node.end() &&
            children->is_array()
        ) {
            appendAllDebridFiles(
                *children,
                out,
                prefix +
                    (
                        name.empty()
                            ? ""
                            : name + "/"
                    )
            );

            continue;
        }

        if (name.empty())
            continue;

        std::uint64_t size = 0;

        auto sizeIt =
            node.find("s");

        if (
            sizeIt != node.end() &&
            sizeIt->is_number_unsigned()
        ) {
            size =
                sizeIt->get<std::uint64_t>();
        }
        else if (
            sizeIt != node.end() &&
            sizeIt->is_number_integer()
        ) {
            const auto raw =
                sizeIt->get<long long>();

            if (raw > 0)
                size =
                    static_cast<std::uint64_t>(
                        raw);
        }

        out.push_back({
            prefix + name,
            "",
            false,
            valueString(node, "l"),
            size
        });
    }
}

class TorBoxBackend final : public DebridBackend {
public:
    explicit TorBoxBackend(DebridConfig config) : config_(std::move(config)) {}

    std::map<std::string, DebridTorrentStatus> check(const std::vector<DebridCandidate>& candidates) override {
        std::map<std::string, DebridTorrentStatus> out;

        // Account lookup is useful for marking torrents already added,
        // but it must not prevent the actual cache check from running.
        std::map<std::string, DebridTorrentStatus> account;

        try {
            account =
                accountTorrentMap();
        }
        catch (...) {
            account.clear();
        }

        std::vector<std::string> hashes;
        std::set<std::string> seen;
        for (const auto& candidate : candidates) {
            const std::string hash = lowerHash(candidate.infoHash);
            if (hash.size() == 40 && seen.insert(hash).second) hashes.push_back(hash);
        }

        for (const auto& hash : hashes)
            out[hash] = {};

        for (size_t offset = 0; offset < hashes.size(); offset += 10) {
            const size_t end = std::min(offset + 10, hashes.size());
            std::string joined;
            for (size_t i = offset; i < end; ++i) {
                if (!joined.empty()) joined += ",";
                joined += hashes[i];
            }

            auto root = jsonResponse(requestRetry(
                "https://api.torbox.app/v1/api/torrents/checkcached?hash=" + encode(joined) + "&format=list&list_files=true",
                false,
                authHeaders()
            ), "TorBox checkcached");

            auto data = root.find("data");
            if (data == root.end()) continue;
            if (data->is_array()) {
                for (const auto& item : *data) parseCachedItem(item, "", account, out);
            } else if (data->is_object()) {
                for (auto it = data->begin(); it != data->end(); ++it) parseCachedItem(it.value(), it.key(), account, out);
            }
        }

        for (const auto& hash : hashes) {
            auto accountIt =
                account.find(hash);

            if (accountIt == account.end())
                continue;

            auto& state =
                out[hash];

            state.downloaded = true;

            if (state.remoteId.empty()) {
                state.remoteId =
                    accountIt->second.remoteId;
            }

            if (state.files.empty()) {
                state.files =
                    accountIt->second.files;
            }

            state.cached =
                state.cached ||
                accountIt->second.cached;
        }

        return out;
    }

    DebridTorrentStatus add(const std::string& infoHash, const std::string& magnet) override {
        const std::string hash = lowerHash(infoHash);
        auto account = accountTorrentMap();
        auto existing = account.find(hash);
        if (existing != account.end()) {
            auto state = existing->second;
            state.downloaded = true;
            if (state.files.empty() && !state.remoteId.empty()) state.files = files(state);
            return state;
        }

        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("TorBox network initialization failed");
        Buffer buffer{{}, 8 * 1024 * 1024, 0};
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + config_.apiKey).c_str());
        curl_mime* mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "magnet");
        curl_mime_data(part, magnet.c_str(), CURL_ZERO_TERMINATED);

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.torbox.app/v1/api/torrents/createtorrent");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchGamesBrowser/0.4");
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#ifdef __SWITCH__
        curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
        auto rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK || status < 200 || status >= 300)
            throw std::runtime_error("TorBox create torrent failed");

        Json root = Json::parse(buffer.bytes);
        auto data = root.find("data");
        std::string id;
        if (data != root.end() && data->is_object()) {
            id = valueString(*data, "torrent_id");
            if (id.empty()) id = valueString(*data, "id");
        } else if (data != root.end() && data->is_array() && !data->empty() && (*data)[0].is_object()) {
            id = valueString((*data)[0], "torrent_id");
            if (id.empty()) id = valueString((*data)[0], "id");
        }
        if (id.empty()) throw std::runtime_error("TorBox create torrent returned no ID");

        DebridTorrentStatus state;
        state.downloaded = true;
        state.remoteId = id;
        for (int i = 0; i < 5 && state.files.empty(); ++i) {
            try { state.files = files(state); } catch (...) {}
            if (state.files.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        state.cached = !state.files.empty();
        return state;
    }

    std::vector<DebridFile> files(const DebridTorrentStatus& torrent) override {
        if (torrent.remoteId.empty()) return {};
        auto root = jsonResponse(requestRetry(
            "https://api.torbox.app/v1/api/torrents/mylist?id=" + encode(torrent.remoteId) + "&bypass_cache=true",
            false,
            authHeaders()
        ), "TorBox torrent details");

        auto data = root.find("data");
        if (data == root.end()) return {};
        Json item;
        if (data->is_object()) item = *data;
        else if (data->is_array() && !data->empty() && (*data)[0].is_object()) item = (*data)[0];
        else return {};

        std::vector<DebridFile> out;
        auto f = item.find("files");
        if (f != item.end()) appendTorBoxFiles(*f, out);
        return out;
    }

    std::string downloadUrl(
        const std::string& remoteId,
        const DebridFile& file
    ) override {
        if (remoteId.empty())
            throw std::runtime_error(
                "TorBox torrent ID is missing");

        if (file.id.empty())
            throw std::runtime_error(
                "TorBox file ID is missing");

        return
            "https://api.torbox.app/v1/api/torrents/requestdl"
            "?token=" + encode(config_.apiKey) +
            "&torrent_id=" + encode(remoteId) +
            "&file_id=" + encode(file.id) +
            "&redirect=true";
    }

    void remove(
        const std::string& remoteId
    ) override {
        if (remoteId.empty())
            throw std::runtime_error(
                "TorBox torrent ID is missing");

        Json body = {
            {"operation", "delete"},
            {"all", false}
        };

        const bool numeric =
            std::all_of(
                remoteId.begin(),
                remoteId.end(),
                [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                });

        if (numeric) {
            try {
                body["torrent_id"] =
                    std::stoll(remoteId);
            }
            catch (...) {
                body["torrent_id"] =
                    remoteId;
            }
        }
        else {
            body["torrent_id"] =
                remoteId;
        }

        auto root = jsonResponse(
            requestRetry(
                "https://api.torbox.app/v1/api/torrents/controltorrent",
                true,
                {
                    "Authorization: Bearer " +
                        config_.apiKey,
                    "Content-Type: application/json"
                },
                body.dump()
            ),
            "TorBox delete");

        if (!root.value("success", false)) {
            throw std::runtime_error(
                root.value(
                    "detail",
                    std::string(
                        "TorBox delete failed")));
        }
    }


    std::vector<DebridAccountTorrent> accountTorrents() override {
        std::vector<DebridAccountTorrent> out;

        auto root = jsonResponse(requestRetry(
            "https://api.torbox.app/v1/api/torrents/mylist?bypass_cache=true&limit=1000",
            false,
            authHeaders()
        ), "TorBox mylist");

        auto data = root.find("data");
        if (data == root.end())
            return out;

        Json rows = Json::array();

        if (data->is_array()) {
            rows = *data;
        }
        else if (data->is_object()) {
            auto torrents = data->find("torrents");

            if (
                torrents != data->end() &&
                torrents->is_array()
            ) {
                rows = *torrents;
            }
            else {
                rows.push_back(*data);
            }
        }

        for (const auto& item : rows) {
            if (!item.is_object())
                continue;

            DebridAccountTorrent row;

            row.name = valueString(item, "name");
            row.infoHash = lowerHash(
                valueString(item, "hash"));

            if (row.infoHash.empty()) {
                row.infoHash = lowerHash(
                    valueString(item, "info_hash"));
            }

            row.remoteId = valueString(item, "id");

            if (row.remoteId.empty()) {
                row.remoteId =
                    valueString(item, "torrent_id");
            }

            auto finished =
                item.find("download_finished");

            if (
                finished != item.end() &&
                finished->is_boolean()
            ) {
                row.complete =
                    finished->get<bool>();
            }

            auto cached =
                item.find("cached");

            if (
                !row.complete &&
                cached != item.end() &&
                cached->is_boolean() &&
                cached->get<bool>()
            ) {
                row.complete = true;
            }

            double progress = 0.0;

            auto progressIt =
                item.find("progress");

            if (
                progressIt != item.end() &&
                progressIt->is_number()
            ) {
                progress =
                    progressIt->get<double>();

                if (
                    progress > 0.0 &&
                    progress <= 1.0
                ) {
                    progress *= 100.0;
                }
            }

            if (row.complete)
                progress = 100.0;

            row.progress =
                std::clamp(
                    static_cast<int>(
                        progress + 0.5),
                    0,
                    100);

            if (row.complete) {
                auto filesIt =
                    item.find("files");

                if (filesIt != item.end()) {
                    appendTorBoxFiles(
                        *filesIt,
                        row.files);
                }
            }

            if (row.name.empty()) {
                row.name =
                    !row.infoHash.empty()
                        ? row.infoHash
                        : row.remoteId;
            }

            out.push_back(
                std::move(row));
        }

        return out;
    }

private:
    DebridConfig config_;

    std::vector<std::string> authHeaders() const {
        return {"Authorization: Bearer " + config_.apiKey};
    }

    std::map<std::string, DebridTorrentStatus> accountTorrentMap() const {
        std::map<std::string, DebridTorrentStatus> out;
        auto root = jsonResponse(requestRetry(
            "https://api.torbox.app/v1/api/torrents/mylist?bypass_cache=true&limit=1000",
            false,
            authHeaders()
        ), "TorBox mylist");
        auto data = root.find("data");
        if (data == root.end()) return out;

        Json rows = Json::array();
        if (data->is_array()) rows = *data;
        else if (data->is_object()) {
            auto torrents = data->find("torrents");
            if (torrents != data->end() && torrents->is_array()) rows = *torrents;
            else rows.push_back(*data);
        }

        for (const auto& item : rows) {
            if (!item.is_object()) continue;
            std::string hash = lowerHash(valueString(item, "hash"));
            if (hash.empty()) hash = lowerHash(valueString(item, "info_hash"));
            if (hash.empty()) continue;
            DebridTorrentStatus state;
            state.downloaded = true;
            state.remoteId = valueString(item, "id");
            if (state.remoteId.empty()) state.remoteId = valueString(item, "torrent_id");
            std::string downloadState = valueString(item, "download_state");
            if (downloadState.empty()) downloadState = valueString(item, "state");
            state.cached = lowerHash(downloadState) == "cached";
            auto filesIt = item.find("files");
            if (filesIt != item.end()) appendTorBoxFiles(*filesIt, state.files);
            if (!state.files.empty()) state.cached = true;
            out[hash] = std::move(state);
        }
        return out;
    }

    static void parseCachedItem(
        const Json& item,
        const std::string& fallbackHash,
        const std::map<std::string, DebridTorrentStatus>& account,
        std::map<std::string, DebridTorrentStatus>& out
    ) {
        if (!item.is_object()) return;
        std::string hash = lowerHash(valueString(item, "hash"));
        if (hash.empty()) hash = lowerHash(valueString(item, "info_hash"));
        if (hash.empty()) hash = lowerHash(fallbackHash);
        if (hash.empty()) return;

        DebridTorrentStatus state;
        state.cached = true;
        auto accountIt = account.find(hash);
        if (accountIt != account.end()) {
            state.downloaded = true;
            state.remoteId = accountIt->second.remoteId;
            state.files = accountIt->second.files;
        }
        auto filesIt = item.find("files");
        if (state.files.empty() && filesIt != item.end()) appendTorBoxFiles(*filesIt, state.files);
        out[hash] = std::move(state);
    }
};

std::mutex gAllDebridCheckMutex;

class AllDebridBackend final : public DebridBackend {
public:
    explicit AllDebridBackend(DebridConfig config) : config_(std::move(config)) {}

    std::map<std::string, DebridTorrentStatus> check(const std::vector<DebridCandidate>& candidates) override {
        // Live Search can run several provider workers at once. AllDebrid
        // cache probing works by temporarily uploading magnets, so concurrent
        // checks can collectively exceed the account's active-magnet limit.
        // Serialize the complete probe lifecycle: status -> upload -> inspect
        // -> delete, then let the next provider through.
        std::unique_lock<std::mutex>
            checkLock(gAllDebridCheckMutex);

        std::map<std::string, DebridTorrentStatus> out;
        const auto existingIds = existingMagnetIds();

        for (const auto& candidate : candidates) {
            const std::string hash =
                lowerHash(candidate.infoHash);

            if (hash.size() == 40)
                out[hash] = {};
        }

        constexpr size_t checkBatchSize = 5;

        for (
            size_t offset = 0;
            offset < candidates.size();
            offset += checkBatchSize
        ) {
            const size_t end =
                std::min(
                    offset + checkBatchSize,
                    candidates.size());
            std::string body;
            for (size_t i = offset; i < end; ++i) {
                if (!body.empty()) body += "&";
                body += "magnets%5B%5D=" + encode(candidates[i].magnet);
            }

            auto root = apiPost("https://api.alldebrid.com/v4/magnet/upload", body, "AllDebrid upload");
            auto data = root.find("data");
            if (data == root.end() || !data->is_object()) continue;
            auto magnets = data->find("magnets");
            if (magnets == data->end() || !magnets->is_array()) continue;

            for (const auto& item : *magnets) {
                if (!item.is_object())
                    continue;

                if (
                    item.contains("error") &&
                    !item["error"].is_null()
                ) {
                    continue;
                }

                const std::string hash =
                    lowerHash(
                        valueString(
                            item,
                            "hash"));

                const std::string id =
                    valueString(
                        item,
                        "id");

                if (
                    hash.empty() ||
                    id.empty()
                ) {
                    continue;
                }

                const bool existedBefore =
                    existingIds.count(id) != 0;

                DebridTorrentStatus state;
                state.cached =
                    item.value(
                        "ready",
                        false);

                state.downloaded =
                    existedBefore;

                state.remoteId =
                    existedBefore
                        ? id
                        : "";

                if (state.cached) {
                    auto trees =
                        filesByIds({id});

                    auto found =
                        trees.find(id);

                    if (
                        found != trees.end()
                    ) {
                        state.files =
                            std::move(
                                found->second);
                    }
                }

                out[hash] =
                    std::move(state);

                // Search-only uploads must never remain in the account.
                // Clean each one up immediately after we have extracted
                // everything needed from it instead of waiting for the
                // rest of the upload batch to finish processing.
                if (!existedBefore) {
                    deleteMagnet(id);
                }
            }
        }
        return out;
    }

    DebridTorrentStatus add(const std::string& infoHash, const std::string& magnet) override {
        (void)infoHash;
        auto root = apiPost(
            "https://api.alldebrid.com/v4/magnet/upload",
            "magnets%5B%5D=" + encode(magnet),
            "AllDebrid upload"
        );
        auto data = root.find("data");
        if (data == root.end() || !data->is_object()) throw std::runtime_error("AllDebrid upload missing data");
        auto magnets = data->find("magnets");
        if (magnets == data->end() || !magnets->is_array() || magnets->empty() || !(*magnets)[0].is_object())
            throw std::runtime_error("AllDebrid upload missing magnet result");
        const auto& item = (*magnets)[0];
        if (item.contains("error") && !item["error"].is_null()) throw std::runtime_error("AllDebrid rejected magnet");

        DebridTorrentStatus state;
        state.remoteId = valueString(item, "id");
        state.cached = item.value("ready", false);
        state.downloaded = !state.remoteId.empty();
        if (state.remoteId.empty()) throw std::runtime_error("AllDebrid upload returned no ID");
        if (state.cached) state.files = files(state);
        return state;
    }

    std::vector<DebridFile> files(const DebridTorrentStatus& torrent) override {
        if (torrent.remoteId.empty()) return {};
        auto rows = filesByIds({torrent.remoteId});
        auto it = rows.find(torrent.remoteId);
        return it == rows.end() ? std::vector<DebridFile>{} : it->second;
    }

    std::string downloadUrl(
        const std::string&,
        const DebridFile& file
    ) override {
        if (file.link.empty())
            throw std::runtime_error(
                "AllDebrid file link is missing");

        auto root = apiPost(
            "https://api.alldebrid.com/v4/link/unlock",
            "link=" + encode(file.link),
            "AllDebrid unlock");

        auto data =
            root.find("data");

        if (
            data == root.end() ||
            !data->is_object()
        ) {
            throw std::runtime_error(
                "AllDebrid unlock missing data");
        }

        const std::string url =
            valueString(*data, "link");

        if (url.empty()) {
            throw std::runtime_error(
                "AllDebrid did not return a direct link");
        }

        return url;
    }

    void remove(
        const std::string& remoteId
    ) override {
        if (remoteId.empty())
            throw std::runtime_error(
                "AllDebrid magnet ID is missing");

        (void)apiPost(
            "https://api.alldebrid.com/v4/magnet/delete",
            "id=" + encode(remoteId),
            "AllDebrid delete");
    }


    std::vector<DebridAccountTorrent> accountTorrents() override {
        std::vector<DebridAccountTorrent> out;

        auto root = apiPost(
            "https://api.alldebrid.com/v4.1/magnet/status",
            "",
            "AllDebrid status");

        auto data = root.find("data");

        if (
            data == root.end() ||
            !data->is_object()
        ) {
            return out;
        }

        auto magnets =
            data->find("magnets");

        if (
            magnets == data->end() ||
            !magnets->is_array()
        ) {
            return out;
        }

        std::vector<std::string> completedIds;

        for (const auto& item : *magnets) {
            if (!item.is_object())
                continue;

            DebridAccountTorrent row;

            row.name =
                valueString(
                    item,
                    "filename");

            row.remoteId =
                valueString(
                    item,
                    "id");

            int statusCode = -1;

            auto statusIt =
                item.find("statusCode");

            if (
                statusIt != item.end() &&
                statusIt->is_number_integer()
            ) {
                statusCode =
                    statusIt->get<int>();
            }

            row.complete =
                statusCode == 4;

            if (!row.complete) {
                const std::string state =
                    lowerHash(
                        valueString(
                            item,
                            "status"));

                row.complete =
                    state == "ready";
            }

            long long size = 0;
            long long downloaded = 0;

            auto sizeIt =
                item.find("size");

            if (
                sizeIt != item.end() &&
                sizeIt->is_number()
            ) {
                size =
                    static_cast<long long>(
                        sizeIt->get<double>());
            }

            auto downloadedIt =
                item.find("downloaded");

            if (
                downloadedIt != item.end() &&
                downloadedIt->is_number()
            ) {
                downloaded =
                    static_cast<long long>(
                        downloadedIt->get<double>());
            }

            if (row.complete) {
                row.progress = 100;
            }
            else if (size > 0) {
                row.progress =
                    std::clamp(
                        static_cast<int>(
                            downloaded * 100 / size),
                        0,
                        100);
            }

            if (
                row.complete &&
                !row.remoteId.empty()
            ) {
                completedIds.push_back(
                    row.remoteId);
            }

            if (row.name.empty())
                row.name = row.remoteId;

            out.push_back(
                std::move(row));
        }

        if (!completedIds.empty()) {
            auto trees =
                filesByIds(
                    completedIds);

            for (auto& row : out) {
                if (!row.complete)
                    continue;

                auto found =
                    trees.find(
                        row.remoteId);

                if (found != trees.end()) {
                    row.files =
                        std::move(
                            found->second);
                }
            }
        }

        return out;
    }

private:
    DebridConfig config_;

    std::vector<std::string> authHeaders() const {
        return {
            "Authorization: Bearer " + config_.apiKey,
            "Content-Type: application/x-www-form-urlencoded"
        };
    }

    Json apiPost(const std::string& url, const std::string& body, const char* label) const {
        auto root = jsonResponse(requestRetry(url, true, authHeaders(), body), label);
        if (root.value("status", "") != "success") throw std::runtime_error(std::string(label) + " failed");
        return root;
    }

    std::set<std::string> existingMagnetIds() const {
        std::set<std::string> ids;
        auto root = apiPost("https://api.alldebrid.com/v4.1/magnet/status", "", "AllDebrid status");
        auto data = root.find("data");
        if (data == root.end() || !data->is_object()) return ids;
        auto magnets = data->find("magnets");
        if (magnets == data->end() || !magnets->is_array()) return ids;
        for (const auto& item : *magnets) {
            if (!item.is_object()) continue;
            const std::string id = valueString(item, "id");
            if (!id.empty()) ids.insert(id);
        }
        return ids;
    }

    std::map<std::string, std::vector<DebridFile>> filesByIds(const std::vector<std::string>& ids) const {
        std::map<std::string, std::vector<DebridFile>> out;
        if (ids.empty()) return out;
        std::string body;
        for (const auto& id : ids) {
            if (!body.empty()) body += "&";
            body += "id%5B%5D=" + encode(id);
        }
        auto root = apiPost("https://api.alldebrid.com/v4/magnet/files", body, "AllDebrid files");
        auto data = root.find("data");
        if (data == root.end() || !data->is_object()) return out;
        auto magnets = data->find("magnets");
        if (magnets == data->end() || !magnets->is_array()) return out;
        for (const auto& item : *magnets) {
            if (!item.is_object() || item.contains("error")) continue;
            const std::string id = valueString(item, "id");
            if (id.empty()) continue;
            auto filesIt = item.find("files");
            if (filesIt != item.end()) appendAllDebridFiles(*filesIt, out[id]);
        }
        return out;
    }

    void deleteMagnet(const std::string& id) const {
        if (id.empty()) return;
        try {
            (void)apiPost(
                "https://api.alldebrid.com/v4/magnet/delete",
                "id=" + encode(id),
                "AllDebrid delete"
            );
        } catch (...) {
            // A failed cleanup must not discard otherwise useful cache results.
        }
    }
};

} // namespace


TorBoxDeviceAuth beginTorBoxDeviceAuth() {
    auto root = jsonResponse(
        requestRetry(
            "https://api.torbox.app/v1/api/user/auth/device/start?app=SwitchGamesBrowser",
            false,
            {}
        ),
        "TorBox device login"
    );

    if (!root.value("success", false))
        throw std::runtime_error(
            root.value(
                "detail",
                std::string("TorBox device login failed")));

    auto data = root.find("data");

    if (data == root.end() || !data->is_object())
        throw std::runtime_error(
            "TorBox device login response missing data");

    TorBoxDeviceAuth auth;

    auth.deviceCode =
        valueString(*data, "device_code");

    auth.code =
        valueString(*data, "code");

    auth.verificationUrl =
        valueString(*data, "verification_url");

    auth.friendlyVerificationUrl =
        valueString(*data, "friendly_verification_url");

    auth.interval =
        data->value("interval", 5);

    auth.expiresIn =
        data->value("expires_in", 600);

    if (
        auth.deviceCode.empty() ||
        auth.code.empty()
    ) {
        throw std::runtime_error(
            "TorBox device login response invalid");
    }

    if (auth.friendlyVerificationUrl.empty())
        auth.friendlyVerificationUrl =
            auth.verificationUrl;

    return auth;
}

TorBoxDeviceCheck checkTorBoxDeviceAuth(
    const TorBoxDeviceAuth& auth
) {
    Json bodyJson = {
        {"device_code", auth.deviceCode}
    };

    const auto response =
        requestRetry(
            "https://api.torbox.app/v1/api/user/auth/device/token",
            true,
            {"Content-Type: application/json"},
            bodyJson.dump()
        );

    TorBoxDeviceCheck result;

    Json root;

    try {
        root = Json::parse(response.body);
    }
    catch (...) {
        if (
            response.status >= 200 &&
            response.status < 300
        ) {
            throw std::runtime_error(
                "TorBox device token returned invalid JSON");
        }

        result.message =
            "Waiting for TorBox approval";

        return result;
    }

    result.message =
        root.value(
            "detail",
            std::string("Waiting for TorBox approval"));

    if (
        response.status < 200 ||
        response.status >= 300 ||
        !root.value("success", false)
    ) {
        return result;
    }

    auto data = root.find("data");

    if (data != root.end()) {
        if (data->is_string()) {
            result.apiKey =
                data->get<std::string>();
        }
        else if (data->is_object()) {
            result.apiKey =
                valueString(*data, "access_token");

            if (result.apiKey.empty())
                result.apiKey =
                    valueString(*data, "api_token");

            if (result.apiKey.empty())
                result.apiKey =
                    valueString(*data, "token");

            if (result.apiKey.empty())
                result.apiKey =
                    valueString(*data, "apikey");

            if (result.apiKey.empty())
                result.apiKey =
                    valueString(*data, "apiKey");
        }
    }

    if (result.apiKey.empty()) {
        result.apiKey =
            valueString(root, "access_token");

        if (result.apiKey.empty())
            result.apiKey =
                valueString(root, "api_token");

        if (result.apiKey.empty())
            result.apiKey =
                valueString(root, "token");

        if (result.apiKey.empty())
            result.apiKey =
                valueString(root, "apikey");
    }

    result.activated =
        !result.apiKey.empty();

    if (result.activated) {
        result.message =
            "TorBox authorized";
    }

    return result;
}


AllDebridPinAuth beginAllDebridPinAuth() {
    auto root = jsonResponse(
        requestRetry(
            "https://api.alldebrid.com/v4.1/pin/get",
            false,
            {}
        ),
        "AllDebrid PIN"
    );

    if (root.value("status", "") != "success")
        throw std::runtime_error("AllDebrid PIN request failed");

    auto data = root.find("data");
    if (data == root.end() || !data->is_object())
        throw std::runtime_error("AllDebrid PIN response missing data");

    AllDebridPinAuth auth;
    auth.pin = valueString(*data, "pin");
    auth.check = valueString(*data, "check");
    auth.userUrl = valueString(*data, "user_url");
    auth.expiresIn = data->value("expires_in", 0);

    if (auth.pin.empty() || auth.check.empty())
        throw std::runtime_error("AllDebrid PIN response invalid");

    return auth;
}

AllDebridPinCheck checkAllDebridPinAuth(
    const AllDebridPinAuth& auth
) {
    std::string body =
        "pin=" + encode(auth.pin) +
        "&check=" + encode(auth.check);

    auto root = jsonResponse(
        requestRetry(
            "https://api.alldebrid.com/v4/pin/check",
            true,
            {"Content-Type: application/x-www-form-urlencoded"},
            body
        ),
        "AllDebrid PIN check"
    );

    if (root.value("status", "") != "success")
        throw std::runtime_error("AllDebrid PIN expired or invalid");

    auto data = root.find("data");
    if (data == root.end() || !data->is_object())
        throw std::runtime_error("AllDebrid PIN check missing data");

    AllDebridPinCheck result;
    result.activated = data->value("activated", false);
    result.expiresIn = data->value("expires_in", 0);
    result.apiKey = valueString(*data, "apikey");

    return result;
}

std::unique_ptr<DebridBackend> createDebridBackend(const DebridConfig& config) {
    if (config.apiKey.empty()) return nullptr;
    if (config.service == DebridService::TorBox) return std::make_unique<TorBoxBackend>(config);
    if (config.service == DebridService::AllDebrid) return std::make_unique<AllDebridBackend>(config);
    return nullptr;
}

} // namespace sgb




