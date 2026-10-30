param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

Push-Location $ProjectRoot
try {
    & npm.cmd run build:gui
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & (Join-Path $PSScriptRoot 'build-engine.ps1') -Configuration $Configuration
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
