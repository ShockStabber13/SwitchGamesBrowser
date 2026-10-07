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
