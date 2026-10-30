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
configuration.light = {
  zenith: 30,
  azimuth: 135,
  direct: 0.8,
  diffuse: 0.2,
  skyTemperature: 250
}
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
