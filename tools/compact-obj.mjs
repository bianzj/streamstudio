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
const number = (value, digits) => {
  const fixed = Number(value).toFixed(digits)
  const text = fixed.includes('.') ? fixed.replace(/\.?0+$/, '') : fixed
  return text === '-0' ? '0' : text
}

const out = [`# Compact copy of ${input.replaceAll('\\', '/').split('/').pop()}: normals/UVs dropped, vertices welded, positions rounded to 1e-${decimals}`]
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
