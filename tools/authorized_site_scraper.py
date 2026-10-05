#!/usr/bin/env python3
"""Search configured provider plugins for every IGDB Switch game."""

import argparse
import concurrent.futures
import datetime as dt
import json
import os
from pathlib import Path
import re
import urllib.parse
import base64

from providers import load_providers


HASH_RE = re.compile(r"^[0-9a-fA-F]{40}$")
BASE32_RE = re.compile(r"^[A-Za-z2-7]{32}$")
ALLOWED_EXTENSIONS = (".nsp", ".nsz", ".xci", ".xcz")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_atomic(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(
        json.dumps(value, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    os.replace(temp, path)


def normalize_hash(value):
    if not isinstance(value, str):
        return ""

    value = value.strip()

    if HASH_RE.fullmatch(value):
        return value.lower()

    if BASE32_RE.fullmatch(value):
        try:
            return base64.b32decode(value.upper()).hex()
        except Exception:
            return ""

    return ""


def info_hash_from_magnet(magnet):
    if not isinstance(magnet, str) or not magnet.startswith("magnet:?"):
        return ""

    query = urllib.parse.parse_qs(
        urllib.parse.urlsplit(magnet).query
    )

    for value in query.get("xt", []):
        if value.lower().startswith("urn:btih:"):
            return normalize_hash(value[9:])

    return ""


def normalize_files(value):
    if not isinstance(value, list):
        return []

    output = []

    for entry in value:
        if isinstance(entry, str):
            output.append(entry)
        elif isinstance(entry, dict):
            name = entry.get("name")
            if isinstance(name, str):
                output.append(name)

    return output


def normalize_provider_result(game, provider, item):
    if not isinstance(item, dict):
        return None

    title = item.get("title", "")
    if not isinstance(title, str) or not title.strip():
        return None

    magnet = item.get("magnet", "")
    digest = normalize_hash(
        item.get("infoHash", item.get("info_hash", ""))
    )

    if not digest:
        digest = info_hash_from_magnet(magnet)

    if not digest:
        return None

    if not isinstance(magnet, str) or not magnet.startswith("magnet:?"):
        magnet = "magnet:?xt=urn:btih:" + digest

    files = normalize_files(item.get("files", []))

    if files and not any(
        name.lower().endswith(ALLOWED_EXTENSIONS)
        for name in files
    ):
        return None

    source = str(
        getattr(provider, "name", provider.__class__.__name__)
    )[:128]

    return {
        "gameId": str(game["id"]),
        "gameName": str(game["name"]),
        "title": title.strip(),
        "magnet": magnet,
        "infoHash": digest,
        "size": str(item.get("size", ""))[:100],
        "source": source,
        "sources": [source],
        "files": files,
    }


def merge_releases(rows):
    unique = {}

    for row in rows:
        gid = str(row.get("gameId", ""))
        digest = normalize_hash(row.get("infoHash", ""))

        if not gid or not digest:
            continue

        key = (gid, digest)

        if key not in unique:
            copy = dict(row)
            sources = copy.get("sources") or [
                copy.get("source", "Provider")
            ]
            copy["sources"] = list(dict.fromkeys(
                str(x)[:128] for x in sources if x
            ))
            copy["source"] = (
                copy["sources"][0]
                if copy["sources"]
                else "Provider"
            )
            unique[key] = copy
            continue

        existing = unique[key]

        names = list(
            existing.get("sources")
            or [existing.get("source", "Provider")]
        )
        names.extend(
            row.get("sources")
            or [row.get("source", "Provider")]
        )

        existing["sources"] = list(dict.fromkeys(
            str(x)[:128] for x in names if x
        ))

        if not existing.get("files") and row.get("files"):
            existing["files"] = row["files"]

        if not existing.get("size") and row.get("size"):
            existing["size"] = row["size"]

        if len(str(row.get("title", ""))) > len(
            str(existing.get("title", ""))
        ):
            existing["title"] = row["title"]

    return list(unique.values())


def search_terms_for_game(game):
    raw = game.get("torrentSearchTerms")

    if not isinstance(raw, list):
        raw = [game.get("name", "")]

    output = []
    seen = set()

    for value in raw:
        if not isinstance(value, str):
            continue

        value = value.strip()
        key = value.casefold()

        if value and key not in seen:
            seen.add(key)
            output.append(value)

    return output[:32]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--catalog",
        default="data/igdb-switch-games.json",
    )
    parser.add_argument(
        "--output",
        default="data/provider-releases.json",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=8,
    )
    args = parser.parse_args()

    games = read_json(args.catalog)
    providers = load_providers()

    if not providers:
        write_atomic(args.output, [])

        status = {
            "generatedAt": dt.datetime.now(
                dt.timezone.utc
            ).isoformat(),
            "providers": [],
            "catalogGames": len(games),
            "matchedGames": 0,
            "uniqueReleases": 0,
            "failedSearches": 0,
            "sourceStats": {},
            "failures": [],
        }

        write_atomic(
            Path(args.output).with_name("scraper-status.json"),
            status,
        )

        print("No providers enabled.")
        return

    previous = (
        read_json(args.output)
        if Path(args.output).exists()
        else []
    )

    old_by_game_source = {}

    for row in previous:
        gid = str(row.get("gameId", ""))

        for source in (
            row.get("sources")
            or [row.get("source", "")]
        ):
            if source:
                old_by_game_source.setdefault(
                    (gid, str(source)),
                    [],
                ).append(row)

    provider_names = {
        provider: str(
            getattr(
                provider,
                "name",
                provider.__class__.__name__,
            )
        )
        for provider in providers
    }

    source_stats = {
        name: {
            "searches": 0,
            "matchedGames": 0,
            "releases": 0,
            "failures": 0,
        }
        for name in provider_names.values()
    }

    collected = []
    failures = []
    matched_games = set()

    def search_provider(game, provider):
        gid = str(game["id"])
        name = str(game["name"])
        source = provider_names[provider]

        rows = []
        local_failures = []
        searches = 0
        provider_failures = 0

        for term in search_terms_for_game(game):
            searches += 1

            try:
                raw = provider.search(term) or []

                for item in raw:
                    normalized = normalize_provider_result(
                        game, provider, item
                    )
                    if normalized:
                        rows.append(normalized)

            except Exception as error:
                provider_failures += 1

                rows.extend(
                    old_by_game_source.get((gid, source), [])
                )

                local_failures.append({
                    "gameId": gid,
                    "gameName": name,
                    "source": source,
                    "query": term,
                    "error": str(error)[:300],
                })

        return {
            "gameId": gid,
            "source": source,
            "rows": rows,
            "failures": local_failures,
            "searches": searches,
            "providerFailures": provider_failures,
        }

    workers = max(1, min(args.workers, 32))

    jobs = []

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=workers
    ) as pool:

        for game in games:
            gid = str(game.get("id", ""))
            name = str(game.get("name", "")).strip()

            if not gid or not name:
                continue

            for provider in providers:
                jobs.append(
                    pool.submit(search_provider, game, provider)
                )

        total = len(jobs)

        for completed, future in enumerate(
            concurrent.futures.as_completed(jobs), 1
        ):
            result = future.result()

            source = result["source"]
            rows = result["rows"]

            source_stats[source]["searches"] += result["searches"]
            source_stats[source]["failures"] += result["providerFailures"]

            if rows:
                source_stats[source]["matchedGames"] += 1
                source_stats[source]["releases"] += len(rows)
                matched_games.add(result["gameId"])
                collected.extend(rows)

            failures.extend(result["failures"])

            if completed % 100 == 0:
                print(
                    f"Completed {completed}/{total} searches; "
                    f"{len(collected)} raw releases",
                    flush=True,
                )

    releases = merge_releases(collected)

    write_atomic(args.output, releases)

    status = {
        "generatedAt": dt.datetime.now(
            dt.timezone.utc
        ).isoformat(),
        "providers": list(provider_names.values()),
        "catalogGames": len(games),
        "matchedGames": len(matched_games),
        "uniqueReleases": len(releases),
        "failedSearches": len(failures),
        "sourceStats": source_stats,
        "failures": failures[:200],
    }

    write_atomic(
        Path(args.output).with_name("scraper-status.json"),
        status,
    )

    print(json.dumps(status, indent=2))


if __name__ == "__main__":
    main()
