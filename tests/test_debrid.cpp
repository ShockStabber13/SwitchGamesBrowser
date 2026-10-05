#include "debrid.hpp"
#include <cassert>
#include <iostream>

int main() {
    const std::string json = R"({
        "1111111111111111111111111111111111111111": {
            "cached": true,
            "downloaded": false,
            "remoteId": "42",
            "files": [
                {"name":"Game.nsp","id":"7"},
                "Update.nsz"
            ]
        }
    })";

    auto rows = sgb::parseDebridStatusJson(json);
    auto it = rows.find("1111111111111111111111111111111111111111");
    assert(it != rows.end());
    assert(it->second.cached);
    assert(!it->second.downloaded);
    assert(it->second.remoteId == "42");
    assert(it->second.files.size() == 2);
    assert(it->second.files[0].name == "Game.nsp");
    assert(it->second.files[0].id == "7");
    assert(it->second.files[1].name == "Update.nsz");
    assert(sgb::parseDebridService("TORBOX") == sgb::DebridService::TorBox);
    assert(sgb::parseDebridService("alldebrid") == sgb::DebridService::AllDebrid);
    auto roundTrip = sgb::parseDebridStatusJson(sgb::serializeDebridStatusJson(rows));
    assert(roundTrip.at("1111111111111111111111111111111111111111").files.size() == 2);
    assert(roundTrip.at("1111111111111111111111111111111111111111").remoteId == "42");
    std::cout << "Debrid status parsing passed\n";
    return 0;
}
