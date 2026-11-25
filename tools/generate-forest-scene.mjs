// ==========================================================================
// 文件：generate-forest-scene.mjs
// 功能：生成确定性的 100 m × 100 m 森林演示整景 OBJ（assets/obj-library/large_scene/
//       forest_demo_100m.obj）：96 棵树 + 160 丛林下植被，合并为一个带 o 分组的 OBJ。
// 主要步骤：
//   1. 定义树种清单 treeSpecs（模型文件、缩放范围）与固定种子的线性同余随机数 random()
//   2. loadObj 读取库内 OBJ 的顶点与面；addInstance 把一个实例（缩放、绕 Y 旋转、平移）
//      追加到输出，并累计顶点偏移 vertexOffset 以重编面索引
//   3. 随机放置 96 棵树（位置在 [2, 98) m，树种与缩放随机）
//   4. 随机放置 160 丛林下植被（位置在 [0, 100) m，缩放 1.2~4.0，抬高 0.02 m）
//   5. 写出 OBJ 并打印路径
// 约定：Y 向上，单位 m；旋转角为弧度；种子固定，重复运行结果一致。
// 注意：loadObj 把负的面索引按 OBJ 规则换算成正索引（正索引输出不变）；
//   实例互不避让，可能重叠。
// ==========================================================================

import { readFile, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const projectRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const libraryRoot = join(projectRoot, 'assets', 'obj-library')
const outputPath = join(libraryRoot, 'large_scene', 'forest_demo_100m.obj')

// 树种表：[相对库的模型文件, 缩放下限, 缩放上限]。森林树 forest_tree 的缩放范围明显更大
// （3.0~4.4），推测其原始模型较小（待核对）。
const treeSpecs = [
  ['vegetation/tree_oak.obj', 0.85, 1.2],
  ['vegetation/tree_pine.obj', 0.75, 1.15],
  ['vegetation/tree_broadleaf.obj', 0.9, 1.3],
  ['vegetation/forest_tree.obj', 3.0, 4.4]
]

// 线性同余随机数（a=1664525, c=1013904223, 模 2^32），返回 [0,1)；固定种子保证可复现。
let randomState = 0x5eed1234
const random = () => {
  randomState = (1664525 * randomState + 1013904223) >>> 0
  return randomState / 0x100000000
}

// 作用：读取库内 OBJ，返回 { vertices, faces }；面只保留顶点编号（去掉 /vt/vn），1 起始。
// 负索引按 OBJ 规则相对“当前已读顶点数”换算成正索引（-1 = 最近读到的顶点）；正索引原样保留。
const loadObj = async (relativePath) => {
  const text = await readFile(join(libraryRoot, relativePath), 'utf8')
  const vertices = []
  const faces = []
  for (const line of text.split(/\r?\n/)) {
    const parts = line.trim().split(/\s+/)
    if (parts[0] === 'v' && parts.length >= 4) {
      vertices.push(parts.slice(1, 4).map(Number))
    } else if (parts[0] === 'f' && parts.length >= 4) {
      faces.push(parts.slice(1).map((token) => {
        const index = Number(token.split('/')[0])
        return index < 0 ? vertices.length + index + 1 : index
      }))
    }
  }
  return { vertices, faces }
}

const lines = ['# Stream Studio deterministic 100 m x 100 m CC0 forest demonstration']
let vertexOffset = 0
// 作用：把模型 source 作为实例 name 追加到 lines：先按 scale 缩放，再绕 Y 轴旋转 rotation（弧度），
// 最后平移 (x, y, z)；面索引加上当前累计顶点数，使多个实例共存于同一个 OBJ。
const addInstance = (name, source, x, y, z, rotation, scale) => {
  const cos = Math.cos(rotation)
  const sin = Math.sin(rotation)
  lines.push(`o ${name}`)
  for (const [vx, vy, vz] of source.vertices) {
    const sx = vx * scale
    const sz = vz * scale
    const tx = sx * cos - sz * sin + x
    const tz = sx * sin + sz * cos + z
    lines.push(`v ${tx.toFixed(6)} ${(vy * scale + y).toFixed(6)} ${tz.toFixed(6)}`)
  }
  for (const face of source.faces) {
    lines.push(`f ${face.map((index) => index + vertexOffset).join(' ')}`)
  }
  vertexOffset += source.vertices.length
}

// 步骤 3：96 棵树：随机选树种、在其缩放范围内随机缩放，位置离边界留 2 m，随机朝向。
const treeSources = await Promise.all(treeSpecs.map(([file]) => loadObj(file)))
for (let index = 0; index < 96; index += 1) {
  const sourceIndex = Math.floor(random() * treeSources.length)
  const [, minScale, maxScale] = treeSpecs[sourceIndex]
  const scale = minScale + random() * (maxScale - minScale)
  addInstance(
    `tree_${String(index + 1).padStart(3, '0')}`,
    treeSources[sourceIndex],
    2 + random() * 96,
    0,
    2 + random() * 96,
    random() * Math.PI * 2,
    scale
  )
}

// 步骤 4：160 丛林下植被：位置覆盖整个 100 m 场景，抬高 0.02 m（避免与地面共面）。
const understorySource = await loadObj('vegetation/forest_understory.obj')
for (let index = 0; index < 160; index += 1) {
  const scale = 1.2 + random() * 2.8
  addInstance(
    `understory_${String(index + 1).padStart(3, '0')}`,
    understorySource,
    random() * 100,
    0.02,
    random() * 100,
    random() * Math.PI * 2,
    scale
  )
}

await writeFile(outputPath, `${lines.join('\n')}\n`, 'utf8')
console.log(outputPath)
