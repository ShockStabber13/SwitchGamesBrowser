# MoviesAndSeries scraper port

Enabled providers match the supplied Android app: ThePirateBay (APIBay), Bitsearch, LimeTorrents and Knaben. CloudTorrents, RarBG and TorrentQuest were commented out in its active search flows and remain excluded.

Update Switch index now runs these collectors before building the index. `data/scraper-config.json` contains provider selection, search queries and bounded pagination (3 pages per query). This is a search-based collection, not an exhaustive site crawl or guarantee of every Switch game. Broader coverage needs targeted game queries and later IGDB integration.

Results retain provider names, and duplicates merge by info hash. A failing or empty provider retains prior rows where available. A failure of every provider aborts the run. `data/scraper-status.json` records source outcomes; examine it after a workflow run. Current page selectors were ported from the user's Android code and checked with fixtures; no live provider access is claimed. Access blocks or changed site markup may need further fixes.

Because this switches away from the previous Langegen catalog, the workflow explicitly accepts a smaller resulting catalog through `--allow-shrink`. Existing entries without matches in our sources can disappear from this release-based index. It does not yet include every IGDB game independently of download availability.

The existing ratings backend remains RAWG in this provider-only patch. Switching to IGDB, downloading cover packs and the portrait UI layout are separate changes and are not implemented by this patch. The user selected IGDB as the intended replacement; no API keys are bundled.

Apply the patch with `Update-SwitchGamesBrowser-Scraper.ps1`. Then run a NEW Update Switch index workflow and refresh with Y. No NRO change is required for this provider patch. The script stages only files named in this patch. It does not replace the NRO source or undo the curl build fix.
