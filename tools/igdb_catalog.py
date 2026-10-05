#!/usr/bin/env python3
"""Fetch the Nintendo Switch (IGDB platform 130) catalog."""
import datetime as dt
import json
import os
import time
import urllib.parse
import urllib.request

MAX_BYTES = 128 * 1024 * 1024
GAMES_URL = "https://api.igdb.com/v4/games"
TOKEN_URL = "https://id.twitch.tv/oauth2/token"
SWITCH_PLATFORM_ID = 130


def _post(url, body=None, headers=None, content_type="text/plain; charset=utf-8"):
    data = body.encode() if isinstance(body, str) else None
    req = urllib.request.Request(url, data=data, method="POST")
    if data is not None:
        req.add_header("Content-Type", content_type)
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    req.add_header("Accept", "application/json")
    req.add_header("User-Agent", "SwitchGamesBrowser/0.4")
    with urllib.request.urlopen(req, timeout=120) as response:
        raw = response.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("IGDB response exceeds size limit")
    return json.loads(raw)


def get_token(client_id, client_secret):
    if not client_id or not client_secret:
        raise ValueError("Set IGDB_CLIENT_ID and IGDB_CLIENT_SECRET")
    result = _post(
        TOKEN_URL,
        body=urllib.parse.urlencode({
            "client_id": client_id,
            "client_secret": client_secret,
            "grant_type": "client_credentials",
        }),
        content_type="application/x-www-form-urlencoded",
    )
    token = result.get("access_token")
    if not token:
        raise ValueError("IGDB returned no access token")
    return token


def igdb_request(headers, body):
    for attempt in range(1, 8):
        try:
            result = _post(GAMES_URL, body=body, headers=headers)
            time.sleep(0.35)
            return result
        except Exception as error:
            if attempt >= 7:
                raise RuntimeError(f"IGDB request failed after 7 attempts: {error}") from None
            delay = min(20, 2 ** attempt)
            print(f"IGDB retry {attempt}/7 in {delay}s: {error}", flush=True)
            time.sleep(delay)


def _switch_release_date(game):
    timestamps = []
    for release in game.get("release_dates") or []:
        try:
            if int(release.get("platform", -1)) == SWITCH_PLATFORM_ID and release.get("date"):
                timestamps.append(int(release["date"]))
        except (TypeError, ValueError):
            pass
    if not timestamps and game.get("first_release_date"):
        try:
            timestamps.append(int(game["first_release_date"]))
        except (TypeError, ValueError):
            pass
    if not timestamps:
        return ""
    return dt.datetime.fromtimestamp(min(timestamps), dt.timezone.utc).strftime("%Y-%m-%d")


def _cover_url(game):
    image_id = (game.get("cover") or {}).get("image_id", "")
    return (
        f"https://images.igdb.com/igdb/image/upload/t_cover_big/{image_id}.jpg"
        if image_id else ""
    )


def fetch_switch_games(client_id, client_secret):
    token = get_token(client_id, client_secret)
    headers = {"Client-ID": client_id, "Authorization": "Bearer " + token}
    games = []
    last_id = 0

    while True:
        body = (
            "fields id,name,slug,game_type,parent_game,genres.name,platforms,"
            "release_dates.date,release_dates.platform,first_release_date,"
            "rating,rating_count,aggregated_rating,aggregated_rating_count,"
            "cover.image_id,summary;"
            f"where id > {last_id} & platforms = {SWITCH_PLATFORM_ID};"
            "sort id asc; limit 500;"
        )
        page = igdb_request(headers, body)
        if not page:
            break

        for game in page:
            gid = int(game["id"])
            if gid <= last_id:
                continue
            last_id = gid
            genres = sorted({
                row.get("name", "").strip()
                for row in (game.get("genres") or [])
                if isinstance(row, dict) and row.get("name")
            })
            rating_count = int(game.get("rating_count") or 0)
            rating = game.get("rating")
            games.append({
                "id": str(gid),
                "name": game.get("name", "").strip(),
                "slug": game.get("slug", ""),
                "gameType": game.get("game_type"),
                "parentGame": game.get("parent_game"),
                "genres": genres,
                "releaseDate": _switch_release_date(game),
                "rating": round(float(rating), 1) if rating is not None and rating_count > 0 else None,
                "ratingCount": rating_count,
                "aggregatedRating": game.get("aggregated_rating"),
                "aggregatedRatingCount": int(game.get("aggregated_rating_count") or 0),
                "cover": _cover_url(game),
                "summary": game.get("summary", "") or "",
            })

        if len(page) < 500:
            break

    games = [game for game in games if game["name"]]
    if not games:
        raise ValueError("IGDB returned no Nintendo Switch games")
    return games


def main():
    games = fetch_switch_games(
        os.getenv("IGDB_CLIENT_ID", ""),
        os.getenv("IGDB_CLIENT_SECRET", ""),
    )
    path = os.getenv("IGDB_OUTPUT", "data/igdb-switch-games.json")
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(games, handle, ensure_ascii=False, separators=(",", ":"))
    print(f"Wrote {len(games)} Nintendo Switch IGDB games -> {path}", flush=True)


if __name__ == "__main__":
    main()
