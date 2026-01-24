# setup_and_run.ps1 - Create folders, copy shaders/models, and run the app
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $scriptDir "build"

Write-Host "Setting up GraphicEngine..." -ForegroundColor Cyan

# Create shaders folder
$shadersDir = Join-Path $buildDir "shaders"
if (-not (Test-Path $shadersDir)) {
    New-Item -ItemType Directory -Path $shadersDir -Force | Out-Null
}

# Create models folder  
$modelsDir = Join-Path $buildDir "models"
if (-not (Test-Path $modelsDir)) {
    New-Item -ItemType Directory -Path $modelsDir -Force | Out-Null
}

# Copy shaders
Write-Host "Copying shaders..." -ForegroundColor Yellow
Copy-Item (Join-Path $scriptDir "shaders\*") $shadersDir -Force

# Copy models
Write-Host "Copying models..." -ForegroundColor Yellow
Copy-Item (Join-Path $scriptDir "models\*") $modelsDir -Force

# Verify files
Write-Host "`nVerifying files:" -ForegroundColor Cyan
$hlslFiles = Get-ChildItem (Join-Path $shadersDir "*.hlsl") -ErrorAction SilentlyContinue
if ($hlslFiles) {
    Write-Host "  HLSL shaders found: $($hlslFiles.Count)" -ForegroundColor Green
    $hlslFiles | ForEach-Object { Write-Host "    - $($_.Name)" }
} else {
    Write-Host "  ERROR: No HLSL shaders found!" -ForegroundColor Red
}

$exePath = Join-Path $buildDir "GraphicEngine.exe"
if (Test-Path $exePath) {
    Write-Host "  Executable: Found" -ForegroundColor Green
} else {
    Write-Host "  ERROR: GraphicEngine.exe not found!" -ForegroundColor Red
    Write-Host "  Run build.ps1 first" -ForegroundColor Yellow
    exit 1
}

# Run the app
Write-Host "`nStarting GraphicEngine..." -ForegroundColor Cyan
Set-Location $buildDir
& .\GraphicEngine.exe

