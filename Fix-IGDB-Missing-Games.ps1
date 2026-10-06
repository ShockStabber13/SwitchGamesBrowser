$ErrorActionPreference = 'Stop'

$Path = '.\tools\igdb_catalog.py'

if (-not (Test-Path -LiteralPath $Path)) {
    throw "Run this from the SwitchGamesBrowser project root. Missing: $Path"
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$text = [System.IO.File]::ReadAllText((Resolve-Path $Path))

Copy-Item -LiteralPath $Path -Destination "$Path.before-complete-catalog-fix.bak" -Force

# ------------------------------------------------------------
# 1. Remove the narrow "visible type" whitelist.
#
# Goal: hide only content that is actually a child of a game:
# DLC/add-on, expansion, pack and update.
# Everything else returned by IGDB for Nintendo Switch stays visible.
# ------------------------------------------------------------

$text = [regex]::Replace(
    $text,
    '(?ms)# Visible standalone titles\.\s*# main_game, standalone_expansion, remake, remaster,\s*# expanded_game, port, fork\s*CATALOG_GAME_TYPES\s*=\s*\{[^}]*\}\s*',
    ''
)

# Remove the temporary Mario Kart one-off workaround if it was applied.
$text = [regex]::Replace(
    $text,
    '(?ms)\s*# IGDB currently categorizes a few legitimate standalone Switch releases.*?ALWAYS_VISIBLE_GAME_IDS\s*=\s*\{.*?\}\s*',
    "`r`n"
)

# Old normal filter.
$text = $text.Replace(
@'
        kind = _game_type_id(game.get("gameType"))
        if kind not in CATALOG_GAME_TYPES:
            continue
'@,
@'
        kind = _game_type_id(game.get("gameType"))

        # Only hide true child content. Bundles, episodes, seasons,
        # remakes, remasters, expanded games, ports, etc. remain
        # visible if IGDB says they are Nintendo Switch titles.
        if kind in ATTACHED_CONTENT_TYPES:
            continue
'@
)

# Previous Mario-specific filter, if that patch was already applied.
$text = [regex]::Replace(
    $text,
    '(?ms)        kind = _game_type_id\(game\.get\("gameType"\)\)\s*        game_id = str\(game\.get\("id"\) or ""\)\s*\s*        if \(\s*            kind not in CATALOG_GAME_TYPES\s*            and game_id not in ALWAYS_VISIBLE_GAME_IDS\s*        \):\s*            continue',
@'
        kind = _game_type_id(game.get("gameType"))

        # Only hide true child content. Bundles, episodes, seasons,
        # remakes, remasters, expanded games, ports, etc. remain
        # visible if IGDB says they are Nintendo Switch titles.
        if kind in ATTACHED_CONTENT_TYPES:
            continue
'@
)

if ($text -match 'CATALOG_GAME_TYPES|ALWAYS_VISIBLE_GAME_IDS') {
    throw 'Old whitelist logic is still present; no file was saved.'
}

if ($text -notmatch 'if kind in ATTACHED_CONTENT_TYPES:\s*\r?\n\s*continue') {
    throw 'Could not install the new visible-game rule; no file was saved.'
}

[System.IO.File]::WriteAllText(
    (Resolve-Path $Path),
    $text,
    $Utf8NoBom
)

# ------------------------------------------------------------
# 2. Update the catalog unit test if present.
# ------------------------------------------------------------

$TestPath = '.\tests\test_igdb_catalog.py'

if (Test-Path -LiteralPath $TestPath) {
    $test = [System.IO.File]::ReadAllText((Resolve-Path $TestPath))
    Copy-Item -LiteralPath $TestPath -Destination "$TestPath.before-complete-catalog-fix.bak" -Force

    # Bundles should now remain visible.
    $test = $test.Replace(
        'self.assertEqual(ids, {"1", "5"})',
        'self.assertEqual(ids, {"1", "4", "5"})'
    )

    [System.IO.File]::WriteAllText(
        (Resolve-Path $TestPath),
        $test,
        $Utf8NoBom
    )
}

# ------------------------------------------------------------
# 3. Syntax/tests when Python is available.
# ------------------------------------------------------------

$python = Get-Command python -ErrorAction SilentlyContinue

if ($python) {
    & python -m py_compile $Path
    if ($LASTEXITCODE -ne 0) {
        throw 'igdb_catalog.py failed Python syntax validation.'
    }

    if (Test-Path -LiteralPath $TestPath) {
        & python -m unittest tests.test_igdb_catalog -v
        if ($LASTEXITCODE -ne 0) {
            throw 'IGDB catalog unit test failed.'
        }
    }
}

Write-Host ''
Write-Host 'IGDB catalog visibility fixed.' -ForegroundColor Green
Write-Host ''
Write-Host 'Now hidden:' -ForegroundColor Cyan
Write-Host '  DLC / add-ons'
Write-Host '  Expansions'
Write-Host '  Packs'
Write-Host '  Updates'
Write-Host ''
Write-Host 'Now kept:' -ForegroundColor Cyan
Write-Host '  Main games'
Write-Host '  Bundles'
Write-Host '  Standalone expansions'
Write-Host '  Episodes / seasons'
Write-Host '  Remakes / remasters'
Write-Host '  Expanded games'
Write-Host '  Ports / forks'
Write-Host '  Other standalone Switch entries'
Write-Host ''
Write-Host 'Backup:' -ForegroundColor Yellow
Write-Host "  $Path.before-complete-catalog-fix.bak"
Write-Host ''
Write-Host 'Regenerate the IGDB catalog:' -ForegroundColor Cyan
Write-Host '  gh workflow run update-igdb.yml'
