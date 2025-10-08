# Build and Run Script for SmallestGraphicsEngine
Write-Host "=== Building SmallestGraphicsEngine ===" -ForegroundColor Cyan

# CMake path
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

# Check if build directory exists, if not create it
if (!(Test-Path "build")) {
    Write-Host "Creating build directory..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# Navigate to build directory
Set-Location "build"

# Configure with CMake
Write-Host "`nConfiguring with CMake..." -ForegroundColor Yellow
& $cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

if ($LASTEXITCODE -ne 0) {
    Write-Host "`nCMake configuration failed!" -ForegroundColor Red
    Set-Location ..
    exit 1
}

# Build the project
Write-Host "`nBuilding project..." -ForegroundColor Yellow
& $cmake --build . --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "`nBuild failed!" -ForegroundColor Red
    Set-Location ..
    exit 1
}

# Return to root directory
Set-Location ..

Write-Host "`n=== Build Complete ===" -ForegroundColor Green
Write-Host "`nRunning GraphicEngine.exe..." -ForegroundColor Cyan
Write-Host "Controls:" -ForegroundColor Yellow
Write-Host "  - WASD: Move camera" -ForegroundColor Gray
Write-Host "  - Mouse: Look around" -ForegroundColor Gray
Write-Host "  - TAB: Toggle UI" -ForegroundColor Gray
Write-Host "  - ESC: Exit" -ForegroundColor Gray
Write-Host ""

# Run the executable
& ".\build\GraphicEngine.exe"
