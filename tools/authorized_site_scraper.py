#!/usr/bin/env python3
"""Search one or more authorized torrent JSON APIs for every IGDB Switch game."""
import argparse
import concurrent.futures
import datetime as dt
import json
import os
from pathlib import Path
import re
import threading
import time
import urllib.parse
import urllib.request

MAX_BYTES = 32 * 1024 * 1024
HASH_RE = re.compile(r"^[0-9a-fA-F]{40}$")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_atomic(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temp, path)


def get_path(value, dotted, default=None):
    if not dotted:
        return value
    current = value
    for part in dotted.split("."):
        if not isinstance(current, dict) or part not in current:
            return default
        current = current[part]
    return current


def info_hash_from_magnet(magnet):
    if not isinstance(magnet, str) or not magnet.startswith("magnet:?"):
        return ""
    for value in urllib.parse.parse_qs(urllib.parse.urlsplit(magnet).query).get("xt", []):
        if value.lower().startswith("urn:btih:"):
            digest = value[9:]
            if HASH_RE.fullmatch(digest):
                return digest.lower()
    return ""


def _file_names(item, config):
    fields = config.get("fields", {})
    raw = get_path(item, fields.get("files", "files"), [])
    if not isinstance(raw, list):
        return []
    name_field = config.get("fileNameField", "name")
    names = []
    for entry in raw:
        if isinstance(entry, str):
            names.append(entry)
        elif isinstance(entry, dict):
            value = get_path(entry, name_field, "")
            if isinstance(value, str):
                names.append(value)
    return names


def convert_results(game, payload, config):
    rows = get_path(payload, config.get("resultsPath", "results"), payload)
    if not isinstance(rows, list):
        raise ValueError("Authorized site response does not contain a result array")

    fields = config.get("fields", {})
    allowed = tuple(ext.lower() for ext in config.get(
        "allowedExtensions", [".nsp", ".nsz", ".xci", ".xcz"]
    ))
    require_files = bool(config.get("requireFileList", False))
    validate_files_when_present = bool(config.get("validateFileExtensionsWhenPresent", True))
    source_name = str(config.get("name", "AuthorizedSite"))[:128]
    limit = int(config.get("maxResultsPerGame", 100))
    output, seen = [], set()

    for item in rows[: max(1, min(limit, 500))]:
        if not isinstance(item, dict):
            continue
        title = get_path(item, fields.get("title", "name"), "")
        magnet = get_path(item, fields.get("magnet", "magnet"), "")
        digest = get_path(item, fields.get("infoHash", "info_hash"), "")
        size = get_path(item, fields.get("size", "size"), "")
        if not isinstance(title, str) or not title.strip():
            continue

        file_names = _file_names(item, config)
        has_switch_file = any(name.lower().endswith(allowed) for name in file_names)
        if require_files and not has_switch_file:
            continue
        if file_names and validate_files_when_present and not has_switch_file:
            continue

        if not isinstance(digest, str) or not HASH_RE.fullmatch(digest):
            digest = info_hash_from_magnet(magnet)
        else:
            digest = digest.lower()
        if not digest:
            continue
        if not isinstance(magnet, str) or not magnet.startswith("magnet:?"):
            magnet = "magnet:?xt=urn:btih:" + digest

        key = (str(game["id"]), digest)
        if key in seen:
            continue
        seen.add(key)
        output.append({
            "gameId": str(game["id"]),
            "gameName": game["name"],
            "title": title.strip(),
            "magnet": magnet,
            "infoHash": digest,
            "size": str(size)[:100],
            "source": source_name,
            "sources": [source_name],
            "files": file_names,
        })
    return output


def _merge_dict(base, override):
    out = dict(base or {})
    for key, value in (override or {}).items():
        if key == "fields" and isinstance(value, dict):
            merged_fields = dict(out.get("fields") or {})
            merged_fields.update(value)
            out[key] = merged_fields
        else:
            out[key] = value
    return out


def load_sources(config):
    """Return enabled source configs. Supports legacy single-source config and runtime JSON secret."""
    if isinstance(config, dict) and isinstance(config.get("sources"), list):
        defaults = config.get("defaults") if isinstance(config.get("defaults"), dict) else {}
        sources = [_merge_dict(defaults, item) for item in config["sources"] if isinstance(item, dict)]
    elif isinstance(config, dict):
        sources = [dict(config)]
    else:
        raise ValueError("Authorized source config must be a JSON object")

    runtime = os.getenv("TORRENT_SOURCES_JSON", "").strip()
    if runtime:
        parsed = json.loads(runtime)
        if isinstance(parsed, dict):
            parsed = parsed.get("sources", [])
        if not isinstance(parsed, list):
            raise ValueError("TORRENT_SOURCES_JSON must be a JSON array or an object containing sources[]")
        by_name = {str(s.get("name", "")): s for s in sources if s.get("name")}
        for item in parsed:
            if not isinstance(item, dict):
                continue
            name = str(item.get("name", ""))
            if name and name in by_name:
                merged = _merge_dict(by_name[name], item)
                sources[sources.index(by_name[name])] = merged
                by_name[name] = merged
            else:
                sources.append(item)

    enabled = []
    for index, source in enumerate(sources, 1):
        if source.get("enabled", True) is False:
            continue
        source = dict(source)
        source.setdefault("name", f"AuthorizedSite{index}")
        enabled.append(source)
    return enabled


def _source_url(config):
    env_name = str(config.get("searchUrlEnv", "")).strip()
    if env_name and os.getenv(env_name):
        return os.getenv(env_name, "")
    # Legacy single-source environment variable remains supported.
    if config.get("legacyEnvironment", False) and os.getenv("TORRENT_SITE_SEARCH_URL"):
        return os.getenv("TORRENT_SITE_SEARCH_URL", "")
    return str(config.get("searchUrl", ""))


def _source_token(config):
    env_name = str(config.get("tokenEnv", "")).strip()
    if env_name:
        return os.getenv(env_name, "")
    if config.get("legacyEnvironment", False):
        return os.getenv("TORRENT_SITE_TOKEN", "")
    return str(config.get("token", ""))


def fetch_search(game_name, config, token=None):
    template = _source_url(config)
    if not template or "{query}" not in template:
        raise ValueError(f"{config.get('name', 'AuthorizedSite')}: searchUrl must contain {{query}}")
    url = template.replace("{query}", urllib.parse.quote(game_name, safe=""))
    if urllib.parse.urlsplit(url).scheme != "https":
        raise ValueError("Authorized torrent site search URL must use HTTPS")

    req = urllib.request.Request(url, method=str(config.get("method", "GET")).upper())
    req.add_header("Accept", "application/json")
    req.add_header("User-Agent", "SwitchGamesBrowser/0.4")
    token = _source_token(config) if token is None else token
    if token:
        req.add_header(
            str(config.get("authHeader", "Authorization")),
            str(config.get("authPrefix", "Bearer ")) + token,
        )

    with urllib.request.urlopen(req, timeout=int(config.get("timeoutSeconds", 45))) as response:
        raw = response.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("Authorized site response exceeds size limit")
    return json.loads(raw)


class SourceRateLimiter:
    def __init__(self, sources):
        self._locks = {str(s.get("name")): threading.Lock() for s in sources}
        self._next = {str(s.get("name")): 0.0 for s in sources}

    def wait(self, source):
        name = str(source.get("name"))
        delay = max(0.0, float(source.get("requestDelaySeconds", 0.25)))
        lock = self._locks[name]
        with lock:
            now = time.monotonic()
            wait_for = max(0.0, self._next[name] - now)
            if wait_for:
                time.sleep(wait_for)
            self._next[name] = time.monotonic() + delay


def merge_releases(rows):
    """Dedupe same game/hash while preserving every provider that found it."""
    unique = {}
    for row in rows:
        gid = str(row.get("gameId", ""))
        digest = str(row.get("infoHash", "")).lower()
        if not gid or not HASH_RE.fullmatch(digest):
            continue
        key = (gid, digest)
        if key not in unique:
            copy = dict(row)
            sources = copy.get("sources") or [copy.get("source", "AuthorizedSite")]
            copy["sources"] = list(dict.fromkeys(str(s)[:128] for s in sources if s))
            copy["source"] = copy["sources"][0] if copy["sources"] else "AuthorizedSite"
            unique[key] = copy
            continue

        existing = unique[key]
        names = list(existing.get("sources") or [existing.get("source", "AuthorizedSite")])
        names.extend(row.get("sources") or [row.get("source", "AuthorizedSite")])
        existing["sources"] = list(dict.fromkeys(str(s)[:128] for s in names if s))
        if not existing.get("files") and row.get("files"):
            existing["files"] = row["files"]
        if not existing.get("size") and row.get("size"):
            existing["size"] = row["size"]
        if len(str(row.get("title", ""))) > len(str(existing.get("title", ""))):
            existing["title"] = row["title"]
    return list(unique.values())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", default="data/igdb-switch-games.json")
    parser.add_argument("--config", default="data/authorized-site-config.json")
    parser.add_argument("--output", default="data/provider-releases.json")
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()

    games = read_json(args.catalog)
    root_config = read_json(args.config)
    sources = load_sources(root_config)

    if not sources:
        write_atomic(args.output, [])
        status = {
            "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
            "providers": [],
            "catalogGames": len(games),
            "matchedGames": 0,
            "uniqueReleases": 0,
            "failedSearches": 0,
            "sourceStats": {},
            "failures": [],
        }
        write_atomic(Path(args.output).with_name("scraper-status.json"), status)
        print("No torrent sources enabled; generated catalog-only provider list.", flush=True)
        print(json.dumps(status, indent=2), flush=True)
        return

    limiter = SourceRateLimiter(sources)

    previous = read_json(args.output) if Path(args.output).exists() else []
    old_by_game_source = {}
    for row in previous:
        gid = str(row.get("gameId", ""))
        for source in row.get("sources") or [row.get("source", "")]:
            if source:
                old_by_game_source.setdefault((gid, str(source)), []).append(row)

    collected, failures = [], []
    matched_games = set()
    source_stats = {
        str(source["name"]): {"searches": 0, "matchedGames": 0, "releases": 0, "failures": 0}
        for source in sources
    }

    def search_one(game, source):
        gid = str(game.get("id", ""))
        name = str(game.get("name", "")).strip()
        source_name = str(source["name"])
        limiter.wait(source)
        source_stats[source_name]["searches"] += 1
        try:
            payload = fetch_search(name, source)
            rows = convert_results(game, payload, source)
            if rows:
                source_stats[source_name]["matchedGames"] += 1
                source_stats[source_name]["releases"] += len(rows)
            return rows, None
        except Exception as error:
            source_stats[source_name]["failures"] += 1
            fallback = old_by_game_source.get((gid, source_name), [])
            return fallback, {
                "gameId": gid,
                "gameName": name,
                "source": source_name,
                "error": str(error)[:300],
            }

    workers = max(1, min(args.workers, len(sources), 16))
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for index, game in enumerate(games, 1):
            gid = str(game.get("id", ""))
            name = str(game.get("name", "")).strip()
            if not gid or not name:
                continue
            futures = [pool.submit(search_one, game, source) for source in sources]
            game_rows = []
            for future in futures:
                rows, failure = future.result()
                game_rows.extend(rows)
                if failure:
                    failures.append(failure)
            if game_rows:
                matched_games.add(gid)
                collected.extend(game_rows)
            if index % 100 == 0:
                print(
                    f"Searched {index}/{len(games)} games across {len(sources)} sources; "
                    f"{len(collected)} raw releases",
                    flush=True,
                )

    releases = merge_releases(collected)
    if not releases:
        raise SystemExit("No authorized Switch torrent results; previous output preserved")
    if failures and all(stats["failures"] == stats["searches"] for stats in source_stats.values()):
        raise SystemExit("All authorized-source searches failed; previous output preserved")

    write_atomic(args.output, releases)
    status = {
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "providers": [source["name"] for source in sources],
        "catalogGames": len(games),
        "matchedGames": len(matched_games),
        "uniqueReleases": len(releases),
        "failedSearches": len(failures),
        "sourceStats": source_stats,
        "failures": failures[:200],
    }
    write_atomic(Path(args.output).with_name("scraper-status.json"), status)
    print(json.dumps(status, indent=2), flush=True)


if __name__ == "__main__":
    main()
