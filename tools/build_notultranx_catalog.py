#!/usr/bin/env python3
"""Build an offline NotUltraNX game-ID/name index; no download links or tokens.

Default mode contacts only the public TitleDB metadata source. Optional
--scan-site mode reads public listing pages conservatively and stops on
rate-limiting instead of retrying or evading access restrictions.
"""
import argparse
import json
import re
import time
import urllib.error
import urllib.request
from pathlib import Path

TITLEDB = "https://raw.githubusercontent.com/blawar/titledb/master/US.en.json"
SITE = "https://not.ultranx.ru/en"
ID_PATTERN = re.compile(r"^[0-9A-F]{16}$")
SITE_ID_PATTERN = re.compile(r"/game/([0-9A-Fa-f]{16})(?![0-9A-Fa-f])")


def valid_id(value):
    return isinstance(value, str) and ID_PATTERN.fullmatch(value.upper()) is not None


def load_catalog(path):
    if not path.exists():
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schemaVersion") != 1 or not isinstance(document.get("games"), list):
        raise ValueError("Unexpected catalog schema in " + str(path))
    result = {}
    for row in document["games"]:
        if not isinstance(row, dict) or not valid_id(row.get("id")):
            continue
        id_ = row["id"].upper()
        result[id_] = {
            "id": id_,
            "name": str(row.get("name") or "")[:512],
            "cardText": str(row.get("cardText") or "")[:3400],
        }
    return result


def fetch(url, limit):
    request = urllib.request.Request(url, headers={
        "User-Agent": "SwitchGamesBrowser-catalog-builder/1.0 (public metadata; single request)",
        "Accept": "application/json,text/html;q=0.8",
    })
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("Metadata response exceeds expected size")
    return data


def merge_site_ids(games, max_pages):
    """Optional, single-threaded public catalog scan, with rate-limit safety."""
    added = 0
    misses = 0
    for page in range(1, max_pages + 1):
        url = SITE + "?p=" + str(page) + "&s=&sb=release_date&so=desc"
        try:
            html = fetch(url, 4 * 1024 * 1024).decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            if exc.code in (403, 429):
                raise RuntimeError("Site denied/rate-limited the catalog scan. Try again later.") from exc
            if page == 1:
                raise
            print(f"Page {page} returned HTTP {exc.code}; stopping.")
            break
        found = {x.upper() for x in SITE_ID_PATTERN.findall(html)}
        new = 0
        for title_id in found:
            if title_id not in games and len(games) < 10000:
                games[title_id] = {"id": title_id, "name": "", "cardText": ""}
                new += 1
        added += new
        misses = 0 if new else misses + 1
        print(f"Page {page}: {len(found)} IDs, {new} new", flush=True)
        if misses >= 6:
            break
        time.sleep(1.5)
    print(f"Optional site scan added {added} new IDs", flush=True)


def add_metadata(games, source):
    """TitleDB is keyed by Nintendo eShop ID; game objects contain title IDs."""
    print("Fetching public TitleDB names...", flush=True)
    payload = json.loads(fetch(source, 150 * 1024 * 1024))
    if not isinstance(payload, dict):
        raise ValueError("Unexpected TitleDB JSON structure")
    named = 0
    for row in payload.values():
        if not isinstance(row, dict):
            continue
        id_ = row.get("id") or row.get("titleId")
        name = row.get("name")
        if not valid_id(id_) or not isinstance(name, str):
            continue
        name = name.strip()
        if not name or len(name) > 512:
            continue
        existing = games.get(id_.upper())
        if existing is not None and not existing["name"]:
            existing["name"] = name
            named += 1
    print(f"Named {named} additional catalog IDs", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, default=Path("data/notultranx-seed.json"))
    parser.add_argument("--output", type=Path, default=Path("data/notultranx-catalog.json"))
    parser.add_argument("--scan-site", action="store_true",
                        help="Opt-in slow public page scan; stops on 403/429")
    parser.add_argument("--max-pages", type=int, default=60)
    parser.add_argument("--titledb", default=TITLEDB)
    args = parser.parse_args()
    games = load_catalog(args.seed)
    if len(games) < 20:
        raise SystemExit("Seed catalog missing or too small; refusing to replace output.")
    for id_, row in load_catalog(args.output).items():
        if id_ not in games:
            games[id_] = row
        elif not games[id_]["name"] and row["name"]:
            games[id_]["name"] = row["name"]
    if args.scan_site:
        merge_site_ids(games, max(1, min(args.max_pages, 96)))
    add_metadata(games, args.titledb)
    if len(games) < 20:
        raise SystemExit("Refusing to publish empty catalog")
    document = {
        "schemaVersion": 1,
        "source": SITE,
        "nameSource": args.titledb,
        "games": [games[id_] for id_ in sorted(games)],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temp = args.output.with_name(args.output.name + ".part")
    temp.write_text(json.dumps(document, ensure_ascii=False, separators=(",", ":")),
                    encoding="utf-8")
    temp.replace(args.output)
    total_named = sum(bool(row["name"]) for row in games.values())
    print(f"Saved {len(games)} IDs ({total_named} named) to {args.output}", flush=True)


if __name__ == "__main__":
    main()
