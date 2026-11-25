// ==========================================================================
// 文件：write-build-manifest.mjs
// 功能：构建完成后，在运行时目录写 build-manifest.json，记录本次构建所用源码与已部署着色器二进制的
//   SHA-256，用来判断"部署的引擎/着色器是否由当前源码构建"。命令行：
//   node tools/write-build-manifest.mjs [运行时目录，缺省 models/bin_x64/Release]。
// 主要步骤：
//   1. 收集源码清单（histream、stream、前端、服务端、部分 tools、package.json）并逐个算 SHA-256，
//      再对清单整体算 sourceTreeSha256
//   2. 列出 CMake 实际生成的 SPIR-V 模块（builtShaderModules，需与 models/histream/CMakeLists.txt 同步），
//      与运行时 shader/ 目录里实际存在的 .spv 比较：已构建、过期（stale，不是本次构建产物）、缺失（missing）
//   3. 读取 git 提交与工作区是否有改动（dirty）；Git 不可用时只告警
//   4. 记录引擎主程序与运行时附带的二进制（histream-usd、stream、lavapipe 等）的哈希
//   5. 写 build-manifest.json（kind = stream-studio-build，schemaVersion 1），并向 stdout 打印摘要 JSON
// 关系：被打包/发布流程调用（待核对）；环境变量 GIT_EXECUTABLE 可指定 git 路径。
//   检查清单里的 tools 文件（runtime-scene、surface-textures、png、tarp-cover、usd-export、usd-import）
//   是服务端运行时会用到的脚本，改动它们会改变 sourceTreeSha256。
// ==========================================================================

// Record the exact source and deployed shader binaries of a completed build.
import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const runtime = path.resolve(root, process.argv[2] || 'models/bin_x64/Release')
const sha256 = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
// 作用：递归列出 directory 下满足 predicate 的文件，按路径排序返回（保证清单顺序稳定、哈希可复现）。
function inventory(directory, predicate) {
  const files = []
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const file = path.join(directory, entry.name)
    if (entry.isDirectory()) files.push(...inventory(file, predicate))
    else if (entry.isFile() && predicate(file)) files.push(file)
  }
  return files.sort()
}
// 步骤 1：源码清单。包含构建脚本、histream 源码与着色器、stream 引擎源码与类别表、
//   前端（src/renderer）、服务端（server.mjs、server/）、被服务端使用的 tools 脚本与 package.json。
const sourceFiles = [
  path.join(root, 'models/cmake/copy-runtime-dependencies.cmake'),
  path.join(root, 'models/histream/CMakeLists.txt'),
  ...inventory(path.join(root, 'models/histream/src'), p => /\.(cpp|h|inl)$/.test(p)),
  ...inventory(path.join(root, 'models/histream/shader'), p => /\.(glsl|h|comp|rgen|rchit|rmiss|vert|frag)$/.test(p)),
  path.join(root, 'models/stream/CMakeLists.txt'),
  ...inventory(path.join(root, 'models/stream'), p => /\.(cpp|h|hpp)$/.test(p)),
  path.join(root, 'models/stream/classes/classes.json'),
  ...inventory(path.join(root, 'src/renderer'), p => /\.(js|html|css)$/.test(p)),
  path.join(root, 'server.mjs'),
  ...inventory(path.join(root, 'server'), p => /\.mjs$/.test(p)),
  ...['tools/runtime-scene.mjs', 'tools/surface-textures.mjs', 'tools/png.mjs',
    'tools/tarp-cover.mjs', 'tools/usd-export.mjs', 'tools/usd-import.mjs']
    .map(file => path.join(root, file)),
  path.join(root, 'package.json')
]
const sources = [...new Set(sourceFiles)]
  .map(file => ({ path: path.relative(root, file).replaceAll('\\', '/'), sha256: sha256(file) }))
// 步骤 2：着色器二进制核对（下方原有注释说明了 stale 与 missing 的含义）。
//   路径比较统一转小写；voxeleb 的 *_most 与 scattering_trace 为带宏变体的额外产物。
// SPIR-V modules the CMake build actually produces (keep in sync with
// models/histream/CMakeLists.txt).  The runtime shader/ directory also holds
// files that are copied, not compiled (copy-core-shaders.cmake never deletes
// deployed files), e.g. old shader/cases/VoxelTriTracing/*.spv; those are listed
// separately as staleShaderBinaries so "manifest matches" means "built from
// the current sources" and nothing else.
const shaderSourceRoot = path.join(root, 'models/histream/shader')
const facetShaders = ['visibility.vert', 'visibility.frag', 'visibility_resolve.comp', 'direct_resolve.comp', 'radiosity.comp', 'ground_mean.comp', 'incident.comp']
const builtShaderModules = new Set([
  ...['offline.rgen', 'closestHit.rchit', 'rmiss.rmiss', 'shadow.rmiss'].map(file => `raytracing/${file}.spv`),
  ...[...facetShaders, 'spectral.comp'].map(file => `facetrt/${file}.spv`),
  ...[...facetShaders, 'energy.comp'].map(file => `faceteb/${file}.spv`),
  ...['voxeleb', 'voxelrt'].flatMap(kind => fs.readdirSync(path.join(shaderSourceRoot, kind))
    .filter(file => file.endsWith('.comp')).map(file => `${kind}/${file}.spv`)),
  // VoxelEB Monin-Obukhov variants compiled with -DVOXELEB_MOST (CMakeLists.txt).
  ...['aeresist', 'budget', 'evapo'].map(name => `voxeleb/${name}_most.comp.spv`),
  // VoxelEB scattering ray tracing stages compiled with -DSCATTER_TRACE_STAGE.
  'voxeleb/voxelrad_scattering_trace.comp.spv'
].map(file => file.toLowerCase()))
const deployedShaderBinaries = inventory(path.join(runtime, 'shader'), p => p.endsWith('.spv'))
  .map(file => ({ path: path.relative(runtime, file).replaceAll('\\', '/'), sha256: sha256(file) }))
const isBuiltShader = entry => builtShaderModules.has(entry.path.replace(/^shader\//, '').toLowerCase())
const shaderBinaries = deployedShaderBinaries.filter(isBuiltShader)
const staleShaderBinaries = deployedShaderBinaries.filter(entry => !isBuiltShader(entry))
const missingShaderBinaries = [...builtShaderModules]
  .filter(module => !shaderBinaries.some(entry => entry.path.replace(/^shader\//, '').toLowerCase() === module))
if (staleShaderBinaries.length) console.warn(`write-build-manifest: ${staleShaderBinaries.length} deployed .spv not produced by this build (listed as staleShaderBinaries): ${staleShaderBinaries.map(entry => entry.path).join(', ')}`)
if (missingShaderBinaries.length) console.warn(`write-build-manifest: ${missingShaderBinaries.length} expected .spv missing from runtime: ${missingShaderBinaries.join(', ')}`)
// 步骤 3：记录 git HEAD 与工作区是否有未提交改动；失败时 commit 为空串、dirty 为 null。
let commit = '', dirty = null
const git = process.env.GIT_EXECUTABLE || 'git'
try {
  commit = execFileSync(git, ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim()
  dirty = Boolean(execFileSync(git, ['-c', 'core.safecrlf=false', 'status', '--porcelain=v1'], { cwd: root, encoding: 'utf8' }).trim())
} catch { console.warn('write-build-manifest: Git unavailable; set GIT_EXECUTABLE to record commit and dirty state.') }
// 步骤 4：引擎主程序与运行时附带二进制（只记录存在的文件）。
const engine = path.join(runtime, process.platform === 'win32' ? 'histream.exe' : 'histream')
const runtimeBinaries = ['histream-usd.exe', 'stream.exe', 'stream-defined/classes.json', 'lavapipe/vulkan_lvp.dll', 'lavapipe/lvp_icd.x86_64.json']
  .map(file => path.join(runtime, file))
  .filter(file => fs.existsSync(file))
  .map(file => ({ path: path.relative(runtime, file).replaceAll('\\', '/'), sha256: sha256(file) }))
// 步骤 5：组装清单并写盘。sourceTreeSha256 = 对 sources 数组的 JSON 求哈希，用于一眼比较源码树是否变化。
const manifest = {
  kind: 'stream-studio-build', schemaVersion: 1,
  applicationVersion: JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8')).version,
  createdAt: new Date().toISOString(), commit, dirty,
  sourceTreeSha256: crypto.createHash('sha256').update(JSON.stringify(sources)).digest('hex'),
  engine: { path: path.basename(engine), sha256: sha256(engine) },
  runtimeBinaries, sources, shaderBinaries, staleShaderBinaries, missingShaderBinaries
}
fs.writeFileSync(path.join(runtime, 'build-manifest.json'), JSON.stringify(manifest, null, 2) + '\n')
console.log(JSON.stringify({ version: manifest.applicationVersion, engineSHA256: manifest.engine.sha256,
  sourceTreeSHA256: manifest.sourceTreeSha256, shaderCount: shaderBinaries.length,
  staleShaderCount: staleShaderBinaries.length, missingShaderCount: missingShaderBinaries.length }))
