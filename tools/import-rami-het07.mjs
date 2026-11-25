// ==========================================================================
// 文件：import-rami-het07.mjs
// 功能：把解压后的 RAMI-V HET07_JPS_SUM（Järvselja 夏季松树林）基准场景导入为仓库内资源库
//       assets/rami-library/HET07_JPS_SUM，用法：node tools/import-rami-het07.mjs <解压目录>
// 主要步骤：
//   1. 定义 11 个树原型（1 个桦树 BEPE + 10 个欧洲赤松 PISY1–PISY10），记录源 OBJ 路径
//   2. convertObjToYUp：把 OBJ 的 Z 向上改为 Y 向上，并丢弃 mtllib/usemtl 行
//   3. 读 HET07_JPS_SUM_field.txt，按原型 id 分组，输出每个原型的位置文件
//   4. 拷贝 scene / spectral / illumination 元数据，写 manifest.json
// 数据流：<解压目录>/tree-species 等 → 本脚本 → assets/rami-library/HET07_JPS_SUM/*
// 约定：输出 OBJ 为 Y-up（原 (x,y,z) → (x,z,-y)）；位置文件每行为 x y z 缩放 旋转角；
//       缩放取源文件三轴缩放的平均值。路径相对当前工作目录（应在仓库根目录运行）。
// ==========================================================================

import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises'
import { join, resolve } from 'node:path'

const sourceRoot = resolve(process.argv[2] || '')
if (!process.argv[2]) throw new Error('Usage: node tools/import-rami-het07.mjs <extracted HET07_JPS_SUM directory>')

const destinationRoot = resolve('assets', 'rami-library', 'HET07_JPS_SUM')
const prototypeRoot = join(destinationRoot, 'prototypes')
const positionRoot = join(destinationRoot, 'positions')
await mkdir(prototypeRoot, { recursive: true })
await mkdir(positionRoot, { recursive: true })

// 步骤 1：树原型表。id 与场地文件第 1 列对应；PISY 编号顺序 1,10,2..9 是按字符串排序的目录顺序（待核对）。
const prototypes = [
  { id: 0, code: 'BEPE', source: 'tree-species/Betula-Pendula/BEPE/Betula_pendula_BEPE_-_Simplified_twig.obj' },
  ...[1, 10, 2, 3, 4, 5, 6, 7, 8, 9].map((number, index) => ({
    id: index + 1,
    code: `PISY${number}`,
    source: `tree-species/Pinus-Sylvestris/PISY${number}/Pinus_sylvestris_PISY${number}_-_Simplified_leaf.obj`
  }))
]

// 步骤 2：顶点/法线 (x, y, z) → (x, z, -y)，把 Z-up 转为 Y-up；材质库引用行被移除。
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

// 步骤 3：解析场地文件。每行至少 10 个数：id、x、y、z、三轴缩放，以及第 8、9 列（含义待核对）和旋转角。
// 缩放取三轴平均；非数值行（表头/空行）与未知 id 被跳过。
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

// 步骤 4：保存 RAMI 原始场景/光谱/光照说明作为元数据，供溯源。
await mkdir(join(destinationRoot, 'metadata'), { recursive: true })
await copyFile(join(sourceRoot, 'scene', 'scene.txt'), join(destinationRoot, 'metadata', 'scene.txt'))
await copyFile(join(sourceRoot, 'spectral-characteristics', 'spectral.txt'), join(destinationRoot, 'metadata', 'spectral.txt'))
await copyFile(join(sourceRoot, 'illumination-characteristics', 'illumination.txt'), join(destinationRoot, 'metadata', 'illumination.txt'))

// 清单：sceneDimensionsMetres 为场景 X、Y、Z 尺寸（m），objects 列出各原型的 OBJ、位置文件与实例数。
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
