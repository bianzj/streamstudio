// ==========================================================================
// 文件：run_scene_examples.mjs
// 功能：端到端生成并运行“建筑 / 森林”两个 FacetEB 组分统计示例，再汇总出对照图与 CSV。
// 主要步骤：
//   1. 解析输出父目录，在其下建临时目录 component-scenes-*，并定位 histream.exe
//   2. 对 building / forest 两个场景：用 createDefaultProject 构造 eFacetEB 工程（24×24 m，
//      64 样本，深度 3，关闭周期邻域与流体），拷贝 OBJ 与位置文件，写 project.json 并校验
//   3. 调用 histream.exe 运行 eFacetEB，日志写入 run.log
//   4. 调用 histream_statistics.py 对 output/process 做统计，输出到 statistics/
//   5. 调用 scene_example_report.py 生成两场景对照图、汇总 CSV 与说明 TXT
// 数据流：assets/obj-library 与 statistic/inputs 的气象 → <dest>/<场景>/{inputs,output,statistics}。
// 与其他文件：依赖 src/renderer/src/project-schema.js、statistic/histream_statistics.py、
//   statistic/scene_example_report.py。
// 注意：project.json 使用绝对路径，换位置后需重新指定；只跑单个时刻，仅验证统计展示。
// ==========================================================================

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
// 步骤 1：优先使用安装目录 engine/ 下的引擎，其次用开发构建输出（Release）。
const exe = [path.join(root, 'engine/histream.exe'), path.join(root, 'models/bin_x64/Release/histream.exe')]
  .find(candidate => fs.existsSync(candidate))
if (!exe) throw new Error('HiStream executable not found; build the engine or run from the installed resources/statistic folder')
const python = process.env.STREAM_STUDIO_PYTHON || 'python'
// 步骤 2-4：逐场景构造工程 → 运行引擎 → 做统计；任一步骤失败即抛错中止。
for (const name of ['building', 'forest']) {
  const folder = path.join(destination, name)
  fs.mkdirSync(path.join(folder, 'inputs'), { recursive: true })
  fs.mkdirSync(path.join(folder, 'output'))
  const project = createDefaultProject({ mode: 'eFacetEB', name: name === 'building' ? '建筑组分统计示例' : '森林组分统计示例' })
  // 场景参数：24×24 m、高 15 m、体元 1 m；64 样本、深度 3、不启用周期邻域；
  // 只输出过程文件（process），不出影像；波段 560/10500 分别对应可见光与热红外（待核对）。
  const c = project.configuration
  c.outDir = path.join(folder, 'output')
  Object.assign(c.scene, { x: 24, y: 24, height: 15, voxel: 1 })
  Object.assign(c.control, { samples: 64, depth: 3, periodicTraversalCount: 0 })
  Object.assign(c.sensor, { image: false, process: true, radiationProcess: true, energyProcess: true, bands: '560,10500' })
  const meteo = path.join(folder, 'inputs/meteo.txt')
  fs.copyFileSync(path.join(root, 'statistic/inputs/scene-example-meteo.txt'), meteo)
  // 气象只取 start..end 这一段，单个时刻演示（对应图中 DOY 213 12:15，待核对）。
  Object.assign(c.meteo, { path: meteo, start: 24, end: 25 })
  c.fluid.enabled = false
  // 物体定义：positions 每行一个实例（前三列像是 x y z，后两列为缩放与旋转，待核对）；建筑 1 栋，森林 9 株树。
  const definitions = name === 'building'
    ? [{ file: 'building/house_a.obj', type: 'Building', positions: '12 12 0 1 0\n' }]
    : [
        { file: 'vegetation/tree_oak.obj', type: 'Vegetation', positions: '5 5 0 1 0\n12 12 0 1 25\n19 19 0 1 50\n' },
        { file: 'vegetation/tree_broadleaf.obj', type: 'Vegetation', positions: '12 5 0 1 15\n19 12 0 1 40\n5 19 0 1 65\n' },
        { file: 'vegetation/tree_oak.obj', type: 'Vegetation', positions: '19 5 0 0.85 45\n5 12 0 0.85 70\n12 19 0 0.85 95\n' }
      ]
  // 把 OBJ 与位置文件复制到 inputs/ 并生成物体条目；整个 OBJ 统一赋予建筑或
  // 植被材质/光谱/温度/冠层属性，不细分木质部与叶片。
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
  // 写出 project.json 前先做 schema 校验。
  const validation = validateProject(project)
  if (!validation.valid) throw new Error(validation.errors.join('; '))
  const input = path.join(folder, 'project.json')
  fs.writeFileSync(input, JSON.stringify(project, null, 2))
  console.log(`Running ${name}: ${input}`)
  const start = Date.now()
  // 步骤 3：HISTREAM_FACET_STEP_SYNC=0 即不在运行中逐节点同步写出中间输出（仅 =1 时开启）；超时 10 分钟。
  const result = spawnSync(exe, ['eFacetEB', input], { cwd: path.dirname(exe), windowsHide: true,
    env: { ...process.env, HISTREAM_FACET_STEP_SYNC: '0' }, encoding: 'utf8', timeout: 600000, maxBuffer: 16 * 1024 * 1024 })
  fs.writeFileSync(path.join(folder, 'run.log'), String(result.stdout || '') + '\n' + String(result.stderr || ''))
  if (result.status !== 0) throw new Error(`${name} failed: ${result.error || result.stderr}; see run.log`)
  console.log(`${name}: simulation completed in ${((Date.now() - start) / 1000).toFixed(1)} seconds`)
  // 步骤 4：对能量过程输出做分组分统计，结果目录 statistics/ 必须存在才算成功。
  const stats = spawnSync(python, [path.join(root, 'statistic/histream_statistics.py'), path.join(c.outDir, 'process'), '-o', path.join(folder, 'statistics')],
    { windowsHide: true, encoding: 'utf8', timeout: 180000, maxBuffer: 4 * 1024 * 1024 })
  fs.writeFileSync(path.join(folder, 'statistics.log'), String(stats.stdout || '') + '\n' + String(stats.stderr || ''))
  if (stats.status !== 0 || !fs.existsSync(path.join(folder, 'statistics'))) throw new Error(`Statistics failed: ${stats.error || stats.stderr}`)
}
// 步骤 5：两个场景都完成后，汇总绘制对照图与 CSV/TXT。
const report = spawnSync(python, [path.join(root, 'statistic/scene_example_report.py'), destination],
  { windowsHide: true, encoding: 'utf8', timeout: 60000 })
if (report.status !== 0) throw new Error(`Report failed: ${report.error || report.stderr}`)
console.log(`Results: ${destination}`)
