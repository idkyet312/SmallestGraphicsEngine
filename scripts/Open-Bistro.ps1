[CmdletBinding()]
param([switch]$Forward)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $repo 'build'
$executable = Join-Path $runtime 'GraphicEngine.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'Build Release first: ./build.ps1 -Configuration Release -NoRun' }
if (!(Test-Path -LiteralPath (Join-Path $runtime 'Content/Cooked/Models/BistroExterior/BistroExterior.sgeasset'))) {
    throw 'Import first: ./scripts/Import-Bistro.ps1'
}
$arguments = @('--editor', '--level=Content/Levels/BistroExterior.json')
if ($Forward) { $arguments += '--forward' }
Start-Process -FilePath $executable -WorkingDirectory $runtime -ArgumentList $arguments
