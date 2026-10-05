import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('builder', Path(__file__).parents[1] / 'tools/build_index.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)

def release(title='Demo Quest [NSZ][ENG]', digest='1' * 40):
    return {'title': title, 'magnet': 'magnet:?xt=urn:btih:' + digest}

def metadata(name='Demo Quest', ident=1):
    return {'id': ident, 'name': name, 'rating': 4.5, 'ratings_count': 10, 'genres': [], 'slug': 'demo-quest', 'platforms': [{'platform': {'slug': 'nintendo-switch'}}]}

class MatchingTests(unittest.TestCase):
    def test_sequel_is_not_matched(self):
        index, report = b.compile_index([release('Demo Quest 2 [NSP]')], [metadata()])
        self.assertFalse(index['games'][0]['metadataMatched'])
        self.assertIsNone(index['games'][0]['rating'])
    def test_exact_match_and_dedup(self):
        index, report = b.compile_index([release(), release()], [metadata()])
        self.assertEqual(index['games'][0]['rating'], 90)
        self.assertEqual(report['duplicates'], 1)
        self.assertEqual(len(index['games'][0]['releases']), 1)
    def test_ambiguous_match_stays_unrated(self):
        index, report = b.compile_index([release()], [metadata(), metadata(ident=2)])
        self.assertEqual(report['ambiguous'], 1)
        self.assertIsNone(index['games'][0]['rating'])
    def test_override_selects_one_match(self):
        index, _ = b.compile_index([release()], [metadata(), metadata(ident=2)], {'1' * 40: 2})
        self.assertTrue(index['games'][0]['metadataMatched'])
    def test_invalid_magnet_rejected(self):
        with self.assertRaises(ValueError):
            b.compile_index([{'title': 'A', 'magnet': 'https://example.org'}], [])
    def test_base32_magnet(self):
        self.assertEqual(b.info_hash('magnet:?xt=urn:btih:' + 'A' * 32), '0' * 40)
    def test_bad_rating_and_zero_reviews(self):
        for value in [float('nan'), 7, -1, None]:
            m = metadata(); m['rating'] = value
            index, _ = b.compile_index([release()], [m])
            self.assertIsNone(index['games'][0]['rating'])
        m = metadata(); m['ratings_count'] = 0
        index, _ = b.compile_index([release()], [m])
        self.assertIsNone(index['games'][0]['rating'])
    def test_wrong_platform(self):
        m = metadata(); m['platforms'][0]['platform']['slug'] = 'pc'
        index, _ = b.compile_index([release()], [m])
        self.assertFalse(index['games'][0]['metadataMatched'])
    def test_two_releases_grouped(self):
        index, _ = b.compile_index([release(), release('Demo Quest [XCI]', '2' * 40)], [metadata()])
        self.assertEqual(len(index['games']), 1)
        self.assertEqual(len(index['games'][0]['releases']), 2)
    def test_edition_preserved(self):
        self.assertNotEqual(b.normalise(b.clean_release_title('Demo Quest Deluxe [NSP]')), b.normalise('Demo Quest'))

if __name__ == '__main__':
    unittest.main()
