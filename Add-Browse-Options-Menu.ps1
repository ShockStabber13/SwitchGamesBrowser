$ErrorActionPreference = 'Stop'

$Main = '.\switch\source\main.cpp'
if (-not (Test-Path -LiteralPath $Main)) {
    throw "Run this from the SwitchGamesBrowser project root. Missing: $Main"
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$text = [System.IO.File]::ReadAllText((Resolve-Path $Main))
$backup = "$Main.before-options-menu.bak"
Copy-Item -LiteralPath $Main -Destination $backup -Force

function Replace-OnceRegex {
    param(
        [string]$InputText,
        [string]$Pattern,
        [string]$Replacement,
        [string]$Description
    )

    $rx = [regex]::new(
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline -bor
        [System.Text.RegularExpressions.RegexOptions]::Singleline
    )

    if (-not $rx.IsMatch($InputText)) {
        throw "Could not find $Description in main.cpp. No changes were saved."
    }

    return $rx.Replace($InputText, $Replacement, 1)
}

# -----------------------------------------------------------------
# 1. Add Page::Options without disturbing any newer pages such as
#    Providers/SearchProgress that may already exist locally.
# -----------------------------------------------------------------
if ($text -notmatch 'enum class Page\s*\{[^}]*\bOptions\b') {
    $pageRx = [regex]::new('enum class Page\s*\{([^}]*)\};')
    $match = $pageRx.Match($text)
    if (-not $match.Success) {
        throw 'Could not find enum class Page in main.cpp. No changes were saved.'
    }

    $items = $match.Groups[1].Value.Trim()
    $replacement = "enum class Page { $items, Options };"
    $text = $pageRx.Replace($text, $replacement, 1)
}

# -----------------------------------------------------------------
# 2. Add an independent cursor for the Options menu.
# -----------------------------------------------------------------
if ($text -notmatch '\boptionsCursor\b') {
    $cursorRx = [regex]::new('(?m)^(\s*size_t settingsCursor\s*=\s*[^;]+;)')
    $match = $cursorRx.Match($text)
    if (-not $match.Success) {
        throw 'Could not find settingsCursor in main.cpp. No changes were saved.'
    }

    $indent = ([regex]::Match($match.Value, '^\s*')).Value
    $replacement = $match.Value + "`r`n" + $indent + 'size_t optionsCursor = 0;'
    $text = $cursorRx.Replace($text, $replacement, 1)
}

# -----------------------------------------------------------------
# 3. Browse controls:
#    Remove the old one-button-per-filter shortcuts and make MINUS
#    open the Options page. Y still opens Settings.
# -----------------------------------------------------------------
if ($text -notmatch 'page\s*=\s*Page::Options;') {
    $browsePattern = '(?m)^\s{12}if \(keys & HidNpadButton_X\).*?^\s{12}if \(keys & HidNpadButton_Y\) \{'
    $browseReplacement = @'
            if (keys & HidNpadButton_Minus) {
                optionsCursor = 0;
                page = Page::Options;
            }
            if (keys & HidNpadButton_Y) {
'@

    $text = Replace-OnceRegex -InputText $text -Pattern $browsePattern -Replacement $browseReplacement -Description 'the old Browse filter shortcut block'
}

# -----------------------------------------------------------------
# 4. Options input handling.
# -----------------------------------------------------------------
if ($text -notmatch 'else if \(page == Page::Options\)') {
    $optionsInput = @'
        } else if (page == Page::Options) {
            if ((keys & HidNpadButton_Up) && optionsCursor > 0)
                --optionsCursor;

            if ((keys & HidNpadButton_Down) && optionsCursor < 7)
                ++optionsCursor;

            if (keys & HidNpadButton_B) {
                page = Page::Browse;
            }

            if (keys & HidNpadButton_A) {
                if (optionsCursor == 0) {
                    filter.search = keyboard(
                        "Search Switch games",
                        filter.search
                    );
                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 1) {
                    filter.sort = static_cast<sgb::Sort>(
                        (static_cast<int>(filter.sort) + 1) % 3
                    );
                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 2) {
                    std::set<std::string> genres;
                    for (const auto& game : games) {
                        for (const auto& genre : game.genres)
                            genres.insert(genre);
                    }

                    if (filter.genre.empty()) {
                        if (!genres.empty())
                            filter.genre = *genres.begin();
                    }
                    else {
                        auto next = genres.upper_bound(filter.genre);
                        filter.genre =
                            next == genres.end()
                                ? ""
                                : *next;
                    }

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 3) {
                    filter.minRating =
                        filter.minRating == 0 ? 70 :
                        filter.minRating == 70 ? 80 :
                        filter.minRating == 80 ? 90 : 0;

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 4) {
                    filter.minReviews =
                        filter.minReviews == 0 ? 10 :
                        filter.minReviews == 10 ? 50 :
                        filter.minReviews == 50 ? 100 : 0;

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 5) {
                    filter.favouritesOnly =
                        !filter.favouritesOnly;

                    cursor = 0;
                    rebuild();
                }
                else if (optionsCursor == 6) {
                    filter = {};
                    cursor = 0;
                    rebuild();
                    status = "Filters reset";
                }
                else {
                    page = Page::Browse;
                }
            }
'@

    $settingsInputMarker = '        } else if (page == Page::Settings) {'
    $idx = $text.IndexOf($settingsInputMarker)
    if ($idx -lt 0) {
        throw 'Could not locate Settings input block in main.cpp. No changes were saved.'
    }

    $text = $text.Substring(0, $idx) +
        $optionsInput +
        $settingsInputMarker +
        $text.Substring($idx + $settingsInputMarker.Length)
}

# -----------------------------------------------------------------
# 5. Options screen UI. Insert it before whichever top-level render
#    page currently comes first (SearchProgress, Providers, Settings).
# -----------------------------------------------------------------
if ($text -notmatch '"FILTER / SORT OPTIONS"') {
    $optionsUi = @'
        if (page == Page::Options) {
            static const char* optionSortLabels[] = {
                "Title A-Z",
                "Highest rated",
                "Newest"
            };

            label(
                renderer,big,
                "FILTER / SORT OPTIONS",
                32,70,1200,green
            );

            label(
                renderer,small,
                "A Change / Select  |  B Back",
                32,112,1200,muted
            );

            std::vector<std::string> optionRows{
                "Search: " +
                    (filter.search.empty() ? std::string("Any") : filter.search),

                "Sort: " +
                    std::string(optionSortLabels[static_cast<int>(filter.sort)]),

                "Genre: " +
                    (filter.genre.empty() ? std::string("All genres") : filter.genre),

                "Minimum rating: " +
                    (filter.minRating == 0
                        ? std::string("Any")
                        : std::to_string(static_cast<int>(filter.minRating)) + "+"),

                "Minimum votes: " +
                    (filter.minReviews == 0
                        ? std::string("Any")
                        : std::to_string(filter.minReviews) + "+"),

                "Favourites only: " +
                    std::string(filter.favouritesOnly ? "On" : "Off"),

                "Reset filters",
                "Back"
            };

            for (size_t i = 0; i < optionRows.size(); ++i) {
                int y = 150 + static_cast<int>(i) * 61;
                SDL_Rect box{32,y,1216,53};

                rect(
                    renderer,
                    box,
                    SDL_Color{18,18,18,255}
                );

                if (i == optionsCursor) {
                    rect(renderer,box,green,true);
                    rect(
                        renderer,
                        {33,y+1,1214,51},
                        green,
                        true
                    );
                }

                label(
                    renderer,small,
                    optionRows[i],
                    52,y+14,1160,
                    i == optionsCursor ? green : white
                );
            }

'@

    $renderMarkers = @(
        '        if (page == Page::SearchProgress) {',
        '        if (page == Page::Providers) {',
        '        if (page == Page::Settings) {'
    )

    $foundRenderMarker = $null
    foreach ($marker in $renderMarkers) {
        # The render copy occurs after SDL_RenderClear; prefer that one.
        $searchFrom = $text.IndexOf('SDL_RenderClear(renderer)')
        if ($searchFrom -lt 0) { $searchFrom = 0 }

        $pos = $text.IndexOf($marker, $searchFrom)
        if ($pos -ge 0) {
            $foundRenderMarker = $marker
            $renderPos = $pos
            break
        }
    }

    if (-not $foundRenderMarker) {
        throw 'Could not locate the top-level page render block in main.cpp. No changes were saved.'
    }

    $nextPage = $foundRenderMarker.Replace('        if ', '        } else if ')
    $text = $text.Substring(0, $renderPos) +
        $optionsUi +
        $nextPage +
        $text.Substring($renderPos + $foundRenderMarker.Length)
}

# Keep the startup hint aligned with the new Browse controls.
$text = $text.Replace(
    'std::string url, status = "Press Y for Settings";',
    'std::string url, status = "Press - for Options";'
)

# -----------------------------------------------------------------
# 6. Replace the two overlapping Browse help lines with one compact
#    footer below the game cards.
# -----------------------------------------------------------------
$footer1 = [regex]::new(
    '(?m)^\s*label\(renderer,small,"A Details \| X Search \| Y Settings \| ZL Sort \| ZR Favourites \| - Rating",32,580,1216\);\s*$'
)
$footer2 = [regex]::new(
    '(?m)^\s*label\(renderer,small,"L/R Page \| Left stick Genre \| Right stick Votes \| B Reset \| \+ Exit",32,614,1216,muted\);\s*$'
)

if ($footer1.IsMatch($text)) {
    $text = $footer1.Replace(
        $text,
        '            label(renderer,small,"- Options  |  A Details  |  Y Settings  |  + Exit",32,648,1216,muted);',
        1
    )
}
elseif ($text -notmatch '"- Options  \|  A Details') {
    throw 'Could not find the first old Browse footer line. No changes were saved.'
}

if ($footer2.IsMatch($text)) {
    $text = $footer2.Replace($text, '', 1)
}

# A final sanity check before writing.
$required = @(
    'Page::Options',
    'optionsCursor',
    'FILTER / SORT OPTIONS',
    '- Options  |  A Details'
)

foreach ($needle in $required) {
    if (-not $text.Contains($needle)) {
        throw "Patch sanity check failed: missing '$needle'. No changes were saved."
    }
}

[System.IO.File]::WriteAllText(
    (Resolve-Path $Main),
    $text,
    $Utf8NoBom
)

Write-Host ''
Write-Host 'Browse Options UI installed.' -ForegroundColor Green
Write-Host "Backup: $backup" -ForegroundColor Yellow
Write-Host ''
Write-Host 'Browse controls:' -ForegroundColor Cyan
Write-Host '  -      Options'
Write-Host '  A      Details'
Write-Host '  Y      Settings'
Write-Host '  +      Exit'
Write-Host ''
Write-Host 'Options menu:' -ForegroundColor Cyan
Write-Host '  Search'
Write-Host '  Sort'
Write-Host '  Genre'
Write-Host '  Minimum rating'
Write-Host '  Minimum votes'
Write-Host '  Favourites only'
Write-Host '  Reset filters'
Write-Host '  Back'
Write-Host ''
Write-Host 'Then rebuild in WSL:' -ForegroundColor Cyan
Write-Host '  cd ~/SwitchGamesBrowserBuild/switch'
Write-Host '  make clean'
Write-Host '  make -j$(nproc)'
