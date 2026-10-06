#!/usr/bin/env python3
import datetime as dt
import hashlib
import json
from pathlib import Path

INPUT = Path("data/provider-releases.json")
OUT = Path("data")
SHARD_COUNT = 16
MAX_SHARD_BYTES = 90 * 1024 * 1024


def shard_for(game_id):
    digest = hashlib.sha1(
        game_id.encode("utf-8")
    ).digest()

    return digest[0] % SHARD_COUNT


def main():
    rows = json.loads(
        INPUT.read_text(encoding="utf-8")
    )

    if not isinstance(rows, list):
        raise SystemExit(
            "provider-releases.json must contain a JSON array"
        )

    shards = [
        {}
        for _ in range(SHARD_COUNT)
    ]

    release_count = 0

    for row in rows:
        if not isinstance(row, dict):
            continue

        game_id = str(
            row.get("gameId", "")
        ).strip()

        if not game_id:
            continue

        shard = shard_for(game_id)

        # gameId/gameName are redundant once grouped.
        release = dict(row)
        release.pop("gameId", None)
        release.pop("gameName", None)

        shards[shard].setdefault(
            game_id,
            []
        ).append(release)

        release_count += 1

    # Remove old shards before rebuilding.
    for old in OUT.glob("torrent-index-*.json"):
        old.unlink()

    manifest_shards = []
    total_games = 0

    for index, games in enumerate(shards):
        name = f"torrent-index-{index:02x}.json"
        path = OUT / name

        payload = {
            "schemaVersion": 1,
            "shard": index,
            "games": games,
        }

        raw = json.dumps(
            payload,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")

        if len(raw) > MAX_SHARD_BYTES:
            raise RuntimeError(
                f"{name} is too large: "
                f"{len(raw) / 1024 / 1024:.2f} MiB"
            )

        path.write_bytes(raw)

        count = sum(
            len(releases)
            for releases in games.values()
        )

        total_games += len(games)

        manifest_shards.append({
            "id": index,
            "file": name,
            "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "gameCount": len(games),
            "releaseCount": count,
        })

        print(
            f"{name}: "
            f"{len(games)} games, "
            f"{count} releases, "
            f"{len(raw) / 1024 / 1024:.2f} MiB",
            flush=True,
        )

    manifest = {
        "schemaVersion": 1,
        "generatedAt": dt.datetime.now(
            dt.timezone.utc
        ).isoformat(),
        "shardCount": SHARD_COUNT,
        "gameCount": total_games,
        "releaseCount": release_count,
        "shards": manifest_shards,
    }

    (OUT / "torrent-manifest.json").write_text(
        json.dumps(
            manifest,
            ensure_ascii=False,
            separators=(",", ":"),
        ),
        encoding="utf-8",
    )

    print(
        f"Built {SHARD_COUNT} torrent shards "
        f"with {release_count} releases."
    )


if __name__ == "__main__":
    main()
