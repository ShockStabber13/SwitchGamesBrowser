#pragma once

#include "provider_diagnostics.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>

namespace sgb_api {

inline size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* output = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    output->append(ptr, bytes);
    return bytes;
}

inline std::string httpGet(
    const std::string& url,
    const char* accept = "*/*")
{
    CURL* curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("curl_easy_init failed");

    std::string response;
    curl_slist* headers = nullptr;

    const std::string acceptHeader =
        std::string("Accept: ") + accept;

    headers = curl_slist_append(headers, acceptHeader.c_str());
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchGamesBrowser/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (url.rfind("https://", 0) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    const CURLcode code = curl_easy_perform(curl);

    long httpStatus = 0;
    char* effectiveUrl = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);

    const std::string finalUrl = effectiveUrl ? effectiveUrl : url;

    sgb::recordProviderHttp(
        httpStatus,
        finalUrl,
        response.size(),
        code == CURLE_OK ? "" : curl_easy_strerror(code));

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK)
        throw std::runtime_error(curl_easy_strerror(code));

    if (httpStatus < 200 || httpStatus >= 300)
        throw std::runtime_error("HTTP " + std::to_string(httpStatus));

    return response;
}

inline std::string urlEncode(const std::string& value)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return value;

    char* encoded = curl_easy_escape(
        curl,
        value.c_str(),
        static_cast<int>(value.size()));

    std::string result = encoded ? encoded : value;
    if (encoded)
        curl_free(encoded);

    curl_easy_cleanup(curl);
    return result;
}

inline std::string trim(const std::string& value)
{
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return "";

    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

inline std::string lowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

inline bool validInfoHash(const std::string& hash)
{
    if (hash.size() == 40) {
        for (unsigned char c : hash) {
            if (!std::isxdigit(c))
                return false;
        }
        return true;
    }

    if (hash.size() == 32) {
        for (unsigned char c : hash) {
            const char u = static_cast<char>(std::toupper(c));
            if (!((u >= 'A' && u <= 'Z') || (u >= '2' && u <= '7')))
                return false;
        }
        return true;
    }

    return false;
}

inline std::string hashFromMagnet(const std::string& magnet)
{
    static const std::regex re(
        R"(xt=urn:btih:([0-9a-fA-F]{40}|[A-Za-z2-7]{32}))",
        std::regex::icase);

    std::smatch match;
    if (!std::regex_search(magnet, match, re))
        return "";

    return lowerAscii(match[1].str());
}

inline std::string magnetFromHash(
    const std::string& hash,
    const std::string& title)
{
    return "magnet:?xt=urn:btih:" + hash +
        "&dn=" + urlEncode(title);
}

inline std::string xmlDecode(std::string value)
{
    struct Pair { const char* from; const char* to; };
    static const Pair replacements[] = {
        {"&amp;", "&"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&#39;", "'"},
        {"&lt;", "<"},
        {"&gt;", ">"}
    };

    for (const auto& replacement : replacements) {
        size_t pos = 0;
        while ((pos = value.find(replacement.from, pos)) != std::string::npos) {
            value.replace(
                pos,
                std::char_traits<char>::length(replacement.from),
                replacement.to);
            pos += std::char_traits<char>::length(replacement.to);
        }
    }

    return value;
}

inline std::string stripCdata(std::string value)
{
    value = trim(value);
    const std::string start = "<![CDATA[";
    const std::string end = "]] >"; // not used; keep parser simple below
    (void)end;

    if (value.rfind(start, 0) == 0 && value.size() >= start.size() + 3) {
        const size_t close = value.rfind("]]>");
        if (close != std::string::npos)
            value = value.substr(start.size(), close - start.size());
    }
    return trim(value);
}

inline std::string tagValue(
    const std::string& xml,
    const std::string& tag)
{
    const std::string open = "<" + tag;
    size_t start = xml.find(open);
    if (start == std::string::npos)
        return "";

    start = xml.find('>', start);
    if (start == std::string::npos)
        return "";
    ++start;

    const size_t end = xml.find("</" + tag + ">", start);
    if (end == std::string::npos)
        return "";

    return xmlDecode(stripCdata(xml.substr(start, end - start)));
}

inline std::string torznabAttr(
    const std::string& xml,
    const std::string& name)
{
    const std::regex re(
        R"(<(?:newznab|torznab):attr\b[^>]*\bname=[\"'])" +
        name +
        R"([\"'][^>]*\bvalue=[\"']([^\"']*)[\"'][^>]*/?>)",
        std::regex::icase);

    std::smatch match;
    if (std::regex_search(xml, match, re))
        return xmlDecode(match[1].str());

    const std::regex reverseRe(
        R"(<(?:newznab|torznab):attr\b[^>]*\bvalue=[\"']([^\"']*)[\"'][^>]*\bname=[\"'])" +
        name +
        R"([\"'][^>]*/?>)",
        std::regex::icase);

    if (std::regex_search(xml, match, reverseRe))
        return xmlDecode(match[1].str());

    return "";
}

} // namespace sgb_api