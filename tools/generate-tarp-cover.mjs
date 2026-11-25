// ==========================================================================
// 文件：generate-tarp-cover.mjs
// 功能：命令行包装：读取目标物体的 OBJ，生成一块覆盖在其上的篷布/伪装网 OBJ（Y 向上，单位 m）。
// 用法：见下方原有的用法注释。可选参数均为数值：
//   --cell 网格间距（缺省按目标最大水平尺寸/80 自动取 0.05~0.5 m）、--lift 离目标表面的抬升
//   （缺省 0.05 m）、--drape 裙边每水平 1 m 下垂高度（缺省 6）、--margin 足迹外水平余量（缺省 0.15 m）。
// 数据流：<target.obj> → generateTarpCover(文本, options) → <output.obj>，并打印目标/覆盖层尺寸、
//   顶点数、三角形数与网格间距。
// 与其他文件：核心算法在 tools/tarp-cover.mjs（也被 server.mjs 的篷布覆盖接口调用）。
// ==========================================================================

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
