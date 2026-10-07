$ErrorActionPreference = "Stop"

$JackettPath = ".\switch\include\jackett_provider.hpp"
$TorrentPath = ".\switch\include\torrent_metainfo.hpp"
$ExpectedJackettSha256 = "0486aa207d19a2e7cc03869e7887eb85960566b4d9647145e0915a78efe78eb9"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if (-not (Test-Path -LiteralPath $JackettPath)) {
    throw "Missing $JackettPath. Run this from the SwitchGamesBrowser repository root."
}

$currentText = [System.IO.File]::ReadAllText((Resolve-Path $JackettPath))
$currentHash = (Get-FileHash -LiteralPath $JackettPath -Algorithm SHA256).Hash.ToLowerInvariant()

$alreadyPatched =
    $currentText.Contains('#include "torrent_metainfo.hpp"') -and
    $currentText.Contains('infoHashFromTorrentUrl') -and
    $currentText.Contains('enclosureUrl')

if ($alreadyPatched) {
    Write-Host "Generic Jackett .torrent enclosure support is already installed." -ForegroundColor Green
    exit 0
}

if ($currentHash -ne $ExpectedJackettSha256) {
    throw @"
jackett_provider.hpp does not match the exact source version I analyzed.
No files were changed.

Expected SHA256: $ExpectedJackettSha256
Current  SHA256: $currentHash
"@
}

$backup = "$JackettPath.before-torrent-enclosure.bak"
if (-not (Test-Path -LiteralPath $backup)) {
    Copy-Item -LiteralPath $JackettPath -Destination $backup
}

$torrentHeader = @'
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace sgb_torrent {
namespace detail {

inline uint32_t rol32(uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32U - bits));
}

inline void sha1Block(
    const unsigned char* block,
    uint32_t& h0,
    uint32_t& h1,
    uint32_t& h2,
    uint32_t& h3,
    uint32_t& h4)
{
    uint32_t w[80]{};

    for (size_t i = 0; i < 16; ++i) {
        const size_t p = i * 4;
        w[i] =
            (static_cast<uint32_t>(block[p]) << 24) |
            (static_cast<uint32_t>(block[p + 1]) << 16) |
            (static_cast<uint32_t>(block[p + 2]) << 8) |
            static_cast<uint32_t>(block[p + 3]);
    }

    for (size_t i = 16; i < 80; ++i)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0;
    uint32_t b = h1;
    uint32_t c = h2;
    uint32_t d = h3;
    uint32_t e = h4;

    for (size_t i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        }
        else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        }
        else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        }
        else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }

        const uint32_t temp =
            rol32(a, 5) + f + e + k + w[i];

        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
}

inline std::array<unsigned char, 20> sha1(
    const char* data,
    size_t size)
{
    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xEFCDAB89U;
    uint32_t h2 = 0x98BADCFEU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xC3D2E1F0U;

    size_t offset = 0;
    while (size - offset >= 64) {
        sha1Block(
            reinterpret_cast<const unsigned char*>(data + offset),
            h0, h1, h2, h3, h4);
        offset += 64;
    }

    unsigned char tail[128]{};
    const size_t remaining = size - offset;

    for (size_t i = 0; i < remaining; ++i)
        tail[i] = static_cast<unsigned char>(data[offset + i]);

    tail[remaining] = 0x80;

    const size_t tailBytes =
        remaining < 56 ? 64 : 128;

    const uint64_t bitLength =
        static_cast<uint64_t>(size) * 8ULL;

    for (size_t i = 0; i < 8; ++i) {
        tail[tailBytes - 1 - i] =
            static_cast<unsigned char>(bitLength >> (i * 8));
    }

    sha1Block(tail, h0, h1, h2, h3, h4);

    if (tailBytes == 128)
        sha1Block(tail + 64, h0, h1, h2, h3, h4);

    std::array<unsigned char, 20> digest{};
    const uint32_t words[5] = {h0, h1, h2, h3, h4};

    for (size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<unsigned char>(words[i] >> 24);
        digest[i * 4 + 1] = static_cast<unsigned char>(words[i] >> 16);
        digest[i * 4 + 2] = static_cast<unsigned char>(words[i] >> 8);
        digest[i * 4 + 3] = static_cast<unsigned char>(words[i]);
    }

    return digest;
}

inline bool readByteString(
    const std::string& data,
    size_t& pos,
    size_t& start,
    size_t& length)
{
    if (pos >= data.size() || data[pos] < '0' || data[pos] > '9')
        return false;

    size_t value = 0;
    size_t digits = 0;

    while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
        const size_t digit = static_cast<size_t>(data[pos] - '0');

        if (value > (std::numeric_limits<size_t>::max() - digit) / 10)
            return false;

        value = value * 10 + digit;
        ++pos;
        ++digits;
    }

    if (digits == 0 || pos >= data.size() || data[pos] != ':')
        return false;

    ++pos;

    if (value > data.size() - pos)
        return false;

    start = pos;
    length = value;
    pos += value;
    return true;
}

inline bool skipValue(
    const std::string& data,
    size_t& pos,
    unsigned depth = 0)
{
    if (depth > 128 || pos >= data.size())
        return false;

    const char kind = data[pos];

    if (kind >= '0' && kind <= '9') {
        size_t start = 0;
        size_t length = 0;
        return readByteString(data, pos, start, length);
    }

    if (kind == 'i') {
        ++pos;
        if (pos >= data.size())
            return false;

        if (data[pos] == '-')
            ++pos;

        const size_t firstDigit = pos;
        while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9')
            ++pos;

        if (pos == firstDigit || pos >= data.size() || data[pos] != 'e')
            return false;

        ++pos;
        return true;
    }

    if (kind == 'l') {
        ++pos;
        while (pos < data.size() && data[pos] != 'e') {
            if (!skipValue(data, pos, depth + 1))
                return false;
        }

        if (pos >= data.size())
            return false;

        ++pos;
        return true;
    }

    if (kind == 'd') {
        ++pos;
        while (pos < data.size() && data[pos] != 'e') {
            size_t keyStart = 0;
            size_t keyLength = 0;

            if (!readByteString(data, pos, keyStart, keyLength))
                return false;

            if (!skipValue(data, pos, depth + 1))
                return false;
        }

        if (pos >= data.size())
            return false;

        ++pos;
        return true;
    }

    return false;
}

inline std::string hexDigest(
    const std::array<unsigned char, 20>& digest)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(40);

    for (size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0F];
    }

    return out;
}

} // namespace detail

inline std::string infoHashV1(const std::string& torrent)
{
    if (torrent.size() < 4 || torrent[0] != 'd')
        return "";

    size_t pos = 1;

    while (pos < torrent.size() && torrent[pos] != 'e') {
        size_t keyStart = 0;
        size_t keyLength = 0;

        if (!detail::readByteString(
                torrent,
                pos,
                keyStart,
                keyLength)) {
            return "";
        }

        const bool isInfo =
            keyLength == 4 &&
            torrent.compare(keyStart, keyLength, "info") == 0;

        const size_t valueStart = pos;

        if (!detail::skipValue(torrent, pos))
            return "";

        if (isInfo) {
            const auto digest =
                detail::sha1(
                    torrent.data() + valueStart,
                    pos - valueStart);

            return detail::hexDigest(digest);
        }
    }

    return "";
}

} // namespace sgb_torrent
'@

$jackettHeader = @'
#pragma once

#include "provider.hpp"
#include "provider_api_utils.hpp"
#include "torrent_metainfo.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

    if (!file) {
        throw std::runtime_error(
            "Missing SD:/switch/SwitchGamesBrowser/jackett.json");
    }

    nlohmann::json root;
    file >> root;

    Config config;
    config.url =
        sgb_api::trim(
            root.value("url", std::string{}));
    config.apiKey =
        sgb_api::trim(
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

inline std::string enclosureUrl(const std::string& item)
{
    static const std::regex re(
        R"(<enclosure\b[^>]*\burl=["']([^"']+)["'][^>]*>)",
        std::regex::icase);

    std::smatch match;
    if (!std::regex_search(item, match, re))
        return "";

    return sgb_api::trim(
        sgb_api::xmlDecode(match[1].str()));
}

inline std::string resolveUrl(
    const Config& config,
    std::string value)
{
    value = sgb_api::trim(value);

    if (
        value.rfind("http://", 0) == 0 ||
        value.rfind("https://", 0) == 0
    ) {
        return value;
    }

    if (!value.empty() && value.front() == '/')
        return config.url + value;

    return "";
}

inline std::string infoHashFromTorrentUrl(
    const std::string& torrentUrl)
{
    if (torrentUrl.empty())
        return "";

    try {
        const std::string torrent =
            sgb_api::httpGet(
                torrentUrl,
                "application/x-bittorrent, application/octet-stream, */*");

        // Torrent metainfo is normally tiny. Refuse unexpectedly large
        // responses so a bad indexer cannot consume excessive Switch RAM.
        constexpr size_t maxTorrentBytes = 8 * 1024 * 1024;
        if (torrent.empty() || torrent.size() > maxTorrentBytes)
            return "";

        return sgb_torrent::infoHashV1(torrent);
    }
    catch (...) {
        // One broken download URL should not fail the entire indexer search.
        return "";
    }
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

    const std::string xml =
        sgb_api::httpGet(
            url,
            "application/rss+xml, application/xml, text/xml, */*");

    static const std::regex itemRe(
        R"(<item\b[\s\S]*?</item>)",
        std::regex::icase);

    std::unordered_set<std::string> seen;

    for (
        auto it =
            std::sregex_iterator(
                xml.begin(),
                xml.end(),
                itemRe);
        it != std::sregex_iterator();
        ++it
    ) {
        const std::string item = it->str();

        const std::string title =
            sgb_api::trim(
                sgb_api::tagValue(item, "title"));

        const std::string link =
            sgb_api::trim(
                sgb_api::tagValue(item, "link"));

        std::string magnet =
            sgb_api::trim(
                sgb_api::torznabAttr(
                    item,
                    "magneturl"));

        if (magnet.empty()) {
            magnet =
                sgb_api::trim(
                    sgb_api::torznabAttr(
                        item,
                        "magnet"));
        }

        if (
            magnet.empty() &&
            link.rfind("magnet:", 0) == 0
        ) {
            magnet = link;
        }

        if (magnet.empty()) {
            const std::string guid =
                sgb_api::trim(
                    sgb_api::tagValue(
                        item,
                        "guid"));

            if (guid.rfind("magnet:", 0) == 0)
                magnet = guid;
        }

        std::string hash =
            sgb_api::lowerAscii(
                sgb_api::trim(
                    sgb_api::torznabAttr(
                        item,
                        "infohash")));

        if (hash.empty() && !magnet.empty())
            hash = sgb_api::hashFromMagnet(magnet);

        // Some Jackett indexers, especially private trackers, return only a
        // .torrent enclosure instead of an info hash or magnet. Resolve that
        // generically by downloading the metainfo through Jackett and hashing
        // the raw bencoded "info" dictionary (BitTorrent v1 info hash).
        if (!sgb_api::validInfoHash(hash)) {
            std::string torrentUrl =
                resolveUrl(
                    config,
                    enclosureUrl(item));

            // Standard Torznab commonly uses <link> as the download URL when
            // there is no enclosure. Only use it when it is HTTP(S), never a
            // details page represented by another scheme.
            if (
                torrentUrl.empty() &&
                (link.rfind("http://", 0) == 0 ||
                 link.rfind("https://", 0) == 0)
            ) {
                torrentUrl = link;
            }

            hash =
                infoHashFromTorrentUrl(
                    torrentUrl);
        }

        if (
            title.empty() ||
            !sgb_api::validInfoHash(hash)
        ) {
            continue;
        }

        hash = sgb_api::lowerAscii(hash);

        if (!seen.insert(hash).second)
            continue;

        if (magnet.empty()) {
            magnet =
                sgb_api::magnetFromHash(
                    hash,
                    title);
        }

        sgb::ProviderResult row;
        row.title = title;
        row.magnet = magnet;
        row.infoHash = hash;
        results.push_back(std::move(row));
    }

    return results;
}

} // namespace sgb_jackett
'@

[System.IO.File]::WriteAllText(
    (Join-Path $PWD $TorrentPath),
    $torrentHeader + "`n",
    $Utf8NoBom
)

[System.IO.File]::WriteAllText(
    (Resolve-Path $JackettPath),
    $jackettHeader + "`n",
    $Utf8NoBom
)

# Verify the exact capabilities we intended to install.
$verify = [System.IO.File]::ReadAllText((Resolve-Path $JackettPath))
foreach ($needle in @(
    '#include "torrent_metainfo.hpp"',
    'enclosureUrl',
    'infoHashFromTorrentUrl',
    'application/x-bittorrent',
    'sgb_torrent::infoHashV1'
)) {
    if (-not $verify.Contains($needle)) {
        throw "Verification failed after writing $JackettPath: missing $needle"
    }
}

Write-Host "" 
Write-Host "Installed generic Jackett/Torznab torrent-file fallback." -ForegroundColor Green
Write-Host "" 
Write-Host "What it now accepts:" -ForegroundColor Cyan
Write-Host "  1. Torznab infohash"
Write-Host "  2. Magnet URL"
Write-Host "  3. <enclosure url=...> .torrent results"
Write-Host "  4. HTTP(S) <link> .torrent fallback"
Write-Host "" 
Write-Host "For enclosure-only results it downloads the .torrent through Jackett," 
Write-Host "computes the BitTorrent v1 info hash from the raw bencoded info dictionary," 
Write-Host "then creates the candidate used by the existing debrid/cache pipeline." 
Write-Host "" 
Write-Host "This is shared by every generated Jackett provider; nothing is TorrentLeech-specific." -ForegroundColor Green
Write-Host "Backup: $backup" -ForegroundColor Yellow
Write-Host "" 
Write-Host "Next: clean-rebuild the NRO." -ForegroundColor Cyan
