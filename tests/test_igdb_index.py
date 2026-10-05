import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).parents[1] / "tools" / "build_igdb_index.py"
spec = importlib.util.spec_from_file_location("igdb_builder", path)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class IgdbIndexTests(unittest.TestCase):
    def test_join_is_by_igdb_id_and_keeps_games_without_candidates(self):
        catalog = [{
            "id": "42", "name": "Demo Quest", "slug": "demo-quest",
            "genres": ["Adventure"], "releaseDate": "2026-01-01",
            "rating": 85.5, "ratingCount": 12,
            "cover": "https://example.com/c.jpg", "summary": "Demo",
        }, {"id": "43", "name": "No Torrent", "genres": []}]
        releases = [{
            "gameId": "42", "title": "Demo Quest [NSP]",
            "infoHash": "1" * 40,
            "magnet": "magnet:?xt=urn:btih:" + "1" * 40,
            "source": "TestSite",
            "sources": ["TestSite", "MirrorSite"],
        }]
        index, report = builder.build_index(catalog, releases)
        self.assertEqual(len(index["games"]), 2)
        by_id = {row["igdbId"]: row for row in index["games"]}
        self.assertEqual(by_id["42"]["genres"], ["Adventure"])
        self.assertEqual(by_id["42"]["releases"][0]["sources"], ["TestSite", "MirrorSite"])
        self.assertEqual(by_id["43"]["releases"], [])
        self.assertEqual(report["gamesWithCandidates"], 1)
        self.assertEqual(report["gamesWithoutCandidates"], 1)


if __name__ == "__main__":
    unittest.main()
