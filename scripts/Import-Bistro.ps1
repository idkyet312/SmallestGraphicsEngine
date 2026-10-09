[CmdletBinding()]
param(
    [string]$Source = 'C:\Users\Bas\Downloads\Bistro_v5_2\Bistro_v5_2',
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
& py -3.10-64 (Join-Path $PSScriptRoot 'import-bistro.py') $Source --configuration $Configuration
if ($LASTEXITCODE -ne 0) { throw "Bistro import failed with exit code $LASTEXITCODE" }
