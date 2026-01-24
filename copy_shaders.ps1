# Quick rebuild and copy shaders script
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $scriptDir "build"
$shadersDir = Join-Path $scriptDir "shaders"
$buildShadersDir = Join-Path $buildDir "shaders"

# Copy all HLSL shaders
Write-Host "Copying HLSL shaders..." -ForegroundColor Cyan
Copy-Item "$shadersDir\*.hlsl" "$buildShadersDir\" -Force
Write-Host "Shaders copied to $buildShadersDir" -ForegroundColor Green

# List shaders
Write-Host "`nHLSL shaders in build:" -ForegroundColor Yellow
Get-ChildItem "$buildShadersDir\*.hlsl" | ForEach-Object { Write-Host "  $_" }

Write-Host "`nDone! Run the application with:" -ForegroundColor Cyan
Write-Host "  cd build; .\GraphicEngine.exe" -ForegroundColor White

