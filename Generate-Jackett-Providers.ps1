param(
    [Parameter(Mandatory=$true)]
    [string]$JackettUrl,

    [Parameter(Mandatory=$true)]
    [string]$ApiKey,

    [string]$ProjectRoot = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'

function Write-Utf8NoBom {
    param([string]$Path, [string]$Content)
    [System.IO.File]::WriteAllText(
        $Path,
        $Content,
        (New-Object System.Text.UTF8Encoding($false))
    )
}

function Get-ProviderStem {
    param([string]$DisplayName, [string]$Id)

    $value = $DisplayName
    if ([string]::IsNullOrWhiteSpace($value)) {
        $value = $Id
    }

    # Keep filenames Make-friendly: no spaces. The filename is the UI ID.
    $parts = @(
        [regex]::Matches($value, '[A-Za-z0-9]+') |
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

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$providerDir = Join-Path $ProjectRoot 'switch\source\providers'
$headerPath  = Join-Path $ProjectRoot 'switch\include\jackett_provider.hpp'

if (-not (Test-Path -LiteralPath $headerPath)) {
    throw "jackett_provider.hpp is missing. Run Apply-SwitchGamesBrowser-Jackett-Streaming.ps1 first."
}

New-Item -ItemType Directory -Force -Path $providerDir | Out-Null

$JackettUrl = $JackettUrl.TrimEnd('/')
$encodedKey = [uri]::EscapeDataString($ApiKey)

$listUrl =
    "$JackettUrl/api/v2.0/indexers/all/results/torznab/api" +
    "?apikey=$encodedKey&t=indexers&configured=true"

Write-Host "Reading configured Jackett indexers..." -ForegroundColor Cyan

$response = Invoke-WebRequest `
    -UseBasicParsing `
    -Uri $listUrl `
    -TimeoutSec 45

[xml]$xml = $response.Content
$nodes = @($xml.SelectNodes('//indexer'))

if ($nodes.Count -eq 0) {
    throw "Jackett returned no configured indexers."
}

$markerPath =
    Join-Path $providerDir '.jackett-generated.txt'

# Delete only files that THIS generator created on its previous run.
if (Test-Path -LiteralPath $markerPath) {
    foreach ($oldName in Get-Content -LiteralPath $markerPath) {
        if ([string]::IsNullOrWhiteSpace($oldName)) {
            continue
        }

        $oldPath =
            Join-Path $providerDir $oldName.Trim()

        if (Test-Path -LiteralPath $oldPath) {
            Remove-Item -LiteralPath $oldPath -Force
        }
    }
}

$existing = @{}
Get-ChildItem -LiteralPath $providerDir -Filter '*.cpp' |
    ForEach-Object {
        $existing[$_.BaseName.ToLowerInvariant()] = $true
    }

$generated = New-Object System.Collections.Generic.List[string]

foreach ($node in $nodes) {
    $id = $node.GetAttribute('id')

    if ([string]::IsNullOrWhiteSpace($id)) {
        continue
    }

    $display = $node.GetAttribute('name')

    if ([string]::IsNullOrWhiteSpace($display)) {
        $display = $node.GetAttribute('title')
    }

    if (
        [string]::IsNullOrWhiteSpace($display) -and
        $node.SelectSingleNode('title')
    ) {
        $display =
            $node.SelectSingleNode('title').InnerText
    }

    if ([string]::IsNullOrWhiteSpace($display)) {
        $display = $id
    }

    $stem = Get-ProviderStem $display $id
    $candidate = $stem

    # Keep the direct provider AND a Jackett duplicate.
    # Example: BitSearch.cpp -> BitSearch-Jackett.cpp
    if ($existing.ContainsKey($candidate.ToLowerInvariant())) {
        $candidate = "$stem-Jackett"
    }

    $suffix = 2
    while ($existing.ContainsKey($candidate.ToLowerInvariant())) {
        $candidate = "$stem-Jackett$suffix"
        ++$suffix
    }

    $existing[$candidate.ToLowerInvariant()] = $true

    $filename = "$candidate.cpp"
    $path = Join-Path $providerDir $filename

    $classTail =
        [regex]::Replace($candidate, '[^A-Za-z0-9_]', '_')

    $className =
        "JackettProvider_$classTail"

    $idLiteral =
        Get-CppStringLiteral $id

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

    Write-Utf8NoBom $path $content
    $generated.Add($filename)

    Write-Host (
        "  {0} -> {1}" -f $id, $filename
    ) -ForegroundColor Green
}

Write-Utf8NoBom `
    $markerPath `
    (($generated -join "`n") + "`n")

Write-Host ''
Write-Host (
    "Generated {0} Jackett provider file(s)." -f
    $generated.Count
) -ForegroundColor Green

Write-Host ''
Write-Host 'Important: Jackett credentials are NOT written into the source.' -ForegroundColor Yellow
Write-Host 'Create this file on the Switch SD card:' -ForegroundColor Cyan
Write-Host '  SD:/switch/SwitchGamesBrowser/jackett.json'
Write-Host ''
Write-Host '{'
Write-Host ('  "url": "' + $JackettUrl + '",')
Write-Host '  "apiKey": "YOUR_JACKETT_API_KEY"'
Write-Host '}'
