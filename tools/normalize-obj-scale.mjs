import { readFile, writeFile } from 'node:fs/promises'

const scale = Number(process.argv[2])
const paths = process.argv.slice(3)
if (!(scale > 0) || paths.length === 0) {
  throw new Error('Usage: node tools/normalize-obj-scale.mjs <scale> <obj...>')
}

for (const path of paths) {
  const text = await readFile(path, 'utf8')
  const lines = text.split(/\r?\n/)
  const vertices = lines.map((line) => line.match(/^\s*v\s+([-+\d.eE]+)\s+([-+\d.eE]+)\s+([-+\d.eE]+)/)).filter(Boolean)
  const minimumY = Math.min(...vertices.map((match) => Number(match[2])))
  const output = lines.map((line) => {
    const match = line.match(/^(\s*v\s+)([-+\d.eE]+)\s+([-+\d.eE]+)\s+([-+\d.eE]+)(.*)$/)
    if (!match) return line
    const [, prefix, x, y, z, rest] = match
    return `${prefix}${Number(x) * scale} ${(Number(y) - minimumY) * scale} ${Number(z) * scale}${rest}`
  }).join('\n')
  await writeFile(path, output, 'utf8')
  console.log(path)
}
