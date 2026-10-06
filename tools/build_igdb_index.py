#!/usr/bin/env python3
"""Join IGDB Switch metadata to authorized torrent candidates."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re

HASH_RE = re.compile(r"^[0-9a-f]{40}$")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_bytes(data)
    os.replace(temp, path)


def build_index(catalog, releases):
    by_game = {}
    duplicates = 0
    for release in releases:
        gid = str(release.get("gameId", ""))
        digest = str(release.get("infoHash", "")).lower()
        magnet = release.get("magnet", "")
        if not gid or not HASH_RE.fullmatch(digest):
            continue
        if not isinstance(magnet, str) or not magnet.startswith("magnet:?"):
            continue
        game_rows = by_game.setdefault(gid, {})
        if digest in game_rows:
            duplicates += 1
        game_rows[digest] = release

    games = []
    for game in catalog:
        gid = str(game.get("id", ""))
        matches = list(by_game.get(gid, {}).values())
        if not gid:
            continue
        rating = game.get("rating")
        rating_count = int(game.get("ratingCount") or 0)
        if rating is None or rating_count <= 0:
            rating = None
            rating_count = 0
        slug = str(game.get("slug", ""))
        row = {
            "id": "igdb-" + gid,
            "igdbId": gid,
            "title": str(game.get("name", ""))[:1024],
            "titleId": "",
            "releaseDate": str(game.get("releaseDate", ""))[:10],
            "rating": rating,
            "ratingCount": rating_count,
            "ratingSource": "IGDB",
            "ratingUrl": ("https://www.igdb.com/games/" + slug) if slug else "",
            "genres": list(game.get("genres") or [])[:64],
            "coverUrl": str(game.get("cover", ""))[:2048],
            "summary": str(game.get("summary", ""))[:4096],
            "metadataMatched": True,
            "gameType": game.get("gameType"),
            "parentGame": game.get("parentGame"),
            "releases": [],
        }
        for release in sorted(matches, key=lambda item: str(item.get("title", "")).casefold()):
            row["releases"].append({
                "title": str(release.get("title", ""))[:1024],
                "infoHash": str(release.get("infoHash", "")).lower(),
                "magnet": str(release.get("magnet", ""))[:4096],
                "size": str(release.get("size", ""))[:100],
                "source": str(release.get("source", "AuthorizedSite"))[:128],
                "sources": [str(x)[:128] for x in (release.get("sources") or [release.get("source", "AuthorizedSite")]) if x][:16],
                "files": [str(x)[:1024] for x in (release.get("files") or []) if isinstance(x, str)][:512],
                "topicId": "",
            })
        games.append(row)

    games.sort(key=lambda item: (item["title"].casefold(), item["id"]))
    if not games:
        raise ValueError("IGDB catalog is empty")

    report = {
        "igdbGames": len(catalog),
        "gamesWithCandidates": sum(1 for game in games if game["releases"]),
        "gamesWithoutCandidates": sum(1 for game in games if not game["releases"]),
        "releaseCandidates": sum(len(game["releases"]) for game in games),
        "duplicateReleases": duplicates,
    }
    return {
        "schemaVersion": 1,
        "platform": "Nintendo Switch",
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "games": games,
    }, report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", default="data/igdb-switch-games.json")
    parser.add_argument("--releases", default="data/provider-releases.json")
    parser.add_argument("--output", default="data")
    parser.add_argument("--allow-shrink", action="store_true")
    args = parser.parse_args()

    root = Path(args.output)
    index, report = build_index(read_json(args.catalog), read_json(args.releases))
    payload = json.dumps(index, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode()
    old_path = root / "switch-index.json"
    if old_path.exists() and not args.allow_shrink:
        old = read_json(old_path)
        old_count = len(old.get("games", []))
        if old_count and len(index["games"]) < old_count * 0.7:
            raise ValueError("Catalog shrank over 30%; previous index preserved")

    manifest = {
        "schemaVersion": 1,
        "generatedAt": index["generatedAt"],
        "gameCount": len(index["games"]),
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "source": "IGDB + authorized torrent sources",
        "report": report,
    }
    write_atomic(old_path, payload)
    write_atomic(root / "manifest.json", json.dumps(manifest, indent=2).encode())
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
