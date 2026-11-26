# ==========================================================================
# 文件：build-engine.ps1
# 功能：用 CMake 构建 HiStream 引擎（models/histream）和 STREAM 遥感引擎（models/stream），
#       并写出构建清单。
# 主要步骤：
#   1. 确定仓库根、源/构建目录；VcpkgRoot 缺省取环境变量 VCPKG_ROOT，其次仓库同级的 vcpkg
#   2. 定位 cmake（PATH 优先，否则在 Visual Studio 安装目录中搜索）
#   3. 配置并构建 HiStream（build/histream）
#   4. 配置并构建 STREAM（build/stream，独立 CMake 工程）
#   5. 必要时定位 Visual Studio 自带的 git，再调用 write-build-manifest.mjs 写构建清单
# 参数：-Configuration Debug|Release（默认 Release）；-VcpkgRoot（可选）。
# 输出：产物与构建清单位于 models/bin_x64/<Configuration>。
# 注意：产品引擎只含核心，不带外部扩展；Release 要求 USD 支持（HISTREAM_REQUIRE_USD=ON）。
# 与其他文件：被 tools/build-all.ps1 调用；依赖 tools/write-build-manifest.mjs。
# ==========================================================================

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

# 步骤 2：优先使用 PATH 中的 cmake；找不到时在 Visual Studio 的 CMake 扩展目录中搜索。
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

# 步骤 3：HiStream 配置参数。
$RuntimeDirectory = Join-Path $ProjectRoot 'models\bin_x64'
$BaseDirectory = Join-Path $ProjectRoot 'models'
if (-not (Test-Path -LiteralPath (Join-Path $BaseDirectory 'nvpro_core\cmake\setup.cmake'))) {
    throw "Missing local nvpro_core sources under $BaseDirectory"
}
$ConfigureArguments = @('-S', $SourceDirectory, '-B', $BuildDirectory,
    "-DBASE_DIRECTORY:PATH=$BaseDirectory", "-DOUTPUT_PATH:PATH=$RuntimeDirectory")
# Release 构建强制要求 USD（缺失则配置失败）；Debug 不强制。
if ($Configuration -eq 'Release') { $ConfigureArguments += '-DHISTREAM_REQUIRE_USD=ON' }
# Clear any extension list cached by a separate experimental build.
$ConfigureArguments += '-DHISTREAM_EXTENSION_DIRS='
if ($VcpkgRoot) {
    $ToolchainFile = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $ToolchainFile)) { throw "vcpkg toolchain was not found: $ToolchainFile" }
    $ConfigureArguments += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
}

# 配置后构建；任何一步失败都直接以其退出码退出。
& $CmakeExecutable @ConfigureArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $CmakeExecutable --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 步骤 4：STREAM 引擎，沿用同一个 vcpkg 工具链（若有）。
# STREAM remote-sensing image engine (stream.exe), an independent CMake project.
$StreamArguments = @('-S', (Join-Path $ProjectRoot 'models\stream'), '-B', (Join-Path $ProjectRoot 'build\stream'),
    "-DSTREAM_RUNTIME_DIR:PATH=$RuntimeDirectory")
if ($ToolchainFile) { $StreamArguments += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile" }
& $CmakeExecutable @StreamArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $CmakeExecutable --build (Join-Path $ProjectRoot 'build\stream') --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 步骤 5：写构建清单需要 git（记录 commit 与工作区是否有未提交改动）；
# PATH 与 GIT_EXECUTABLE 都没有时，回退到 Visual Studio 自带的 git。
if (-not $env:GIT_EXECUTABLE -and -not (Get-Command git -ErrorAction SilentlyContinue)) {
    $VisualStudioGit = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio') -Filter git.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*Team Explorer\Git\cmd\git.exe' } |
        Select-Object -First 1 -ExpandProperty FullName
    if ($VisualStudioGit) { $env:GIT_EXECUTABLE = $VisualStudioGit }
}

& node (Join-Path $PSScriptRoot 'write-build-manifest.mjs') (Join-Path $ProjectRoot "models\bin_x64\$Configuration")
exit $LASTEXITCODE
