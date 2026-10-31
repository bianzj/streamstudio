// node statistic/run_scene_examples.mjs [output-parent]
// Python must provide numpy and Pillow; override with STREAM_STUDIO_PYTHON if needed.
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawnSync } from 'node:child_process'
import { createDefaultProject, validateProject } from '../src/renderer/src/project-schema.js'

const root = fileURLToPath(new URL('../', import.meta.url))
const parent = path.resolve(process.argv[2] || path.join(root, 'output'))
fs.mkdirSync(parent, { recursive: true })
const destination = fs.mkdtempSync(path.join(parent, 'component-scenes-'))
const exe = [path.join(root, 'engine/histream.exe'), path.join(root, 'models/bin_x64/Release/histream.exe')]
  .find(candidate => fs.existsSync(candidate))
if (!exe) throw new Error('HiStream executable not found; build the engine or run from the installed resources/statistic folder')
const python = process.env.STREAM_STUDIO_PYTHON || 'python'
for (const name of ['building', 'forest']) {
  const folder = path.join(destination, name)
  fs.mkdirSync(path.join(folder, 'inputs'), { recursive: true })
  fs.mkdirSync(path.join(folder, 'output'))
  const project = createDefaultProject({ mode: 'eFacetEB', name: name === 'building' ? '建筑组分统计示例' : '森林组分统计示例' })
  const c = project.configuration
  c.outDir = path.join(folder, 'output')
  Object.assign(c.scene, { x: 24, y: 24, height: 15, voxel: 1 })
  Object.assign(c.control, { samples: 64, depth: 3, periodicTraversalCount: 0 })
  Object.assign(c.sensor, { image: false, process: true, radiationProcess: true, energyProcess: true, bands: '560,10500' })
  const meteo = path.join(folder, 'inputs/meteo.txt')
  fs.copyFileSync(path.join(root, 'statistic/inputs/scene-example-meteo.txt'), meteo)
  Object.assign(c.meteo, { path: meteo, start: 24, end: 25 })
  c.fluid.enabled = false
  const definitions = name === 'building'
    ? [{ file: 'building/house_a.obj', type: 'Building', positions: '12 12 0 1 0\n' }]
    : [
        { file: 'vegetation/tree_oak.obj', type: 'Vegetation', positions: '5 5 0 1 0\n12 12 0 1 25\n19 19 0 1 50\n' },
        { file: 'vegetation/tree_broadleaf.obj', type: 'Vegetation', positions: '12 5 0 1 15\n19 12 0 1 40\n5 19 0 1 65\n' },
        { file: 'vegetation/tree_oak.obj', type: 'Vegetation', positions: '19 5 0 0.85 45\n5 12 0 0.85 70\n12 19 0 0.85 95\n' }
      ]
  c.objects.items = definitions.map((item, index) => {
    const fileName = path.join(folder, 'inputs', path.basename(item.file))
    const positionFile = path.join(folder, 'inputs', `positions_${index}.txt`)
    fs.copyFileSync(path.join(root, 'assets/obj-library', item.file), fileName)
    fs.writeFileSync(positionFile, item.positions)
    const building = item.type === 'Building'
    return { name: `object_${index}`, type: item.type, fileName, positionFile,
      materialName: building ? 'building_surface' : 'leaf_c3',
      spectralName: building ? 'concrete' : 'green_leaf',
      thermalName: building ? 'building_temperature' : 'vegetation_temperature',
      canopyName: building ? 'rigid_body' : 'canopy_default', meshes: [] }
  })
  c.objects.names = c.objects.items.map(item => item.name)
  c.objects.count = c.objects.items.length
  const validation = validateProject(project)
  if (!validation.valid) throw new Error(validation.errors.join('; '))
  const input = path.join(folder, 'project.json')
  fs.writeFileSync(input, JSON.stringify(project, null, 2))
  console.log(`Running ${name}: ${input}`)
  const start = Date.now()
  const result = spawnSync(exe, ['eFacetEB', input], { cwd: path.dirname(exe), windowsHide: true,
    env: { ...process.env, HISTREAM_FACET_STEP_SYNC: '0' }, encoding: 'utf8', timeout: 600000, maxBuffer: 16 * 1024 * 1024 })
  fs.writeFileSync(path.join(folder, 'run.log'), String(result.stdout || '') + '\n' + String(result.stderr || ''))
  if (result.status !== 0) throw new Error(`${name} failed: ${result.error || result.stderr}; see run.log`)
  console.log(`${name}: simulation completed in ${((Date.now() - start) / 1000).toFixed(1)} seconds`)
  const stats = spawnSync(python, [path.join(root, 'statistic/histream_statistics.py'), path.join(c.outDir, 'process'), '-o', path.join(folder, 'statistics')],
    { windowsHide: true, encoding: 'utf8', timeout: 180000, maxBuffer: 4 * 1024 * 1024 })
  fs.writeFileSync(path.join(folder, 'statistics.log'), String(stats.stdout || '') + '\n' + String(stats.stderr || ''))
  if (stats.status !== 0 || !fs.existsSync(path.join(folder, 'statistics'))) throw new Error(`Statistics failed: ${stats.error || stats.stderr}`)
}
const report = spawnSync(python, [path.join(root, 'statistic/scene_example_report.py'), destination],
  { windowsHide: true, encoding: 'utf8', timeout: 60000 })
if (report.status !== 0) throw new Error(`Report failed: ${report.error || report.stderr}`)
console.log(`Results: ${destination}`)
