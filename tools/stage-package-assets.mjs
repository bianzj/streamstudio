// ==========================================================================
// 文件：stage-package-assets.mjs
// 功能：打包前把 assets/ 整理到 .packaging/assets/（安装包使用的精简资源副本），
//       排除体积过大、生成产物与源数据，并同步重写 OBJ 库与 RAMI 库的 catalog.json
// 主要步骤：
//   1. scan：递归扫描 assets/，记录 >15 MiB 的 OBJ、examples 下的 output 目录、_*_source 目录
//   2. findIncompleteRamiScenes：含超大 OBJ 的 RAMI 场景整体剔除（避免场景残缺）
//   3. copyTree：删除旧的 .packaging/assets 后复制，按 excluded() 规则跳过
//   4. rewriteObjCatalog / rewriteRamiCatalog：把被剔除的模型、场景从目录 JSON 中移除
//   5. 打印统计（文件数、体积、各类排除项数量）
// 排除规则：超大 OBJ；crop 目录下不在白名单的 OBJ（只带 4 个作物模型）；log_nvprosample.txt；
//       examples 下的 output；_*_source 目录；含超大模型的 RAMI 场景目录。
// 约定：路径比较使用绝对路径；体积上限 MAX_OBJ_BYTES = 15 MiB。
// ==========================================================================

import { copyFile, mkdir, readFile, readdir, rm, stat, writeFile } from 'node:fs/promises'
import { dirname, extname, join, relative, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'

const MAX_OBJ_BYTES = 15 * 1024 * 1024
const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const sourceRoot = join(projectRoot, 'assets')
const examplesRoot = join(sourceRoot, 'examples')
const targetRoot = join(projectRoot, '.packaging', 'assets')
const cropModelRoot = join(sourceRoot, 'obj-library', 'crop')
// 作物模型白名单：crop 目录下只打包这 4 个 OBJ，其余作物模型不进安装包。
const packagedCropModels = new Set([
  join(cropModelRoot, 'maize-growth', 'maize_stage_04.obj'),
  join(cropModelRoot, 'wheat.obj'),
  join(cropModelRoot, 'rice.obj'),
  join(cropModelRoot, 'sunflower.obj')
])

const sourceDirectories = new Set()
const generatedOutputDirectories = new Set()
const oversizedObjects = new Map()
const excludedSceneDirectories = new Set()
let copiedFiles = 0
let copiedBytes = 0

// 判断 path 是否位于 directory 内（含自身）；用相对路径是否以 .. 开头判定。
function isInside(path, directory) {
  const local = relative(directory, path)
  return local === '' || (!local.startsWith(`..${sep}`) && local !== '..')
}

// 步骤 1：递归扫描。命中的 output / _*_source 目录只记录、不再深入；超大 OBJ 记录其字节数。
async function scan(path) {
  for (const entry of await readdir(path, { withFileTypes: true })) {
    const fullPath = join(path, entry.name)
    if (entry.isDirectory()) {
      if (entry.name.toLowerCase() === 'output' && isInside(fullPath, examplesRoot)) {
        generatedOutputDirectories.add(fullPath)
        continue
      }
      if (/^_.*_source$/i.test(entry.name)) {
        sourceDirectories.add(fullPath)
        continue
      }
      await scan(fullPath)
      continue
    }
    if (!entry.isFile() || extname(entry.name).toLowerCase() !== '.obj') continue
    const info = await stat(fullPath)
    if (info.size > MAX_OBJ_BYTES) oversizedObjects.set(fullPath, info.size)
  }
}

// 步骤 2：按 catalog.json 逐个读取 RAMI 场景清单，其中任一 OBJ 超限则整场景目录加入排除集合。
async function findIncompleteRamiScenes() {
  const catalogPath = join(sourceRoot, 'rami-library', 'catalog.json')
  const catalog = JSON.parse(await readFile(catalogPath, 'utf8'))
  for (const scene of catalog.scenes || []) {
    const manifestPath = resolve(dirname(catalogPath), scene.manifest)
    const manifest = JSON.parse(await readFile(manifestPath, 'utf8'))
    const hasOversizedObject = (manifest.objects || []).some((object) =>
      oversizedObjects.has(resolve(dirname(manifestPath), object.file)))
    if (hasOversizedObject) excludedSceneDirectories.add(dirname(manifestPath))
  }
}

// 排除判定：对文件或目录路径都适用（目录命中则其子树被跳过）。
function excluded(path) {
  if (oversizedObjects.has(path)) return true
  if (extname(path).toLowerCase() === '.obj' && isInside(path, cropModelRoot) && !packagedCropModels.has(path)) return true
  if (path.toLowerCase().endsWith(`${sep}log_nvprosample.txt`)) return true
  for (const directory of generatedOutputDirectories) if (isInside(path, directory)) return true
  for (const directory of sourceDirectories) if (isInside(path, directory)) return true
  for (const directory of excludedSceneDirectories) if (isInside(path, directory)) return true
  return false
}

async function copyTree(source, target) {
  await mkdir(target, { recursive: true })
  for (const entry of await readdir(source, { withFileTypes: true })) {
    const sourcePath = join(source, entry.name)
    if (excluded(sourcePath)) continue
    const targetPath = join(target, entry.name)
    if (entry.isDirectory()) {
      await copyTree(sourcePath, targetPath)
    } else if (entry.isFile()) {
      await mkdir(dirname(targetPath), { recursive: true })
      await copyFile(sourcePath, targetPath)
      const info = await stat(sourcePath)
      copiedFiles += 1
      copiedBytes += info.size
    }
  }
}

// 作用：重写打包副本的 obj-library/catalog.json：删除被排除的模型，并同步清理 defaultSets 中失效的 id。
async function rewriteObjCatalog() {
  const sourceCatalog = join(sourceRoot, 'obj-library', 'catalog.json')
  const targetCatalog = join(targetRoot, 'obj-library', 'catalog.json')
  const catalog = JSON.parse(await readFile(sourceCatalog, 'utf8'))
  catalog.models = (catalog.models || []).filter((model) =>
    !excluded(resolve(dirname(sourceCatalog), model.file)))
  const includedIds = new Set(catalog.models.map((model) => model.id))
  for (const defaults of Object.values(catalog.defaultSets || {})) {
    if (Array.isArray(defaults.typical)) {
      defaults.typical = defaults.typical.filter((id) => includedIds.has(id))
    }
    if (defaults.simple && !includedIds.has(defaults.simple)) delete defaults.simple
  }
  await writeFile(targetCatalog, `${JSON.stringify(catalog, null, 2)}\n`, 'utf8')
}

// 作用：重写打包副本的 rami-library/catalog.json，去掉被排除的场景。
async function rewriteRamiCatalog() {
  const sourceCatalog = join(sourceRoot, 'rami-library', 'catalog.json')
  const targetCatalog = join(targetRoot, 'rami-library', 'catalog.json')
  const catalog = JSON.parse(await readFile(sourceCatalog, 'utf8'))
  catalog.scenes = (catalog.scenes || []).filter((scene) =>
    !excludedSceneDirectories.has(dirname(resolve(dirname(sourceCatalog), scene.manifest))))
  await writeFile(targetCatalog, `${JSON.stringify(catalog, null, 2)}\n`, 'utf8')
}

// 主流程：扫描 → 判定不完整场景 → 清空并复制 → 重写两个目录 JSON。
await scan(sourceRoot)
await findIncompleteRamiScenes()
await rm(targetRoot, { recursive: true, force: true })
await copyTree(sourceRoot, targetRoot)
await rewriteObjCatalog()
await rewriteRamiCatalog()

const excludedModels = [...oversizedObjects.entries()]
  .map(([path, size]) => `${relative(sourceRoot, path)} (${(size / 1024 / 1024).toFixed(2)} MiB)`)
console.log(`Packaged assets: ${copiedFiles} files, ${(copiedBytes / 1024 / 1024).toFixed(2)} MiB`)
console.log(`Packaged crop OBJ models: ${packagedCropModels.size}`)
console.log(`Excluded generated example output directories: ${generatedOutputDirectories.size}`)
console.log(`Excluded source directories: ${sourceDirectories.size}`)
console.log(`Excluded OBJ files over 15 MiB: ${excludedModels.length}`)
for (const model of excludedModels) console.log(`  - ${model}`)
console.log(`Excluded incomplete RAMI scenes: ${excludedSceneDirectories.size}`)
