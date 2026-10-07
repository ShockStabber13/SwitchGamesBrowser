$ErrorActionPreference = "Stop"

$Main = ".\switch\source\main.cpp"

if (-not (Test-Path -LiteralPath $Main)) {
    throw "Run this from the SwitchGamesBrowser project root. Could not find $Main"
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$path = (Resolve-Path -LiteralPath $Main).Path

$backup = "$path.before-cache-scope-fix.bak"
if (-not (Test-Path -LiteralPath $backup)) {
    Copy-Item -LiteralPath $path -Destination $backup
}

$text = [System.IO.File]::ReadAllText($path)

# v3 accidentally inserted this optional status line after multiple
# "status = result.message;" sites. Only the live-search completion block
# has scrapeSucceeded/scrapeCached in scope, so the extra copies fail to build.
#
# The line is cosmetic only. Cache saving itself happens separately, so it is
# safest to remove every copy.
$pattern = '(?m)^[ \t]*if\s*\(\s*scrapeSucceeded\s*&&\s*!scrapeCached\s*\)\s*status\s*\+=\s*"\s*\(cache save failed\)\s*";[ \t]*\r?\n?'

$matches = [regex]::Matches($text, $pattern)

if ($matches.Count -eq 0) {
    Write-Host "No stray cache-status lines found. Nothing changed." -ForegroundColor Yellow
    exit 0
}

$fixed = [regex]::Replace($text, $pattern, "")

[System.IO.File]::WriteAllText($path, $fixed, $Utf8NoBom)

Write-Host ""
Write-Host ("Removed {0} out-of-scope cache-status line(s)." -f $matches.Count) -ForegroundColor Green
Write-Host "The actual TorBox/AllDebrid cache save logic was left untouched." -ForegroundColor Green
Write-Host ""
Write-Host "Rebuild with:" -ForegroundColor Cyan
Write-Host "  cd ~/SwitchGamesBrowserBuild/switch"
Write-Host "  make -j`$(nproc)"
