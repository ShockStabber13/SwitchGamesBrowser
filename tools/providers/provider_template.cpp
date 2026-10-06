#include "provider.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using Json = nlohmann::json;


// ------------------------------------------------------------
// HTTP helper
// ------------------------------------------------------------

static size_t writeCallback(
    char* ptr,
    size_t size,
    size_t nmemb,
    void* userdata
) {
    auto* output =
        static_cast<std::string*>(userdata);

    output->append(
        ptr,
        size * nmemb
    );

    return size * nmemb;
}

static std::string httpGet(
    const std::string& url
) {
    CURL* curl = curl_easy_init();

    if (!curl)
        throw std::runtime_error(
            "curl_easy_init failed"
        );

    std::string response;

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url.c_str()
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        writeCallback
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response
    );

    curl_easy_setopt(
        curl,
        CURLOPT_FOLLOWLOCATION,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        20L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_USERAGENT,
        "SwitchGamesBrowser/1.0"
    );

    CURLcode result =
        curl_easy_perform(curl);

    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        throw std::runtime_error(
            curl_easy_strerror(result)
        );
    }

    return response;
}


// ------------------------------------------------------------
// URL encoder
// ------------------------------------------------------------

static std::string urlEncode(
    const std::string& value
) {
    CURL* curl = curl_easy_init();

    if (!curl)
        return value;

    char* encoded =
        curl_easy_escape(
            curl,
            value.c_str(),
            static_cast<int>(value.size())
        );

    std::string result =
        encoded ? encoded : value;

    if (encoded)
        curl_free(encoded);

    curl_easy_cleanup(curl);

    return result;
}


// ------------------------------------------------------------
// Provider
// ------------------------------------------------------------

class MyProvider final : public Provider {
public:

    std::string id() const override {
        // Keep this matched to the filename.
        // Example:
        // my_provider.cpp
        return "my_provider";
    }

    std::string name() const override {
        return "My Provider";
    }

    std::vector<ProviderResult>
    search(
        const std::string& query
    ) override {
        std::vector<ProviderResult> results;

        // Adapt this URL to your provider.
        std::string url =
            "https://YOUR-SOURCE.example/search?q="
            + urlEncode(query);

        std::string body =
            httpGet(url);

        // Example for a JSON response.
        Json root =
            Json::parse(body);

        if (!root.is_array())
            return results;

        for (const auto& item : root) {

            ProviderResult result;

            result.title =
                item.value(
                    "title",
                    ""
                );

            result.magnet =
                item.value(
                    "magnet",
                    ""
                );

            result.infoHash =
                item.value(
                    "infoHash",
                    ""
                );

            // Provider layer only returns candidates.
            // No size filtering.
            // No file-list lookup.
            // No NSP/NSZ/XCI/XCZ filtering here.

            if (result.title.empty())
                continue;

            if (
                result.magnet.empty() &&
                result.infoHash.empty()
            ) {
                continue;
            }

            results.push_back(
                std::move(result)
            );
        }

        return results;
    }
};