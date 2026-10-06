#pragma once
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sgb {
using Json = nlohmann::json;
struct Release { std::string title, magnet, size, infoHash, source; std::vector<std::string> files; };
struct Game {
    std::string id, title, titleId, date, cover, summary, ratingSource, ratingUrl;
    std::vector<std::string> genres;
    std::vector<Release> releases;
    double rating = -1;
    int ratingCount = 0;
};
inline std::string lower(std::string s) {
    for (char& c : s) if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string field(const Json& j, const char* key, size_t = 0) {
    auto p = j.find(key);
    if (p == j.end() || !p->is_string()) return "";
    std::string value = p->get<std::string>();
    for (char& c : value) if (static_cast<unsigned char>(c) < 32 && c != '\n') c = ' ';
    return value;
}
inline std::vector<Game> parse(const std::string& bytes) {
    auto root = Json::parse(bytes);
    if (root.at("schemaVersion") != 1 || root.at("platform") != "Nintendo Switch") throw std::runtime_error("Unsupported index format");
    auto& rows = root.at("games");
    if (!rows.is_array() || rows.empty() || rows.size() > 20000) throw std::runtime_error("Invalid game count");
    std::vector<Game> result;
    std::set<std::string> ids;
    for (const auto& row : rows) {
        Game g;
        g.id = field(row, "id", 64); g.title = field(row, "title", 1024);
        if (g.id.empty() || g.title.empty() || !ids.insert(g.id).second) throw std::runtime_error("Invalid or duplicate game ID");
        g.titleId = field(row, "titleId", 16); g.date = field(row, "releaseDate", 10);
        g.cover = field(row, "coverUrl", 2048); g.summary = field(row, "summary");
        g.ratingSource = field(row, "ratingSource", 128); g.ratingUrl = field(row, "ratingUrl", 2048);
        if (row.contains("ratingCount") && row["ratingCount"].is_number_integer()) g.ratingCount = row["ratingCount"].get<int>();
        if (row.contains("rating") && row["rating"].is_number()) {
            double r = row["rating"].get<double>();
            if (std::isfinite(r) && r >= 0 && r <= 100 && g.ratingCount > 0) g.rating = r;
        }
        if (row.contains("genres") && row["genres"].is_array()) {
            if (row["genres"].size() > 64) throw std::runtime_error("Too many genres");
            for (auto& genre : row["genres"]) if (genre.is_string()) g.genres.push_back(genre.get<std::string>().substr(0, 256));
        }
        auto& releases = row.at("releases");
        if (!releases.is_array()) throw std::runtime_error("Invalid release list");
        for (const auto& r : releases) {
            Release release{field(r, "title", 1024), field(r, "magnet", 4096), field(r, "size", 100), field(r, "infoHash", 64), field(r, "source", 128), {}};
            if (r.contains("files") && r["files"].is_array()) {
                if (r["files"].size() > 512) throw std::runtime_error("Too many release files");
                for (const auto& f : r["files"]) if (f.is_string()) release.files.push_back(f.get<std::string>().substr(0, 1024));
            }
            if (release.magnet.rfind("magnet:?", 0) != 0) throw std::runtime_error("Invalid release magnet");
            if (release.infoHash.size() != 40) throw std::runtime_error("Invalid release info hash");
            g.releases.push_back(std::move(release));
        }
        result.push_back(std::move(g));
    }
    return result;
}

inline std::vector<Game> parseIgdbCatalog(const std::string& bytes) {
    auto rows = Json::parse(bytes);

    if (!rows.is_array() || rows.empty() || rows.size() > 20000)
        throw std::runtime_error("Invalid IGDB catalog");

    std::vector<Game> result;
    std::set<std::string> ids;

    for (const auto& row : rows) {
        std::string igdbId = field(row, "id", 64);
        std::string name = field(row, "name", 1024);

        if (igdbId.empty() || name.empty())
            continue;

        Game g;
        g.id = "igdb-" + igdbId;
        g.title = name;

        if (!ids.insert(g.id).second)
            continue;

        g.date = field(row, "releaseDate", 10);
        g.cover = field(row, "cover", 2048);
        g.summary = field(row, "summary", 4096);
        g.ratingSource = "IGDB";

        std::string slug = field(row, "slug", 512);
        if (!slug.empty())
            g.ratingUrl = "https://www.igdb.com/games/" + slug;

        if (
            row.contains("ratingCount") &&
            row["ratingCount"].is_number_integer()
        ) {
            g.ratingCount = row["ratingCount"].get<int>();
        }

        if (
            row.contains("rating") &&
            row["rating"].is_number() &&
            g.ratingCount > 0
        ) {
            double rating = row["rating"].get<double>();

            if (
                std::isfinite(rating) &&
                rating >= 0 &&
                rating <= 100
            ) {
                g.rating = rating;
            }
        }

        if (
            row.contains("genres") &&
            row["genres"].is_array()
        ) {
            for (const auto& genre : row["genres"]) {
                if (genre.is_string())
                    g.genres.push_back(
                        genre.get<std::string>().substr(0, 256)
                    );
            }
        }

        result.push_back(std::move(g));
    }

    if (result.empty())
        throw std::runtime_error("IGDB catalog is empty");

    return result;
}


inline std::vector<Game> parseTorrentShard(
    const std::string& bytes
) {
    auto root = Json::parse(bytes);

    if (
        root.value("schemaVersion", 0) != 1 ||
        !root.contains("games") ||
        !root["games"].is_object()
    ) {
        throw std::runtime_error(
            "Invalid torrent shard"
        );
    }

    std::vector<Game> result;

    for (
        auto gameIt = root["games"].begin();
        gameIt != root["games"].end();
        ++gameIt
    ) {
        if (!gameIt.value().is_array())
            continue;

        Game game{};
        game.id = gameIt.key();

        for (const auto& row : gameIt.value()) {
            if (!row.is_object())
                continue;

            std::vector<std::string> files;

            if (
                row.contains("files") &&
                row["files"].is_array()
            ) {
                for (const auto& file : row["files"]) {
                    if (!file.is_string())
                        continue;

                    auto value =
                        file.get<std::string>();

                    if (
                        !value.empty() &&
                        value.size() <= 4096
                    ) {
                        files.push_back(
                            std::move(value)
                        );
                    }
                }
            }

            std::string source =
                field(row, "source", 1024);

            // Shards also preserve all provider names.
            // Release currently stores one display source,
            // so use the first source if "source" is empty.
            if (
                source.empty() &&
                row.contains("sources") &&
                row["sources"].is_array() &&
                !row["sources"].empty() &&
                row["sources"][0].is_string()
            ) {
                source =
                    row["sources"][0]
                        .get<std::string>();
            }

            game.releases.push_back(
                Release{
                    field(row, "title", 16384),
                    field(row, "magnet", 65536),
                    field(row, "size", 1024),
                    field(row, "infoHash", 256),
                    source,
                    std::move(files)
                }
            );
        }

        result.push_back(
            std::move(game)
        );
    }

    return result;
}

inline void mergeReleases(
    std::vector<Game>& catalog,
    const std::vector<Game>& torrentIndex
) {
    std::map<std::string, const Game*> torrents;

    for (const auto& game : torrentIndex)
        torrents[game.id] = &game;

    for (auto& game : catalog) {
        auto found = torrents.find(game.id);

        if (found != torrents.end())
            game.releases = found->second->releases;
        else
            game.releases.clear();
    }
}

inline std::string read(const std::string& path, size_t = 0) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("File unavailable");
    auto size = file.tellg();
    if (size < 0) throw std::runtime_error("Invalid file size");
    std::string bytes(static_cast<size_t>(size), '\0');
    file.seekg(0); file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("File read failed");
    return bytes;
}
enum class Sort { Title, Rating, Newest };
struct Filter { std::string search, genre; double minRating = 0; int minReviews = 0; bool favouritesOnly = false; Sort sort = Sort::Title; };
inline std::vector<size_t> browse(const std::vector<Game>& games, const Filter& f, const std::set<std::string>& favourites) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < games.size(); ++i) {
        const auto& g = games[i];
        if (!f.search.empty() && lower(g.title).find(lower(f.search)) == std::string::npos) continue;
        if (!f.genre.empty() && std::find(g.genres.begin(), g.genres.end(), f.genre) == g.genres.end()) continue;
        if (f.minRating > 0 && g.rating < f.minRating) continue;
        if (g.ratingCount < f.minReviews) continue;
        if (f.favouritesOnly && !favourites.count(g.id)) continue;
        indices.push_back(i);
    }
    std::stable_sort(indices.begin(), indices.end(), [&](size_t ai, size_t bi) {
        const auto& a = games[ai]; const auto& b = games[bi];
        if (f.sort == Sort::Rating && a.rating != b.rating) return a.rating > b.rating;
        if (f.sort == Sort::Newest && a.date != b.date) return a.date > b.date;
        if (lower(a.title) != lower(b.title)) return lower(a.title) < lower(b.title);
        return a.id < b.id;
    });
    return indices;
}
}
