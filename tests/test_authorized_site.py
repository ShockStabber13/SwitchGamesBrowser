import sys
from pathlib import Path
import unittest

sys.path.insert(
    0,
    str(Path(__file__).parents[1] / "tools"),
)

from authorized_site_scraper import (
    info_hash_from_magnet,
    merge_releases,
    normalize_provider_result,
    search_terms_for_game,
)


class Provider:
    name = "TestProvider"


class AuthorizedSiteTests(unittest.TestCase):

    def test_minimal_provider_result(self):
        game = {"id": "10", "name": "Demo Quest"}

        row = normalize_provider_result(
            game,
            Provider(),
            {
                "title": "Demo Quest",
                "infoHash": "a" * 40,
                "magnet": "magnet:?xt=urn:btih:" + "a" * 40,
            },
        )

        self.assertEqual(row["gameId"], "10")
        self.assertEqual(row["source"], "TestProvider")
        self.assertEqual(row["files"], [])
        self.assertEqual(row["size"], "")

    def test_hash_from_magnet(self):
        digest = "b" * 40
        magnet = "magnet:?xt=urn:btih:" + digest

        self.assertEqual(
            info_hash_from_magnet(magnet),
            digest,
        )

    def test_reject_invalid_hash(self):
        game = {"id": "10", "name": "Demo"}

        row = normalize_provider_result(
            game,
            Provider(),
            {
                "title": "Demo",
                "infoHash": "bad",
            },
        )

        self.assertIsNone(row)

    def test_reject_known_non_switch_files(self):
        game = {"id": "10", "name": "Demo"}

        row = normalize_provider_result(
            game,
            Provider(),
            {
                "title": "Demo",
                "infoHash": "c" * 40,
                "files": ["demo.iso"],
            },
        )

        self.assertIsNone(row)

    def test_duplicate_hash_merges_sources(self):
        rows = [
            {
                "gameId": "10",
                "infoHash": "d" * 40,
                "source": "One",
                "sources": ["One"],
                "files": [],
            },
            {
                "gameId": "10",
                "infoHash": "d" * 40,
                "source": "Two",
                "sources": ["Two"],
                "files": ["Demo.nsp"],
            },
        ]

        merged = merge_releases(rows)

        self.assertEqual(len(merged), 1)
        self.assertEqual(
            merged[0]["sources"],
            ["One", "Two"],
        )
        self.assertEqual(
            merged[0]["files"],
            ["Demo.nsp"],
        )

    def test_search_terms(self):
        game = {
            "name": "Demo",
            "torrentSearchTerms": [
                "Demo",
                "Demo DLC",
                "Demo",
            ],
        }

        self.assertEqual(
            search_terms_for_game(game),
            ["Demo", "Demo DLC"],
        )


if __name__ == "__main__":
    unittest.main()
