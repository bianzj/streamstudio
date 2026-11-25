// ==========================================================================
// 文件：compact-obj.mjs
// 功能：生成 OBJ 的紧凑副本，减小体积（说明见下方原有注释）。
// 主要步骤：
//   1. 解析参数：输入、输出、可选 --decimals（位置小数位数，0-100，默认 4）
//   2. 逐行处理：v 行按“位置 + 颜色”拼成键，相同键的顶点合并（颜色保留 3 位小数）；
//      f 行按合并后的新编号重写（支持负索引）；o/g 行原样保留；其余行（vn、vt 等）丢弃
//   3. 写出输出文件并打印顶点数、面数前后变化
// 约定：面只保留顶点编号（去掉 /vt/vn 部分），面顺序不变；顶点颜色取每个 v 行的第 4-6 个字段。
// 注意：合并后会改变顶点共享关系，依赖法线的外部渲染需重算法线（编辑器会重算）。
// ==========================================================================

// Writes a compact copy of an OBJ: drops normals and texture coordinates (the
// engine ignores them and the editor recomputes normals), welds vertices that
// share position and colour, and rounds positions. Groups and face order are kept.
//   node tools/compact-obj.mjs <input.obj> <output.obj> [--decimals 4]
import { readFileSync, writeFileSync } from 'node:fs'
import { resolve } from 'node:path'

const [input, output, ...rest] = process.argv.slice(2)
if (!input || !output) throw new Error('Usage: node tools/compact-obj.mjs <input.obj> <output.obj> [--decimals 4]')
const flag = rest.indexOf('--decimals')
const decimals = flag >= 0 ? Number(rest[flag + 1]) : 4
if (!Number.isInteger(decimals) || decimals < 0 || decimals > 100) throw new Error('--decimals must be an integer from 0 to 100')
// 数字格式化：保留 digits 位小数并去掉多余的 0；-0 统一写成 0（以便相同位置能合并）。
const number = (value, digits) => {
  const fixed = Number(value).toFixed(digits)
  const text = fixed.includes('.') ? fixed.replace(/\.?0+$/, '') : fixed
  return text === '-0' ? '0' : text
}

const out = [`# Compact copy of ${input.replaceAll('\\', '/').split('/').pop()}: normals/UVs dropped, vertices welded, positions rounded to 1e-${decimals}`]
// remap：原始顶点序号 → 合并后序号；welded：位置/颜色键 → 新序号（从 1 开始）。
const remap = [], welded = new Map()
let sourceVertices = 0, faces = 0
for (const line of readFileSync(resolve(input), 'utf8').split(/\r?\n/)) {
  const cells = line.trim().split(/\s+/)
  if (cells[0] === 'v') {
    sourceVertices += 1
    const key = [...cells.slice(1, 4).map((value) => number(value, decimals)), ...cells.slice(4, 7).map((value) => number(value, 3))].join(' ')
    if (!welded.has(key)) { welded.set(key, welded.size + 1); out.push(`v ${key}`) }
    remap.push(welded.get(key))
  } else if (cells[0] === 'f') {
    const ids = cells.slice(1).map((token) => {
      const raw = Number(token.split('/')[0])
      return remap[raw > 0 ? raw - 1 : remap.length + raw]
    })
    out.push(`f ${ids.join(' ')}`)
    faces += 1
  } else if (cells[0] === 'o' || cells[0] === 'g') {
    out.push(line.trim())
  }
}
writeFileSync(resolve(output), `${out.join('\n')}\n`, 'utf8')
console.log(`${input} -> ${output}: vertices ${sourceVertices} -> ${welded.size}, faces ${faces}`)
