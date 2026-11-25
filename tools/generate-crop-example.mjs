// ==========================================================================
// 文件：generate-crop-example.mjs
// 功能：生成内置示例 assets/examples/crop_gpp_npp（20 m × 20 m 玉米田，VoxelEB，GPP/NPP 演示）。
// 主要步骤：
//   1. 生成 12×12 = 144 株的位置文件 positions/maize_field_position.txt（株距 1.2 m，
//      带确定性的缩放与旋转扰动）
//   2. 从内置 meteo.txt 取前 48 个半小时节点写 meteorology/meteo_crop_48.txt
//   3. 用 createDefaultProject 创建 eVoxelEB 工程，覆盖场景/光照/传感器/控制/气象参数
//   4. 添加玉米冠层（canopy）与玉米 OBJ 对象（网格分布 12×12）
//   5. 写出 project.json 与 README.md
// 输出：assets/examples/crop_gpp_npp/{project.json,README.md,positions/,meteorology/}。
// 约定：长度 m；时间步长 dTime = 1800 s；位置文件每行为 北向 x、东向 z、高度 y、缩放、旋转角（度），
//   详见下方位置文件处的原有注释；project.json 中的相对路径以示例目录为基准。
// 与其他文件：依赖 src/renderer/src/project-schema.js；需要已构建的 models/bin_x64/Release/defined/
//   meteo.txt；校验用的 validated-carbon-summary.json 由别处生成，被 tools/build-example-manual-pdf.py 使用。
// ==========================================================================

import { mkdir, readFile, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

import { createDefaultProject, stringifyProject } from '../src/renderer/src/project-schema.js'

const projectRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const exampleRoot = join(projectRoot, 'assets', 'examples', 'crop_gpp_npp')
const positionsRoot = join(exampleRoot, 'positions')
const meteorologyRoot = join(exampleRoot, 'meteorology')

await mkdir(positionsRoot, { recursive: true })
await mkdir(meteorologyRoot, { recursive: true })

// 步骤 1：12×12 株规则网格，株距 1.2 m，起点 3.4 m（整片田左右各留 3.4 m，居中于 20 m 场景）；
// 缩放 0.94~1.06（按行列取模的确定性扰动）、旋转 0~359°，保证每次生成结果相同。
const rows = 12
const columns = 12
const spacing = 1.2
const origin = 3.4
const positions = []
for (let row = 0; row < rows; row += 1) {
  for (let column = 0; column < columns; column += 1) {
    const x = origin + column * spacing
    const z = origin + row * spacing
    const scale = 0.94 + ((row * 13 + column * 7) % 9) * 0.015
    const rotation = (row * 17 + column * 29) % 360
    // Position files use X north, Z east, Y height, then scale and rotation.
    positions.push(`${x.toFixed(3)} ${z.toFixed(3)} 0 ${scale.toFixed(3)} ${rotation}`)
  }
}
await writeFile(join(positionsRoot, 'maize_field_position.txt'), `${positions.join('\n')}\n`, 'utf8')

// 步骤 2：气象取内置文件第 2~49 行（跳过首行节点数），共 48 个半小时节点；
// 写出时首行改为“48”。
const sourceMeteo = (await readFile(
  join(projectRoot, 'models', 'bin_x64', 'Release', 'defined', 'meteo.txt'), 'utf8'
)).replace(/^\uFEFF/, '').split(/\r?\n/).filter((line) => line.trim())
const meteoNodes = sourceMeteo.slice(1, 49)
if (meteoNodes.length !== 48) throw new Error('Built-in meteorology does not contain 48 data nodes')
await writeFile(
  join(meteorologyRoot, 'meteo_crop_48.txt'),
  `48  \n${meteoNodes.join('\n')}\n`,
  'utf8'
)

// 步骤 3：工程基础参数。固定 createdAt/updatedAt 使重复生成的文件内容不变。
// 场景 20×20×4 m、体元 0.5 m；背景为土壤。
const project = createDefaultProject({
  name: 'Crop GPP-NPP - 20 m Maize Field',
  mode: 'eVoxelEB',
  sceneX: 20,
  sceneY: 20,
  sceneHeight: 4,
  voxelSize: 0.5
})
project.createdAt = '2026-09-09T00:00:00.000Z'
project.updatedAt = project.createdAt

const configuration = project.configuration
configuration.outDir = 'output'
configuration.scene.voxelFillThreshold = 0.05
configuration.scene.background = {
  spectralName: 'soil',
  thermalName: 'soil_temperature',
  materialType: 'Soil',
  materialName: 'soilset',
  heterogeneityEnabled: false,
  angularEffectStrength: 0.5
}
// 太阳天顶角 30°、方位角 135°；直射/散射比 0.8/0.2；天空等效温度 250 K。
// （该固定光照与逐时气象的关系待核对。）
configuration.light = {
  zenith: 30,
  azimuth: 135,
  direct: 0.8,
  diffuse: 0.2,
  skyTemperature: 250
}
// 传感器：256×256 平行投影，天顶观测（vza=0），波段 475/560/668/717/842 nm 与 10500 nm（热红外）。
configuration.sensor = {
  ...configuration.sensor,
  x: 256,
  y: 256,
  bands: '475 560 668 717 842 10500',
  projection: 'parallel',
  vza: 0,
  vaa: 0,
  positionX: 10,
  positionY: 10,
  height: 500,
  image: true,
  radiationProcess: true,
  energyProcess: true,
  temperature: true,
  albedo: false
}
// 求解控制：16 样本、深度 4；能量平衡与辐射耦合迭代最多 12 次，
// 温度容差 0.05（单位 K，待核对）、松弛系数 0.5。
configuration.control = {
  ...configuration.control,
  depth: 4,
  samples: 16,
  couplingIterations: 12,
  temperatureTolerance: 0.05,
  temperatureRelaxation: 0.5,
  soilTemperatureMethod: 2,
  vegetationTemperatureMethod: 0
}
// 气象驱动：使用第 1~48 个节点；北京附近（纬度 40°、经度 116°，仅示例取值）；
// dTime = 1800 s；Tsold = 298.15 K 为土壤初始温度，SatWater = 0.45 为土壤饱和度/含水量（待核对）。
configuration.meteo = {
  ...configuration.meteo,
  path: 'meteorology/meteo_crop_48.txt',
  start: 1,
  end: 48,
  z: 4,
  Tsold: 298.15,
  SatWater: 0.45,
  dTime: 1800,
  latitude: 40,
  longitude: 116
}

// 玉米冠层结构参数：LAI 3.5、株高 hc = 2 m、LIDFa/LIDFb 为叶倾角分布参数、
// hspot 为热点参数、leafwidth 为叶宽（m）。其余字段含义待核对。
configuration.canopies.push({
  name: 'maize_crop_canopy',
  label: '玉米冠层',
  structureType: 'canopy',
  lai: 3.5,
  density: 2.2,
  hc: 2,
  G: 0.5,
  LIDFa: -0.2,
  LIDFb: -0.1,
  hspot: 0.12,
  leafwidth: 0.08
})

// 步骤 4：玉米对象。使用 C4 叶片材质 leaf_c4；distribution 与步骤 1 的位置文件参数一致
// （网格 12×12，株距 1.2 m），scaleMin/Max 记录缩放范围，seed 为随机种子（网格模式下是否使用待核对）。
const maizeObject = {
  name: 'Maize field - mature C4 crop',
  type: 'Vegetation',
  materialName: 'leaf_c4',
  canopyName: 'maize_crop_canopy',
  fileName: 'assets/obj-library/crop/maize-growth/maize_stage_04.obj',
  positionFile: 'positions/maize_field_position.txt',
  shape: 'cube',
  dimensions: [0.887, 1.967, 0.649],
  meshes: [{
    name: 'Corn_4_Cube.013',
    spectralName: 'maize_leaf',
    thermalName: 'vegetation_temperature',
    materialName: 'leaf_c4',
    canopyName: 'maize_crop_canopy'
  }],
  distribution: {
    mode: 'grid',
    basis: 'count',
    count: rows * columns,
    rows,
    columns,
    spacingX: spacing,
    spacingY: spacing,
    minX: origin,
    maxX: origin + (columns - 1) * spacing,
    minY: origin,
    maxY: origin + (rows - 1) * spacing,
    z: 0,
    scaleMin: 0.94,
    scaleMax: 1.06,
    seed: 20260909
  },
  instanceCount: rows * columns
}
configuration.objects = {
  count: 1,
  names: [maizeObject.name],
  items: [maizeObject]
}

// 步骤 5：写工程文件与 README。
await writeFile(join(exampleRoot, 'project.json'), stringifyProject(project), 'utf8')
await writeFile(join(exampleRoot, 'README.md'), `# Crop GPP/NPP example

Open \`project.json\` in Stream Studio and run the default VoxelEB configuration.

- Domain: 20 m x 20 m x 4 m
- Crop: 144 mature maize plants, C4 physiology
- Voxel: 0.5 m
- Meteorology: 48 half-hour nodes
- Outputs: images, radiation process, energy process, GPP, and NPP

GPP is gross leaf photosynthesis. NPP is the current leaf net-assimilation
approximation (GPP minus dark respiration); stem and root respiration are not
yet included.
`, 'utf8')

console.log(`Generated crop example: ${exampleRoot}`)
console.log(`Maize instances: ${positions.length}`)
console.log(`Meteorology nodes: ${meteoNodes.length}`)
