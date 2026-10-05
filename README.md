# Switch Games Browser — v0.4 source

Native Nintendo Switch browser for the full IGDB Nintendo Switch catalog with multiple configurable authorized/private torrent sources and optional TorBox or AllDebrid account integration.

## What v0.4 does

- Shows the full Nintendo Switch catalog returned by IGDB, including games that currently have no torrent candidates.
- Search, genre/rating/vote filters, favourites, title/rating/release-date sorting.
- Searches multiple configurable authorized/private torrent JSON APIs for each IGDB game.
- Queries enabled sources in parallel, rate-limited independently per source.
- Deduplicates identical torrents by info hash while preserving every source that found the hash.
- Providers that do not expose a file list can still contribute a candidate; if a provider does return a file list, known non-Switch file results are rejected.
- Supports `TorBox` and `AllDebrid` from `config.json` on the SD card.
- Opening the torrent screen performs a live cache/account check.
- Displays `Cached: True/False` and `Downloaded: True/False`.
- `A` on a torrent adds it to the selected debrid account.
- `Y` on the torrent screen refreshes live debrid state.
- `X` opens the torrent file list.
- File screen supports multi-select and writes selections to `install-queue.json`.
- Live debrid results are cached locally in `debrid-status.json`.
- Compatible with the older libcurl currently shipped by many devkitPro Switch installations.

**v0.4 does not yet download/install selected NSP/NSZ/XCI/XCZ files.** It stops after the live debrid/account/file-list layer and local install queue.

## Multiple authorized torrent sources

Edit `data/authorized-site-config.json`. Each object under `sources` is one provider adapter:

```json
{
  "defaults": {
    "resultsPath": "results",
    "fields": {
      "title": "name",
      "magnet": "magnet",
      "infoHash": "info_hash",
      "size": "size",
      "files": "files"
    },
    "requireFileList": false,
    "validateFileExtensionsWhenPresent": true
  },
  "sources": [
    {
      "name": "PrivateIndexA",
      "enabled": true,
      "searchUrl": "https://YOUR-AUTHORIZED-SITE.example/api/search?q={query}",
      "tokenEnv": "TORRENT_SOURCE_1_TOKEN"
    },
    {
      "name": "PrivateIndexB",
      "enabled": true,
      "searchUrl": "https://ANOTHER-AUTHORIZED-SITE.example/search?q={query}",
      "tokenEnv": "TORRENT_SOURCE_2_TOKEN"
    }
  ]
}
```

Adapters can override `resultsPath`, `fields`, authentication header/prefix, delay, timeout, result limit, and file-list behavior individually.

For private values you do not want committed, GitHub Actions also accepts a `TORRENT_SOURCES_JSON` secret containing an array of source definitions or overrides. A source with the same `name` as the committed config is merged over that config.

Example secret value:

```json
[
  {
    "name": "PrivateIndexA",
    "searchUrl": "https://private.example/api/search?q={query}",
    "token": "PRIVATE_TOKEN",
    "enabled": true
  }
]
```

The workflow also exposes `TORRENT_SOURCE_1_TOKEN` through `TORRENT_SOURCE_8_TOKEN` for sources that use `tokenEnv`.

Use only sources/content you are authorized to access and install.

## Debrid setup on the SD card

After building the NRO, create:

`SD:/switch/SwitchGamesBrowser/config.json`

For TorBox:

```json
{
  "indexUrl": "https://raw.githubusercontent.com/YOUR_USERNAME/YOUR_REPOSITORY/main/data/switch-index.json",
  "debridService": "torbox",
  "debridApiKey": "YOUR_TORBOX_API_KEY"
}
```

For AllDebrid change `debridService` to `alldebrid` and use the corresponding API key.

Keep `config.json` private. The repository ignores `switch/config.json`; never commit an API key.

## Catalog/index workflow

The GitHub Action:

1. Fetches the Nintendo Switch catalog from IGDB.
2. Searches every enabled authorized/private source by game name.
3. Merges/deduplicates torrent candidates by game ID + info hash.
4. Builds `data/switch-index.json` containing **all IGDB games**, including games with an empty `releases` array.
5. Writes source-by-source search statistics to `data/scraper-status.json`.

Required GitHub Actions secrets:

- `IGDB_CLIENT_ID`
- `IGDB_CLIENT_SECRET`
- provider credentials as required by your private sources (`TORRENT_SOURCE_1_TOKEN`, etc.), or one `TORRENT_SOURCES_JSON` secret.

The debrid API key is **not** part of the GitHub index. It stays on the user's SD card.

## Controls

- Browser: D-pad move, L/R page, A details, X search, Y index refresh, ZL sort, ZR favourites.
- Details: A torrents (or reports no torrents), X favourite, B back.
- Torrents: Up/Down select, A add to debrid, X view files, Y refresh debrid status, B back.
- Files: Up/Down select, A toggle, X select all, Y queue selected files, B back.
- Plus: exit.

## Compile for Switch

Install devkitPro/devkitA64/libnx and the required portlibs:

```bash
sudo dkp-pacman -S --needed switch-dev switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-curl switch-mbedtls
export DEVKITPRO=/opt/devkitpro
make -C switch -j$(nproc)
```

If the Windows project path contains spaces, bind-mount the project to a no-space WSL alias first, as you did for v0.3.

The expected output is:

`switch/SwitchGamesBrowser.nro`

Copy it to:

`SD:/switch/SwitchGamesBrowser/SwitchGamesBrowser.nro`

## Local tests

```bash
python -m unittest discover -s tests -v
c++ -std=c++17 -Wall -Wextra -Werror -I switch/include tests/test_catalog.cpp -o /tmp/sgb-catalog
/tmp/sgb-catalog tests/fixture-index.json
c++ -std=c++17 -Wall -Wextra -Werror -I switch/include tests/test_debrid.cpp -o /tmp/sgb-debrid
/tmp/sgb-debrid
```
