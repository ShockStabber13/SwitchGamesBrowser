#pragma once

#include "json.hpp"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sgb {

enum class DebridService {
    None,
    TorBox,
    AllDebrid
};

struct DebridConfig {
    DebridService service = DebridService::None;
    std::string apiKey;
    // NotUltraNX website session. Password is never persisted.
    std::string notUltraNxToken;
    // Tunable HTTP range workers: use 4 for baseline or 8 for faster mirrors.
    unsigned int notUltraNxConnections = 8;
};

struct DebridCandidate {
    std::string infoHash;
    std::string magnet;
};

struct DebridFile {
    std::string name;
    std::string id;
    bool selected = false;
    std::string link;
    std::uint64_t size = 0;
};

struct DebridTorrentStatus {
    bool cached = false;
    bool downloaded = false;
    std::string remoteId;
    std::vector<DebridFile> files;
};

struct DebridAccountTorrent {
    std::string name;
    std::string infoHash;
    std::string remoteId;
    bool complete = false;
    int progress = 0;
    std::vector<DebridFile> files;
};

inline DebridService parseDebridService(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

    if (value == "torbox") return DebridService::TorBox;
    if (value == "alldebrid") return DebridService::AllDebrid;
    return DebridService::None;
}

inline std::string debridServiceName(DebridService service) {
    switch (service) {
        case DebridService::TorBox: return "TorBox";
        case DebridService::AllDebrid: return "AllDebrid";
        default: return "None";
    }
}

inline std::map<std::string, DebridTorrentStatus> parseDebridStatusJson(const std::string& bytes) {
    std::map<std::string, DebridTorrentStatus> out;
    auto root = nlohmann::json::parse(bytes);
    if (!root.is_object()) return out;

    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().is_object()) continue;

        DebridTorrentStatus status;
        status.cached = it.value().value("cached", false);
        status.downloaded = it.value().value("downloaded", false);
        status.remoteId = it.value().value("remoteId", "");

        auto files = it.value().find("files");
        if (files != it.value().end() && files->is_array()) {
            for (const auto& f : *files) {
                if (f.is_string()) {
                    status.files.push_back({f.get<std::string>(), "", false});
                } else if (f.is_object()) {
                    status.files.push_back({
                        f.value("name", ""),
                        f.value("id", ""),
                        false,
                        f.value("link", ""),
                        f.value("size", std::uint64_t(0))
                    });
                }
            }
        }
        out[it.key()] = std::move(status);
    }
    return out;
}

inline std::string serializeDebridStatusJson(const std::map<std::string, DebridTorrentStatus>& rows) {
    nlohmann::json root = nlohmann::json::object();
    for (const auto& pair : rows) {
        nlohmann::json files = nlohmann::json::array();
        for (const auto& file : pair.second.files) {
            files.push_back({
                {"name", file.name},
                {"id", file.id},
                {"link", file.link},
                {"size", file.size}
            });
        }
        root[pair.first] = {
            {"cached", pair.second.cached},
            {"downloaded", pair.second.downloaded},
            {"remoteId", pair.second.remoteId},
            {"files", files}
        };
    }
    return root.dump(2);
}

class DebridBackend {
public:
    virtual ~DebridBackend() = default;

    virtual std::map<std::string, DebridTorrentStatus> check(
        const std::vector<DebridCandidate>& candidates
    ) = 0;

    virtual DebridTorrentStatus add(
        const std::string& infoHash,
        const std::string& magnet
    ) = 0;

    virtual std::vector<DebridFile> files(
        const DebridTorrentStatus& torrent
    ) = 0;

    virtual std::string downloadUrl(
        const std::string& remoteId,
        const DebridFile& file
    ) = 0;

    virtual void remove(
        const std::string& remoteId
    ) = 0;

    virtual std::vector<DebridAccountTorrent> accountTorrents() = 0;
};


struct TorBoxDeviceAuth {
    std::string deviceCode;
    std::string code;
    std::string verificationUrl;
    std::string friendlyVerificationUrl;
    int interval = 5;
    int expiresIn = 600;
};

struct TorBoxDeviceCheck {
    bool activated = false;
    std::string apiKey;
    std::string message;
};

struct AllDebridPinAuth {
    std::string pin;
    std::string check;
    std::string userUrl;
    int expiresIn = 0;
};

struct AllDebridPinCheck {
    bool activated = false;
    std::string apiKey;
    int expiresIn = 0;
};

TorBoxDeviceAuth beginTorBoxDeviceAuth();
TorBoxDeviceCheck checkTorBoxDeviceAuth(const TorBoxDeviceAuth& auth);

AllDebridPinAuth beginAllDebridPinAuth();
AllDebridPinCheck checkAllDebridPinAuth(const AllDebridPinAuth& auth);

void setDebridCancelFlag(
    const std::atomic<bool>* flag);

std::unique_ptr<DebridBackend> createDebridBackend(const DebridConfig& config);

} // namespace sgb

