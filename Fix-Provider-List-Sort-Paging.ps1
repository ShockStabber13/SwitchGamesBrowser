$ErrorActionPreference = 'Stop'

$RegistryPath = '.\switch\source\provider_registry.cpp'
$MainPath     = '.\switch\source\main.cpp'

foreach ($path in @($RegistryPath, $MainPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing $path. Run this from the SwitchGamesBrowser repository root."
    }
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Read-Normalized([string]$Path) {
    return [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path)).Replace("`r`n", "`n")
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText(
        (Resolve-Path -LiteralPath $Path),
        $Text,
        $Utf8NoBom
    )
}

function Backup-Once([string]$Path) {
    $backup = "$Path.before-provider-sort-paging.bak"
    if (-not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $Path -Destination $backup
    }
}

Backup-Once $RegistryPath
Backup-Once $MainPath

# -------------------------------------------------------------------
# 1. Sort the runtime provider registry by provider ID before exposing it.
#    Provider IDs are already normalized to lowercase by providerIdFromFile().
# -------------------------------------------------------------------
$registry = Read-Normalized $RegistryPath

$oldRegistry = @'
const std::vector<ProviderEntry>&
providerRegistry() {
    return mutableRegistry();
}
'@

$newRegistry = @'
const std::vector<ProviderEntry>&
providerRegistry() {
    auto& registry =
        mutableRegistry();

    std::sort(
        registry.begin(),
        registry.end(),
        [](const ProviderEntry& a, const ProviderEntry& b) {
            return a.id < b.id;
        }
    );

    return registry;
}
'@

if ($registry.Contains($newRegistry)) {
    Write-Host 'Provider registry sorting is already installed.' -ForegroundColor Yellow
}
elseif ($registry.Contains($oldRegistry)) {
    $registry = $registry.Replace($oldRegistry, $newRegistry)
    Write-Utf8NoBom $RegistryPath $registry
    Write-Host 'Sorted providerRegistry() alphabetically.' -ForegroundColor Green
}
else {
    throw 'provider_registry.cpp does not match the uploaded project. No registry change was written.'
}

# -------------------------------------------------------------------
# 2. Add L/R page jumps to the exact current Providers input handler.
# -------------------------------------------------------------------
$main = Read-Normalized $MainPath

$oldInput = @'
                if (
                    (keys & HidNpadButton_Down) &&
                    providerCursor + 1 < providers.size()
                ) {
                    ++providerCursor;
                }

                if (keys & HidNpadButton_A) {
'@

$newInput = @'
                if (
                    (keys & HidNpadButton_Down) &&
                    providerCursor + 1 < providers.size()
                ) {
                    ++providerCursor;
                }

                if (keys & HidNpadButton_L) {
                    providerCursor =
                        providerCursor >= 8
                            ? providerCursor - 8
                            : 0;
                }

                if (keys & HidNpadButton_R) {
                    providerCursor =
                        std::min(
                            providerCursor + 8,
                            providers.size() - 1
                        );
                }

                if (keys & HidNpadButton_A) {
'@

if ($main.Contains($newInput)) {
    Write-Host 'Provider L/R paging is already installed.' -ForegroundColor Yellow
}
elseif ($main.Contains($oldInput)) {
    $main = $main.Replace($oldInput, $newInput)
}
else {
    throw 'Could not find the exact Providers input block in main.cpp. No main.cpp change was written.'
}

# -------------------------------------------------------------------
# 3. Show total providers and current position in the exact renderer.
# -------------------------------------------------------------------
$oldRender = @'
            label(
                renderer,big,
                "SCRAPE PROVIDERS",
                32,70,1200,green
            );

            label(
                renderer,small,
                "A Toggle  |  X Enable All  |  Y Disable All  |  B Back",
                32,112,1200,muted
            );
'@

$newRender = @'
            const std::string providerHeader =
                "SCRAPE PROVIDERS (" +
                std::to_string(providers.size()) +
                " total)";

            const std::string providerControls =
                providers.empty()
                    ? "0 / 0  |  A Toggle  |  L/R Page  |  X All  |  Y None  |  B Back"
                    : std::to_string(providerCursor + 1) +
                        " / " +
                        std::to_string(providers.size()) +
                        "  |  A Toggle  |  L/R Page  |  X All  |  Y None  |  B Back";

            label(
                renderer,big,
                providerHeader,
                32,70,1200,green
            );

            label(
                renderer,small,
                providerControls,
                32,112,1200,muted
            );
'@

if ($main.Contains($newRender)) {
    Write-Host 'Provider count/position display is already installed.' -ForegroundColor Yellow
}
elseif ($main.Contains($oldRender)) {
    $main = $main.Replace($oldRender, $newRender)
}
else {
    throw 'Could not find the exact Providers renderer block in main.cpp. No main.cpp change was written.'
}

Write-Utf8NoBom $MainPath $main

Write-Host ''
Write-Host 'Provider list fix applied.' -ForegroundColor Green
Write-Host 'Changes:'
Write-Host '  - Runtime providers sorted alphabetically'
Write-Host '  - L/R jumps 8 providers per press'
Write-Host '  - Providers screen shows current / total and total count'
Write-Host ''
Write-Host 'Backups:' -ForegroundColor Cyan
Write-Host "  $RegistryPath.before-provider-sort-paging.bak"
Write-Host "  $MainPath.before-provider-sort-paging.bak"
Write-Host ''
Write-Host 'Verify before building:' -ForegroundColor Cyan
Write-Host '  git diff -- switch/source/provider_registry.cpp switch/source/main.cpp'
