# ---------------------------------------------------------------------------
#  build.ps1 -- configure + build DragonflySensorDiag
#
#  Usage:
#      .\build.ps1                 # Release
#      .\build.ps1 -Config Debug
#      .\build.ps1 -Fresh          # delete the build folder first
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [switch]$Fresh,
    [string]$Generator = 'Visual Studio 17 2022'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root 'build'

if ($Fresh -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "Configuring ($Generator, x64)..."
cmake -S $root -B $buildDir -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building ($Config)..."
cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = Join-Path $buildDir "$Config\DragonflySensorDiag.exe"
Write-Host ""
if (Test-Path $exe) {
    Write-Host "Built: $exe"
    Write-Host "Try:   & '$exe' --enumerate"
} else {
    Write-Warning "Build reported success but $exe was not found."
}
