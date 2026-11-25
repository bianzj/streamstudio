// ==========================================================================
// 文件：export-example.mjs
// 功能：把仓库中的示例工程导出成可独立运行的文件夹：复制示例目录，并补齐它引用的共享
//       assets/ 文件（DEM、气象、大气 LUT、OBJ/位置文件、光谱与纹理），不依赖 Stream Studio 自带资源。
// 用法：node tools/export-example.mjs <示例目录> <目标目录>（目标目录必须不存在）。
// 主要步骤：
//   1. 复制示例目录，但排除其 output/ 子目录，并重建空的 output/
//   2. 读 project.json 的 configuration，收集所有被引用的文件路径
//   3. 对示例内缺失的引用：仅允许来自仓库 assets/，从仓库复制过来；其他缺失视为错误
//   4. 复查全部引用均已在目标目录内，打印补充的资源数量与体积
// 约定：路径均为相对工程目录的路径；defined/meteo.txt 为内置气象占位，不需要实际文件。
// ==========================================================================

// Copies a repo example to a standalone folder, adding every shared assets/ file it
// references so it runs without Stream Studio's bundled resources.
//   node tools/export-example.mjs <example directory> <target directory>
import { cpSync, existsSync, mkdirSync, readFileSync, statSync } from 'node:fs'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const projectRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const [sourceArg, targetArg] = process.argv.slice(2)
if (!sourceArg || !targetArg) throw new Error('Usage: node tools/export-example.mjs <example directory> <target directory>')
const source = resolve(sourceArg), target = resolve(targetArg)
if (existsSync(target)) throw new Error(`Target already exists: ${target}`)

cpSync(source, target, { recursive: true, filter: (path) => relative(source, path).split(/[\\/]/)[0] !== 'output' })
mkdirSync(join(target, 'output'), { recursive: true })

const c = JSON.parse(readFileSync(join(target, 'project.json'), 'utf8')).configuration
// 步骤 2：收集工程引用的文件：DEM、气象、大气 LUT（仅启用时）、对象 OBJ 与位置文件、
// 各光谱的主文件及 physical/class/openness 三类纹理（仅启用时）。
const referenced = new Set([
  c.scene?.demFile, c.meteo?.path, c.atmosphere?.enabled ? c.atmosphere.lutFile : '',
  ...(c.objects?.items || []).flatMap((item) => [item.fileName, item.positionFile]),
  ...(c.spectra || []).flatMap((s) => [s.fileName, s.physicalTexture?.enabled && s.physicalTexture.fileName, s.classTexture?.enabled && s.classTexture.fileName, s.opennessTexture?.enabled && s.opennessTexture.fileName])
].filter(Boolean))
let added = 0, bytes = 0
// 步骤 3：已存在则跳过；缺失的只能从仓库 assets/ 取，否则报错。
for (const path of referenced) {
  if (path === 'defined/meteo.txt' || existsSync(join(target, path))) continue
  if (!/^assets[\\/]/.test(path)) throw new Error(`Missing project file: ${path}`)
  mkdirSync(dirname(join(target, path)), { recursive: true })
  cpSync(join(projectRoot, path), join(target, path))
  added += 1
  bytes += statSync(join(target, path)).size
}
const missing = [...referenced].filter((path) => path !== 'defined/meteo.txt' && !existsSync(join(target, path)))
if (missing.length) throw new Error(`Still missing: ${missing.join(', ')}`)
console.log(`Exported ${source} -> ${target}: ${referenced.size} referenced files all local, ${added} shared assets added (${(bytes / 1048576).toFixed(1)} MB)`)
