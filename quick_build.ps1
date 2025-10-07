# Quick Build Script (faster rebuild for code changes)
Write-Host "=== Quick Rebuild ===" -ForegroundColor Cyan

# CMake path
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

Set-Location "build"

Write-Host "Building project..." -ForegroundColor Yellow
& $cmake --build . --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "`nBuild failed!" -ForegroundColor Red
    Set-Location ..
    exit 1
}

Set-Location ..

Write-Host "`n=== Build Complete ===" -ForegroundColor Green
Write-Host "Running GraphicEngine.exe..." -ForegroundColor Cyan
Write-Host ""

& ".\build\GraphicEngine.exe"
