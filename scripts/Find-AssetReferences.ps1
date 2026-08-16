# Discovers which Content/Models directories are actually referenced, by
# scanning the things that reference them rather than by keeping a hand-written
# list. Dot-sourced by package-release.ps1; also runnable on its own to see what
# the package would keep and why.
#
# This exists because the keep-list it replaces went stale silently. A model
# added to a prefab shipped fine on the author's machine (the repo has it) and
# was missing from the package, which only shows up once a level loads on
# someone else's machine. The tower and turret were lost exactly that way.
#
# THREE reference forms have to be found. Missing the third is what untextured
# the terrain the first time the old list was pruned:
#
#   1. Full quoted paths in C++:      "Content/Models/Humvee/humvee.fbx"
#   2. Full quoted paths in data:     {"path": "Content/Models/Tower/Tower.fbx"}
#   3. BARE folder names joined to kModelRoot at the call site:
#      TerrainRendererDX12 names its splat layers "terrain/dark_rock" and
#      "Grass3/Grass004_2K-JPG" with no Content/Models prefix, so a search for
#      the full path finds nothing and both directories look unused.
#
# Form 3 cannot be found by pattern alone -- any string literal could be a bare
# folder name. It is resolved by existence instead: take every plausible
# "<dir>/<something>" literal in the source, and keep it only if
# Content/Models/<dir> is a real directory on disk. That turns a guess into a
# filesystem check.

Set-StrictMode -Version Latest

# Strips // and /* */ comments from C++ so prose cannot keep an asset alive.
# This is not a real lexer -- a "//" inside a string literal is treated as the
# start of a comment -- but the failure mode is safe in the wrong direction only
# for URLs, and no asset path in this repo contains "//". Worth the imprecision:
# without it, a comment mentioning a mesh by name pins that mesh into every
# package forever, which is how h1.glb kept shipping.
# JSON has no comments, so it is passed through untouched.
function Remove-CodeComments {
    param([string]$Text, [string]$Path)
    if ($Path -like '*.json') { return $Text }
    $Text = [regex]::Replace($Text, '/\*.*?\*/', ' ', 'Singleline')
    $Text = [regex]::Replace($Text, '(?m)//.*$', ' ')
    return $Text
}

function Get-ReferencedModelDirs {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Repo,
        # Emits a line per directory naming the file that referenced it, so a
        # surprising keep or drop can be traced to its source.
        [switch]$Explain
    )

    $modelRoot = Join-Path $Repo 'Content\Models'
    if (-not (Test-Path $modelRoot)) {
        throw "Content/Models not found at $modelRoot"
    }

    # Directory name (as it appears on disk) -> list of files referencing it.
    # Keyed case-insensitively because C++ literals and the filesystem disagree
    # about case more often than not ("SVD.FBX" vs "svd.fbx").
    $refs = [System.Collections.Generic.Dictionary[string, System.Collections.Generic.List[string]]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)

    # Real top-level directory names, used both to canonicalise a match and to
    # validate bare-folder candidates.
    $actualDirs = @{}
    foreach ($d in (Get-ChildItem $modelRoot -Directory)) {
        $actualDirs[$d.Name] = $d.Name
    }

    function Add-Ref {
        param([string]$Dir, [string]$Source)
        if (-not $Dir) { return }
        # Canonicalise to the on-disk spelling; ignore anything that is not a
        # real directory, so a typo in a literal cannot inflate the keep set.
        if (-not $actualDirs.ContainsKey($Dir)) { return }
        $canonical = $actualDirs[$Dir]
        if (-not $refs.ContainsKey($canonical)) {
            $refs[$canonical] = [System.Collections.Generic.List[string]]::new()
        }
        if (-not $refs[$canonical].Contains($Source)) {
            $refs[$canonical].Add($Source)
        }
    }

    # -- Forms 1 and 2: full "Content/Models/<dir>/..." paths -----------------
    # Scanned in source AND data with one pattern, since both quote the path the
    # same way. Backslash-separated literals appear in a few places, so both
    # separators are accepted.
    $scanDirs = @('src', 'shaders', 'prefabs', 'levels', 'tools',
                  'Content\Prefabs', 'Content\Levels')
    $scanExts = @('*.cpp', '*.h', '*.hlsl', '*.hlsli', '*.json')

    $fullPathPattern = 'Content[/\\]+Models[/\\]+([^"''/\\<>|*?]+)'

    foreach ($dir in $scanDirs) {
        $path = Join-Path $Repo $dir
        if (-not (Test-Path $path)) { continue }
        $files = Get-ChildItem $path -Recurse -File -Include $scanExts -ErrorAction SilentlyContinue
        foreach ($file in $files) {
            $text = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
            if (-not $text) { continue }
            $text = Remove-CodeComments -Text $text -Path $file.FullName
            $rel = $file.FullName.Substring($Repo.Length).TrimStart('\')
            foreach ($m in [regex]::Matches($text, $fullPathPattern)) {
                Add-Ref -Dir $m.Groups[1].Value.Trim() -Source $rel
            }
        }
    }

    # -- Form 3: bare "<dir>/<something>" literals resolved by existence ------
    # Only source is scanned: a bare folder name is meaningful because C++ joins
    # it to kModelRoot, whereas data files always carry the full path.
    #
    # The candidate pattern is deliberately loose (any two-segment quoted
    # literal). Precision comes from the actualDirs check inside Add-Ref, not
    # from the regex -- a literal like "vs_5_0" or "utf-8" simply is not a
    # directory under Content/Models and drops out.
    $barePattern = '"([A-Za-z0-9_\-. ]+)[/\\][^"]*"'
    foreach ($dir in @('src', 'tools')) {
        $path = Join-Path $Repo $dir
        if (-not (Test-Path $path)) { continue }
        $files = Get-ChildItem $path -Recurse -File -Include @('*.cpp', '*.h') -ErrorAction SilentlyContinue
        foreach ($file in $files) {
            $text = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
            if (-not $text) { continue }
            $text = Remove-CodeComments -Text $text -Path $file.FullName
            $rel = $file.FullName.Substring($Repo.Length).TrimStart('\')
            foreach ($m in [regex]::Matches($text, $barePattern)) {
                Add-Ref -Dir $m.Groups[1].Value.Trim() -Source "$rel (bare)"
            }
        }
    }

    if ($Explain) {
        foreach ($key in ($refs.Keys | Sort-Object)) {
            Write-Host ("  {0}" -f $key) -ForegroundColor Gray
            foreach ($src in $refs[$key]) {
                Write-Host ("      <- {0}" -f $src) -ForegroundColor DarkGray
            }
        }
    }

    return $refs
}

# Loose files directly under Content/Models (h2.glb, gun.glb, ...) referenced by
# name. Same existence-checked approach as the directories above.
function Get-ReferencedModelFiles {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Repo)

    $modelRoot = Join-Path $Repo 'Content\Models'
    $actualFiles = @{}
    foreach ($f in (Get-ChildItem $modelRoot -File)) { $actualFiles[$f.Name] = $f.Name }

    $keep = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)

    $scanDirs = @('src', 'tools', 'prefabs', 'levels',
                  'Content\Prefabs', 'Content\Levels')
    $scanExts = @('*.cpp', '*.h', '*.json')
    foreach ($dir in $scanDirs) {
        $path = Join-Path $Repo $dir
        if (-not (Test-Path $path)) { continue }
        $files = Get-ChildItem $path -Recurse -File -Include $scanExts -ErrorAction SilentlyContinue
        foreach ($file in $files) {
            $text = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
            if (-not $text) { continue }
            $text = Remove-CodeComments -Text $text -Path $file.FullName
            foreach ($name in $actualFiles.Keys) {
                # Must appear inside a quoted literal, not merely somewhere in
                # the file. A bare Contains() kept h1.glb (80 MB) on the strength
                # of the words "the h1.glb house" in a comment about backface
                # culling -- a file nothing loads, shipped because prose named it.
                $quoted = '"[^"]*' + [regex]::Escape($name) + '[^"]*"'
                if ([regex]::IsMatch($text, $quoted)) {
                    [void]$keep.Add($actualFiles[$name])
                }
            }
        }
    }
    return $keep
}

# Writes the discovered set as a --only list for AssetCooker: content-relative
# paths, one per line. Keeping cook and package on the same discovered set is
# the point -- a directory that ships must be cooked, and cooking art that never
# ships is what produced a 270 MB ship.sgeasset nothing loads.
function Write-CookOnlyList {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Repo,
        [Parameter(Mandatory)][string]$Path
    )
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('# Generated by scripts/Find-AssetReferences.ps1 -- do not edit.')
    $lines.Add('# Content-relative paths that the engine or its data reference.')
    foreach ($dir in ((Get-ReferencedModelDirs -Repo $Repo).Keys | Sort-Object)) {
        $lines.Add("Models/$dir")
    }
    foreach ($file in ((Get-ReferencedModelFiles -Repo $Repo) | Sort-Object)) {
        $lines.Add("Models/$file")
    }
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Set-Content -Path $Path -Value $lines -Encoding utf8
    return $Path
}

# Standalone use: ./scripts/Find-AssetReferences.ps1
if ($MyInvocation.InvocationName -ne '.') {
    $repo = Split-Path -Parent $PSScriptRoot
    Write-Host "Referenced Content/Models directories:" -ForegroundColor Cyan
    $found = Get-ReferencedModelDirs -Repo $repo -Explain
    Write-Host ""
    Write-Host "Referenced loose files:" -ForegroundColor Cyan
    foreach ($f in (Get-ReferencedModelFiles -Repo $repo | Sort-Object)) {
        Write-Host "  $f" -ForegroundColor Gray
    }
    Write-Host ""
    $all = (Get-ChildItem (Join-Path $repo 'Content\Models') -Directory).Name
    $unref = $all | Where-Object { -not $found.ContainsKey($_) } | Sort-Object
    Write-Host ("Unreferenced (would be pruned): {0}" -f $unref.Count) -ForegroundColor Yellow
    foreach ($d in $unref) { Write-Host "  $d" -ForegroundColor DarkYellow }
}
