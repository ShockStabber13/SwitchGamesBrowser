import os
import sys
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from authorized_site_scraper import convert_results, load_sources, merge_releases

CONFIG = {
    "name": "TestSite",
    "resultsPath": "results",
    "fields": {
        "title": "name", "magnet": "magnet", "infoHash": "info_hash",
        "size": "size", "files": "files"
    },
    "fileNameField": "name",
    "allowedExtensions": [".nsp", ".nsz", ".xci", ".xcz"],
    "requireFileList": True,
}


class AuthorizedSiteTests(unittest.TestCase):
    def test_requires_switch_file_when_enabled(self):
        game = {"id": "10", "name": "Demo Quest"}
        payload = {"results": [
            {"name": "Demo Quest", "info_hash": "1" * 40, "files": [{"name": "Demo Quest.nsp"}]},
            {"name": "Demo Quest PC", "info_hash": "2" * 40, "files": [{"name": "demo.iso"}]},
        ]}
        rows = convert_results(game, payload, CONFIG)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["gameId"], "10")

    def test_hash_from_magnet(self):
        game = {"id": "10", "name": "Demo Quest"}
        payload = {"results": [{
            "name": "Demo Quest",
            "magnet": "magnet:?xt=urn:btih:" + "a" * 40,
            "files": ["Demo Quest.xci"],
        }]}
        rows = convert_results(game, payload, CONFIG)
        self.assertEqual(rows[0]["infoHash"], "a" * 40)

    def test_can_keep_candidate_when_provider_has_no_file_list(self):
        game = {"id": "10", "name": "Demo Quest"}
        cfg = dict(CONFIG)
        cfg["requireFileList"] = False
        payload = {"results": [{
            "name": "Demo Quest Release",
            "info_hash": "b" * 40,
            "magnet": "magnet:?xt=urn:btih:" + "b" * 40,
        }]}
        rows = convert_results(game, payload, cfg)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["files"], [])

    def test_rejects_known_non_switch_file_list_even_when_not_required(self):
        game = {"id": "10", "name": "Demo Quest"}
        cfg = dict(CONFIG)
        cfg["requireFileList"] = False
        cfg["validateFileExtensionsWhenPresent"] = True
        payload = {"results": [{
            "name": "Demo Quest",
            "info_hash": "c" * 40,
            "files": ["demo.iso"],
        }]}
        self.assertEqual(convert_results(game, payload, cfg), [])

    def test_multi_source_defaults_and_disable(self):
        config = {
            "defaults": {"timeoutSeconds": 30, "fields": {"title": "title"}},
            "sources": [
                {"name": "One", "enabled": True, "searchUrl": "https://one.test/?q={query}"},
                {"name": "Two", "enabled": False, "searchUrl": "https://two.test/?q={query}"},
            ],
        }
        with patch.dict(os.environ, {}, clear=False):
            sources = load_sources(config)
        self.assertEqual([x["name"] for x in sources], ["One"])
        self.assertEqual(sources[0]["timeoutSeconds"], 30)
        self.assertEqual(sources[0]["fields"]["title"], "title")

    def test_runtime_json_can_add_private_source(self):
        config = {"sources": [{"name": "One", "enabled": False}]}
        runtime = '[{"name":"Private","searchUrl":"https://private.test/?q={query}","enabled":true}]'
        with patch.dict(os.environ, {"TORRENT_SOURCES_JSON": runtime}, clear=False):
            sources = load_sources(config)
        self.assertEqual([x["name"] for x in sources], ["Private"])

    def test_duplicate_hash_keeps_all_source_names(self):
        rows = [
            {"gameId": "10", "infoHash": "d" * 40, "source": "One", "sources": ["One"], "files": []},
            {"gameId": "10", "infoHash": "d" * 40, "source": "Two", "sources": ["Two"], "files": ["Demo.nsp"]},
        ]
        merged = merge_releases(rows)
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0]["sources"], ["One", "Two"])
        self.assertEqual(merged[0]["files"], ["Demo.nsp"])


if __name__ == "__main__":
    unittest.main()
