import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
from build_index import compile_index
rows = [{'title': title, 'magnet': 'magnet:?xt=urn:btih:' + str(i) * 40} for i, title in enumerate(['Demo Quest [NSZ]', 'Demo Quest 2 [NSP]', 'Puzzle Garden [XCI]'], 1)]
metadata = [{'id': 1, 'name': 'Demo Quest', 'rating': 4.5, 'ratings_count': 10, 'genres': [{'name': 'Adventure'}], 'released': '2024-05-01'}, {'id': 2, 'name': 'Puzzle Garden', 'rating': 3.5, 'ratings_count': 5, 'genres': [{'name': 'Puzzle'}], 'released': '2025-03-15'}]
index, _ = compile_index(rows, metadata)
root = Path(__file__).parents[1]
(root / 'tests/fixture-index.json').write_text(json.dumps(index, indent=2))
(root / 'tests/fixture-releases.json').write_text(json.dumps(rows, indent=2))
(root / 'tests/fixture-metadata.json').write_text(json.dumps(metadata, indent=2))
