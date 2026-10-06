#!/usr/bin/env python3
import concurrent.futures
import datetime as dt
import hashlib
import io
import json
from pathlib import Path
import urllib.request

from PIL import Image, ImageOps

CATALOG = Path("data/igdb-switch-games.json")
OUT = Path("data")

WIDTH = 150
HEIGHT = 212
SHARDS = 16
WORKERS = 16


def image_url(game):
    return (
        game.get("cover")
        or game.get("coverUrl")
        or game.get("imageUrl")
        or ""
    )


def candidates(url):
    if not url:
        return []

    if url.startswith("//"):
        url = "https:" + url

    result = [url]

    if "/t_cover_big_2x/" in url:
        result.append(
            url.replace(
                "/t_cover_big_2x/",
                "/t_cover_big/",
            )
        )

    return result


def download(url):
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "SwitchGamesBrowser/1.0"},
    )

    with urllib.request.urlopen(req, timeout=30) as response:
        data = response.read(8 * 1024 * 1024 + 1)

    if len(data) > 8 * 1024 * 1024:
        raise ValueError("Image too large")

    return data


def make_cover(game):
    gid = str(game.get("id", "")).strip()
    url = image_url(game)

    if not gid or not url:
        return None

    key = gid if gid.startswith("igdb-") else "igdb-" + gid

    for candidate in candidates(url):
        try:
            raw = download(candidate)

            with Image.open(io.BytesIO(raw)) as image:
                image = image.convert("RGB")

                image = ImageOps.fit(
                    image,
                    (WIDTH, HEIGHT),
                    method=Image.Resampling.LANCZOS,
                )

                out = io.BytesIO()

                image.save(
                    out,
                    format="JPEG",
                    quality=82,
                    optimize=True,
                )

                return key, out.getvalue()

        except Exception:
            continue

    return None


def main():
    games = json.loads(CATALOG.read_text(encoding="utf-8"))

    covers = {}

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=WORKERS
    ) as pool:
        futures = [
            pool.submit(make_cover, game)
            for game in games
        ]

        for done, future in enumerate(
            concurrent.futures.as_completed(futures),
            1,
        ):
            result = future.result()

            if result:
                key, data = result
                covers[key] = data

            if done % 100 == 0:
                print(
                    f"Covers processed: {done}/{len(games)} "
                    f"successful={len(covers)}",
                    flush=True,
                )

    shards = [bytearray() for _ in range(SHARDS)]
    entries = {}

    for key in sorted(covers):
        data = covers[key]

        shard = int(
            hashlib.sha1(key.encode()).hexdigest()[0],
            16,
        )

        offset = len(shards[shard])
        shards[shard].extend(data)

        entries[key] = {
            "pack": shard,
            "offset": offset,
            "size": len(data),
        }

    # Remove previous packs.
    for old in OUT.glob("covers-*.pack"):
        old.unlink()

    packs = {}

    for i, blob in enumerate(shards):
        name = f"covers-{i:02x}.pack"
        path = OUT / name
        path.write_bytes(blob)

        packs[str(i)] = {
            "file": name,
            "bytes": len(blob),
            "sha256": hashlib.sha256(blob).hexdigest(),
        }

    manifest = {
        "schemaVersion": 1,
        "generatedAt": dt.datetime.now(
            dt.timezone.utc
        ).isoformat(),
        "width": WIDTH,
        "height": HEIGHT,
        "format": "jpeg",
        "catalogGames": len(games),
        "imageCount": len(entries),
        "missingImages": len(games) - len(entries),
        "packs": packs,
        "entries": entries,
    }

    (OUT / "covers-manifest.json").write_text(
        json.dumps(
            manifest,
            separators=(",", ":"),
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )

    print(
        f"Built {len(entries)} covers "
        f"across {SHARDS} packs."
    )


if __name__ == "__main__":
    main()
