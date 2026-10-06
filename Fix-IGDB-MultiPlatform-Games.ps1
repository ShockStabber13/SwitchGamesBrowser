$ErrorActionPreference = 'Stop'

$Path = '.\tools\igdb_catalog.py'
$TestPath = '.\tests\test_igdb_catalog.py'

if (-not (Test-Path -LiteralPath $Path)) {
    throw "Run this from the SwitchGamesBrowser project root. Missing: $Path"
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$text = [System.IO.File]::ReadAllText((Resolve-Path $Path))
Copy-Item -LiteralPath $Path -Destination "$Path.before-platform-filter-fix.bak" -Force

# 1. Correct IGDB array filtering:
#    platforms = (130) means the game includes Nintendo Switch among its platforms.
$text = $text.Replace(
    'f"where id > {last_id} & platforms = {SWITCH_PLATFORM_ID};"',
    'f"where id > {last_id} & platforms = ({SWITCH_PLATFORM_ID});"'
)

if ($text -notmatch 'platforms = \(\{SWITCH_PLATFORM_ID\}\)') {
    throw 'Could not patch the IGDB platform filter.'
}

# 2. Restore base/standalone game types only.
if ($text -notmatch 'CATALOG_GAME_TYPES') {
    $marker = 'SWITCH_PLATFORM_ID = 130'
    if (-not $text.Contains($marker)) {
        throw 'Could not find SWITCH_PLATFORM_ID.'
    }

    $replacement = @'
SWITCH_PLATFORM_ID = 130

# Visible standalone/base game entries.
# main_game, standalone_expansion, remake, remaster,
# expanded_game, port, fork
CATALOG_GAME_TYPES = {0, 4, 8, 9, 10, 11, 12}
'@
    $text = $text.Replace($marker, $replacement)
}

# Remove any Mario-specific exception left over from the earlier workaround.
$text = [regex]::Replace(
    $text,
    '(?ms)\s*# IGDB currently categorizes a few legitimate standalone Switch releases.*?ALWAYS_VISIBLE_GAME_IDS\s*=\s*\{.*?\}\s*',
    "`r`n"
)

# Replace broad "hide only child content" logic with the intended base-game whitelist.
$oldBroad = @'
        # Only hide true child content. Bundles, episodes, seasons,
        # remakes, remasters, expanded games, ports, etc. remain
        # visible if IGDB says they are Nintendo Switch titles.
        if kind in ATTACHED_CONTENT_TYPES:
            continue
'@

$newBase = @'
        if kind not in CATALOG_GAME_TYPES:
            continue
'@

if ($text.Contains($oldBroad)) {
    $text = $text.Replace($oldBroad, $newBase)
}
elseif ($text -notmatch 'if kind not in CATALOG_GAME_TYPES:\s*\r?\n\s*continue') {
    # Handle Mario-specific workaround form if present.
    $text = [regex]::Replace(
        $text,
        '(?ms)        kind = _game_type_id\(game\.get\("gameType"\)\)\s*        game_id = str\(game\.get\("id"\) or ""\)\s*\s*        if \(\s*            kind not in CATALOG_GAME_TYPES\s*            and game_id not in ALWAYS_VISIBLE_GAME_IDS\s*        \):\s*            continue',
@'
        kind = _game_type_id(game.get("gameType"))
        if kind not in CATALOG_GAME_TYPES:
            continue
'@
    )
}

if ($text -match 'ALWAYS_VISIBLE_GAME_IDS') {
    throw 'Old Mario-specific workaround is still present.'
}

if ($text -notmatch 'if kind not in CATALOG_GAME_TYPES:\s*\r?\n\s*continue') {
    throw 'Could not restore base-game-only visibility filtering.'
}

[System.IO.File]::WriteAllText(
    (Resolve-Path $Path),
    $text,
    $Utf8NoBom
)

# 3. Restore the catalog test expectation: bundles remain hidden.
if (Test-Path -LiteralPath $TestPath) {
    $test = [System.IO.File]::ReadAllText((Resolve-Path $TestPath))
    Copy-Item -LiteralPath $TestPath -Destination "$TestPath.before-platform-filter-fix.bak" -Force

    $test = $test.Replace(
        'self.assertEqual(ids, {"1", "4", "5"})',
        'self.assertEqual(ids, {"1", "5"})'
    )

    [System.IO.File]::WriteAllText(
        (Resolve-Path $TestPath),
        $test,
        $Utf8NoBom
    )
}

# 4. Validate Python syntax/tests.
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python -m py_compile $Path
    if ($LASTEXITCODE -ne 0) {
        throw 'igdb_catalog.py failed syntax validation.'
    }

    if (Test-Path -LiteralPath $TestPath) {
        & python -m unittest tests.test_igdb_catalog -v
        if ($LASTEXITCODE -ne 0) {
            throw 'IGDB catalog unit test failed.'
        }
    }
}

Write-Host ''
Write-Host 'IGDB platform filter fixed.' -ForegroundColor Green
Write-Host ''
Write-Host 'Changes:' -ForegroundColor Cyan
Write-Host '  platforms = 130  ->  platforms = (130)'
Write-Host '  Multi-platform Switch games are now included'
Write-Host '  Bundles / collections are hidden again'
Write-Host '  DLC / expansions / packs / updates remain hidden from Browse'
Write-Host ''
Write-Host 'Expected restores include:' -ForegroundColor Cyan
Write-Host '  Mario Kart 8 Deluxe'
Write-Host '  Crash Bandicoot N. Sane Trilogy'
Write-Host '  Need for Speed: Hot Pursuit Remastered'
Write-Host ''
Write-Host 'Now push and rebuild the catalog:' -ForegroundColor Cyan
Write-Host '  git add tools/igdb_catalog.py tests/test_igdb_catalog.py'
Write-Host '  git commit -m "Fix IGDB multi-platform Switch catalog query"'
Write-Host '  git push origin HEAD'
Write-Host '  gh workflow run update-igdb.yml'
