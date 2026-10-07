# SwitchGamesBrowser — Jackett + Cached/New Scrape

This supersedes the earlier streaming-results patch.

## Status

Jackett is considered fully integrated only after:

1. This patch is applied.
2. `Update-Jackett-Providers.ps1` generates the configured Jackett provider `.cpp` files.
3. `jackett.json` exists on the Switch SD card.
4. The NRO builds successfully.
5. At least one Jackett provider is tested successfully on the Switch.

## UI flow

Press **A** on a game:

```text
Cached Scrape
New Scrape
```

### Cached Scrape

Loads the previous final validated torrent list immediately.

It **does not call providers** and **does not call TorBox/AllDebrid again**.

Availability/file results are cached too by reusing the app's existing persistent
`debridStatuses` store. The per-game scrape cache contains the validated release
rows and is tied to the debrid service that created it.

### New Scrape

Runs all enabled providers, then the selected debrid service.

After a successful scrape:

- the final torrent list is saved per game
- the existing debrid status/file cache is saved
- the previous scrape cache is replaced

If a new scrape fails, the previous good cache is kept.

## Cache location

```text
SD:/switch/SwitchGamesBrowser/cache/scrapes/
```

Each game uses a stable hashed filename so titles containing punctuation do not
create invalid SD-card filenames.

## Jackett configuration

Create:

```text
SD:/switch/SwitchGamesBrowser/jackett.json
```

Example:

```json
{
  "url": "http://192.168.1.50:9117",
  "apiKey": "YOUR_JACKETT_API_KEY"
}
```

## Apply

From the SwitchGamesBrowser repository root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Apply-SwitchGamesBrowser-Jackett-Cache.ps1
```

Then synchronize `.cpp` files with your currently configured Jackett indexers:

```powershell
.\Update-Jackett-Providers.ps1 `
  -JackettUrl "http://YOUR-PHONE-IP:9117" `
  -ApiKey "YOUR_JACKETT_API_KEY"
```

Run the updater again whenever you add/remove Jackett indexers.

Existing non-Jackett providers are preserved.

If a Jackett indexer name collides with an existing provider filename, the
generated copy receives a `-Jackett` suffix, for example:

```text
BitSearch.cpp
BitSearch-Jackett.cpp
```

## Build

Build the NRO normally after applying the patch and generating providers.
