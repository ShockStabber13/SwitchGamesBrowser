#include "provider.hpp"
#include "provider_diagnostics.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* output = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    output->append(ptr, bytes);
    return bytes;
}

std::string httpGet(const std::string& url)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("curl_easy_init failed");

    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 Chrome/114.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode result = curl_easy_perform(curl);
    long diagnosticHttpStatus = 0;
    char* diagnosticEffectiveUrl = nullptr;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &diagnosticHttpStatus
    );

    curl_easy_getinfo(
        curl,
        CURLINFO_EFFECTIVE_URL,
        &diagnosticEffectiveUrl
    );

    const std::string diagnosticFinalUrl =
        diagnosticEffectiveUrl
            ? diagnosticEffectiveUrl
            : url;

    sgb::recordProviderHttp(
        diagnosticHttpStatus,
        diagnosticFinalUrl,
        response.size(),
        result == CURLE_OK
            ? ""
            : curl_easy_strerror(result)
    );
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK)
        throw std::runtime_error(curl_easy_strerror(result));
    return response;
}

std::string urlEncode(const std::string& value)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return value;
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string result = encoded ? encoded : value;
    if (encoded)
        curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

std::string trim(const std::string& s)
{
    const size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos)
        return "";
    const size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string decodeHtmlEntities(std::string s)
{
    const std::pair<const char*, const char*> replacements[] = {
        {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"},
        {"&lt;", "<"}, {"&gt;", ">"}
    };
    for (const auto& replacement : replacements) {
        size_t pos = 0;
        while ((pos = s.find(replacement.first, pos)) != std::string::npos) {
            s.replace(pos, std::char_traits<char>::length(replacement.first), replacement.second);
            pos += std::char_traits<char>::length(replacement.second);
        }
    }
    return s;
}

std::string stripTags(std::string text)
{
    static const std::regex tagRe(R"(<[^>]+>)");
    text = std::regex_replace(text, tagRe, "");
    return trim(decodeHtmlEntities(text));
}

std::string extractInfoHash(const std::string& magnet)
{
    static const std::regex re(
        R"(xt=urn:btih:([0-9a-fA-F]{40}|[A-Za-z2-7]{32}))",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(magnet, match, re))
        return "";
    std::string hash = match[1];
    std::transform(hash.begin(), hash.end(), hash.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return hash;
}

std::string extractMagnetFromHtml(const std::string& html)
{
    static const std::regex re(R"(magnet:[^\"'<>\s]+)", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(html, match, re))
        return "";
    return decodeHtmlEntities(match[0]);
}

std::vector<std::string> splitByTag(const std::string& html, const std::string& tag)
{
    std::vector<std::string> parts;
    const std::string openTag = "<" + tag;
    const std::string closeTag = "</" + tag + ">";
    size_t pos = 0;
    while ((pos = html.find(openTag, pos)) != std::string::npos) {
        size_t endPos = html.find(closeTag, pos);
        if (endPos == std::string::npos)
            break;
        endPos += closeTag.size();
        parts.push_back(html.substr(pos, endPos - pos));
        pos = endPos;
    }
    return parts;
}

struct AnchorInfo {
    std::string href;
    std::string text;
};

std::vector<AnchorInfo> extractAnchors(const std::string& html)
{
    std::vector<AnchorInfo> anchors;
    static const std::regex re(
        R"(<a[^>]*href=["']([^"']*)["'][^>]*>([\s\S]*?)</a>)",
        std::regex::icase);

    auto begin = std::sregex_iterator(html.begin(), html.end(), re);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        AnchorInfo anchor;
        anchor.href = decodeHtmlEntities((*it)[1]);
        anchor.text = stripTags((*it)[2]);
        anchors.push_back(std::move(anchor));
    }
    return anchors;
}

bool hasClassToken(const std::string& html, const std::string& token)
{
    static const std::regex classRe(R"(class=["']([^"']*)["'])", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(html, match, classRe))
        return false;

    const std::string classes = match[1];
    const std::regex tokenRe("(^|\\s)" + token + "(\\s|$)", std::regex::icase);
    return std::regex_search(classes, tokenRe);
}

} // namespace

class LimeTorrentsProvider final : public sgb::Provider {
public:
    std::string id() const override { return "limetorrents"; }

    std::vector<sgb::ProviderResult> search(const std::string& query) override
    {
        std::vector<sgb::ProviderResult> results;
        if (trim(query).empty())
            return results;

        std::unordered_set<std::string> seenHashes;
        int page = 1;
        int emptyPageCount = 0;

        while (true) {
            const std::string url =
                "https://www.limetorrents.fun/search/all/" +
                urlEncode(query) + "//" + std::to_string(page) + "/";

            std::string html;
            try {
                html = httpGet(url);
            } catch (...) {
                break;
            }

            const auto rows = splitByTag(html, "tr");
            bool hadNewResults = false;

            for (const auto& row : rows) {
                if (row.find("tt-name") == std::string::npos)
                    continue;

                const size_t marker = row.find("tt-name");
                const size_t divStart = row.rfind("<div", marker);
                const size_t divEndRaw = row.find("</div>", marker);
                if (divStart == std::string::npos || divEndRaw == std::string::npos)
                    continue;

                const std::string container = row.substr(
                    divStart, divEndRaw + 6 - divStart);
                const auto anchors = extractAnchors(container);

                AnchorInfo selected;
                for (const auto& anchor : anchors) {
                    if (anchor.text.empty() || anchor.href.empty())
                        continue;
                    if (anchor.href.rfind("magnet:", 0) == 0 ||
                        anchor.href.rfind("javascript:", 0) == 0 ||
                        anchor.href == "#")
                        continue;
                    selected = anchor;
                }

                if (selected.text.empty() || selected.href.empty())
                    continue;

                std::string detailsUrl = selected.href;
                if (detailsUrl[0] == '/')
                    detailsUrl = "https://www.limetorrents.fun" + detailsUrl;

                std::string magnet;
                try {
                    magnet = extractMagnetFromHtml(httpGet(detailsUrl));
                } catch (...) {
                    continue;
                }

                const std::string infoHash = extractInfoHash(magnet);
                if (magnet.empty() || infoHash.empty() ||
                    !seenHashes.insert(infoHash).second)
                    continue;

                sgb::ProviderResult result;
                result.title = selected.text;
                result.magnet = magnet;
                result.infoHash = infoHash;
                results.push_back(std::move(result));
                hadNewResults = true;
            }

            if (!hadNewResults) {
                if (++emptyPageCount >= 3)
                    break;
            } else {
                emptyPageCount = 0;
            }
            ++page;
        }

        return results;
    }
};
SGB_REGISTER_PROVIDER(LimeTorrentsProvider);