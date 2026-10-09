/*
 * SwitchGamesBrowser package installer.
 *
 * The content-storage/install flow follows the same public Switch service
 * model used by established MIT-licensed homebrew installers such as Tinleaf
 * (copyright Adubbz and contributors), while this implementation is written
 * specifically for SwitchGamesBrowser.
 */

#include "installer.hpp"

#include <switch.h>
#include <curl/curl.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace sgb {
namespace {

constexpr u32 MagicPfs0 = 0x30534650;
constexpr u32 MagicHfs0 = 0x30534648;
constexpr u32 MagicNca3 = 0x3341434e;
constexpr std::size_t NcaHeaderSize = 0x4000;
constexpr u64 NczSectionMagic = 0x4e544345535a434eULL;

struct Pfs0Header {
    u32 magic;
    u32 numFiles;
    u32 stringTableSize;
    u32 reserved;
} __attribute__((packed));

struct Pfs0Entry {
    u64 dataOffset;
    u64 fileSize;
    u32 stringOffset;
    u32 reserved;
} __attribute__((packed));

struct Hfs0Header {
    u32 magic;
    u32 numFiles;
    u32 stringTableSize;
    u32 reserved;
} __attribute__((packed));

struct Hfs0Entry {
    u64 dataOffset;
    u64 fileSize;
    u32 stringOffset;
    u32 hashedSize;
    u64 reserved;
    u8 hash[0x20];
} __attribute__((packed));

struct NcaSectionEntry {
    u32 mediaStartOffset;
    u32 mediaEndOffset;
    u8 reserved[8];
} __attribute__((packed));

struct NcaFsHeader {
    u8 bytes[0x200];
} __attribute__((packed));

struct NcaHeader {
    u8 fixedKeySignature[0x100];
    u8 npdmKeySignature[0x100];
    u32 magic;
    u8 distribution;
    u8 contentType;
    u8 cryptoType;
    u8 keyAreaKeyIndex;
    u64 ncaSize;
    u64 titleId;
    u8 reserved218[4];
    u32 sdkVersion;
    u8 cryptoType2;
    u8 reserved221[0x0f];
    u64 rightsId[2];
    NcaSectionEntry sections[4];
    u8 sectionHashes[4 * 0x20];
    u8 keys[4 * 0x10];
    u8 reserved340[0xc0];
    NcaFsHeader fsHeaders[4];
} __attribute__((packed));

static_assert(sizeof(NcaHeader) == 0xc00, "NCA header layout mismatch");

struct PackagedContentMetaHeader {
    u64 titleId;
    u32 version;
    u8 type;
    u8 reserved0d;
    u16 extendedHeaderSize;
    u16 contentCount;
    u16 contentMetaCount;
    u8 attributes;
    u8 storageId;
    u8 installType;
    u8 committed;
    u32 requiredSystemVersion;
    u32 reserved1c;
} __attribute__((packed));

static_assert(
    sizeof(PackagedContentMetaHeader) == 0x20,
    "CNMT header layout mismatch");

struct NczSection {
    u64 offset;
    u64 size;
    u8 cryptoType;
    u8 padding1[7];
    u64 padding2;
    u8 cryptoKey[0x10];
    u8 cryptoCounter[0x10];
} __attribute__((packed));

struct PackageEntry {
    std::string name;
    u64 offset = 0;
    u64 size = 0;
};

struct HttpRangeContext {
    std::function<void(
        const u8*,
        std::size_t)> consume;

    std::shared_ptr<std::atomic<bool>>
        cancel;

    std::exception_ptr error;
    const std::atomic<bool>* stop = nullptr;

    u64 expected = 0;
    u64 received = 0;
};

size_t httpRangeWrite(
    char* data,
    size_t size,
    size_t count,
    void* opaque)
{
    auto* context =
        static_cast<HttpRangeContext*>(
            opaque);

    if ((context->cancel && context->cancel->load()) ||
        (context->stop && context->stop->load())) {
        return 0;
    }

    if (
        size &&
        count >
            SIZE_MAX / size
    ) {
        return 0;
    }

    const std::size_t bytes =
        size * count;

    if (
        context->received >
            context->expected ||
        bytes >
            context->expected -
                context->received
    ) {
        // Never accept data outside the requested byte range.
        // This also prevents a server which ignored Range from
        // making us download the complete package.
        return 0;
    }

    try {
        if (context->consume) {
            context->consume(
                reinterpret_cast<
                    const u8*>(data),
                bytes);
        }
    }
    catch (...) {
        context->error =
            std::current_exception();

        return 0;
    }

    context->received +=
        bytes;

    return bytes;
}

int httpRangeProgress(
    void* opaque,
    curl_off_t,
    curl_off_t,
    curl_off_t,
    curl_off_t)
{
    auto* context =
        static_cast<HttpRangeContext*>(
            opaque);

    return ((context->cancel && context->cancel->load()) ||
            (context->stop && context->stop->load())) ? 1 : 0;
}

class HttpPackageSource {
public:
    HttpPackageSource(
        std::string url,
        std::shared_ptr<std::atomic<bool>>
            cancel,
        u64 knownSize)
        : url_(std::move(url)),
          cancel_(std::move(cancel)),
          knownSize_(knownSize)
    {
        curl_ =
            curl_easy_init();

        if (!curl_)
            throw std::runtime_error(
                "Cloud install initialization failed");

        curl_easy_setopt(
            curl_,
            CURLOPT_URL,
            url_.c_str());

        curl_easy_setopt(
            curl_,
            CURLOPT_USERAGENT,
            "SwitchGamesBrowser/0.4");

        curl_easy_setopt(
            curl_,
            CURLOPT_FOLLOWLOCATION,
            1L);

        curl_easy_setopt(
            curl_,
            CURLOPT_MAXREDIRS,
            8L);

        curl_easy_setopt(
            curl_,
            CURLOPT_CONNECTTIMEOUT,
            20L);

        curl_easy_setopt(
            curl_,
            CURLOPT_TIMEOUT,
            0L);

        curl_easy_setopt(
            curl_,
            CURLOPT_NOSIGNAL,
            1L);

        curl_easy_setopt(
            curl_,
            CURLOPT_LOW_SPEED_LIMIT,
            1L);

        curl_easy_setopt(
            curl_,
            CURLOPT_LOW_SPEED_TIME,
            30L);

        // Keep the network side buffered while the NCA writer consumes
        // the stream. libcurl may clamp this to its supported maximum.
        curl_easy_setopt(
            curl_,
            CURLOPT_BUFFERSIZE,
            512L * 1024L);

        curl_easy_setopt(
            curl_,
            CURLOPT_ACCEPT_ENCODING,
            "identity");

        curl_easy_setopt(
            curl_,
            CURLOPT_PROTOCOLS,
            CURLPROTO_HTTP |
                CURLPROTO_HTTPS);

        curl_easy_setopt(
            curl_,
            CURLOPT_REDIR_PROTOCOLS,
            CURLPROTO_HTTP |
                CURLPROTO_HTTPS);

#ifdef __SWITCH__
        curl_easy_setopt(
            curl_,
            CURLOPT_CAINFO,
            "romfs:/cacert.pem");
#endif

        curl_easy_setopt(
            curl_,
            CURLOPT_SSL_VERIFYPEER,
            1L);

        curl_easy_setopt(
            curl_,
            CURLOPT_SSL_VERIFYHOST,
            2L);

        curl_easy_setopt(
            curl_,
            CURLOPT_WRITEFUNCTION,
            httpRangeWrite);

        curl_easy_setopt(
            curl_,
            CURLOPT_NOPROGRESS,
            0L);

        curl_easy_setopt(
            curl_,
            CURLOPT_XFERINFOFUNCTION,
            httpRangeProgress);
    }

    ~HttpPackageSource()
    {
        if (curl_) {
            curl_easy_cleanup(
                curl_);
        }
    }

    HttpPackageSource(
        const HttpPackageSource&) = delete;

    HttpPackageSource& operator=(
        const HttpPackageSource&) = delete;

    void streamExact(
        u64 offset,
        u64 size,
        const std::function<void(
            const u8*,
            std::size_t)>& consume,
        const std::atomic<bool>* stop = nullptr)
    {
        if (!size)
            return;

        if (
            offset >
                UINT64_MAX - size ||
            (
                knownSize_ &&
                (
                    offset > knownSize_ ||
                    size > knownSize_ - offset
                )
            )
        ) {
            throw std::runtime_error(
                "Package range is outside the cloud file");
        }

        if (
            cancel_ &&
            cancel_->load()
        ) {
            throw std::runtime_error(
                "Install cancelled");
        }

        const std::string range =
            std::to_string(offset) +
            "-" +
            std::to_string(
                offset + size - 1);

        HttpRangeContext context;
        context.consume = consume;
        context.cancel = cancel_;
        context.stop = stop;
        context.expected = size;

        curl_easy_setopt(
            curl_,
            CURLOPT_RANGE,
            range.c_str());

        curl_easy_setopt(
            curl_,
            CURLOPT_WRITEDATA,
            &context);

        curl_easy_setopt(
            curl_,
            CURLOPT_XFERINFODATA,
            &context);

        const CURLcode result =
            curl_easy_perform(
                curl_);

        long httpStatus = 0;

        curl_easy_getinfo(
            curl_,
            CURLINFO_RESPONSE_CODE,
            &httpStatus);

        if (
            cancel_ &&
            cancel_->load()
        ) {
            throw std::runtime_error(
                "Install cancelled");
        }

        if (context.error) {
            std::rethrow_exception(
                context.error);
        }

        if (httpStatus == 200) {
            throw std::runtime_error(
                "Cloud source does not support ranged reads");
        }

        if (httpStatus != 206) {
            throw std::runtime_error(
                "Cloud read HTTP " +
                std::to_string(
                    httpStatus));
        }

        if (result != CURLE_OK) {
            throw std::runtime_error(
                std::string(
                    "Cloud read failed: ") +
                curl_easy_strerror(
                    result));
        }

        if (
            context.received !=
                context.expected
        ) {
            throw std::runtime_error(
                "Cloud package read was incomplete");
        }
    }

    // Separate easy handles are required for concurrent HTTP ranges.
    std::unique_ptr<HttpPackageSource> clone() const {
        return std::make_unique<HttpPackageSource>(url_, cancel_, knownSize_);
    }

    void readExact(
        u64 offset,
        void* output,
        std::size_t size)
    {
        auto* out =
            static_cast<u8*>(
                output);

        std::size_t written = 0;

        streamExact(
            offset,
            size,
            [&](const u8* data,
                std::size_t bytes)
            {
                std::memcpy(
                    out + written,
                    data,
                    bytes);

                written += bytes;
            });

        if (written != size) {
            throw std::runtime_error(
                "Cloud package read was incomplete");
        }
    }

private:
    std::string url_;

    std::shared_ptr<std::atomic<bool>>
        cancel_;

    u64 knownSize_ = 0;

    CURL* curl_ = nullptr;
};

struct Package {
    std::shared_ptr<HttpPackageSource>
        source;

    std::vector<PackageEntry>
        entries;
};

struct ContentStorageRecord {
    NcmContentMetaKey metaRecord;
    u64 storageId;
} __attribute__((packed));

static std::atomic<bool>*
    gInstallCancel = nullptr;

bool cancelled()
{
    return
        gInstallCancel &&
        gInstallCancel->load();
}

void checkCancelled()
{
    if (cancelled()) {
        throw std::runtime_error(
            "Install cancelled");
    }
}

std::string lowerAscii(
    std::string value)
{
    for (char& c : value) {
        const auto uc =
            static_cast<unsigned char>(c);

        if (uc < 128) {
            c =
                static_cast<char>(
                    std::tolower(uc));
        }
    }

    return value;
}

bool endsWithInsensitive(
    const std::string& value,
    const std::string& suffix)
{
    if (
        value.size() <
            suffix.size()
    ) {
        return false;
    }

    return
        lowerAscii(
            value.substr(
                value.size() -
                suffix.size())) ==
        lowerAscii(
            suffix);
}

void readExact(
    HttpPackageSource& source,
    u64 offset,
    void* output,
    std::size_t size)
{
    source.readExact(
        offset,
        output,
        size);
}

std::vector<u8> readBytes(
    HttpPackageSource& source,
    u64 offset,
    std::size_t size)
{
    std::vector<u8> out(size);

    readExact(
        source,
        offset,
        out.data(),
        out.size());

    return out;
}

std::string entryString(
    const std::vector<u8>& strings,
    u32 offset)
{
    if (offset >= strings.size())
        throw std::runtime_error(
            "Package string offset invalid");

    const char* start =
        reinterpret_cast<const char*>(
            strings.data() + offset);

    const std::size_t remaining =
        strings.size() - offset;

    const void* nul =
        std::memchr(
            start,
            0,
            remaining);

    if (!nul)
        throw std::runtime_error(
            "Package file name invalid");

    return std::string(start);
}

Package openPfs0(
    const std::shared_ptr<
        HttpPackageSource>& source)
{
    Package package;
    package.source = source;

    Pfs0Header header{};

    readExact(
        *package.source,
        0,
        &header,
        sizeof(header));

    if (header.magic != MagicPfs0)
        throw std::runtime_error(
            "Invalid NSP/NSZ PFS0 header");

    if (
        header.numFiles == 0 ||
        header.numFiles > 8192 ||
        header.stringTableSize >
            64 * 1024 * 1024
    ) {
        throw std::runtime_error(
            "Invalid NSP/NSZ header values");
    }

    std::vector<Pfs0Entry> rows(
        header.numFiles);

    readExact(
        *package.source,
        sizeof(header),
        rows.data(),
        rows.size() *
            sizeof(Pfs0Entry));

    const u64 stringOffset =
        sizeof(header) +
        rows.size() *
            sizeof(Pfs0Entry);

    auto strings =
        readBytes(
            *package.source,
            stringOffset,
            header.stringTableSize);

    const u64 dataOffset =
        stringOffset +
        header.stringTableSize;

    package.entries.reserve(
        rows.size());

    for (const auto& row : rows) {
        package.entries.push_back({
            entryString(
                strings,
                row.stringOffset),
            dataOffset +
                row.dataOffset,
            row.fileSize
        });
    }

    return package;
}

Package openXci(
    const std::shared_ptr<
        HttpPackageSource>& source)
{
    Package package;
    package.source = source;

    constexpr u64 rootOffset =
        0xf000;

    Hfs0Header root{};

    readExact(
        *package.source,
        rootOffset,
        &root,
        sizeof(root));

    if (root.magic != MagicHfs0)
        throw std::runtime_error(
            "Invalid XCI/XCZ HFS0 header");

    if (
        root.numFiles == 0 ||
        root.numFiles > 1024 ||
        root.stringTableSize >
            16 * 1024 * 1024
    ) {
        throw std::runtime_error(
            "Invalid XCI/XCZ root header");
    }

    std::vector<Hfs0Entry> rootRows(
        root.numFiles);

    readExact(
        *package.source,
        rootOffset +
            sizeof(root),
        rootRows.data(),
        rootRows.size() *
            sizeof(Hfs0Entry));

    const u64 rootStringsOffset =
        rootOffset +
        sizeof(root) +
        rootRows.size() *
            sizeof(Hfs0Entry);

    auto rootStrings =
        readBytes(
            *package.source,
            rootStringsOffset,
            root.stringTableSize);

    const u64 rootDataOffset =
        rootStringsOffset +
        root.stringTableSize;

    bool foundSecure = false;
    u64 secureOffset = 0;

    for (const auto& row : rootRows) {
        const auto name =
            entryString(
                rootStrings,
                row.stringOffset);

        if (name == "secure") {
            secureOffset =
                rootDataOffset +
                row.dataOffset;

            foundSecure = true;
            break;
        }
    }

    if (!foundSecure)
        throw std::runtime_error(
            "XCI secure partition not found");

    Hfs0Header secure{};

    readExact(
        *package.source,
        secureOffset,
        &secure,
        sizeof(secure));

    if (secure.magic != MagicHfs0)
        throw std::runtime_error(
            "Invalid XCI secure partition");

    if (
        secure.numFiles == 0 ||
        secure.numFiles > 8192 ||
        secure.stringTableSize >
            64 * 1024 * 1024
    ) {
        throw std::runtime_error(
            "Invalid XCI secure header values");
    }

    std::vector<Hfs0Entry> rows(
        secure.numFiles);

    readExact(
        *package.source,
        secureOffset +
            sizeof(secure),
        rows.data(),
        rows.size() *
            sizeof(Hfs0Entry));

    const u64 stringsOffset =
        secureOffset +
        sizeof(secure) +
        rows.size() *
            sizeof(Hfs0Entry);

    auto strings =
        readBytes(
            *package.source,
            stringsOffset,
            secure.stringTableSize);

    const u64 dataOffset =
        stringsOffset +
        secure.stringTableSize;

    package.entries.reserve(
        rows.size());

    for (const auto& row : rows) {
        package.entries.push_back({
            entryString(
                strings,
                row.stringOffset),
            dataOffset +
                row.dataOffset,
            row.fileSize
        });
    }

    return package;
}

Package openPackage(
    const std::string& url,
    const std::string& name,
    const std::shared_ptr<
        std::atomic<bool>>& cancel,
    u64 knownSize)
{
    auto source =
        std::make_shared<
            HttpPackageSource>(
                url,
                cancel,
                knownSize);

    if (
        endsWithInsensitive(name, ".nsp") ||
        endsWithInsensitive(name, ".nsz")
    ) {
        return openPfs0(
            source);
    }

    if (
        endsWithInsensitive(name, ".xci") ||
        endsWithInsensitive(name, ".xcz")
    ) {
        return openXci(
            source);
    }

    throw std::runtime_error(
        "Unsupported package type");
}

const PackageEntry* findEntry(
    const Package& package,
    const std::string& name)
{
    for (const auto& entry :
         package.entries) {
        if (entry.name == name)
            return &entry;
    }

    return nullptr;
}

std::vector<const PackageEntry*>
findEntriesEndingWith(
    const Package& package,
    const std::string& suffix)
{
    std::vector<const PackageEntry*> out;

    for (const auto& entry :
         package.entries) {
        if (
            endsWithInsensitive(
                entry.name,
                suffix)
        ) {
            out.push_back(&entry);
        }
    }

    return out;
}

std::string contentIdString(
    const NcmContentId& id)
{
    static const char* hex =
        "0123456789abcdef";

    std::string out;
    out.resize(32);

    for (std::size_t i = 0; i < 16; ++i) {
        out[i * 2] =
            hex[(id.c[i] >> 4) & 0xf];

        out[i * 2 + 1] =
            hex[id.c[i] & 0xf];
    }

    return out;
}

NcmContentId contentIdFromName(
    const std::string& name)
{
    if (name.size() < 32)
        throw std::runtime_error(
            "NCA file name is too short");

    NcmContentId out{};

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';

        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;

        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;

        return -1;
    };

    for (std::size_t i = 0; i < 16; ++i) {
        const int hi =
            nibble(name[i * 2]);

        const int lo =
            nibble(name[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            throw std::runtime_error(
                "NCA file name has invalid ID");

        out.c[i] =
            static_cast<u8>(
                (hi << 4) | lo);
    }

    return out;
}

u64 baseTitleId(
    u64 titleId,
    u8 type)
{
    switch (
        static_cast<NcmContentMetaType>(
            type)
    ) {
        case NcmContentMetaType_Patch:
            return titleId ^ 0x800;

        case NcmContentMetaType_AddOnContent:
        case NcmContentMetaType_DataPatch:
            return
                (titleId ^ 0x1000) &
                ~u64(0xfff);

        default:
            return titleId;
    }
}

void deriveHeaderKey(
    u8 out[0x20])
{
    static const u8 kekSource[0x10] = {
        0x1f,0x12,0x91,0x3a,0x4a,0xcb,0xf0,0x0d,
        0x4c,0xde,0x3a,0xf6,0xd5,0x23,0x88,0x2a
    };

    static const u8 keySource[0x20] = {
        0x5a,0x3e,0xd8,0x4f,0xde,0xc0,0xd8,0x26,
        0x31,0xf7,0xe2,0x5d,0x19,0x7b,0xf5,0xd0,
        0x1c,0x9b,0x7b,0xfa,0xf6,0x28,0x18,0x3d,
        0x71,0xf6,0x4d,0x73,0xf1,0x50,0xb9,0xd2
    };

    u8 kek[0x10]{};

    Result rc =
        splCryptoGenerateAesKek(
            kekSource,
            0,
            0,
            kek);

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to derive NCA header KEK");

    rc =
        splCryptoGenerateAesKey(
            kek,
            keySource,
            out);

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to derive NCA header key");

    rc =
        splCryptoGenerateAesKey(
            kek,
            keySource + 0x10,
            out + 0x10);

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to derive NCA header key");
}

struct NczSectionContext {
    NczSection section{};
    Aes128CtrContext aes{};
    std::array<u8, 0x10> counter{};
};

void resetCtr(
    NczSectionContext& context,
    u64 absoluteOffset)
{
    std::memcpy(
        context.counter.data(),
        context.section.cryptoCounter,
        context.counter.size());

    const u64 relative =
        absoluteOffset -
        context.section.offset;

    u64 block =
        relative >> 4;

    for (int i = 15; i >= 0 && block; --i) {
        const u64 sum =
            static_cast<u64>(
                context.counter[
                    static_cast<std::size_t>(i)]) +
            (block & 0xff);

        context.counter[
            static_cast<std::size_t>(i)] =
                static_cast<u8>(
                    sum & 0xff);

        block =
            (block >> 8) +
            (sum >> 8);
    }

    aes128CtrContextResetCtr(
        &context.aes,
        context.counter.data());
}

class NcaOutput {
public:
    NcaOutput(
        NcmContentStorage* storage,
        const NcmContentId& contentId,
        const std::shared_ptr<std::atomic<bool>>& cancel,
        InstallProgress& progress,
        int basePercent,
        int spanPercent)
        : storage_(storage),
          contentId_(contentId),
          placeholderId_(
              *reinterpret_cast<const NcmPlaceHolderId*>(
                  &contentId)),
          cancel_(cancel),
          progress_(progress),
          basePercent_(basePercent),
          spanPercent_(spanPercent)
    {
        deriveHeaderKey(
            headerKey_.data());
    }

    ~NcaOutput()
    {
        if (!registered_ && created_) {
            ncmContentStorageDeletePlaceHolder(
                storage_,
                &placeholderId_);
        }
    }

    void write(
        const u8* data,
        std::size_t size)
    {
        if (!size)
            return;

        check();

        std::size_t consumed = 0;

        if (header_.size() < NcaHeaderSize) {
            const std::size_t take =
                std::min(
                    size,
                    NcaHeaderSize -
                        header_.size());

            header_.insert(
                header_.end(),
                data,
                data + take);

            consumed += take;

            if (
                header_.size() ==
                    NcaHeaderSize
            ) {
                flushHeader();
            }
        }

        if (consumed < size) {
            writeBody(
                data + consumed,
                size - consumed);
        }
    }

    void finish()
    {
        // A normal NCA is not required to be 0x4000 bytes long.
        // Sphaira only needs the encrypted NCA header itself (0xC00)
        // to determine the real NCA size. We normally buffer 0x4000
        // so we can detect the NCZ section header at offset 0x4000,
        // but a small plain NCA may legitimately end before that.
        if (
            header_.size() <
                NcaHeaderSize
        ) {
            if (
                header_.size() <
                    sizeof(NcaHeader)
            ) {
                throw std::runtime_error(
                    "NCA header incomplete");
            }

            // End-of-entry before 0x4000 means this cannot contain
            // an NCZ section header. Treat the bytes we have as a
            // complete plain NCA and validate its true size below.
            flushHeader();
        }

        if (ncz_) {
            flushNcz(true);
        }

        if (
            written_ !=
                expectedSize_
        ) {
            throw std::runtime_error(
                "NCA size mismatch after install");
        }

        Result rc =
            ncmContentStorageRegister(
                storage_,
                &contentId_,
                &placeholderId_);

        if (R_FAILED(rc)) {
            bool exists = false;

            if (
                R_SUCCEEDED(
                    ncmContentStorageHas(
                        storage_,
                        &exists,
                        &contentId_)) &&
                exists
            ) {
                registered_ = true;
            }
            else {
                throw std::runtime_error(
                    "Failed to register NCA");
            }
        }
        else {
            registered_ = true;
        }

        ncmContentStorageDeletePlaceHolder(
            storage_,
            &placeholderId_);

        created_ = false;

        updateProgress(100);
    }

private:
    NcmContentStorage* storage_;
    NcmContentId contentId_{};
    NcmPlaceHolderId placeholderId_{};
    std::shared_ptr<std::atomic<bool>> cancel_;
    InstallProgress& progress_;
    int basePercent_ = 0;
    int spanPercent_ = 100;

    std::array<u8,0x20> headerKey_{};
    std::vector<u8> header_;
    bool created_ = false;
    bool registered_ = false;
    bool ncz_ = false;
    bool nczHeaderReady_ = false;
    u64 expectedSize_ = 0;
    u64 written_ = 0;

    std::vector<u8> nczHeaderBuffer_;
    std::vector<NczSectionContext> nczSections_;
    ZSTD_DCtx* zstd_ = nullptr;

    void check()
    {
        if (
            cancel_ &&
            cancel_->load()
        ) {
            throw std::runtime_error(
                "Install cancelled");
        }
    }

    void updateProgress(
        int localPercent)
    {
        const int clamped =
            std::clamp(
                localPercent,
                0,
                100);

        progress_.set(
            "Installing",
            basePercent_ +
                clamped *
                    spanPercent_ /
                    100);
    }

    void flushHeader()
    {
        if (
            header_.size() <
                sizeof(NcaHeader)
        ) {
            throw std::runtime_error(
                "NCA header invalid");
        }

        NcaHeader plain{};

        aes128XtsContextCreate(
            &xtsDecrypt_,
            headerKey_.data(),
            headerKey_.data() + 0x10,
            false);

        aes128XtsContextCreate(
            &xtsEncrypt_,
            headerKey_.data(),
            headerKey_.data() + 0x10,
            true);

        for (
            std::size_t offset = 0;
            offset < sizeof(NcaHeader);
            offset += 0x200
        ) {
            aes128XtsContextResetSector(
                &xtsDecrypt_,
                offset / 0x200,
                true);

            aes128XtsDecrypt(
                &xtsDecrypt_,
                reinterpret_cast<u8*>(&plain) +
                    offset,
                header_.data() +
                    offset,
                0x200);
        }

        if (plain.magic != MagicNca3)
            throw std::runtime_error(
                "Invalid NCA magic");

        expectedSize_ =
            plain.ncaSize;

        if (
            expectedSize_ <
                sizeof(NcaHeader)
        ) {
            throw std::runtime_error(
                "Invalid NCA size");
        }

        plain.distribution = 0;

        for (
            std::size_t offset = 0;
            offset < sizeof(NcaHeader);
            offset += 0x200
        ) {
            aes128XtsContextResetSector(
                &xtsEncrypt_,
                offset / 0x200,
                true);

            aes128XtsEncrypt(
                &xtsEncrypt_,
                header_.data() +
                    offset,
                reinterpret_cast<const u8*>(
                    &plain) +
                    offset,
                0x200);
        }

        ncmContentStorageDeletePlaceHolder(
            storage_,
            &placeholderId_);

        ncmContentStorageDelete(
            storage_,
            &contentId_);

        Result rc =
            ncmContentStorageCreatePlaceHolder(
                storage_,
                &contentId_,
                &placeholderId_,
                static_cast<s64>(
                    expectedSize_));

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to create NCA placeholder");

        created_ = true;

        rc =
            ncmContentStorageWritePlaceHolder(
                storage_,
                &placeholderId_,
                0,
                header_.data(),
                header_.size());

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to write NCA header");

        written_ =
            header_.size();
    }

    void writeRaw(
        const u8* data,
        std::size_t size)
    {
        check();

        const Result rc =
            ncmContentStorageWritePlaceHolder(
                storage_,
                &placeholderId_,
                written_,
                data,
                size);

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed writing NCA content");

        written_ += size;

        updateProgress(
            expectedSize_
                ? static_cast<int>(
                    written_ * 100 /
                    expectedSize_)
                : 0);
    }

    void writeBody(
        const u8* data,
        std::size_t size)
    {
        if (!nczHeaderReady_) {
            nczHeaderBuffer_.insert(
                nczHeaderBuffer_.end(),
                data,
                data + size);

            if (
                nczHeaderBuffer_.size() <
                    sizeof(u64) * 2
            ) {
                return;
            }

            u64 magic = 0;
            u64 count = 0;

            std::memcpy(
                &magic,
                nczHeaderBuffer_.data(),
                sizeof(magic));

            if (magic != NczSectionMagic) {
                nczHeaderReady_ = true;
                ncz_ = false;

                writeRaw(
                    nczHeaderBuffer_.data(),
                    nczHeaderBuffer_.size());

                nczHeaderBuffer_.clear();
                return;
            }

            std::memcpy(
                &count,
                nczHeaderBuffer_.data() +
                    sizeof(u64),
                sizeof(count));

            if (
                count == 0 ||
                count > 64
            ) {
                throw std::runtime_error(
                    "Invalid NCZ section count");
            }

            const std::size_t needed =
                sizeof(u64) * 2 +
                static_cast<std::size_t>(count) *
                    sizeof(NczSection);

            if (
                nczHeaderBuffer_.size() <
                    needed
            ) {
                return;
            }

            ncz_ = true;
            nczHeaderReady_ = true;

            nczSections_.resize(
                static_cast<std::size_t>(
                    count));

            const u8* sections =
                nczHeaderBuffer_.data() +
                sizeof(u64) * 2;

            for (
                std::size_t i = 0;
                i < nczSections_.size();
                ++i
            ) {
                std::memcpy(
                    &nczSections_[i].section,
                    sections +
                        i *
                            sizeof(NczSection),
                    sizeof(NczSection));

                aes128CtrContextCreate(
                    &nczSections_[i].aes,
                    nczSections_[i]
                        .section.cryptoKey,
                    nczSections_[i]
                        .section.cryptoCounter);
            }

            zstd_ =
                ZSTD_createDCtx();

            if (!zstd_)
                throw std::runtime_error(
                    "Failed to initialize NSZ decompressor");

            const std::size_t extra =
                nczHeaderBuffer_.size() -
                needed;

            if (extra) {
                decompressNcz(
                    nczHeaderBuffer_.data() +
                        needed,
                    extra);
            }

            nczHeaderBuffer_.clear();
            return;
        }

        if (ncz_)
            decompressNcz(data, size);
        else
            writeRaw(data, size);
    }

    NczSectionContext* sectionFor(
        u64 offset)
    {
        for (auto& context :
             nczSections_) {
            if (
                offset >=
                    context.section.offset &&
                offset <
                    context.section.offset +
                    context.section.size
            ) {
                return &context;
            }
        }

        return nullptr;
    }

    void encryptNczOutput(
        u8* data,
        std::size_t size,
        u64 offset)
    {
        std::size_t position = 0;

        while (position < size) {
            NczSectionContext* context =
                sectionFor(
                    offset +
                    position);

            if (!context)
                throw std::runtime_error(
                    "NCZ output outside section map");

            const u64 sectionEnd =
                context->section.offset +
                context->section.size;

            const std::size_t chunk =
                static_cast<std::size_t>(
                    std::min<u64>(
                        size - position,
                        sectionEnd -
                            (offset + position)));

            if (
                context->section.cryptoType ==
                    3
            ) {
                resetCtr(
                    *context,
                    offset + position);

                aes128CtrCrypt(
                    &context->aes,
                    data + position,
                    data + position,
                    chunk);
            }

            position += chunk;
        }
    }

    void decompressNcz(
        const u8* data,
        std::size_t size)
    {
        ZSTD_inBuffer input{
            data,
            size,
            0
        };

        std::vector<u8> output(
            ZSTD_DStreamOutSize());

        while (input.pos < input.size) {
            check();

            ZSTD_outBuffer out{
                output.data(),
                output.size(),
                0
            };

            const size_t rc =
                ZSTD_decompressStream(
                    zstd_,
                    &out,
                    &input);

            if (ZSTD_isError(rc))
                throw std::runtime_error(
                    std::string(
                        "NSZ decompression failed: ") +
                    ZSTD_getErrorName(rc));

            if (out.pos) {
                encryptNczOutput(
                    output.data(),
                    out.pos,
                    written_);

                writeRaw(
                    output.data(),
                    out.pos);
            }
        }
    }

    void flushNcz(
        bool final)
    {
        if (!zstd_)
            return;

        if (final) {
            std::vector<u8> output(
                ZSTD_DStreamOutSize());

            while (true) {
                ZSTD_inBuffer input{
                    nullptr,
                    0,
                    0
                };

                ZSTD_outBuffer out{
                    output.data(),
                    output.size(),
                    0
                };

                const size_t rc =
                    ZSTD_decompressStream(
                        zstd_,
                        &out,
                        &input);

                if (ZSTD_isError(rc))
                    throw std::runtime_error(
                        "NSZ finalization failed");

                if (out.pos) {
                    encryptNczOutput(
                        output.data(),
                        out.pos,
                        written_);

                    writeRaw(
                        output.data(),
                        out.pos);
                }

                if (rc == 0)
                    break;

                if (out.pos == 0)
                    break;
            }
        }

        ZSTD_freeDCtx(zstd_);
        zstd_ = nullptr;
    }

    Aes128XtsContext xtsDecrypt_{};
    Aes128XtsContext xtsEncrypt_{};
};

class InstallServices {
public:
    InstallServices()
    {
        Result rc =
            ncmInitialize();

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to initialize NCM");

        ncm_ = true;

        rc = nsInitialize();

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to initialize NS");

        ns_ = true;

        rc = smGetService(
            &es_,
            "es");

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to initialize ES");

        rc =
            splCryptoInitialize();

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to initialize SPL crypto");

        splCrypto_ = true;

        rc =
            splInitialize();

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to initialize SPL");

        spl_ = true;

        if (hosversionAtLeast(3,0,0)) {
            rc =
                nsGetApplicationManagerInterface(
                    &nsAppManager_);
        }
        else {
            Service* session =
                nsGetServiceSession_ApplicationManagerInterface();

            if (session) {
                serviceClone(
                    session,
                    &nsAppManager_);
                rc = 0;
            }
            else {
                rc =
                    MAKERESULT(
                        Module_Libnx,
                        LibnxError_NotInitialized);
            }
        }

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to open NS application manager");
    }

    ~InstallServices()
    {
        serviceClose(&nsAppManager_);
        serviceClose(&es_);

        if (spl_)
            splExit();

        if (splCrypto_)
            splCryptoExit();

        if (ns_)
            nsExit();

        if (ncm_)
            ncmExit();
    }

    Service* es()
    {
        return &es_;
    }

    Service* nsAppManager()
    {
        return &nsAppManager_;
    }

private:
    bool ncm_ = false;
    bool ns_ = false;
    bool splCrypto_ = false;
    bool spl_ = false;

    Service es_{};
    Service nsAppManager_{};
};

void importTicket(
    Service* es,
    const void* ticket,
    std::size_t ticketSize,
    const void* cert,
    std::size_t certSize)
{
    const Result rc =
        serviceDispatch(
            es,
            1,
            .buffer_attrs = {
                SfBufferAttr_HipcMapAlias |
                    SfBufferAttr_In,
                SfBufferAttr_HipcMapAlias |
                    SfBufferAttr_In
            },
            .buffers = {
                { ticket, ticketSize },
                { cert, certSize }
            }
        );

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to import ticket");
}

Result nsPushApplicationRecordLocal(
    Service* service,
    u64 titleId,
    u8 lastModifiedEvent,
    ContentStorageRecord* records,
    std::size_t size)
{
    struct {
        u8 event;
        u8 padding[7];
        u64 titleId;
    } input{
        lastModifiedEvent,
        {0},
        titleId
    };

    return serviceDispatchIn(
        service,
        16,
        input,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias |
                SfBufferAttr_In
        },
        .buffers = {
            { records, size }
        }
    );
}

Result nsListApplicationRecordContentMetaLocal(
    Service* service,
    u64 offset,
    u64 titleId,
    void* outputBuffer,
    std::size_t outputSize,
    u32* entriesRead)
{
    struct {
        u64 offset;
        u64 titleId;
    } input{
        offset,
        titleId
    };

    struct {
        u32 entriesRead;
    } output{};

    const Result rc =
        serviceDispatchInOut(
            service,
            17,
            input,
            output,
            .buffer_attrs = {
                SfBufferAttr_HipcMapAlias |
                    SfBufferAttr_Out
            },
            .buffers = {
                {
                    outputBuffer,
                    outputSize
                }
            }
        );

    if (
        R_SUCCEEDED(rc) &&
        entriesRead
    ) {
        *entriesRead =
            output.entriesRead;
    }

    return rc;
}

Result nsDeleteApplicationRecordLocal(
    Service* service,
    u64 titleId)
{
    struct {
        u64 titleId;
    } input{
        titleId
    };

    return serviceDispatchIn(
        service,
        27,
        input);
}

std::vector<u8> readInstalledCnmt(
    NcmContentStorage* storage,
    const NcmContentId& id)
{
    char path[FS_MAX_PATH]{};

    Result rc =
        ncmContentStorageGetPath(
            storage,
            path,
            sizeof(path),
            &id);

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to resolve installed CNMT path");

    FsFileSystem fs{};

    rc =
        fsOpenFileSystemWithId(
            &fs,
            0,
            FsFileSystemType_ContentMeta,
            path,
            FsContentAttributes_None);

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to mount CNMT NCA");

    FsDir dir{};

    rc =
        fsFsOpenDirectory(
            &fs,
            "/",
            FsDirOpenMode_ReadFiles,
            &dir);

    if (R_FAILED(rc)) {
        fsFsClose(&fs);
        throw std::runtime_error(
            "Failed to read CNMT NCA");
    }

    s64 count = 0;

    rc =
        fsDirGetEntryCount(
            &dir,
            &count);

    if (
        R_FAILED(rc) ||
        count <= 0 ||
        count > 4096
    ) {
        fsDirClose(&dir);
        fsFsClose(&fs);

        throw std::runtime_error(
            "CNMT file not found");
    }

    std::vector<FsDirectoryEntry> rows(
        static_cast<std::size_t>(count));

    s64 readCount = 0;

    rc =
        fsDirRead(
            &dir,
            &readCount,
            rows.size(),
            rows.data());

    fsDirClose(&dir);

    if (R_FAILED(rc)) {
        fsFsClose(&fs);

        throw std::runtime_error(
            "Failed to list CNMT files");
    }

    std::string cnmtName;

    for (
        s64 i = 0;
        i < readCount;
        ++i
    ) {
        const std::string name =
            rows[
                static_cast<std::size_t>(i)
            ].name;

        if (
            endsWithInsensitive(
                name,
                ".cnmt")
        ) {
            cnmtName = name;
            break;
        }
    }

    if (cnmtName.empty()) {
        fsFsClose(&fs);

        throw std::runtime_error(
            "CNMT metadata file missing");
    }

    FsFile file{};

    rc =
        fsFsOpenFile(
            &fs,
            ("/" + cnmtName).c_str(),
            FsOpenMode_Read,
            &file);

    if (R_FAILED(rc)) {
        fsFsClose(&fs);

        throw std::runtime_error(
            "Failed to open CNMT metadata");
    }

    s64 size = 0;

    rc =
        fsFileGetSize(
            &file,
            &size);

    if (
        R_FAILED(rc) ||
        size <= 0 ||
        size > 64 * 1024 * 1024
    ) {
        fsFileClose(&file);
        fsFsClose(&fs);

        throw std::runtime_error(
            "Invalid CNMT metadata size");
    }

    std::vector<u8> data(
        static_cast<std::size_t>(size));

    u64 bytesRead = 0;

    rc =
        fsFileRead(
            &file,
            0,
            data.data(),
            data.size(),
            FsReadOption_None,
            &bytesRead);

    fsFileClose(&file);
    fsFsClose(&fs);

    if (
        R_FAILED(rc) ||
        bytesRead != data.size()
    ) {
        throw std::runtime_error(
            "Failed to read CNMT metadata");
    }

    return data;
}

struct ParsedCnmt {
    NcmContentMetaKey key{};
    std::vector<NcmContentInfo> content;
    std::vector<u8> installMeta;
};

ParsedCnmt parseCnmt(
    const std::vector<u8>& bytes,
    const NcmContentInfo& cnmtInfo)
{
    if (
        bytes.size() <
            sizeof(PackagedContentMetaHeader)
    ) {
        throw std::runtime_error(
            "CNMT metadata too small");
    }

    const auto* header =
        reinterpret_cast<
            const PackagedContentMetaHeader*>(
                bytes.data());

    const std::size_t infosOffset =
        sizeof(PackagedContentMetaHeader) +
        header->extendedHeaderSize;

    const std::size_t infosSize =
        static_cast<std::size_t>(
            header->contentCount) *
        sizeof(NcmPackagedContentInfo);

    if (
        infosOffset >
            bytes.size() ||
        infosSize >
            bytes.size() -
                infosOffset
    ) {
        throw std::runtime_error(
            "CNMT metadata truncated");
    }

    ParsedCnmt out;

    out.key.id =
        header->titleId;

    out.key.version =
        header->version;

    out.key.type =
        header->type;

    out.key.install_type =
        NcmContentInstallType_Full;

    const auto* packaged =
        reinterpret_cast<
            const NcmPackagedContentInfo*>(
                bytes.data() +
                infosOffset);

    for (
        u16 i = 0;
        i < header->contentCount;
        ++i
    ) {
        if (
            packaged[i].info.content_type <=
                NcmContentType_LegalInformation
        ) {
            out.content.push_back(
                packaged[i].info);
        }
    }

    NcmContentMetaHeader meta{};

    meta.extended_header_size =
        header->extendedHeaderSize;

    meta.content_count =
        static_cast<u16>(
            out.content.size() + 1);

    meta.content_meta_count =
        header->contentMetaCount;

    meta.attributes =
        header->attributes;

    meta.storage_id = 0;

    const std::size_t baseSize =
        sizeof(meta) +
        meta.extended_header_size +
        static_cast<std::size_t>(
            meta.content_count) *
            sizeof(NcmContentInfo);

    std::size_t extraSize = 0;

    if (
        header->type ==
            NcmContentMetaType_Patch &&
        header->extendedHeaderSize >=
            sizeof(NcmPatchMetaExtendedHeader)
    ) {
        const auto* ext =
            reinterpret_cast<
                const NcmPatchMetaExtendedHeader*>(
                    bytes.data() +
                    sizeof(
                        PackagedContentMetaHeader));

        extraSize =
            ext->extended_data_size;
    }
    else if (
        header->type ==
            NcmContentMetaType_DataPatch &&
        header->extendedHeaderSize >=
            sizeof(NcmDataPatchMetaExtendedHeader)
    ) {
        const auto* ext =
            reinterpret_cast<
                const NcmDataPatchMetaExtendedHeader*>(
                    bytes.data() +
                    sizeof(
                        PackagedContentMetaHeader));

        extraSize =
            ext->extended_data_size;
    }

    out.installMeta.assign(
        baseSize + extraSize,
        0);

    std::size_t cursor = 0;

    std::memcpy(
        out.installMeta.data() +
            cursor,
        &meta,
        sizeof(meta));

    cursor += sizeof(meta);

    if (meta.extended_header_size) {
        std::memcpy(
            out.installMeta.data() +
                cursor,
            bytes.data() +
                sizeof(
                    PackagedContentMetaHeader),
            meta.extended_header_size);

        cursor +=
            meta.extended_header_size;
    }

    std::memcpy(
        out.installMeta.data() +
            cursor,
        &cnmtInfo,
        sizeof(cnmtInfo));

    cursor += sizeof(cnmtInfo);

    for (const auto& info :
         out.content) {
        std::memcpy(
            out.installMeta.data() +
                cursor,
            &info,
            sizeof(info));

        cursor += sizeof(info);
    }

    return out;
}

void installApplicationRecord(
    Service* nsAppManager,
    const ParsedCnmt& cnmt,
    NcmStorageId storageId)
{
    const u64 baseId =
        baseTitleId(
            cnmt.key.id,
            cnmt.key.type);

    s32 count = 0;

    Result rc =
        nsCountApplicationContentMeta(
            baseId,
            &count);

    if (
        R_FAILED(rc) &&
        rc != 0x410
    ) {
        throw std::runtime_error(
            "Failed to read application record");
    }

    std::vector<ContentStorageRecord>
        records;

    if (count > 0) {
        records.resize(
            static_cast<std::size_t>(
                count));

        u32 read = 0;

        rc =
            nsListApplicationRecordContentMetaLocal(
                nsAppManager,
                0,
                baseId,
                records.data(),
                records.size() *
                    sizeof(
                        ContentStorageRecord),
                &read);

        if (R_FAILED(rc))
            throw std::runtime_error(
                "Failed to list application metadata");

        records.resize(
            std::min<std::size_t>(
                records.size(),
                read));
    }

    ContentStorageRecord current{};

    current.metaRecord =
        cnmt.key;

    current.storageId =
        static_cast<u64>(
            storageId);

    records.push_back(
        current);

    nsDeleteApplicationRecordLocal(
        nsAppManager,
        baseId);

    rc =
        nsPushApplicationRecordLocal(
            nsAppManager,
            baseId,
            3,
            records.data(),
            records.size() *
                sizeof(
                    ContentStorageRecord));

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to register application");
}

// Fetch a large NCA using four independent HTTP range connections.  Only
// four 8 MiB chunks can be outstanding at once, and the consumer writes each
// completed chunk in ascending file order. This preserves NCZ stream ordering.
void installEntryParallel(
    Package& package,
    const PackageEntry& entry,
    const NcmContentId& contentId,
    NcmContentStorage* storage,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel,
    int basePercent,
    int spanPercent)
{
    constexpr std::size_t connections = 4;
    constexpr u64 chunkSize = 8ULL * 1024 * 1024;
    const u64 count = 1 + (entry.size - 1) / chunkSize;

    NcaOutput output(
        storage, contentId, cancel, progress, basePercent, spanPercent);

    struct Slot {
        std::vector<u8> data;
        bool ready = false;
    };
    std::array<Slot, connections> slots;
    std::mutex mutex;
    std::condition_variable changed;
    u64 nextRequest = 0;
    u64 nextWrite = 0;
    std::exception_ptr networkError;
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    workers.reserve(connections);

    const auto worker = [&]() {
        try {
            // A libcurl easy handle must never be shared between threads.
            auto source = package.source->clone();
            while (!stop.load()) {
                u64 index;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&]() {
                        return stop.load() ||
                            (cancel && cancel->load()) ||
                            nextRequest == count ||
                            nextRequest - nextWrite < connections;
                    });
                    if (stop.load() || (cancel && cancel->load())) {
                        break;
                    }
                    if (nextRequest == count) {
                        break;
                    }
                    index = nextRequest++;
                }

                const u64 localOffset = index * chunkSize;
                const auto length = static_cast<std::size_t>(
                    std::min<u64>(chunkSize, entry.size - localOffset));
                std::vector<u8> bytes(length);
                std::size_t received = 0;
                source->streamExact(
                    entry.offset + localOffset,
                    length,
                    [&](const u8* data, std::size_t size) {
                        if (size > bytes.size() - received) {
                            throw std::runtime_error("Invalid HTTP range length");
                        }
                        std::memcpy(bytes.data() + received, data, size);
                        received += size;
                        progress.addNetworkBytes(size);
                    },
                    &stop);

                if (stop.load()) break;

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto& slot = slots[index % connections];
                    slot.data = std::move(bytes);
                    slot.ready = true;
                }
                changed.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!networkError) networkError = std::current_exception();
            }
            stop.store(true);
            changed.notify_all();
        }
    };

    try {
        for (std::size_t i = 0; i < connections; ++i) {
            workers.emplace_back(worker);
        }

        for (u64 index = 0; index < count; ++index) {
            std::vector<u8> bytes;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&]() {
                    return slots[index % connections].ready ||
                        stop.load() || (cancel && cancel->load());
                });

                if (cancel && cancel->load()) {
                    throw std::runtime_error("Install cancelled");
                }
                if (networkError) std::rethrow_exception(networkError);
                if (!slots[index % connections].ready) {
                    throw std::runtime_error("Parallel HTTP transfer stopped");
                }

                auto& slot = slots[index % connections];
                bytes = std::move(slot.data);
                slot.ready = false;
                ++nextWrite;
            }
            changed.notify_all();

            output.write(bytes.data(), bytes.size());
            progress.addTransferBytes(bytes.size());
        }

        // All network calls must finish before closing the content storage.
        for (auto& thread : workers) thread.join();
        output.finish();
    } catch (...) {
        stop.store(true);
        changed.notify_all();
        for (auto& thread : workers) {
            if (thread.joinable()) thread.join();
        }
        throw;
    }
}

void installEntry(
    Package& package,
    const PackageEntry& entry,
    const NcmContentId& contentId,
    NcmContentStorage* storage,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel,
    int basePercent,
    int spanPercent)
{
    // Concurrent ranged GETs provide better throughput on servers that
    // limit per-connection speed. Small entries use the original pipeline.
    if (entry.size >= 32ULL * 1024 * 1024) {
        installEntryParallel(package, entry, contentId, storage,
                             progress, cancel, basePercent, spanPercent);
        return;
    }

    // Keep libcurl and content-storage writes on separate threads. Network
    // callbacks never block on ncmContentStorageWritePlaceHolder directly.
    // The four-block queue caps buffered data at roughly 4 MiB.
    constexpr std::size_t streamBufferSize = 4 * 1024 * 1024;
    constexpr std::size_t maxQueuedBlocks = 2;

    NcaOutput output(
        storage, contentId, cancel, progress, basePercent, spanPercent);

    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque<std::vector<u8>> queue;
    std::exception_ptr downloadError;
    bool downloadDone = false;
    std::atomic<bool> stopDownload{false};

    std::thread downloader([&]() {
        try {
            std::vector<u8> buffer;
            buffer.reserve(streamBufferSize);

            const auto enqueue = [&]() {
                if (buffer.empty()) return;

                std::unique_lock<std::mutex> lock(queueMutex);
                queueChanged.wait(lock, [&]() {
                    return stopDownload.load() ||
                        (cancel && cancel->load()) ||
                        queue.size() < maxQueuedBlocks;
                });

                if (stopDownload.load() || (cancel && cancel->load())) {
                    throw std::runtime_error("Install cancelled");
                }

                queue.emplace_back(std::move(buffer));
                lock.unlock();
                queueChanged.notify_all();

                buffer = std::vector<u8>{};
                buffer.reserve(streamBufferSize);
            };

            package.source->streamExact(
                entry.offset,
                entry.size,
                [&](const u8* data, std::size_t bytes) {
                    progress.addNetworkBytes(static_cast<std::uint64_t>(bytes));
                    while (bytes) {
                        if (stopDownload.load() || (cancel && cancel->load())) {
                            throw std::runtime_error("Install cancelled");
                        }

                        const auto take = std::min(
                            streamBufferSize - buffer.size(), bytes);
                        buffer.insert(buffer.end(), data, data + take);
                        data += take;
                        bytes -= take;

                        if (buffer.size() == streamBufferSize) enqueue();
                    }
                },
                &stopDownload);

            enqueue();
        } catch (...) {
            std::lock_guard<std::mutex> lock(queueMutex);
            downloadError = std::current_exception();
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            downloadDone = true;
        }
        queueChanged.notify_all();
    });

    try {
        while (true) {
            std::vector<u8> chunk;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueChanged.wait(lock, [&]() {
                    return !queue.empty() || downloadDone ||
                        (cancel && cancel->load());
                });

                if (cancel && cancel->load()) {
                    throw std::runtime_error("Install cancelled");
                }

                if (queue.empty()) {
                    if (downloadError) std::rethrow_exception(downloadError);
                    break;
                }

                chunk = std::move(queue.front());
                queue.pop_front();
            }
            queueChanged.notify_all();

            // The main installer thread exclusively owns the Switch storage
            // handle; count bytes once they are written successfully.
            output.write(chunk.data(), chunk.size());
            progress.addTransferBytes(static_cast<std::uint64_t>(chunk.size()));
        }

        downloader.join();
        output.finish();
    } catch (...) {
        // Also interrupts the transfer's progress/write callbacks, so the
        // worker can exit even if the storage writer fails.
        stopDownload.store(true);
        queueChanged.notify_all();
        if (downloader.joinable()) downloader.join();
        throw;
    }
}

void installPackageCloud(
    const std::string& url,
    const std::string& name,
    u64 knownSize,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancel)
{
    InstallServices services;

    progress.set(
        "Installing",
        0,
        "Opening cloud package");

    Package package =
        openPackage(
            url,
            name,
            cancel,
            knownSize);

    if (package.entries.empty())
        throw std::runtime_error(
            "Package contains no files");

    std::uint64_t transferTotal = 0;

    for (const auto& entry :
         package.entries) {
        if (
            endsWithInsensitive(
                entry.name,
                ".nca") ||
            endsWithInsensitive(
                entry.name,
                ".ncz")
        ) {
            transferTotal +=
                entry.size;
        }
    }

    if (!transferTotal)
        transferTotal =
            knownSize;

    progress.beginTransfer(
        transferTotal);

    NcmContentStorage storage{};

    Result rc =
        ncmOpenContentStorage(
            &storage,
            NcmStorageId_SdCard);

    if (R_FAILED(rc))
        throw std::runtime_error(
            "Failed to open SD content storage");

    struct StorageCloser {
        NcmContentStorage* storage;

        ~StorageCloser() {
            if (storage)
                ncmContentStorageClose(
                    storage);
        }
    } closer{&storage};

    const auto cnmtEntries =
        findEntriesEndingWith(
            package,
            ".cnmt.nca");

    auto cnmtCompressed =
        findEntriesEndingWith(
            package,
            ".cnmt.ncz");

    std::vector<const PackageEntry*>
        allCnmt =
            cnmtEntries;

    allCnmt.insert(
        allCnmt.end(),
        cnmtCompressed.begin(),
        cnmtCompressed.end());

    if (allCnmt.empty())
        throw std::runtime_error(
            "Package contains no CNMT NCA");

    std::vector<ParsedCnmt> parsed;
    parsed.reserve(
        allCnmt.size());

    for (
        std::size_t i = 0;
        i < allCnmt.size();
        ++i
    ) {
        const auto& entry =
            *allCnmt[i];

        const NcmContentId id =
            contentIdFromName(
                entry.name);

        progress.set(
            "Installing",
            0,
            "Preparing " +
                entry.name);

        installEntry(
            package,
            entry,
            id,
            &storage,
            progress,
            cancel,
            0,
            10);

        auto cnmtBytes =
            readInstalledCnmt(
                &storage,
                id);

        NcmContentInfo cnmtInfo{};

        cnmtInfo.content_id = id;

        ncmU64ToContentInfoSize(
            entry.size,
            &cnmtInfo);

        cnmtInfo.content_type =
            NcmContentType_Meta;

        parsed.push_back(
            parseCnmt(
                cnmtBytes,
                cnmtInfo));

    }

    std::vector<
        std::pair<
            const PackageEntry*,
            NcmContentId
        >
    > contentToInstall;

    for (const auto& cnmt :
         parsed) {
        for (const auto& info :
             cnmt.content) {
            const std::string id =
                contentIdString(
                    info.content_id);

            const PackageEntry* entry =
                findEntry(
                    package,
                    id + ".nca");

            if (!entry)
                entry =
                    findEntry(
                        package,
                        id + ".ncz");

            if (!entry)
                throw std::runtime_error(
                    "Required NCA missing from package: " +
                    id);

            contentToInstall.push_back({
                entry,
                info.content_id
            });
        }
    }

    const std::size_t totalContent =
        std::max<std::size_t>(
            contentToInstall.size(),
            1);

    for (
        std::size_t i = 0;
        i < contentToInstall.size();
        ++i
    ) {
        const int base =
            10 +
            static_cast<int>(
                i * 85 /
                totalContent);

        const int span =
            static_cast<int>(
                85 /
                totalContent);

        progress.set(
            "Installing",
            base,
            contentToInstall[i]
                .first->name);

        installEntry(
            package,
            *contentToInstall[i].first,
            contentToInstall[i].second,
            &storage,
            progress,
            cancel,
            base,
            std::max(span, 1));
    }

    const auto tickets =
        findEntriesEndingWith(
            package,
            ".tik");

    const auto certs =
        findEntriesEndingWith(
            package,
            ".cert");

    const std::size_t pairCount =
        std::min(
            tickets.size(),
            certs.size());

    for (
        std::size_t i = 0;
        i < pairCount;
        ++i
    ) {
        checkCancelled();

        auto ticket =
            readBytes(
                *package.source,
                tickets[i]->offset,
                static_cast<std::size_t>(
                    tickets[i]->size));

        auto cert =
            readBytes(
                *package.source,
                certs[i]->offset,
                static_cast<std::size_t>(
                    certs[i]->size));

        try {
            importTicket(
                services.es(),
                ticket.data(),
                ticket.size(),
                cert.data(),
                cert.size());
        }
        catch (...) {
            // Ticket import can legitimately be unnecessary for
            // ticketless content. Do not roll back an otherwise
            // complete content installation.
        }
    }

    // Only expose the application to HOME after every required NCA has
    // completed and ticket processing has finished. A failed or cancelled
    // transfer must never publish an incomplete application record.
    checkCancelled();
    progress.set("Installing", 98, "Registering content metadata");

    for (const auto& cnmt : parsed) {
        checkCancelled();

        NcmContentMetaDatabase database{};
        rc = ncmOpenContentMetaDatabase(
            &database, NcmStorageId_SdCard);
        if (R_FAILED(rc))
            throw std::runtime_error("Failed to open content metadata database");

        rc = ncmContentMetaDatabaseSet(
            &database,
            &cnmt.key,
            reinterpret_cast<const NcmContentMetaHeader*>(cnmt.installMeta.data()),
            cnmt.installMeta.size());
        if (R_SUCCEEDED(rc))
            rc = ncmContentMetaDatabaseCommit(&database);
        ncmContentMetaDatabaseClose(&database);
        if (R_FAILED(rc))
            throw std::runtime_error("Failed to register content metadata");
    }

    checkCancelled();
    progress.set("Installing", 99, "Registering application");
    for (const auto& cnmt : parsed) {
        checkCancelled();
        installApplicationRecord(
            services.nsAppManager(), cnmt, NcmStorageId_SdCard);
    }

    progress.set(
        "Installing",
        100,
        "Finalizing");
}


} // namespace

void InstallProgress::set(
    const std::string& newStage,
    int newPercent,
    const std::string& newDetail)
{
    percent.store(
        std::clamp(
            newPercent,
            0,
            100));

    {
        std::lock_guard<std::mutex>
            lock(mutex);

        stage = newStage;

        if (!newDetail.empty())
            detail = newDetail;
    }
}

void InstallProgress::beginTransfer(
    std::uint64_t totalBytes)
{
    bytesDone.store(0);
    bytesTotal.store(totalBytes);
    bytesPerSecond.store(0);
    networkBytesPerSecond.store(0);
    networkBytesDone.store(0);

    std::lock_guard<std::mutex>
        lock(mutex);

    transferSampleStarted_ =
        std::chrono::steady_clock::now();

    transferSampleBytes_ = 0;
    networkSampleStarted_ = transferSampleStarted_;
    networkSampleBytes_ = 0;
}

void InstallProgress::addTransferBytes(
    std::uint64_t bytes)
{
    if (!bytes)
        return;

    const std::uint64_t done =
        bytesDone.fetch_add(bytes) +
        bytes;

    const std::uint64_t total =
        bytesTotal.load();

    if (total) {
        percent.store(
            std::clamp(
                static_cast<int>(
                    done * 100 /
                    total),
                0,
                99));
    }

    std::lock_guard<std::mutex>
        lock(mutex);

    const auto now =
        std::chrono::steady_clock::now();

    const auto elapsedMs =
        std::chrono::duration_cast<
            std::chrono::milliseconds>(
                now -
                transferSampleStarted_)
            .count();

    if (elapsedMs >= 1000) {
        const std::uint64_t delta =
            done -
            transferSampleBytes_;

        bytesPerSecond.store(
            static_cast<std::uint64_t>(
                delta * 1000 /
                static_cast<std::uint64_t>(
                    elapsedMs)));

        transferSampleBytes_ =
            done;

        transferSampleStarted_ =
            now;
    }
}

void InstallProgress::addNetworkBytes(std::uint64_t bytes)
{
    if (!bytes) return;
    const auto done = networkBytesDone.fetch_add(bytes) + bytes;
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<
        std::chrono::milliseconds>(now - networkSampleStarted_).count();
    if (elapsedMs >= 1000) {
        const auto delta = done - networkSampleBytes_;
        networkBytesPerSecond.store(
            delta * 1000 / static_cast<std::uint64_t>(elapsedMs));
        networkSampleBytes_ = done;
        networkSampleStarted_ = now;
    }
}

void InstallProgress::snapshot(
    std::string& outStage,
    int& outPercent,
    std::string& outDetail) const
{
    outPercent =
        percent.load();

    std::lock_guard<std::mutex>
        lock(mutex);

    outStage = stage;
    outDetail = detail;
}

void InstallProgress::snapshotTransfer(
    std::uint64_t& outDone,
    std::uint64_t& outTotal,
    std::uint64_t& outBytesPerSecond,
    std::uint64_t& outNetworkBytesPerSecond) const
{
    outDone =
        bytesDone.load();

    outTotal =
        bytesTotal.load();

    outBytesPerSecond =
        bytesPerSecond.load();
    outNetworkBytesPerSecond = networkBytesPerSecond.load();
}

InstallResult runInstallJob(
    const DebridConfig& config,
    const InstallJob& job,
    const std::string& cacheDirectory,
    InstallProgress& progress,
    const std::shared_ptr<std::atomic<bool>>& cancelRequested)
{
    InstallResult result;

    progress.running.store(true);
    progress.set(
        "Installing",
        0,
        job.file.name);

    (void)cacheDirectory;

    try {
        if (
            config.service ==
                DebridService::None ||
            config.apiKey.empty()
        ) {
            throw std::runtime_error(
                "Debrid authorization is required");
        }

        if (job.remoteId.empty())
            throw std::runtime_error(
                "Debrid torrent ID is missing");

        auto backend =
            createDebridBackend(config);

        if (!backend)
            throw std::runtime_error(
                "Unable to initialize debrid service");

        const std::string url =
            backend->downloadUrl(
                job.remoteId,
                job.file);

        if (url.empty())
            throw std::runtime_error(
                "Debrid did not return a download URL");

        if (
            cancelRequested &&
            cancelRequested->load()
        ) {
            throw std::runtime_error(
                "Install cancelled");
        }

        progress.set(
            "Installing",
            0,
            job.file.name);

        gInstallCancel =
            cancelRequested.get();

        installPackageCloud(
            url,
            job.file.name,
            job.file.size,
            progress,
            cancelRequested);

        gInstallCancel = nullptr;

        progress.set(
            "Completed",
            100,
            job.file.name);

        result.success = true;
        result.message =
            "Install completed";
    }
    catch (const std::exception& e) {
        gInstallCancel = nullptr;

        result.cancelled =
            cancelRequested &&
            cancelRequested->load();

        result.message =
            result.cancelled
                ? "Install cancelled"
                : e.what();

        progress.set(
            result.cancelled
                ? "Cancelled"
                : "Failed",
            progress.percent.load(),
            result.message);
    }

    progress.running.store(false);
    return result;
}

} // namespace sgb
