$ErrorActionPreference = 'Stop'
$binary = Join-Path $PSScriptRoot 'build\bin\DenseTrees.exe'
if (-not (Test-Path -LiteralPath $binary)) { & (Join-Path $PSScriptRoot 'build.ps1') }
& $binary
