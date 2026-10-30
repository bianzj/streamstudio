param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$SourceDirectory = Join-Path $ProjectRoot 'models\histream'
$BuildDirectory = Join-Path $ProjectRoot 'build\histream'
$FallbackVcpkgRoot = Join-Path (Split-Path $ProjectRoot -Parent) 'vcpkg'

if (-not $VcpkgRoot -and (Test-Path -LiteralPath $FallbackVcpkgRoot)) {
    $VcpkgRoot = $FallbackVcpkgRoot
}

$CmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $CmakeCommand) {
    $VisualStudioRoot = Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio'
    $CmakeExecutable = Get-ChildItem -LiteralPath $VisualStudioRoot -Filter cmake.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*CommonExtensions*Microsoft*CMake*' } |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $CmakeExecutable) { throw 'cmake.exe was not found.' }
} else {
    $CmakeExecutable = $CmakeCommand.Source
}

$ConfigureArguments = @('-S', $SourceDirectory, '-B', $BuildDirectory)
if ($Configuration -eq 'Release') { $ConfigureArguments += '-DHISTREAM_REQUIRE_USD=ON' }
if ($VcpkgRoot) {
    $ToolchainFile = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $ToolchainFile)) { throw "vcpkg toolchain was not found: $ToolchainFile" }
    $ConfigureArguments += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
}

& $CmakeExecutable @ConfigureArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $CmakeExecutable --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# STREAM remote-sensing image engine (stream.exe), an independent CMake project.
$StreamArguments = @('-S', (Join-Path $ProjectRoot 'models\stream'), '-B', (Join-Path $ProjectRoot 'build\stream'))
if ($ToolchainFile) { $StreamArguments += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile" }
& $CmakeExecutable @StreamArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $CmakeExecutable --build (Join-Path $ProjectRoot 'build\stream') --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $env:GIT_EXECUTABLE -and -not (Get-Command git -ErrorAction SilentlyContinue)) {
    $VisualStudioGit = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio') -Filter git.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*Team Explorer\Git\cmd\git.exe' } |
        Select-Object -First 1 -ExpandProperty FullName
    if ($VisualStudioGit) { $env:GIT_EXECUTABLE = $VisualStudioGit }
}

& node (Join-Path $PSScriptRoot 'write-build-manifest.mjs') (Join-Path $ProjectRoot "models\bin_x64\$Configuration")
exit $LASTEXITCODE
