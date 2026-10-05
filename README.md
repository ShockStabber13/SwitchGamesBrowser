# Switch Games Browser — v0.1 source prototype

A native Nintendo Switch game browser inspired by Nicholas Kwek's VideoGamesBrowser and MoviesAndSeries projects. The GitHub builder prepares a compact index; the Switch refreshes it, caches covers, and searches/sorts locally.

## Current features

- Four-column, two-row controller-driven game cards, black background, #15BE19 accents, white text.
- Search; genre, minimum rating, minimum vote count and favourites filters.
- Title, rating and release-date sorting. Missing ratings sort below rated games.
- Persistent browse state and favourites; SD cover cache and bounded decoded texture cache.
- Background HTTPS refresh with SHA-256 manifest validation, schema validation and recovery cache.
- Multiple releases per matched game; magnet selection saved to `magnet-queue.json`.
- Daily GitHub catalog update, weekly RAWG metadata refresh when a secret is supplied.
- Optional APIBay collector adapted from the supplied Android project's JSON provider.

**This version does not download torrents or install packages.** The saved magnet queue is an integration point for a later downloader/installer. No connection to pipensx's internal queue is claimed. Switch-specific rendering, compilation and runtime behavior still require an actual devkitPro build and physical console test; no tested NRO is included.

## Quick offline test

Python 3.10+ and a C++17 compiler:

```bash
python -m unittest discover -s tests -v
python tools/build_index.py --source tests/fixture-releases.json --metadata tests/fixture-metadata.json --output /tmp/switch-browser-demo
c++ -std=c++17 -Wall -Wextra -Werror -I switch/include tests/test_catalog.cpp -o /tmp/sgb-test
/tmp/sgb-test tests/fixture-index.json
```

Fixtures are fictional test games and invented scores. The browser preview starts with these clearly labeled examples, not a real game catalog. Open `preview/index.html` and load your generated `switch-index.json` to inspect a real index.

## GitHub setup

1. Create a repository and upload this directory's contents, including `.github/workflows`. No repository is created automatically by this package.
2. In Settings → Actions → General, allow the workflow write access if your account requires it. The update workflow declares `contents: write`.
3. For ratings, obtain your own RAWG key and add the Actions secret `RAWG_API_KEY`. Do not put API credentials in the Switch app or repository. Without a key the imported catalog still works, with unmatched games marked unrated.
4. Run **Update Switch index** manually once. Afterward it runs daily at **04:30 Singapore time** (20:30 UTC). RAWG metadata refreshes weekly on Sunday UTC, on the first run with a key, or through the manual refresh-ratings option.
5. RAWG's full metadata cache is stored in the Actions cache, not committed. A missing cache triggers a fresh fetch. The job commits only the project index, manifest and any manually maintained overrides.
6. Use your repository's raw URL for `data/switch-index.json` in the SD `config.json`.

Private repositories need a separate authenticated delivery mechanism; this prototype's raw-index refresh currently supports public raw GitHub URLs only. Decide how to expose your project-specific index before using private data. RAWG requires attribution and limits redistribution; review https://rawg.io/apidocs and your plan's terms before publishing a public index. The app exposes each matched game's RAWG attribution URL in its detail page, but platform-specific hyperlink/attribution requirements need review for your distribution.

### Catalog sources and matching

The default source is Langegen/switch-games, fetched over HTTPS. This is importing its prepared RuTracker catalog, not scraping RuTracker on the console. Its network availability and results have not been tested here.

RAWG game-list ratings are **RAWG user scores**, scaled from 0–5 to 0–100. They are not claimed to be Switch-specific critic scores. The optional Metacritic value is labeled platform unspecified and is not used by the browser's score sorting. RAWG's list release date can also be the game's general release date, not its Switch port date. A later platform-specific provider can fill in those fields.

Matching is exact after accent/case normalization and removing recognized release tags. Sequel numbers and edition words remain. Ambiguous or unmatched releases stay visible but unrated. Region variants are grouped only after matching to the same metadata ID, or when they share an explicit source title ID; no Nintendo base/update/DLC title-ID bit manipulation is assumed.

`data/match-overrides.json` maps an info hash or source title ID to a RAWG game ID:

```json
{"0123456789abcdef0123456789abcdef01234567": 12345}
```

Review mapping decisions. Update/DLC releases are not guessed as standalone games or silently auto-installed. Some release titles may require explicit overrides.

### Optional direct provider collection

```bash
python tools/scrape_torrents.py --query "Nintendo Switch" --query "Switch homebrew" --output data/provider-releases.json
python tools/build_index.py --source data/provider-releases.json --fetch-rawg
```

This adapter follows the Android project's APIBay pattern, collects Switch/NSP/NSZ/XCI/XCZ-marked results and deduplicates info hashes. It has fixture tests, but the provider was not contacted during development. No peer-health, platform authenticity, malware or content authorization guarantee follows from a title marker. Use sources and content you are permitted to access.

The daily workflow imports Langegen by default. To schedule your own provider, add the collector command before the build command and pass `--source data/provider-releases.json`. Do not assume that every website can be scraped from GitHub runners; authentication and access controls may require a separate server.

## Compile for Switch

Install devkitPro/devkitA64/libnx and the portlibs:

```bash
sudo dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-curl switch-mbedtls
export DEVKITPRO=/opt/devkitpro
make -C switch -j2
```

Alternatively run **Build Switch browser** in GitHub Actions and download its NRO artifact if the build succeeds. This workflow was authored but not executed here.

The Makefile uses the official switch-examples application template and devkitPro pkg-config to resolve SDL/image/font/curl link dependencies. The code uses libnx shared fonts, so no proprietary font files are bundled. A CA trust bundle copied from the development environment is included in ROMFS; refresh it from a trusted OS certificate store when maintaining the project.

## Install after a successful build

Copy:

- `switch/SwitchGamesBrowser.nro` → `SD:/switch/SwitchGamesBrowser/SwitchGamesBrowser.nro`
- `switch/config.example.json` → `SD:/switch/SwitchGamesBrowser/config.json`, then edit the repository URL.
- Optional: your generated `data/switch-index.json` → `SD:/switch/SwitchGamesBrowser/switch-index.json` for offline startup.

Hold R while launching a game to enter hbmenu application mode. Applet mode is rejected. Press Y to refresh.

Controls: D-pad move; L/R page; A details/save selected magnet; X search or favourite on details; Y refresh; ZL cycle sort; ZR toggle favourites; minus cycle minimum score; left stick click cycle genre; right stick click cycle minimum votes; B back/reset; plus exit. Touch selection is not implemented in this prototype.

## Verification and remaining work

Portable builder/provider tests and C++ parsing/sort/filter validation can run without Switch SDKs. They cover ambiguous titles, sequels, malformed/duplicate magnets, multiple releases, missing ratings and filtering. The SDK, RAWG credentials, live provider queries and physical Switch were unavailable in the development environment.

Next integration work: validate the NRO build; test refresh/sleep/SD behavior on hardware; add touch navigation and collection folders; wire selected magnets into a downloader/debrid backend; then add package installation. Those features are not represented as completed.

## Dependencies

Vendored nlohmann/json 3.11.3 includes its MIT notice in `switch/include/json.hpp`. Makefile template derived from switchbrew/switch-examples. Runtime libraries retain their upstream licenses; see `THIRD_PARTY.md`. Android sources were used as design references rather than copied into this package.
