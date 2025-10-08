# Build script for GraphicEngine using Visual Studio's CMake
# This script locates Visual Studio's CMake and builds the project

Write-Host "=== GraphicEngine Build Script ===" -ForegroundColor Cyan

# Common Visual Studio CMake paths
$vsPaths = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

$cmake = $null
foreach ($path in $vsPaths) {
    if (Test-Path $path) {
        $cmake = $path
        Write-Host "Found CMake at: $cmake" -ForegroundColor Green
        break
    }
}

if (-not $cmake) {
    # Try to find cmake in PATH
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) {
        $cmake = $cmake.Source
        Write-Host "Found CMake in PATH: $cmake" -ForegroundColor Green
    } else {
        Write-Host "ERROR: CMake not found. Please install CMake or Visual Studio with C++ CMake tools." -ForegroundColor Red
        Write-Host "You need to install:" -ForegroundColor Yellow
        Write-Host "  1. Visual Studio 2019/2022 with 'Desktop development with C++'" -ForegroundColor Yellow
        Write-Host "  2. Make sure 'C++ CMake tools for Windows' is selected in the installer" -ForegroundColor Yellow
        exit 1
    }
}

# Get script directory
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $scriptDir "build"

# Create build directory if it doesn't exist
if (-not (Test-Path $buildDir)) {
    Write-Host "Creating build directory..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

# Look for vcpkg toolchain file
$vcpkgToolchain = $null
$vcpkgPaths = @(
    "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake",
    "C:\vcpkg\scripts\buildsystems\vcpkg.cmake",
    "C:\tools\vcpkg\scripts\buildsystems\vcpkg.cmake",
    "$HOME\vcpkg\scripts\buildsystems\vcpkg.cmake"
)

foreach ($path in $vcpkgPaths) {
    if (Test-Path $path) {
        $vcpkgToolchain = $path
        Write-Host "Found vcpkg toolchain: $vcpkgToolchain" -ForegroundColor Green
        break
    }
}

if (-not $vcpkgToolchain) {
    Write-Host "Warning: vcpkg not found. Dependencies must be installed manually." -ForegroundColor Yellow
    Write-Host "To install vcpkg:" -ForegroundColor Yellow
    Write-Host "  git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg" -ForegroundColor White
    Write-Host "  cd C:\vcpkg" -ForegroundColor White
    Write-Host "  .\bootstrap-vcpkg.bat" -ForegroundColor White
    Write-Host "  .\vcpkg integrate install" -ForegroundColor White
    Write-Host "  .\vcpkg install glfw3:x64-windows glm:x64-windows glad:x64-windows" -ForegroundColor White
}

# Configure with CMake
Write-Host "`nConfiguring project with CMake..." -ForegroundColor Cyan
Push-Location $buildDir
try {
    $cmakeArgs = @("..", "-G", "Visual Studio 17 2022", "-A", "x64")
    if ($vcpkgToolchain) {
        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    }
    
    & $cmake $cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nCMake configuration failed. Common issues:" -ForegroundColor Red
        Write-Host "  - Missing dependencies: GLFW, GLM, GLAD" -ForegroundColor Yellow
        if (-not $vcpkgToolchain) {
            Write-Host "  - vcpkg not detected. Install dependencies manually or set VCPKG_ROOT" -ForegroundColor Yellow
        } else {
            Write-Host "  - Install via vcpkg: vcpkg install glfw3:x64-windows glm:x64-windows glad:x64-windows" -ForegroundColor Yellow
        }
        exit 1
    }
} catch {
    Write-Host "CMake configuration error: $_" -ForegroundColor Red
    exit 1
} finally {
    Pop-Location
}

# Build
Write-Host "`nBuilding project (Release)..." -ForegroundColor Cyan
Push-Location $buildDir
try {
    & $cmake --build . --config Release
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`n=== Build Successful ===" -ForegroundColor Green
        Write-Host "Executable location: $buildDir\Release\GraphicEngine.exe" -ForegroundColor Green
        Write-Host "`nTo run the application:" -ForegroundColor Cyan
        Write-Host "  cd `"$scriptDir`"" -ForegroundColor White
        Write-Host "  .\build\Release\GraphicEngine.exe" -ForegroundColor White
    } else {
        Write-Host "`n=== Build Failed ===" -ForegroundColor Red
        exit 1
    }
} catch {
    Write-Host "Build error: $_" -ForegroundColor Red
    exit 1
} finally {
    Pop-Location
}
