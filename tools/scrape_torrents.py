#!/usr/bin/env python3
"""Optional APIBay adapter adapted from the supplied Android provider.
No claim is made about availability or safety of results. Network not required by tests.
"""
import argparse
import json
from pathlib import Path
import re
import urllib.parse
from build_index import fetch_json, info_hash, write_atomic

def convert(results):
    if not isinstance(results, list):
        raise ValueError('Provider did not return an array')
    rows, seen = [], set()
    for item in results:
        if not isinstance(item, dict):
            continue
        title = item.get('name', '')
        if not isinstance(title, str) or not re.search(r'\b(?:switch|NSP|NSZ|XCI|XCZ)\b', title, re.I):
            continue
        digest = item.get('info_hash', '')
        magnet = 'magnet:?xt=urn:btih:' + str(digest) + '&dn=' + urllib.parse.quote(title)
        digest = info_hash(magnet)
        if not digest or digest == '0' * 40 or digest in seen:
            continue
        seen.add(digest)
        rows.append({'title': title, 'magnet': magnet, 'size': str(item.get('size', '')) + ' bytes'})
    return rows

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--query', action='append', required=True)
    parser.add_argument('--output', default='data/provider-releases.json')
    args = parser.parse_args()
    rows, seen = [], set()
    for query in args.query:
        data = fetch_json('https://apibay.org/q.php?' + urllib.parse.urlencode({'q': query}))
        for row in convert(data):
            digest = info_hash(row['magnet'])
            if digest not in seen:
                seen.add(digest); rows.append(row)
    if not rows:
        raise SystemExit('No valid Switch releases; previous provider file preserved')
    write_atomic(Path(args.output), json.dumps(rows, ensure_ascii=False).encode())
    print('Collected', len(rows), 'Switch-marked releases')

if __name__ == '__main__':
    main()
