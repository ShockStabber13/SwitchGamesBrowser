#!/usr/bin/env python3
"""Build a project-specific Switch catalog. Python standard library only."""
import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import time
import unicodedata
import urllib.parse
import urllib.request

MAX_BYTES = 48 * 1024 * 1024
DEFAULT_SOURCE = 'https://raw.githubusercontent.com/Langegen/switch-games/refs/heads/main/switch_games.json'

def fetch_json(url):
    if urllib.parse.urlsplit(url).scheme != 'https':
        raise ValueError('HTTPS required')
    # Retry without printing URLs: RAWG credentials are query parameters.
    for attempt in range(3):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'SwitchGamesBrowser/0.1'})
            with urllib.request.urlopen(req, timeout=45) as response:
                if urllib.parse.urlsplit(response.url).scheme != 'https':
                    raise ValueError('HTTPS redirect required')
                data = response.read(MAX_BYTES + 1)
            if len(data) > MAX_BYTES:
                raise ValueError('Response exceeds size limit')
            return json.loads(data)
        except Exception:
            if attempt == 2:
                raise RuntimeError('Source request failed; previous index was preserved') from None
            time.sleep(2 ** attempt)

def read_json(path):
    data = Path(path).read_bytes()
    if len(data) > MAX_BYTES:
        raise ValueError('Input exceeds size limit')
    return json.loads(data)

def normalise(title):
    text = unicodedata.normalize('NFKD', title).casefold()
    text = ''.join(c for c in text if not unicodedata.combining(c))
    return ' '.join(re.findall(r'[^\W_]+', text.replace('&', ' and '), flags=re.UNICODE))

def clean_release_title(title):
    # Preserve sequel numbers and edition names; strip only release markers.
    title = re.sub(r'\[[^\]]*\]', ' ', title)
    title = re.sub(r'\((?:NSP|NSZ|XCI|XCZ|RUS[^)]*|ENG[^)]*|Multi\d+[^)]*)\)', ' ', title, flags=re.I)
    title = re.split(r'\s+(?:v\d+(?:\.\d+)+|\+\s*(?:DLC|Update)|(?:NSP|NSZ|XCI|XCZ)\b)', title, maxsplit=1, flags=re.I)[0]
    return ' '.join(title.split()).strip(' -')

def info_hash(magnet):
    if not isinstance(magnet, str) or len(magnet) > 4096:
        return None
    url = urllib.parse.urlsplit(magnet)
    if url.scheme != 'magnet':
        return None
    for xt in urllib.parse.parse_qs(url.query).get('xt', []):
        if xt.lower().startswith('urn:btih:'):
            value = xt[9:]
            if re.fullmatch(r'[a-fA-F0-9]{40}', value):
                return value.lower()
            if re.fullmatch(r'[a-zA-Z2-7]{32}', value):
                import base64
                return base64.b32decode(value.upper()).hex()
    return None

def number(value, lo=0, hi=100):
    if isinstance(value, bool):
        return None
    try:
        n = float(value)
        return n if math.isfinite(n) and lo <= n <= hi else None
    except (TypeError, ValueError):
        return None

def text(value, limit=4096):
    return value.encode('utf-8')[:limit].decode('utf-8', errors='ignore') if isinstance(value, str) else ''

def rawg_catalog(key):
    def api(route, params):
        return fetch_json('https://api.rawg.io/api/' + route + '?' + urllib.parse.urlencode({'key': key, **params}))
    platform_id = None
    for page in range(1, 21):
        result = api('platforms', {'page_size': 40, 'page': page})
        for platform in result.get('results', []):
            if platform.get('slug') == 'nintendo-switch':
                platform_id = platform['id']
        if platform_id is not None or not result.get('next'):
            break
    if platform_id is None:
        raise ValueError('RAWG Nintendo Switch platform not found')
    games = []
    # Explicit page numbers prevent forwarding API keys to an arbitrary next URL.
    for page in range(1, 1001):
        result = api('games', {'platforms': platform_id, 'page_size': 40, 'page': page, 'ordering': 'id'})
        games.extend(result.get('results', []))
        if not result.get('next'):
            return games
        time.sleep(0.25)
    raise ValueError('RAWG pagination limit reached; refusing a partial catalog')

def compile_index(releases, metadata, overrides=None):
    if not isinstance(releases, list) or not isinstance(metadata, list):
        raise ValueError('Inputs must be JSON arrays')
    if len(releases) > 20000 or len(metadata) > 40000:
        raise ValueError('Catalog row limit exceeded')
    overrides = overrides or {}
    by_name, by_id = {}, {}
    for item in metadata:
        if not isinstance(item, dict):
            continue
        if 'platforms' in item:
            slugs = [p.get('platform', {}).get('slug') for p in item['platforms'] if isinstance(p, dict)]
            if 'nintendo-switch' not in slugs:
                continue
        key = str(item.get('id', ''))
        if not key:
            continue
        if key in by_id:
            raise ValueError('Duplicate metadata ID')
        by_id[key] = item
        by_name.setdefault(normalise(text(item.get('name') or item.get('title'))), []).append(item)
    rows, groups, seen = [], {}, set()
    report = {'matched': 0, 'unmatched': 0, 'ambiguous': 0, 'invalid': 0, 'duplicates': 0}
    for release in releases:
        if not isinstance(release, dict):
            report['invalid'] += 1
            continue
        title = text(release.get('title'), 1024)
        magnet = release.get('magnet') or release.get('magnetURI')
        digest = info_hash(magnet)
        if not title or not digest:
            report['invalid'] += 1
            continue
        if digest in seen:
            report['duplicates'] += 1
            continue
        seen.add(digest)
        clean = clean_release_title(title)
        title_id = text(release.get('title_id'), 16).upper()
        if not re.fullmatch(r'[0-9A-F]{16}', title_id):
            title_id = ''
        explicit_id = overrides.get(digest, overrides.get(title_id)) if title_id else overrides.get(digest)
        match = None
        if explicit_id is not None:
            match = by_id.get(str(explicit_id))
            if match is None:
                raise ValueError('Override points to missing metadata ID')
        else:
            candidates = by_name.get(normalise(clean), [])
            if len(candidates) == 1:
                match = candidates[0]
            elif len(candidates) > 1:
                report['ambiguous'] += 1
        report['matched' if match else 'unmatched'] += 1
        group = ('rawg:' + str(match['id'])) if match else ('title:' + title_id if title_id else 'name:' + normalise(clean))
        if group not in groups:
            rating = number(match.get('rating'), 0, 5) if match else None
            count = int(number(match.get('ratings_count'), 0, 100000000) or 0) if match else 0
            # RAWG list Metacritic score may cover another platform. Label it explicitly.
            critic = number(match.get('metacritic')) if match else None
            rawg_url = 'https://rawg.io/games/' + text(match.get('slug'), 256) if match and match.get('slug') else ''
            row = {'id': hashlib.sha256(group.encode()).hexdigest()[:24], 'title': text(match.get('name') or match.get('title'), 1024) if match else clean,
                   'titleId': title_id, 'releaseDate': text(match.get('released'), 10) if match else '',
                   'rating': rating * 20 if rating is not None and count > 0 else None,
                   'ratingCount': count, 'ratingSource': 'RAWG users' if rating is not None and count > 0 else '',
                   'ratingUrl': rawg_url, 'criticRating': critic, 'criticSource': 'Metacritic via RAWG (platform unspecified)' if critic is not None else '',
                   'coverUrl': text(match.get('background_image'), 2048) if match else text(release.get('cover') or release.get('poster'), 2048),
                   'genres': [text(g.get('name'), 100) for g in match.get('genres', []) if isinstance(g, dict)] if match else [text(release.get('genre'), 256)] if release.get('genre') else [],
                   'summary': text(release.get('description')), 'metadataMatched': bool(match), 'releases': []}
            if row['coverUrl'] and not row['coverUrl'].startswith('https://'):
                row['coverUrl'] = ''
            groups[group] = row
            rows.append(row)
        groups[group]['releases'].append({'title': title, 'infoHash': digest, 'magnet': magnet, 'size': text(str(release.get('size', '')), 100), 'source': text(release.get('source') or 'Imported catalog', 128), 'sources': release.get('sources', []), 'topicId': text(str(release.get('topic_id', '')), 32)})
    rows.sort(key=lambda row: (normalise(row['title']), row['id']))
    if not rows:
        raise ValueError('No valid games; refusing to replace the previous index')
    return {'schemaVersion': 1, 'platform': 'Nintendo Switch', 'generatedAt': dt.datetime.now(dt.timezone.utc).isoformat(), 'games': rows}, report

def write_atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', help='Local release catalog JSON; omitted to fetch Langegen')
    parser.add_argument('--source-url', default=DEFAULT_SOURCE)
    parser.add_argument('--metadata', help='Local RAWG-style metadata array')
    parser.add_argument('--fetch-rawg', action='store_true')
    parser.add_argument('--overrides', default='data/match-overrides.json')
    parser.add_argument('--output', default='data')
    parser.add_argument('--allow-shrink', action='store_true', help='Accept a smaller catalog when intentionally switching sources')
    args = parser.parse_args()
    root = Path(args.output)
    releases = read_json(args.source) if args.source else fetch_json(args.source_url)
    if args.fetch_rawg:
        key = os.getenv('RAWG_API_KEY', '')
        if not key:
            raise ValueError('Set RAWG_API_KEY in your environment or GitHub Secrets')
        metadata = rawg_catalog(key)
    elif args.metadata:
        metadata = read_json(args.metadata)
    elif (root / 'metadata-cache.json').exists():
        metadata = read_json(root / 'metadata-cache.json')
    else:
        metadata = []
    overrides = read_json(args.overrides) if Path(args.overrides).exists() else {}
    index, report = compile_index(releases, metadata, overrides)
    if (root / 'switch-index.json').exists():
        old = read_json(root / 'switch-index.json')
        if not args.allow_shrink and len(index['games']) < len(old.get('games', [])) * 0.7:
            raise ValueError('Catalog shrank over 30%; previous index was preserved')
    payload = json.dumps(index, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode()
    if len(payload) > MAX_BYTES:
        raise ValueError('Output exceeds Switch index size limit')
    manifest = {'schemaVersion': 1, 'generatedAt': index['generatedAt'], 'gameCount': len(index['games']), 'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest(), 'matchReport': report}
    # Manifest last is the publication commit marker; workflow commits all files together.
    if args.fetch_rawg:
        write_atomic(root / 'metadata-cache.json', json.dumps(metadata, ensure_ascii=False).encode())
    write_atomic(root / 'switch-index.json', payload)
    write_atomic(root / 'manifest.json', json.dumps(manifest, indent=2).encode())
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        raise SystemExit(str(error)) from None
