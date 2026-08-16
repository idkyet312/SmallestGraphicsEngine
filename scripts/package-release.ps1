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
$dataDirs = @('shaders', 'prefabs', 'assetcache')
foreach ($d in $dataDirs) {
    $src = Join-Path $repo "build\$d"
    if (-not (Test-Path $src)) { $src = Join-Path $repo $d }
    if (Test-Path $src) {
        Write-Host "  $d" -ForegroundColor DarkGray
        Copy-Item $src (Join-Path $stage $d) -Recurse -Force
    }
}

# Content/Levels is the canonical authoring tree. build/levels is a legacy
# partial copy and can survive for months without receiving newer maps; using
# the generic build-first rule above made a fresh package drop Islandv10 even
# though the level was present in the repository. Keep the simple levels/
# package layout, but always populate it from the canonical tree.
$levelSrc = Join-Path $repo 'Content\Levels'
if (-not (Test-Path $levelSrc)) {
    $levelSrc = Join-Path $repo 'build\Content\Levels'
}
if (-not (Test-Path $levelSrc)) {
    throw "No level directory found. Expected Content/Levels or build/Content/Levels."
}
Write-Host "  levels (Content/Levels)" -ForegroundColor DarkGray
Copy-Item $levelSrc (Join-Path $stage 'levels') -Recurse -Force

# Content is the bulk of the package. Cooked and Models are both loaded at
# runtime -- Cooked by the prefab/barrel loader, Models by a long list of
# hardcoded paths in main.cpp -- so neither can be dropped without breaking
# the build in ways that only appear once a level loads.
#
# Each subdirectory is taken from build/Content or from Content, whichever
# exists -- chosen PER SUBDIRECTORY rather than once for the whole tree.
# build/Content is a partial mirror maintained by the shader/content copy step,
# so a blanket "prefer build/" silently shipped its stale Cooked/ and dropped
# the newly cooked Tower and Turret, which the cooker had written to the repo
# copy. Where both exist, the newer one wins.
$contentRepo = Join-Path $repo 'Content'
$contentBuild = Join-Path $repo 'build\Content'
$contentDst = Join-Path $stage 'Content'
New-Item -ItemType Directory -Path $contentDst -Force | Out-Null

$subNames = @{}
foreach ($root in @($contentRepo, $contentBuild)) {
    if (-not (Test-Path $root)) { continue }
    foreach ($sub in (Get-ChildItem $root -Directory)) { $subNames[$sub.Name] = $true }
}

foreach ($name in ($subNames.Keys | Sort-Object)) {
    if ($SkipModels -and $name -eq 'Models') {
        Write-Warning "Skipping Content/Models (-SkipModels): the package will not be playable."
        continue
    }
    $fromRepo = Join-Path $contentRepo $name
    $fromBuild = Join-Path $contentBuild $name
    $src = $null
    if ((Test-Path $fromRepo) -and (Test-Path $fromBuild)) {
        # Newest file anywhere under each candidate. Comparing the directory's
        # own timestamp is not enough: copying into a subfolder does not touch
        # the parent, so a stale tree can look current.
        $repoTime = (Get-ChildItem $fromRepo -Recurse -File -ErrorAction SilentlyContinue |
                     Measure-Object LastWriteTimeUtc -Maximum).Maximum
        $buildTime = (Get-ChildItem $fromBuild -Recurse -File -ErrorAction SilentlyContinue |
                      Measure-Object LastWriteTimeUtc -Maximum).Maximum
        $src = if ($null -ne $repoTime -and ($null -eq $buildTime -or $repoTime -ge $buildTime)) {
            $fromRepo
        } else { $fromBuild }
    } elseif (Test-Path $fromRepo) { $src = $fromRepo }
    else { $src = $fromBuild }

    $origin = if ($src -eq $fromRepo) { 'repo' } else { 'build' }
    Write-Host "  Content/$name ($origin)" -ForegroundColor DarkGray
    Copy-Item $src (Join-Path $contentDst $name) -Recurse -Force
}

# -- Prune unreferenced source art from the staged copy ------------------------
# Content/Models accumulated directories that nothing loads: earlier versions of
# meshes that were replaced, and imports that were tried and abandoned. They are
# kept in the repo -- deleting source art is the author's call, not the
# packager's -- but there is no reason to ship them.
#
# The keep set is DISCOVERED, not hand-written. Find-AssetReferences scans the
# engine source, shaders, prefabs and level data for the three ways a model
# directory gets referenced, and keeps every directory it finds. See that script
# for what those three forms are and why the third one needs a filesystem check.
#
# This used to be a literal array maintained by hand, and it went stale exactly
# the way such lists do: the tower and turret were added to prefabs, nobody
# updated the array, and both were silently pruned from the package while
# working perfectly in the repo. The array had also drifted the other way,
# keeping Skyboxes (the live sky EXRs load from Content/Textures/Sky instead)
# and naming gun.glb/gun.obj, which no longer exist.
#
# assetcache/registry.json still carries entries for some pruned directories.
# That is harmless: registry lookups resolve a GUID to a path inside catch(...)
# and return empty on a miss, so a stale entry is never loaded.
#
# Pruning affects the PACKAGE ONLY -- the repo copy is never touched.
. (Join-Path $PSScriptRoot 'Find-AssetReferences.ps1')

# Measure-Object returns an object with no Sum property when the pipeline is
# empty, which throws under StrictMode rather than yielding 0. An empty
# directory in the staging tree is ordinary, so handle it here instead of at
# each call site.
function Get-DirectorySize {
    param([string]$Path)
    $measured = Get-ChildItem $Path -Recurse -File -ErrorAction SilentlyContinue |
                Measure-Object Length -Sum
    if ($measured -and $null -ne $measured.Sum) { return [long]$measured.Sum }
    return [long]0
}

Write-Host "Scanning for asset references..." -ForegroundColor Cyan
$referenced = Get-ReferencedModelDirs -Repo $repo
$keepModelDirs = @($referenced.Keys)
Write-Host ("  {0} referenced Models directories" -f $keepModelDirs.Count) -ForegroundColor DarkGray

# A scan that finds almost nothing means the heuristics broke (a moved source
# tree, a renamed Content root), not that the game stopped using art. Shipping
# that result would produce a package with no meshes, so fail instead -- a
# packaging error is recoverable, a silently empty release is not.
if ($keepModelDirs.Count -lt 10) {
    throw "Asset reference scan found only $($keepModelDirs.Count) referenced directories, which indicates the scan failed rather than a genuinely small asset set. Refusing to prune. Run ./scripts/Find-AssetReferences.ps1 to inspect."
}
# Loose files sitting directly under Models/, discovered the same way: a
# filename is kept only if it appears inside a quoted string somewhere that
# loads it. Hoisted out of the block below because the Cooked/ prune needs it
# too. The rest (crate, h1, level, ship, test and a stray displacement map) are
# scratch imports that nothing opens.
$keepModelFiles = @(Get-ReferencedModelFiles -Repo $repo)

$stagedModels = Join-Path $contentDst 'Models'
if (Test-Path $stagedModels) {
    $freed = 0
    foreach ($dir in (Get-ChildItem $stagedModels -Directory)) {
        if ($keepModelDirs -contains $dir.Name) { continue }
        $bytes = Get-DirectorySize $dir.FullName
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
    # Loose files under Models/ get the same treatment, against the set
    # discovered above.
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

# Cooked/ mirrors Models/, so the same keep set applies. A cook run from before
# a mesh was dropped leaves its .sgeasset behind -- the cooker writes new blobs
# but never deletes stale ones -- and those would otherwise ship. ship.sgeasset
# alone is 270 MB of an asset nothing loads.
$stagedCooked = Join-Path $contentDst 'Cooked\Models'
if (Test-Path $stagedCooked) {
    $cookedFreed = 0
    foreach ($dir in (Get-ChildItem $stagedCooked -Directory)) {
        if ($keepModelDirs -contains $dir.Name) { continue }
        $bytes = Get-DirectorySize $dir.FullName
        try {
            Remove-Item $dir.FullName -Recurse -Force -ErrorAction Stop
            $cookedFreed += $bytes
            Write-Host ("  pruned Cooked/Models/{0} ({1:N0} MB)" -f $dir.Name, ($bytes / 1MB)) -ForegroundColor DarkYellow
        } catch {
            Write-Warning "Could not prune Cooked/Models/$($dir.Name): $($_.Exception.Message)"
        }
    }
    # Loose .sgeasset blobs cooked from loose sources (ship.glb -> ship.sgeasset).
    # Matched on the source stem, since the extension always differs.
    $keepStems = @($keepModelFiles | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
    foreach ($file in (Get-ChildItem $stagedCooked -File)) {
        if ($keepStems -contains [IO.Path]::GetFileNameWithoutExtension($file.Name)) { continue }
        try {
            Remove-Item $file.FullName -Force -ErrorAction Stop
            $cookedFreed += $file.Length
            Write-Host ("  pruned Cooked/Models/{0} ({1:N0} MB)" -f $file.Name, ($file.Length / 1MB)) -ForegroundColor DarkYellow
        } catch {
            Write-Warning "Could not prune Cooked/Models/$($file.Name): $($_.Exception.Message)"
        }
    }
    if ($cookedFreed -gt 0) {
        Write-Host ("  cooked pruning freed {0:N2} GB" -f ($cookedFreed / 1GB)) -ForegroundColor Green
    }
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
@echo off
cd /d "%~dp0"
start "" "GraphicEngine.exe" --level "levels\Islandv10.json"
'@ | Set-Content (Join-Path $stage 'Play Islandv10.bat') -Encoding ascii

@'
Smallest Graphics Engine
========================

Run Play.bat to start at the main menu, or Play Islandv10.bat to load Islandv10
directly from the packaged levels folder.

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
