// node tools/generate-tarp-cover.mjs <target.obj> <output.obj> [--cell m] [--lift m] [--drape slope] [--margin m]
import { readFileSync, writeFileSync } from 'node:fs'
import { resolve } from 'node:path'

import { generateTarpCover } from './tarp-cover.mjs'

const [target, output, ...rest] = process.argv.slice(2)
if (!target || !output) throw new Error('Usage: node tools/generate-tarp-cover.mjs <target.obj> <output.obj> [--cell m] [--lift m] [--drape slope] [--margin m]')
const options = {}
for (const name of ['cell', 'lift', 'drape', 'margin']) {
  const i = rest.indexOf(`--${name}`)
  if (i >= 0) options[name] = Number(rest[i + 1])
}
const cover = generateTarpCover(readFileSync(resolve(target), 'utf8'), options)
writeFileSync(resolve(output), cover.obj, 'utf8')
console.log(`Target ${cover.targetSize.join(' x ')} m -> cover ${cover.dimensions.join(' x ')} m, ${cover.vertices} vertices, ${cover.triangles} triangles, cell ${cover.cell.toFixed(3)} m`)
