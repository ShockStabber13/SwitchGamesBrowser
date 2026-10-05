#include "catalog.hpp"
#include <cassert>
#include <iostream>
int main(int argc, char** argv) {
    assert(argc == 2);
    auto games = sgb::parse(sgb::read(argv[1]));
    assert(games.size() == 3);
    sgb::Filter f; f.sort = sgb::Sort::Rating;
    auto rows = sgb::browse(games, f, {});
    assert(games[rows.front()].title == "Demo Quest");
    assert(games[rows.back()].rating == -1);
    f.minRating = 80; assert(sgb::browse(games, f, {}).size() == 1);
    f.minReviews = 50; assert(sgb::browse(games, f, {}).empty());
    f = {}; f.search = "QUEST"; assert(sgb::browse(games, f, {}).size() == 2);
    f = {}; f.favouritesOnly = true;
    assert(sgb::browse(games, f, {games[0].id}).size() == 1);
    auto invalid = sgb::Json::parse(sgb::read(argv[1]));
    invalid["games"][1]["id"] = invalid["games"][0]["id"];
    bool rejected = false; try { sgb::parse(invalid.dump()); } catch (...) { rejected = true; }
    assert(rejected);
    std::cout << "Catalog parsing, sorting, filtering and duplicate validation passed\n";
}
