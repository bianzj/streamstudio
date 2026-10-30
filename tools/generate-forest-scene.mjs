import { readFile, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const projectRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const libraryRoot = join(projectRoot, 'assets', 'obj-library')
const outputPath = join(libraryRoot, 'large_scene', 'forest_demo_100m.obj')

const treeSpecs = [
  ['vegetation/tree_oak.obj', 0.85, 1.2],
  ['vegetation/tree_pine.obj', 0.75, 1.15],
  ['vegetation/tree_broadleaf.obj', 0.9, 1.3],
  ['vegetation/forest_tree.obj', 3.0, 4.4]
]

let randomState = 0x5eed1234
const random = () => {
  randomState = (1664525 * randomState + 1013904223) >>> 0
  return randomState / 0x100000000
}

const loadObj = async (relativePath) => {
  const text = await readFile(join(libraryRoot, relativePath), 'utf8')
  const vertices = []
  const faces = []
  for (const line of text.split(/\r?\n/)) {
    const parts = line.trim().split(/\s+/)
    if (parts[0] === 'v' && parts.length >= 4) {
      vertices.push(parts.slice(1, 4).map(Number))
    } else if (parts[0] === 'f' && parts.length >= 4) {
      faces.push(parts.slice(1).map((token) => Number(token.split('/')[0])))
    }
  }
  return { vertices, faces }
}

const lines = ['# Stream Studio deterministic 100 m x 100 m CC0 forest demonstration']
let vertexOffset = 0
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
