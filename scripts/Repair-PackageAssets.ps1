param(
    [Parameter(Mandatory = $true)]
    [string]$Package,
    [string]$Repo = (Split-Path -Parent $PSScriptRoot),
    [string[]]$RelativeDirs = @(
        'Content\Prefabs',
        'Content\Models\RPG7',
        'Content\Models\NewBlackHawk',
        'Content\Models\MainPlayer',
        'Content\Models\MilitaryMercenaryBandit',
        'Content\Models\OH-1_fbx',
        'Content\Models\fps_arms',
        'Content\Models\MarineAlly',
        'Content\Models\terrain',
        'Content\Models\Turret',
        'Content\Models\MetalRoof',
        'Content\Models\palmtree',
        'Content\Models\MilitaryBoatNew'
    )
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Package -PathType Container)) {
    throw "Package directory does not exist: $Package"
}

# Use the build's runtime content first, with authoring content as fallback.
# Never overwrite a staged file:
# this script is intended to repair a live restage safely.
$sourceRoots = @(
    (Join-Path $Repo 'build\Content'),
    (Join-Path $Repo 'Content')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Container }
$copied = 0
$bytes = [long]0

foreach ($relativeDir in $RelativeDirs) {
    $relative = $relativeDir.Replace('/', '\')
    $destinationRoots = @(Join-Path $Package $relative)
    # Runtime lookup historically used the short root-level directory while
    # authoring and validation use Content/Prefabs. Keep both layouts in sync.
    if ($relative -eq 'Content\Prefabs') {
        $destinationRoots += Join-Path $Package 'prefabs'
    }
    foreach ($sourceRoot in $sourceRoots) {
        $sourceDir = Join-Path $sourceRoot ($relative -replace '^Content\\', '')
        if (-not (Test-Path -LiteralPath $sourceDir -PathType Container)) { continue }
        foreach ($file in (Get-ChildItem -LiteralPath $sourceDir -Recurse -File)) {
            if ($file.Name -match '(?i)(\.tmp|\.bak|\.orig(?:\..*)?)$') { continue }
            $tail = $file.FullName.Substring($sourceDir.Length + 1)
            foreach ($destinationRoot in $destinationRoots) {
                $destination = Join-Path $destinationRoot $tail
                if (Test-Path -LiteralPath $destination) { continue }
                New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
                Copy-Item -LiteralPath $file.FullName -Destination $destination
                $copied++
                $bytes += $file.Length
                Write-Host ("  copied {0} ({1:N0} bytes)" -f ($destination.Substring($Package.Length + 1)), $file.Length)
            }
        }
    }
}

Write-Host ("Repaired {0} files ({1:N2} MB)" -f $copied, ($bytes / 1MB))
