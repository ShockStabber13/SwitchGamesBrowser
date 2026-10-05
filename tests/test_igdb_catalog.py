import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).parents[1] / "tools" / "igdb_catalog.py"
spec = importlib.util.spec_from_file_location("igdb_catalog", path)
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class IgdbCatalogTests(unittest.TestCase):

    def test_only_standalone_games_are_visible(self):
        rows = [
            {"id": "1", "name": "Demo Game", "gameType": 0},
            {"id": "2", "name": "Expansion One", "gameType": 1, "parentGame": "1"},
            {"id": "3", "name": "Update 2.0", "gameType": 14, "parentGame": "1"},
            {"id": "4", "name": "Demo Bundle", "gameType": 3},
            {"id": "5", "name": "Demo Remastered", "gameType": 9},
        ]

        result = catalog._visible_catalog(rows)

        ids = {game["id"] for game in result}
        self.assertEqual(ids, {"1", "5"})

        base = next(game for game in result if game["id"] == "1")

        self.assertIn("Demo Game", base["torrentSearchTerms"])
        self.assertIn("Demo Game Expansion One", base["torrentSearchTerms"])
        self.assertIn("Demo Game Update 2.0", base["torrentSearchTerms"])

        self.assertEqual(len(base["relatedContent"]), 2)


if __name__ == "__main__":
    unittest.main()
