# ==========================================================================
# 文件：build-all.ps1
# 功能：一键完整构建：先构建前端 GUI，再构建 HiStream 与 STREAM 两个引擎。
# 主要步骤：
#   1. 切换到仓库根目录（本脚本所在目录的上一级）
#   2. 运行 npm run build:gui 构建 Vite 前端
#   3. 调用同目录的 build-engine.ps1（传入 Debug/Release），并以其退出码作为本脚本退出码
# 参数：-Configuration Debug|Release（默认 Release）。
# 与其他文件：依赖 package.json 的 build:gui 脚本与 tools/build-engine.ps1。
# ==========================================================================

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

Push-Location $ProjectRoot
try {
    # 步骤 2：前端构建失败则立即以其退出码退出，不再构建引擎。
    & npm.cmd run build:gui
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & (Join-Path $PSScriptRoot 'build-engine.ps1') -Configuration $Configuration
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
