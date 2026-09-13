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
    # Ships the cooked .sgeasset for a mesh and leaves the source FBX/GLB behind.
    # Unlike -SkipModels this produces a package that plays: the engine reads the
    # cooked asset and only falls back to the import when there is no cook. See
    # the selection block below for the handful of assets whose loaders bypass
    # the cooked cache and so keep their source.
    [switch]$CookedOnly,
    # Levels the package ships. Content/Levels holds two dozen maps, most of
    # them authoring history and test scenes; a build sent to someone else wants
    # the ones the game can actually reach from its own menus. These three are
    # what the engine names in code -- the hub, the island the travel board
    # flies to, and the training range. Level 1 and the test level need no entry
    # here: both are built into the exe rather than loaded from a file.
    [string[]]$Levels = @('Base.json', 'Islandv10.json', 'TrainingRange.json'),
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$outputRoot = if ([IO.Path]::IsPathRooted($OutputDir)) { [IO.Path]::GetFullPath($OutputDir) } else { [IO.Path]::GetFullPath((Join-Path $repo $OutputDir)) }
$finalStage = Join-Path $outputRoot "SmallestGraphicsEngine"
# Build in a uniquely named sibling. The previous package remains available if
# copying, pruning, or validation fails; it is swapped only after all checks pass.
$stage = Join-Path (Split-Path -Parent $finalStage) ("SmallestGraphicsEngine.restage-{0}-{1}" -f $PID, (Get-Date -Format 'yyyyMMddHHmmss'))
$zipPath = Join-Path $outputRoot "SmallestGraphicsEngine.zip"

if (-not $stage.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, 'OrdinalIgnoreCase') -or
    -not $finalStage.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, 'OrdinalIgnoreCase')) {
    throw "Refusing to stage outside the output directory: $stage"
}

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
# Walked, not listed. A hand-maintained list goes stale the moment a dependency
# gains one of its own, and the failure is invisible on the build machine: vcpkg
# puts its bin directory on PATH, so a DLL missing from the package still
# resolves locally and only fails on the recipient's machine, at startup, with a
# dialog naming a file they have never heard of. That is exactly how
# libcrypto-3-x64.dll -- OpenSSL, pulled in by GameNetworkingSockets for
# connection encryption -- shipped missing.
#
# The walk is transitive from the exe: every dependent that exists in build/ is
# ours and ships, everything else is a system DLL and does not.
$dumpbin = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' `
    -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if (-not $dumpbin) {
    throw "dumpbin.exe not found. It ships with Visual Studio's C++ tools and is how this script discovers which DLLs to package."
}

function Get-Dependents {
    param([string]$Binary)
    $output = & $dumpbin.FullName /DEPENDENTS $Binary 2>$null
    $names = @()
    foreach ($line in $output) {
        if ($line -match '^\s{4}([\w\.\-\+]+\.dll)\s*$') { $names += $Matches[1] }
    }
    return $names
}

$buildDir = Join-Path $repo 'build'
$pending = [System.Collections.Queue]::new()
$pending.Enqueue($exe)
$shipped = @{}
while ($pending.Count -gt 0) {
    foreach ($name in (Get-Dependents $pending.Dequeue())) {
        $key = $name.ToLower()
        if ($shipped.ContainsKey($key)) { continue }
        $src = Join-Path $buildDir $name
        # Not in build/ means the system provides it (d3d12, dxgi, XAudio2, the
        # CRT forwarders); those must not be packaged.
        if (-not (Test-Path $src)) { continue }
        Copy-Item $src $stage -Force
        $shipped[$key] = $true
        $pending.Enqueue($src)
    }
}
Write-Host ("  {0} runtime DLLs (walked from the exe)" -f $shipped.Count) -ForegroundColor DarkGray

# dxcompiler/dxil are the DirectX shader compiler. Not a link-time import --
# the engine compiles HLSL at runtime -- so dumpbin does not list them, but
# shader compilation fails without them on machines lacking a DXC install.
foreach ($dll in @('dxcompiler.dll', 'dxil.dll')) {
    $src = Join-Path $repo "build\$dll"
    if (Test-Path $src) { Copy-Item $src $stage }
}

# Steam, also not a link-time import: the engine loads it by name at startup and
# runs without it. Shipping it is what makes the game show up as played, and
# what the Steam-relayed multiplayer transport needs; a recipient with no Steam
# is unaffected either way.
$steamDll = Join-Path $repo 'build\steam_api64.dll'
if (Test-Path $steamDll) {
    Copy-Item $steamDll $stage
    # A build launched by hand rather than from a Steam library has no AppID,
    # and Steam refuses to talk to a process it cannot identify. 480 is Valve's
    # public test app, which is what an unreleased game can use: it makes the
    # session real, at the cost of Steam calling it Spacewar. Replace the
    # contents with the real AppID once there is one -- and delete the file
    # entirely for a build shipped through Steam, which supplies its own.
    $appId = if ($env:SGE_STEAM_APPID) { $env:SGE_STEAM_APPID } else { '480' }
    Set-Content (Join-Path $stage 'steam_appid.txt') $appId -Encoding ascii -NoNewline
    Write-Host "  steam_api64.dll (app $appId)" -ForegroundColor DarkGray
} else {
    Write-Warning "steam_api64.dll not in build/: the package will run without Steam."
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
#
# Only the levels named in -Levels ship, each with its sidecars: a map's splat
# texture is a separate file beside it, and a level without its splat loads with
# the wrong terrain materials. Both roots are searched per level because they
# have drifted apart -- Base.json lives in the repo and Base_splat.png only in
# build/Content/Levels -- and the newest copy wins where both have a file.
$levelRoots = @((Join-Path $repo 'Content\Levels'),
                (Join-Path $repo 'build\Content\Levels')) |
              Where-Object { Test-Path $_ }
if (-not $levelRoots) {
    throw "No level directory found. Expected Content/Levels or build/Content/Levels."
}
# Staged twice on purpose. The engine resolves a level through a candidate list
# that tries Content/Levels before levels/, and both layouts exist in the wild
# (repo run, packaged run); a handful of JSON costs nothing next to the art.
$levelTargets = @((Join-Path $stage 'levels'),
                  (Join-Path $stage 'Content\Levels'))
foreach ($target in $levelTargets) {
    New-Item -ItemType Directory -Path $target -Force | Out-Null
}
foreach ($name in $Levels) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($name)
    $found = $false
    # Newest wins per file name, not repo-first. The roots hold the same map at
    # different ages: the editor writes wherever the game was run from, so a hub
    # edited in a build/ run leaves the repo copy behind. Repo-first shipped that
    # stale copy -- a package whose Base.json was the old flat hub while its
    # Base_splat.png came from build/, i.e. a mismatched pair that loaded the
    # wrong map from the menu while the editor, listing Content/Levels, showed
    # the right one.
    $newest = @{}
    foreach ($root in $levelRoots) {
        foreach ($file in (Get-ChildItem $root -File -Filter "$stem*" -ErrorAction SilentlyContinue)) {
            $key = $file.Name.ToLower()
            if (-not $newest.ContainsKey($key) -or
                $file.LastWriteTimeUtc -gt $newest[$key].LastWriteTimeUtc) {
                $newest[$key] = $file
            }
            if ($file.Name -eq $name) { $found = $true }
        }
    }
    if (-not $found) {
        throw "Level '$name' was not found in Content/Levels or build/Content/Levels."
    }
    # Both staging layouts get the same bytes. They are two spellings of one
    # level, and the engine picks Content/Levels first; letting them differ is
    # how a package ends up playing a different map than it shows.
    foreach ($file in $newest.Values) {
        foreach ($target in $levelTargets) {
            Copy-Item $file.FullName (Join-Path $target $file.Name) -Force
        }
    }
    Write-Host "  levels/$name" -ForegroundColor DarkGray
}
# Baked DDGI probe data, named by a hash of the level rather than by its file
# name, so there is no way to ship a subset. It is a few KB per level.
$ddgiSrc = Join-Path $repo 'Content\Levels\.ddgi'
if (Test-Path $ddgiSrc) {
    foreach ($target in $levelTargets) {
        Copy-Item $ddgiSrc (Join-Path $target '.ddgi') -Recurse -Force
    }
}

# Select before copying so abandoned source art never consumes staging space.
# The canonical tree wins per file; build-only assets remain available.
. (Join-Path $PSScriptRoot 'Find-AssetReferences.ps1')
$referenced = Get-ReferencedModelDirs -Repo $repo
$keepLoose = @(Get-ReferencedModelFiles -Repo $repo)
$buildModels = Join-Path $repo 'build\Content\Models'
$sourceText = (Get-ChildItem (Join-Path $repo 'src') -Recurse -File -Include *.h,*.cpp |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
if (Test-Path $buildModels) {
    foreach ($dir in (Get-ChildItem -LiteralPath $buildModels -Directory)) {
        if ($sourceText -match ('(?i)Models[/\\]+' + [regex]::Escape($dir.Name) + '[/\\]')) {
            $referenced[$dir.Name] = [System.Collections.Generic.List[string]]::new()
        }
    }
}
if ($referenced.Count -lt 10) { throw 'Asset reference discovery failed.' }
$keepStems = @($keepLoose | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
$contentDst = Join-Path $stage 'Content'
$copyPlan = @{}
foreach ($root in @((Join-Path $repo 'Content'), (Join-Path $repo 'build\Content'))) {
    if (-not (Test-Path $root)) { continue }
    foreach ($file in (Get-ChildItem -LiteralPath $root -Recurse -File)) {
        $relative = $file.FullName.Substring($root.Length + 1)
        $parts = $relative.Split('\')
        if ($parts[0] -eq 'Levels') { continue }
        if ($file.Name -match '(?i)(\.tmp|\.bak|\.orig(?:\..*)?)$') { continue }
        $modelOffset = -1
        if ($parts[0] -eq 'Models') {
            if ($SkipModels) { continue }
            $modelOffset = 1
        } elseif ($parts.Count -gt 2 -and $parts[0] -eq 'Cooked' -and $parts[1] -eq 'Models') {
            $modelOffset = 2
        }
        if ($modelOffset -ge 0) {
            if ($parts.Count -gt ($modelOffset + 1)) {
                if (-not $referenced.ContainsKey($parts[$modelOffset])) { continue }
            } elseif ($modelOffset -eq 1) {
                if ($keepLoose -notcontains $file.Name) { continue }
            } elseif ($keepStems -notcontains $file.BaseName) { continue }
        }
        if (-not $copyPlan.ContainsKey($relative)) { $copyPlan[$relative] = $file }
    }
}
# -CookedOnly drops a source mesh whose cooked twin is already in the plan.
# CookedAssetLoader::LoadForSource skips its staleness check when the source is
# absent and serves the .sgeasset, and PrefabRegistry accepts a prefab whose
# model resolves to a cooked asset, so the import is only ever reached for a
# mesh with no cook. Measured at 1.1 GB off a 7.1 GB package.
#
# The exceptions are the call sites that deliberately bypass the cooked cache
# and therefore need their source: SkinnedFBXImporter parses the mesh out of the
# FBX (the bandit, the marine, the player arms), GLBImporter skips the cache
# whenever a skeleton is requested (both Black Hawk airframes), and FBXImporter
# skips it when the caller passes loadMaterials=false or diffuseAndNormalOnly
# (the AK/RPG/palm/dandelion viewmodels and the Humvee). Textures are never
# dropped here -- several are loaded by path through ResolveTexturePath.
if ($CookedOnly) {
    $sourceRequired = @(
        'MainPlayer', 'MilitaryMercenaryBandit', 'MarineAlly', 'Arms',
        'BlackHawk', 'NewBlackHawk', 'Humvee', 'palmtree', 'fbx_Dandelion',
        'RPG7', 'ak47'
    )
    $meshPattern = '(?i)\.(glb|gltf|fbx|obj)$'
    $dropped = 0
    $droppedBytes = [long]0
    foreach ($relative in @($copyPlan.Keys)) {
        $parts = $relative.Split('\')
        if ($parts[0] -ne 'Models' -or $parts.Count -lt 2) { continue }
        if ($relative -notmatch $meshPattern) { continue }
        if ($sourceRequired -contains $parts[1]) { continue }
        $cooked = 'Cooked\' + [IO.Path]::ChangeExtension($relative, '.sgeasset')
        if (-not $copyPlan.ContainsKey($cooked)) { continue }
        $droppedBytes += $copyPlan[$relative].Length
        $copyPlan.Remove($relative)
        ++$dropped
    }
    Write-Host ("  cooked-only: dropped {0} source meshes ({1:N2} GB)" -f `
        $dropped, ($droppedBytes / 1GB)) -ForegroundColor DarkGray
}
$requiredBytes = [long](($copyPlan.Values | Measure-Object Length -Sum).Sum)
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($stage))
if ($drive.AvailableFreeSpace -lt ($requiredBytes + 512MB)) {
    throw "Insufficient staging space: selected content requires $requiredBytes bytes plus 512 MB reserve."
}
Write-Host ("  selected {0} content files ({1:N2} GB)" -f $copyPlan.Count, ($requiredBytes / 1GB))
foreach ($relative in ($copyPlan.Keys | Sort-Object)) {
    $destination = Join-Path $contentDst $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $copyPlan[$relative].FullName -Destination $destination
}
# Retain source meshes and their textures: importer and animation paths may
# bypass cooked loading, and texture names can be assembled at runtime.

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

# Hosting from a .bat rather than the menu because a tester joining a session
# usually wants to be in a level already: the client follows the host onto
# whatever map it is on, and a host still sitting in the menu has none to give.
@'
@echo off
cd /d "%~dp0"
start "" "GraphicEngine.exe" --level "levels\Islandv10.json" -host-steam
'@ | Set-Content (Join-Path $stage 'Host on Steam.bat') -Encoding ascii

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

What is in this build
---------------------
  TEST LEVEL    empty terrain, no gameplay actors -- fastest thing to load
  LEVEL 1       the built-in map
  ENTER BASE    the hub; the travel board there flies to Island 1
  Island 1      Islandv10, also reachable directly via Play Islandv10.bat
  Training Range

Other maps in the authoring tree are left out of this package.

Multiplayer (two players)
-------------------------
MULTIPLAYER on the main menu. One player hosts, the other joins; the host picks
the level and everyone else loads the same map automatically, on join and on
every change after it.

  HOST ON STEAM          needs Steam running. The panel then shows a
                         steam:<id> address with a COPY INVITE button; send
                         that to a friend and they paste it into ADDRESS.
                         Works from anywhere -- Steam relays the connection,
                         so no port forwarding.
  HOST ON THIS NETWORK   direct UDP on the port shown. Only reachable by
                         someone on the same network unless that port is
                         forwarded.
  JOIN                   an address for a direct host (127.0.0.1 is this
                         machine), or steam:<id> for a Steam host.

Host on Steam.bat does the Steam version in one step, starting on Islandv10.

Steam
-----
If Steam is running, the game registers with it and shows as played. It
currently identifies itself as Valve's public test app (480), so Steam will
name it Spacewar until the game has its own app ID -- that is expected, not a
fault in the build. Without Steam, everything except Steam-relayed multiplayer
works exactly the same.

That borrowed app ID has one consequence worth knowing. "Join Game" on your
Steam profile tells the friend's Steam client to launch app 480, which on their
machine is Spacewar and not this game -- so if they are not already running it,
the button does nothing. Send them the steam:<id> address from the HOST ON
STEAM panel instead; pasting it into ADDRESS always works. Once they do have
the game open, Join Game reaches them normally.

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

# -- Verify the package can resolve its own imports ----------------------------
# Every DLL in the staged folder is re-walked, and each dependent must be either
# in the folder or a genuine system DLL. This is the check the build machine
# cannot do by running the exe: PATH hides a missing dependency locally, so the
# only honest test is name-by-name against the package's own contents.
$systemDirs = @("$env:SystemRoot\System32", "$env:SystemRoot\SysWOW64")
$stagedBinaries = @($exe) + (Get-ChildItem $stage -Filter *.dll -File |
                             ForEach-Object { $_.FullName })
$unresolved = @{}
foreach ($binary in $stagedBinaries) {
    foreach ($name in (Get-Dependents $binary)) {
        if (Test-Path (Join-Path $stage $name)) { continue }
        $inSystem = $false
        foreach ($dir in $systemDirs) {
            if (Test-Path (Join-Path $dir $name)) { $inSystem = $true; break }
        }
        # api-ms-win-* are the CRT's API sets, resolved by the loader from the
        # OS rather than from a file on disk.
        if ($inSystem -or $name -like 'api-ms-win-*') { continue }
        $unresolved[$name] = [IO.Path]::GetFileName($binary)
    }
}
if ($unresolved.Count -gt 0) {
    foreach ($name in $unresolved.Keys) {
        Write-Warning "$name is imported by $($unresolved[$name]) and is not in the package."
    }
    throw "The package is missing $($unresolved.Count) runtime DLL(s); it would fail to start on a machine without them on PATH."
}
Write-Host "  every import resolves inside the package" -ForegroundColor DarkGray

# Asset validation must pass before replacing a previous distribution.
& py -3 (Join-Path $PSScriptRoot 'validate-package-assets.py') $stage --repo $repo --manifest (Join-Path $stage 'asset-manifest.sha256')
if ($LASTEXITCODE -ne 0) { throw 'Package asset validation failed; previous distribution preserved.' }

# -- Commit the validated stage ------------------------------------------------
# Keep the prior package as a sibling backup so a failed copy or later review
# can be recovered without reconstructing the old content.
$backupStage = "$finalStage.previous-$(Get-Date -Format 'yyyyMMddHHmmss')"
if (Test-Path $finalStage) {
    Move-Item -LiteralPath $finalStage -Destination $backupStage -Force
}
try {
    Move-Item -LiteralPath $stage -Destination $finalStage -Force
    $stage = $finalStage
    Write-Host "  committed validated package; previous copy: $backupStage" -ForegroundColor Green
} catch {
    if ((Test-Path $backupStage) -and -not (Test-Path $finalStage)) {
        Move-Item -LiteralPath $backupStage -Destination $finalStage -Force
    }
    throw
}

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

