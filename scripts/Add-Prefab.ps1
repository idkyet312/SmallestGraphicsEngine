# Authors a prefab JSON from a model file, with the mesh's real dimensions,
# triangle count and material flags measured out of the file rather than assumed.
#
# The editor's "Import Model..." button already writes a prefab, but it hardcodes
# targetSize 2.0 and box collision and copies the model into
# Content/Models/Imported. That suits a dropped-in test asset; it is wrong for
# art already authored at real-world scale and sitting in Content, which is why
# every shipped prop under Content/Prefabs/Props was written by hand instead.
# This script covers that case: it leaves the model where it is and picks the
# fields from what the file actually contains.
#
# What it measures, and what each measurement decides:
#   size        -- world-space bounds through the node transforms. Above
#                  -RealWorldScaleThreshold the art is already in metres, so
#                  targetSize stays 0 and the mesh is never rescaled.
#   triangles   -- reported so per-triangle collision is a decision made against
#                  a number, and warned about over -TriangleWarning.
#   doubleSided -- a material already double-sided, or alpha MASK/BLEND, means
#                  open-shell sheet geometry, which vanishes when back-face
#                  culled from the inside.
#
# Examples:
#   ./scripts/Add-Prefab.ps1 Content/Models/CarPark/Carpark_Asphalt.glb
#   ./scripts/Add-Prefab.ps1 Content/Models/Foo/Foo.glb -Inspect
#   ./scripts/Add-Prefab.ps1 Content/Models/Foo/Foo.glb -Collision box -Force
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    # Model to wrap. GLB is measured; FBX/GLTF are accepted but cannot be
    # measured here, so they fall back to defaults and say so.
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Model,

    # Prefab id. Defaults to props/<model stem>, lowercased.
    [string]$Id,

    # Display name in the editor palette. Defaults to the model stem, spaced.
    [string]$Name,

    # none | box | mesh. 'auto' picks mesh, matching every shipped prop: a box
    # seals doorways shut and buries walkable decks.
    [ValidateSet('none', 'box', 'mesh', 'auto')]
    [string]$Collision = 'auto',

    # Rescale the largest dimension to this many metres. 0 keeps authored scale.
    # Negative (the default) decides from the measured size.
    [double]$TargetSize = -1,

    # Force the double-sided import override instead of deciding from materials.
    [ValidateSet('auto', 'on', 'off')]
    [string]$DoubleSided = 'auto',

    # Where the JSON lands.
    [string]$OutputDirectory = 'Content/Prefabs/Props',

    # Print the measurements and the prefab that would be written, then stop.
    [switch]$Inspect,

    # Overwrite an existing prefab at the same path.
    [switch]$Force,

    # Below this many metres on every axis the model is treated as unscaled
    # source art and normalized via targetSize.
    [double]$RealWorldScaleThreshold = 0.5,

    # Triangle count above which per-triangle collision is called out as a cost.
    [int]$TriangleWarning = 50000
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# Throw rather than `exit`, so a bad argument reports one clean line and unwinds
# this script only -- `exit` would tear down the caller's session when the script
# is dot-sourced or run in a loop over several models.
function Fail($message) { throw $message }

# --- Locate the model, keeping the project-relative path the registry wants ---
# PrefabRegistry rejects anything that is not a project-relative FBX/GLB/GLTF, so
# an absolute path from the shell has to be folded back to a relative one.
$modelFull = [System.IO.Path]::GetFullPath((Join-Path $repo $Model))
if (-not (Test-Path -LiteralPath $modelFull)) {
    $modelFull = [System.IO.Path]::GetFullPath($Model)
}
if (-not (Test-Path -LiteralPath $modelFull -PathType Leaf)) {
    Fail "Model not found: $Model"
}
$repoFull = [System.IO.Path]::GetFullPath($repo)
if (-not $modelFull.StartsWith($repoFull, [StringComparison]::OrdinalIgnoreCase)) {
    Fail "Model must live inside the repo so the prefab path stays project-relative: $modelFull"
}
$modelRelative = $modelFull.Substring($repoFull.Length).TrimStart('\', '/').Replace('\', '/')

$extension = [System.IO.Path]::GetExtension($modelFull).ToLowerInvariant()
if ($extension -notin @('.glb', '.gltf', '.fbx')) {
    Fail "Model must be .fbx, .glb, or .gltf (got '$extension')"
}
$stem = [System.IO.Path]::GetFileNameWithoutExtension($modelFull)

# --- Measure ---
# GLB carries its JSON chunk in the clear, so bounds, triangles and material
# flags can be read without standing up a full importer.
$measured = $null
if ($extension -eq '.glb') {
    $measureScript = Join-Path $PSScriptRoot 'measure-glb.js'
    if (-not (Test-Path -LiteralPath $measureScript)) {
        Fail "Missing $measureScript, which does the measuring."
    }
    if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
        Fail 'node is required to measure a GLB. Install Node, or pass -TargetSize/-Collision/-DoubleSided explicitly.'
    }
    $raw = & node $measureScript $modelFull
    $measured = ($raw -join [Environment]::NewLine) | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $measured.error) {
        Fail "Could not measure ${modelRelative}: $($measured.error)"
    }
}

# --- Decide the fields from the measurements ---
$notes = New-Object System.Collections.Generic.List[string]

if ($measured) {
    $sx = [double]$measured.size[0]
    $sy = [double]$measured.size[1]
    $sz = [double]$measured.size[2]
    $largest = [Math]::Max($sx, [Math]::Max($sy, $sz))
    $notes.Add(("size {0:N2} x {1:N2} x {2:N2} m" -f $sx, $sy, $sz))
    $notes.Add("$($measured.triangles) triangles, $($measured.materialsUsed) material(s), $($measured.images) image(s)")
} else {
    $largest = 0
    $notes.Add("$extension cannot be measured by this script; using defaults")
}

# targetSize: real-world art keeps its authored size; tiny source art is
# normalized so it is not a speck in the world.
$resolvedTargetSize = 0.0
if ($TargetSize -ge 0) {
    $resolvedTargetSize = $TargetSize
    $notes.Add("targetSize $resolvedTargetSize (explicit)")
} elseif ($measured -and $largest -lt $RealWorldScaleThreshold) {
    $resolvedTargetSize = 2.0
    $notes.Add("targetSize 2.0 -- largest axis is only $([Math]::Round($largest, 3)) m, so this is unscaled source art")
} else {
    $notes.Add('targetSize 0 -- already at real-world scale, mesh is not rescaled')
}

# collision: mesh, like every shipped prop. A box only suits a solid convex
# blocker with nothing to walk into or stand on.
$resolvedCollision = $Collision
if ($Collision -eq 'auto') {
    $resolvedCollision = 'mesh'
    $notes.Add('collision mesh -- a box seals doorways and buries walkable surfaces')
} else {
    $notes.Add("collision $resolvedCollision (explicit)")
}
if ($resolvedCollision -eq 'mesh' -and $measured -and [int]$measured.triangles -gt $TriangleWarning) {
    Write-Warning ("$($measured.triangles) triangles is a heavy per-triangle collision build " +
        "(over $TriangleWarning). It is cached after the first load, but consider " +
        '-Collision box if this prop is a solid blocker.')
}

# forceDoubleSided: open-shell geometry disappears when viewed from inside.
$resolvedDoubleSided = $false
if ($DoubleSided -eq 'on') {
    $resolvedDoubleSided = $true
    $notes.Add('forceDoubleSided true (explicit)')
} elseif ($DoubleSided -eq 'off') {
    $notes.Add('forceDoubleSided false (explicit)')
} elseif ($measured) {
    # The flag is an override that stamps doubleSided onto every primitive, so it
    # earns its place only where the source art does not already say so. Art that
    # is uniformly double-sided already renders correctly and needs nothing; the
    # case that does is alpha cut-outs (foliage, chain-link) and mixed models
    # where some sheet material was left single-sided.
    # Default on for anything with border edges. Every prop measured so far is an
    # open shell to some degree, and the failure is asymmetric: the override on a
    # model that did not need it costs a little overdraw, while leaving it off a
    # model that did makes surfaces render inside-out. The car park is the worked
    # example -- one opaque single-sided material, nothing in the material flags
    # asking for it, but the slab has no underside at all, so it shaded
    # inside-out until the override went on.
    #
    # This is the one field the file cannot settle on its own, and the script
    # says so: the silo, watchtower and barrack set it while the visually similar
    # chain-link fence does not, and what separates them is whether the model has
    # an inside the player sees.
    if ($measured.allDoubleSided) {
        $notes.Add('forceDoubleSided false -- the source art is already double-sided throughout, so the override would be a no-op')
    } elseif ($measured.openShell -eq $true) {
        $resolvedDoubleSided = $true
        $notes.Add("forceDoubleSided true -- open shell ($($measured.boundaryEdges) border edges), so back faces would render inside-out")
        $notes.Add('  (verify in the editor; -DoubleSided off is cheaper if it looks right without)')
    } elseif ($measured.anyAlphaNonOpaque) {
        $resolvedDoubleSided = $true
        $notes.Add('forceDoubleSided true -- alpha MASK/BLEND cut-out geometry')
    } else {
        $notes.Add('forceDoubleSided false -- closed solid with opaque single-sided materials')
    }
}

# --- Id and name ---
if (-not $Id) {
    $slug = ($stem -replace '[^A-Za-z0-9]+', '_').Trim('_').ToLowerInvariant()
    if (-not $slug) { $slug = 'imported_model' }
    $Id = "props/$slug"
}
if ($Id -notmatch '^[a-z0-9_]+(/[a-z0-9_]+)+$') {
    Fail "Prefab id must be lowercase path segments like props/my_prop (got '$Id')"
}
if (-not $Name) {
    # Split camel case through Regex directly: PowerShell 5.1's -replace fires a
    # zero-width lookaround between every character, which turns Carpark_Asphalt
    # into "C a r p a r k ...". Separators collapse afterwards.
    $Name = [System.Text.RegularExpressions.Regex]::Replace(
        $stem, '(?<=[a-z0-9])(?=[A-Z])', ' ')
    $Name = (($Name -replace '[_\-]+', ' ') -replace '\s+', ' ').Trim()
}

# --- Build the document, matching the shipped prefabs field for field ---
$prefab = [ordered]@{
    schemaVersion = 2
    id            = $Id
    name          = $Name
    components    = [ordered]@{
        staticMesh = [ordered]@{
            path                     = $modelRelative
            defaultScale             = @(1.0, 1.0, 1.0)
            targetSize               = $resolvedTargetSize
            castShadow               = $true
            useMaterials             = $true
            materialAmbientScale     = 1.0
            materialViewFillStrength = 0.0
            forceDoubleSided         = $resolvedDoubleSided
        }
        collision  = [ordered]@{ shape = $resolvedCollision }
    }
}
$json = $prefab | ConvertTo-Json -Depth 8

$leaf = $Id.Split('/')[-1]
$outputPath = Join-Path $OutputDirectory "$leaf.json"

Write-Host ''
Write-Host $modelRelative -ForegroundColor Cyan
foreach ($note in $notes) { Write-Host "  $note" -ForegroundColor DarkGray }
Write-Host "  -> $($outputPath.Replace('\', '/'))  [$Id]" -ForegroundColor DarkGray

if ($Inspect) {
    Write-Host ''
    Write-Host $json
    Write-Host ''
    Write-Host 'Inspect only; nothing written.' -ForegroundColor Yellow
    return
}

if ((Test-Path -LiteralPath $outputPath) -and -not $Force) {
    Fail "$outputPath already exists. Pass -Force to overwrite."
}

if ($PSCmdlet.ShouldProcess($outputPath, 'Write prefab')) {
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    # The engine parses these as UTF-8. A BOM would be a stray leading character
    # for anything that reads the file without nlohmann's tolerance for one.
    # Resolve against the repo only when the path is relative: joining an
    # absolute -OutputDirectory onto the repo yields "E:\repo\C:\...", and
    # .NET's own working directory does not follow PowerShell's Set-Location.
    $absoluteOutput = if ([System.IO.Path]::IsPathRooted($outputPath)) {
        [System.IO.Path]::GetFullPath($outputPath)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $repo $outputPath))
    }
    [System.IO.File]::WriteAllText(
        $absoluteOutput,
        $json + [Environment]::NewLine,
        (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Wrote $outputPath" -ForegroundColor Green

    # LFS-tracked art has to be committed too, or the prefab resolves to nothing
    # on anyone else's checkout -- which the registry drops silently.
    & git ls-files --error-unmatch $modelRelative 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Note: $modelRelative is not committed yet -- git add it, or the prefab silently vanishes for everyone else." -ForegroundColor Yellow
    }
    Write-Host 'Restart the editor, or hit Refresh in the prefab palette, to see it.' -ForegroundColor DarkGray
}
