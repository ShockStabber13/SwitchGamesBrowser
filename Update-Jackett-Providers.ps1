param(
    [string]$JackettUrl = $env:JACKETT_URL,
    [string]$ApiKey = $env:JACKETT_API_KEY,
    [string]$ProjectRoot = $PSScriptRoot,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Content
    )

    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    [System.IO.File]::WriteAllText(
        $Path,
        $Content,
        (New-Object System.Text.UTF8Encoding($false))
    )
}

function Get-SafeStem {
    param(
        [Parameter(Mandatory=$true)][string]$DisplayName,
        [Parameter(Mandatory=$true)][string]$IndexerId
    )

    $name = $DisplayName
    if ([string]::IsNullOrWhiteSpace($name)) {
        $name = $IndexerId
    }

    # Provider filenames are used as visible provider/filter names.
    # Keep them Make/C++ friendly while preserving recognizable words.
    $parts = @(
        [regex]::Matches($name, '[A-Za-z0-9]+') |
            ForEach-Object { $_.Value }
    )

    if ($parts.Count -eq 0) {
        return 'JackettIndexer'
    }

    return ($parts -join '')
}

function Get-CppStringLiteral {
    param([string]$Value)
    return $Value.Replace('\', '\\').Replace('"', '\"')
}

function Get-JackettIndexerName {
    param($Node)

    foreach ($attr in @('name', 'title')) {
        $value = $Node.GetAttribute($attr)
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            return $value
        }
    }

    $titleNode = $Node.SelectSingleNode('title')
    if ($titleNode -and -not [string]::IsNullOrWhiteSpace($titleNode.InnerText)) {
        return $titleNode.InnerText
    }

    return $Node.GetAttribute('id')
}

# ----------------------------------------------------------------------
# Resolve project / configuration
# ----------------------------------------------------------------------

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Get-Location).Path
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path

$providerDir = Join-Path $ProjectRoot 'switch\source\providers'
$headerPath  = Join-Path $ProjectRoot 'switch\include\jackett_provider.hpp'
$markerPath  = Join-Path $providerDir '.jackett-generated.txt'

if (-not (Test-Path -LiteralPath $providerDir)) {
    throw "Provider directory not found: $providerDir"
}

if (-not (Test-Path -LiteralPath $headerPath)) {
    throw @"
Missing shared Jackett helper:
$headerPath

Apply the Jackett provider patch first, then run this updater.
"@
}

if ([string]::IsNullOrWhiteSpace($JackettUrl)) {
    $JackettUrl = Read-Host 'Jackett URL (example: http://192.168.1.50:9117)'
}

if ([string]::IsNullOrWhiteSpace($ApiKey)) {
    $ApiKey = Read-Host 'Jackett API key'
}

$JackettUrl = $JackettUrl.Trim().TrimEnd('/')
$ApiKey = $ApiKey.Trim()

if (
    $JackettUrl -notmatch '^https?://' -or
    [string]::IsNullOrWhiteSpace($ApiKey)
) {
    throw 'Jackett URL/API key is invalid.'
}

# ----------------------------------------------------------------------
# Ask Jackett which indexers are currently configured.
# ----------------------------------------------------------------------

$encodedKey = [uri]::EscapeDataString($ApiKey)

$listUrl =
    "$JackettUrl/api/v2.0/indexers/all/results/torznab/api" +
    "?apikey=$encodedKey&t=indexers&configured=true"

Write-Host ''
Write-Host 'Reading configured Jackett indexers...' -ForegroundColor Cyan
Write-Host "  $JackettUrl" -ForegroundColor DarkGray

$response = Invoke-WebRequest `
    -UseBasicParsing `
    -Uri $listUrl `
    -TimeoutSec 45

[xml]$xml = $response.Content

$nodes = @($xml.SelectNodes('//indexer'))

if ($nodes.Count -eq 0) {
    throw 'Jackett returned no configured indexers.'
}

# ----------------------------------------------------------------------
# Work out which provider files are ours.
# Existing non-Jackett providers are NEVER deleted.
# ----------------------------------------------------------------------

$previousGenerated = New-Object System.Collections.Generic.HashSet[string] (
    [System.StringComparer]::OrdinalIgnoreCase
)

if (Test-Path -LiteralPath $markerPath) {
    foreach ($line in Get-Content -LiteralPath $markerPath) {
        $trimmed = $line.Trim()
        if ($trimmed) {
            [void]$previousGenerated.Add($trimmed)
        }
    }
}

$directProviderStems = New-Object System.Collections.Generic.HashSet[string] (
    [System.StringComparer]::OrdinalIgnoreCase
)

Get-ChildItem -LiteralPath $providerDir -Filter '*.cpp' |
    ForEach-Object {
        if (-not $previousGenerated.Contains($_.Name)) {
            [void]$directProviderStems.Add($_.BaseName)
        }
    }

# Generate the new desired file set in memory first.
$desired = @{}
$usedStems = New-Object System.Collections.Generic.HashSet[string] (
    [System.StringComparer]::OrdinalIgnoreCase
)

foreach ($stem in $directProviderStems) {
    [void]$usedStems.Add($stem)
}

foreach ($node in $nodes) {
    $indexerId = $node.GetAttribute('id')

    if ([string]::IsNullOrWhiteSpace($indexerId)) {
        continue
    }

    $displayName = Get-JackettIndexerName $node
    $stem = Get-SafeStem $displayName $indexerId
    $candidate = $stem

    # If a direct provider already owns the visible name, preserve it and
    # create a Jackett duplicate beside it.
    if ($usedStems.Contains($candidate)) {
        $candidate = "$stem-Jackett"
    }

    $n = 2
    while ($usedStems.Contains($candidate)) {
        $candidate = "$stem-Jackett$n"
        $n++
    }

    [void]$usedStems.Add($candidate)

    $filename = "$candidate.cpp"

    $classTail =
        [regex]::Replace($candidate, '[^A-Za-z0-9_]', '_')

    $className = "JackettProvider_$classTail"
    $idLiteral = Get-CppStringLiteral $indexerId

    $content = @"
#include "provider.hpp"
#include "jackett_provider.hpp"

#include <string>
#include <vector>

class $className final : public sgb::Provider {
public:
    std::string id() const override
    {
        return "jackett:$idLiteral";
    }

    std::vector<sgb::ProviderResult>
    search(const std::string& query) override
    {
        return sgb_jackett::search(
            "$idLiteral",
            query);
    }
};

SGB_REGISTER_PROVIDER($className);
"@

    $desired[$filename] = @{
        Id = $indexerId
        DisplayName = $displayName
        Content = $content
    }
}

# ----------------------------------------------------------------------
# Show plan
# ----------------------------------------------------------------------

Write-Host ''
Write-Host ('Configured indexers: {0}' -f $desired.Count) -ForegroundColor Green
Write-Host ''

foreach ($name in ($desired.Keys | Sort-Object)) {
    $item = $desired[$name]
    Write-Host ('  {0}  <=  {1}' -f $name, $item.Id)
}

$filesToDelete = @(
    $previousGenerated |
        Where-Object { -not $desired.ContainsKey($_) }
)

if ($filesToDelete.Count -gt 0) {
    Write-Host ''
    Write-Host 'Old generated providers to remove:' -ForegroundColor Yellow
    foreach ($name in ($filesToDelete | Sort-Object)) {
        Write-Host "  $name"
    }
}

if ($DryRun) {
    Write-Host ''
    Write-Host 'Dry run only; no files changed.' -ForegroundColor Yellow
    exit 0
}

# ----------------------------------------------------------------------
# Remove only old files previously generated by this updater.
# ----------------------------------------------------------------------

foreach ($name in $previousGenerated) {
    $path = Join-Path $providerDir $name
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

# ----------------------------------------------------------------------
# Write current Jackett provider files.
# ----------------------------------------------------------------------

foreach ($name in $desired.Keys) {
    $path = Join-Path $providerDir $name
    Write-Utf8NoBom $path $desired[$name].Content
}

$markerContent =
    (($desired.Keys | Sort-Object) -join "`n") + "`n"

Write-Utf8NoBom $markerPath $markerContent

Write-Host ''
Write-Host 'Jackett provider files are now synchronized.' -ForegroundColor Green
Write-Host ''
Write-Host 'Existing non-Jackett providers were left untouched.' -ForegroundColor Green
Write-Host ''
Write-Host 'Rebuild the NRO after running this script.' -ForegroundColor Cyan
