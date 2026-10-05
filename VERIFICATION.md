# Verification — 5 October 2026

Passed:

- 12 Python unit tests covering matching, ambiguity, sequel/edition preservation, duplicate and invalid magnets, base32 hashes, rating validity, platform filtering and provider sentinel results.
- Offline end-to-end index generation using fictional fixtures.
- Manifest SHA-256 and byte-count check against generated output.
- Portable C++17 catalog test compiled with `-Wall -Wextra -Werror`; parsing, rating sort, search, minimum score/votes, favourites and duplicate-ID rejection passed.
- Preview JavaScript passed `node --check`.

Not verified:

- The native Switch source and build workflow: devkitPro/devkitA64 not installed here.
- Real catalog/provider requests and RAWG metadata: no live provider collection or API credentials were used.
- Physical Switch rendering, controls, shared fonts, networking, cache recovery and sleep behavior.
- Visual/interaction browser preview: Playwright was present, but its browser executable was absent. An attempted browser test stopped before page launch. No screenshot or browser-interaction success is claimed.

The HTML preview is included for local review. No NRO binary is included, and this package is not represented as a complete downloader/installer.
