param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [ValidateRange(1, 256)]
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$NoRun,
    # Build the agent worktree (../Engine-Agent) instead of this checkout.
    # That tree holds in-progress feature branches, so its build/ is separate
    # and this checkout is left untouched.
    [switch]$Agent
)

$ErrorActionPreference = "Stop"
if ($Agent) {
    $projectDir = Join-Path (Split-Path -Parent $PSScriptRoot) "Engine-Agent"
    if (-not (Test-Path -LiteralPath (Join-Path $projectDir "CMakeLists.txt"))) {
        Write-Error "Agent worktree not found at $projectDir. Create it with: git worktree add ../Engine-Agent <branch>"
    }
    $branch = (& git -C $projectDir rev-parse --abbrev-ref HEAD 2>$null)
    Write-Host "Agent worktree: $projectDir ($branch)" -ForegroundColor Yellow
} else {
    $projectDir = $PSScriptRoot
}
$buildDir = Join-Path $projectDir "build"

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $cmakeCandidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    $cmake = $cmakeCandidates | Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}

if (-not $cmake) {
    Write-Error "CMake not found. Install Visual Studio C++ CMake tools or add cmake.exe to PATH."
}

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $buildDir "CMakeCache.txt"))) {
    Write-Host "Configuring DX12 build..." -ForegroundColor Cyan
    $configureArgs = @(
        "-S", $projectDir,
        "-B", $buildDir,
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DUSE_DX12=ON"
    )

    $vcpkgRoot = $env:VCPKG_ROOT
    if (-not $vcpkgRoot -and (Test-Path -LiteralPath "C:\vcpkg")) {
        $vcpkgRoot = "C:\vcpkg"
    }
    if ($vcpkgRoot) {
        $toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path -LiteralPath $toolchain) {
            $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
        }
    }

    & $cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Building $Configuration with $Jobs parallel jobs..." -ForegroundColor Cyan
& $cmake --build $buildDir --config $Configuration `
    --target GraphicEngine --parallel $Jobs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$executable = Join-Path $buildDir "GraphicEngine.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    $executable = Join-Path $buildDir "$Configuration\GraphicEngine.exe"
}
if (-not (Test-Path -LiteralPath $executable)) {
    Write-Error "Build succeeded but GraphicEngine.exe was not found."
}

Write-Host "Build complete: $executable" -ForegroundColor Green
if ($NoRun) { exit 0 }

Write-Host "Running latest build..." -ForegroundColor Cyan
Push-Location $buildDir
try {
    & $executable
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
