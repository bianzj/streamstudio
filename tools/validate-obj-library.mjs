// ==========================================================================
// 文件：validate-obj-library.mjs
// 功能：校验 assets/obj-library 的 OBJ 资产目录 catalog.json 与实际 OBJ 文件是否一致（命令行脚本，
//   有失败则逐行打印到 stderr 并以退出码 1 结束，通过则打印模型数与推荐数）。
// 主要步骤：
//   1. 逐个模型检查：文件存在；顶点（v 行）与三角形数都不为 0；
//      OBJ 实际三角形数（每个 f 行按 n 边形计 n-2 个三角形）与 catalog 中的 triangles 一致
//   2. 检查 defaultSets：每个类别引用的 typical / simple 模型都存在且 recommended 为真
// 关系：目录文件为 assets/obj-library/catalog.json；不依赖其他脚本。运行：node tools/validate-obj-library.mjs
// ==========================================================================

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const library = path.join(root, 'assets', 'obj-library')
const catalog = JSON.parse(fs.readFileSync(path.join(library, 'catalog.json'), 'utf8'))
const failures = []

// 步骤 1：逐模型检查文件存在性、几何非空以及三角形数与目录一致。
for (const model of catalog.models) {
  const file = path.join(library, model.file)
  if (!fs.existsSync(file)) {
    failures.push(`${model.id}: file not found: ${model.file}`)
    continue
  }
  let vertices = 0
  let triangles = 0
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    if (line.startsWith('v ')) vertices += 1
    if (line.startsWith('f ')) triangles += Math.max(0, line.trim().split(/\s+/).length - 3)
  }
  if (vertices === 0 || triangles === 0) failures.push(`${model.id}: empty geometry`)
  if (triangles !== model.triangles) failures.push(`${model.id}: catalog=${model.triangles}, OBJ=${triangles} triangles`)
}

// 步骤 2：检查各类别缺省模型集合引用的 id 都存在于目录且被标记为推荐。
for (const [category, defaults] of Object.entries(catalog.defaultSets || {})) {
  const ids = [...(defaults.typical || []), defaults.simple].filter(Boolean)
  for (const id of ids) {
    const model = catalog.models.find((item) => item.id === id)
    if (!model) failures.push(`${category}: unknown default model ${id}`)
    else if (!model.recommended) failures.push(`${category}: default model ${id} is not recommended`)
  }
}

if (failures.length) {
  console.error(failures.join('\n'))
  process.exit(1)
}

console.log(`OBJ asset catalog valid: ${catalog.models.length} models, ${catalog.models.filter((model) => model.recommended).length} recommended`)
