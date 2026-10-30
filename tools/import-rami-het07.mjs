import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises'
import { join, resolve } from 'node:path'

const sourceRoot = resolve(process.argv[2] || '')
if (!process.argv[2]) throw new Error('Usage: node tools/import-rami-het07.mjs <extracted HET07_JPS_SUM directory>')

const destinationRoot = resolve('assets', 'rami-library', 'HET07_JPS_SUM')
const prototypeRoot = join(destinationRoot, 'prototypes')
const positionRoot = join(destinationRoot, 'positions')
await mkdir(prototypeRoot, { recursive: true })
await mkdir(positionRoot, { recursive: true })

const prototypes = [
  { id: 0, code: 'BEPE', source: 'tree-species/Betula-Pendula/BEPE/Betula_pendula_BEPE_-_Simplified_twig.obj' },
  ...[1, 10, 2, 3, 4, 5, 6, 7, 8, 9].map((number, index) => ({
    id: index + 1,
    code: `PISY${number}`,
    source: `tree-species/Pinus-Sylvestris/PISY${number}/Pinus_sylvestris_PISY${number}_-_Simplified_leaf.obj`
  }))
]

const convertObjToYUp = (text) => text.split(/\r?\n/).flatMap((line) => {
  const match = line.match(/^\s*(v|vn)\s+([-+\d.eE]+)\s+([-+\d.eE]+)\s+([-+\d.eE]+)(.*)$/)
  if (!match) return /^\s*(?:mtllib|usemtl)\b/i.test(line) ? [] : [line]
  const [, type, x, y, z, rest] = match
  return [`${type} ${x} ${z} ${-Number(y)}${rest}`]
}).join('\n')

for (const prototype of prototypes) {
  const input = await readFile(join(sourceRoot, prototype.source), 'utf8')
  prototype.file = `prototypes/${prototype.code}.obj`
  await writeFile(join(destinationRoot, prototype.file), `${convertObjToYUp(input)}\n`, 'utf8')
}

const placements = Array.from({ length: prototypes.length }, () => [])
const fieldText = await readFile(join(sourceRoot, 'tree-species', 'HET07_JPS_SUM_field.txt'), 'utf8')
for (const line of fieldText.split(/\r?\n/)) {
  const values = line.trim().split(/\s+/).map(Number)
  if (values.length < 10 || !values.every(Number.isFinite)) continue
  const [id, x, y, z, sx, sy, sz, , , rotation] = values
  if (!placements[id]) continue
  const scale = (sx + sy + sz) / 3
  placements[id].push(`${x} ${y} ${z} ${scale} ${rotation}`)
}

for (const prototype of prototypes) {
  prototype.positionFile = `positions/${prototype.code}_position.txt`
  prototype.instances = placements[prototype.id].length
  await writeFile(join(destinationRoot, prototype.positionFile), `${placements[prototype.id].join('\n')}\n`, 'utf8')
}

await mkdir(join(destinationRoot, 'metadata'), { recursive: true })
await copyFile(join(sourceRoot, 'scene', 'scene.txt'), join(destinationRoot, 'metadata', 'scene.txt'))
await copyFile(join(sourceRoot, 'spectral-characteristics', 'spectral.txt'), join(destinationRoot, 'metadata', 'spectral.txt'))
await copyFile(join(sourceRoot, 'illumination-characteristics', 'illumination.txt'), join(destinationRoot, 'metadata', 'illumination.txt'))

const manifest = {
  id: 'RAMI5_HET07_JPS_SUM',
  label: 'RAMI-V Järvselja Pine Stand (Summer)',
  coordinateSystem: 'Y-up OBJ; position files use X/Y horizontal and Z vertical',
  sceneDimensionsMetres: [105.932, 106.118, 18.56],
  totalInstances: prototypes.reduce((sum, item) => sum + item.instances, 0),
  source: 'https://rami-benchmark.jrc.ec.europa.eu/',
  dartObjSource: 'https://dart.omp.eu/index.php#/doc',
  license: 'RAMI-V reference data; source acknowledgement required',
  objects: prototypes.map(({ id, code, file, positionFile, instances }) => ({ id, code, file, positionFile, instances }))
}
await writeFile(join(destinationRoot, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`, 'utf8')
console.log(`${manifest.label}: ${manifest.totalInstances} instances`)
