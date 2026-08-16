# Cooks source art into runtime .sgeasset blobs, restricted to what the engine
# and its data actually reference.
#
# The bare CMake target (`cmake --build build --target CookAssets`) still cooks
# everything under Content, which is fine for a one-off but spends minutes and
# gigabytes on art nothing loads -- a 270 MB ship.sgeasset, a 63 MB palm -- and
# leaves that output to be pruned again later. This wrapper discovers the
# referenced set first and passes it through AssetCooker's --only filter, so the
# cook and the package agree on what ships.
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    # Cook everything, ignoring reference discovery. For rebuilding art that is
    # referenced from somewhere the scanner cannot see (a level authored at
    # runtime, an experiment) without editing the scanner first.
    [switch]$All
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$cooker = Join-Path $repo "build\$Configuration\AssetCooker.exe"
if (-not (Test-Path $cooker)) {
    Write-Host "Building AssetCooker..." -ForegroundColor Cyan
    cmake --build build --target AssetCooker --config $Configuration | Out-Null
    if (-not (Test-Path $cooker)) { throw "AssetCooker.exe not found at $cooker" }
}

$content = Join-Path $repo 'Content'
$cooked = Join-Path $repo 'Content\Cooked'

# AssetCooker writes progress notes ("using legacy ufbx fallback") to stderr.
# Under $ErrorActionPreference='Stop', PowerShell 5.1 promotes any native stderr
# line to a terminating NativeCommandError, which failed the cook on a message
# that is not an error. Success is the exit code and nothing else.
$ErrorActionPreference = 'Continue'

if ($All) {
    Write-Host "Cooking all assets (-All)..." -ForegroundColor Cyan
    & $cooker --all $content --out $cooked
} else {
    . (Join-Path $PSScriptRoot 'Find-AssetReferences.ps1')
    $listPath = Join-Path $repo 'build\cook-only.txt'
    Write-Host "Discovering referenced assets..." -ForegroundColor Cyan
    Write-CookOnlyList -Repo $repo -Path $listPath | Out-Null
    $count = (Get-Content $listPath | Where-Object { $_ -and $_ -notmatch '^\s*#' }).Count
    Write-Host "  $count referenced paths -> $listPath" -ForegroundColor DarkGray
    & $cooker --all $content --out $cooked --only $listPath
}
if ($LASTEXITCODE -ne 0) { throw "AssetCooker failed with exit code $LASTEXITCODE" }
