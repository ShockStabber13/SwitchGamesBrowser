import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
from scrape_torrents import convert

class ProviderTests(unittest.TestCase):
    def test_switch_only_and_sentinel_rejected(self):
        rows = convert([
            {'name': 'Demo Quest [NSP]', 'info_hash': '1'*40, 'size': '123'},
            {'name': 'Demo Quest PC', 'info_hash': '2'*40},
            {'name': 'No results Switch', 'info_hash': '0'*40},
            {'name': 'Demo Quest [NSP]', 'info_hash': '1'*40},
        ])
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0]['magnet'].startswith('magnet:?'))
    def test_error_response(self):
        with self.assertRaises(ValueError): convert({'error':'offline'})
