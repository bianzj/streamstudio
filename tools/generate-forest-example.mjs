// Builds examples on the realistic 100 m forest (20 jacaranda trees + 200 searsia
// shrubs, layout in assets/obj-library/large_scene/realistic_forest_100m/).
//   node tools/generate-forest-example.mjs [output directory]             VoxelEB: temperature, GPP, NPP
//   node tools/generate-forest-example.mjs --vehicles [output directory]  RayTracing: vehicle lineup in forest openings
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

import { createDefaultProject, stringifyProject, validateProject } from '../src/renderer/src/project-schema.js'
import { addVehicleLineup, CANOPY_FOOTPRINT } from './vehicle-lineup.mjs'

const projectRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const args = process.argv.slice(2)
const withVehicles = args.includes('--vehicles')
const outputArg = args.find((arg) => !arg.startsWith('--'))
const exampleRoot = resolve(outputArg ?? (withVehicles ? 'C:/cases/vehicle-tarp/forest' : join(projectRoot, 'assets', 'examples', 'forest')))
for (const folder of ['positions', 'meteorology']) mkdirSync(join(exampleRoot, folder), { recursive: true })

const SCENE = 100
const LAYOUT = 'assets/obj-library/large_scene/realistic_forest_100m'
const mesh = (name, spectralName, thermalName, materialName, canopyName) => ({ name, spectralName, thermalName, materialName, canopyName })
// Shared library models (not copied): the server resolves assets/ paths against Stream Studio's resources.
// The jacaranda uses its compact copy (tools/compact-obj.mjs) so it fits the 15 MiB packaging limit.
const SPECIES = [
  {
    name: 'jacaranda', label: '蓝花楹（真实模型）', layout: 'jacaranda_position.txt', file: 'assets/obj-library/vegetation/realistic/jacaranda_tree_realistic_compact.obj',
    meshes: [
      mesh('part_1__jacaranda_tree_branches', 'tree_wood', 'tree_wood_temperature', 'tree_branch_wood', 'tree_branch_rigid'),
      mesh('part_2__jacaranda_tree_trunk', 'tree_wood', 'tree_wood_temperature', 'tree_trunk_wood', 'tree_trunk_rigid'),
      mesh('part_3__jacaranda_tree_leaves', 'broadleaf_tree_leaf', 'vegetation_temperature', 'leaf_c3', 'tree_leaf_canopy')
    ]
  },
  {
    name: 'searsia', label: '漆树灌木 Searsia（真实模型）', layout: 'searsia_position.txt', file: 'assets/obj-library/vegetation/realistic/searsia_tree_realistic.obj',
    meshes: [
      mesh('part_1__searsia_lucida', 'green_leaf', 'vegetation_temperature', 'leaf_c3', 'tree_leaf_canopy'),
      mesh('part_2__searsia_lucida_leaves', 'green_leaf', 'vegetation_temperature', 'leaf_c3', 'tree_leaf_canopy'),
      mesh('part_3__searsia_lucida_twigs', 'tree_wood', 'tree_wood_temperature', 'tree_branch_wood', 'tree_branch_rigid')
    ]
  }
]

const parseObj = (path) => {
  const vertices = [], faces = []
  for (const line of readFileSync(join(projectRoot, path), 'utf8').split(/\r?\n/)) {
    if (line.startsWith('v ')) vertices.push(line.split(/\s+/).slice(1, 4).map(Number))
    else if (line.startsWith('f ')) faces.push(line.trim().split(/\s+/).slice(1).map((token) => Number(token.split('/')[0])))
  }
  return { vertices, faces }
}

const meteo = readFileSync(join(projectRoot, 'models', 'bin_x64', 'Release', 'defined', 'meteo.txt'), 'utf8')
  .replace(/^\uFEFF/, '').split(/\r?\n/).filter((line) => line.trim())
writeFileSync(join(exampleRoot, 'meteorology', 'meteo_doy214_48.txt'), `48\n${meteo.slice(1, 49).join('\n')}\n`, 'utf8')

// Jacaranda crowns reach 19.3 m x 1.14 = 22 m.
const project = withVehicles
  ? createDefaultProject({ name: 'Realistic 100 m forest - bare, shade-canopy and tarp-covered vehicles', mode: 'eRaytracing', sceneX: SCENE, sceneY: SCENE, sceneHeight: 23, voxelSize: 0.25 })
  : createDefaultProject({ name: 'Realistic 100 m forest - GPP/NPP', mode: 'eVoxelEB', sceneX: SCENE, sceneY: SCENE, sceneHeight: 23, voxelSize: 0.5 })
project.createdAt = '2026-10-18T00:00:00.000Z'
project.updatedAt = project.createdAt
const c = project.configuration
c.outDir = 'output'
c.scene.voxelFillThreshold = 0.05
Object.assign(c.scene.background, { spectralName: 'soil', thermalName: 'soil_temperature', materialType: 'Soil', materialName: 'soilset' })
Object.assign(c.light, { zenith: 30, azimuth: 135, direct: 0.8, diffuse: 0.2, skyTemperature: 250 })
Object.assign(c.meteo, { path: 'meteorology/meteo_doy214_48.txt', start: 1, end: 48, z: 25, Tsold: 298.15, SatWater: 0.45, dTime: 1800, latitude: 40, longitude: 116 })
if (withVehicles) {
  // 100 continuous bands, 400-2380 nm at 20 nm; 0.125 m pixels. Depth 16 keeps NIR multiple scattering in the crowns.
  Object.assign(c.sensor, {
    x: 800, y: 800, bands: '', projection: 'parallel', vza: 0, vaa: 0,
    continuousBands: true, bandStart: 400, bandEnd: 2380, bandStep: 20,
    positionX: SCENE / 2, positionY: SCENE / 2, height: 500,
    image: true, temperature: false, radiationProcess: false, energyProcess: false, albedo: false
  })
  Object.assign(c.control, { depth: 16, samples: 64 })
} else {
  Object.assign(c.sensor, {
    x: 400, y: 400, bands: '475 560 668 717 842 10500', projection: 'parallel', vza: 0, vaa: 0,
    positionX: SCENE / 2, positionY: SCENE / 2, height: 500,
    image: true, radiationProcess: true, energyProcess: true, temperature: true, albedo: false
  })
  Object.assign(c.control, { depth: 4, samples: 16, couplingIterations: 12, temperatureTolerance: 0.05, temperatureRelaxation: 0.5, soilTemperatureMethod: 2, vegetationTemperatureMethod: 0 })
}

const items = SPECIES.map((species) => {
  const rows = readFileSync(join(projectRoot, LAYOUT, species.layout), 'utf8').trim().split(/\r?\n/)
  const positionFile = `positions/${species.name}_position.txt`
  writeFileSync(join(exampleRoot, positionFile), `${rows.join('\n')}\n`, 'utf8')
  const json = JSON.parse(readFileSync(join(projectRoot, species.file.replace(/\.obj$/, '.json')), 'utf8'))
  const scales = rows.map((row) => Number(row.split(/\s+/)[3]))
  species.placements = rows.map((row) => row.split(/\s+/).map(Number))
  return {
    name: species.name, label: species.label, type: 'Vegetation', materialName: 'leaf_c3', canopyName: 'tree_leaf_canopy',
    fileName: species.file, positionFile, shape: 'cube', dimensions: json.dimensions, meshes: species.meshes,
    distribution: { mode: 'manual', basis: 'count', count: rows.length, minX: 0, maxX: SCENE, minY: 0, maxY: SCENE, z: 0, scaleMin: Math.min(...scales), scaleMax: Math.max(...scales), seed: 1, offsetX: 0, offsetY: 0, offsetZ: 0, points: rows.join('\n') },
    instanceCount: rows.length
  }
})

const spots = []
if (withVehicles) {
  // Openings in the forest: nothing below 3.3 m (trunks, low branches, shrubs)
  // may enter a canopy-sized footprint plus 0.5 m; higher crowns may overhang.
  // Each model is rasterised once into a local 0.2 m column grid holding the
  // lowest geometry height, then stamped per instance into a world 0.25 m grid.
  const CLEAR_HEIGHT = 3.3, MARGIN = 0.5, LOCAL = 0.2, WORLD = 0.25, N = Math.round(SCENE / WORLD)
  const lowest = new Float64Array(N * N).fill(Infinity)
  for (const species of SPECIES) {
    const { vertices, faces } = parseObj(species.file)
    const columns = new Map()
    for (const face of faces) for (let k = 1; k < face.length - 1; k += 1) {
      const [A, B, C] = [face[0], face[k], face[k + 1]].map((index) => vertices[index - 1])
      const edge = Math.max(Math.hypot(B[0] - A[0], B[2] - A[2]), Math.hypot(C[0] - A[0], C[2] - A[2]), Math.hypot(C[0] - B[0], C[2] - B[2]))
      const steps = Math.max(1, Math.ceil(edge / (LOCAL / 2)))
      for (let u = 0; u <= steps; u += 1) for (let v = 0; v <= steps - u; v += 1) {
        const s = u / steps, t = v / steps, w = 1 - s - t
        const x = A[0] * w + B[0] * s + C[0] * t, y = A[1] * w + B[1] * s + C[1] * t, z = A[2] * w + B[2] * s + C[2] * t
        const key = `${Math.round(x / LOCAL)},${Math.round(z / LOCAL)}`
        if (!(columns.get(key) <= y)) columns.set(key, y)
      }
    }
    for (const [north, east, height, scale, rotation] of species.placements) {
      const radians = -rotation * Math.PI / 180, cosine = Math.cos(radians), sine = Math.sin(radians)
      for (const [key, y] of columns) {
        const [i, k] = key.split(',').map(Number)
        const x = i * LOCAL * scale, z = k * LOCAL * scale
        const row = Math.floor((north + cosine * x + sine * z) / WORLD), col = Math.floor((east - sine * x + cosine * z) / WORLD)
        // Fill the scaled column footprint so rotation cannot leave gaps.
        for (let dr = -1; dr <= 1; dr += 1) for (let dc = -1; dc <= 1; dc += 1) {
          const r = row + dr, cc = col + dc
          if (r >= 0 && r < N && cc >= 0 && cc < N) lowest[r * N + cc] = Math.min(lowest[r * N + cc], height + y * scale)
        }
      }
    }
  }
  const cellsIn = (spot, halfAlong, halfAcross, test) => {
    const radians = (spot.rotation - 90) * Math.PI / 180, reach = Math.hypot(halfAlong, halfAcross)
    let count = 0
    for (let r = Math.floor((spot.north - reach) / WORLD); r <= Math.floor((spot.north + reach) / WORLD); r += 1)
      for (let cc = Math.floor((spot.east - reach) / WORLD); cc <= Math.floor((spot.east + reach) / WORLD); cc += 1) {
        if (r < 0 || r >= N || cc < 0 || cc >= N) continue
        const dn = (r + 0.5) * WORLD - spot.north, de = (cc + 0.5) * WORLD - spot.east
        if (Math.abs(dn * Math.cos(radians) + de * Math.sin(radians)) <= halfAlong && Math.abs(-dn * Math.sin(radians) + de * Math.cos(radians)) <= halfAcross && test(lowest[r * N + cc])) count += 1
      }
    return count
  }
  const candidates = []
  for (let north = 8; north <= SCENE - 8; north += 1) for (let east = 8; east <= SCENE - 8; east += 1) for (const rotation of [90, 0]) {
    const spot = { north, east, rotation }
    if (cellsIn(spot, CANOPY_FOOTPRINT.length / 2 + MARGIN, CANOPY_FOOTPRINT.width / 2 + MARGIN, (h) => h < CLEAR_HEIGHT)) continue
    candidates.push({ ...spot, cover: cellsIn(spot, CANOPY_FOOTPRINT.length / 2, CANOPY_FOOTPRINT.width / 2, (h) => Number.isFinite(h)) })
  }
  // Least crown cover first (the nadir sensor must see each vehicle and its cover),
  // then spread over the forest by farthest-point selection.
  const minimumCover = Math.min(...candidates.map((candidate) => candidate.cover))
  const open = candidates.filter((candidate) => candidate.cover <= minimumCover + 5)
  spots.push(open.reduce((best, candidate) => Math.hypot(candidate.north - 50, candidate.east - 50) < Math.hypot(best.north - 50, best.east - 50) ? candidate : best))
  while (spots.length < 11) {
    let best = null, bestDistance = 0
    for (const candidate of open) {
      const distance = Math.min(...spots.map((spot) => Math.hypot(spot.north - candidate.north, spot.east - candidate.east)))
      if (distance > bestDistance) { best = candidate; bestDistance = distance }
    }
    if (!best || bestDistance < 10) throw new Error(`Only ${spots.length} open spots at least 10 m apart (${candidates.length} clear candidates)`)
    spots.push(best)
  }
  console.log(`Clear candidates ${candidates.length}, open (cover <= ${minimumCover + 5} cells) ${open.length}`)

  mkdirSync(join(exampleRoot, 'models'), { recursive: true })
  items.push(...addVehicleLineup({ theme: 'forest', spots, configuration: c, exampleRoot, projectRoot, sceneSize: SCENE }))
  const shared = new Set()
  for (const s of c.spectra) for (const file of [s.fileName, s.physicalTexture?.fileName, s.classTexture?.fileName, s.opennessTexture?.fileName]) if (file) shared.add(file)
  for (const file of shared) {
    if (!String(file).startsWith('assets/')) continue
    mkdirSync(dirname(join(exampleRoot, file)), { recursive: true })
    copyFileSync(join(projectRoot, file), join(exampleRoot, file))
  }
}
c.objects = { count: items.length, names: items.map((item) => item.name), items }

const validation = validateProject(project)
if (!validation.valid) throw new Error(validation.errors.join('; '))
writeFileSync(join(exampleRoot, 'project.json'), stringifyProject(project), 'utf8')
const forestLine = '- Forest: realistic 100 m layout (`' + LAYOUT + '`): 20 jacaranda trees (Poly Haven, 24 m crowns) and 200 searsia shrubs, with separate leaf, branch and trunk meshes'
writeFileSync(join(exampleRoot, 'README.md'), withVehicles ? `# Realistic forest vehicle example

Open it from **打开示例** in Stream Studio and run the RayTracing configuration.

${forestLine}
- 11 vehicles in forest openings, at least 10 m apart; nothing below 3.3 m enters a canopy footprint (plus 0.5 m), higher crowns may overhang:
  - 1 bare white SUV (reference)
  - 5 cars under gable shade canopies: olive canvas, green net (35 % open), damaged green net, white PE stripes, IR camouflage woodland
  - 5 tarp-covered cars: woodland camouflage, multispectral woodland camouflage (class texture), IR camouflage digital, olive canvas, white PE
- Sensor: 100 bands, 400-2380 nm, 0.125 m pixels
- Vehicles are always dropped onto the ground before a run
` : `# Realistic forest GPP/NPP example

Open it from **打开示例** in Stream Studio and run the VoxelEB configuration.

${forestLine}
- Domain: 100 m x 100 m x 23 m, voxel 0.5 m, C3 physiology
- Meteorology: 48 half-hour nodes
- Outputs: images, radiation and energy process, canopy temperature, GPP, NPP
`, 'utf8')
console.log(`Generated ${exampleRoot}`)
for (const spot of spots) console.log(`spot north ${spot.north} east ${spot.east} rot ${spot.rotation} crown cover cells ${spot.cover}`)
