# Builds a redistributable package: the exe, the DLLs it actually imports, the
# runtime data it loads, and a zip of the whole thing.
#
# The DLL list is not guessed. It is what dumpbin /DEPENDENTS reports for
# GraphicEngine.exe, plus assimp's own five transitive dependencies. The debug
# variants sitting in build/ (assimp-vc143-mtd, zlibd1) are deliberately absent:
# they are never loaded by a Release exe and only cost the recipient a download.
#
# The VC++ runtime DLLs are copied in beside the exe rather than assuming the
# recipient has the redistributable installed -- app-local deployment is
# supported for exactly this case and removes the most common "it does not
# start on a clean machine" failure.
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$OutputDir = 'dist',
    # Content/Models is ~3.5 GB of source art and the engine loads from it at
    # runtime, so it ships by default. -SkipModels produces a much smaller
    # archive that will start but fail to find most meshes; it is for testing
    # the packaging itself, not for redistribution.
    [switch]$SkipModels,
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$stage = Join-Path $repo "$OutputDir\SmallestGraphicsEngine"
$zipPath = Join-Path $repo "$OutputDir\SmallestGraphicsEngine.zip"

Write-Host "Staging to $stage" -ForegroundColor Cyan

# A previously packaged build still running holds its assets open, and every
# file it has mapped refuses to delete. This has to happen before the wipe
# below rather than later on: the wipe is the first thing to touch the old
# staging tree, so it is the first thing a stale process breaks.
$running = Get-Process GraphicEngine -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($stage, 'OrdinalIgnoreCase') }
if ($running) {
    Write-Warning "Stopping $($running.Count) GraphicEngine process(es) running from the staging directory."
    $running | Stop-Process -Force
    $running | ForEach-Object { $_.WaitForExit(10000) | Out-Null }
}

# Even after the process exits, Windows can hold its mapped files briefly.
# Retry rather than failing the run on a lock that clears itself in a second.
if (Test-Path $stage) {
    for ($attempt = 1; $attempt -le 5; ++$attempt) {
        try {
            Remove-Item $stage -Recurse -Force -ErrorAction Stop
            break
        } catch {
            if ($attempt -eq 5) {
                throw "Could not clear $stage after 5 attempts. Close anything using it and re-run. Last error: $($_.Exception.Message)"
            }
            Write-Warning "Staging directory locked (attempt $attempt/5); retrying in 3s..."
            Start-Sleep -Seconds 3
        }
    }
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# -- Executable ---------------------------------------------------------------
$exe = Join-Path $repo 'build\GraphicEngine.exe'
if (-not (Test-Path $exe)) {
    throw "GraphicEngine.exe not found at $exe. Run ./build.ps1 -Configuration $Configuration -NoRun first."
}
Copy-Item $exe $stage

# -- Runtime DLLs -------------------------------------------------------------
# Direct imports of the exe, then the libraries assimp itself pulls in.
$dlls = @(
    'meshoptimizer.dll',
    'assimp-vc143-mt.dll',
    'miniz.dll',
    # assimp's transitive set
    'poly2tri.dll',
    'minizip.dll',
    'zlib1.dll',
    'kubazip.dll',
    'pugixml.dll'
)
foreach ($dll in $dlls) {
    $src = Join-Path $repo "build\$dll"
    if (Test-Path $src) { Copy-Item $src $stage }
    else { Write-Warning "Missing runtime DLL: $dll" }
}

# dxcompiler/dxil are the DirectX shader compiler. Not a link-time import --
# the engine compiles HLSL at runtime -- so dumpbin does not list them, but
# shader compilation fails without them on machines lacking a DXC install.
foreach ($dll in @('dxcompiler.dll', 'dxil.dll')) {
    $src = Join-Path $repo "build\$dll"
    if (Test-Path $src) { Copy-Item $src $stage }
}

# -- Visual C++ runtime -------------------------------------------------------
$redist = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' `
    -Directory -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if ($redist) {
    foreach ($n in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
                     'msvcp140_atomic_wait.dll', 'vcruntime140.dll',
                     'vcruntime140_1.dll')) {
        $src = Join-Path $redist.FullName $n
        if (Test-Path $src) { Copy-Item $src $stage }
    }
    Write-Host "  VC++ runtime from $($redist.FullName)" -ForegroundColor DarkGray
} else {
    Write-Warning "VC++ redist not found. Recipients will need the Visual C++ 2015-2022 x64 redistributable."
}

# -- Runtime data -------------------------------------------------------------
# Paths the engine opens are relative to the working directory, so the layout
# under the exe has to mirror the repo layout exactly.
$dataDirs = @('shaders', 'prefabs', 'assetcache', 'levels')
foreach ($d in $dataDirs) {
    $src = Join-Path $repo "build\$d"
    if (-not (Test-Path $src)) { $src = Join-Path $repo $d }
    if (Test-Path $src) {
        Write-Host "  $d" -ForegroundColor DarkGray
        Copy-Item $src (Join-Path $stage $d) -Recurse -Force
    }
}

# Content is the bulk of the package. Cooked and Models are both loaded at
# runtime -- Cooked by the prefab/barrel loader, Models by a long list of
# hardcoded paths in main.cpp -- so neither can be dropped without breaking
# the build in ways that only appear once a level loads.
$contentSrc = Join-Path $repo 'build\Content'
if (-not (Test-Path $contentSrc)) { $contentSrc = Join-Path $repo 'Content' }
$contentDst = Join-Path $stage 'Content'
New-Item -ItemType Directory -Path $contentDst -Force | Out-Null
foreach ($sub in (Get-ChildItem $contentSrc -Directory)) {
    if ($SkipModels -and $sub.Name -eq 'Models') {
        Write-Warning "Skipping Content/Models (-SkipModels): the package will not be playable."
        continue
    }
    Write-Host "  Content/$($sub.Name)" -ForegroundColor DarkGray
    Copy-Item $sub.FullName (Join-Path $contentDst $sub.Name) -Recurse -Force
}

# -- Prune unreferenced source art from the staged copy ------------------------
# Content/Models accumulated directories that nothing loads: earlier versions of
# meshes that were replaced, and imports that were tried and abandoned. They are
# kept in the repo -- deleting source art is the author's call, not the
# packager's -- but there is no reason to ship them.
#
# The keep list is every Models/ directory referenced from the engine or from
# level/prefab data. Two forms of reference have to be searched, and missing
# the second is what broke terrain the first time this ran:
#
#   1. Full paths, "Content/Models/<dir>/...", which most call sites use.
#   2. Bare relative folders that are joined to kModelRoot at the call site.
#      TerrainRendererDX12 names its splat layers as "terrain/dark_rock" and
#      "Grass3/Grass004_2K-JPG" with no Content/Models prefix, so a search for
#      the full path finds nothing and both directories look unused. Pruning
#      them left the terrain untextured.
#
# Before adding to the pruned set, grep for the bare directory name as well as
# the full path. The remainder here was checked both ways against src, shaders,
# Content/Levels, levels, prefabs and Content/Prefabs and appears in none.
#
# assetcache/registry.json still carries entries for some pruned directories.
# That is harmless: registry lookups resolve a GUID to a path inside catch(...)
# and return empty on a miss, so a stale entry is never loaded.
#
# Anything not on this list is deleted from the PACKAGE ONLY. If a mesh turns
# out to be needed, add its directory here -- do not assume the list is complete
# for a level set this script has not seen.
$keepModelDirs = @(
    'Barrel Explosive', 'BlackHawk', 'CommunicationTower',
    'Corrugated metal pack', 'HarpoonGun', 'HarpoonSpear', 'house_pbr',
    'Humvee', 'Imported', 'MainPlayer', 'MarineAlly', 'MetalRoof',
    'MilitaryMercenaryBandit', 'MiltaryBoat', 'OH-1_fbx', 'palmtree',
    'polyhaven', 'Rock1', 'RPG7', 'shotgun_fbx', 'Skyboxes', 'SVD_v1.3',
    'ak47', 'fbx_Dandelion', 'grass', 'textures',
    # Terrain splat layers, referenced bare rather than by full path -- see the
    # note above. Grass3 is the terrain's grass layer and is distinct from the
    # 'grass' directory used by the grass field.
    'terrain', 'Grass3'
)
$stagedModels = Join-Path $contentDst 'Models'
if (Test-Path $stagedModels) {
    $freed = 0
    foreach ($dir in (Get-ChildItem $stagedModels -Directory)) {
        if ($keepModelDirs -contains $dir.Name) { continue }
        $bytes = (Get-ChildItem $dir.FullName -Recurse -File -ErrorAction SilentlyContinue |
                  Measure-Object Length -Sum).Sum
        # Count the bytes only once the delete succeeds, and keep going if one
        # directory is locked -- a single stuck file should not cost the whole
        # package. The warning names it so the shipped size can be explained.
        try {
            Remove-Item $dir.FullName -Recurse -Force -ErrorAction Stop
            $freed += $bytes
            Write-Host ("  pruned Models/{0} ({1:N0} MB)" -f $dir.Name, ($bytes / 1MB)) -ForegroundColor DarkYellow
        } catch {
            Write-Warning "Could not prune Models/$($dir.Name): $($_.Exception.Message)"
        }
    }
    # Loose files sitting directly under Models/ get the same treatment. Only
    # h2.glb, gun.glb and gun.obj are referenced from main.cpp; the rest
    # (crate, h1, level, ship, test and a stray displacement map) are scratch
    # imports totalling ~260 MB that nothing opens.
    $keepModelFiles = @('h2.glb', 'gun.glb', 'gun.obj')
    foreach ($file in (Get-ChildItem $stagedModels -File)) {
        if ($keepModelFiles -contains $file.Name) { continue }
        try {
            Remove-Item $file.FullName -Force -ErrorAction Stop
            $freed += $file.Length
            Write-Host ("  pruned Models/{0} ({1:N0} MB)" -f $file.Name, ($file.Length / 1MB)) -ForegroundColor DarkYellow
        } catch {
            Write-Warning "Could not prune Models/$($file.Name): $($_.Exception.Message)"
        }
    }
    Write-Host ("  pruning freed {0:N2} GB" -f ($freed / 1GB)) -ForegroundColor Green
}

# -- Launcher and readme ------------------------------------------------------
# A .bat rather than asking the user to run the exe directly: it pins the
# working directory to the package root, which is what every relative asset
# path in the engine resolves against. Double-clicking the exe from a shortcut
# elsewhere would otherwise find no Content at all.
@'
@echo off
cd /d "%~dp0"
start "" "GraphicEngine.exe"
'@ | Set-Content (Join-Path $stage 'Play.bat') -Encoding ascii

@'
Smallest Graphics Engine
========================

Run Play.bat to start.

Play.bat sets the working directory to this folder before launching. The engine
resolves every asset path relative to that directory, so launching
GraphicEngine.exe from a shortcut somewhere else will start with no content.

Requirements
------------
  Windows 10/11 x64
  A GPU with DirectX 12 support

The Visual C++ runtime is included in this folder, so no separate
redistributable install is needed.

Controls
--------
  WASD          move
  Mouse         look
  Left click    fire
  Right click   aim (scope on the SVD, iron sights otherwise)
  R             reload
  G             grenade
  F             pick up / throw
  J             raise or lower night vision goggles (if NVG is in the gear slot)
  V             toggle first/third person
  Tab           show or hide the UI
  Esc           menu
'@ | Set-Content (Join-Path $stage 'README.txt') -Encoding ascii

# -- Report and zip -----------------------------------------------------------
$size = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Staged {0:N2} GB to {1}" -f ($size / 1GB), $stage) -ForegroundColor Green

if (-not $NoZip) {
    Write-Host "Compressing (this takes a while at this size)..." -ForegroundColor Cyan
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stage, $zipPath,
        [System.IO.Compression.CompressionLevel]::Optimal, $true)
    $zipSize = (Get-Item $zipPath).Length
    Write-Host ("Wrote {0} ({1:N2} GB)" -f $zipPath, ($zipSize / 1GB)) -ForegroundColor Green
}
