// ==========================================================================
// 文件：main.js
// 功能：StreamStudio 渲染进程（Vite + 原生 JS）的主入口。管理工程状态 state、Three.js 三维场景预览、
//       右侧属性面板（inspector）、各类对话框（物体/材质/预设/几何生成）、模拟运行与结果查看。
// 主要模块（按文件中出现的先后顺序）：
//   1. 导入与全局常量：纹理预设列表、全局 state（模式、工程路径、运行进度、结果视图等）
//   2. 预设合并与配置解析：ensureMaterialPresets、parseMaterialNode、parseXml（旧 XML 兼容，
//      新工程为 project.json，由 project-schema.js 解析）
//   3. 工程文本同步、toast / 日志 / 运行进度条（setRunning 等）
//   4. 三维场景交互：几何绘制（geometryDraw）、物体选取与变换编辑（objectEdit）
//   5. 三维场景构建：天空盒、场景边界引导、巡航/卫星观测可视化、generateWorld、updateSun、
//      体元预览 buildVoxelPreview、相机与渲染循环 animate
//   6. 属性面板：renderInspector 生成 HTML，bindInspector 绑定输入；质量预设与计算设备选择
//   7. 工程读写：loadXml / saveXml、示例工程、气象与 DEM 导入
//   8. 目标分布（distribution）与运动（movement）的表单与采样生成
//   9. 物体 / 材质 / 物化参数 / 纹理 / 预设对话框及 OBJ 几何生成与导入
//  10. 运行模拟：预检、启动、事件处理、引擎日志质量统计（runQuality）
//  11. 结果查看：ENVI/TIFF/图像/风场预览，面元/体元/土壤剖面三维视图，统计分析图表，变化分析
//  12. 工程对话框（新建/另存为/参考/HiStream 代理）、帮助、菜单栏与 STREAM 工作台接入
//  13. init()：读取桌面桥接默认值、重新打开上次工程；启动本地化
// 数据流：用户操作 → 修改 state.config（工程配置）→ markXmlDirty/syncProjectText 同步工程文本
//       → refreshFromState 重建三维预览；运行时通过 webApi 调用服务端，服务端事件回到
//       handleSimulationEvent 更新进度与日志，结果文件由 refreshResults/openResultFile 读取显示。
// 与其他文件的关系：web-api.js（与服务端通信）、project-schema.js（工程结构、默认值与质量预设）、
//       scene-loader.js（把配置转为 Three.js 场景）、i18n.js（界面本地化 t()）、
//       stream/workspace.js（STREAM 工作台）、menubar.js（菜单栏）、material-specular.js。
// 约定：模式名为 eRaytracing / eVoxelRT / eVoxelEB / eFacetRT / eFacetEB；
//       场景尺寸、距离以米计（待核对）；角度以度计（天顶角 VZA、方位角 VAA）。
// ==========================================================================

import { webApi } from './web-api.js'
import { appStorage } from './storage.js'
import { getLocale, startLocalization, t } from './i18n.js'
import { invalidateImageTextures, loadXmlScene, updateSceneDynamics } from './scene-loader.js'
import { createVehicleBodyMaterial, VEHICLE_PAINT_COLORS, VEHICLE_SEAT_TYPES, VEHICLE_SPECTRA, splitVehicleObj, vehicleEngineDefaults, vehicleInteriorDefaults, vehicleMeshBinding, vehiclePaintSpectrumName } from './project-schema.js'
import { autoRadiationMaxSteps, createDefaultProject, createMaterialPresets, DEFAULT_CPU_THREADS, groundedPlacementHeight, MAX_SENSOR_BANDS, normalizeCpuThreads, normalizeRunTimeoutMinutes, normalizeTerrainEnvironment, normalizeProject, parseProjectJson, QUALITY_PRESET_LABELS, QUALITY_PRESETS, qualityPresetFields, qualityPresetMatches, qualityPresetValues, sensorBandValues, sensorCruisePositions, sensorViewAngles, simplifySolarSpectra, stringifyProject } from './project-schema.js'
import { createStreamWorkspace } from './stream/workspace.js'
import { createMenubar } from './menubar.js'
import * as THREE from 'three'
import { specularParametersFromValues, specularParameterFields, normalizeSpecular, meshSpecular, meshSpecularFields, meshSpecularFromRow, meshSpecularRunError } from './material-specular.js'
import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
import { TransformControls } from 'three/addons/controls/TransformControls.js'
import { OBJLoader } from 'three/addons/loaders/OBJLoader.js'
import './styles.css'

// 模块：DOM 简写与全局句柄。$ 取单个元素，$$ 取元素数组；api 为与服务端通信的 webApi；
// histreamProxyState 缓存 HiStream 代理对话框的信息与最近一次结果（见 openHistreamProxyDialog）。
const $ = (selector) => document.querySelector(selector)
const $$ = (selector) => [...document.querySelectorAll(selector)]
const api = webApi
const histreamProxyState = { info: null, result: null }

// 模块：纹理预设清单。每项为 [资源相对路径或内置键, 界面显示名]，供各纹理下拉框使用。
//   PHYSICAL_TEXTURE_PRESETS：物理纹理（*_variance.jpg，用于调制材质参数的空间变化，待核对）
//   CLASS_TEXTURE_PRESETS：分类纹理（按类别索引给不同参数）
//   OPENNESS_TEXTURE_PRESETS：开度纹理（遮阳网等的疏密/破损）
//   PREVIEW_TEXTURE_PRESETS：三维预览外观纹理（只影响预览，键名由 scene-loader 解释，待核对）
const PHYSICAL_TEXTURE_PRESETS = [
  ['assets/texture-library/concrete_variance.jpg', '混凝土 · Concrete034'],
  ['assets/texture-library/brick_variance.jpg', '砖墙 · Bricks075A'],
  ['assets/texture-library/asphalt_variance.jpg', '沥青 · Asphalt031'],
  ['assets/texture-library/metal_variance.jpg', '金属 · Metal046B'],
  ['assets/texture-library/canvas_weave_variance.jpg', '帆布织纹 · 程序生成'],
  ['assets/texture-library/camouflage_woodland_variance.jpg', '林地迷彩 · 程序生成'],
  ['assets/texture-library/camouflage_desert_variance.jpg', '荒漠迷彩 · 程序生成'],
  ['assets/texture-library/camouflage_digital_variance.jpg', '数码迷彩 · 程序生成'],
  ['assets/texture-library/stripes_variance.jpg', '遮阳条纹 · 程序生成'],
  ['assets/texture-library/crumpled_film_variance.jpg', '褶皱反光膜 · 程序生成'],
  ['assets/texture-library/sand_mottling_variance.jpg', '沙面风纹 · 程序生成'],
  ['assets/texture-library/soil_mottling_variance.jpg', '裸土斑块 · 程序生成']
]

const CLASS_TEXTURE_PRESETS = [
  ['assets/texture-library/class_woodland_4.png', '林地迷彩 · 4 类'],
  ['assets/texture-library/class_digital_4.png', '数码迷彩 · 4 类'],
  ['assets/texture-library/class_desert_3.png', '荒漠迷彩 · 3 类']
]

const OPENNESS_TEXTURE_PRESETS = [
  ['assets/texture-library/openness_damaged_net.png', '遮阳网破损'],
  ['assets/texture-library/openness_weave_density.png', '织物疏密不均']
]

const PREVIEW_TEXTURE_PRESETS = [
  ['solar-panel', '太阳能电池板'],
  ['red-brick', '红砖墙'],
  ['gravel', '石子路 / 粗粒路面'],
  ['concrete', '混凝土墙'],
  ['plaster', '浅色粉刷墙'],
  ['stone-wall', '自然石墙'],
  ['roof-tile', '红瓦屋面'],
  ['metal', '金属表面'],
  ['wood', '木材'],
  ['grass', '草地'],
  ['canvas', '帆布 / 篷布'],
  ['camouflage-woodland', '林地迷彩'],
  ['camouflage-desert', '荒漠迷彩'],
  ['camouflage-digital', '数码迷彩'],
  ['stripes', '遮阳条纹'],
  ['shade-net', '遮阳网']
]

// 模块：渲染进程全局状态（单例）。
//   - mode / config：当前计算模式与工程配置（project.configuration 的引用）；
//   - inputPath / projectPath / xmlText / xmlDirty / editRevision：工程文件路径、
//     序列化文本、未保存标记与修改版本号（用于判断保存期间是否又被编辑）；
//   - running / progress / progressStage：模拟运行状态；
//   - pendingObject / editingObjectIndex / selectedObjectIndex 等：对象对话框与三维选取；
//   - resultFiles / selectedResult / resultView / analysis*：结果查看器状态；
//   - result*Request：异步请求计数，用于丢弃过期的预览/打开结果。
const state = {
  mode: 'eVoxelEB',
  executable: '',
  executableExists: true,
  defaultExecutable: '',
  defaultExecutableExists: false,
  inputPath: '',
  outputDir: '',
  xmlText: '',
  xmlDirty: false,
  editRevision: 0,
  projectGeneration: 0,
  xmlManualEdit: false,
  runPending: false,
  lastSaveError: null,
  project: null,
  projectPath: '',
  running: false,
  startedAt: 0,
  progress: 0,
  progressStage: '待命',
  logCount: 0,
  panel: 'scene',
  config: null,
  pendingObject: null,
  drawPlacement: null,
  editingObjectIndex: -1,
  selectedObjectIndex: -1,
  editingAttributeIndex: -1,
  editingPreset: null,
  presetPhysicalType: 'Vegetation',
  resultFiles: [],
  selectedResult: null,
  resultRaster: null,
  resultView: 'image',
  analysisType: 'angle',
  analysisSelection: {},
  changeAnalysisRequest: 0,
  resultDisplay: {},
  resultPreviewRequest: 0,
  resultOpenRequest: 0
}

// 作用：把工程中已有的命名预设 values 合并到内置预设 presets 上，同名项逐字段覆盖。
// 输入：内置预设数组、工程预设数组（均含 name）；输出：合并后的新数组（内置顺序在前）。
// 说明：params / previewTexture / physicalTexture 做浅合并，使旧工程缺失的新字段保留内置缺省。
function mergeNamedPresets(presets, values) {
  const merged = new Map(presets.map((item) => [item.name, item]))
  for (const item of values || []) {
    const preset = merged.get(item.name) || {}
    merged.set(item.name, { ...preset, ...item, params: { ...(preset.params || {}), ...(item.params || {}) }, previewTexture: { ...(preset.previewTexture || {}), ...(item.previewTexture || {}) }, physicalTexture: { ...(preset.physicalTexture || {}), ...(item.physicalTexture || {}) } })
  }
  return [...merged.values()]
}

// 作用：把标签中的“Canopy / 冠层”统一显示为“混沌介质”（结构类型改名后的兼容显示）。
function structureLabel(value) {
  return String(value || '')
    .replace(/Canopy\s*冠层/gi, '混沌介质')
    .replace(/Canopy/gi, '混沌介质')
    .replace(/冠层/g, '混沌介质')
}

// 作用：打开/新建工程后，把内置的光谱、结构（canopy）、热、材质预设合并进 config，并规范化。
// 输入：config（原地修改并返回）；副作用：改写 config.spectra/canopies/thermals/materials 及
//   objects.count/names。
// 说明：旧标签“火焰/烟羽”统一改为“火焰”；BSM 光谱的 BSMlat/BSMlon 同为 0 时视为未设置，
//   改回缺省 25/45；所有 Soil 材质的 method 与 control.soilTemperatureMethod 对齐（限幅 0..2，缺省 2）。
function ensureMaterialPresets(config) {
  const presets = createMaterialPresets()
  config.spectra = simplifySolarSpectra(mergeNamedPresets(presets.spectra, config.spectra), config)
    .map((item) => {
    item = { ...item, label: String(item.label || '').replaceAll('火焰/烟羽', '火焰') }
    if (item.name === 'water_surface') item.label = '水体光谱'
    if (item.model !== 'BSM') return item
    const params = item.params || {}
    const latitude = Number(params.BSMlat)
    const longitude = Number(params.BSMlon)
    if (Number.isFinite(latitude) && Number.isFinite(longitude)
      && Math.abs(latitude) < 1e-9 && Math.abs(longitude) < 1e-9) {
      return { ...item, params: { ...params, BSMlat: 25, BSMlon: 45 } }
    }
    return item
  })
  config.canopies = mergeNamedPresets(presets.canopies, config.canopies)
    .map((item) => ({ ...item, label: structureLabel(String(item.label || '').replaceAll('火焰/烟羽', '火焰')), structureType: ['canopy', 'rigid', 'fire', 'fog'].includes(item.structureType) ? item.structureType : 'canopy' }))
  config.thermals = mergeNamedPresets(presets.thermals, config.thermals)
    .map((item) => ({ ...item, label: item.name === 'water_temperature' ? '水体初始温度' : String(item.label || '').replaceAll('火焰/烟羽', '火焰') }))
  config.objects.items = config.objects.items || []
  config.objects.count = config.objects.items.length
  config.objects.names = config.objects.items.map((item) => item.name)
  config.materials = mergeNamedPresets(presets.materials, config.materials)
    .map((item) => ({ ...item, label: item.name === 'water_set' ? '水体物化参数' : item.label }))
  const soilMethod = Math.max(0, Math.min(2, Math.round(Number(config.control?.soilTemperatureMethod ?? 2))))
  config.materials.filter((item) => item.type === 'Soil').forEach((item) => { item.params.method = soilMethod })
  return config
}

// 作用：返回一份新的缺省工程配置；气象路径指向“HiStream 内置气象数据”。
const defaultConfig = () => {
  const config = createDefaultProject().configuration
  config.meteo.path = 'HiStream 内置气象数据'
  return config
}

// 作用：转义 & < > ' " ，用于把文本安全拼入 innerHTML 模板。
function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>'"]/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[char])
}

// 作用：读取旧 XML 工程中 selector 对应元素的文本（去空白），缺失或为空时返回 fallback。
function xmlValue(doc, selector, fallback = '') {
  return doc?.querySelector(selector)?.textContent?.trim() || fallback
}

// 作用：解析 “a,b” 形式的数值对；元素不足 2 个或含非数时返回 fallback。parseTriple 同理取前 3 个。
function parsePair(value, fallback) {
  const parts = String(value).split(',').map(Number)
  return parts.length >= 2 && parts.every(Number.isFinite) ? parts : fallback
}

function parseTriple(value, fallback) {
  const parts = String(value).split(',').map(Number)
  return parts.length >= 3 && parts.slice(0, 3).every(Number.isFinite) ? parts.slice(0, 3) : fallback
}

// 作用：从工程文本中识别计算模式。优先按 project.json 解析；失败再按旧 XML 根元素 mode 属性；
//   都不认识时返回 fallback。
function projectModeFromXml(content, fallback = state.mode) {
  try {
    const project = parseProjectJson(content)
    return project.mode
  } catch {}
  const doc = new DOMParser().parseFromString(String(content || ''), 'application/xml')
  const mode = doc.documentElement?.getAttribute('mode')
  return ['eVoxelEB', 'eFacetEB', 'eFacetRT', 'eVoxelRT', 'eRaytracing'].includes(mode) ? mode : fallback
}

// 作用：把旧 XML 的材质节点解析为工程材质 {name, type, params}。
// 输入：节点标签 leafBio→Vegetation（叶片光合/气孔参数）、waterSet→Water、其余（soilSet）→Soil。
// 说明：soilSet 还承载 FacetEB 的扩展参数：墙体（wallMode / 分层）、船体（吃水线、航速、
//   航向风向）、车辆部件（vehiclePart）及座舱/发动机参数；各项仅在对应标签存在时才写入 params，
//   以保持旧工程的缺省行为；含 vehiclePart 的材质类型记为 Vehicle。
function parseMaterialNode(node) {
  const type = node.tagName === 'leafBio' ? 'Vegetation' : node.tagName === 'waterSet' ? 'Water' : 'Soil'
  let params
  if (type === 'Vegetation') {
    params = {
      Vcmax: Number(xmlValue(node, 'Vcmax', 80)), m: Number(xmlValue(node, 'm', 9)),
      BallBerry: Number(xmlValue(node, 'BallBerry', 0.01)), Type: Number(xmlValue(node, 'Type', 3)),
      kV: Number(xmlValue(node, 'kV', 0.6396)), Rdparam: Number(xmlValue(node, 'Rdparam', 0.015)),
      Tparam: xmlValue(node, 'Tparam', '0.2,0.3,288,313,328'), Tyear: Number(xmlValue(node, 'Tyear', 25)),
      beta: Number(xmlValue(node, 'beta', 0.507)), kNPQs: Number(xmlValue(node, 'kNPQs', 0)),
      qLs: Number(xmlValue(node, 'qLs', 1)), stressfactor: Number(xmlValue(node, 'stressfactor', 1)), Tcor: Number(xmlValue(node, 'Tcor', 0))
    }
  } else if (type === 'Water') {
    params = {
      rss: Number(xmlValue(node, 'rss', 0)), heatCapacity: Number(xmlValue(node, 'heatCapacity', 4186000)),
      mixingDepth: Number(xmlValue(node, 'mixingDepth', 0.5)), evaporationCoefficient: Number(xmlValue(node, 'evaporationCoefficient', 1)),
      brdfModel: ['1', 'coxmunk', 'cox-munk'].includes(xmlValue(node, 'brdfModel', 'CoxMunk').toLowerCase()) ? 1 : 0,
      refractiveIndex: Number(xmlValue(node, 'refractiveIndex', 1.333)), slopeVariance: Number(xmlValue(node, 'slopeVariance', 0)),
      diffuseFraction: Number(xmlValue(node, 'diffuseFraction', 0.02))
    }
    // 水体初始温度模式：dailyMeanAir（取日均气温）或 fixed（固定值，另读 waterInitialTemperature）。
    const waterInitial = xmlValue(node, 'waterInitialMode', '')
    if (waterInitial === 'dailyMeanAir' || waterInitial === 'fixed') params.initialTemperatureMode = waterInitial
    if (waterInitial === 'fixed') params.initialTemperature = Number(xmlValue(node, 'waterInitialTemperature', 20))
  } else {
    params = {
      method: Number(xmlValue(node, 'method', 2)), rss: Number(xmlValue(node, 'rss', 2000)),
      cs: Number(xmlValue(node, 'cs', 1180)), rhos: Number(xmlValue(node, 'rhos', 1800)),
      lambdas: Number(xmlValue(node, 'lambdas', 1.55)), Tsoil: Number(xmlValue(node, 'Tsoil', 25)),
      SMC: Number(xmlValue(node, 'SMC', 25)), Satwater: Number(xmlValue(node, 'Satwater', 0.45))
    }
    if (xmlValue(node, 'wallMode', '') === 'wall') Object.assign(params, {
      wallMode: 'wall', thickness: Number(xmlValue(node, 'thickness', 0.2)),
      indoorTemperature: Number(xmlValue(node, 'indoorTemperature', 22)),
      indoorHeatTransferCoefficient: Number(xmlValue(node, 'indoorHeatTransferCoefficient', 8))
    })
    if (params.wallMode === 'wall' && xmlValue(node, 'wallLayers', '').trim()) params.layers = parseWallLayers(xmlValue(node, 'wallLayers', ''))
    // FacetEB ship hull options (soilSet of a Ship material).
    if (xmlValue(node, 'shipSpeed', '').trim()) params.speed = Number(xmlValue(node, 'shipSpeed', 0))
    // FacetEB vehicle part and cabin parameters (vehiclePart 1 body, 2 glass, 3 underbody).
    const vehiclePart = { 1: 'body', 2: 'glass', 3: 'underbody', 4: 'hood' }[Number(xmlValue(node, 'vehiclePart', 0))]
    if (vehiclePart) {
      params.vehiclePart = vehiclePart
      if (vehiclePart === 'body' && xmlValue(node, 'vcEnabled', '').trim() !== '') {
        const v = (tag, fallback) => Number(xmlValue(node, tag, fallback))
        params.cabin = { enabled: v('vcEnabled', 0) > 0.5, occupants: v('vcOccupants', 0), metabolicW: v('vcMetabolicW', 100), speed: v('vcSpeed', 0),
          ventilationACH: v('vcVentilationAch', 0.5), windowCracked: v('vcWindowCracked', 0) > 0.5, crackedACH: v('vcCrackedAch', 10),
          acOn: v('vcAcOn', 0) > 0.5, acPower: v('vcAcPower', 3500), acSetpoint: v('vcAcSetpoint', 24), acBand: v('vcAcBand', 2),
          engineOn: v('vcEngineOn', 0) > 0.5, enginePower: v('vcEnginePower', 15000), engineShare: v('vcEngineShare', 0.3), volume: v('vcVolume', 2.8),
          interiorCapacity: v('vcInteriorCapacity', 80000), interiorArea: v('vcInteriorArea', 8), interiorH: v('vcInteriorH', 8),
          absorbedFraction: v('vcAbsorbedFraction', 0.8) }
        if (xmlValue(node, 'vcHeading', '').trim() && xmlValue(node, 'vcWindDirection', '').trim()) {
          params.cabin.heading = v('vcHeading', 0)
          params.cabin.windDirection = v('vcWindDirection', 0)
        }
        // Parts model: interior nodes and engine / bay nodes (comma lists, same order as the XML writer).
        const list = (tag) => xmlValue(node, tag, '').split(',').map(Number)
        if (xmlValue(node, 'vcIntModel', '').trim() === '1') {
          const [share, capacity, hcA, radArea] = ['vcIntShares', 'vcIntCapacities', 'vcIntHcA', 'vcIntRadArea'].map(list)
          const ids = ['dashboard', 'seat', 'wheel', 'lining']
          const effusivity = v('vcSeatEffusivity', 300)
          params.cabin.interior = { enabled: true, shellRadFraction: v('vcShellRadFraction', 0.45), seatEffusivity: effusivity,
            seatType: effusivity >= 350 ? 'leather' : 'fabric', ...Object.fromEntries(ids.map((id, k) => [id, { share: share[k], capacity: capacity[k], hcA: hcA[k], radArea: radArea[k] }])) }
        }
        if (xmlValue(node, 'vcEngModel', '').trim() === '1') {
          const caps = list('vcEngCaps'), vent = list('vcEngBayVent'), split = list('vcEngSplit')
          params.cabin.engine = { enabled: true, warm: v('vcEngWarm', 0) > 0.5, warmTemperature: v('vcEngWarmTemp', 90), blockCapacity: caps[0], bayCapacity: caps[1],
            blockToBay: v('vcEngBlockBay', 40), bayVent0: vent[0], bayVentK: vent[1], radiatorShare: split[0], blockShare: split[1], exhaustShare: split[2], firewallG: v('vcFirewallG', 2) }
        }
      }
    }
    if (xmlValue(node, 'waterlineHeight', '').trim()) {
      params.waterlineHeight = Number(xmlValue(node, 'waterlineHeight', 0))
      params.waterHeatTransferCoefficient = Number(xmlValue(node, 'waterHeatTransferCoefficient', 500))
      if (xmlValue(node, 'waterTemperature', '').trim()) params.waterTemperature = Number(xmlValue(node, 'waterTemperature', 20))
    } else if (xmlValue(node, 'waterlineSource', '').trim() === 'water') {
      params.waterlineSource = 'water'
      params.waterlineOffset = Number(xmlValue(node, 'waterlineOffset', 0))
      params.waterHeatTransferCoefficient = Number(xmlValue(node, 'waterHeatTransferCoefficient', 500))
      if (xmlValue(node, 'waterTemperature', '').trim()) params.waterTemperature = Number(xmlValue(node, 'waterTemperature', 20))
    }
    if (xmlValue(node, 'shipHeading', '').trim() && xmlValue(node, 'shipWindDirection', '').trim()) {
      params.heading = Number(xmlValue(node, 'shipHeading', 0))
      params.windDirection = Number(xmlValue(node, 'shipWindDirection', 0))
    }
    if (xmlValue(node, 'waterShielding', '').trim() === '1') params.waterShielding = true
    if (xmlValue(node, 'waterFeedback', '').trim() === '1') params.waterFeedback = true
    if (xmlValue(node, 'voxelWall', '').trim() === '1') params.voxelWall = true
    if (xmlValue(node, 'internalHeatFlux', '').trim()) params.internalHeatFlux = Number(xmlValue(node, 'internalHeatFlux', 0))
    if (xmlValue(node, 'internalHeatPower', '').trim()) params.internalHeatPower = Number(xmlValue(node, 'internalHeatPower', 0))
  }
  return { name: node.getAttribute('name') || 'material', type: params.vehiclePart ? 'Vehicle' : type, params }
}

// 作用：把工程文本解析为 configuration 对象（旧 XML 兼容路径；JSON 工程直接走 parseProjectJson）。
// 输入：content（project.json 文本或旧 XML）、mode（决定能量平衡模式下的过程开关缺省）。
// 输出：与 createDefaultProject().configuration 同构的配置；缺失字段用 defaultConfig() 补齐。
// 说明：XML 解析错误抛出中文异常；各数值字段的限幅/兼容缺省见函数内注释。
// 步骤：1 解析 DOM 与缺省值；2 预处理光照/传感器/地形；3 解析对象；4 解析光谱/热/材质/结构；
//   5 组装返回的 scene / light / sensor / control / meteo / atmosphere。
function parseXml(content, mode = state.mode) {
  if (String(content || '').trimStart().startsWith('{')) return parseProjectJson(content).configuration
  const doc = new DOMParser().parseFromString(content, 'application/xml')
  if (doc.querySelector('parsererror')) throw new Error('XML 格式错误，请检查标签是否闭合')
  // 步骤 1：以缺省配置为底，XML 里缺失的字段逐项回退到 base。
  const base = defaultConfig()
  const energyMode = mode === 'eFacetEB' || mode === 'eVoxelEB'
  const lightPair = parsePair(xmlValue(doc, 'Geometry Light light lightAngle lightAngle', '30,135'), [30, 135])
  const samplingNode = doc.querySelector('Geometry Sensor sensor observationSampling')
  const continuousNode = doc.querySelector('Geometry Sensor sensor continuousBand')
  const generatedViewPair = parsePair(xmlValue(doc, 'Geometry Sensor sensor viewAngle viewAngle[type="custom"] viewAngles', '0,0'), [0, 0])
  // 步骤 2：传感器观测角（VZA, VAA）优先取 observationSampling 的 custom 属性，否则取 viewAngles。
  const viewPair = [
    Number(samplingNode?.getAttribute('customVza') ?? generatedViewPair[0]),
    Number(samplingNode?.getAttribute('customVaa') ?? generatedViewPair[1])
  ]
  const sceneX = Number(xmlValue(doc, 'Scene sceneSizeX', base.scene.x))
  const sceneY = Number(xmlValue(doc, 'Scene sceneSizeY', base.scene.y))
  const projectionText = xmlValue(doc, 'Geometry Sensor sensor projection', 'parallel').toLowerCase()
  // 传感器位置缺省：场景中心上方 3000 m（透视/巡航模式使用，待核对单位）。
  const sensorPosition = parseTriple(
    xmlValue(doc, 'Geometry Sensor sensor sensorPosition', ''),
    [sceneX / 2, sceneY / 2, 3000]
  )
  const cruiseNode = doc.querySelector('Geometry Sensor sensor uavPositions')
  // Older exports omitted the heading attribute and wrote customVaa as the
  // effective azimuth (first cruise heading + relative VAA + 180).
  if (cruiseNode && !cruiseNode.hasAttribute('heading') && samplingNode?.hasAttribute('customVaa')) {
    const firstHeading = Number(cruiseNode.querySelector('position')?.getAttribute('heading'))
    if (Number.isFinite(firstHeading) && Number.isFinite(viewPair[1])) viewPair[1] = (((viewPair[1] - firstHeading - 180) % 360) + 360) % 360
  }
  // 直射比例：优先读 directScatteringRatio，旧字段 direct 作后备；限幅到 [0, 1]，散射比 = 1 - 直射比。
  const directFraction = Math.max(0, Math.min(1, finiteNumber(xmlValue(
    doc, 'Geometry Light light directScatteringRatio',
    xmlValue(doc, 'Geometry Light light direct', base.light.direct)
  ), base.light.direct)))
  // 地形开关：有 Control isDEM 时以其值为准；旧工程没有该元素时，只要写了 DEM 文件就视为启用。
  const demFile = xmlValue(doc, 'Scene DEM', '')
  const demSwitchNode = doc.querySelector('Control isDEM')
  const terrainEnabled = demSwitchNode
    ? String(demSwitchNode.textContent || '').trim() === '1'
    : Boolean(demFile)
  const environmentNode = doc.querySelector('Scene TerrainEnvironment')
  const terrainEnvironment = normalizeTerrainEnvironment(Object.fromEntries(
    Object.keys(base.scene.terrainEnvironment).filter((key) => environmentNode?.hasAttribute(key))
      .map((key) => [key, environmentNode.getAttribute(key)])
  ))
  // 步骤 3：解析场景对象。多网格对象的 meshNames / spectralNames / thermalNames / bioNames /
  //   canopyNames 是逗号分隔、按网格序号对齐的列表；某一网格缺项时回退到列表首项。
  //   mediumKind 为 fire / fog 的对象按介质（Fire / Fog）处理，仅参与辐射。
  const objectNodes = [...doc.querySelectorAll('Scene Object object')]
  const names = objectNodes.map((node) => node.getAttribute('objName') || node.getAttribute('name') || `对象 ${node.getAttribute('id') || ''}`)
  const items = objectNodes.map((node) => {
    const mediumKind = xmlValue(node, 'mediumKind', '')
    const type = mediumKind === 'fire' ? 'Fire' : mediumKind === 'fog' ? 'Fog' : xmlValue(node, 'types', 'Other')
    const meshNames = xmlValue(node, 'meshNames', node.getAttribute('objName') || 'object').split(',')
    const spectralNames = xmlValue(node, 'spectralNames', 'soil').split(',')
    const meshSpecularEnabled = xmlValue(node, 'meshSpecularEnabled', '').split(',')
    const thermalNames = xmlValue(node, 'thermalNames', 'soil_temperature').split(',')
    const defaultMaterial = type === 'Vegetation' ? 'leaf_c3' : type === 'Water' ? 'water_set' : 'soilset'
    const materialNames = xmlValue(node, 'bioNames', xmlValue(node, 'propNames', defaultMaterial)).split(',')
    const canopyNames = xmlValue(node, 'canopyNames', type === 'Vegetation' ? 'canopy_default' : 'rigid_body').split(',')
    return {
      name: node.getAttribute('objName') || node.getAttribute('name') || 'object', type,
      shape: xmlValue(node, 'shapeTypes', ''), dimensions: xmlValue(node, 'shapes', '1,1,1').split(',').map(Number),
      fileName: xmlValue(node, 'fileName', xmlValue(node, 'objectfile', '')), positionFile: xmlValue(node, 'objectPosition', ''),
      materialName: materialNames[0] || defaultMaterial,
      canopyName: canopyNames[0] || 'canopy_default',
      meshes: meshNames.map((name, index) => ({
        name,
        ...(meshSpecularEnabled[index] !== undefined && meshSpecularEnabled[index] !== '' ? { specularEnabled: normalizeSpecular({ enabled: meshSpecularEnabled[index] }).enabled } : {}),
        spectralName: spectralNames[index] || spectralNames[0] || 'soil',
        thermalName: thermalNames[index] || thermalNames[0] || 'soil_temperature',
        materialName: materialNames[index] || materialNames[0] || defaultMaterial,
        canopyName: canopyNames[index] || canopyNames[0] || 'canopy_default'
      })),
      ...(mediumKind ? { medium: { kind: mediumKind, radiationOnly: true, coordinateOrder: 'XYZ' }, sourceKind: 'prim', generation: { type: mediumKind === 'fog' ? '雾' : '火焰', shape: xmlValue(node, 'shapeTypes', 'cube') } } : {})
    }
  })
  // 步骤 4：解析光谱预设（PROSPECT/BSM 等模型参数、TIR 反射率/透射率、镜面与物理纹理参数）。
  const spectra = [...doc.querySelectorAll('Attribute Spectral spectral')].map((node) => ({
    name: node.getAttribute('name') || 'spectral', model: node.getAttribute('type') || 'custom',
    reflectance: xmlValue(node, 'reflectance', '0.20'), transmittance: xmlValue(node, 'transmittance', '0.0'),
    refTir: Number(xmlValue(node, 'ref_TIR', 0.05)), tauTir: Number(xmlValue(node, 'tau_TIR', 0)), fileName: xmlValue(node, 'spectral_file', ''),
    refTirWet: xmlValue(node, 'ref_TIR_wet', '') === '' ? undefined : Number(xmlValue(node, 'ref_TIR_wet')),
    specular: normalizeSpecular({ enabled: xmlValue(node, 'specularEnabled', '0'), fraction: xmlValue(node, 'specularFraction', 0.5), roughness: xmlValue(node, 'specularRoughness', 0.25) }),
    physicalTexture: {
      enabled: ['1', 'true', 'yes', 'on'].includes(xmlValue(node, 'physicalTextureEnabled', '0').toLowerCase()),
      fileName: xmlValue(node, 'physicalTextureFile', ''),
      strength: Number(xmlValue(node, 'physicalTextureStrength', 0.2)),
      repeatSize: Number(xmlValue(node, 'physicalTextureRepeatSize', 2))
    },
    params: { Cab: Number(xmlValue(node, 'Cab', 40)), Cw: Number(xmlValue(node, 'Cw', 0.01)), Cdm: Number(xmlValue(node, 'Cdm', 0.01)), Cs: Number(xmlValue(node, 'Cs', 0)), N: Number(xmlValue(node, 'N', 1.5)), SMC: Number(xmlValue(node, 'SMC', 25)), BSMBrightness: Number(xmlValue(node, 'BSMBrightness', 0.5)), BSMlat: Number(xmlValue(node, 'BSMlat', 25)), BSMlon: Number(xmlValue(node, 'BSMlon', 45)) }
  }))
  // 热预设：给定向阳/背阴温度（缺省 305 / 295，单位推测为 K，待核对）。
  const thermals = [...doc.querySelectorAll('Attribute Thermal thermal')].map((node) => ({ name: node.getAttribute('name') || 'temperature', sunlitTemperature: Number(xmlValue(node, 'sunlitTemperature', 305)), shadedTemperature: Number(xmlValue(node, 'shadedTemperature', 295)) }))
  // 材质：叶片 / 土壤(含墙体、船、车辆) / 水体；名为 unused_leaf 的占位叶片不读入。
  const materials = [...doc.querySelectorAll('Attribute Biochemistry leafBio, Attribute Biochemistry soilSet, Attribute Biochemistry waterSet')]
    .filter((node) => node.getAttribute('name') !== 'unused_leaf')
    .map(parseMaterialNode)
  // 结构（canopy）预设：structureType 1/rigid、2/fire、3/fog，其余按 canopy 处理。
  const canopies = [...doc.querySelectorAll('Attribute Canopy canopy')].map((node) => ({
    name: node.getAttribute('name') || 'canopy_default',
    structureType: (() => {
      const value = xmlValue(node, 'structureType', 'canopy').toLowerCase()
      return value === '1' || value === 'rigid' ? 'rigid' : value === '2' || value === 'fire' ? 'fire' : value === '3' || value === 'fog' ? 'fog' : 'canopy'
    })(),
    lai: Number(xmlValue(node, 'lai', 3)), density: Number(xmlValue(node, 'density', 1)),
    hc: Number(xmlValue(node, 'hc', 2)), G: Number(xmlValue(node, 'G', 0.5)),
    LIDFa: Number(xmlValue(node, 'LIDFa', -0.35)), LIDFb: Number(xmlValue(node, 'LIDFb', -0.15)),
    hspot: Number(xmlValue(node, 'hspot', 0.2)), leafwidth: Number(xmlValue(node, 'leafwidth', 0.1)),
    extinction: Number(xmlValue(node, 'extinction', 0)), scatteringAlbedo: Number(xmlValue(node, 'scatteringAlbedo', 0)),
    asymmetry: Number(xmlValue(node, 'asymmetry', 0)), emissionScale: Number(xmlValue(node, 'emissionScale', 0)),
    fixedTemperature: Number(xmlValue(node, 'fixedTemperature', 0))
  }))
  // 步骤 5：组装配置对象。以下各段缺失字段均回退到 base，数值段对越界值限幅。
  return {
    outDir: xmlValue(doc, 'Control outDir', ''),
    scene: {
      x: sceneX,
      y: sceneY,
      height: Number(xmlValue(doc, 'Scene Height', base.scene.height)),
      offsetX: Number(xmlValue(doc, 'Scene offsetX', base.scene.offsetX)),
      offsetY: Number(xmlValue(doc, 'Scene offsetY', base.scene.offsetY)),
      offsetZ: Number(xmlValue(doc, 'Scene offsetZ', base.scene.offsetZ)),
      voxel: Number(xmlValue(doc, 'Scene voxelSize', base.scene.voxel)),
      voxelFillThreshold: Number(xmlValue(doc, 'Scene voxelFillThreshold', base.scene.voxelFillThreshold)),
      terrain: terrainEnabled,
      terrainEnvironment,
      demFile,
      demInfo: null,
      background: {
        spectralName: xmlValue(doc, 'Scene bgSpectral', base.scene.background.spectralName),
        ...(doc.querySelector('Scene backgroundSpecularEnabled') ? {
          specularEnabled: normalizeSpecular({ enabled: xmlValue(doc, 'Scene backgroundSpecularEnabled', '0') }).enabled
        } : {}),
        thermalName: xmlValue(doc, 'Scene bgThermal', base.scene.background.thermalName),
        materialType: xmlValue(doc, 'Scene bgBioType', base.scene.background.materialType),
        materialName: xmlValue(doc, 'Scene bgBioName', base.scene.background.materialName),
        heterogeneityEnabled: xmlValue(doc, 'Scene backgroundHeterogeneity', 'None').toLowerCase() === 'hapke',
        angularEffectStrength: Math.max(0, Math.min(1, Number(xmlValue(
          doc, 'Scene backgroundAngularStrength', base.scene.background.angularEffectStrength
        ))))
      }
    },
    light: {
      zenith: lightPair[0], azimuth: lightPair[1],
      direct: directFraction,
      diffuse: 1 - directFraction,
      skyTemperature: Number(xmlValue(doc, 'Geometry Light light skyTemperature', base.light.skyTemperature)),
      shortwaveIrradiance: Math.max(0, Number(xmlValue(doc, 'Geometry Light light shortwaveIrradiance', base.light.shortwaveIrradiance ?? 0)) || 0),
      // XML 缺该元素（旧工程）= fixed，与引擎一致。
      diffuseFractionMode: String(xmlValue(doc, 'Geometry Light light diffuseFractionMode', 'fixed')).trim() === 'erbs' ? 'erbs' : 'fixed'
    },
    sensor: {
      x: Number(xmlValue(doc, 'Geometry Sensor sensor pixelResolutionX', base.sensor.x)),
      y: Number(xmlValue(doc, 'Geometry Sensor sensor pixelResolutionY', base.sensor.y)),
      bands: xmlValue(continuousNode, 'customBands', xmlValue(doc, 'Geometry Sensor sensor controlBand', base.sensor.bands)),
      continuousBands: continuousNode?.getAttribute('enabled') === '1',
      bandStart: Number(xmlValue(continuousNode, 'start', base.sensor.bandStart)),
      bandEnd: Number(xmlValue(continuousNode, 'end', base.sensor.bandEnd)),
      bandStep: Number(xmlValue(continuousNode, 'step', base.sensor.bandStep)),
      principalPlane: samplingNode?.getAttribute('principalPlane') === '1',
      hemisphere: samplingNode?.getAttribute('hemisphere') === '1',
      projection: ['perspective', 'central', 'center'].includes(projectionText) ? 'perspective' : 'parallel',
      vza: viewPair[0], vaa: viewPair[1],
      positionX: sensorPosition[0], positionY: sensorPosition[1], height: sensorPosition[2],
      fov: Number(xmlValue(doc, 'Geometry Sensor sensor FOV', base.sensor.fov)),
      cruiseEnabled: projectionText === 'perspective' && xmlValue(doc, 'Control isUAVtrave', '0') === '1',
      cruiseHeading: Number(cruiseNode?.getAttribute('heading') ?? base.sensor.cruiseHeading),
      cruiseRoute: cruiseNode?.getAttribute('route') || base.sensor.cruiseRoute,
      cruiseTargetX: Number(cruiseNode?.getAttribute('targetX') ?? base.sensor.cruiseTargetX),
      cruiseTargetY: Number(cruiseNode?.getAttribute('targetY') ?? base.sensor.cruiseTargetY),
      cruiseTargetZ: Number(cruiseNode?.getAttribute('targetZ') ?? sensorPosition[2]),
      cruiseStep: Number(cruiseNode?.getAttribute('step') ?? base.sensor.cruiseStep),
      cruiseCount: Number(cruiseNode?.getAttribute('count') ?? cruiseNode?.querySelectorAll('position').length ?? base.sensor.cruiseCount),
      image: xmlValue(doc, 'Control isImage', '1') === '1',
      temperature: xmlValue(doc, 'Control isTemperature', '1') === '1',
      process: xmlValue(doc, 'Control isProcess', '0') === '1',
      // 兼容旧工程：旧版只有 isProcess。能量平衡模式下它表示能量过程输出，其余模式表示辐射过程输出；
      //   新工程分别写 isRadiationProcess / isEnergyProcess。
      radiationProcess: xmlValue(doc, 'Control isRadiationProcess', energyMode ? '0' : xmlValue(doc, 'Control isProcess', '0')) === '1',
      energyProcess: xmlValue(doc, 'Control isEnergyProcess', energyMode ? xmlValue(doc, 'Control isProcess', '0') : '0') === '1',
      albedo: xmlValue(doc, 'Control isAlbedo', '0') === '1'
    },
    control: {
      // 控制参数段：periodicTraversalCount 限幅 0..20；voxelCollisionOrders 限幅 2..5；
      //   facetBackend / computeBackend 只认 cpu、auto，其余一律回到 gpu；
      //   soilTemperatureMethod 限幅 0..2，缺失时取首个 Soil 材质的 method。
      specularEnabled: xmlValue(doc, 'Control specularReflectionEnabled', '1') === '1',
      depth: Number(xmlValue(doc, 'Control rayTracingDepth', base.control.depth)),
      samples: Number(xmlValue(doc, 'Control sampleCount', base.control.samples)),
      periodicTraversalCount: Math.max(0, Math.min(20, Math.round(Number(
        xmlValue(doc, 'Control periodicNeighborCount', base.control.periodicTraversalCount)
      )))),
      skyboxEnabled: xmlValue(doc, 'Control skyboxEnabled', '0') === '1',
      facetBackend: ['cpu', 'auto'].includes(xmlValue(doc, 'Control facetBackend', 'gpu').toLowerCase())
        ? xmlValue(doc, 'Control facetBackend', 'gpu').toLowerCase() : 'gpu',
      computeBackend: ['cpu', 'auto'].includes(xmlValue(doc, 'Control computeBackend', 'gpu').toLowerCase())
        ? xmlValue(doc, 'Control computeBackend', 'gpu').toLowerCase() : 'gpu',
      cpuThreads: normalizeCpuThreads(xmlValue(doc, 'Control cpuThreads', String(DEFAULT_CPU_THREADS))),
      voxelCollisionOrders: Math.max(2, Math.min(5, Math.round(Number(
        xmlValue(doc, 'Control voxelCollisionOrders', base.control.voxelCollisionOrders)
      )) || 2)),
      radiationSolver: xmlValue(doc, 'Control radiationSolver', base.control.radiationSolver) === 'accelerated'
        ? 'accelerated' : 'traditional',
      gpu: Number(xmlValue(doc, 'Control GPU', base.control.gpu)),
      couplingIterations: Number(xmlValue(doc, 'Control couplingIterations', base.control.couplingIterations)),
      temperatureTolerance: Number(xmlValue(doc, 'Control temperatureTolerance', base.control.temperatureTolerance)),
      temperatureRelaxation: Number(xmlValue(doc, 'Control temperatureRelaxation', base.control.temperatureRelaxation)),
      stabilityCorrection: xmlValue(doc, 'Control stabilityCorrection', '0') === '1',
      soilTemperatureMethod: Math.max(0, Math.min(2, Math.round(Number(xmlValue(
        doc, 'Control soilTemperatureMethod',
        materials.find((item) => item.type === 'Soil')?.params?.method ?? base.control.soilTemperatureMethod
      ))))),
      // 旧 XML 无此标签：保持原 8 节点截断（truncated）。
      soilInertiaHistory: ['1', 'full'].includes(xmlValue(doc, 'Control soilInertiaHistory', '0')) ? 'full' : 'truncated',
      vegetationTemperatureMethod: Math.max(0, Math.min(1, Math.round(Number(xmlValue(
        doc, 'Control vegetationTemperatureMethod', base.control.vegetationTemperatureMethod
      )))))
    },
    objects: { count: objectNodes.length, names: names.length ? names : ['均质地表'], items },
    spectra: spectra.length ? spectra : base.spectra,
    canopies: canopies.length ? canopies : base.canopies,
    thermals: thermals.length ? thermals : base.thermals,
    materials: materials.length ? materials : base.materials,
    meteo: {
      path: xmlValue(doc, 'Meteorology filePath', '未配置'),
      start: Number(xmlValue(doc, 'Meteorology startTimeNode', 0)),
      end: Number(xmlValue(doc, 'Meteorology endTimeNode', 0)),
      z: Number(xmlValue(doc, 'Meteorology z', base.meteo.z)),
      Tsold: Number(xmlValue(doc, 'Meteorology Tsold', base.meteo.Tsold)),
      SatWater: Number(xmlValue(doc, 'Meteorology SatWater', base.meteo.SatWater)),
      dTime: Number(xmlValue(doc, 'Meteorology dTime', base.meteo.dTime)),
      latitude: Number(xmlValue(doc, 'Meteorology Latitude', base.meteo.latitude)),
      longitude: Number(xmlValue(doc, 'Meteorology Longitude', base.meteo.longitude)),
      year: Number(xmlValue(doc, 'Meteorology Year', base.meteo.year)),
      timezone: Number(xmlValue(doc, 'Meteorology TimeZone', base.meteo.timezone))
    },
    // 大气参数：模型/气溶胶不在白名单内时回到 midlatitude-summer / rural；
    //   waterVapor 限幅 0.5..5，visibility 限幅 10..50（单位待核对）。
    atmosphere: {
      enabled: xmlValue(doc, 'Atmosphere enabled', '0') === '1',
      model: ['tropical', 'midlatitude-summer', 'midlatitude-winter'].includes(xmlValue(doc, 'Atmosphere model', 'midlatitude-summer'))
        ? xmlValue(doc, 'Atmosphere model', 'midlatitude-summer') : 'midlatitude-summer',
      waterVapor: Math.max(0.5, Math.min(5, Number(xmlValue(doc, 'Atmosphere waterVapor', 2)))),
      aerosol: ['rural', 'urban'].includes(xmlValue(doc, 'Atmosphere aerosol', 'rural'))
        ? xmlValue(doc, 'Atmosphere aerosol', 'rural') : 'rural',
      visibility: Math.max(10, Math.min(50, Number(xmlValue(doc, 'Atmosphere visibility', 23)))),
      lutFile: xmlValue(doc, 'Atmosphere lutFile', 'assets/atmosphere/simple_modtran_lut.csv')
    }
  }
}

// 作用：标记工程文本已被修改，并递增 editRevision（保存流程据此判断保存期间是否有新编辑）。
function markXmlDirty() {
  state.xmlDirty = true
  state.editRevision += 1
}

// 作用：把当前 state.config / state.mode 写回 state.project，规范化后序列化为 project.json 文本，
//   同步到文本编辑框并显示“有未保存的修改”。副作用：取消待执行的防抖定时器。
function syncProjectText() {
  if (projectTextSyncTimer) { clearTimeout(projectTextSyncTimer); projectTextSyncTimer = null }
  if (!state.project || !state.config) return
  state.project.configuration = state.config
  state.project.mode = state.mode
  state.project = normalizeProject(state.project)
  state.xmlText = stringifyProject(state.project)
  markXmlDirty()
  state.xmlManualEdit = false
  $('#xmlEditor').value = state.xmlText
  $('#unsavedMark').hidden = false
  $('#xmlStatus').textContent = '有未保存的修改'
}

// Re-serialising the whole project (including DEM samples) on every keystroke
// is expensive; live inputs debounce it and saveXml() flushes it first.
let projectTextSyncTimer = null
function scheduleProjectTextSync() {
  markXmlDirty()
  $('#unsavedMark').hidden = false
  if (projectTextSyncTimer) clearTimeout(projectTextSyncTimer)
  projectTextSyncTimer = setTimeout(syncProjectText, 250)
}

function flushProjectTextSync() {
  if (projectTextSyncTimer) syncProjectText()
}

const pendingFramePreviews = new Map()
// 作用：同一 key 的预览刷新在下一帧只执行最后一次提交的 render，避免输入过程中重复重建场景。
function previewOnNextFrame(key, render) {
  if (!pendingFramePreviews.has(key)) requestAnimationFrame(() => {
    const latest = pendingFramePreviews.get(key)
    pendingFramePreviews.delete(key)
    latest()
  })
  pendingFramePreviews.set(key, render)
}

function fileName(path) {
  if (!path) return '未打开工程'
  return path.replaceAll('\\', '/').split('/').pop()
}

// 对象分类：isPrimObject 为程序生成的基元几何（sourceKind=prim 或带 generation）；
//   isMediumObject 为火焰/雾等介质对象。
function isPrimObject(item = {}) {
  return item.sourceKind === 'prim' || Boolean(item.generation)
}

function isMediumObject(item = {}) {
  return item.type === 'Fire' || item.type === 'Fog' || Boolean(item.medium)
}

// 作用：在右下角（toastStack）弹出提示，约 4.2 秒后自动移除；kind 控制样式（如 error）。
function toast(title, message, kind = '') {
  const item = document.createElement('div')
  item.className = `toast ${kind}`
  item.innerHTML = `<strong>${escapeHtml(title)}</strong><span>${escapeHtml(message)}</span>`
  $('#toastStack').append(item)
  setTimeout(() => item.remove(), 4200)
}

// 控制台日志最多保留 2000 行，超出时从最旧的开始删除（见 addLog）。
const MAX_LOG_ROWS = 2000

function addLog(text, kind = '') {
  const lines = String(text).replace(/\r/g, '').split('\n').filter(Boolean)
  for (const line of lines) {
    const row = document.createElement('div')
    row.className = `log-line ${kind}`
    row.innerHTML = `<time>${new Date().toLocaleTimeString('zh-CN', { hour12: false })}</time><span></span>`
    row.querySelector('span').textContent = line
    $('#consoleOutput').append(row)
    state.logCount += 1
  }
  const output = $('#consoleOutput')
  while (output.childElementCount > MAX_LOG_ROWS) output.firstElementChild.remove()
  $('#logCount').textContent = state.logCount
  output.scrollTop = output.scrollHeight
}

// 作用：更新底部状态栏的引擎状态：标题、详情（路径/PID）和状态点样式（ready / running / 空）。
function setEngineState(label, detail, css = 'ready') {
  $('#engineState').textContent = label
  $('#enginePath').textContent = detail
  $('#engineDot').className = css
}

// 作用：把毫秒格式化为 [HH:]MM:SS.t（t 为十分之一秒；不足 1 小时不显示小时）。
function runElapsedText(milliseconds = 0) {
  const seconds = Math.max(0, milliseconds) / 1000
  const hours = Math.floor(seconds / 3600)
  const minutes = Math.floor(seconds / 60) % 60
  const remaining = Math.floor(seconds % 60)
  const tenths = Math.floor((seconds % 1) * 10)
  return `${hours ? `${String(hours).padStart(2, '0')}:` : ''}${String(minutes).padStart(2, '0')}:${String(remaining).padStart(2, '0')}.${tenths}`
}

// 作用：刷新运行进度条（10 个分块，每块 10%）、阶段文字、已运行/耗时与预计剩余时间。
// 输入：finalElapsed 为结束后固定的耗时（毫秒）；为空时按当前时间 - state.startedAt 实时计算。
// 说明：预计剩余时间按引擎自报进度线性外推，仅在进度 5%~100% 且已运行 3 秒以上时显示。
function renderRunProgress(finalElapsed = null) {
  const percent = Math.max(0, Math.min(100, Math.round(Number(state.progress) || 0)))
  const elapsed = finalElapsed == null ? Date.now() - state.startedAt : finalElapsed
  $('#elapsed').textContent = `${state.running ? '已运行' : '耗时'} ${runElapsedText(elapsed)}`
  // While running show the current stage and an ETA extrapolated from the
  // engine's own progress (only once enough progress exists to be stable).
  const eta = state.running && percent >= 5 && percent < 100 && elapsed > 3000
    ? ` · 预计剩余 ${runElapsedText(elapsed * (100 - percent) / percent).replace(/\.\d$/, '')}` : ''
  $('#runProgressText').textContent = state.running
    ? `${state.progressStage || '运行中'}${eta}` : (percent >= 100 ? '已完成' : state.progressStage)
  $('#runProgressText').title = $('#runProgressText').textContent
  $('#runProgressPercent').textContent = `${percent}%`
  $$('#runProgress .run-progress-blocks i').forEach((block, index) => {
    const fill = Math.max(0, Math.min(100, (percent - index * 10) * 10))
    block.style.setProperty('--block-progress', `${fill}%`)
  })
  $('#runProgress').classList.toggle('running', state.running)
  $('#runProgress').classList.toggle('complete', !state.running && percent >= 100)
  if (state.running) {
    $('#engineState').textContent = `模拟运行中 · ${percent}%`
    $('#enginePath').textContent = `PID ${state.pid || '...'} · ${state.progressStage} · ${runElapsedText(elapsed)}`
  }
}

// 作用：更新进度百分比与阶段名。进度只增不减（取已有进度与新值的较大者），上限 100。
function setSimulationProgress(percent, stage = state.progressStage) {
  state.progress = Math.max(state.progress, Math.min(100, Math.max(0, Number(percent) || 0)))
  state.progressStage = stage || state.progressStage
  renderRunProgress()
}

// 作用：切换“运行中/待命”状态：运行中禁用运行按钮、模式切换、绘制/编辑工具与计算设备选项，
//   并关闭正在进行的几何绘制和对象编辑；待命时恢复引擎状态栏（Facet 模式显示 Facet 待命）。
function setRunning(running) {
  state.running = running
  $('#runBtn').disabled = running
  $('#stopBtn').disabled = !running
  $$('#modeGrid button').forEach((button) => { button.disabled = running })
  $('#drawRegionBtn').disabled = running
  $('#drawBoxBtn').disabled = running
  $('#editObjectBtn').disabled = running
  $('#sceneObjectToolsBtn').disabled = running
  $('#sceneObjectModifyBtn').disabled = running
  $('#sceneObjectAddBtn').disabled = running
  $('#computeDevice').disabled = running
  $('#cpuThreads').disabled = running
  $('#refreshDevicesBtn').disabled = running
  if (running) closeSceneObjectToolsMenu()
  if (running && geometryDraw.mode) cancelGeometryDraw()
  if (running && objectEdit.enabled) setObjectEditEnabled(false)
  if (running) {
    setEngineState('模拟运行中', `PID ${state.pid || '...'}`, 'running')
    renderRunProgress()
  }
  else {
    const facet = state.mode === 'eFacetRT' || state.mode === 'eFacetEB'
    const exists = state.executableExists
    const path = state.executable
    setEngineState(exists ? (facet ? 'HiStream Facet 待命' : 'HiStream 待命') : '未找到引擎', path, exists ? 'ready' : '')
  }
}

// Three.js 场景
// 模块：Three.js 场景、相机、渲染器与全局三维对象。
// 约定：三维预览以场景中心为原点；北向（config.scene.x 方向）对应 Three 的 x 轴，
//   东向（config.scene.y 方向）对应 z 轴，y 轴向上；world.userData.displayScale 为
//   “场景米 → 预览单位”的缩放（见 generateWorld）。
const viewport = $('#viewport')
const scene = new THREE.Scene()
const defaultSceneBackground = new THREE.Color(0xf3f6fa)
scene.background = defaultSceneBackground
// Physical scene objects must not fade into the background as camera distance
// increases; distance fog was visually equivalent to extra transmittance.
scene.fog = null
const camera = new THREE.PerspectiveCamera(42, 1, 0.1, 1200)
camera.position.set(25, 22, 28)
const renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: 'high-performance' })
renderer.setPixelRatio(Math.min(devicePixelRatio, 2))
renderer.shadowMap.enabled = true
renderer.shadowMap.type = THREE.PCFShadowMap
renderer.outputColorSpace = THREE.SRGBColorSpace
renderer.toneMapping = THREE.ACESFilmicToneMapping
renderer.toneMappingExposure = 1.05
viewport.append(renderer.domElement)
const controls = new OrbitControls(camera, renderer.domElement)
controls.enableDamping = true
controls.dampingFactor = 0.075
controls.maxPolarAngle = Math.PI * 0.48
controls.minDistance = 4
controls.maxDistance = 320
const transformControls = new TransformControls(camera, renderer.domElement)
transformControls.setSize(.82)
transformControls.space = 'world'
scene.add(transformControls.getHelper())
scene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.65))
const sun = new THREE.DirectionalLight(0xfff7e8, 3.2)
sun.castShadow = true
sun.shadow.mapSize.set(2048, 2048)
sun.shadow.camera.left = sun.shadow.camera.bottom = -70
sun.shadow.camera.right = sun.shadow.camera.top = 70
sun.shadow.bias = -0.0005
scene.add(sun)
const world = new THREE.Group()
scene.add(world)
// 以下为预览中可被重建/释放的对象句柄：地面网格、体元预览、场景边界引导、偏移预览定时器、
//   巡航可视化、太阳可视化、当前显示风格、结果查看器与典型天空盒贴图。
let grid = null
let voxelPreview = null
let sceneBoundsGuide = null
let sceneOffsetPreviewTimer = null
let cruiseVisualization = null
let sunVisualization = null
let currentStyle = 'solid'
let resultViewer = null
let typicalSkyboxTexture = null
// 状态：视口内“绘制区域 / 绘制立方体”交互。phase：idle → ready（已选工具）→ base（拖拽底面）
//   → height（仅立方体，移动鼠标定高度）；start/current 为地面点，height 为当前高度（米）。
const geometryDraw = {
  mode: null,
  phase: 'idle',
  start: null,
  current: null,
  height: 0,
  initialHeight: 0,
  heightAnchorY: 0,
  pointerId: null,
  preview: null
}
const geometryDrawRaycaster = new THREE.Raycaster()
const geometryDrawPointer = new THREE.Vector2()
const objectEditRaycaster = new THREE.Raycaster()
const objectEditPointer = new THREE.Vector2()
// 状态：视口内对象选取与变换编辑（TransformControls）。selected 为选中的三维对象，
//   pointerStart 用于区分点击与拖动，saving/savePending 用于串行化保存变换。
const objectEdit = {
  enabled: false,
  selected: null,
  selectionBox: null,
  pointerStart: null,
  transformStartScale: new THREE.Vector3(),
  saving: false,
  savePending: false
}

// 作用：在视口上显示/隐藏绘制提示文字（空串则隐藏）。
function geometryDrawHint(text = '') {
  const hint = $('#viewportDrawHint')
  hint.hidden = !text
  hint.textContent = text
}

// 作用：关闭“场景对象工具”菜单及其“修改/添加”子菜单，并复位 aria-expanded 与 open 样式。
function closeSceneObjectToolsMenu() {
  const menu = $('#sceneObjectToolsMenu')
  const button = $('#sceneObjectToolsBtn')
  if (menu) menu.hidden = true
  button?.setAttribute('aria-expanded', 'false')
  for (const [triggerId, menuId] of [['sceneObjectModifyBtn', 'sceneObjectModifyMenu'], ['sceneObjectAddBtn', 'sceneObjectAddMenu']]) {
    const trigger = $(`#${triggerId}`)
    const submenu = $(`#${menuId}`)
    if (submenu) submenu.hidden = true
    trigger?.classList.remove('open')
    trigger?.setAttribute('aria-expanded', 'false')
  }
}

// 作用：展开/收起“修改”或“添加”子菜单（kind = Modify / Add），同一时刻只展开一个。
function toggleSceneObjectSubmenu(kind) {
  const selectedTrigger = $(`#sceneObject${kind}Btn`)
  const selectedMenu = $(`#sceneObject${kind}Menu`)
  const opening = selectedMenu.hidden
  for (const [triggerId, menuId] of [['sceneObjectModifyBtn', 'sceneObjectModifyMenu'], ['sceneObjectAddBtn', 'sceneObjectAddMenu']]) {
    const trigger = $(`#${triggerId}`)
    const submenu = $(`#${menuId}`)
    const active = opening && trigger === selectedTrigger
    submenu.hidden = !active
    trigger.classList.toggle('open', active)
    trigger.setAttribute('aria-expanded', String(active))
  }
}

function updateSceneObjectToolsButton() {
  $('#sceneObjectToolsBtn')?.classList.toggle('active', Boolean(objectEdit.enabled || geometryDraw.mode))
}

// 作用：移除并释放绘制过程中的半透明预览盒。
function clearGeometryDrawPreview() {
  if (!geometryDraw.preview) return
  scene.remove(geometryDraw.preview)
  disposeObject(geometryDraw.preview)
  geometryDraw.preview = null
}

// 作用：由拖拽起点/当前点求绘制矩形的尺寸与中心。
// 输出：sizeX/sizeZ（米，北向/东向边长）、centerX/centerZ（场景坐标，含 offset）、
//   worldX/worldZ/worldY（预览坐标，worldY 取两点较高者）；尚未拖出时返回 null。
function geometryDrawSize() {
  if (!geometryDraw.start || !geometryDraw.current) return null
  return {
    sizeX: Math.abs(geometryDraw.current.north - geometryDraw.start.north),
    sizeZ: Math.abs(geometryDraw.current.east - geometryDraw.start.east),
    centerX: (geometryDraw.current.north + geometryDraw.start.north) / 2,
    centerZ: (geometryDraw.current.east + geometryDraw.start.east) / 2,
    worldX: (geometryDraw.current.worldX + geometryDraw.start.worldX) / 2,
    worldZ: (geometryDraw.current.worldZ + geometryDraw.start.worldZ) / 2,
    worldY: Math.max(geometryDraw.current.worldY, geometryDraw.start.worldY)
  }
}

// 作用：按当前拖拽范围（和立方体高度）重建半透明预览盒与棱线，并更新尺寸提示。
// 说明：区域模式只画很薄的一层；立方体模式使用 geometryDraw.height；
//   预览单位由 displayScale 缩放，提示中的尺寸仍为米。
function renderGeometryDrawPreview() {
  clearGeometryDrawPreview()
  const size = geometryDrawSize()
  if (!size || size.sizeX <= 0 || size.sizeZ <= 0) return
  const displayScale = Number(world.userData.displayScale) || 1
  const width = size.sizeX * displayScale
  const depth = size.sizeZ * displayScale
  const height = geometryDraw.mode === 'box' ? Math.max(.02, geometryDraw.height * displayScale) : Math.max(.018, displayScale * .04)
  const geometry = new THREE.BoxGeometry(width, height, depth)
  const group = new THREE.Group()
  const fill = new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({
    color: geometryDraw.mode === 'box' ? 0x2f81d4 : 0x2ba66d,
    transparent: true, opacity: .24, depthTest: false, depthWrite: false
  }))
  const edges = new THREE.LineSegments(new THREE.EdgesGeometry(geometry), new THREE.LineBasicMaterial({
    color: geometryDraw.mode === 'box' ? 0x1768c4 : 0x16845a,
    transparent: true, opacity: .96, depthTest: false, depthWrite: false
  }))
  fill.renderOrder = 80
  edges.renderOrder = 81
  group.add(fill, edges)
  group.position.set(size.worldX, size.worldY + height / 2 + .025, size.worldZ)
  geometryDraw.preview = group
  scene.add(group)
  const dimensions = geometryDraw.mode === 'box'
    ? `${size.sizeX.toFixed(2)} × ${geometryDraw.height.toFixed(2)} × ${size.sizeZ.toFixed(2)} m`
    : `${size.sizeX.toFixed(2)} × ${size.sizeZ.toFixed(2)} m`
  geometryDrawHint(geometryDraw.phase === 'height'
    ? `尺寸 ${dimensions}；移动鼠标调整高度，单击确认，Esc 或右键取消`
    : `尺寸 ${dimensions}；拖动确定范围，Esc 或右键取消`)
}

// 作用：退出绘制模式，恢复轨道相机控制与按钮状态，可选弹出提示 message。
function cancelGeometryDraw(message = '') {
  clearGeometryDrawPreview()
  geometryDraw.mode = null
  geometryDraw.phase = 'idle'
  geometryDraw.start = null
  geometryDraw.current = null
  geometryDraw.pointerId = null
  controls.enabled = true
  $('.viewport-panel')?.classList.remove('drawing-geometry')
  $('#drawRegionBtn')?.classList.remove('active')
  $('#drawBoxBtn')?.classList.remove('active')
  updateSceneObjectToolsButton()
  geometryDrawHint('')
  if (message) toast('场景绘制', message)
}

// 作用：进入绘制模式 mode（'region' 地面矩形 / 'box' 立方体）。再次点同一工具则取消。
// 说明：需已打开工程且未在运行；绘制期间禁用相机旋转以免与拖拽冲突，并关闭对象编辑。
function startGeometryDraw(mode) {
  if (!state.inputPath) { toast('无法绘制对象', '请先新建或打开工程', 'error'); return }
  if (state.running) { toast('无法绘制对象', '请先停止当前模拟', 'error'); return }
  if (geometryDraw.mode === mode) { cancelGeometryDraw(); return }
  if (objectEdit.enabled) setObjectEditEnabled(false)
  cancelGeometryDraw()
  closeSceneObjectToolsMenu()
  geometryDraw.mode = mode
  geometryDraw.phase = 'ready'
  controls.enabled = false
  $('.viewport-panel')?.classList.add('drawing-geometry')
  $(`#${mode === 'box' ? 'drawBoxBtn' : 'drawRegionBtn'}`)?.classList.add('active')
  updateSceneObjectToolsButton()
  geometryDrawHint(mode === 'box'
    ? '在地面拖出立方体底面；松开后移动鼠标设置高度，再单击确认'
    : '在地面按住左键拖出矩形区域；松开后直接生成 OBJ')
}

// 作用：把鼠标位置射线投到地面，返回吸附到体元网格的地面点。
// 输出：north/east 为场景坐标（含 offsetX/offsetZ，米），worldX/worldZ/worldY 为预览坐标；
//   未命中地面返回 null。
// 说明：吸附步长取 scene.voxel（至少 0.01 m），并限制在场景范围 [0, scene.x/y] 内。
function geometryPointFromPointer(event) {
  const bounds = renderer.domElement.getBoundingClientRect()
  if (!bounds.width || !bounds.height) return null
  geometryDrawPointer.set(
    (event.clientX - bounds.left) / bounds.width * 2 - 1,
    -(event.clientY - bounds.top) / bounds.height * 2 + 1
  )
  geometryDrawRaycaster.setFromCamera(geometryDrawPointer, camera)
  const ground = world.children.find((child) => child.userData.kind === 'ground')
  const hit = ground ? geometryDrawRaycaster.intersectObject(ground, false)[0] : null
  if (!hit) return null
  const config = state.config
  const scale = Number(world.userData.displayScale) || 1
  const sceneX = Math.max(.1, Number(config.scene.x) || 60)
  const sceneZ = Math.max(.1, Number(config.scene.y) || 60)
  const offsetX = Number(config.scene.offsetX) || 0
  const offsetZ = Number(config.scene.offsetZ) || 0
  const snap = Math.max(.01, Number(config.scene.voxel) || 1)
  const snapWithin = (value, maximum) => THREE.MathUtils.clamp(Math.round(value / snap) * snap, 0, maximum)
  const localNorth = snapWithin(hit.point.x / scale + sceneX / 2, sceneX)
  const localEast = snapWithin(hit.point.z / scale + sceneZ / 2, sceneZ)
  return {
    north: localNorth + offsetX,
    east: localEast + offsetZ,
    worldX: (localNorth - sceneX / 2) * scale,
    worldZ: (localEast - sceneZ / 2) * scale,
    worldY: hit.point.y
  }
}

// 作用：绘制完成：把范围/高度填入“几何生成”表单并直接提交。
//   box 模式生成立方体表面几何（surface），位置记入 state.drawPlacement；
//   region 模式生成一层水体几何（water），位置/偏移填入水体表单项。
// 说明：范围过小（边长 ≤ 0）时提示重新拖动。
function finishGeometryDraw() {
  const size = geometryDrawSize()
  const mode = geometryDraw.mode
  if (!size || size.sizeX <= 0 || size.sizeZ <= 0) {
    clearGeometryDrawPreview()
    geometryDraw.phase = 'ready'
    geometryDrawHint('绘制范围太小，请重新拖动')
    return
  }
  const form = $('#geometryForm')
  const index = (state.config?.objects?.items?.length || 0) + 1
  const verticalOffset = Number(state.config?.scene?.offsetY) || 0
  form.reset()
  form.elements.geometryName.value = `${mode === 'box' ? 'draw_box' : 'draw_region'}_${index}`
  form.elements.geometryShape.value = 'cube'
  form.elements.geometryMode.value = mode === 'box' ? 'surface' : 'water'
  form.elements.geometrySizeX.value = size.sizeX
  form.elements.geometrySizeY.value = mode === 'box' ? geometryDraw.height : Math.max(.01, Number(state.config?.scene?.voxel) || 1)
  form.elements.geometrySizeZ.value = size.sizeZ
  form.elements.geometryDetail.value = 1
  if (mode === 'box') {
    state.drawPlacement = { north: size.centerX, east: size.centerZ, height: verticalOffset }
  } else {
    form.elements.waterPositionX.value = size.centerX
    form.elements.waterPositionY.value = size.centerZ
    form.elements.waterPositionZ.value = verticalOffset
    form.elements.waterOffsetX.value = 0
    form.elements.waterOffsetY.value = 0
    form.elements.waterOffsetZ.value = 0
  }
  cancelGeometryDraw()
  updateGeometryDialog()
  form.requestSubmit()
}

// 作用：绘制交互的指针事件。按下（左键）开始拖拽底面，立方体在 height 阶段再次按下即确认；
//   移动更新范围或高度；抬起结束底面：region 直接完成，box 进入定高阶段。
function onGeometryDrawPointerDown(event) {
  if (!geometryDraw.mode || event.button !== 0) return
  event.preventDefault()
  event.stopPropagation()
  if (geometryDraw.phase === 'height') { finishGeometryDraw(); return }
  const point = geometryPointFromPointer(event)
  if (!point) return
  geometryDraw.phase = 'base'
  geometryDraw.start = point
  geometryDraw.current = point
  geometryDraw.pointerId = event.pointerId
  renderer.domElement.setPointerCapture?.(event.pointerId)
  renderGeometryDrawPreview()
}

// 定高阶段：鼠标每向上移动一个像素，高度按 sceneHeight / max(120, 视口高度) 增加，
//   再吸附到体元步长并限制在 [snap, scene.height]（height 缺省 20 m）。
function onGeometryDrawPointerMove(event) {
  if (!geometryDraw.mode) return
  if (geometryDraw.phase === 'height') {
    const sceneHeight = Math.max(.1, Number(state.config?.scene?.height) || 20)
    const snap = Math.max(.01, Number(state.config?.scene?.voxel) || 1)
    const raw = geometryDraw.initialHeight + (geometryDraw.heightAnchorY - event.clientY) * sceneHeight / Math.max(120, viewport.clientHeight)
    geometryDraw.height = THREE.MathUtils.clamp(Math.max(snap, Math.round(raw / snap) * snap), snap, sceneHeight)
    renderGeometryDrawPreview()
    return
  }
  if (geometryDraw.phase !== 'base' || geometryDraw.pointerId !== event.pointerId) return
  const point = geometryPointFromPointer(event)
  if (!point) return
  geometryDraw.current = point
  renderGeometryDrawPreview()
}

// 初始高度取底面较长边的一半，同样吸附到体元步长并限制在 [snap, scene.height]。
function onGeometryDrawPointerUp(event) {
  if (!geometryDraw.mode || geometryDraw.phase !== 'base' || geometryDraw.pointerId !== event.pointerId) return
  event.preventDefault()
  event.stopPropagation()
  renderer.domElement.releasePointerCapture?.(event.pointerId)
  geometryDraw.pointerId = null
  const point = geometryPointFromPointer(event)
  if (point) geometryDraw.current = point
  const size = geometryDrawSize()
  if (!size || size.sizeX <= 0 || size.sizeZ <= 0) {
    clearGeometryDrawPreview()
    geometryDraw.phase = 'ready'
    geometryDrawHint('绘制范围太小，请重新拖动')
    return
  }
  if (geometryDraw.mode === 'region') { finishGeometryDraw(); return }
  const snap = Math.max(.01, Number(state.config?.scene?.voxel) || 1)
  const sceneHeight = Math.max(snap, Number(state.config?.scene?.height) || 20)
  geometryDraw.initialHeight = THREE.MathUtils.clamp(Math.max(snap, Math.max(size.sizeX, size.sizeZ) * .5), snap, sceneHeight)
  geometryDraw.height = Math.max(snap, Math.round(geometryDraw.initialHeight / snap) * snap)
  geometryDraw.heightAnchorY = event.clientY
  geometryDraw.phase = 'height'
  renderGeometryDrawPreview()
}

// 模块：对象选取与变换编辑（objectEdit）。
//   updateObjectSelectionBox 刷新选择框；clearObjectSelection 取消选择并卸下 TransformControls；
//   setObjectTransformMode 切换移动/旋转/缩放；setObjectEditEnabled 开关编辑模式；
//   objectAtPointer 用射线找到点击处的工程对象；selectObjectForEditing 选中并显示手柄。
function updateObjectSelectionBox() {
  if (!objectEdit.selectionBox || !objectEdit.selected) return
  objectEdit.selectionBox.setFromObject(objectEdit.selected)
}

function clearObjectSelection(showHint = true) {
  transformControls.detach()
  if (objectEdit.selectionBox) {
    scene.remove(objectEdit.selectionBox)
    objectEdit.selectionBox.geometry?.dispose?.()
    objectEdit.selectionBox.material?.dispose?.()
  }
  objectEdit.selected = null
  objectEdit.selectionBox = null
  if (objectEdit.enabled && showHint) geometryDrawHint('单击选择 OBJ；双击修改物化属性；W/E/R 切换移动、旋转、缩放')
}

// 作用：切换变换手柄模式。旋转模式只显示 Y 轴手柄（对象只绕竖直轴旋转，与 editedPlacement 一致）。
function setObjectTransformMode(mode) {
  if (!['translate', 'rotate', 'scale'].includes(mode)) return
  transformControls.setMode(mode)
  transformControls.showX = mode !== 'rotate'
  transformControls.showY = true
  transformControls.showZ = mode !== 'rotate'
  $$('#transformModeButtons button').forEach((button) => button.classList.toggle('active', button.dataset.transformMode === mode))
}

// 作用：开启/关闭对象编辑模式。开启前要求已打开工程且未运行；开启时先取消几何绘制，
//   关闭时清除选择。会同步按钮、视口样式与提示文字。
function setObjectEditEnabled(enabled) {
  if (enabled && !state.inputPath) { toast('无法编辑 OBJ', '请先新建或打开工程', 'error'); return }
  if (enabled && state.running) { toast('无法编辑 OBJ', '请先停止当前模拟', 'error'); return }
  objectEdit.enabled = Boolean(enabled)
  if (objectEdit.enabled) {
    if (geometryDraw.mode) cancelGeometryDraw()
    setObjectTransformMode(transformControls.mode || 'translate')
    geometryDrawHint('单击选择 OBJ；双击修改物化属性；W/E/R 切换移动、旋转、缩放')
  } else {
    clearObjectSelection(false)
    geometryDrawHint('')
  }
  $('#editObjectBtn')?.classList.toggle('active', objectEdit.enabled)
  $('.viewport-panel')?.classList.toggle('editing-object', objectEdit.enabled)
  closeSceneObjectToolsMenu()
  updateSceneObjectToolsButton()
}

// 作用：由射线交点沿父节点向上，找到带 userData.projectObject 标记的对象根（world 的直接子级）。
function projectObjectFromIntersection(intersection) {
  let object = intersection?.object || null
  while (object && object !== world) {
    if (object.userData.projectObject) return object
    object = object.parent
  }
  return null
}

function objectAtPointer(event) {
  const bounds = renderer.domElement.getBoundingClientRect()
  if (!bounds.width || !bounds.height) return null
  objectEditPointer.set(
    (event.clientX - bounds.left) / bounds.width * 2 - 1,
    -(event.clientY - bounds.top) / bounds.height * 2 + 1
  )
  objectEditRaycaster.setFromCamera(objectEditPointer, camera)
  const roots = world.children.filter((child) => child.userData.projectObject)
  for (const intersection of objectEditRaycaster.intersectObjects(roots, true)) {
    const object = projectObjectFromIntersection(intersection)
    if (object) return object
  }
  return null
}

// 作用：选中一个工程对象实例：加高亮 BoxHelper 与 TransformControls，记录 selectedObjectIndex。
// 说明：启用了随机移动（movement.enabled）的对象位置由运动轨迹决定，不允许手动拖动。
function selectObjectForEditing(object) {
  if (!object) { clearObjectSelection(); return }
  const itemIndex = Number(object.userData.projectObjectIndex)
  const item = state.config?.objects?.items?.[itemIndex]
  if (!item) return
  if (item.movement?.enabled) {
    toast('暂时不能移动该 OBJ', '请先在对象设置中关闭随机移动', 'error')
    return
  }
  clearObjectSelection(false)
  objectEdit.selected = object
  objectEdit.selectionBox = new THREE.BoxHelper(object, 0x1768c4)
  objectEdit.selectionBox.material.depthTest = false
  objectEdit.selectionBox.material.transparent = true
  objectEdit.selectionBox.material.opacity = .9
  objectEdit.selectionBox.renderOrder = 90
  scene.add(objectEdit.selectionBox)
  transformControls.attach(object)
  state.selectedObjectIndex = itemIndex
  const placementIndex = Number(object.userData.projectPlacementIndex) + 1
  geometryDrawHint(`${item.name} · 第 ${placementIndex} 个实例；拖动手柄编辑，双击修改物化属性`)
}

// 作用：在 DEM 地形上双线性插值，返回 (north, east) 处相对 DEM 最低值的高度（米，≥ 0）。
// 输入：north/east 为场景局部坐标（不含 offset）；config.scene.demInfo 提供 terrainWidth/
//   terrainHeight/terrainValues（按行存储）和 minimum。
// 说明：地形未启用或采样数与尺寸不符时返回 0；图像行方向与北向相反（v = 1 - north/x），
//   缺失/非有限采样值用 minimum 代替（待核对：DEM 采样行列方向约定）。
function sceneTerrainHeight(config, north, east) {
  if (!config?.scene?.terrain) return 0
  const dem = config.scene.demInfo
  const width = Math.max(2, Math.round(Number(dem?.terrainWidth) || 0))
  const height = Math.max(2, Math.round(Number(dem?.terrainHeight) || 0))
  const values = Array.isArray(dem?.terrainValues) ? dem.terrainValues : []
  if (values.length !== width * height) return 0
  const u = THREE.MathUtils.clamp(Number(east) / Math.max(1, Number(config.scene.y)), 0, 1) * (width - 1)
  const v = (1 - THREE.MathUtils.clamp(Number(north) / Math.max(1, Number(config.scene.x)), 0, 1)) * (height - 1)
  const x0 = Math.floor(u), y0 = Math.floor(v)
  const x1 = Math.min(width - 1, x0 + 1), y1 = Math.min(height - 1, y0 + 1)
  const fallback = Number.isFinite(Number(dem.minimum)) ? Number(dem.minimum) : 0
  const sample = (column, row) => {
    const value = Number(values[row * width + column])
    return Number.isFinite(value) ? value : fallback
  }
  const top = THREE.MathUtils.lerp(sample(x0, y0), sample(x1, y0), u - x0)
  const bottom = THREE.MathUtils.lerp(sample(x0, y1), sample(x1, y1), u - x0)
  return Math.max(0, THREE.MathUtils.lerp(top, bottom, v - y0) - fallback)
}

// 作用：把拖动后的三维对象位姿换算回工程中的实例位置记录。
// 输出：x/y（北/东，含 offsetX/offsetZ）、z（离地高度，含 offsetY）、scale、rotation（度，
//   取 [0, 360)，Three 的 rotation.y 取负号后得到），均保留 6 位小数。
// 说明：缩放因子由对象当前缩放与初始缩放之比得到（下限 0.01）；若对象贴地（有
//   projectGroundFootprint）则按足迹重新求贴地高度，否则按对象 y 减去竖向锚点与地形高度。
function editedPlacement(object) {
  const config = state.config
  const displayScale = Number(world.userData.displayScale) || 1
  const sceneX = Math.max(.1, Number(config.scene.x) || 60)
  const sceneY = Math.max(.1, Number(config.scene.y) || 60)
  const localNorth = object.position.x / displayScale + sceneX / 2
  const localEast = object.position.z / displayScale + sceneY / 2
  const initialScale = object.userData.projectInitialScale || object.scale
  const scaleFactor = Math.max(.01, object.scale.x / Math.max(1e-9, initialScale.x))
  const original = object.userData.projectPlacement || { scale: 1 }
  const verticalAnchor = (Number(object.userData.projectVerticalAnchor) || 0) * scaleFactor
  const round = (value) => Number(Number(value).toFixed(6))
  let rotation = -THREE.MathUtils.radToDeg(object.rotation.y)
  rotation = ((rotation % 360) + 360) % 360
  const scale = Math.max(.01, (Number(original.scale) || 1) * scaleFactor)
  const footprint = object.userData.projectGroundFootprint
  const height = footprint
    ? groundedPlacementHeight(footprint, { north: localNorth, east: localEast, scale, rotation }, (north, east) => sceneTerrainHeight(config, north, east))
    : (object.position.y - verticalAnchor) / displayScale - sceneTerrainHeight(config, localNorth, localEast)
  return {
    x: round(localNorth + (Number(config.scene.offsetX) || 0)),
    y: round(localEast + (Number(config.scene.offsetZ) || 0)),
    z: round(height + (Number(config.scene.offsetY) || 0)),
    scale: round(scale),
    rotation: round(rotation)
  }
}

// 作用：贴地对象（有足迹）在水平拖动/缩放后，按新位置的地形高度把 y 重新吸附到地面。
function snapGroundedObject(object) {
  if (!object?.userData?.projectGroundFootprint) return
  const config = state.config
  const displayScale = Number(world.userData.displayScale) || 1
  const placement = editedPlacement(object)
  const north = object.position.x / displayScale + Math.max(.1, Number(config.scene.x) || 60) / 2
  const east = object.position.z / displayScale + Math.max(.1, Number(config.scene.y) || 60) / 2
  object.position.y = (sceneTerrainHeight(config, north, east) + placement.z - (Number(config.scene.offsetY) || 0)) * displayScale
}

// 作用：把拖动编辑后的实例位姿保存到工程。
// 步骤：1 读出该对象现有的实例位置表（positionFile 或由 distribution 生成）；
//   2 用 editedPlacement 覆盖对应实例；3 经 api.saveDistribution 写回位置文件，并把分布改为
//   manual；4 更新对象的初始缩放/竖向锚点缓存，标脏并调用 saveXml 保存工程。
// 说明：saving/savePending 保证同时只有一次保存，保存期间又有编辑则结束后再保存一次。
async function saveEditedObjectTransform(object) {
  if (!object?.userData?.projectObject || objectEdit.saving) {
    if (objectEdit.saving) objectEdit.savePending = true
    return
  }
  const itemIndex = Number(object.userData.projectObjectIndex)
  const placementIndex = Number(object.userData.projectPlacementIndex)
  const item = state.config?.objects?.items?.[itemIndex]
  if (!item || !Number.isInteger(placementIndex)) return
  objectEdit.saving = true
  try {
    let points
    if (item.positionFile) points = parseDistributionPointsText((await api.readText(item.positionFile)).content)
    else points = generateDistribution(item.distribution || { ...defaultDistribution(item), mode: 'single' })
    if (!points[placementIndex]) throw new Error(`找不到第 ${placementIndex + 1} 个实例的位置记录`)
    const placement = editedPlacement(object)
    points[placementIndex] = placement
    const saved = await api.saveDistribution({ path: item.positionFile, name: item.name, instances: points })
    item.positionFile = saved.path
    item.distribution = {
      ...defaultDistribution(item), mode: 'manual', basis: 'count', count: points.length,
      points: distributionPointsText(points), offsetX: 0, offsetY: 0, offsetZ: 0
    }
    item.instanceCount = points.length
    object.userData.projectPlacement = { ...placement }
    object.userData.projectInitialScale = object.scale.clone()
    object.userData.projectVerticalAnchor = (Number(object.userData.projectVerticalAnchor) || 0) * Math.max(.01, object.scale.x / Math.max(1e-9, objectEdit.transformStartScale.x || object.scale.x))
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw state.lastSaveError || new Error('工程配置保存失败')
    addLog(`OBJ 实例位置已更新：${item.name} #${placementIndex + 1}`, 'success')
  } catch (error) {
    toast('OBJ 位置保存失败', error.message, 'error')
    addLog(`OBJ 位置保存失败：${error.message}`, 'error')
  } finally {
    objectEdit.saving = false
    if (objectEdit.savePending) {
      objectEdit.savePending = false
      saveEditedObjectTransform(objectEdit.selected)
    }
  }
}

// 作用：对象编辑的指针事件。按下记录起点；抬起时若位移 ≤ 5 像素视为点击并选中，
//   否则视为相机拖动；双击则选中并打开对象物化属性对话框。
function onObjectEditPointerDown(event) {
  if (!objectEdit.enabled || geometryDraw.mode || event.button !== 0 || transformControls.axis || transformControls.dragging) return
  objectEdit.pointerStart = { x: event.clientX, y: event.clientY }
}

function onObjectEditPointerUp(event) {
  if (!objectEdit.enabled || geometryDraw.mode || !objectEdit.pointerStart || transformControls.dragging || transformControls.axis) return
  const distance = Math.hypot(event.clientX - objectEdit.pointerStart.x, event.clientY - objectEdit.pointerStart.y)
  objectEdit.pointerStart = null
  if (distance <= 5) selectObjectForEditing(objectAtPointer(event))
}

function onObjectEditDoubleClick(event) {
  if (!objectEdit.enabled || geometryDraw.mode) return
  const object = objectAtPointer(event)
  if (!object) return
  selectObjectForEditing(object)
  showObjectAttributeDialog(Number(object.userData.projectObjectIndex))
}

// 作用：用 2D canvas 程序化生成 1024×512 的“典型天空”等距柱状贴图（蓝色渐变 + 若干云团），
//   仅用于预览背景，与辐射计算无关。
function createTypicalSkyboxTexture() {
  const canvas = document.createElement('canvas')
  canvas.width = 1024
  canvas.height = 512
  const context = canvas.getContext('2d')
  const sky = context.createLinearGradient(0, 0, 0, canvas.height)
  sky.addColorStop(0, '#1769b0')
  sky.addColorStop(0.48, '#65b9ed')
  sky.addColorStop(0.7, '#d7effb')
  sky.addColorStop(1, '#eef5f6')
  context.fillStyle = sky
  context.fillRect(0, 0, canvas.width, canvas.height)

  const cloudBands = [
    [90, 185, 145, 34], [285, 145, 190, 43], [535, 205, 165, 38],
    [760, 130, 205, 46], [975, 210, 145, 34], [420, 260, 125, 27]
  ]
  for (const [x, y, width, height] of cloudBands) {
    const glow = context.createRadialGradient(x, y, 2, x, y, width * 0.58)
    glow.addColorStop(0, 'rgba(255,255,255,0.92)')
    glow.addColorStop(0.55, 'rgba(250,253,255,0.72)')
    glow.addColorStop(1, 'rgba(235,244,249,0)')
    context.fillStyle = glow
    context.beginPath()
    context.ellipse(x, y, width, height, 0, 0, Math.PI * 2)
    context.fill()
  }
  const texture = new THREE.CanvasTexture(canvas)
  texture.mapping = THREE.EquirectangularReflectionMapping
  texture.colorSpace = THREE.SRGBColorSpace
  return texture
}

// 作用：按 control.skyboxEnabled 切换预览背景：典型天空盒（懒创建并缓存）或默认纯色。
function updateSceneSkybox(config) {
  if (config?.control?.skyboxEnabled) {
    typicalSkyboxTexture ||= createTypicalSkyboxTexture()
    scene.background = typicalSkyboxTexture
  } else {
    scene.background = defaultSceneBackground
  }
}

// 作用：递归释放对象的几何、材质及其纹理 / uniform 纹理，避免 GPU 内存泄漏。
function disposeObject(object) {
  object.traverse((child) => {
    child.geometry?.dispose?.()
    const materials = Array.isArray(child.material) ? child.material : [child.material]
    materials.forEach((item) => {
      if (!item) return
      for (const value of Object.values(item)) if (value?.isTexture) value.dispose()
      for (const uniform of Object.values(item.uniforms || {})) if (uniform?.value?.isTexture) uniform.value.dispose()
      item.dispose?.()
    })
  })
}

// 作用：清空 world 下的全部三维对象并释放资源，同时清除各类可视化句柄与选择状态。
function clearWorld() {
  clearObjectSelection(false)
  while (world.children.length) {
    const child = world.children.pop()
    disposeObject(child)
  }
  grid = null
  voxelPreview = null
  sceneBoundsGuide = null
  cruiseVisualization = null
}

// 作用：格式化尺寸标注：≥ 100 取整，否则保留最多 1 位小数，并带中文千分位。
function formatSceneDimension(value) {
  const number = Math.max(0, Number(value) || 0)
  return number >= 100 ? Math.round(number).toLocaleString('zh-CN') : Number(number.toFixed(1)).toLocaleString('zh-CN')
}

// 作用：用 canvas 绘制带圆角边框的文字标签并返回 THREE.Sprite（始终面向相机、不做深度测试）。
// 输入：text（经 t() 本地化）、width 为 320 px 宽标签对应的世界宽度（预览单位）；
//   文字更长时画布变宽，Sprite 同比例变宽。
function createDimensionLabel(text, width) {
  const label = t(text)
  const font = '700 34px "Segoe UI", "Microsoft YaHei", sans-serif'
  const canvas = document.createElement('canvas')
  const measure = canvas.getContext('2d')
  measure.font = font
  canvas.width = Math.max(320, Math.ceil(measure.measureText(label).width) + 48)
  canvas.height = 82
  const context = canvas.getContext('2d')
  context.fillStyle = 'rgba(248, 251, 255, 0.94)'
  context.strokeStyle = 'rgba(36, 111, 211, 0.9)'
  context.lineWidth = 4
  context.beginPath()
  context.roundRect(4, 4, canvas.width - 8, canvas.height - 8, 18)
  context.fill()
  context.stroke()
  context.fillStyle = '#173f70'
  context.font = font
  context.textAlign = 'center'
  context.textBaseline = 'middle'
  context.fillText(label, canvas.width / 2, canvas.height / 2 + 1)

  const texture = new THREE.CanvasTexture(canvas)
  texture.colorSpace = THREE.SRGBColorSpace
  const sprite = new THREE.Sprite(new THREE.SpriteMaterial({
    map: texture,
    transparent: true,
    depthTest: false,
    depthWrite: false
  }))
  // `width` is the world width of a 320 px label; longer text widens the sprite.
  const scale = width / 320
  sprite.scale.set(canvas.width * scale, canvas.height * scale, 1)
  sprite.renderOrder = 31
  return sprite
}

// 作用：在 start→end 之间放一根细圆柱作为边框线（比 WebGL 线宽更醒目），加入 group。
function addSceneBoundsEdge(group, start, end, radius, material) {
  const direction = end.clone().sub(start)
  const geometry = new THREE.CylinderGeometry(radius, radius, direction.length(), 6, 1, false)
  const edge = new THREE.Mesh(geometry, material.clone())
  edge.position.copy(start).add(end).multiplyScalar(0.5)
  edge.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), direction.normalize())
  edge.renderOrder = 29
  group.add(edge)
}

// 作用：重建“计算域”引导框：蓝色线框盒 + 三轴范围标签 + 正北箭头，并更新底部尺寸摘要。
// 说明：框体只反映配置的仿真域（scene.x/y/height），不随网格包围盒变化；标签 X/Z/Y 依次为
//   北向、东向、竖向范围（均含 offset，单位 m）。预览坐标中 Three 的 +X 即正北。
//   所有引导物体关闭深度测试，使稠密城市几何不会遮住参考框和指北针。
function updateSceneBoundsGuide(config) {
  if (sceneBoundsGuide) {
    world.remove(sceneBoundsGuide)
    disposeObject(sceneBoundsGuide)
    sceneBoundsGuide = null
  }

  const sceneX = Math.max(0.1, Number(config?.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config?.scene?.y) || 60)
  const offsetX = Number(config?.scene?.offsetX) || 0
  const offsetY = Number(config?.scene?.offsetY) || 0
  const offsetZ = Number(config?.scene?.offsetZ) || 0
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  const width = sceneX * displayScale
  const depth = sceneY * displayScale
  // The guide describes the configured simulation domain, not the current
  // mesh bounds. Objects may extend outside it, but must not resize the box.
  const sceneHeight = Math.max(0, Number(config?.scene?.height) || 0)
  const displayHeight = Math.max(0.06, sceneHeight * displayScale)

  const bounds = new THREE.Box3(
    new THREE.Vector3(-width / 2, 0, -depth / 2),
    new THREE.Vector3(width / 2, displayHeight, depth / 2)
  )
  const group = new THREE.Group()
  group.name = 'scene-bounds-guide'
  group.userData.sceneBoundsGuide = true

  const helper = new THREE.Box3Helper(bounds, 0x246fd3)
  helper.material.transparent = true
  helper.material.opacity = 0.66
  helper.material.depthTest = false
  helper.material.depthWrite = false
  helper.renderOrder = 30
  group.add(helper)

  const edgeMaterial = new THREE.MeshBasicMaterial({ color: 0x247bd8, transparent: true, opacity: 0.72, depthTest: false, depthWrite: false })
  const edgeRadius = THREE.MathUtils.clamp(Math.max(width, depth) * 0.0014, 0.018, 0.065)
  const bottom = 0.035
  const corners = [
    new THREE.Vector3(-width / 2, bottom, -depth / 2),
    new THREE.Vector3(width / 2, bottom, -depth / 2),
    new THREE.Vector3(width / 2, bottom, depth / 2),
    new THREE.Vector3(-width / 2, bottom, depth / 2)
  ]
  const topCorners = corners.map((corner) => new THREE.Vector3(corner.x, displayHeight, corner.z))
  for (let index = 0; index < 4; index += 1) {
    const next = (index + 1) % 4
    addSceneBoundsEdge(group, corners[index], corners[next], edgeRadius, edgeMaterial)
    addSceneBoundsEdge(group, topCorners[index], topCorners[next], edgeRadius, edgeMaterial)
    addSceneBoundsEdge(group, corners[index], topCorners[index], edgeRadius, edgeMaterial)
  }
  edgeMaterial.dispose()

  const extent = Math.max(width, depth)
  const labelWidth = THREE.MathUtils.clamp(extent * 0.16, 2.8, 8)
  const padding = THREE.MathUtils.clamp(extent * 0.035, 0.35, 1.4)
  const xLabel = createDimensionLabel(`X · ${formatSceneDimension(offsetX)}–${formatSceneDimension(offsetX + sceneX)} m`, labelWidth)
  xLabel.position.set(0, Math.max(0.18, displayHeight * 0.025), depth / 2 - padding * 0.55)
  group.add(xLabel)
  const zLabel = createDimensionLabel(`Z · ${formatSceneDimension(offsetZ)}–${formatSceneDimension(offsetZ + sceneY)} m`, labelWidth)
  zLabel.position.set(width / 2 - padding * 0.55, Math.max(0.18, displayHeight * 0.025), 0)
  group.add(zLabel)
  const heightLabel = createDimensionLabel(`Y · ${formatSceneDimension(offsetY)}–${formatSceneDimension(offsetY + sceneHeight)} m`, labelWidth)
  heightLabel.position.set(width / 2 - padding * 0.55, displayHeight / 2, depth / 2 - padding * 0.55)
  group.add(heightLabel)

  // Geographic azimuths use north as 0 degrees. Three.js +X is scene north.
  const northLength = Math.min(
    THREE.MathUtils.clamp(extent * 0.16, 2.8, 7.2),
    Math.max(1.2, depth * 0.3)
  )
  // Keep the compass inside the front-left quadrant; depth testing is disabled
  // so dense city geometry cannot hide the reference direction.
  const northOrigin = new THREE.Vector3(
    -width * 0.28,
    Math.max(0.12, displayHeight + 0.08),
    depth * 0.2 - northLength
  )
  const northArrow = new THREE.ArrowHelper(
    new THREE.Vector3(1, 0, 0), northOrigin, northLength,
    0x15805f, northLength * 0.22, northLength * 0.12
  )
  northArrow.name = 'scene-north-arrow'
  northArrow.traverse((child) => {
    child.renderOrder = 40
    if (child.material) {
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(northArrow)
  const northLabel = createDimensionLabel('正北 N · 0°', THREE.MathUtils.clamp(labelWidth * 0.72, 2.8, 5.4))
  northLabel.name = 'scene-north-label'
  northLabel.position.copy(northOrigin).add(new THREE.Vector3(northLength * 1.08, Math.max(0.18, displayHeight * 0.025), 0))
  northLabel.renderOrder = 41
  group.add(northLabel)

  sceneBoundsGuide = group
  world.add(group)
  world.userData.boundsHeight = displayHeight
  world.userData.sceneHeight = sceneHeight
  const summary = $('#sceneDimensionSummary')
  if (summary) summary.textContent = `计算域 X ${formatSceneDimension(offsetX)}–${formatSceneDimension(offsetX + sceneX)} · Y ${formatSceneDimension(offsetY)}–${formatSceneDimension(offsetY + sceneHeight)} · Z ${formatSceneDimension(offsetZ)}–${formatSceneDimension(offsetZ + sceneY)} m`
}

// 模块：传感器/巡航可视化（平行观测的卫星、透视成像的飞机与航线、视场足迹）。
// 作用：把传感器位置点（场景坐标：x 北、y 东、z 高度，米）换算为预览坐标 Vector3。
//   预览 x = (x - scene.x/2)·displayScale，预览 y = z·displayScale（竖直向上），
//   预览 z = (y - scene.y/2)·displayScale；高度下限为 0。
function cruisePointToWorld(point, config) {
  const sceneX = Math.max(0.1, Number(config?.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config?.scene?.y) || 60)
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  return new THREE.Vector3(
    (Number(point.x) - sceneX * 0.5) * displayScale,
    Math.max(0, Number(point.z)) * displayScale,
    (Number(point.y) - sceneY * 0.5) * displayScale
  )
}

// 作用：由航向（度，北为 0，向东增大）求飞机绕预览 Y 轴的偏航角（弧度）。
// 说明：预览 x = 场景 x（北）、预览 z = 场景 y（东），航向 h 的前进方向为 (x,z) = (cos h, sin h)，
//   观测箭头与足迹用的也是这一向量。飞机机头在局部 +Z，Three.js 中绕 Y 轴转 θ 后局部 +Z 指向
//   (x,z) = (sin θ, cos θ)，令其等于 (cos h, sin h) 得 θ = 90° − h。
//   （旧写法 θ = h + 90° 只在航向 0°/180° 正确，90°/270° 时机头与箭头相反；已用 three 实测核对。）
//   createCruiseAircraft 与 updateCruiseAnimation 共用本函数，保证两处朝向一致。
function cruiseAircraftYaw(headingDegrees) {
  return THREE.MathUtils.degToRad(90 - headingDegrees)
}

// 作用：用基本几何体拼一架简化飞机（机身、机头、机翼、尾翼、座舱）并带一个朝向相机的图标。
// 输入：size 为机身尺度（预览单位）、headingDegrees 为航向（度，北为 0，向东增大）。
// 说明：真实尺寸在公里级场景里会小到看不见，因此额外保留一个始终面向相机的 ✈ 圆形标记；
//   材质 userData.baseColor 记录原色，供显示风格切换时恢复。
function createCruiseAircraft(size, headingDegrees) {
  const aircraft = new THREE.Group()
  aircraft.name = 'cruise-aircraft'
  aircraft.userData.kind = 'sensor'
  aircraft.rotation.y = cruiseAircraftYaw(headingDegrees)

  const white = new THREE.MeshStandardMaterial({ color: 0xf4f7fa, roughness: 0.42, metalness: 0.1, transparent: true, opacity: 0.86 })
  const blue = new THREE.MeshStandardMaterial({ color: 0x83acd2, roughness: 0.46, metalness: 0.08, transparent: true, opacity: 0.84 })
  const dark = new THREE.MeshStandardMaterial({ color: 0x718da6, roughness: 0.34, metalness: 0.14, transparent: true, opacity: 0.86 })
  for (const item of [white, blue, dark]) item.userData.baseColor = item.color.getHex()

  const body = new THREE.Mesh(new THREE.CylinderGeometry(size * 0.085, size * 0.12, size * 1.18, 10), white)
  body.rotation.x = Math.PI / 2
  aircraft.add(body)
  const nose = new THREE.Mesh(new THREE.ConeGeometry(size * 0.12, size * 0.34, 10), blue)
  nose.rotation.x = Math.PI / 2
  nose.position.z = size * 0.76
  aircraft.add(nose)
  const wings = new THREE.Mesh(new THREE.BoxGeometry(size * 1.45, size * 0.055, size * 0.34), blue)
  wings.position.z = size * 0.04
  aircraft.add(wings)
  const tail = new THREE.Mesh(new THREE.BoxGeometry(size * 0.68, size * 0.045, size * 0.22), white)
  tail.position.z = -size * 0.5
  aircraft.add(tail)
  const fin = new THREE.Mesh(new THREE.BoxGeometry(size * 0.055, size * 0.3, size * 0.25), blue)
  fin.position.set(0, size * 0.15, -size * 0.48)
  aircraft.add(fin)
  const cockpit = new THREE.Mesh(new THREE.SphereGeometry(size * 0.105, 10, 6), dark)
  cockpit.scale.set(0.85, 0.55, 1.35)
  cockpit.position.set(0, size * 0.1, size * 0.28)
  aircraft.add(cockpit)

  // The aircraft's physical scale can be tiny in kilometre-scale scenes, so
  // retain the 3-D model and add a camera-facing marker for legibility.
  const markerCanvas = document.createElement('canvas')
  markerCanvas.width = markerCanvas.height = 160
  const markerContext = markerCanvas.getContext('2d')
  markerContext.beginPath()
  markerContext.arc(80, 80, 66, 0, Math.PI * 2)
  markerContext.fillStyle = 'rgba(255, 255, 255, 0.76)'
  markerContext.fill()
  markerContext.lineWidth = 9
  markerContext.strokeStyle = '#8aafd1'
  markerContext.stroke()
  markerContext.fillStyle = '#789fc2'
  markerContext.font = '700 82px "Segoe UI Symbol", "Microsoft YaHei", sans-serif'
  markerContext.textAlign = 'center'
  markerContext.textBaseline = 'middle'
  markerContext.fillText('✈', 80, 78)
  const markerTexture = new THREE.CanvasTexture(markerCanvas)
  markerTexture.colorSpace = THREE.SRGBColorSpace
  const marker = new THREE.Sprite(new THREE.SpriteMaterial({ map: markerTexture, transparent: true, depthTest: false, depthWrite: false }))
  marker.name = 'cruise-aircraft-marker'
  marker.position.y = size * 0.68
  marker.scale.set(size * 1.12, size * 1.12, 1)
  marker.renderOrder = 38
  aircraft.add(marker)
  aircraft.traverse((child) => {
    if (!child.isMesh) return
    child.userData.kind = 'sensor'
    child.castShadow = true
    child.renderOrder = 35
  })
  return aircraft
}

// 作用：用基本几何体拼一颗简化卫星（本体、太阳能板、相机镜头、天线），用于平行投影观测示意。
// 说明：相机镜头朝本地 -Y，调用方负责按观测方向旋转整星（见 createParallelObservationVisualization）。
function createObservationSatellite(size) {
  const satellite = new THREE.Group()
  satellite.name = 'parallel-observation-satellite'
  satellite.userData.kind = 'sensor'

  const bodyMaterial = new THREE.MeshStandardMaterial({ color: 0xeaf0f4, roughness: 0.42, metalness: 0.34, transparent: true, opacity: 0.86 })
  const trimMaterial = new THREE.MeshStandardMaterial({ color: 0xd9bd83, roughness: 0.48, metalness: 0.3, transparent: true, opacity: 0.84 })
  const panelMaterial = new THREE.MeshStandardMaterial({ color: 0x7197bc, roughness: 0.4, metalness: 0.2, transparent: true, opacity: 0.82 })
  const cellMaterial = new THREE.LineBasicMaterial({ color: 0xa9c3dc, transparent: true, opacity: 0.58 })
  for (const item of [bodyMaterial, trimMaterial, panelMaterial]) item.userData.baseColor = item.color.getHex()

  const body = new THREE.Mesh(new THREE.BoxGeometry(size * 0.58, size * 0.48, size * 0.72), bodyMaterial)
  satellite.add(body)
  const trim = new THREE.Mesh(new THREE.BoxGeometry(size * 0.66, size * 0.12, size * 0.78), trimMaterial)
  satellite.add(trim)

  for (const side of [-1, 1]) {
    const panel = new THREE.Mesh(new THREE.BoxGeometry(size * 1.05, size * 0.055, size * 0.58), panelMaterial)
    panel.position.x = side * size * 0.86
    satellite.add(panel)
    const panelGrid = new THREE.GridHelper(size * 0.92, 4, 0x71a7e8, 0x71a7e8)
    panelGrid.scale.z = 0.52
    panelGrid.position.set(side * size * 0.86, size * 0.035, 0)
    panelGrid.material = cellMaterial.clone()
    satellite.add(panelGrid)
  }

  // The camera points along local -Y; the whole satellite is rotated toward
  // the scene target after it has been positioned on the viewing hemisphere.
  const cameraHousing = new THREE.Mesh(new THREE.CylinderGeometry(size * 0.18, size * 0.23, size * 0.32, 14), trimMaterial)
  cameraHousing.position.y = -size * 0.38
  satellite.add(cameraHousing)
  const lens = new THREE.Mesh(new THREE.CylinderGeometry(size * 0.105, size * 0.105, size * 0.08, 16), new THREE.MeshStandardMaterial({ color: 0x71869a, roughness: 0.28, metalness: 0.4, transparent: true, opacity: 0.86 }))
  lens.position.y = -size * 0.58
  satellite.add(lens)

  const antenna = new THREE.Mesh(new THREE.SphereGeometry(size * 0.24, 16, 8, 0, Math.PI * 2, 0, Math.PI * 0.48), bodyMaterial)
  antenna.scale.y = 0.38
  antenna.position.y = size * 0.38
  satellite.add(antenna)
  satellite.traverse((child) => {
    if (!child.isMesh) return
    child.userData.kind = 'sensor'
    child.castShadow = true
    child.renderOrder = 36
  })
  return satellite
}

// 作用：平行（非透视）观测的示意图：卫星位于观测半球上、箭头指向场景目标，并画天顶线、
//   天顶角弧线、方位角箭头和“卫星观测 Z…° A…°”标签。
// 输入：config.sensor 的观测角（经 sensorViewAngles 展开后取第一组代表方向）与光照方位；
//   天顶角限制在 [0, 89.999]°，方位角取模到 [0, 360)。
// 步骤：1 由天顶角/方位角求指向卫星的单位向量；2 在半径 clamp(0.63·extent, 13, 31) 处放卫星并
//   朝向目标；3 添加视线箭头、天顶参考虚线、天顶角弧线与方位箭头；4 添加文字标签。
// 约定：方位角从北（预览 +X）起，向东（预览 +Z）增大；批量观测时标签注明“代表方向 1/n”。
function createParallelObservationVisualization(config) {
  const sensor = config.sensor
  const sceneX = Math.max(0.1, Number(config.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config.scene?.y) || 60)
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  const extent = Math.max(sceneX, sceneY) * displayScale
  const sceneHeight = Math.max(0, Number(config.scene?.height) || 0) * displayScale
  const angles = sensorViewAngles(sensor, config.light?.azimuth)
  const [zenithDegrees, azimuthDegrees] = angles[0] || [Number(sensor.vza) || 0, Number(sensor.vaa) || 0]
  const zenith = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(Number(zenithDegrees) || 0, 0, 89.999))
  const azimuthValue = ((Number(azimuthDegrees) || 0) % 360 + 360) % 360
  const azimuth = THREE.MathUtils.degToRad(azimuthValue)
  const target = new THREE.Vector3(0, Math.max(0.04, sceneHeight * 0.32), 0)
  const observationOut = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  const orbitalRadius = THREE.MathUtils.clamp(extent * 0.63, 13, 31)
  const satellitePosition = target.clone().addScaledVector(observationOut, orbitalRadius)
  const satelliteSize = THREE.MathUtils.clamp(extent * 0.026, 0.55, 1.15)

  const group = new THREE.Group()
  group.name = 'sensor-parallel-visualization'
  const satellite = createObservationSatellite(satelliteSize)
  satellite.position.copy(satellitePosition)
  satellite.quaternion.setFromUnitVectors(
    new THREE.Vector3(0, -1, 0), target.clone().sub(satellitePosition).normalize()
  )
  group.add(satellite)

  const viewArrow = new THREE.ArrowHelper(
    observationOut.clone().negate(), satellitePosition,
    orbitalRadius * 0.82, 0xc28aa6,
    satelliteSize * 0.4, satelliteSize * 0.22
  )
  viewArrow.name = 'parallel-view-direction'
  viewArrow.traverse((child) => {
    child.renderOrder = 39
    if (child.material) {
      child.material.transparent = true
      child.material.opacity = 0.62
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(viewArrow)

  const referenceGeometry = new THREE.BufferGeometry().setFromPoints([
    target,
    target.clone().add(new THREE.Vector3(0, orbitalRadius, 0))
  ])
  const reference = new THREE.Line(referenceGeometry, new THREE.LineDashedMaterial({
    color: 0x82a9cd, dashSize: satelliteSize * 0.28, gapSize: satelliteSize * 0.18,
    transparent: true, opacity: 0.5, depthTest: false, depthWrite: false
  }))
  reference.computeLineDistances()
  reference.name = 'parallel-zenith-reference'
  reference.renderOrder = 38
  group.add(reference)

  if (zenith > 0.002) {
    const arcPoints = []
    const arcRadius = orbitalRadius * 0.24
    for (let index = 0; index <= 28; index += 1) {
      const angle = zenith * index / 28
      arcPoints.push(target.clone().add(new THREE.Vector3(
        Math.sin(angle) * Math.cos(azimuth) * arcRadius,
        Math.cos(angle) * arcRadius,
        Math.sin(angle) * Math.sin(azimuth) * arcRadius
      )))
    }
    const arc = new THREE.Line(
      new THREE.BufferGeometry().setFromPoints(arcPoints),
      new THREE.LineBasicMaterial({ color: 0xc28aa6, transparent: true, opacity: 0.6, depthTest: false, depthWrite: false })
    )
    arc.name = 'parallel-view-zenith-arc'
    arc.renderOrder = 40
    group.add(arc)
  }

  const groundY = 0.075
  const azimuthDirection = new THREE.Vector3(Math.cos(azimuth), 0, Math.sin(azimuth)).normalize()
  const azimuthLength = THREE.MathUtils.clamp(extent * 0.2, 3.6, 8.5)
  const azimuthArrow = new THREE.ArrowHelper(
    azimuthDirection, new THREE.Vector3(0, groundY, 0), azimuthLength,
    0x78a799, satelliteSize * 0.3, satelliteSize * 0.17
  )
  azimuthArrow.name = 'parallel-view-azimuth'
  azimuthArrow.traverse((child) => {
    child.renderOrder = 39
    if (child.material) {
      child.material.transparent = true
      child.material.opacity = 0.62
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(azimuthArrow)

  const batchSuffix = angles.length > 1 ? ` · 代表方向 1/${angles.length}` : ''
  const label = createDimensionLabel(
    `卫星观测 Z${formatSceneDimension(zenithDegrees)}° A${formatSceneDimension(azimuthValue)}°${batchSuffix}`,
    THREE.MathUtils.clamp(extent * 0.17, 4.2, 7.6)
  )
  label.name = 'parallel-observation-label'
  label.position.copy(satellitePosition)
  label.position.y += satelliteSize * 1.25
  label.renderOrder = 42
  group.add(label)

  group.userData = { satellite, viewArrow, zenithDegrees, azimuthDegrees: azimuthValue }
  return group
}

// 作用：创建透视成像的视场足迹对象（地面四边形填充、轮廓线、相机到四角的锥线），
//   几何顶点先留空，由 updateObservationFootprint 每帧写入。
function createObservationFootprint() {
  const group = new THREE.Group()
  group.name = 'sensor-observation-footprint'

  const fillGeometry = new THREE.BufferGeometry()
  fillGeometry.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(12), 3))
  fillGeometry.setIndex([0, 1, 2, 0, 2, 3])
  const fill = new THREE.Mesh(fillGeometry, new THREE.MeshBasicMaterial({
    color: 0xc28aa6,
    transparent: true,
    opacity: 0.08,
    side: THREE.DoubleSide,
    depthWrite: false
  }))
  fill.renderOrder = 32
  group.add(fill)

  const outlineGeometry = new THREE.BufferGeometry()
  outlineGeometry.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(15), 3))
  const outline = new THREE.Line(outlineGeometry, new THREE.LineBasicMaterial({
    color: 0xc28aa6,
    transparent: true,
    opacity: 0.62,
    depthTest: false,
    depthWrite: false
  }))
  outline.renderOrder = 37
  group.add(outline)

  const coneGeometry = new THREE.BufferGeometry()
  coneGeometry.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(24), 3))
  const cone = new THREE.LineSegments(coneGeometry, new THREE.LineBasicMaterial({
    color: 0xc28aa6,
    transparent: true,
    opacity: 0.36,
    depthTest: false,
    depthWrite: false
  }))
  cone.renderOrder = 36
  group.add(cone)

  return { group, fillGeometry, outlineGeometry, coneGeometry, label: null }
}

// 作用：按相机位置 origin 与航向，把视场四个角射线投到地面，更新足迹的填充、轮廓和锥线。
// 输入：data 为 cruiseVisualization.userData（含 verticalFov、aspect、zenith、relativeVaa 等）。
// 说明：水平半视场角由垂直半视场角与宽高比求得（verticalFov 限制在 0.1°~120°）；
//   射线不朝下或离地太远时取场景尺寸 4 倍的远距离；交点限制在场景范围内，所以足迹
//   不会超出场景边界。尺寸标签只在首次创建，显示“视场约 宽 × 长 m”（由四角距离平均得到）。
function updateObservationFootprint(data, origin, headingDegrees) {
  const footprint = data.footprint
  if (!footprint) return
  const groundY = 0.045
  const verticalHalfFov = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(data.verticalFov, 0.1, 120) * 0.5)
  const horizontalHalfFov = Math.atan(Math.tan(verticalHalfFov) * data.aspect)
  const viewAzimuth = THREE.MathUtils.degToRad(headingDegrees + data.relativeVaa)
  const forward = new THREE.Vector3(
    Math.sin(data.zenith) * Math.cos(viewAzimuth),
    -Math.cos(data.zenith),
    Math.sin(data.zenith) * Math.sin(viewAzimuth)
  ).normalize()
  const right = new THREE.Vector3(-Math.sin(viewAzimuth), 0, Math.cos(viewAzimuth))
  const up = right.clone().cross(forward).normalize()
  const halfSceneWidth = data.sceneX * data.displayScale * 0.5
  const halfSceneDepth = data.sceneY * data.displayScale * 0.5
  const farDistance = Math.max(halfSceneWidth, halfSceneDepth) * 4
  const corners = [[-1, 1], [1, 1], [1, -1], [-1, -1]].map(([horizontal, vertical]) => {
    const ray = forward.clone()
      .addScaledVector(right, horizontal * Math.tan(horizontalHalfFov))
      .addScaledVector(up, vertical * Math.tan(verticalHalfFov))
      .normalize()
    const distance = ray.y < -0.0001
      ? Math.max(0, (origin.y - groundY) / -ray.y)
      : farDistance
    const point = origin.clone().addScaledVector(ray, distance)
    point.set(
      THREE.MathUtils.clamp(point.x, -halfSceneWidth, halfSceneWidth),
      groundY,
      THREE.MathUtils.clamp(point.z, -halfSceneDepth, halfSceneDepth)
    )
    return point
  })

  const fillPositions = footprint.fillGeometry.getAttribute('position')
  corners.forEach((point, index) => fillPositions.setXYZ(index, point.x, point.y, point.z))
  fillPositions.needsUpdate = true

  const outlinePositions = footprint.outlineGeometry.getAttribute('position')
  ;[...corners, corners[0]].forEach((point, index) => outlinePositions.setXYZ(index, point.x, point.y, point.z))
  outlinePositions.needsUpdate = true

  const conePositions = footprint.coneGeometry.getAttribute('position')
  corners.forEach((point, index) => {
    conePositions.setXYZ(index * 2, origin.x, origin.y, origin.z)
    conePositions.setXYZ(index * 2 + 1, point.x, point.y, point.z)
  })
  conePositions.needsUpdate = true

  const center = corners.reduce((result, point) => result.add(point), new THREE.Vector3()).multiplyScalar(0.25)
  if (!footprint.label) {
    const width = (corners[0].distanceTo(corners[1]) + corners[3].distanceTo(corners[2])) * 0.5 / data.displayScale
    const length = (corners[0].distanceTo(corners[3]) + corners[1].distanceTo(corners[2])) * 0.5 / data.displayScale
    const labelWidth = THREE.MathUtils.clamp(Math.max(data.sceneX, data.sceneY) * data.displayScale * 0.18, 3.2, 8)
    footprint.label = createDimensionLabel(`视场约 ${formatSceneDimension(width)} × ${formatSceneDimension(length)} m`, labelWidth)
    footprint.label.name = 'sensor-observation-footprint-label'
    footprint.label.renderOrder = 39
    footprint.group.add(footprint.label)
  }
  footprint.label.position.copy(center)
  footprint.label.position.y = groundY + Math.max(0.12, data.aircraftSize * 0.14)
}

// 作用：重建传感器可视化：平行投影用卫星示意；透视成像则画飞机、航线、观测箭头与视场足迹。
// 步骤：1 移除旧可视化；2 求巡航点（开启巡航时用 sensorCruisePositions，否则单点：
//   传感器位置，航向取 vaa）；3 画航线段与标记点；4 创建飞机、观测方向箭头与足迹；
//   5 把动画所需数据存入 group.userData，供 updateCruiseAnimation 使用。
// 说明：相邻两点在 x 或 y 上跳变超过场景一半视为周期边界回绕（引擎 wrapCoordinate 把航线坐标按场景尺寸取模，与 project-schema 的 sensorCruisePositions 一致），不画连线；标记点最多约 512 个；
//   动画周期 = clamp((点数-1)·0.5, 2.5, 12) 秒；巡航时 vaa 为相对航向的方位角，
//   固定位置时 vaa 即绝对航向（relativeVaa 取 0）。
function updateCruiseVisualization(config) {
  if (cruiseVisualization) {
    world.remove(cruiseVisualization)
    disposeObject(cruiseVisualization)
    cruiseVisualization = null
  }
  const sensor = config?.sensor
  if (!sensor) return
  if (sensor.projection !== 'perspective') {
    cruiseVisualization = createParallelObservationVisualization(config)
    world.add(cruiseVisualization)
    return
  }

  const cruiseEnabled = Boolean(sensor.cruiseEnabled)
  const fixedHeading = ((Number(sensor.vaa) || 0) % 360 + 360) % 360
  const positions = cruiseEnabled
    ? sensorCruisePositions(sensor, config.scene)
    : [{ x: Number(sensor.positionX), y: Number(sensor.positionY), z: Number(sensor.height), heading: fixedHeading }]
  if (!positions.length) return
  const sceneX = Math.max(0.1, Number(config.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config.scene?.y) || 60)
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  const worldPoints = positions.map((point) => cruisePointToWorld(point, config))
  const group = new THREE.Group()
  group.name = 'sensor-perspective-visualization'

  const segmentVertices = []
  for (let index = 1; cruiseEnabled && index < positions.length; index += 1) {
    const previous = positions[index - 1]
    const current = positions[index]
    if (Math.abs(current.x - previous.x) > sceneX * 0.5 || Math.abs(current.y - previous.y) > sceneY * 0.5) continue
    segmentVertices.push(...worldPoints[index - 1].toArray(), ...worldPoints[index].toArray())
  }
  if (segmentVertices.length) {
    const geometry = new THREE.BufferGeometry()
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(segmentVertices, 3))
    const line = new THREE.LineSegments(geometry, new THREE.LineBasicMaterial({ color: 0x82a9cd, transparent: true, opacity: 0.58, depthTest: false, depthWrite: false }))
    line.renderOrder = 33
    group.add(line)
  }

  if (cruiseEnabled) {
    const maximumMarkers = 512
    const markerStep = Math.max(1, Math.ceil(worldPoints.length / maximumMarkers))
    const markerPoints = worldPoints.filter((_, index) => index % markerStep === 0 || index === worldPoints.length - 1)
    const markerGeometry = new THREE.BufferGeometry().setFromPoints(markerPoints)
    const markers = new THREE.Points(markerGeometry, new THREE.PointsMaterial({ color: 0xd9ad79, transparent: true, opacity: 0.68, size: 4.5, sizeAttenuation: false, depthTest: false, depthWrite: false }))
    markers.renderOrder = 34
    group.add(markers)
  }

  const aircraftSize = THREE.MathUtils.clamp(Math.max(sceneX, sceneY) * displayScale * 0.024, 0.52, 1.25)
  const aircraft = createCruiseAircraft(aircraftSize, Number(positions[0].heading) || 0)
  aircraft.position.copy(worldPoints[0])
  group.add(aircraft)

  const zenith = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(Number(sensor.vza) || 0, 0, 89.999))
  const relativeVaa = cruiseEnabled ? Number(sensor.vaa) || 0 : 0
  const relativeAzimuth = Number(positions[0].heading) + relativeVaa
  const azimuth = THREE.MathUtils.degToRad(relativeAzimuth || 0)
  const viewDirection = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    -Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  const heightDisplay = Math.max(0.01, Number(sensor.height) * displayScale)
  const arrowLength = THREE.MathUtils.clamp(heightDisplay / Math.max(0.12, Math.cos(zenith)) * 0.7, aircraftSize * 1.1, aircraftSize * 4)
  const viewArrow = new THREE.ArrowHelper(viewDirection, worldPoints[0], arrowLength, 0xc28aa6, aircraftSize * 0.28, aircraftSize * 0.16)
  viewArrow.traverse((child) => { child.renderOrder = 36; if (child.material) { child.material.transparent = true; child.material.opacity = 0.62; child.material.depthTest = false; child.material.depthWrite = false } })
  group.add(viewArrow)

  const footprint = createObservationFootprint()
  group.add(footprint.group)
  group.userData = {
    positions,
    worldPoints,
    aircraft,
    viewArrow,
    sceneX,
    sceneY,
    displayScale,
    aircraftSize,
    zenith,
    relativeVaa,
    verticalFov: Number(sensor.fov) || 60,
    aspect: Math.max(0.01, Number(sensor.x) || 1) / Math.max(0.01, Number(sensor.y) || 1),
    footprint,
    startedAt: performance.now(),
    duration: THREE.MathUtils.clamp(Math.max(1, positions.length - 1) * 0.5, 2.5, 12)
  }
  updateObservationFootprint(group.userData, worldPoints[0], Number(positions[0].heading) || 0)
  cruiseVisualization = group
  world.add(group)
}

// 作用：每帧推进飞机沿巡航点循环飞行，并同步观测箭头与足迹。now 为毫秒时间戳。
// 说明：在相邻两点间线性插值；x/y 用“环绕插值”（wrappedLerp）按周期边界走最短路径；
//   航向取当前线段起点的航向；巡航时叠加一个微小上下起伏（0.035·机身尺度）。
function updateCruiseAnimation(now) {
  const data = cruiseVisualization?.userData
  if (!data?.aircraft || !data.positions?.length) return
  const count = data.positions.length
  let point = data.positions[0]
  let currentHeading = Number(point.heading) || 0
  if (count > 1) {
    const startedAt = Number.isFinite(Number(data.startedAt)) ? Number(data.startedAt) : now
    const duration = Math.max(0.001, Number(data.duration) || 1)
    const elapsedSeconds = Math.max(0, now - startedAt) / 1000
    const progress = (elapsedSeconds % duration) / duration * (count - 1)
    const index = Math.max(0, Math.min(count - 2, Math.floor(progress)))
    const fraction = progress - index
    const start = data.positions[index] || data.positions[0]
    const end = data.positions[index + 1] || start
    currentHeading = Number(start.heading) || 0
    const wrappedLerp = (a, b, extent) => {
      let delta = b - a
      if (delta > extent * 0.5) delta -= extent
      if (delta < -extent * 0.5) delta += extent
      return ((a + delta * fraction) % extent + extent) % extent
    }
    point = {
      x: wrappedLerp(start.x, end.x, data.sceneX),
      y: wrappedLerp(start.y, end.y, data.sceneY),
      z: THREE.MathUtils.lerp(start.z, end.z, fraction)
    }
  }
  const worldPoint = cruisePointToWorld(point, state.config)
  if (count > 1) worldPoint.y += Math.sin(now * 0.004) * data.aircraftSize * 0.035
  data.aircraft.position.copy(worldPoint)
  data.aircraft.rotation.y = cruiseAircraftYaw(currentHeading)
  data.viewArrow.position.copy(worldPoint)
  const viewAzimuth = THREE.MathUtils.degToRad(currentHeading + data.relativeVaa)
  data.viewArrow.setDirection(new THREE.Vector3(
    Math.sin(data.zenith) * Math.cos(viewAzimuth),
    -Math.cos(data.zenith),
    Math.sin(data.zenith) * Math.sin(viewAzimuth)
  ).normalize())
  updateObservationFootprint(data, worldPoint, currentHeading)
}

// 作用：创建场景预览用的不透明 MeshStandardMaterial，并在 userData.baseColor 记录原色，
//   以便 applyViewStyle 在不同显示风格间切换后恢复。
function material(color, roughness = .82) {
  const result = new THREE.MeshStandardMaterial({
    color,
    roughness,
    metalness: .02,
    transparent: false,
    opacity: 1,
    depthTest: true,
    depthWrite: true,
    alphaTest: 0,
    alphaToCoverage: false
  })
  result.userData.baseColor = result.color.getHex()
  return result
}

// 作用：按配置重建基础场景：地面（可带 DEM 起伏）、地面网格，并刷新边界引导、传感器可视化。
// 输入：config.scene（x/y 为北向/东向尺寸，米；voxel 体元尺寸；terrain/demInfo 地形）。
// 副作用：清空 world；设置 world.userData.extent / displayScale。
// 步骤：1 求预览缩放 displayScale = min(1, 45 / 场景最大边长)，使大场景缩到约 45 个预览单位；
//   2 创建地面网格：有 DEM 时按体元尺寸取网格密度（最多约 128 格），否则固定 36×36；
//   3 逐顶点采样 DEM 设置高度（相对 DEM 最小值，≥ 0）；4 加网格、边界框与传感器可视化。
// 约定：DEM 列向东增大、第 0 行为北边缘；预览中 +X 为北、+Z 为东。
function generateWorld(config = defaultConfig()) {
  clearWorld()
  const sx = Math.max(0.1, Number(config.scene.x) || 60)
  const sz = Math.max(0.1, Number(config.scene.y) || 60)
  const displayScale = Math.min(1, 45 / Math.max(sx, sz))
  const width = sx * displayScale
  const depth = sz * displayScale
  world.userData.extent = Math.max(width, depth)
  world.userData.displayScale = displayScale
  const dem = config.scene.terrain ? config.scene.demInfo : null
  const demWidth = Math.max(2, Math.round(Number(dem?.terrainWidth) || 0))
  const demHeight = Math.max(2, Math.round(Number(dem?.terrainHeight) || 0))
  const demValues = Array.isArray(dem?.terrainValues) ? dem.terrainValues : []
  const hasDemSurface = demValues.length === demWidth * demHeight
  const sceneVoxel = Math.max(.1, Number(config.scene.voxel) || 1)
  const requestedColumns = Math.max(2, Math.ceil(sx / sceneVoxel) + 1)
  const requestedRows = Math.max(2, Math.ceil(sz / sceneVoxel) + 1)
  const previewScale = Math.min(1, 128 / Math.max(requestedColumns, requestedRows))
  const terrainColumns = Math.max(2, Math.round(requestedColumns * previewScale))
  const terrainRows = Math.max(2, Math.round(requestedRows * previewScale))
  const groundGeo = hasDemSurface
    ? new THREE.PlaneGeometry(width, depth, terrainColumns - 1, terrainRows - 1)
    : new THREE.PlaneGeometry(width, depth, 36, 36)
  const positions = groundGeo.attributes.position
  const baseElevation = Number.isFinite(Number(dem?.minimum)) ? Number(dem.minimum) : 0
  const demSample = (u, v) => {
    const imageX = THREE.MathUtils.clamp(u, 0, 1) * (demWidth - 1)
    const imageY = THREE.MathUtils.clamp(v, 0, 1) * (demHeight - 1)
    const x0 = Math.floor(imageX), y0 = Math.floor(imageY)
    const x1 = Math.min(demWidth - 1, x0 + 1), y1 = Math.min(demHeight - 1, y0 + 1)
    const at = (x, y) => {
      const source = demValues[y * demWidth + x]
      const value = source == null ? Number.NaN : Number(source)
      return Number.isFinite(value) ? value : baseElevation
    }
    const top = THREE.MathUtils.lerp(at(x0, y0), at(x1, y0), imageX - x0)
    const bottom = THREE.MathUtils.lerp(at(x0, y1), at(x1, y1), imageX - x0)
    return THREE.MathUtils.lerp(top, bottom, imageY - y0)
  }
  // 地面平面旋转 -90° 绕 X 之前，平面局部 Y 对应预览 -Z（东向反向）。因此 u = 0.5 - y/depth
  //   给出东向列比例，v = 0.5 - x/width 给出自北向南的行比例；局部 z 即抬升高度。
  for (let i = 0; i < positions.count; i++) {
    // After the -90° X rotation, world +X is north and world +Z is east.
    // GeoTIFF columns grow eastward and row zero is the northern edge.
    const u = depth > 0 ? .5 - positions.getY(i) / depth : .5
    const v = width > 0 ? .5 - positions.getX(i) / width : .5
    const sourceElevation = hasDemSurface ? demSample(u, v) : baseElevation
    const elevation = Number.isFinite(sourceElevation) ? sourceElevation : baseElevation
    positions.setZ(i, hasDemSurface ? Math.max(0, elevation - baseElevation) * displayScale : 0)
  }
  groundGeo.computeVertexNormals()
  const ground = new THREE.Mesh(groundGeo, material(0xcbd8e6, .96))
  ground.rotation.x = -Math.PI / 2
  ground.receiveShadow = true
  ground.userData.kind = 'ground'
  world.add(ground)
  grid = new THREE.GridHelper(Math.max(width, depth), Math.max(8, Math.round(Math.max(width, depth) / 1.5)), 0x7895b5, 0xb8c6d6)
  grid.position.y = .012
  grid.material.opacity = .23
  grid.material.transparent = true
  world.add(grid)

  updateSceneBoundsGuide(config)
  updateCruiseVisualization(config)
  applyViewStyle(currentStyle)
  updateStats()
}

// 作用：按光照配置更新预览太阳：平行光方向/强度，以及示意用的太阳球、光晕、入射箭头与标签。
// 输入：config.light 的 zenith（限制在 [0, 90]°）、azimuth（北为 0 向东增大，取模到 [0, 360)）、
//   direct（直射比例）；平行光强度 = 1.2 + 2.5·direct（仅预览亮度，不参与辐射计算）。
// 说明：太阳示意位置在以场景为中心、半径 clamp(0.54·extent, 11, 28) 的球面上；
//   调用前需已更新 world.userData.extent / boundsHeight（由 generateWorld 与边界引导设置）。
function updateSun(config) {
  const zenithDegrees = THREE.MathUtils.clamp(Number(config.light.zenith) || 0, 0, 90)
  const azimuthDegrees = ((Number(config.light.azimuth) || 0) % 360 + 360) % 360
  const zenith = THREE.MathUtils.degToRad(zenithDegrees)
  const azimuth = THREE.MathUtils.degToRad(azimuthDegrees)
  const lightDirection = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  sun.position.copy(lightDirection).multiplyScalar(45)
  sun.intensity = 1.2 + config.light.direct * 2.5

  if (sunVisualization) {
    scene.remove(sunVisualization)
    disposeObject(sunVisualization)
  }
  const extent = Math.max(12, Number(world.userData.extent) || 45)
  const sceneHeight = Math.max(0, Number(world.userData.boundsHeight) || 0)
  const radius = THREE.MathUtils.clamp(extent * 0.54, 11, 28)
  const center = new THREE.Vector3(0, sceneHeight * 0.35, 0)
  const sunPosition = center.clone().addScaledVector(lightDirection, radius)
  const group = new THREE.Group()
  group.name = 'sun-direction-visualization'

  const sunSize = THREE.MathUtils.clamp(extent * 0.024, 0.45, 1.05)
  const sphere = new THREE.Mesh(
    new THREE.SphereGeometry(sunSize, 24, 14),
    new THREE.MeshBasicMaterial({ color: 0xf1d57c, transparent: true, opacity: 0.84, depthTest: false, depthWrite: false })
  )
  sphere.position.copy(sunPosition)
  sphere.renderOrder = 44
  group.add(sphere)

  const glowCanvas = document.createElement('canvas')
  glowCanvas.width = glowCanvas.height = 192
  const glowContext = glowCanvas.getContext('2d')
  const glow = glowContext.createRadialGradient(96, 96, 18, 96, 96, 92)
  glow.addColorStop(0, 'rgba(255, 248, 205, 0.74)')
  glow.addColorStop(0.34, 'rgba(244, 215, 132, 0.44)')
  glow.addColorStop(1, 'rgba(237, 196, 101, 0)')
  glowContext.fillStyle = glow
  glowContext.fillRect(0, 0, 192, 192)
  const glowTexture = new THREE.CanvasTexture(glowCanvas)
  glowTexture.colorSpace = THREE.SRGBColorSpace
  const glowSprite = new THREE.Sprite(new THREE.SpriteMaterial({ map: glowTexture, transparent: true, depthTest: false, depthWrite: false }))
  glowSprite.position.copy(sunPosition)
  glowSprite.scale.setScalar(sunSize * 3.5)
  glowSprite.renderOrder = 43
  group.add(glowSprite)

  const incomingDirection = center.clone().sub(sunPosition).normalize()
  const arrow = new THREE.ArrowHelper(incomingDirection, sunPosition, radius * 0.72, 0xd8ba73, sunSize * 0.76, sunSize * 0.4)
  arrow.name = 'sun-incoming-direction'
  arrow.traverse((child) => {
    child.renderOrder = 42
    if (child.material) {
      child.material.transparent = true
      child.material.opacity = 0.58
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(arrow)

  const label = createDimensionLabel(`太阳 Z${formatSceneDimension(zenithDegrees)}° A${formatSceneDimension(azimuthDegrees)}°`, THREE.MathUtils.clamp(extent * 0.11, 3.2, 5.4))
  label.position.copy(sunPosition)
  label.position.y += sunSize * 1.75
  label.renderOrder = 45
  group.add(label)

  sunVisualization = group
  scene.add(group)
}

// 热力视图（applyViewStyle 的 'thermal'）用：缓存材质原始颜色，切回其他视图时据此恢复。
const thermalBaseColors = new WeakMap()

// Approximate voxelization of the previewed scene objects at the configured
// voxel size: triangles are sampled at half-voxel spacing and the occupied
// cells drawn as translucent boxes. It shows the grid resolution and which
// structures survive voxelization before running VoxelRT/VoxelEB. The engine
// additionally applies the fill threshold, so counts are an upper estimate.
// Very large cell counts are shown at a coarser display resolution.
// 作用：把预览场景中的对象近似体元化，生成半透明实例化方块（voxelPreview）加入 world。
// 输入：state.config.scene（voxel 体元边长 m；x/y/height 场景尺寸 m）；world 中的网格。
// 输出：返回提示文字（网格尺寸、占用体元数，是否粗化显示）；无可显示对象时返回空串。
// 单位：显示坐标 = 米 × world.userData.displayScale，故 cellSize 以显示单位计，提示时再除回。
// 说明：纯预览，不参与引擎计算；单元数超过 150000 或累计采样点超过 sampleBudget（6e6）时，
//   本轮作废、cellSize 翻倍后重试（最多 8 轮），提示文字会注明“粗化显示”；
//   三角形总数过大时仍按 stride 隔点抽样（见下），这一项是近似，结果只作预览。
function buildVoxelPreview() {
  const c = state.config
  if (!c) return ''
  const voxelMeters = Math.max(0.01, Number(c.scene?.voxel) || 1)
  const scale = Number(world.userData.displayScale) || 1
  if (voxelPreview) {
    world.remove(voxelPreview)
    disposeObject(voxelPreview)
    voxelPreview = null
  }
  world.updateMatrixWorld(true)
  // 步骤 1：收集参与体元化的网格；排除地面、传感器、辅助线框（网格/边界/巡航）和精灵材质。
  const meshes = []
  const helperRoots = new Set([grid, sceneBoundsGuide, cruiseVisualization].filter(Boolean))
  const isHelper = (object) => {
    for (let node = object; node && node !== world; node = node.parent) if (helperRoots.has(node)) return true
    return false
  }
  world.traverse((child) => {
    if (!child.isMesh || !child.visible || !child.geometry?.attributes?.position) return
    const kind = child.userData.kind
    if (kind === 'ground' || kind === 'sensor' || child.userData.previewHelper || isHelper(child)) return
    if (child.material && !Array.isArray(child.material) && child.material.isSpriteMaterial) return
    meshes.push(child)
  })
  if (!meshes.length) return ''
  // 显示单元数上限与总采样点预算：防止大场景/小体元时卡顿或内存暴涨。
  const maximumCells = 150000
  const sampleBudget = 6e6
  const inverseWorld = new THREE.Matrix4().copy(world.matrixWorld).invert()
  const instanceMatrix = new THREE.Matrix4()
  const transform = new THREE.Matrix4()
  const a = new THREE.Vector3(), b = new THREE.Vector3(), d = new THREE.Vector3()
  let cellSize = voxelMeters * scale
  let cells = null
  // 步骤 2：把三角形按重心坐标加密采样，落入的网格单元记入 cells；
  // 超过 maximumCells 时放弃本轮，cellSize 翻倍后重试。
  for (let attempt = 0; attempt < 8; attempt += 1) {
    cells = new Set()
    let overflow = false
    let samples = 0
    for (const mesh of meshes) {
      const position = mesh.geometry.attributes.position
      const index = mesh.geometry.index
      const triangleCount = Math.floor((index ? index.count : position.count) / 3)
      const instances = mesh.isInstancedMesh ? mesh.count : 1
      // 三角形总数过大时按步长 stride 隔点抽样以限制耗时，占用结果可能偏少，仅作预览。
      const stride = Math.max(1, Math.ceil(triangleCount * instances / 400000))
      for (let instance = 0; instance < instances && !overflow; instance += 1) {
        transform.copy(mesh.matrixWorld)
        if (mesh.isInstancedMesh) { mesh.getMatrixAt(instance, instanceMatrix); transform.multiply(instanceMatrix) }
        // 变换到 world 局部坐标，使体元网格与 world 对齐（含实例矩阵）。
        transform.premultiply(inverseWorld)
        for (let triangle = 0; triangle < triangleCount; triangle += stride) {
          const i0 = index ? index.getX(3 * triangle) : 3 * triangle
          const i1 = index ? index.getX(3 * triangle + 1) : 3 * triangle + 1
          const i2 = index ? index.getX(3 * triangle + 2) : 3 * triangle + 2
          a.fromBufferAttribute(position, i0).applyMatrix4(transform)
          b.fromBufferAttribute(position, i1).applyMatrix4(transform)
          d.fromBufferAttribute(position, i2).applyMatrix4(transform)
          const edge = Math.max(a.distanceTo(b), b.distanceTo(d), d.distanceTo(a))
          // 按最长边确定每边细分数：采样间距约半个体元，单个三角形最多 48 段。
          const steps = Math.min(48, Math.max(1, Math.ceil(edge / (cellSize * 0.5))))
          for (let u = 0; u <= steps; u += 1) {
            for (let v = 0; v <= steps - u; v += 1) {
              const s = u / steps, t = v / steps
              const x = a.x + (b.x - a.x) * s + (d.x - a.x) * t
              const y = a.y + (b.y - a.y) * s + (d.y - a.y) * t
              const z = a.z + (b.z - a.z) * s + (d.z - a.z) * t
              cells.add(`${Math.floor(x / cellSize)},${Math.floor(y / cellSize)},${Math.floor(z / cellSize)}`)
            }
          }
          samples += (steps + 1) * (steps + 2) / 2
          if (cells.size > maximumCells) { overflow = true; break }
          // 超出采样预算：与单元数超限一样，按粗化重试处理（细分数随 cellSize 增大而减少，
          //   翻倍后采样量约降为 1/4）；此前只中断当前实例遍历，其余实例被静默截断。
          if (samples > sampleBudget) { overflow = true; break }
        }
      }
      if (overflow) break
    }
    if (!overflow) break
    // 最后一轮仍超限时保持 cellSize 不变：cells 的键是按当前 cellSize 计算的，再翻倍会使方块尺寸与键不符。
    if (attempt < 7) cellSize *= 2
  }
  if (!cells?.size) return ''
  // 步骤 3：每个占用单元画一个略小（0.94 倍）的方块，避免相邻方块边界重叠闪烁。
  const box = new THREE.BoxGeometry(cellSize * 0.94, cellSize * 0.94, cellSize * 0.94)
  const material = new THREE.MeshBasicMaterial({ color: 0x2f7fe0, transparent: true, opacity: 0.32, depthWrite: false })
  const mesh = new THREE.InstancedMesh(box, material, cells.size)
  mesh.userData.previewHelper = true
  mesh.renderOrder = 20
  const matrix = new THREE.Matrix4()
  let n = 0
  for (const key of cells) {
    const [ix, iy, iz] = key.split(',').map(Number)
    matrix.makeTranslation((ix + 0.5) * cellSize, (iy + 0.5) * cellSize, (iz + 0.5) * cellSize)
    mesh.setMatrixAt(n, matrix)
    n += 1
  }
  mesh.instanceMatrix.needsUpdate = true
  voxelPreview = mesh
  world.add(mesh)
  // 步骤 4：生成状态栏文字；网格数按场景尺寸 / 体元边长向上取整。
  const nx = Math.ceil((Number(c.scene?.x) || 0) / voxelMeters)
  const ny = Math.ceil((Number(c.scene?.y) || 0) / voxelMeters)
  const nz = Math.ceil((Number(c.scene?.height) || 0) / voxelMeters)
  const shownMeters = cellSize / scale
  const coarse = shownMeters > voxelMeters * 1.01
  return `网格 ${nx}×${ny}×${nz}（体元 ${voxelMeters} m），对象占用约 ${coarse ? '（按 ' + Number(shownMeters.toFixed(2)) + ' m 粗化显示）' : ''}${cells.size.toLocaleString('zh-CN')} 个${coarse ? '显示单元' : '体元'}；引擎还会按填充阈值剔除低占比体元`
}

// 作用：切换视口显示样式。style = 'wireframe' 时线框；'thermal' 时按对象类别着示意色
// （非真实温度，仅区分植被/建筑/地面/传感器）；其他值恢复原始颜色。
// 说明：进入 thermal 前把原色存入 thermalBaseColors，离开时还原；体元预览网格不受影响。
function applyViewStyle(style) {
  currentStyle = style
  world.traverse((child) => {
    if (!child.isMesh || child === voxelPreview) return
    const materials = Array.isArray(child.material) ? child.material : [child.material]
    for (const item of materials) {
      if (!item?.color) continue
      item.wireframe = style === 'wireframe'
      if (style === 'thermal') {
        if (!thermalBaseColors.has(item)) thermalBaseColors.set(item, item.color.clone())
        const colors = { vegetation: 0xe7b84a, building: 0xd3544c, ground: 0x4c72bd, sensor: 0xe8ebe9 }
        item.color.setHex(colors[child.userData.kind] || 0x61b68e)
      } else if (thermalBaseColors.has(item)) {
        item.color.copy(thermalBaseColors.get(item))
        thermalBaseColors.delete(item)
      } else if (item.userData.baseColor != null) item.color.setHex(item.userData.baseColor)
    }
  })
}

// 作用：按场景范围 extent 与高度 boundsHeight，把相机放到斜上方，轨道控制器目标取场景中心偏下处。
function fitCamera() {
  const extent = world.userData.extent || 20
  const height = world.userData.boundsHeight || 2
  camera.position.set(extent * 1.04, Math.max(extent * .86, height * 1.35), extent * 1.14)
  controls.target.set(0, Math.min(height * .36, extent * .25), 0)
  controls.update()
}

// 作用：统计 world 中所有网格的三角形数（含实例化网格的实例数），显示为 K TRI（千三角形）。
function updateStats() {
  let triangles = 0
  world.traverse((object) => {
    if (!object.isMesh || !object.geometry) return
    const count = object.geometry.index ? object.geometry.index.count / 3 : object.geometry.attributes.position.count / 3
    triangles += count * (object.isInstancedMesh ? object.count : 1)
  })
  $('#triangles').textContent = `${Math.round(triangles / 100) / 10}K TRI`
}

// 作用：视口尺寸变化时同步渲染器尺寸与相机宽高比（由下面的 ResizeObserver 触发）。
function resize() {
  renderer.setSize(viewport.clientWidth, viewport.clientHeight, false)
  camera.aspect = viewport.clientWidth / Math.max(1, viewport.clientHeight)
  camera.updateProjectionMatrix()
}
new ResizeObserver(resize).observe(viewport)
// 渲染循环状态：frameCount/lastFps 用于每 750 ms 刷新一次 FPS 显示；lastAnimationTime 用于动画步长。
let frameCount = 0, lastFps = performance.now(), lastAnimationTime = performance.now()
// While the engine runs, the 60 fps viewport (with its 2048² shadow map)
// competes with it for the GPU. Throttle to ~10 fps unless the user is
// interacting with the viewport.
const RUNNING_FRAME_INTERVAL_MS = 100
const VIEWPORT_INTERACTION_GRACE_MS = 1500
let lastRenderTime = 0, lastViewportInput = 0
for (const type of ['pointerdown', 'pointermove', 'wheel', 'keydown']) {
  renderer.domElement.addEventListener(type, () => { lastViewportInput = performance.now() }, { passive: true })
}
// 作用：requestAnimationFrame 渲染循环。
// 步骤 1：引擎运行且用户近期无视口输入时，按 RUNNING_FRAME_INTERVAL_MS 降帧。
// 步骤 2：动画步长 deltaSeconds 限幅到 [0, 0.1] s，避免切回页面后动画跳变。
// 步骤 3：更新控制器、场景动态对象、巡航动画、选中框，渲染主场景与结果查看器，并统计 FPS。
function animate(now) {
  requestAnimationFrame(animate)
  if (state.running && now - lastViewportInput > VIEWPORT_INTERACTION_GRACE_MS &&
      now - lastRenderTime < RUNNING_FRAME_INTERVAL_MS) return
  lastRenderTime = now
  const deltaSeconds = Math.min(.1, Math.max(0, (now - lastAnimationTime) / 1000))
  lastAnimationTime = now
  controls.update(); updateSceneDynamics(world, deltaSeconds, state.config); updateCruiseAnimation(now); updateObjectSelectionBox(); renderer.render(scene, camera); renderResultViewer(); frameCount++
  if (now - lastFps > 750) {
    $('#fps').textContent = `${Math.round(frameCount * 1000 / (now - lastFps))} FPS`
    frameCount = 0; lastFps = now
  }
}
requestAnimationFrame(animate)

// 属性面板
// 属性面板：各面板 id 对应的标题；以下是生成面板 HTML 的小工具函数（返回 HTML 字符串，t() 为界面翻译）。
const panelTitles = { scene: '场景', light: '太阳与天空', sensor: '传感器', objects: '场景对象', materials: '材质属性', atmosphere: '大气影响', meteorology: '气象驱动', fluid: '流体力学', simulation: '仿真模拟', 'shared-geo': '地理与时间', 'shared-dem': 'DEM 地形' }
function inputUnit(id, value, unit, attrs = '') { return `<div class="input-with-unit"><input class="input" id="${id}" value="${escapeHtml(value)}" ${attrs}><span>${unit}</span></div>` }
function field(label, content) { return `<div class="field"><label>${t(label)}</label>${content}</div>` }
function switchRow(label, id, on, disabled = false) { return `<div class="switch-row"><span>${t(label)}</span><button type="button" class="switch ${on ? 'on' : ''}" id="${id}" role="switch" aria-checked="${on}" ${disabled ? 'disabled' : ''}></button></div>` }
function builtinNotice() { return state.inputPath ? '' : '<div class="property-group"><div class="notice">尚未打开工程。请点击“打开工程”，选择已有工程的 project.json。</div></div>' }

// 作用：生成材质库中一张卡片的 HTML；values 为 [名称, 值] 列表，kind/index 写入 data 属性供点击编辑，
// 并把标题与参数拼成小写搜索串写入 data-library-search（筛选框使用）。
function libraryCard(title, subtitle, values, kind, index) {
  const detail = values.map(([key, value]) => `${key}: ${value}`).join('；')
  const search = `${title} ${subtitle} ${detail}`.toLowerCase()
  return `<button class="library-card" type="button" data-library-search="${escapeHtml(search)}" data-preset-kind="${kind}" data-preset-index="${index}" title="点击修改 · ${escapeHtml(`${title} · ${subtitle}\n${detail}`)}"><strong>${escapeHtml(title)}</strong><small>${escapeHtml(subtitle)}</small><div class="library-values">${values.map(([key, value]) => `<span><i>${escapeHtml(key)}</i>${escapeHtml(value)}</span>`).join('')}</div></button>`
}

// 作用：把“物化属性”预设（item）转成卡片展示的 [名称, 值] 列表。
// 说明：按 energyModel（光伏/木质/薄膜）与 type（植被/水体/船舶/载具/其他）分支；
// 墙体厚度 0.2 m、室内 22 ℃、h=8 W/(m²·K)、船舶水侧 h=500、座舱通风 0.5 ACH、空调 3500 W / 24 ℃、
// 水体初温 20 ℃ 等缺省值已对照引擎核实一致（fileio_json.cpp 的船体墙板与水线、facetrtcore.cpp 写 XML 处
// 的 thickness/indoorTemperature/ventilationACH/acPower/acSetpoint 缺省），与 project-schema 缺省也相同；
// 这里只在字段缺失时用于界面显示，不改变传给引擎的值。
function physicalValues(item) {
  const p = item.params || {}
  if (item.energyModel === 'photovoltaic') return [['模型','光伏热电耦合'],['参考效率',p.eta25],['温度系数',p.gamma],['背面系数',p.bifaciality],['面热容',p.heatCapacityPerArea],['对流倍率',p.convectiveScale]]
  if (item.energyModel === 'wood') return [['模型', '木质固体'], ['比热容', p.cs], ['密度', p.rhos], ['导热率', p.lambdas], ['对流倍率', p.convectiveScale], ['初温', `${p.Tsoil} ℃`], ['蒸腾', '关闭']]
  if (item.energyModel === 'membrane') return [['模型', '双面薄膜'], ['面热容', p.heatCapacityPerArea], ['对流倍率', p.convectiveScale], ['蒸腾', '关闭']]
  if (item.type === 'Vegetation') return [['Vcmax', p.Vcmax], ['BallBerry', p.BallBerry], ['kV', p.kV], ['Rd', p.Rdparam], ['Tyear', p.Tyear], ['胁迫', p.stressfactor]]
  if (item.type === 'Water') return [['辐射模型', Number(p.brdfModel) === 1 ? 'Cox–Munk' : 'Lambert'], ['折射率', p.refractiveIndex], ['坡度方差', Number(p.slopeVariance) > 0 ? p.slopeVariance : '随风速'], ['漫反射比例', p.diffuseFraction], ['rss', p.rss], ['热容', p.heatCapacity], ['混合深度', p.mixingDepth], ['蒸发系数', p.evaporationCoefficient], ['初始水温', p.initialTemperatureMode === 'dailyMeanAir' ? '首日平均气温' : p.initialTemperatureMode === 'fixed' ? `${p.initialTemperature ?? 20} ℃` : '首节点气温']]
  return [['rss', p.rss], ['热容', p.cs], ['密度', p.rhos], ['导热率', p.lambdas], ['SMC', p.SMC], ['饱和量', p.Satwater],
    ...(p.wallMode === 'wall' ? [['墙体模式', `${Array.isArray(p.layers) && p.layers.length ? `${p.layers.length} 层构造 ${p.layers.reduce((sum, layer) => sum + Number(layer.thickness || 0), 0).toFixed(3)} m` : `墙体 ${p.thickness ?? 0.2} m`}，室内 ${p.indoorTemperature ?? 22} ℃，h=${p.indoorHeatTransferCoefficient ?? 8}`]] : []),
    ...(item.type === 'Ship' && Number(p.speed) > 0 ? [['航速', `${p.speed} m/s`]] : []),
    ...(item.type === 'Ship' && shipValueGiven(p.waterlineHeight) ? [['水线', `${p.waterlineHeight} m，水侧 h=${p.waterHeatTransferCoefficient ?? 500}，水温 ${shipValueGiven(p.waterTemperature) ? `${p.waterTemperature} ℃` : '场景水体'}`]] : []),
    ...(item.type === 'Ship' && !shipValueGiven(p.waterlineHeight) && p.waterlineSource === 'water' ? [['水线', `场景水面${Number(p.waterlineOffset) ? ` ${p.waterlineOffset > 0 ? '+' : ''}${p.waterlineOffset} m` : ''}（吃水），水侧 h=${p.waterHeatTransferCoefficient ?? 500}`]] : []),
    ...(item.type === 'Ship' && shipValueGiven(p.heading) && shipValueGiven(p.windDirection) ? [['航向/风来向', `${p.heading}° / ${p.windDirection}°`]] : []),
    ...(item.type === 'Ship' && (p.waterShielding || p.waterFeedback) ? [['水体耦合', [p.waterShielding ? '水下遮蔽' : '', p.waterFeedback ? '反馈水温' : ''].filter(Boolean).join('、')]] : []),
    ...(item.type === 'Ship' && (Number(p.internalHeatFlux) || Number(p.internalHeatPower)) ? [['内热源', [Number(p.internalHeatFlux) ? `${p.internalHeatFlux} W/m²` : '', Number(p.internalHeatPower) ? `${p.internalHeatPower} W` : ''].filter(Boolean).join(' + ')]] : []),
    ...(item.type === 'Vehicle' && p.vehiclePart ? [['部件', { body: '车身（含座舱参数）', glass: '车窗玻璃', underbody: '底盘/轮胎/灯具薄壳', hood: '引擎盖（内侧=发动机舱空气）' }[p.vehiclePart] || p.vehiclePart]] : []),
    ...(item.type === 'Vehicle' && p.vehiclePart === 'body' && p.cabin ? [['座舱', p.cabin.enabled ? `开启，${p.cabin.occupants ?? 0} 人，车速 ${p.cabin.speed ?? 0} m/s，通风 ${p.cabin.ventilationACH ?? 0.5} ACH${p.cabin.acOn ? `，空调 ${p.cabin.acPower ?? 3500} W/${p.cabin.acSetpoint ?? 24} ℃` : ''}${p.cabin.engineOn ? '，发动机开' : ''}` : '关闭']] : [])]
}

// 判断船舶参数是否“已给定”（非空且为有限数）；未给定则按场景水体/缺省处理。
function shipValueGiven(value) { return value !== undefined && value !== null && value !== '' && Number.isFinite(Number(value)) }

// Mirrors withLeafAngles in models/histream/src/base/compo.cpp: how VoxelRT /
// VoxelEB obtain the leaf projection G of a canopy.
//   lidf: G(Ω) and the phase function come from the SAIL LIDF (LIDFa, LIDFb)
//   spherical: LIDFa > 1, exact spherical distribution (G = 0.5)
//   constant: rigid/medium or LIDFa = LIDFb = 0 (unspecified) -> stored G
//   invalid: |LIDFa| + |LIDFb| > 1 -> stored G (the engine prints a warning)
function canopyLeafAngleMode(item = {}) {
  if ((item.structureType || 'canopy') !== 'canopy') return 'constant'
  const a = Number(item.LIDFa ?? -0.35)
  const b = Number(item.LIDFb ?? -0.15)
  if (!Number.isFinite(a) || !Number.isFinite(b)) return 'invalid'
  if (a === 0 && b === 0) return 'constant'
  if (a > 1) return 'spherical'
  return Math.abs(a) + Math.abs(b) > 1 + 1e-6 ? 'invalid' : 'lidf'
}

// 结构参数面板中 G 值的说明文字，键对应 canopyLeafAngleMode 的返回值。
const CANOPY_G_HINTS = Object.freeze({
  lidf: 'G 由叶倾角分布 LIDFa/LIDFb（SAIL 13 区间）按方向推导，消光与叶片散射相函数一致；上面的 G 值仅保留，不参与计算。',
  spherical: 'LIDFa > 1 表示精确球形叶倾角分布：G = 0.5，使用 Ross 球形闭式；上面的 G 值不参与计算。',
  constant: '使用上面的常数 G（刚体、参与介质或 LIDFa = LIDFb = 0 未指定叶倾角时）；散射相函数为球形形状并按 G/0.5 归一，保持能量守恒。',
  invalid: 'LIDFa/LIDFb 超出 SAIL 定义域（|a| + |b| ≤ 1），引擎回退为上面的常数 G 与球形形状相函数。'
})

// 作用：把结构参数（冠层/刚体/火焰/雾）预设转成卡片展示的 [名称, 值] 列表。
// 说明：火焰与雾显示消光、散射率等介质参数；冠层按 canopyLeafAngleMode 决定 G 显示为 LIDF/球形/常数。
function canopyValues(item) {
  const labels = { canopy: '混沌介质', rigid: '刚体', fire: '火焰', fog: '雾' }
  if (item.structureType === 'fire' || item.structureType === 'fog') return [
    ['结构', labels[item.structureType]], ['消光', `${item.extinction} m⁻¹`], ['散射率', item.scatteringAlbedo],
    ['非对称', item.asymmetry], ['温度', `${item.fixedTemperature} K`], ['发射倍率', item.emissionScale]
  ]
  const mode = canopyLeafAngleMode(item)
  const gLabel = mode === 'lidf' ? `LIDF (${item.LIDFa ?? -0.35}, ${item.LIDFb ?? -0.15})`
    : mode === 'spherical' ? '0.5（球形）' : item.G
  return [
    ['结构', labels[item.structureType] || '混沌介质'],
    ['LAI', item.lai], ['LAD', item.density], ['高度', `${item.hc} m`],
    ['G', gLabel], ['热点', item.hspot]
  ]
}

// 作用：材质库分组标题（含数量、筛选输入框与“新增”按钮）的 HTML。
function libraryHeading(title, count, kind) {
  return `<div class="library-heading"><h3>${escapeHtml(title)} · ${count}</h3><div class="library-actions"><input class="input library-filter" data-library-filter="${kind}" placeholder="筛选 ${escapeHtml(title)}"><button class="button ghost library-add" type="button" data-preset-kind="${kind}"><svg><use href="#i-plus"/></svg>新增</button></div></div>`
}

// 作用：拼出“材质属性”面板：光谱特征、温度特征、结构参数、物化属性四组预设卡片。
// 输入：工程配置 c（spectra/thermals/canopies/materials）；点击卡片编辑，保存后写回 project.json。
function materialLibraryHtml(c) {
  const previewLabels = Object.fromEntries(PREVIEW_TEXTURE_PRESETS)
  const spectra = c.spectra.map((item, index) => libraryCard(item.label || item.name, `${item.name} · ${item.model}`, [['反射率', item.reflectance], ['透射率', item.transmittance], ['TIR反射', item.refTir], ...(item.physicalTexture?.enabled ? [['物理纹理', Number(item.physicalTexture.relativeStd) > 0 ? `CV ${item.physicalTexture.relativeStd}` : `强度 ${item.physicalTexture.strength}`]] : []), ...(Number(item.openness) > 0 ? [['开孔率', item.openness]] : []), ...(item.classTexture?.enabled ? [['分类纹理', `${item.classTexture.classes.filter((entry) => entry.spectralName).length} 类`]] : []), ...(item.opennessTexture?.enabled ? [['开孔纹理', `${item.opennessTexture.minOpenness}–${item.opennessTexture.maxOpenness}`]] : []), ['预览', item.previewTexture?.enabled ? (previewLabels[item.previewTexture.preset] || item.previewTexture.preset) : '关闭']], 'spectrum', index)).join('')
  const thermals = c.thermals.map((item, index) => libraryCard(item.label || item.name, item.name, [['阳面', `${item.sunlitTemperature} K`], ['阴面', `${item.shadedTemperature} K`]], 'thermal', index)).join('')
  const canopies = c.canopies.map((item, index) => libraryCard(item.label || item.name, item.name, canopyValues(item), 'canopy', index)).join('')
  const types = { Vegetation: '植被生理生化', Soil: '土壤表面物化', Water: '水体物性', Ship: '船舶表面物化', Vehicle: '载具物化（座舱）' }
  const materials = c.materials.map((item, index) => libraryCard(item.label || item.name, `${item.name} · ${item.energyModel === 'wood' ? '木质固体热平衡' : item.energyModel === 'membrane' ? '篷布/织物薄膜' : (types[item.type] || item.type)}`, physicalValues(item), 'physical', index)).join('')
  return `<div class="property-group">${libraryHeading('光谱特征', c.spectra.length, 'spectrum')}<div class="library-list" data-library-list="spectrum">${spectra}</div></div><div class="property-group">${libraryHeading('温度特征', c.thermals.length, 'thermal')}<div class="library-list" data-library-list="thermal">${thermals}</div></div><div class="property-group">${libraryHeading('结构参数', c.canopies.length, 'canopy')}<div class="library-list" data-library-list="canopy">${canopies}</div></div><div class="property-group">${libraryHeading('物化属性', c.materials.length, 'physical')}<div class="library-list" data-library-list="physical">${materials}</div></div><div class="property-group"><div class="notice">点击任一属性可直接修改；保存后同步更新当前工程的 project.json。</div></div>`
}

// 是否面元模式（FacetRT 或 FacetEB）；面元与体元使用不同的计算后端字段与 GPU 编号。
function isFacetMode(mode = state.mode) { return mode === 'eFacetRT' || mode === 'eFacetEB' }

// 作用：取当前模式对应的后端（面元用 facetBackend，体元用 computeBackend）；仅识别 cpu/auto，其余一律视为 gpu。
function selectedBackend(control) {
  const value = isFacetMode() ? control.facetBackend : control.computeBackend
  return ['cpu', 'auto'].includes(value) ? value : 'gpu'
}

// 作用：刷新左侧“计算设备”下拉框与 CPU 线程数/提示文字。
// 输入：state.config.control（后端、gpu 序号、cpuThreads）、state.devices（设备检测报告）。
// 说明：面元模式与体元模式使用不同的 GPU 编号（facetIndex / voxelIndex），
// 编号为负表示该 GPU 不满足对应模式要求，只显示为禁用项。
function renderComputeDevices() {
  const select = $('#computeDevice')
  const control = state.config?.control
  if (!select || !control) return
  const facet = isFacetMode()
  const backend = selectedBackend(control)
  const gpuIndex = Math.max(0, Math.round(Number(control.gpu) || 0))
  const report = state.devices
  const indexKey = facet ? 'facetIndex' : 'voxelIndex'
  const options = []
  for (const gpu of (report?.gpus || []).filter((item) => item[indexKey] >= 0).sort((a, b) => a[indexKey] - b[indexKey])) {
    options.push({ value: `gpu:${gpu[indexKey]}`, label: `GPU ${gpu[indexKey]} · ${gpu.name}` })
  }
  if (!options.some((item) => item.value === `gpu:${gpuIndex}`)) {
    options.push({ value: `gpu:${gpuIndex}`, label: `GPU ${gpuIndex}${report ? `（${t('未检测到')}）` : ''}` })
  }
  for (const gpu of (report?.gpus || []).filter((item) => item[indexKey] < 0)) {
    options.push({ value: '', label: `${gpu.name}（${facet ? t('不满足面元 GPU 要求') : t('不支持硬件光线追踪')}）`, disabled: true })
  }
  // CPU 与“自动”（GPU 不可用时回退 CPU）两个固定选项；体元 CPU 版经 Mesa lavapipe 软件 Vulkan，面元 CPU 版为原生多线程栅格。
  options.push({ value: 'cpu', label: facet ? `CPU · ${t('原生多线程栅格')}` : `CPU · Mesa lavapipe${report && !report.cpu?.lavapipe ? `（${t('驱动未就绪')}）` : ''}` })
  options.push({ value: 'auto', label: `${t('自动')} · GPU ${gpuIndex}，${t('不可用时回退 CPU')}` })
  select.innerHTML = options.map((item) => `<option value="${item.value}"${item.disabled ? ' disabled' : ''}>${escapeHtml(item.label)}</option>`).join('')
  select.value = backend === 'gpu' ? `gpu:${gpuIndex}` : backend
  $('#cpuThreadsRow').hidden = backend === 'gpu'
  // CPU 线程数不超过本机逻辑处理器数；体元 CPU（lavapipe）实际最多用 lavapipeThreadLimit（缺省 32）个线程，超出时在提示里说明。
  const processors = report?.logicalProcessors
  const threads = processors ? Math.min(normalizeCpuThreads(control.cpuThreads), processors) : normalizeCpuThreads(control.cpuThreads)
  $('#cpuThreads').max = String(processors || 1024)
  $('#cpuThreads').value = String(threads)
  const lavapipeLimit = report?.lavapipeThreadLimit || 32
  const used = facet ? threads : Math.min(threads, lavapipeLimit)
  let hint = ''
  if (state.devicesLoading) hint = t('正在检测计算设备…')
  else if (state.devicesError) hint = `${t('设备检测失败')}：${state.devicesError}`
  else if (backend !== 'gpu') {
    hint = `${t('多核并行')}：${used} ${t('线程')}（${t('范围')} 1–${processors || 1024}${processors ? `，${t('本机')} ${processors} ${t('逻辑处理器')}` : ''}）`
    if (used < threads) hint += `。${t('当前 lavapipe 驱动最多使用')} ${lavapipeLimit} ${t('线程')}`
  }
  $('#computeDeviceHint').textContent = hint
}

// 设备检测请求序号：只接受最近一次请求的结果，丢弃过期的异步返回。
let deviceRequest = 0
// 作用：向后端请求计算设备报告（refresh=true 强制重新检测），完成后重绘设备下拉框。
async function loadComputeDevices(refresh = false) {
  const request = ++deviceRequest
  state.devicesLoading = true; state.devicesError = ''; renderComputeDevices()
  let devices = null
  let devicesError = ''
  try { devices = await api.devices(state.executable, refresh) }
  catch (error) { devicesError = error.message }
  if (request !== deviceRequest) return
  state.devices = devices; state.devicesError = devicesError; state.devicesLoading = false
  renderComputeDevices()
}

// 作用：在体元模式与面元模式之间切换时，把已选 GPU 换成另一套编号下的同一块物理 GPU，
// 避免两套编号不一致导致选到别的设备；找不到对应设备时保持原值。
function remapGpuIndex(previousMode, nextMode) {
  const control = state.config?.control
  const facetModes = ['eFacetRT', 'eFacetEB']
  const fromKey = facetModes.includes(previousMode) ? 'facetIndex' : 'voxelIndex'
  const toKey = facetModes.includes(nextMode) ? 'facetIndex' : 'voxelIndex'
  if (!control || fromKey === toKey || !state.devices?.gpus) return
  const current = Math.max(0, Math.round(Number(control.gpu) || 0))
  const gpu = state.devices.gpus.find((item) => item[fromKey] === current)
  if (gpu && gpu[toKey] >= 0 && gpu[toKey] !== current) {
    control.gpu = gpu[toKey]
    syncProjectText()
  }
}

// 作用：按当前面板 state.panel 重新生成右侧属性面板 HTML，写入 #inspectorContent 并绑定事件。
// 输入：state.config（缺省用 defaultConfig()）、state.mode、state.panel。
// 结构：先算若干共用片段（DEM 信息、背景属性），再按面板分支拼 html：
//   shared-geo 地理与时间 / shared-dem DEM 地形 / scene 场景 / light 太阳与天空 / sensor 传感器 /
//   objects 场景对象 / materials 材质 / atmosphere 大气 / meteorology 气象 / fluid 流体 /
//   其余（simulation 仿真模拟）。
// 说明：部分分支会就地给 c.* 补缺省对象（terrainEnvironment、atmosphere、fluid），属兼容旧工程的惰性初始化。
function renderInspector() {
  renderComputeDevices()
  const c = state.config || defaultConfig()
  $('#inspectorTitle').textContent = t(panelTitles[state.panel])
  const background = c.scene.background || {}
  const dem = c.scene.demInfo
  const demNumber = (value, digits = 3) => Number.isFinite(Number(value)) ? Number(value).toLocaleString(getLocale(), { maximumFractionDigits: digits }) : '—'
  const demPixelSize = dem ? `${demNumber(dem.pixelSizeX)} × ${demNumber(dem.pixelSizeY)} ${dem.unit || ''}`.trim() : '—'
  const sceneOffsetX = Number(c.scene.offsetX) || 0
  const sceneOffsetY = Number(c.scene.offsetY) || 0
  const sceneOffsetZ = Number(c.scene.offsetZ) || 0
  // DEM 高差超过场景最大高度时给出警告，提示用户增大“最大高度”。
  const demReliefWarning = dem && Number(dem.relief) > Number(c.scene.height)
    ? `<div class="notice warning">高差 ${demNumber(dem.relief)} m 超过场景最大高度 ${demNumber(c.scene.height)} m，请增大最大高度。</div>` : ''
  const demMetadata = dem ? `<div class="metric-grid"><div class="metric"><strong>${escapeHtml(dem.width)} × ${escapeHtml(dem.height)}</strong><small>栅格列 × 行</small></div><div class="metric"><strong>${escapeHtml(demPixelSize)}</strong><small>像元大小</small></div><div class="metric"><strong>${demNumber(dem.minimum)}–${demNumber(dem.maximum)} m</strong><small>最低–最高高程</small></div><div class="metric"><strong>${dem.noData == null ? '未设置' : demNumber(dem.noData)}</strong><small>NoData</small></div></div><div class="notice">整幅 DEM 自动双线性拉伸至 ${demNumber(c.scene.x)} × ${demNumber(c.scene.y)} m 场景范围。</div>${demReliefWarning}` : ''
  const angularStrength = Math.max(0, Math.min(1, Number(background.angularEffectStrength ?? 0.5)))
  const backgroundPanel = `<div class="property-group"><h3>背景属性</h3>${field('光谱属性', `<select class="select" id="backgroundSpectral">${bindingOptionList(c.spectra, background.spectralName, 'soil')}</select>`)}${field('温度属性', `<select class="select" id="backgroundThermal">${bindingOptionList(c.thermals, background.thermalName, 'soil_temperature')}</select>`)}${field('物化属性', `<select class="select" id="backgroundMaterial">${bindingOptionList(c.materials, background.materialName, 'soilset')}</select>`)}${switchRow('背景使用镜面反射', 'backgroundSpecularSwitch', Boolean(background.specularEnabled))}</div><div class="property-group"><h3>背景异质性</h3>${switchRow('启用 Hapke 角度效应', 'backgroundHeterogeneitySwitch', Boolean(background.heterogeneityEnabled))}${field('结构强度', `<div class="range-wrap"><input class="range" id="backgroundAngularStrength" type="range" min="0" max="1" step="0.01" value="${angularStrength}" ${background.heterogeneityEnabled ? '' : 'disabled'}><span class="range-value" id="backgroundAngularStrengthValue">${angularStrength.toFixed(2)}</span></div>`)}<div class="notice">光学波段使用 Hapke，热红外使用红外 Hapke；0 为各向同性，1 为完整角度效应。</div></div>`
  let html = ''
  // 三维工程输入：影像模拟（STREAM）使用独立工作台与独立工程，不共享这些值。
  const sharedNotice = `<div class="notice">这些输入只属于当前三维工程；影像模拟（STREAM）使用自己的工程，不读取这里的值。</div>`
  if (state.panel === 'shared-geo') {
    html = `<div class="property-group"><h3>站点位置</h3>${field('纬度', inputUnit('meteoLatitude', c.meteo.latitude, '°', 'type="number" min="-90" max="90" step="0.0001"'))}${field('经度', inputUnit('meteoLongitude', c.meteo.longitude, '°', 'type="number" min="-180" max="180" step="0.0001"'))}${field('时区', inputUnit('meteoTimezone', c.meteo.timezone, 'UTC±h', 'type="number" min="-12" max="14" step="0.25"'))}${field('年份', inputUnit('meteoYear', c.meteo.year, '年', 'type="number" min="-2000" max="6000" step="1"'))}<div class="notice">气象文件中的年积日按此年份和当地时区解释，用于计算太阳位置。</div></div>${sharedNotice}${builtinNotice()}`
  } else if (state.panel === 'shared-dem') {
    // 分支 shared-dem：DEM 地形开关/导入与“地形水热经验场（VoxelEB）”参数，
    // 参数存于 c.scene.terrainEnvironment；经验公式见面板内说明文字。
    const environment = c.scene.terrainEnvironment || (c.scene.terrainEnvironment = normalizeTerrainEnvironment())
    const environmentFields = (rows) => rows.map(([key, label, unit, attributes]) =>
      field(label, inputUnit(`terrainEnv_${key}`, environment[key], unit, `type="number" ${attributes || 'step="any"'}`))).join('')
    const environmentPanel = `<div class="property-group"><h3>地形水热经验场 · VoxelEB</h3>
      ${switchRow('启用气温与水汽高度变化', 'terrainAirSwitch', environment.airEnabled)}
      ${environmentFields([
        ['referenceHeight', '气象参考高度', 'm'],
        ['temperatureLapseRate', '气温递减率', 'K/m'],
        ['vaporScaleHeight', '水汽压尺度高度', 'm', 'min="0.001" step="any"']
      ])}
      ${switchRow('启用土壤水高度变化', 'terrainSoilSwitch', environment.soilEnabled)}
      ${environmentFields([
        ['drainageHeight', '沟谷基准高度', 'm'],
        ['soilDry', '高坡位干端含水量', 'm³/m³', 'min="0" max="1" step="0.01"'],
        ['soilValley', '沟谷含水量', 'm³/m³', 'min="0" max="1" step="0.01"'],
        ['soilScaleHeight', '土壤水衰减尺度', 'm', 'min="0.001" step="any"']
      ])}
      <div class="notice">高度以 DEM 最低点为零；将气象站海拔减去 DEM 最低海拔后填入参考高度。正递减率表示随高度降温，负值表示逆温。水汽压按指数变化并限制在局地饱和值内，相对湿度由局地温度计算。土壤水采用指定的统一沟谷基准面，未自动提取河网；默认参数未经当地标定，不包含水量收支。</div>
      <div class="notice">T = Tref − Γ(h − href)；e = eref exp[−(h − href)/Hv]；θ = θdry + (θvalley − θdry) exp[−max(0, hground − hdrain)/L]。气温与水汽会进入局地蒸散和叶片生理；土壤水会影响蒸发阻力和土壤热传导。</div>
    </div>`
    html = `<div class="property-group"><h3>DEM 地形</h3>${switchRow('启用 DEM 地形', 'terrainSwitch', c.scene.terrain)}${c.scene.demFile ? `<div class="path-input"><span title="${escapeHtml(c.scene.demFile)}">${escapeHtml(c.scene.demFile)}</span></div>` : ''}${demMetadata}<label class="button ghost" for="demFile" style="width:100%;justify-content:center;margin-top:9px"><svg><use href="#i-import"/></svg>导入 DEM 文件<input id="demFile" type="file" accept=".tif,.tiff,.asc,image/tiff,text/plain" hidden></label></div><div class="property-group"><div class="notice">DEM 用于三维地形网格。</div></div>${environmentPanel}${sharedNotice}${builtinNotice()}`
  } else if (state.panel === 'scene') {
    // 分支 scene：空间范围、计算域偏移与背景属性。注意界面坐标：X 向北、Z 向东、Y 向上，
    // 而 c.scene.x/y/height 分别对应 X 尺寸、Z 尺寸（东西）、Y 最大高度。
    const domainNotice = `Three.js 显示全部实例；运行时跳过完整 OBJ 包围盒与计算域 X=[${formatSceneDimension(sceneOffsetX)}, ${formatSceneDimension(sceneOffsetX + Number(c.scene.x))}]、Y=[${formatSceneDimension(sceneOffsetY)}, ${formatSceneDimension(sceneOffsetY + Number(c.scene.height))}]、Z=[${formatSceneDimension(sceneOffsetZ)}, ${formatSceneDimension(sceneOffsetZ + Number(c.scene.y))}] m 无交集的实例；跨界对象仅计算域内部分。`
    html = `<div class="property-group"><h3>空间范围</h3>${field('X 尺寸（南北）', inputUnit('sceneX', c.scene.x, 'm', 'type="number" min="1"'))}${field('Z 尺寸（东西）', inputUnit('sceneY', c.scene.y, 'm', 'type="number" min="1"'))}${field('Y 最大高度', inputUnit('sceneHeight', c.scene.height, 'm', 'type="number" min="0.1"'))}${field('体元大小', inputUnit('voxelSize', c.scene.voxel, 'm', 'type="number" min="0.1" step="0.1"'))}${field('OBJ填充阈值', inputUnit('voxelFillThreshold', c.scene.voxelFillThreshold, '0–1', 'type="number" min="0" max="1" step="0.01"'))}<div class="notice">统一方向：+X 向北、−X 向南；+Y 向上；+Z 向东、−Z 向西。OBJ、Three.js 与计算引擎采用同一约定；方位角 0° 向北、90° 向东；平行投影固定上北、右东，中心投影保持天空向上和地平线水平。</div></div><div class="property-group"><h3>计算域偏移</h3>${field('X 偏移（北向）', inputUnit('sceneOffsetX', sceneOffsetX, 'm', 'type="number" step="any"'))}${field('Y 偏移（高度）', inputUnit('sceneOffsetY', sceneOffsetY, 'm', 'type="number" step="any"'))}${field('Z 偏移（东向）', inputUnit('sceneOffsetZ', sceneOffsetZ, 'm', 'type="number" step="any"'))}<div class="notice">${domainNotice}</div></div>${backgroundPanel}${builtinNotice()}`
  } else if (state.panel === 'light') {
    // 分支 light：太阳位置与辐照条件。EB 模式显示气象短波直散拆分；
    // RT 模式显示地表水平短波总辐照（仅影响热红外波段的太阳反射绝对量）。
    html = `<div class="property-group"><h3>太阳位置</h3>${field('太阳天顶角', inputUnit('sunZenith', c.light.zenith, '°', 'type="number" min="0" max="90"'))}${field('太阳方位角', inputUnit('sunAzimuth', c.light.azimuth, '°', 'type="number" min="0" max="360"'))}</div>
      <div class="property-group"><h3>辐照条件</h3>${field('直射比例', `<div class="range-wrap"><input class="range" id="directLight" type="range" min="0" max="1" step="0.01" value="${c.light.direct}"><span class="range-value" id="directValue">${c.light.direct.toFixed(2)}</span></div>`)}${field('漫射比例', `<div class="range-wrap"><input class="range" id="diffuseLight" type="range" min="0" max="1" step="0.01" value="${c.light.diffuse}"><span class="range-value" id="diffuseValue">${c.light.diffuse.toFixed(2)}</span></div>`)}${field('天空温度', inputUnit('skyTemperature', c.light.skyTemperature, 'K', 'type="number"'))}<div class="notice">HiStream 自动令漫射比例 = 1 − 直射比例。</div>${['eVoxelEB', 'eFacetEB'].includes(state.mode) ? `${field('气象短波直散拆分', `<select class="select" id="diffuseFractionMode"><option value="erbs" ${c.light.diffuseFractionMode === 'erbs' ? 'selected' : ''}>逐节点（实测列或 Erbs 模型）</option><option value="fixed" ${c.light.diffuseFractionMode !== 'erbs' ? 'selected' : ''}>固定为上面的直射比例（旧工程兼容）</option></select>`)}<div class="notice">逐节点：气象文件第 8、9 列给出水平直射与散射短波（W/m²）时按其比例拆分 Rin；否则由 Rin 与大气顶辐照（太阳天顶角、日地距离）求晴空指数 kt，按 Erbs (1982) 求散射份额（晴天正午 kt≈0.75 时散射约 0.18）；夜间与太阳高度 < 1° 全部按漫射。固定：全时段用直射比例。运行日志每个节点打印所用直射份额（SHORTWAVE_SPLIT）。</div>` : ''}${['eRaytracing', 'eVoxelRT', 'eFacetRT'].includes(state.mode) ? `${field('地表水平短波总辐照', inputUnit('shortwaveIrradiance', c.light.shortwaveIrradiance ?? 0, 'W/m²', 'type="number" min="0" max="2000" step="10"'))}<div class="notice">只用于 2.5–6 µm 热红外波段的太阳反射绝对量（按上面的直射/漫射比例拆分）；0 = 默认（${state.mode === 'eFacetRT' ? '700·cosθs + 50 W/m²' : '晴空 Haurwitz 公式'}）。可见光–短波红外输出是归一化反射率，不受影响。</div>` : ''}</div>${builtinNotice()}`
  } else if (state.panel === 'sensor') {
    // 分支 sensor：观测方向、采样方式、成像参数；中心投影时额外显示位置/视场角/航点巡航。
    // 步骤 1：预览巡航位置（最多 8 个；更多时只列前 5 个和最后 2 个，中间用省略号）。
    const perspective = c.sensor.projection === 'perspective'
    updateCruiseVisualization(c)
    const cruisePositions = perspective && c.sensor.cruiseEnabled ? sensorCruisePositions(c.sensor, c.scene) : []
    const previewIndices = cruisePositions.length <= 8
      ? cruisePositions.map((_, index) => index)
      : [0, 1, 2, 3, 4, cruisePositions.length - 2, cruisePositions.length - 1]
    const cruisePreview = cruisePositions.length
      ? `<div class="notice"><strong>预计巡航位置 · 共 ${cruisePositions.length} 个</strong><br>${previewIndices.map((index, previewIndex) => {
          const point = cruisePositions[index]
          const separator = previewIndex === 5 && cruisePositions.length > 8 ? '…　' : ''
          return `${separator}P${index + 1} · X/Y/Z (${demNumber(point.x)}, ${demNumber(point.z)}, ${demNumber(point.y)}) m`
        }).join('　')}</div>`
      : ''
    // 步骤 2：按巡航轨迹类型（直线往返/中心矩形/随机）生成各自的输入项，未知值回退为 line。
    const cruiseRoute = ['line', 'rectangle', 'random'].includes(c.sensor.cruiseRoute) ? c.sensor.cruiseRoute : 'line'
    const cruiseRouteOptions = `<select class="input" id="sensorCruiseRoute"><option value="line" ${cruiseRoute === 'line' ? 'selected' : ''}>直线往返</option><option value="rectangle" ${cruiseRoute === 'rectangle' ? 'selected' : ''}>中心矩形</option><option value="random" ${cruiseRoute === 'random' ? 'selected' : ''}>随机飞行</option></select>`
    const cruiseRouteFields = cruiseRoute === 'line'
      ? `${field('目标位置 X（北向）', inputUnit('sensorCruiseTargetX', c.sensor.cruiseTargetX, 'm', 'type="number"'))}${field('目标位置 Z（东向）', inputUnit('sensorCruiseTargetY', c.sensor.cruiseTargetY, 'm', 'type="number"'))}${field('目标位置 Y（高度）', inputUnit('sensorCruiseTargetZ', c.sensor.cruiseTargetZ, 'm', 'type="number" min="0.01"'))}`
      : cruiseRoute === 'rectangle'
        ? `${field('矩形长度 X（南北）', inputUnit('sensorCruiseRectangleWidth', c.sensor.cruiseRectangleWidth, 'm', 'type="number" min="0.01"'))}${field('矩形宽度 Z（东西）', inputUnit('sensorCruiseRectangleHeight', c.sensor.cruiseRectangleHeight, 'm', 'type="number" min="0.01"'))}`
        : '<button class="button ghost" id="randomCruiseBtn" type="button" style="width:100%;justify-content:center;margin:8px 0"><svg><use href="#i-camera"/></svg>换一条随机路线</button>'
    const cruiseFields = c.sensor.cruiseEnabled
      ? `${field('巡航轨迹', cruiseRouteOptions)}${cruiseRouteFields}${field('航点间距', inputUnit('sensorCruiseStep', c.sensor.cruiseStep, 'm', 'type="number" min="0.01" step="any"'))}${field('航点数量', inputUnit('sensorCruiseCount', c.sensor.cruiseCount, '个', 'type="number" min="1" max="10000" step="1"'))}<div class="notice">直线轨迹在起点与目标点之间三维往返，高度随航程线性变化；矩形和随机轨迹保持当前高度。越界后从对侧继续。</div>${cruisePreview}`
      : ''
    const sensorHeightLabel = c.sensor.cruiseEnabled ? (cruiseRoute === 'line' ? '起始位置 Y（高度）' : '巡航高度 Y') : '观测位置 Y（高度）'
    const projectionFields = perspective
      ? `${field(cruiseRoute === 'rectangle' && c.sensor.cruiseEnabled ? '矩形中心 X（北向）' : '起始位置 X（北向）', inputUnit('sensorPositionX', c.sensor.positionX, 'm', 'type="number"'))}${field(cruiseRoute === 'rectangle' && c.sensor.cruiseEnabled ? '矩形中心 Z（东向）' : '起始位置 Z（东向）', inputUnit('sensorPositionY', c.sensor.positionY, 'm', 'type="number"'))}${field(sensorHeightLabel, inputUnit('sensorHeight', c.sensor.height, 'm', 'type="number" min="0.01"'))}${field('垂直视场角', inputUnit('sensorFov', c.sensor.fov, '°', 'type="number" min="0.1" max="120" step="0.1"'))}${switchRow('启用航点巡航', 'sensorCruiseSwitch', c.sensor.cruiseEnabled)}${cruiseFields}<div class="notice">视场方向见上方“观测方向”。</div>`
      : '<div class="notice">平行投影的观测方向与采样方式见上方“观测方向”。</div>'
    // 步骤 3：统计输出波段数与平行投影的观测方向数，用于下面的说明文字。
    const bandCount = sensorBandValues(c.sensor).length
    const angleCount = perspective ? 1 : sensorViewAngles(c.sensor, c.light.azimuth).length
    const continuousFields = c.sensor.continuousBands
      ? `${field('起始波段', inputUnit('bandStart', c.sensor.bandStart, 'nm', 'type="number" min="0.000001" step="any"'))}${field('结束波段', inputUnit('bandEnd', c.sensor.bandEnd, 'nm', 'type="number" min="0.000001" step="any"'))}${field('波段间隔', inputUnit('bandStep', c.sensor.bandStep, 'nm', 'type="number" min="0.000001" step="any"'))}`
      : ''
    const projectionNotice = perspective
      ? (c.sensor.cruiseEnabled
          ? '相机方向随飞机航向旋转。天顶角 0° 表示垂直向下；相对飞机方位角 0° 向前、90° 向右、180° 向后、270° 向左。倾斜观测保持天空向上、地平线水平；正下视保持上北、右东。'
          : '中心投影使用观测位置、巡航高度、视场角和视场方向；天顶角 0° 表示垂直向下。倾斜观测保持天空向上、地平线水平；正下视保持上北、右东。')
      : `平行投影共 ${angleCount} 个观测方向。主平面批量范围为 −75°～75°、间隔 5°，同时包含太阳主平面和垂直太阳主平面；半球采用 128 个等面积方向并限制在 VZA 0°～75°。输出统一为上北、右东。`
    html = `<div class="property-group"><h3>观测方向</h3>${field('观测天顶角', inputUnit('viewZenith', c.sensor.vza, '°', 'type="number" min="0" max="89.999" step="any"'))}${field('观测方位角', inputUnit('viewAzimuth', c.sensor.vaa, '°', 'type="number" min="0" max="360" step="any"'))}<div class="notice">方位角从正北顺时针。平行投影按该方向成像；中心投影下它是视场指向（巡航时表示相对飞机方位角）。</div></div><div class="property-group"><h3>采样方式</h3>${switchRow('太阳主平面与垂直主平面', 'principalPlaneSwitch', c.sensor.principalPlane)}${switchRow('128 方向半球观测', 'hemisphereSwitch', c.sensor.hemisphere)}</div><div class="property-group"><h3>成像参数</h3>${field('图像分辨率', `<div class="input-row"><input class="input" id="sensorX" type="number" value="${escapeHtml(c.sensor.x)}"><input class="input" id="sensorY" type="number" value="${escapeHtml(c.sensor.y)}"></div>`)}${field('自定义波段 [nm]', `<input class="input" id="sensorBands" value="${escapeHtml(c.sensor.bands)}">`)}${switchRow('连续波段模拟', 'continuousBandsSwitch', c.sensor.continuousBands)}${continuousFields}<div class="notice">最终输出 ${bandCount} 个波段：连续与自定义波段合并、去重并按波长升序排列；最多 ${MAX_SENSOR_BANDS} 个波段。</div>${field('投影方式', `<select class="input" id="sensorProjection"><option value="parallel" ${perspective ? '' : 'selected'}>平行投影</option><option value="perspective" ${perspective ? 'selected' : ''}>中心投影</option></select>`)}${projectionFields}</div><div class="property-group"><div class="notice">${projectionNotice}</div></div>${builtinNotice()}`
  } else if (state.panel === 'objects') {
    // 分支 objects：场景对象列表，分 OBJ 导入实体 / PRIM 原型几何 / 参与介质（火焰、雾）三组。
    // objectCard 生成单个对象卡片；介质对象仅在 VoxelRT/VoxelEB 模式可新增。
    const objectCard = (item, index) => {
      const prim = isPrimObject(item)
      const meshBindings = (item.meshes || []).map((mesh) => `${mesh.name}: ${mesh.spectralName} / ${mesh.thermalName} / ${mesh.materialName || item.materialName} / ${mesh.canopyName || item.canopyName}`).join('；') || '无 Mesh 映射'
      const generatedShape = item.type === 'Water'
        ? (item.generation?.shape || item.shape) === 'ellipsoid' ? '圆形/椭圆水面' : '矩形水面'
        : (item.generation?.shape || item.shape) === 'ellipsoid' ? '椭球' : '立方体'
      const generatedGeometry = prim
        ? `<small>${escapeHtml(item.generation?.type || 'PRIM 原型')} · ${generatedShape} · ${(item.dimensions || []).map((value) => Number(value).toLocaleString('zh-CN')).join(' × ')} m</small>`
        : ''
      return `<div class="object-card ${state.selectedObjectIndex === index ? 'active' : ''}" data-index="${index}" role="button" tabindex="0">
        <div class="object-card-head"><strong>${escapeHtml(item.name)}</strong><div class="object-card-actions"><button class="button ghost object-material" type="button">属性</button><button class="button ghost object-distribution" type="button">分布</button>${!prim && !isMediumObject(item) && item.fileName && !item.tarpCoverOf ? '<button class="button ghost object-tarp" type="button" title="按该对象外形生成贴合的篷布罩">篷布罩</button>' : ''}<button class="button ghost danger object-delete" type="button">删除</button></div></div>
        ${generatedGeometry}
        <small title="${escapeHtml(meshBindings)}">属性 · ${escapeHtml(item.materialName || '未定义物化属性')} · ${escapeHtml(item.canopyName || '未定义结构参数')} · ${escapeHtml(meshBindings)}</small>
        <small>分布 · ${escapeHtml(distributionSummary(item))}</small>
        <small title="${escapeHtml(item.fileName)}">${prim ? 'PRIM 网格' : 'OBJ 文件'} · ${escapeHtml(item.fileName || '无网格文件')}</small>
      </div>`
    }
    const indexedObjects = (c.objects.items || []).map((item, index) => ({ item, index }))
    const importedCards = indexedObjects.filter(({ item }) => !isPrimObject(item) && !isMediumObject(item)).map(({ item, index }) => objectCard(item, index)).join('')
    const primitiveCards = indexedObjects.filter(({ item }) => isPrimObject(item) && !isMediumObject(item)).map(({ item, index }) => objectCard(item, index)).join('')
    const mediumCards = indexedObjects.filter(({ item }) => isMediumObject(item)).map(({ item, index }) => objectCard(item, index)).join('')
    const mediumDisabled = state.mode === 'eVoxelRT' || state.mode === 'eVoxelEB' ? '' : 'disabled'
    html = `<div class="property-group"><h3>场景统计</h3><div class="metric-grid"><div class="metric"><strong>${escapeHtml(c.objects.count || '—')}</strong><small>计算对象</small></div><div class="metric"><strong>${c.spectra.length}</strong><small>光谱定义</small></div></div></div>
      <div class="property-group"><h3>OBJ 导入实体</h3>${importedCards || '<div class="notice">尚未导入 OBJ 实体。</div>'}<button class="button ghost" id="objectImportAction" style="width:100%;justify-content:center;margin-top:8px"><svg><use href="#i-import"/></svg>导入 OBJ 实体</button></div>
      <div class="property-group"><h3>PRIM 原型几何</h3>${primitiveCards || '<div class="notice">尚未生成 PRIM 原型。</div>'}<button class="button ghost" id="objectGenerateAction" style="width:100%;justify-content:center;margin-top:8px"><svg><use href="#i-cube"/></svg>新增 PRIM 原型</button></div>
      <div class="property-group"><h3>参与介质场景对象</h3>${mediumCards || '<div class="notice">尚未添加火焰或雾。</div>'}<div class="library-actions"><button class="button ghost" id="objectMediumAction" ${mediumDisabled} style="flex:1;justify-content:center;margin-top:8px"><svg><use href="#i-cloud"/></svg>新增火焰</button><button class="button ghost" id="objectFogAction" ${mediumDisabled} style="flex:1;justify-content:center;margin-top:8px"><svg><use href="#i-cloud"/></svg>新增雾</button></div><div class="notice">火焰和雾参与 VoxelRT / VoxelEB 的体元辐射传输；雾以局部参与介质体积表示，不替代 MODTRAN 大气。</div></div>
      <div class="property-group"><div class="notice">OBJ 用于管理外部导入实体；PRIM 用于管理椭球、立方体、混沌介质和水平水面。对象均可独立设置属性和实例分布。</div></div>`
  } else if (state.panel === 'materials') {
    html = materialLibraryHtml(c)
  } else if (state.panel === 'atmosphere') {
    // 分支 atmosphere：MODTRAN 查找表（LUT）大气参数；未启用时输入项置灰。
    const atmosphere = c.atmosphere || (c.atmosphere = { enabled: false, model: 'midlatitude-summer', waterVapor: 2, aerosol: 'rural', visibility: 23, lutFile: 'assets/atmosphere/simple_modtran_lut.csv' })
    const disabled = atmosphere.enabled ? '' : 'disabled'
    html = `<div class="property-group"><h3>大气辐射传输</h3>${switchRow('启用 MODTRAN 查找表', 'atmosphereSwitch', atmosphere.enabled)}${field('大气类型', `<select class="select" id="atmosphereModel" ${disabled}><option value="tropical" ${atmosphere.model === 'tropical' ? 'selected' : ''}>热带大气</option><option value="midlatitude-summer" ${atmosphere.model === 'midlatitude-summer' ? 'selected' : ''}>中纬度夏季</option><option value="midlatitude-winter" ${atmosphere.model === 'midlatitude-winter' ? 'selected' : ''}>中纬度冬季</option></select>`)}${field('柱状水汽量 PWV', inputUnit('atmosphereWaterVapor', atmosphere.waterVapor, 'cm', `type="number" min="0.5" max="5" step="0.1" ${disabled}`))}${field('气溶胶类型', `<select class="select" id="atmosphereAerosol" ${disabled}><option value="rural" ${atmosphere.aerosol === 'rural' ? 'selected' : ''}>乡村型</option><option value="urban" ${atmosphere.aerosol === 'urban' ? 'selected' : ''}>城市型</option></select>`)}${field('能见度', inputUnit('atmosphereVisibility', atmosphere.visibility, 'km', `type="number" min="10" max="50" step="1" ${disabled}`))}<div class="notice">传感器高度和观测角度自动读取，参数在 LUT 节点间插值，覆盖 350–14000 nm。地表短波仅计算路径透过率衰减，不包含大气散射程辐射；热红外包含透过率和程辐射。视场中的天空像元按各自半球天顶角计算；启用时使用 LUT，关闭时使用简化经验天空模型。</div></div>${builtinNotice()}`
  } else if (state.panel === 'meteorology') {
    html = `<div class="property-group"><h3>时间设置</h3>${field('开始节点', `<input class="input" id="meteoStart" type="number" min="0" step="1" value="${escapeHtml(c.meteo.start)}">`)}${field('结束节点', `<input class="input" id="meteoEnd" type="number" min="${Number(c.meteo.start) + 1}" step="1" value="${escapeHtml(c.meteo.end)}">`)}${field('时间步长 dTime', inputUnit('meteoDTime', c.meteo.dTime, 's', 'type="number" min="1" step="1"'))}<div class="notice">气象文件中的年积日按“场景输入 → 地理与时间”的年份与时区解释；节点索引以文件首行为 0。</div></div>
      <div class="property-group"><h3>站点参数</h3>${field('观测高度 z', inputUnit('meteoHeight', c.meteo.z, 'm', 'type="number" min="0" step="0.1"'))}<div class="notice">纬度和经度在“场景输入 → 地理与时间”中设置。</div></div>
      <div class="property-group"><h3>初始状态</h3>${field('初始土壤温度 Tsold', inputUnit('meteoSoilTemperature', c.meteo.Tsold, 'K', 'type="number" min="150" max="400" step="0.1"'))}${field('饱和含水量 SatWater', inputUnit('meteoSatWater', c.meteo.SatWater, '0–1', 'type="number" min="0" max="1" step="0.01"'))}<div class="notice">FacetEB 与 VoxelEB 都以此值（K）作为土壤初温；工程 JSON 中未写 Tsold 时，FacetEB 取首个气象节点气温、VoxelEB 取 300 K。饱和含水量以“材质属性 → 物化属性”的土壤参数为准。</div></div>
      <div class="property-group"><h3>驱动文件</h3><div class="path-input"><span title="${escapeHtml(c.meteo.path)}">${escapeHtml(c.meteo.path)}</span></div><label class="button ghost" for="meteoFile" style="width:100%;justify-content:center;margin-top:9px"><svg><use href="#i-import"/></svg>导入本地 meteo 文件<input id="meteoFile" type="file" accept=".txt,.meteo,.dat,.csv,text/plain" hidden></label><div class="notice">文件将复制到当前工程的 meteorology 目录，并写入 project.json。</div></div>${builtinNotice()}`
  } else if (state.panel === 'fluid') {
    // 分支 fluid：D3Q19 LBM 流体力学参数，当前仅 VoxelEB 模式可用，缺省值写入 c.fluid。
    const fluid = c.fluid || (c.fluid = { enabled: false, voxelSize: Number(c.scene.voxel) || 1, timeStep: 5, pressureIterations: 20, windDirection: 0, buoyancy: 0.033, dragCoefficient: 0.3, thermalCoupling: 1, diffusivity: 0.1, smokeEmission: 0.02, maxVelocity: 30, outputWind: false, outputAirTemperature: false, outputHeight: 2 })
    if (state.mode !== 'eVoxelEB') {
      html = `<div class="property-group"><h3>流体力学</h3><div class="notice">流体力学当前只接入体元能量平衡（VoxelEB）。切换到“体元能量平衡”后，可设置风场、热浮力、树冠阻力以及地表换热。</div></div>${builtinNotice()}`
    } else {
      const disabled = fluid.enabled ? '' : 'disabled'
      const spacing = Math.max(0.1, Number(fluid.voxelSize) || Number(c.scene.voxel) || 1)
      const fluidHeight = Math.max(
        spacing,
        Number(c.scene.height) || 0,
        (Number(fluid.outputHeight) || 2) + spacing * 0.5
      )
      // 流体网格尺寸估算：高度方向至少覆盖场景高度与输出高度 + 半个体元；
      // 显存估算按每单元 236 字节计（经验值，待核对来源）。
      const gridX = Math.max(1, Math.ceil(Number(c.scene.x) / spacing))
      const gridY = Math.max(1, Math.ceil(fluidHeight / spacing))
      const gridZ = Math.max(1, Math.ceil(Number(c.scene.y) / spacing))
      const cellCount = gridX * gridY * gridZ
      const memoryMb = cellCount * 236 / 1048576
      // 输出高度落到的流体体元层号，并换算为该层中心高度（实际输出高度）。
      const outputLayer = Math.max(0, Math.min(gridY - 1, Math.floor((Number(fluid.outputHeight) || 2) / spacing)))
      const resolvedOutputHeight = (outputLayer + 0.5) * spacing
      html = `<div class="property-group"><h3>求解器 · D3Q19 LBM</h3>${switchRow('启用流体力学', 'fluidSwitch', fluid.enabled)}${field('流体体元大小', inputUnit('fluidVoxelSize', spacing, 'm', `type="number" min="0.1" step="0.1" ${disabled}`))}<div class="metric-grid"><div class="metric"><strong>${gridX} × ${gridY} × ${gridZ}</strong><small>空气网格 X × 高 × Y</small></div><div class="metric"><strong>${cellCount.toLocaleString('zh-CN')}</strong><small>空气单元</small></div><div class="metric"><strong>${memoryMb < 1 ? memoryMb.toFixed(2) : memoryMb.toFixed(1)} MB</strong><small>流体状态估算显存</small></div></div><div class="notice">流体采用 D3Q19 BGK 格子玻尔兹曼方法；分布函数、风速、气温和烟雾使用独立稠密网格，辐射传输与能量平衡仍使用场景体元。调大流体体元可显著降低显存占用。</div></div>
        <div class="property-group"><h3>来流边界</h3>${field('来流风向', inputUnit('fluidWindDirection', fluid.windDirection, '°', `type="number" min="0" max="359.9" step="1" ${disabled}`))}${field('数值限速上限', inputUnit('fluidMaxVelocity', fluid.maxVelocity, 'm/s', `type="number" min="0.1" max="200" step="0.5" ${disabled}`))}<div class="notice">来流风速逐时间读取气象驱动；限速上限只用于防止数值发散，不是入射风速或色标上限。0° 沿场景正 Y 方向，90° 沿正 X 方向。</div></div>
        <div class="property-group"><h3>数值求解</h3>${field('最大标量时间步', inputUnit('fluidTimeStep', fluid.timeStep, 's', `type="number" min="0.1" max="60" step="0.1" ${disabled}`))}${field('运动黏度/标量扩散', inputUnit('fluidDiffusivity', fluid.diffusivity, 'm²/s', `type="number" min="0" step="0.01" ${disabled}`))}<div class="notice">每个气象节点至少推进一次来流穿越计算域的 LBM 迭代；物理风速采用固定比例映射到稳定的格子速度，温度与烟雾按这里的最大时间步输运。LBM 直接由密度分布恢复压力，不再执行压力 Jacobi 迭代。</div></div>
        <div class="property-group"><h3>热力与介质耦合</h3>${field('热浮力系数', `<input class="input" id="fluidBuoyancy" type="number" min="0" step="0.001" value="${fluid.buoyancy}" ${disabled}>`)}${field('树冠阻力系数', `<input class="input" id="fluidDrag" type="number" min="0" step="0.05" value="${fluid.dragCoefficient}" ${disabled}>`)}${field('地表换热强度', `<input class="input" id="fluidThermalCoupling" type="number" min="0" step="0.1" value="${fluid.thermalCoupling}" ${disabled}>`)}${field('火焰烟雾源强', `<input class="input" id="fluidSmokeEmission" type="number" min="0" step="0.01" value="${fluid.smokeEmission}" ${disabled}>`)}<div class="notice">建筑和刚体为固体边界，树冠为多孔阻力区；火焰提供温度和烟雾浓度源项。</div></div>
        <div class="property-group"><h3>高度切片输出</h3>${field('输出高度', inputUnit('fluidOutputHeight', fluid.outputHeight, 'm', `type="number" min="0.1" step="0.1" ${disabled}`))}${switchRow('输出风速 GeoTIFF', 'fluidWindOutputSwitch', Boolean(fluid.outputWind), !fluid.enabled)}${switchRow('输出气温 GeoTIFF', 'fluidTemperatureOutputSwitch', Boolean(fluid.outputAirTemperature), !fluid.enabled)}<div class="notice">填写高度会定位到对应流体体元；当前实际输出体元中心约为 ${resolvedOutputHeight.toFixed(2)} m。两个文件均按气象时间节点输出到 output 目录。风速文件包含水平风速大小、X 分量、垂直分量和 Y 分量；气温文件单位为 ℃。建筑内部按固体处理，在风速和气温图中写为 NoData。默认均不输出。</div></div>${builtinNotice()}`
    }
  } else {
    // 分支 simulation（其余面板）：输出选项、计算控制（采样/深度/周期边界/求解方法）、
    // RT–EB 耦合参数与温度模拟方法；各控件只在对应模式下出现。
    const energyBalanceOutput = state.mode === 'eFacetEB' || state.mode === 'eVoxelEB'
    const imageOutputLabel = energyBalanceOutput ? '逐时间输出图像' : '输出图像'
    const processOutputNotice = energyBalanceOutput
      ? state.mode === 'eVoxelEB'
        ? '<div class="notice">辐射过程：短波、长波和净辐射；能量过程：潜热、显热、表面热通量、GPP 和 NPP。GPP 为叶片总光合，NPP 为叶片净同化近似值，暂未扣除根和茎呼吸。</div>'
        : '<div class="notice">辐射过程：短波、长波和净辐射；能量过程：潜热、显热和表面热通量。两类结果可独立输出并用于三维分析。</div>'
      : '<div class="notice">辐射过程保存面元或体元辐射度，用于三维分析。</div>'
    const processOutputSwitches = switchRow('输出辐射过程', 'radiationProcessSwitch', c.sensor.radiationProcess) +
      (energyBalanceOutput ? switchRow('输出能量过程', 'energyProcessSwitch', c.sensor.energyProcess) : '')
    // Only show controls the selected engine actually reads (FacetRT/FacetEB
    // use neither tracing depth nor pixel samples; VoxelRT reads "depth" as a
    // DDA step budget; VoxelEB uses radiationMaxSteps instead; RayTracing has
    // its own scattering-order budget).
    const samplingControl = isFacetMode()
      ? ''
      : field(state.mode === 'eRaytracing' ? '每像元路径数' : '像元采样数（抗锯齿）', `<input class="input" id="sampleCount" type="number" min="1" max="1024" step="1" value="${escapeHtml(c.control.samples)}">`)
    const depthControl = state.mode === 'eRaytracing'
      ? `${field('最大散射次数', `<input class="input" id="raytracingDepth" type="number" min="1" max="64" step="1" value="${escapeHtml(c.control.raytracingDepth ?? 16)}">`)}<div class="notice">每条路径最多计算的散射次数（标准档 16）。第 3 次散射起按俄罗斯轮盘赌无偏终止，深路径代价很小；设得过低会截断多次散射，使植被近红外反射率偏低。</div>`
      : state.mode === 'eVoxelRT'
        ? `${field('体元射线步数预算', `<input class="input" id="rayDepth" type="number" min="1" max="16" value="${escapeHtml(c.control.depth)}">`, '×128')}<div class="notice">每条体元射线最多穿越 数值×128 个体元面（128–2048），不是散射次数；散射阶数见下方“真实碰撞次数”。大场景或小体元时适当增大。</div>`
        : state.mode === 'eVoxelEB'
          ? `${field('体元射线步数上限', `<input class="input" id="radiationMaxSteps" type="number" min="16" max="4096" step="16" value="${escapeHtml(c.control.radiationMaxSteps ?? 64)}">`)}${field('短波散射阶数', `<select class="select" id="shortwaveScatteringOrders">${[[1, '1 阶（快速档）'], [2, '2 阶（标准档）'], [3, '3 阶（高精度档）']].map(([value, label]) => `<option value="${value}" ${Number(c.control.shortwaveScatteringOrders ?? 1) === value ? 'selected' : ''}>${label}</option>`).join('')}</select>`)}<div class="notice">步数上限约束短波、长波和成像射线的最大体元穿越数；运行后“运行质量”中若出现步数截断，应增大该值。显式散射阶数之外的高阶散射用光谱不变理论补偿，增加阶数可提高浓密冠层近红外吸收精度。</div>`
          : ''
    // 周期穿透边界次数限幅到 0–20（0 = 关闭）；非数值按 0 处理。
    const configuredPeriodicCount = Math.max(0, Math.min(20, Math.round(
      Number.isFinite(Number(c.control.periodicTraversalCount))
        ? Number(c.control.periodicTraversalCount) : 0
    )))
    const effectivePeriodicCount = configuredPeriodicCount
    const periodicScopeNotice = isFacetMode()
      ? '面元模式按实例复制邻块（向上取整为 4/8/12/20 块）；可见性栅格的水平窗口只覆盖中心块，邻块只参与遮挡，邻块越多可见性边数越多、运行越慢。'
      : state.mode === 'eRaytracing'
        ? '不会复制邻域场景。1–20 表示每条射线（主视线、散射、太阳阴影、天空）离开一侧边界后从对侧继续查询的最大次数，不占散射次数。'
        : '不会复制邻域场景。1–20 表示射线离开一侧边界后从对侧继续查询的最大次数；成像、太阳、漫射/散射与长波传输都按周期遍历。'
    const periodicBoundaryControl = `${field('周期穿透边界次数', `<input class="input" id="periodicTraversalCount" type="number" min="0" max="20" step="1" value="${effectivePeriodicCount}">`, '0=关闭')}${switchRow('典型天空盒', 'skyboxSwitch', Boolean(c.control.skyboxEnabled))}<div class="notice">0 为关闭。${periodicScopeNotice}天空盒勾选后显示典型蓝天与云层；不勾选时保持默认 COS 天空响应。两项互相独立。</div>`
    const heterogeneousVoxelControl = (state.mode === 'eVoxelRT' || state.mode === 'eVoxelEB')
      ? `${switchRow('使用异质性体元', 'heterogeneousVoxelSwitch', Boolean(c.control.heterogeneousVoxel))}<div class="notice">开启后从 OBJ 面元计算每个体元的三轴聚集指数和体密度，并在 VoxelRT/VoxelEB 消光、散射及热辐射中使用；关闭时保持均匀体元算法。默认关闭。</div>`
      : ''
    const radiationSolverControl = state.mode === 'eFacetRT' || state.mode === 'eVoxelRT'
      ? `${field('辐射求解方法', `<select class="select" id="radiationSolver"><option value="traditional" ${c.control.radiationSolver !== 'accelerated' ? 'selected' : ''}>传统稳健方法</option><option value="accelerated" ${c.control.radiationSolver === 'accelerated' ? 'selected' : ''}>${state.mode === 'eVoxelRT' ? '光谱批处理加速' : '光谱复用加速'}</option></select>`)}<div class="notice">${state.mode === 'eVoxelRT' ? '传统方法逐波段提交并等待 GPU；加速方法每次同时求解最多 4 个相邻波段，共享同一轮几何射线遍历，保留全部射线、光谱参数和物理公式，不进行波段插值。' : '传统方法每个波段从零开始固定迭代；加速方法复用相邻同类型波段的辐射度，并按残差提前收敛。两者使用相同几何、采样和输出定义。'}</div>`
      : ''
    const collisionOrdersControl = state.mode === 'eVoxelRT'
      ? `${field('真实碰撞次数', `<select class="select" id="voxelCollisionOrders">${[[2, '2 次（快速档）'], [3, '3 次（标准档）'], [4, '4 次'], [5, '5 次（高精度档）']].map(([value, label]) => `<option value="${value}" ${Number(c.control.voxelCollisionOrders ?? 2) === value ? 'selected' : ''}>${label}</option>`).join('')}</select>`)}<div class="notice">体元之间的散射逐次显式追踪，其余高阶散射用光谱不变（回碰）理论补偿。与完全收敛的多次散射相比：2 次约 5% 以内，3 次约 2%，4 次约 1%，5 次基本收敛；误差主要出现在近红外等叶片反照率高的密冠层，可见光几乎不受影响。每增加一次多一轮漫射遍历。</div>`
      : ''
    const backendNotice = `<div class="notice">${isFacetMode()
      ? 'GPU / CPU / 自动及 CPU 线程数在左侧“计算设备”中选择。面元 CPU 版为原生多线程软件栅格可见性、CSR 邻接图和 Jacobi 辐射度迭代；FacetEB 在该辐射解上继续多线程计算温度与能量通量，通常比 GPU 慢。'
      : 'GPU / CPU / 自动及 CPU 线程数在左侧“计算设备”中选择。CPU 版通过 Mesa lavapipe 软件 Vulkan 驱动多线程执行与 GPU 相同的着色器和光线追踪算法，速度明显慢于 GPU，建议先用较少采样数试算。'}</div>`
    const couplingGeometry = state.mode === 'eFacetEB' ? '面元' : state.mode === 'eVoxelEB' ? '体元' : ''
    const spectralAcceleration = field('光谱加速宽度', inputUnit('spectralAccelerationWidth', c.control.spectralAccelerationWidth ?? 100, 'nm', 'type="number" min="1" max="1000" step="1"'))
    // RT–EB 耦合说明：VoxelEB 与 FacetEB 的迭代上限和收敛判据不同，具体数值以下面文字为准（待核对与引擎一致）。
    const couplingNotice = state.mode === 'eVoxelEB'
      ? '默认100 nm。直射逐波段积分，漫射与多次散射按窗口内辐照度加权的平均光学属性求解。温度迭代由引擎自动控制（最多 50 次，逐步减小步长），收敛判据为全部体元能量残差 < 2 W/m²。大气稳定度选 Monin–Obukhov 时，按场景显热与地面柱 u* 在迭代中更新 Obukhov 长度（-5 ≤ z/L ≤ 1）并重算气动阻抗，同时要求 z/L 收敛；中性大气与旧版逐位一致。'
      : '默认100 nm。短波净辐射按窗口分别积分直射和漫射，并使用窗口加权光学属性求解后累加；最终影像对传感器选定波段逐个独立求解，不进行输出波段插值。收敛判据：能量残差 ≥ 2 W/m² 的面元 < 0.1%，且任一面元的温度等效残差 ≤ 0.5 K；未收敛节点会在运行质量中标出。'
    // FacetEB 与 VoxelEB 共用 control.stabilityCorrection；字段缺失 = 中性（旧工程兼容）。
    const stabilityControl = field('大气稳定度', `<select class="select" id="stabilityCorrection"><option value="1" ${c.control.stabilityCorrection === true ? 'selected' : ''}>Monin–Obukhov 修正</option><option value="0" ${c.control.stabilityCorrection !== true ? 'selected' : ''}>中性大气（旧工程兼容）</option></select>`)
    const facetCouplingControls = state.mode === 'eVoxelEB'
      ? stabilityControl
      : state.mode === 'eFacetEB'
      ? `${stabilityControl}${field('最大耦合迭代', `<input class="input" id="couplingIterations" type="number" min="1" max="100" value="${escapeHtml(c.control.couplingIterations)}">`)}${field('温度松弛系数', `<input class="input" id="temperatureRelaxation" type="number" min="0.05" max="1" step="0.05" value="${escapeHtml(c.control.temperatureRelaxation)}">`)}`
      : ''
    const coupling = couplingGeometry ? `<div class="property-group"><h3>${couplingGeometry} RT–EB 耦合</h3>${spectralAcceleration}${facetCouplingControls}<div class="notice">${couplingNotice}场景几何和${couplingGeometry}辐射结构只初始化一次。${state.mode === 'eFacetEB' ? '松弛系数默认 0.8；出现振荡（运行质量中迭代用满）时再调小。' : ''}</div></div>` : ''
    const temperatureMethods = energyBalanceOutput ? `<div class="property-group"><h3>温度模拟方法</h3>${field('土壤温度', `<select class="select" id="soilTemperatureMethod"><option value="0" ${c.control.soilTemperatureMethod === 0 ? 'selected' : ''}>0 · 瞬时能量平衡</option><option value="1" ${c.control.soilTemperatureMethod === 1 ? 'selected' : ''}>1 · 热惯性动态模型</option><option value="2" ${c.control.soilTemperatureMethod === 2 ? 'selected' : ''}>2 · 温度廓线传导模型（默认）</option></select>`)}${field('方法 1 热历史', `<select class="select" id="soilInertiaHistory"><option value="full" ${c.control.soilInertiaHistory === 'full' ? 'selected' : ''}>完整历史（8 节点 + 指数模态，新建默认）</option><option value="truncated" ${c.control.soilInertiaHistory !== 'full' ? 'selected' : ''}>仅最近 8 个节点（旧工程）</option></select>`)}${field('植被温度', `<select class="select" id="vegetationTemperatureMethod"><option value="0" ${c.control.vegetationTemperatureMethod === 0 ? 'selected' : ''}>经验法 · Ball–Berry</option><option value="1" ${c.control.vegetationTemperatureMethod === 1 ? 'selected' : ''}>机制法 · Farquhar</option></select>`)}<div class="notice">初始温度只作为求解初值；能量平衡各时刻均由所选方法动态更新。土壤方法会同步应用到全部土壤物化属性。</div></div>` : ''
    // 拼装 simulation 面板：运行质量/时限 → 模拟产出 → 计算控制 → 温度方法 → RT–EB 耦合。
    html = `<div class="property-group"><h3>运行质量与时限</h3>${qualityPresetControl(c)}${runTimeoutControl(c)}</div><div class="property-group"><h3>模拟产出</h3>${switchRow(imageOutputLabel, 'imageSwitch', c.sensor.image)}${processOutputSwitches}${processOutputNotice}${switchRow('输出温度', 'temperatureSwitch', c.sensor.temperature)}${switchRow('输出反照率', 'albedoSwitch', c.sensor.albedo)}</div><div class="property-group"><h3>计算控制</h3>${depthControl}${samplingControl}${periodicBoundaryControl}${heterogeneousVoxelControl}${radiationSolverControl}${collisionOrdersControl}${backendNotice}</div>${temperatureMethods}${coupling}${builtinNotice()}`
  }
  $('#inspectorContent').innerHTML = html
  bindInspector()
}

// 作用：以当前工程配置重新载入实际场景对象，失败时写错误日志。
function restoreActualScene() {
  loadActualScene(state.config).catch((error) => addLog(`恢复场景对象失败：${error.message}`, 'error'))
}

// 运行质量档位：按当前模式显示该档会写入的参数；档位与当前模式参数不一致时显示为“自定义”。
const QUALITY_FIELD_LABELS = {
  samples: () => state.mode === 'eRaytracing' ? '路径数' : '采样数', raytracingDepth: () => '散射次数',
  voxelCollisionOrders: () => '碰撞次数', shortwaveScatteringOrders: () => '短波阶数',
  radiationMaxSteps: () => '步数上限', couplingIterations: () => '耦合迭代'
}

// 作用：读取工程记录的质量档位；不是已知档位时视为 'custom'。
function storedQualityPreset(c) {
  return QUALITY_PRESETS.includes(c.control?.qualityPreset) ? c.control.qualityPreset : 'custom'
}

// 作用：生成“运行质量”下拉框与各档参数摘要。
// 说明：档位按模式分别写入参数；若当前模式参数与记录的档位不一致，则显示为“自定义”并提示重新选择。
function qualityPresetControl(c) {
  const fields = qualityPresetFields(state.mode)
  if (!fields.length) return `<div class="notice">运行质量档位：当前模式（面元辐射传输）没有受档位管理的参数，切换到其他模式时按工程记录的档位写入。</div>`
  const stored = storedQualityPreset(c)
  const shown = stored !== 'custom' && !qualityPresetMatches(stored, state.mode, c.control, c.scene) ? 'custom' : stored
  const options = QUALITY_PRESETS.map((preset) => `<option value="${preset}" ${preset === shown ? 'selected' : ''} ${preset === 'custom' ? 'disabled' : ''}>${t(QUALITY_PRESET_LABELS[preset])}</option>`).join('')
  const summary = ['fast', 'standard', 'high'].map((preset) => `${t(QUALITY_PRESET_LABELS[preset])}：${Object.entries(qualityPresetValues(preset, state.mode, c.scene)).map(([key, value]) => `${t(QUALITY_FIELD_LABELS[key]())} ${value}`).join('、')}`).join('；')
  const mismatch = shown !== stored
    ? `<div class="notice warning">工程记录的档位为“${t(QUALITY_PRESET_LABELS[stored])}”，但当前模式的参数与该档不一致（档位按模式分别写入），重新选择档位即可套用。</div>` : ''
  return `${field('运行质量', `<select class="select" id="qualityPreset">${options}</select>`)}<div class="notice">${summary}。选择档位只改写当前模式实际读取的参数；手动修改其中任一参数后显示为“自定义”。${state.mode === 'eVoxelEB' ? '标准档步数上限 = 场景对角线体元数 × 1.5，高精度档 = max(512, × 3)，取 16 的倍数；修改场景尺寸或体元大小时随之更新。' : ''}</div>${mismatch}`
}

// 作用：生成“单次运行上限”输入项（分钟）；留空或 0 使用服务端默认。
// 单位：分钟（范围 1–1440，由 normalizeRunTimeoutMinutes 规范化）。
function runTimeoutControl(c) {
  const minutes = normalizeRunTimeoutMinutes(c.control?.runTimeoutMinutes)
  const serverDefault = state.serverRunTimeoutSeconds ? formatDuration(state.serverRunTimeoutSeconds) : '15 分钟'
  return `${field('单次运行上限', inputUnit('runTimeoutMinutes', minutes || '', '分钟', `type="number" min="0" max="1440" step="1" placeholder="默认 ${escapeHtml(serverDefault)}"`))}<div class="notice">只计引擎执行时间，超过后终止运行并记为超时。留空或 0 使用服务端默认（当前 ${escapeHtml(serverDefault)}，可用环境变量 STREAM_STUDIO_RUN_TIMEOUT_MS 设置）；工程内可设 1–1440 分钟。</div>`
}

// 作用：用户选择质量档位（fast/standard/high）时，把该档在当前模式下的参数写入 c.control，
// 记录档位名、写日志、同步 project.json 文本并重绘面板。
function applyQualityPreset(preset) {
  const c = state.config
  if (!c || !['fast', 'standard', 'high'].includes(preset)) return
  const values = qualityPresetValues(preset, state.mode, c.scene)
  Object.assign(c.control, values)
  c.control.qualityPreset = preset
  addLog(`运行质量已切换为“${QUALITY_PRESET_LABELS[preset]}”：${Object.entries(values).map(([key, value]) => `${key}=${value}`).join('，') || '当前模式无受管参数'}`)
  syncProjectText()
  renderInspector()
}

// 用户手动修改受档位管理的参数后切到“自定义”（改回与档位相同的值则保持原档位）。
function markQualityCustom(key) {
  const c = state.config
  if (!c?.control || !qualityPresetFields(state.mode).includes(key)) return
  const stored = storedQualityPreset(c)
  if (stored === 'custom' || qualityPresetMatches(stored, state.mode, c.control, c.scene)) return
  c.control.qualityPreset = 'custom'
  const select = $('#qualityPreset')
  if (select) select.value = 'custom'
  addLog(`已手动修改受档位管理的参数（${key}），运行质量记为“自定义”`)
}

// 场景尺寸或体元大小改变时，若 VoxelEB 参数正按某档取值，按新场景重算步数上限。手动改过受管参数的
// 工程已记为“自定义”，因此档位非自定义且其余受管参数与该档一致即视为在用该档（高度输入框的实时预览
// 会先写入场景，不能依赖改动前的场景判断步数是否一致）。
function followQualityPresetScene(mutate) {
  mutate()
  const c = state.config
  const stored = c ? storedQualityPreset(c) : 'custom'
  if (stored === 'custom' || state.mode !== 'eVoxelEB') return
  const values = qualityPresetValues(stored, state.mode, c.scene)
  const following = Object.entries(values).every(([key, value]) => key === 'radiationMaxSteps' || Number(c.control[key]) === value)
  if (following && values.radiationMaxSteps !== c.control.radiationMaxSteps) {
    c.control.radiationMaxSteps = values.radiationMaxSteps
    addLog(`场景尺寸变化，按“${QUALITY_PRESET_LABELS[stored]}”档重算体元射线步数上限为 ${values.radiationMaxSteps}`)
  }
}

// 切换计算模式后，若工程记录了某个档位，把该档写入新模式实际读取的参数。
function followQualityPresetMode() {
  const c = state.config
  const stored = c ? storedQualityPreset(c) : 'custom'
  if (stored === 'custom' || qualityPresetMatches(stored, state.mode, c.control, c.scene)) return
  const values = qualityPresetValues(stored, state.mode, c.scene)
  Object.assign(c.control, values)
  addLog(`按工程运行质量“${QUALITY_PRESET_LABELS[stored]}”写入新模式参数：${Object.entries(values).map(([key, value]) => `${key}=${value}`).join('，')}`)
  syncProjectText()
}

// Blank or non-numeric entries in a number field restore the previous value
// instead of silently becoming 0 (Number('') === 0).
// 作用：把输入框内容解析为有限数；空串或非数值返回 null（调用方据此恢复原值）。
function finiteInputNumber(input) {
  const raw = String(input?.value ?? '').trim()
  const value = Number(raw)
  return raw !== '' && Number.isFinite(value) ? value : null
}

// 作用：给输入框 #id 绑定 change 事件，把值写回配置。
// 参数：target(value) 写入配置；converter 把输入转为目标类型（兼作限幅）；rebuildScene 为真则重建实际场景。
// 说明：写回后调用 syncProjectText 同步工程文本（旧版曾传入 XML 选择器字符串，现已无用并删除）。
// 说明：空串/非有限值会恢复上一次有效值，避免 Number('') === 0 被静默写成 0。
function bindValue(id, target, converter = Number, rebuildScene = false) {
  const input = $(`#${id}`)
  if (!input) return
  let previous = input.value
  input.addEventListener('change', () => {
    const numeric = input.type === 'number' || converter === Number
    let value = numeric && finiteInputNumber(input) === null ? undefined : converter(input.value)
    // Restoring re-applies the previous value: live 'input' previews may
    // already have written an intermediate value into the configuration.
    if (value === undefined || (typeof value === 'number' && !Number.isFinite(value))) {
      if (numeric && String(previous).trim() === '') { input.value = previous; return }
      value = converter(previous)
    }
    if (typeof value === 'number' && !Number.isFinite(value)) { input.value = previous; return }
    target(value); input.value = String(value); previous = input.value; syncProjectText()
    refreshFromState(rebuildScene)
    if (rebuildScene) restoreActualScene()
  })
}

// 作用：给开关按钮 #id 绑定点击事件：取反配置值、更新按钮外观并同步工程文本。
function bindSwitch(id, getter, setter, rebuildScene = false) {
  const button = $(`#${id}`)
  button?.addEventListener('click', () => {
    const next = !getter(); setter(next); button.classList.toggle('on', next); button.setAttribute('aria-checked', String(next)); syncProjectText(); refreshFromState(rebuildScene)
    if (rebuildScene) restoreActualScene()
  })
}

// 传感器采样参数变化后的同步入口（目前等价于 syncProjectText，保留旧名）。
function syncSensorSamplingXml() {
  syncProjectText()
}

// 作用：为 renderInspector 刚生成的面板控件绑定事件，把输入写回 state.config 并同步工程文本。
// 说明：各面板的控件互斥存在，绑定函数对不存在的 id 直接忽略（$(...)? 判空）。
// 分段：场景尺寸/偏移 → 计算控制 → 质量档位/超时 → 温度方法 → 流体 → DEM 地形环境 →
// 大气 → 背景 → 太阳/辐照 → 传感器 → 观测方向与输出选项。
// 范围限幅：多数 converter 给出限幅与缺省值（如 rayDepth 1–16、sampleCount 1–1024）。
function bindInspector() {
  const c = state.config
  if (!c) return
  // 场景/采样数值在 bindValue 的 converter 中限幅（下限/取整），与 validateProject 保持一致。
  // 场景尺寸与体元大小：修改后需重建场景，并让 VoxelEB 的步数上限随质量档位重算。
  // 下限与 validateProject 保持一致，避免 0/负数写进 project.json 后才在保存时失败。
  const positiveScene = (value) => Math.max(0.01, Number(value) || 0.01)
  bindValue('sceneX', (v) => followQualityPresetScene(() => c.scene.x = v), positiveScene, true); bindValue('sceneY', (v) => followQualityPresetScene(() => c.scene.y = v), positiveScene, true)
  bindValue('sceneHeight', (v) => followQualityPresetScene(() => c.scene.height = v), (value) => Math.max(0, Number(value) || 0), true); bindValue('voxelSize', (v) => followQualityPresetScene(() => c.scene.voxel = v), (value) => Math.max(0.0001, Number(value) || 0.0001)); bindValue('voxelFillThreshold', (v) => c.scene.voxelFillThreshold = Math.min(1, Math.max(0, v)))
  // 计算域偏移：输入过程中实时更新边界辅助框，并用 280 ms 防抖重建实际场景；
  // change 提交时取消待执行的防抖（bindValue 已做完整重建）。
  for (const [id, key] of [['sceneOffsetX', 'offsetX'], ['sceneOffsetY', 'offsetY'], ['sceneOffsetZ', 'offsetZ']]) {
    bindValue(id, (value) => c.scene[key] = value, Number, true)
    $(`#${id}`)?.addEventListener('input', (event) => {
      const value = Number(event.target.value)
      if (!Number.isFinite(value)) return
      c.scene[key] = value
      previewOnNextFrame('sceneBounds', () => updateSceneBoundsGuide(c))
      scheduleProjectTextSync()
      if (sceneOffsetPreviewTimer) clearTimeout(sceneOffsetPreviewTimer)
      sceneOffsetPreviewTimer = setTimeout(() => {
        sceneOffsetPreviewTimer = null
        restoreActualScene()
      }, 280)
    })
    $(`#${id}`)?.addEventListener('change', () => {
      if (sceneOffsetPreviewTimer) clearTimeout(sceneOffsetPreviewTimer)
      sceneOffsetPreviewTimer = null
    })
  }
  // Height changes need immediate visual feedback while the number field is
  // being edited; the normal change handler still performs the full rebuild.
  $('#sceneHeight')?.addEventListener('input', (event) => {
    const value = Number(event.target.value)
    if (!Number.isFinite(value) || value < 0.1) return
    c.scene.height = value
    previewOnNextFrame('sceneBounds', () => { updateSceneBoundsGuide(c); updateSun(c) })
    scheduleProjectTextSync()
  })
  // 计算控制：各深度/采样/阶数字段限幅并带缺省值；改动受管参数时 markQualityCustom 把档位记为“自定义”。
  bindValue('rayDepth', (v) => c.control.depth = Math.max(1, Math.min(16, Math.round(v))), (value) => Math.max(1, Math.min(16, Math.round(Number(value) || 4))))
  bindValue('raytracingDepth', (v) => { c.control.raytracingDepth = v; markQualityCustom('raytracingDepth') }, (value) => Math.max(1, Math.min(64, Math.round(Number(value) || 16))))
  bindValue('radiationMaxSteps', (v) => { c.control.radiationMaxSteps = v; markQualityCustom('radiationMaxSteps') }, (value) => Math.max(16, Math.min(4096, Math.round(Number(value) || 64))))
  bindValue('shortwaveScatteringOrders', (v) => { c.control.shortwaveScatteringOrders = v; markQualityCustom('shortwaveScatteringOrders') }, (value) => Math.max(1, Math.min(3, Math.round(Number(value)) || 1)))
  bindValue('sampleCount', (v) => { c.control.samples = Math.min(1024, Math.max(1, Math.round(v))); markQualityCustom('samples') }, (value) => Math.min(1024, Math.max(1, Math.round(Number(value) || 32))))
  bindValue('periodicTraversalCount', (count) => {
    c.control.periodicTraversalCount = count
  }, (value) => {
    const parsed = Number(value)
    return Math.max(0, Math.min(20, Math.round(Number.isFinite(parsed) ? parsed : 0)))
  })
  bindSwitch('skyboxSwitch', () => Boolean(c.control.skyboxEnabled), (value) => {
    c.control.skyboxEnabled = value
    updateSceneSkybox(c)
  })
  bindSwitch('heterogeneousVoxelSwitch', () => Boolean(c.control.heterogeneousVoxel), (v) => c.control.heterogeneousVoxel = v)
  bindValue('voxelCollisionOrders', (v) => { c.control.voxelCollisionOrders = v; markQualityCustom('voxelCollisionOrders') }, (value) => Math.max(2, Math.min(5, Math.round(Number(value)) || 2)))
  bindValue('radiationSolver', (v) => c.control.radiationSolver = v, (value) => value === 'accelerated' ? 'accelerated' : 'traditional')
  bindValue('couplingIterations', (v) => { c.control.couplingIterations = v; markQualityCustom('couplingIterations') }, (value) => Math.max(1, Math.min(100, Math.round(Number(value) || 20))))
  // 质量档位与单次运行上限。
  $('#qualityPreset')?.addEventListener('change', (event) => applyQualityPreset(event.target.value))
  // 留空或 0 = 服务端默认；其他值取整并限制在 1–1440 分钟。
  $('#runTimeoutMinutes')?.addEventListener('change', (event) => {
    const raw = String(event.target.value ?? '').trim()
    const minutes = raw === '' ? 0 : normalizeRunTimeoutMinutes(raw)
    if (raw !== '' && !Number.isFinite(Number(raw))) { event.target.value = c.control.runTimeoutMinutes || ''; return }
    c.control.runTimeoutMinutes = minutes
    event.target.value = minutes || ''
    syncProjectText()
    addLog(minutes ? `单次运行上限设为 ${minutes} 分钟` : '单次运行上限恢复为服务端默认')
  })
  // RT–EB 耦合与温度方法：光谱加速宽度限幅 1–1000 nm；土壤方法同时写入所有 Soil 物化属性的 params.method。
  bindValue('spectralAccelerationWidth', (v) => c.control.spectralAccelerationWidth = Math.max(1, Math.min(1000, v)), (value) => Math.max(1, Math.min(1000, Number(value) || 100)))
  bindValue('temperatureTolerance', (v) => c.control.temperatureTolerance = v, (value) => Math.max(0.001, Math.min(100, Number(value) || 0.05)))
  bindValue('temperatureRelaxation', (v) => c.control.temperatureRelaxation = v, (value) => Math.max(0.05, Math.min(1, Number(value) || 0.8)))
  bindValue('stabilityCorrection', (v) => c.control.stabilityCorrection = v === 1)
  bindValue('soilTemperatureMethod', (v) => {
    c.control.soilTemperatureMethod = Math.max(0, Math.min(2, Math.round(v)))
    c.materials.filter((item) => item.type === 'Soil').forEach((item) => { item.params.method = c.control.soilTemperatureMethod })
  }, (value) => Math.max(0, Math.min(2, Math.round(Number(value) || 0))))
  bindValue('soilInertiaHistory', (v) => { c.control.soilInertiaHistory = v }, (value) => value === 'full' ? 'full' : 'truncated')
  bindValue('vegetationTemperatureMethod', (v) => c.control.vegetationTemperatureMethod = Math.max(0, Math.min(1, Math.round(v))), (value) => Math.max(0, Math.min(1, Math.round(Number(value) || 0))))
  // 流体力学参数：数值各自限幅（如最大速度 0.1–200 m/s、时间步 0.1–60 s）；
  // 风向规范到 [0, 360) °。压力迭代次数（fluid.pressureIterations）在当前面板中没有输入框，已删除旧绑定；
// 该字段仍由 project-schema 的 normalize 保留（缺省 20，范围 2–100），以兼容引擎与旧工程。
  const fluid = c.fluid || (c.fluid = {})
  $('#fluidSwitch')?.addEventListener('click', () => {
    fluid.enabled = !fluid.enabled
    syncProjectText()
    refreshFromState(false)
    renderInspector()
  })
  bindValue('fluidWindDirection', (v) => fluid.windDirection = ((v % 360) + 360) % 360, (value) => ((Number(value) % 360) + 360) % 360)
  bindValue('fluidVoxelSize', (v) => fluid.voxelSize = Math.max(0.1, v), (value) => Math.max(0.1, Number(value) || Number(c.scene.voxel) || 1), true)
  bindValue('fluidTimeStep', (v) => fluid.timeStep = Math.max(0.1, Math.min(60, v)), (value) => Math.max(0.1, Math.min(60, Number(value) || 5)))
  bindValue('fluidBuoyancy', (v) => fluid.buoyancy = Math.max(0, v), (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidDrag', (v) => fluid.dragCoefficient = Math.max(0, v), (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidThermalCoupling', (v) => fluid.thermalCoupling = Math.max(0, v), (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidDiffusivity', (v) => fluid.diffusivity = Math.max(0, v), (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidSmokeEmission', (v) => fluid.smokeEmission = Math.max(0, v), (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidMaxVelocity', (v) => fluid.maxVelocity = Math.max(0.1, Math.min(200, v)), (value) => Math.max(0.1, Math.min(200, Number(value) || 30)))
  bindValue('fluidOutputHeight', (v) => fluid.outputHeight = Math.max(0.1, v), (value) => Math.max(0.1, Number(value) || 2))
  bindSwitch('fluidWindOutputSwitch', () => Boolean(fluid.outputWind), (v) => fluid.outputWind = v)
  bindSwitch('fluidTemperatureOutputSwitch', () => Boolean(fluid.outputAirTemperature), (v) => fluid.outputAirTemperature = v)
  // DEM 地形开关与地形水热经验场（terrainEnvironment）参数；开关切换需重建场景。
  bindSwitch('terrainSwitch', () => c.scene.terrain, (v) => c.scene.terrain = v, true)
  const terrainEnvironment = c.scene.terrainEnvironment || (c.scene.terrainEnvironment = normalizeTerrainEnvironment())
  bindSwitch('terrainAirSwitch', () => terrainEnvironment.airEnabled, (v) => terrainEnvironment.airEnabled = v)
  bindSwitch('terrainSoilSwitch', () => terrainEnvironment.soilEnabled, (v) => terrainEnvironment.soilEnabled = v)
  for (const key of ['referenceHeight', 'temperatureLapseRate', 'vaporScaleHeight', 'drainageHeight', 'soilDry', 'soilValley', 'soilScaleHeight']) {
    bindValue(`terrainEnv_${key}`, (v) => terrainEnvironment[key] = v)
  }

  // 大气 LUT 参数：水汽 0.5–5 cm、能见度 10–50 km 限幅；开关同时启用/禁用其余输入框。
  const atmosphere = c.atmosphere || (c.atmosphere = { enabled: false, model: 'midlatitude-summer', waterVapor: 2, aerosol: 'rural', visibility: 23, lutFile: 'assets/atmosphere/simple_modtran_lut.csv' })
  const atmosphereSwitch = $('#atmosphereSwitch')
  atmosphereSwitch?.addEventListener('click', () => {
    atmosphere.enabled = !atmosphere.enabled
    atmosphereSwitch.classList.toggle('on', atmosphere.enabled)
    atmosphereSwitch.setAttribute('aria-checked', String(atmosphere.enabled))
    for (const id of ['atmosphereModel', 'atmosphereWaterVapor', 'atmosphereAerosol', 'atmosphereVisibility']) {
      const input = $(`#${id}`)
      if (input) input.disabled = !atmosphere.enabled
    }
    syncProjectText()
    refreshFromState(false)
  })
  bindValue('atmosphereModel', (value) => atmosphere.model = value, String)
  bindValue('atmosphereWaterVapor', (value) => atmosphere.waterVapor = Math.max(0.5, Math.min(5, value)), (value) => Math.max(0.5, Math.min(5, Number(value) || 2)))
  bindValue('atmosphereAerosol', (value) => atmosphere.aerosol = value, String)
  bindValue('atmosphereVisibility', (value) => atmosphere.visibility = Math.max(10, Math.min(50, value)), (value) => Math.max(10, Math.min(50, Number(value) || 23)))
  // 背景属性：光谱/温度/物化绑定、镜面反射开关与 Hapke 角度效应强度（0–1，关闭时写 0）。
  const background = c.scene.background || (c.scene.background = {})
  const bindBackground = (id, key) => $(`#${id}`)?.addEventListener('change', (event) => {
    background[key] = event.target.value
    syncProjectText()
    if (key === 'materialName') {
      const material = c.materials.find((item) => item.name === event.target.value)
      background.materialType = material?.type || background.materialType || 'Soil'
      syncProjectText()
    }
    refreshFromState(false)
  })
  bindBackground('backgroundSpectral', 'spectralName')
  bindBackground('backgroundThermal', 'thermalName')
  bindBackground('backgroundMaterial', 'materialName')
  bindSwitch('backgroundSpecularSwitch', () => Boolean(background.specularEnabled), (value) => {
    background.specularEnabled = value
  })
  const heterogeneitySwitch = $('#backgroundHeterogeneitySwitch')
  heterogeneitySwitch?.addEventListener('click', () => {
    const enabled = !Boolean(background.heterogeneityEnabled)
    background.heterogeneityEnabled = enabled
    heterogeneitySwitch.classList.toggle('on', enabled)
    heterogeneitySwitch.setAttribute('aria-checked', String(enabled))
    const strengthInput = $('#backgroundAngularStrength')
    if (strengthInput) strengthInput.disabled = !enabled
    syncProjectText()
    refreshFromState(false)
  })
  const angularStrengthInput = $('#backgroundAngularStrength')
  angularStrengthInput?.addEventListener('input', () => {
    const value = Math.max(0, Math.min(1, Number(angularStrengthInput.value)))
    background.angularEffectStrength = value
    $('#backgroundAngularStrengthValue').textContent = value.toFixed(2)
    syncProjectText()
  })
  angularStrengthInput?.addEventListener('change', () => refreshFromState(false))
  // 太阳天顶/方位角输入；方位角变化且启用主平面采样时需同步传感器采样设置。
  const bindAngle = (id, key) => $(`#${id}`)?.addEventListener('change', (event) => {
    const value = finiteInputNumber(event.target)
    if (value === null) { event.target.value = String(c.light[key] ?? ''); return }
    // 天顶角 0–180°，方位角 -360–360°；与引擎的角度定义一致
    c.light[key] = key === 'zenith' ? Math.max(0, Math.min(180, value)) : Math.max(-360, Math.min(360, value))
    syncProjectText()
    if (key === 'azimuth' && c.sensor.principalPlane) syncSensorSamplingXml()
    refreshFromState(false)
  })
  bindAngle('sunZenith', 'zenith'); bindAngle('sunAzimuth', 'azimuth')
  // 直射/漫射比例联动：二者之和恒为 1，拖动滑块时实时预览太阳光，释放后才刷新场景。
  const bindLightFraction = (id, sourceKey) => {
    const input = $(`#${id}`)
    input?.addEventListener('input', () => {
      const value = Math.max(0, Math.min(1, Number(input.value)))
      c.light[sourceKey] = value
      c.light[sourceKey === 'direct' ? 'diffuse' : 'direct'] = 1 - value
      $('#directLight').value = String(c.light.direct); $('#diffuseLight').value = String(c.light.diffuse)
      $('#directValue').textContent = c.light.direct.toFixed(2); $('#diffuseValue').textContent = c.light.diffuse.toFixed(2)
      previewOnNextFrame('sun', () => updateSun(c))
    })
    input?.addEventListener('change', () => {
      syncProjectText()
      refreshFromState(false)
    })
  }
  bindLightFraction('directLight', 'direct'); bindLightFraction('diffuseLight', 'diffuse')
  $('#diffuseFractionMode')?.addEventListener('change', (event) => {
    c.light.diffuseFractionMode = event.target.value === 'erbs' ? 'erbs' : 'fixed'
    syncProjectText()
    refreshFromState(false)
  })
  bindValue('skyTemperature', (v) => c.light.skyTemperature = v, (value) => Math.max(0, Math.min(1000, Number(value) || 250)))
  bindValue('shortwaveIrradiance', (v) => c.light.shortwaveIrradiance = Math.max(0, Math.min(2000, v)))
  // 传感器：图像分辨率、波段、投影方式、位置/视场角与航点巡航参数。
  bindValue('sensorX', (v) => c.sensor.x = v, (value) => Math.max(1, Math.round(Number(value) || 1))); bindValue('sensorY', (v) => c.sensor.y = v, (value) => Math.max(1, Math.round(Number(value) || 1)))
  $('#sensorBands')?.addEventListener('change', (event) => { c.sensor.bands = event.target.value; syncSensorSamplingXml(); renderInspector() })
  const bindSensorNumber = (id, key, minimum = -Infinity) => $(`#${id}`)?.addEventListener('change', (event) => {
    const value = finiteInputNumber(event.target)
    if (value === null) { event.target.value = String(c.sensor[key] ?? ''); return }
    c.sensor[key] = Math.max(minimum, value); syncSensorSamplingXml(); renderInspector()
  })
  bindSensorNumber('bandStart', 'bandStart', 0.000001); bindSensorNumber('bandEnd', 'bandEnd', 0.000001); bindSensorNumber('bandStep', 'bandStep', 0.000001)
  const bindSensorSwitch = (id, key) => $(`#${id}`)?.addEventListener('click', () => {
    c.sensor[key] = !c.sensor[key]; syncSensorSamplingXml(); renderInspector()
  })
  bindSensorSwitch('continuousBandsSwitch', 'continuousBands')
  bindSensorSwitch('principalPlaneSwitch', 'principalPlane')
  bindSensorSwitch('hemisphereSwitch', 'hemisphere')
  bindSensorSwitch('sensorCruiseSwitch', 'cruiseEnabled')
  // 切换投影方式；首次切到中心投影时给缺省位置（场景中心）与高度 3000 m（当前值无效时）。
  $('#sensorProjection')?.addEventListener('change', (event) => {
    c.sensor.projection = event.target.value === 'perspective' ? 'perspective' : 'parallel'
    if (!Number.isFinite(Number(c.sensor.positionX))) c.sensor.positionX = Number(c.scene.x) / 2
    if (!Number.isFinite(Number(c.sensor.positionY))) c.sensor.positionY = Number(c.scene.y) / 2
    if (!(Number(c.sensor.height) > 0)) c.sensor.height = 3000
    syncProjectText()
    syncSensorSamplingXml()
    refreshFromState(false)
    renderInspector()
  })
  const bindSensorPosition = (id, key) => $(`#${id}`)?.addEventListener('change', (event) => {
    const value = finiteInputNumber(event.target)
    if (value === null) { event.target.value = String(c.sensor[key] ?? ''); return }
    c.sensor[key] = value
    syncProjectText()
    refreshFromState(false)
    renderInspector()
  })
  bindSensorPosition('sensorPositionX', 'positionX'); bindSensorPosition('sensorPositionY', 'positionY'); bindSensorPosition('sensorHeight', 'height')
  bindSensorNumber('sensorFov', 'fov', 0.1)
  $('#sensorCruiseRoute')?.addEventListener('change', (event) => {
    c.sensor.cruiseRoute = event.target.value
    syncSensorSamplingXml()
    renderInspector()
  })
  bindSensorNumber('sensorCruiseTargetX', 'cruiseTargetX')
  bindSensorNumber('sensorCruiseTargetY', 'cruiseTargetY')
  bindSensorNumber('sensorCruiseTargetZ', 'cruiseTargetZ', 0.01)
  bindSensorNumber('sensorCruiseRectangleWidth', 'cruiseRectangleWidth', 0.01)
  bindSensorNumber('sensorCruiseRectangleHeight', 'cruiseRectangleHeight', 0.01)
  bindSensorNumber('sensorCruiseStep', 'cruiseStep', 0.01)
  bindSensorNumber('sensorCruiseCount', 'cruiseCount', 1)
  // 随机巡航路线：换一个随机种子（1 ~ 2147483646 的整数）。
  $('#randomCruiseBtn')?.addEventListener('click', () => {
    c.sensor.cruiseRandomSeed = Math.max(1, Math.floor(Math.random() * 2147483646))
    syncSensorSamplingXml()
    renderInspector()
  })
  const bindView = (id, key) => $(`#${id}`)?.addEventListener('change', (event) => { const value = finiteInputNumber(event.target); if (value === null) { event.target.value = String(c.sensor[key] ?? ''); return } c.sensor[key] = value; syncSensorSamplingXml(); renderInspector() })
  bindView('viewZenith', 'vza'); bindView('viewAzimuth', 'vaa')
  // 模拟产出开关：能量平衡模式下“过程输出”= 辐射过程或能量过程任一开启；其他模式仅看辐射过程。
  const energyOutputMode = state.mode === 'eFacetEB' || state.mode === 'eVoxelEB'
  const syncActiveProcess = () => syncProjectText()
  bindSwitch('imageSwitch', () => c.sensor.image, (v) => c.sensor.image = v)
  bindSwitch('radiationProcessSwitch', () => c.sensor.radiationProcess, (v) => { c.sensor.radiationProcess = v; syncActiveProcess() })
  bindSwitch('energyProcessSwitch', () => c.sensor.energyProcess, (v) => { c.sensor.energyProcess = v; syncActiveProcess() })
  bindSwitch('temperatureSwitch', () => c.sensor.temperature, (v) => c.sensor.temperature = v); bindSwitch('albedoSwitch', () => c.sensor.albedo, (v) => c.sensor.albedo = v)
  // 气象时间与站点参数：开始节点改动后若结束节点不大于它，则自动置为开始节点 + 1。
  // converter 同时是缺省值来源：dTime 1800 s、年份 2019、时区 8（UTC+8）、土壤初温 300 K（范围 150–400 K）；
  // 这四项缺省已对照 fileio_json.cpp（dTime 1800、year 2019、timezone 8、Tsold 300）与 project-schema 的
  // 气象缺省核实一致。缺省仅在输入无效时使用（空串/非数值由 bindValue 先恢复上次有效值）；
  // 纬度/经度/高度/饱和水汽的 converter 兜底值（0）与引擎缺省（40°、116°、15 m、0.45）不同，
  // 但因 bindValue 已拦截无效输入，实际不会触发。
  bindValue('meteoStart', (v) => {
    c.meteo.start = v
    if (!(Number(c.meteo.end) > v)) { c.meteo.end = v + 1; syncProjectText(); $('#meteoEnd').value = String(c.meteo.end) }
  }, (value) => Math.max(0, Math.round(Number(value) || 0)))
  bindValue('meteoEnd', (v) => c.meteo.end = v, (value) => Math.max(Number(c.meteo.start) + 1, Math.round(Number(value) || 0)))
  bindValue('meteoDTime', (v) => c.meteo.dTime = v, (value) => Math.max(1, Number(value) || 1800))
  bindValue('meteoLatitude', (v) => c.meteo.latitude = v, (value) => Math.max(-90, Math.min(90, Number(value) || 0)))
  bindValue('meteoLongitude', (v) => c.meteo.longitude = v, (value) => Math.max(-180, Math.min(180, Number(value) || 0)))
  bindValue('meteoYear', (v) => c.meteo.year = v, (value) => Math.max(-2000, Math.min(6000, Math.round(Number(value)) || 2019)))
  bindValue('meteoTimezone', (v) => c.meteo.timezone = v, (value) => { const hours = Number(value); return Number.isFinite(hours) ? Math.max(-12, Math.min(14, hours)) : 8 })
  bindValue('meteoHeight', (v) => c.meteo.z = v, (value) => Math.max(0, Number(value) || 0))
  bindValue('meteoSoilTemperature', (v) => c.meteo.Tsold = v, (value) => Math.max(150, Math.min(400, Number(value) || 300)))
  bindValue('meteoSatWater', (v) => c.meteo.SatWater = v, (value) => Math.max(0, Math.min(1, Number(value) || 0)))
  // 文件导入、对象列表（选中/属性/分布/删除/篷布罩）与材质库（新增/编辑/筛选）的事件绑定。
  $('#demFile')?.addEventListener('change', (event) => importDem(event.target.files?.[0]))
  $('#meteoFile')?.addEventListener('change', (event) => importMeteo(event.target.files?.[0]))
  $('#objectImportAction')?.addEventListener('click', importObj)
  $('#objectGenerateAction')?.addEventListener('click', showGeometryDialog)
  $('#objectMediumAction:not([disabled])')?.addEventListener('click', () => showMediumDialog('fire'))
  $('#objectFogAction:not([disabled])')?.addEventListener('click', () => showMediumDialog('fog'))
  $$('.object-card[data-index]').forEach((card) => {
    const showAttributes = () => showObjectAttributeDialog(Number(card.dataset.index))
    card.addEventListener('click', (event) => { if (!event.target.closest('button')) showAttributes() })
    card.addEventListener('keydown', (event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); showAttributes() } })
  })
  $$('.object-material').forEach((button) => button.addEventListener('click', (event) => { event.stopPropagation(); showObjectAttributeDialog(Number(button.closest('.object-card').dataset.index)) }))
  $$('.object-distribution').forEach((button) => button.addEventListener('click', (event) => { event.stopPropagation(); showObjectDialog(Number(button.closest('.object-card').dataset.index)) }))
  $$('.object-delete').forEach((button) => button.addEventListener('click', (event) => { event.stopPropagation(); deleteSceneObject(Number(button.closest('.object-card').dataset.index)) }))
  $$('.object-tarp').forEach((button) => button.addEventListener('click', async (event) => {
    event.stopPropagation()
    button.disabled = true
    try { await addTarpCover(Number(button.closest('.object-card').dataset.index)) } finally { button.disabled = false }
  }))
  $$('.library-add').forEach((button) => button.addEventListener('click', () => showPresetDialog(button.dataset.presetKind)))
  $$('.library-card[data-preset-index]').forEach((card) => card.addEventListener('click', () => showPresetDialog(card.dataset.presetKind, Number(card.dataset.presetIndex))))
  $$('.library-filter').forEach((input) => input.addEventListener('input', () => {
    const keyword = input.value.trim().toLowerCase()
    $$(`[data-library-list="${input.dataset.libraryFilter}"] .library-card`).forEach((card) => card.classList.toggle('filtered-out', !card.dataset.librarySearch.includes(keyword)))
  }))
}

// 作用：刷新左侧导航各项的右侧小徽标（场景尺寸、太阳角、传感器分辨率、对象数、大气/流体状态等）。
function renderNavBadges() {
  const c = state.config
  if (!c) return
  const simulationOutputs = []
  if (c.sensor?.image) simulationOutputs.push(t('图像'))
  if (c.sensor?.radiationProcess || c.sensor?.energyProcess || c.sensor?.process) simulationOutputs.push(t('过程'))
  simulationOutputs.push(t('统计'))
  $('#sceneSizeBadge').textContent = `${c.scene.x} × ${c.scene.y} m`; $('#lightBadge').textContent = `${c.light.zenith}° / ${c.light.azimuth}°`; $('#sensorBadge').textContent = `${c.sensor.x} × ${c.sensor.y}`
  $('#objectBadge').textContent = c.objects.count || '0'; $('#materialBadge').textContent = `${c.spectra.length + c.thermals.length + c.canopies.length + c.materials.length}`; $('#atmosphereBadge').textContent = t(c.atmosphere?.enabled ? '已启用' : '关闭'); $('#meteoBadge').textContent = state.inputPath ? `${c.meteo.start}–${c.meteo.end}` : t('未加载'); $('#fluidBadge').textContent = t(state.mode === 'eVoxelEB' ? (c.fluid?.enabled ? '已启用' : '关闭') : '仅 VoxelEB'); $('#simulationBadge').textContent = simulationOutputs.join(' / ')
  // 场景输入：地理/DEM 的分组徽标
  $('#geoBadge').textContent = `${Number(c.meteo.latitude).toFixed(1)}° / UTC${Number(c.meteo.timezone) >= 0 ? '+' : ''}${Number(c.meteo.timezone)}`
  $('#demBadge').textContent = t(c.scene.demFile ? (c.scene.terrain ? '已启用' : '已载入') : '关闭')
}

// 作用：配置变化后刷新界面：徽标 →（rebuild 为真时）重新生成预览场景并复位相机 → 天空盒与太阳。
function refreshFromState(rebuild = true) {
  const c = state.config
  renderNavBadges()
  if (rebuild) { generateWorld(c); fitCamera() }
  updateSceneSkybox(c)
  updateSun(c)
}

// 判断 DEM 信息里是否已带地形高程栅格（terrainValues 长度须等于宽×高，宽高各至少 2）。
function hasTerrainValues(config) {
  const dem = config?.scene?.demInfo
  const width = Math.max(2, Math.round(Number(dem?.terrainWidth) || 0))
  const height = Math.max(2, Math.round(Number(dem?.terrainHeight) || 0))
  return Array.isArray(dem?.terrainValues) && dem.terrainValues.length === width * height
}

// 作用：启用了地形且有 DEM 文件、但缺少高程栅格时，向后端查询 DEM 元数据与高程并写入 config。
// 返回：是否已具备高程栅格（true 时调用方需重新生成场景）。
async function ensureDemInfo(config, projectPath = state.inputPath) {
  if (!config?.scene?.terrain || !config.scene.demFile || hasTerrainValues(config)) return false
  const inspected = await api.inspectDem({ path: config.scene.demFile, projectPath })
  config.scene.demInfo = inspected.metadata || null
  // Save snapshots are detached; only the active scene may update its project.
  if (state.config === config && state.project?.configuration) state.project.configuration.scene.demInfo = config.scene.demInfo
  return hasTerrainValues(config)
}

// 实际场景加载的代数：并发的 loadActualScene 只让最新一次生效，旧的异步结果丢弃。
let sceneLoadGeneration = 0

// 作用：载入工程里真实的场景对象（OBJ/PRIM 等）到 world，并刷新边界框、统计和视图样式。
// 步骤：清除对象选中 → 必要时补取 DEM 高程并重生成地形 → loadXmlScene 载入对象 → 刷新辅助显示。
async function loadActualScene(config) {
  const generation = ++sceneLoadGeneration
  const isCurrent = () => generation === sceneLoadGeneration
  clearObjectSelection(false)
  try {
    if (await ensureDemInfo(config) && isCurrent()) generateWorld(config)
  } catch (error) {
    if (isCurrent()) addLog(`DEM 高程恢复失败：${error.message}`, 'error')
  }
  if (!isCurrent()) return
  if (await loadXmlScene({ api, world, config, log: addLog, isCurrent }) === null) return
  updateSceneBoundsGuide(config); updateStats(); applyViewStyle(currentStyle); fitCamera()
  if (objectEdit.enabled) geometryDrawHint('单击选择 OBJ；双击修改物化属性；W/E/R 切换移动、旋转、缩放')
}

// 作用：把打开工程/示例得到的结果装载进界面（名字沿用旧的 XML 时代，现为 project.json）。
// 输入：result（path/content/project/projectPath 等，来自后端）。
// 说明：影像模拟（STREAM）工程交给 streamWorkspace，不走三维流程；模拟运行中拒绝切换工程。
// 作用：切换工程/新建工程前确认未保存修改。返回 false 表示用户取消。
function confirmDiscardChanges(action = '继续') {
  if (!state.xmlDirty) return true
  return window.confirm(`当前工程有未保存的修改，${action}将放弃这些修改。是否继续？`)
}

function loadXml(result, confirm = true) {
  // Opening/reloading a project replaces the whole editor state.  Ask before
  // silently discarding unsaved changes (the dirty flag was previously only
  // used by the window title).
  if (confirm && !confirmDiscardChanges('打开其他工程')) return
  if (result?.streamProject) {
    Promise.resolve(streamWorkspace.openProject(result.streamProject))
      .catch((error) => toast('无法打开影像模拟工程', error?.message || String(error), 'error'))
    return
  }
  if (state.running) { toast('无法打开工程', '模拟运行中，不能更换工程', 'error'); return }
  streamWorkspace.close()
  if (projectTextSyncTimer) { clearTimeout(projectTextSyncTimer); projectTextSyncTimer = null }
  try {
    // 步骤 1：解析并规范化工程，补齐材质预设；沿用工程里保存的 DEM 元数据。
    const projectMode = result.project?.mode || projectModeFromXml(result.content, state.mode)
    const project = normalizeProject(result.project || parseProjectJson(result.content))
    const config = ensureMaterialPresets(project.configuration)
    if (project?.configuration?.scene?.demInfo) config.scene.demInfo = project.configuration.scene.demInfo
    const storedItems = project?.configuration?.objects?.items || []
    // 步骤 2：按文件名或名称（否则按序号）匹配文件中保存的对象，恢复其 distribution / instanceCount。
    config.objects.items = config.objects.items.map((item, index) => {
      const stored = storedItems.find((entry) => (entry.fileName && entry.fileName === item.fileName) || entry.name === item.name) || storedItems[index]
      return stored ? { ...stored, ...item, distribution: stored.distribution, instanceCount: stored.instanceCount } : item
    })
    if (project) project.configuration = config
    // 步骤 3：清纹理缓存，一次性替换 state 中与工程相关的字段（工程代数 +1），并切换到工程记录的模式。
    invalidateTextureCaches()
    state.projectGeneration += 1
    Object.assign(state, { inputPath: result.path, xmlText: result.content, xmlDirty: false, xmlManualEdit: false, project, projectPath: result.projectPath || '', config, outputDir: config.outDir || result.path.replace(/[\\/][^\\/]+$/, '') , selectedObjectIndex: -1, editingObjectIndex: -1, editingAttributeIndex: -1 })
    selectMode(projectMode)
    // 步骤 4：刷新编辑器文本与标题，重建预览与实际场景、属性面板，并写日志。
    $('#xmlEditor').value = result.content; $('#xmlEditor').hidden = false; $('#emptyEditor').hidden = true; $('#applyXmlBtn').disabled = false; $('#xmlStatus').textContent = result.path
    $('#projectName').textContent = project?.name || fileName(result.path); $('#unsavedMark').hidden = true
    refreshFromState(); restoreActualScene(); renderInspector(); addLog(`已加载配置：${result.path}`, 'success'); toast('配置已加载', fileName(result.path))
    void reportUsdOpen(result)
  } catch (error) { toast('无法读取配置', error.message, 'error'); addLog(error.message, 'error') }
}

// 作用：弹出“打开工程”选择框（按当前模式过滤），选中后调用 loadXml。
async function chooseXml() {
  try {
    const result = await api.chooseXml(state.mode)
    if (result) loadXml(result)
  } catch (error) {
    toast('无法打开工程', error.message, 'error')
    addLog(`无法打开工程：${error.message}`, 'error')
  }
}

// 作用：把保存工程时同步 project.usd 的结果写入日志（失败、统计、外部修改备份、跳过的对象）。
function reportUsdSave(usd) {
  if (!usd) return
  if (usd.error) { addLog(`project.usd 同步失败：${usd.error}`, 'warning'); return }
  addLog(`已同步 project.usd（${usd.prototypes} 个模型、${usd.instances} 个实例，${(usd.elapsedMs / 1000).toFixed(1)} s）`)
  if (usd.externalBackup) addLog(`project.usd 曾在其他软件中被修改，覆盖前已备份为 ${usd.externalBackup}`, 'warning')
  for (const warning of usd.skippedObjects || []) addLog(`USD 导出跳过：${warning}`, 'warning')
}

// A USD file opened as a project, or project.usd edited elsewhere since Stream Studio last wrote it.
// 作用：打开工程后处理 USD 相关提示：汇报从 OpenUSD 导入的摘要；
// 若 project.usd 在外部被改动，询问是否载入这些修改（载入即重新打开 project.usd）。
async function reportUsdOpen(result) {
  const summary = result?.usdImport
  if (summary) {
    addLog(`已从 OpenUSD 载入场景：${result.usdSource}（${summary.objects} 个对象、${summary.instances} 个实例${summary.createdProject ? '，已新建 project.json' : '，原 project.json 已备份'}${summary.restoredFromMetadata ? '，参数按 USD 内嵌的 Stream Studio 工程恢复' : ''}）`, 'success')
    for (const warning of summary.warnings || []) addLog(`USD 提示：${warning}`, 'warning')
  }
  if (!result?.usdChangedExternally) return
  if (!window.confirm(t('project.usd 已在其他软件中被修改，是否载入这些修改？选择“取消”则保留当前工程，下次保存时会先备份再覆盖 project.usd。'))) return
  try {
    const reopened = await api.openProject(result.path.replace(/project\.json$/i, 'project.usd'), state.mode)
    loadXml(reopened)
  } catch (error) {
    toast('无法载入 project.usd', error.message, 'error')
    addLog(`无法载入 project.usd：${error.message}`, 'error')
  }
}

function hideExampleDialog() { $('#exampleDialog').hidden = true }

// 作用：在示例对话框里显示所选示例的推荐模式、关注点与备注。
function renderExampleSummary() {
  const example = state.examples?.find((item) => item.id === $('#exampleSelect').value)
  const mode = example?.recommendedMode
  $('#exampleSummary').textContent = example ? `${t('推荐模式')}：${mode} · ${example.focus.map((item) => t(item)).join('、')}${example.note ? ` · ${t(example.note)}` : ''}` : ''
}

// 作用：向后端获取示例列表，按“技术演示/内置精选 · 类别”分组填入下拉框并打开对话框；运行中禁止。
async function showExampleDialog() {
  if (state.running) { toast('无法打开示例', '模拟运行中，不能更换工程', 'error'); return }
  try {
    const { examples, directory } = await api.listExamples()
    if (!examples.length) throw new Error('没有可用的示例工程')
    state.examples = examples
    const groups = new Map()
    for (const item of [...examples].sort((a, b) => Number(a.tier === 'technical') - Number(b.tier === 'technical'))) {
      const group = `${t(item.tier === 'technical' ? '技术演示' : '内置精选')} · ${t(item.categoryLabel || '其他案例')}`
      if (!groups.has(group)) groups.set(group, [])
      groups.get(group).push(item)
    }
    $('#exampleSelect').innerHTML = [...groups].map(([label, items]) =>
      `<optgroup label="${escapeHtml(label)}">${items.map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(t(item.label))}</option>`).join('')}</optgroup>`).join('')
    $('#exampleDirectory').value = appStorage.getItem('histreamExampleDirectory') || directory
    renderExampleSummary()
    $('#exampleDialog').hidden = false
  } catch (error) { toast('无法读取示例', error.message, 'error') }
}

// 作用：把所选示例复制到用户指定目录（同名副本会被覆盖）并作为工程打开；
// 目录记入 appStorage（histreamExampleDirectory），去除用户粘贴时可能带的外层引号。
async function openExampleFromDialog(event) {
  event.preventDefault()
  const submit = $('#submitExampleBtn')
  try {
    submit.disabled = true
    const directory = $('#exampleDirectory').value.trim().replace(/^"(.*)"$/, '$1')
    const result = await api.openExample($('#exampleSelect').value, directory)
    appStorage.setItem('histreamExampleDirectory', directory)
    loadXml(result)
    hideExampleDialog()
    addLog(result.overwritten ? `示例已覆盖同名副本并打开：${result.path}` : `示例已复制并打开：${result.path}`, 'success')
  } catch (error) {
    toast('无法打开示例', error.message, 'error')
    addLog(error.message, 'error')
  } finally { submit.disabled = false }
}

// 作用：导入气象驱动文件：内容交给后端复制到工程 meteorology 目录，更新 meteo.path 并保存工程，
// 然后重新载入场景。导入期间若工程被切换或保存失败则放弃/报错。
async function importMeteo(file) {
  if (!file) return
  try {
    if (state.running) throw new Error('模拟运行中，不能更换气象驱动文件')
    const projectPath = state.inputPath
    const imported = await api.importMeteo({ name: file.name, content: await file.text() })
    if (state.inputPath !== projectPath) throw new Error('导入期间已切换工程，本次气象文件未写入')
    const project = ensureProjectState()
    state.config.meteo.path = imported.relativePath || imported.path
    project.configuration = state.config
    syncProjectText()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    refreshFromState(false)
    await loadActualScene(state.config)
    renderInspector()
    toast('导入成功', imported.relativePath || file.name)
    addLog(`已导入气象驱动：${imported.path}`, 'success')
  } catch (error) {
    toast('气象文件导入失败', error.message, 'error')
    addLog(error.message, 'error')
  }
}

// 作用：把 File 读成 base64 字符串（去掉 data URL 前缀），用于上传 DEM。
function readFileBase64(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.onerror = () => reject(reader.error || new Error('无法读取 DEM 文件'))
    reader.onload = () => resolve(String(reader.result || '').split(',', 2)[1] || '')
    reader.readAsDataURL(file)
  })
}

// 作用：导入 DEM（GeoTIFF/ASC，≤ 64 MB）：后端写入工程并返回元数据，随即启用地形、保存工程，
// 重新生成场景；后端给出的 warnings 以提示显示。切换工程/保存失败的处理同 importMeteo。
async function importDem(file) {
  if (!file) return
  try {
    if (state.running) throw new Error('模拟运行中，不能更换 DEM 文件')
    if (file.size > 64 * 1024 * 1024) throw new Error('DEM 文件超过 64MB，请先裁剪或降采样')
    const projectPath = state.inputPath
    const imported = await api.importDem({ name: file.name, contentBase64: await readFileBase64(file), projectPath: state.inputPath })
    if (state.inputPath !== projectPath) throw new Error('导入期间已切换工程，本次 DEM 未写入')
    const project = ensureProjectState()
    state.config.scene.demFile = imported.relativePath || imported.path
    state.config.scene.demInfo = imported.metadata || null
    state.config.scene.terrain = true
    project.configuration = state.config
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    refreshFromState()
    await loadActualScene(state.config)
    renderInspector()
    const warning = (imported.warnings || []).join('；')
    toast(warning ? 'DEM 已导入（有提示）' : 'DEM 导入成功', warning || imported.relativePath || file.name, warning ? 'warning' : '')
    addLog(`已导入 DEM：${imported.path}`, 'success')
    if (warning) addLog(`DEM 提示：${warning}`, 'warning')
  } catch (error) {
    toast('DEM 导入失败', error.message, 'error')
    addLog(error.message, 'error')
  }
}

// 作用：保存当前工程（project.json 文本与配置）到磁盘。
// 参数：fromProject 为真表示配置来自界面状态（state.config），否则可能来自用户在 JSON 编辑器中
// 手改的文本（此时重新解析，替换 state.config）。
// 返回：true 保存并生效；false 失败；null 已被新的工程切换/新的编辑取代（调用方应跳过刷新与回滚）。
async function saveXml(fromProject = false) {
  fromProject = fromProject === true
  // 步骤 1：先提交正在编辑的数字框（blur 触发 change），并冲刷待同步的工程文本。
  // Commit a number field still being edited (its change event fires on blur).
  const active = document.activeElement
  if (active && active !== document.body && active.closest?.('#inspectorContent')) active.blur()
  flushProjectTextSync()
  // 步骤 2：记录 projectGeneration / editRevision 等快照，保存期间据此检测工程切换或后续编辑。
  const generation = state.projectGeneration
  const revision = state.editRevision
  const savedInputPath = state.inputPath
  const savedText = state.xmlText
  const savedMode = state.mode
  const isCurrent = () => state.projectGeneration === generation && state.inputPath === savedInputPath
  if (!savedText) { await chooseXml(); return isCurrent() ? false : null }
  state.lastSaveError = null
  try {
    // Inspector/dialog handlers hold a reference to state.config, so only text
    // typed into the JSON editor may replace it with a newly parsed object.
    const manualEdit = !fromProject && (state.xmlManualEdit || !state.config)
    const sourceConfig = manualEdit ? ensureMaterialPresets(parseXml(savedText, savedMode)) : state.config
    const replaced = sourceConfig !== state.config
    // 步骤 3：对配置与工程做深拷贝后提交后端，避免保存过程中被界面修改。
    const config = structuredClone(sourceConfig)
    const project = structuredClone(state.project)
    if (fromProject) await ensureDemInfo(config, savedInputPath)
    // null means superseded: callers must skip both refresh and failure rollback.
    if (!isCurrent() || state.editRevision !== revision) return null
    if (project) {
      project.configuration = config
      project.mode = savedMode
    }
    // 步骤 4：写盘；之后按快照是否仍有效，决定更新 state、保持“未保存”标记，还是只写日志。
    const result = await api.saveXml({ path: savedInputPath, content: savedText, project })
    if (!result?.path) return isCurrent() ? false : null
    // The submitted snapshot is on disk; newer edits must remain dirty and intact.
    if (!isCurrent()) { addLog('已保存提交时的配置；当前工程保持不变', 'warning'); reportUsdSave(result.usd); return null }
    // 保存期间用户又有新编辑：磁盘已是提交时的快照，界面保持“未保存”状态。
    if (state.editRevision !== revision) {
      // An older save may finish after a newer one cleared the dirty indicator.
      state.xmlDirty = true
      $('#unsavedMark').hidden = false
      $('#xmlStatus').textContent = '有未保存的修改'
      addLog('已保存提交时的配置；后续修改仍需保存', 'warning')
      reportUsdSave(result.usd)
      return true
    }
    state.config = sourceConfig
    if (fromProject && sourceConfig?.scene) sourceConfig.scene.demInfo = config.scene.demInfo
    state.inputPath = result.path
    state.project = result.project || project
    state.projectPath = result.projectPath || state.projectPath
    state.xmlText = result.content || state.xmlText
    state.xmlDirty = false
    state.xmlManualEdit = false
    $('#xmlEditor').value = state.xmlText
    $('#unsavedMark').hidden = true; $('#xmlStatus').textContent = result.path; $('#projectName').textContent = state.project?.name || fileName(result.path)
    if (replaced) { refreshFromState(); restoreActualScene(); renderInspector() }
    addLog(`已保存配置：${result.path}`, 'success'); toast('保存成功', fileName(result.path)); reportUsdSave(result.usd); return true
  } catch (error) {
    if (isCurrent()) state.lastSaveError = error
    toast('保存失败', error.message, 'error')
    addLog(`工程配置保存失败（${savedInputPath}）：${error.message}`, 'error')
    return isCurrent() ? false : null
  }
}

// 作用：保证 state.project 存在；没有时按工程文件所在目录名建一个默认工程并挂上当前配置。
function ensureProjectState() {
  if (state.project) return state.project
  const parts = state.inputPath.replaceAll('\\', '/').split('/')
  state.project = createDefaultProject({ name: parts.at(-2) || '导入工程', mode: state.mode })
  state.project.configuration = state.config
  return state.project
}

// 作用：把文件名等转成可作标识的名字（去扩展名，非字母数字/下划线/连字符替换为下划线）；为空用 fallback。
function uniqueName(value, fallback) {
  const cleaned = String(value || '').replace(/\.[^.]+$/, '').replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '')
  return cleaned || fallback
}

// 作用：去掉 OBJ 文本中的 mtllib / usemtl 行，使导入网格不依赖外部 MTL 文件；结果以换行结尾。
function sanitizeObjContent(content) {
  const sanitized = String(content)
    .split(/\r?\n/)
    .filter((line) => !/^\s*(mtllib|usemtl)\b/i.test(line))
    .join('\n')
    .trimEnd()
  return sanitized ? sanitized + '\n' : ''
}

// 作用：转为数值，非有限数时返回 fallback。
function finiteNumber(value, fallback) {
  const number = Number(value)
  return Number.isFinite(number) ? number : fallback
}

// 实例分布参数约束：密度下限 0.01 个/公顷，保留 2 位小数；单个对象最多 100000 个实例；
// 1 公顷 = 10000 m²（分布范围面积按 m² 计，密度统一存为 个/公顷）。
const MIN_DISTRIBUTION_DENSITY = .01
const DISTRIBUTION_DENSITY_DECIMALS = 2
const MAX_DISTRIBUTION_INSTANCES = 100000
const SQUARE_METERS_PER_HECTARE = 10000

// 作用：把密度换算为 个/公顷（unit 为 squareKilometer 时 1 km² = 100 公顷，故除以 100）。
function densityPerHectare(value, unit = 'hectare') {
  const density = normalizeDistributionDensity(value)
  return unit === 'squareKilometer'
    ? normalizeDistributionDensity(density / 100)
    : density
}

// 作用：密度规范化：非正/非数值用缺省 200 个/公顷，保留 2 位小数并不低于下限。
function normalizeDistributionDensity(value, fallback = 200) {
  const number = Number(value)
  const safe = Number.isFinite(number) && number > 0 ? number : fallback
  return Math.max(MIN_DISTRIBUTION_DENSITY, Number(safe.toFixed(DISTRIBUTION_DENSITY_DECIMALS)))
}

function distributionDensityInputValue(value) {
  return normalizeDistributionDensity(value).toFixed(DISTRIBUTION_DENSITY_DECIMALS)
}

// 作用：分布矩形范围（minX–maxX × minY–maxY）的面积（m²），下限 1e-6 以避免除零。
function distributionArea(distribution) {
  const width = Math.max(0, Math.abs(distribution.maxX - distribution.minX))
  const height = Math.max(0, Math.abs(distribution.maxY - distribution.minY))
  return Math.max(.000001, width * height)
}

// 作用：按“数量”或“密度”为准互相换算：basis = 'density' 时由密度×面积得数量，
// 否则由数量/面积反推密度；数量限幅到 1–100000。返回 { count, density }。
function populationFromValues(countValue, densityValue, area, basis) {
  let count = Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, Math.round(finiteNumber(countValue, 1))))
  let density = normalizeDistributionDensity(densityValue)
  if (basis === 'density') {
    count = Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, Math.round(area * density / SQUARE_METERS_PER_HECTARE)))
  } else {
    density = normalizeDistributionDensity(count / area * SQUARE_METERS_PER_HECTARE)
  }
  return { count, density }
}

// 作用：新对象的缺省分布：覆盖整个场景范围、随机分布、密度 200 个/公顷、种子 1；
// 植被缩放 0.85–1.15，其余对象不缩放；rows/columns 为网格模式缺省值。
function defaultDistribution(item = {}) {
  const sceneX = Math.max(1, finiteNumber(state.config?.scene?.x, 60))
  const sceneY = Math.max(1, finiteNumber(state.config?.scene?.y, 60))
  const vegetation = item.type === 'Vegetation' || !item.type
  const density = 200
  const count = Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, Math.round(sceneX * sceneY * density / SQUARE_METERS_PER_HECTARE)))
  return {
    mode: 'random', basis: 'density', count, density, densityUnit: 'hectare',
    rows: 4, columns: 6,
    minX: 0, maxX: sceneX,
    minY: 0, maxY: sceneY,
    seed: 1, scaleMin: vegetation ? .85 : 1, scaleMax: vegetation ? 1.15 : 1,
    z: 0, offsetX: 0, offsetY: 0, offsetZ: 0, points: ''
  }
}

// 作用：从分布对话框表单读取并规范化分布参数（缺项用 defaultDistribution）。
// 说明：options.requireValidDensity 为真且模式为 random/grid 且按密度时，密度必须是有效正数，否则抛错；
// 数量与密度经 populationFromValues 互相换算；缩放下限 0.01。
function distributionFromForm(form, options = {}) {
  const values = Object.fromEntries(new FormData(form).entries())
  const defaults = defaultDistribution({ type: values.objectType })
  const mode = ['random', 'grid', 'single', 'manual'].includes(values.distributionMode) ? values.distributionMode : defaults.mode
  const basis = values.distributionBasis === 'density' ? 'density' : 'count'
  const densityInput = String(values.distributionDensity ?? '').trim()
  const densityValue = Number(densityInput)
  const requiresDensity = options.requireValidDensity &&
    (mode === 'random' || mode === 'grid') && basis === 'density'
  if (requiresDensity && (!densityInput || !Number.isFinite(densityValue) || densityValue < MIN_DISTRIBUTION_DENSITY)) {
    throw new Error('分布密度必须填写大于 0 的有效数值，且保留两位小数')
  }
  const minX = finiteNumber(values.distributionMinX, defaults.minX)
  const maxX = finiteNumber(values.distributionMaxX, defaults.maxX)
  const minY = finiteNumber(values.distributionMinY, defaults.minY)
  const maxY = finiteNumber(values.distributionMaxY, defaults.maxY)
  const population = populationFromValues(
    values.distributionCount,
    values.distributionDensity,
    distributionArea({ minX, maxX, minY, maxY }),
    basis
  )
  return {
    mode,
    basis,
    count: population.count,
    density: population.density,
    densityUnit: 'hectare',
    rows: Math.max(1, Math.round(finiteNumber(values.distributionRows, defaults.rows))),
    columns: Math.max(1, Math.round(finiteNumber(values.distributionColumns, defaults.columns))),
    minX, maxX, minY, maxY,
    seed: Math.round(finiteNumber(values.distributionSeed, defaults.seed)),
    scaleMin: Math.max(.01, finiteNumber(values.distributionScaleMin, defaults.scaleMin)),
    scaleMax: Math.max(.01, finiteNumber(values.distributionScaleMax, defaults.scaleMax)),
    z: finiteNumber(values.distributionZ, defaults.z),
    offsetX: finiteNumber(values.distributionOffsetX, defaults.offsetX),
    offsetY: finiteNumber(values.distributionOffsetY, defaults.offsetY),
    offsetZ: finiteNumber(values.distributionOffsetZ, defaults.offsetZ),
    points: String(values.distributionPoints || '').trim()
  }
}

// 作用：把分布参数填回表单；旧工程若以 km² 为密度单位，先换算为 个/公顷。
function setDistributionForm(form, value) {
  const source = value || {}
  const distribution = {
    ...defaultDistribution(),
    ...source,
    density: densityPerHectare(source.density, source.densityUnit),
    densityUnit: 'hectare'
  }
  const fields = {
    distributionMode: distribution.mode, distributionBasis: distribution.basis,
    distributionCount: distribution.count, distributionDensity: distributionDensityInputValue(distribution.density),
    distributionDensityUnit: 'hectare',
    distributionRows: distribution.rows, distributionColumns: distribution.columns,
    distributionMinX: distribution.minX, distributionMaxX: distribution.maxX,
    distributionMinY: distribution.minY, distributionMaxY: distribution.maxY,
    distributionSeed: distribution.seed, distributionScaleMin: distribution.scaleMin,
    distributionScaleMax: distribution.scaleMax, distributionZ: distribution.z,
    distributionOffsetX: distribution.offsetX, distributionOffsetY: distribution.offsetY,
    distributionOffsetZ: distribution.offsetZ,
    distributionPoints: distribution.points || ''
  }
  for (const [name, fieldValue] of Object.entries(fields)) if (form.elements[name]) form.elements[name].value = fieldValue
}

// 作用：带种子的伪随机数生成器（mulberry32 形式），返回 [0, 1) 的函数；同一种子结果可复现。
function seededRandom(seed) {
  let value = seed >>> 0
  return () => {
    value += 0x6D2B79F5
    let result = value
    result = Math.imul(result ^ result >>> 15, result | 1)
    result ^= result + Math.imul(result ^ result >>> 7, result | 61)
    return ((result ^ result >>> 14) >>> 0) / 4294967296
  }
}

// 作用：计算要生成的实例数：按密度时 = 面积×密度/10000，否则取 count；限幅到 1–100000。
function distributionTargetCount(distribution) {
  const width = Math.max(0, Math.abs(distribution.maxX - distribution.minX))
  const height = Math.max(0, Math.abs(distribution.maxY - distribution.minY))
  const requested = distribution.basis === 'density'
    ? Math.round(width * height * distribution.density / SQUARE_METERS_PER_HECTARE)
    : Math.round(distribution.count)
  return Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, requested || 1))
}

// 可移动对象类型：人、车辆、船舶（这些类型才有“随机移动”设置）。
function isMobileAgentType(type) {
  return type === 'Human' || type === 'Vehicle' || type === 'Ship'
}

// 作用：随机移动的缺省参数；车辆/船舶 speed 8.3 m/s、人 1.3 m/s，
// 单位：speed / speedVariation 为 m/s；range 为相对出生点的最大游走半径（m）；turnInterval 为转向间隔（s，
// 实际每次取 0.5–1.5 倍随机）；seed 为随机种子。随机移动目前只用于三维预览动画（scene-loader.js 的
// attachRandomMovement / updateSceneDynamics），引擎不读取 movement，用户手册 8.5 节也明确“不是交通流求解器”。
function defaultMovement(type) {
  const vehicle = type === 'Vehicle' || type === 'Ship'
  return {
    enabled: false,
    mode: 'random',
    speed: vehicle ? 8.3 : 1.3,
    speedVariation: vehicle ? 1 : .2,
    range: vehicle ? 50 : 20,
    turnInterval: vehicle ? 8 : 5,
    seed: 1
  }
}

// 作用：从表单读取随机移动参数（仅可移动类型，否则返回 null）；速度、范围等做下限保护。
function movementFromForm(form) {
  const type = form.elements.objectType.value
  if (!isMobileAgentType(type)) return null
  const defaults = defaultMovement(type)
  return {
    enabled: form.elements.movementEnabled.checked,
    mode: 'random',
    speed: Math.max(0, finiteNumber(form.elements.movementSpeed.value, defaults.speed)),
    speedVariation: Math.max(0, finiteNumber(form.elements.movementSpeedVariation.value, defaults.speedVariation)),
    range: Math.max(.1, finiteNumber(form.elements.movementRange.value, defaults.range)),
    turnInterval: Math.max(.1, finiteNumber(form.elements.movementTurnInterval.value, defaults.turnInterval)),
    seed: Math.round(finiteNumber(form.elements.movementSeed.value, defaults.seed))
  }
}

// 作用：把移动参数填回表单（缺项用缺省值；模式目前固定为 random）。
function setMovementForm(form, movement, type) {
  const value = { ...defaultMovement(type), ...(movement || {}), mode: 'random' }
  form.elements.movementEnabled.checked = Boolean(value.enabled)
  form.elements.movementSpeed.value = value.speed
  form.elements.movementSpeedVariation.value = value.speedVariation
  form.elements.movementRange.value = value.range
  form.elements.movementTurnInterval.value = value.turnInterval
  form.elements.movementSeed.value = value.seed
}

// 作用：仅对可移动类型显示移动设置，且未勾选“启用”时禁用其余输入框。
function updateMovementFormState(form) {
  const section = $('#objectMovementFields')
  const visible = isMobileAgentType(form.elements.objectType.value)
  section.hidden = !visible
  const enabled = visible && form.elements.movementEnabled.checked
  section.querySelectorAll('input:not([name="movementEnabled"])').forEach((input) => { input.disabled = !enabled })
}

// 作用：解析“自定义坐标”文本，每行 2–5 个数：x y [z [scale [rotation]]]（空白或逗号分隔）。
// 说明：z 缺省取 defaultZ，scale 缺省 1（须 > 0），rotation 缺省 0；以 # 或 // 开头的行为注释；
// 总数不得超过 100000，行号在错误提示中给出。
function parseDistributionPointsText(text, defaultZ = 0) {
  const points = []
  const lines = String(text || '').replace(/^\uFEFF/, '').split(/\r?\n/)
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index].trim()
    if (!line || line.startsWith('#') || line.startsWith('//')) continue
    const values = line.split(/[\s,]+/).filter(Boolean).map(Number)
    if (values.length < 2 || values.length > 5 || !values.every(Number.isFinite)) {
      throw new Error(`自定义坐标第 ${index + 1} 行无效，应为 2–5 个数值`)
    }
    const point = {
      x: values[0], y: values[1], z: values[2] ?? defaultZ,
      scale: values[3] ?? 1, rotation: values[4] ?? 0
    }
    if (point.scale <= 0) throw new Error(`自定义坐标第 ${index + 1} 行的缩放必须大于 0`)
    points.push(point)
  }
  if (!points.length) throw new Error('TXT 中没有有效的自定义坐标')
  if (points.length > MAX_DISTRIBUTION_INSTANCES) throw new Error('单个 OBJ 最多支持 100000 个实例')
  return points
}

// 作用：parseDistributionPointsText 的逆过程，把点列表序列化为文本。
function distributionPointsText(points) {
  return points.map((point) => [point.x, point.y, point.z, point.scale, point.rotation].join(' ')).join('\n')
}

// 作用：给所有实例点统一加上分布的整体偏移 offsetX/Y/Z（m）。
function applyDistributionOffset(points, distribution) {
  const offsetX = finiteNumber(distribution.offsetX, 0)
  const offsetY = finiteNumber(distribution.offsetY, 0)
  const offsetZ = finiteNumber(distribution.offsetZ, 0)
  return points.map((point) => ({
    ...point,
    x: point.x + offsetX,
    y: point.y + offsetY,
    z: point.z + offsetZ
  }))
}

// 作用：按分布参数生成实例点列表 [{x, y, z, scale, rotation}]（最后统一加整体偏移）。
// 模式：manual 读自定义坐标；single 取范围中心、缩放取 scaleMin；
// grid 按范围宽高比选列数 ceil(√(N·w/h))、行数 ceil(N/列数)，等间距铺点（只取前 N 个）；
// random 在范围内均匀随机，缩放在 [scaleMin, scaleMax]，旋转 0–360 度（位置文件 rotation 单位为度：
// 预览用 degToRad 转换，引擎 scene.cpp 的旋转面积掩码也按 rotationDegrees 处理）。
// 说明：范围与缩放的上下限先排序，容忍用户把 min/max 填反；grid 会回写 d.rows/d.columns（仅局部副本）。
function generateDistribution(distribution) {
  const d = { ...defaultDistribution(), ...distribution }
  const minX = Math.min(d.minX, d.maxX), maxX = Math.max(d.minX, d.maxX)
  const minY = Math.min(d.minY, d.maxY), maxY = Math.max(d.minY, d.maxY)
  const scaleMin = Math.min(d.scaleMin, d.scaleMax), scaleMax = Math.max(d.scaleMin, d.scaleMax)
  if (d.mode === 'manual') {
    return applyDistributionOffset(parseDistributionPointsText(d.points, d.z), d)
  }
  if (d.mode === 'single') return applyDistributionOffset([{ x: (minX + maxX) / 2, y: (minY + maxY) / 2, z: d.z, scale: scaleMin, rotation: 0 }], d)
  const targetCount = distributionTargetCount(d)
  if (d.mode === 'grid') {
    const width = Math.max(.001, maxX - minX), height = Math.max(.001, maxY - minY)
    const columns = Math.max(1, Math.ceil(Math.sqrt(targetCount * width / height)))
    const rows = Math.max(1, Math.ceil(targetCount / columns))
    d.rows = rows; d.columns = columns
    return applyDistributionOffset(Array.from({ length: targetCount }, (_, index) => {
      const row = Math.floor(index / columns), column = index % columns
      return {
        x: columns === 1 ? (minX + maxX) / 2 : minX + (maxX - minX) * column / (columns - 1),
        y: rows === 1 ? (minY + maxY) / 2 : minY + (maxY - minY) * row / (rows - 1),
        z: d.z, scale: scaleMin, rotation: 0
      }
    }), d)
  }
  const random = seededRandom(d.seed)
  return applyDistributionOffset(Array.from({ length: targetCount }, () => ({
    x: minX + random() * (maxX - minX), y: minY + random() * (maxY - minY), z: d.z,
    scale: scaleMin + random() * (scaleMax - scaleMin), rotation: random() * 360
  })), d)
}

// 作用：生成对象卡片上的分布摘要：模式 · 密度或实例数 ·（偏移）·（随机移动速度）。
function distributionSummary(item) {
  const labels = { random: '随机', grid: '网格', single: '单实例', manual: '自定义' }
  const count = item.instanceCount || (item.distribution ? distributionTargetCount({ ...defaultDistribution(item), ...item.distribution }) : 1)
  const control = item.distribution?.basis === 'density' ? `${item.distribution.density} 个/公顷` : `${count} 个实例`
  const offsets = [item.distribution?.offsetX, item.distribution?.offsetY, item.distribution?.offsetZ].map((value) => finiteNumber(value, 0))
  const offset = offsets.some((value) => value !== 0) ? ` · 偏移 (${offsets.join(', ')}) m` : ''
  const movement = isMobileAgentType(item.type) && item.movement?.enabled
    ? ` · 随机移动 ${finiteNumber(item.movement.speed, defaultMovement(item.type).speed)} m/s`
    : ''
  return `${labels[item.distribution?.mode] || '已有位置'} · ${control}${offset}${movement}`
}

// 作用：生成下拉选项：首项“新建：xxx”（值 __new__），其后为已有预设，selected 标记当前项。
function optionList(items, newLabel, selected = '__new__') {
  const create = `<option value="__new__" ${selected === '__new__' ? 'selected' : ''}>新建：${escapeHtml(newLabel)}</option>`
  return create + items.map((item) => `<option value="${escapeHtml(item.name)}" ${item.name === selected ? 'selected' : ''}>${escapeHtml(item.label || item.name)} · ${escapeHtml(item.name)}</option>`).join('')
}

// 材质/预设对话框（#materialForm）的字段读写小工具：按 name 取输入框，值非空时才写入。
function formField(name) { return document.querySelector("#materialForm [name=\"" + name + "\"]") }

function setFormField(name, value) {
  const input = formField(name)
  if (input && value !== undefined && value !== null) input.value = value
}

// 作用：由对象类型映射到物化属性（材质）类型：光伏板→Photovoltaic；植被/火焰/雾→Vegetation；其余按类型，未知为 Soil。
function physicalTypeForObject(type) {
  if (type === 'SolarPanel') return 'Photovoltaic'
  if (type === 'Vegetation' || type === 'Fire' || type === 'Fog') return 'Vegetation'
  if (type === 'Water' || type === 'Ship' || type === 'Building' || type === 'Vehicle') return type
  return 'Soil'
}

// 作用：取对象的分类标签：光伏板单独保留，其余用 item.type（缺省 Other）。
function objectClassification(item) {
  return item.classification === 'SolarPanel' ? 'SolarPanel' : (item.type || 'Other')
}

// 作用：由界面分类得到要写入对象的字段；光伏板在引擎中按 Building 处理，分类以 classification 持久记录。
function objectTypeFields(classification) {
  // Keep the engine's supported solid type; the UI classification is persistent metadata.
  return { type: classification === 'SolarPanel' ? 'Building' : classification,
    classification: classification === 'SolarPanel' ? 'SolarPanel' : undefined }
}

// 作用：判断某个物化属性（material）能否绑定到某类对象（objectType），用于过滤下拉选项。
// 规则：光伏模型除火焰/雾外都可用；薄膜模型除火焰/雾/水外都可用；
// 其余要求 material.type 与对象类型对应的物化类型一致（见 physicalTypeForObject），
// 并兼容旧工程：建筑可用 Soil；车辆/人可用无 energyModel 的 Building 材质。
function materialMatchesObjectType(material, objectType) {
  if (!material) return false
  // A surface's geometry and semantic label do not restrict photovoltaic EB.
  if (material.energyModel === 'photovoltaic') return objectType !== 'Fire' && objectType !== 'Fog'
  // A thin sheet (tarp, shade net) can cover any solid object.
  if (material.energyModel === 'membrane') return !['Fire', 'Fog', 'Water'].includes(objectType)
  // Keep legacy soil bindings available for existing buildings.
  return material.type === physicalTypeForObject(objectType)
    || (objectType === 'Building' && material.type === 'Soil')
    // The engine exports vehicles and people as Building solids.
    || ((objectType === 'Vehicle' || objectType === 'Human') && material.type === 'Building' && !material.energyModel)
}

// 作用：生成对话框中带标签的数值输入框 HTML（供物化属性等表单拼装使用）。
function physicalInput(label, name, value, step = 'any') {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><input class="input" name="${name}" type="number" step="${step}" value="${escapeHtml(value)}"></label>`
}

// FacetEB building wall/roof mode for solid (Building/Soil/Ship) materials on scene objects.
function wallModeFields(defaults = {}) {
  const wall = defaults.wallMode === 'wall'
  return `<label class="dialog-field"><span>墙体模式（FacetEB 对象面元）</span><select class="select" name="wallMode"><option value="shell" ${wall ? '' : 'selected'}>薄壳（两面室外，旧算法）</option><option value="wall" ${wall ? 'selected' : ''}>墙体/屋顶（厚度导热 + 恒温室内）</option></select></label>${physicalInput('墙体厚度（m，无构造层时使用）', 'wallThickness', defaults.thickness ?? 0.2, 0.01)}${physicalInput('室内温度（℃）', 'indoorTemperature', defaults.indoorTemperature ?? 22, 0.1)}${physicalInput('室内换热系数（W·m⁻²·K⁻¹）', 'indoorHeatTransferCoefficient', defaults.indoorHeatTransferCoefficient ?? 8, 0.1)}<label class="dialog-field" style="grid-column:1 / -1"><span>构造层（由外到内，每行：厚度 m, λ W·m⁻¹·K⁻¹, ρ kg·m⁻³, cp J·kg⁻¹·K⁻¹；最多 6 层；留空 = 用上面的厚度与 λ/ρ/cs 单层均质）</span><textarea class="input" name="wallLayers" rows="3">${escapeHtml(wallLayersText(defaults.layers))}</textarea></label>`
}

// FacetEB ship hull options (Ship materials): relative wind and waterline contact.
function shipFields(defaults = {}) {
  const fromWater = defaults.waterlineSource === 'water' && !shipValueGiven(defaults.waterlineHeight)
  const yesNo = (name, label, on) => `<label class="dialog-field"><span>${label}</span><select class="select" name="${name}"><option value="0" ${on ? '' : 'selected'}>关闭</option><option value="1" ${on ? 'selected' : ''}>开启</option></select></label>`
  return `<div class="notice">VoxelEB 船体体元读取航速、航向/风来向、固定水线高度与水侧换热系数（水温取场景水体体元平均，不反馈水温）、内热源面功率，以及“船体按钢板墙体计算”开关打开时的墙体厚度/构造层（折算为一块等效均质板）与室内温度/换热系数；水面吃水、水温、遮蔽、反馈与内热源总功率仅 FacetEB 生效。</div>${physicalInput('航速（m/s；0 = 停泊）。无航向/风向时相对风 √(u²+v²)，有则 |u风−v船|', 'shipSpeed', defaults.speed ?? 0, 0.1)}${physicalInput('航向（°，船首朝向，罗经方位：北 0°顺时针；与风向同时给出，留空 = 按垂直取模）', 'shipHeading', defaults.heading ?? '', 1)}${physicalInput('风来向（°，风吹来的方向，罗经方位：北 0°顺时针；留空同上）', 'shipWindDirection', defaults.windDirection ?? '', 1)}${physicalInput('水线高度（m，世界高度；留空 = 不计水线，或用下面的水面吃水）', 'shipWaterlineHeight', defaults.waterlineHeight ?? '', 0.01)}<label class="dialog-field"><span>水线来源（水线高度留空时）</span><select class="select" name="shipWaterlineSource"><option value="height" ${fromWater ? '' : 'selected'}>固定高度（上面的水线高度）</option><option value="water" ${fromWater ? 'selected' : ''}>场景水面（真实吃水，按面元浸湿比例）</option></select></label>${physicalInput('水面吃水偏移（m，正值抬高水线）', 'shipWaterlineOffset', defaults.waterlineOffset ?? 0, 0.01)}${physicalInput('水侧换热系数（W·m⁻²·K⁻¹）', 'shipWaterHeatTransferCoefficient', defaults.waterHeatTransferCoefficient ?? 500, 1)}${physicalInput('水温（℃，留空 = 场景水体平均温度，无水体时首节点气温）', 'shipWaterTemperature', defaults.waterTemperature ?? '', 0.1)}${yesNo('shipWaterShielding', '水线下面元遮蔽（不接收短波/长波）', defaults.waterShielding)}${yesNo('shipWaterFeedback', '船体热量反馈场景水温（需场景水体，不能同时给固定水温）', defaults.waterFeedback)}${yesNo('shipVoxelWall', 'VoxelEB：船体按钢板墙体计算（需墙体模式 = 墙体；关闭 = 半无限固体，旧工程缺省）', defaults.voxelWall)}${physicalInput('内热源面功率（W·m⁻²，面元面积均摊；缺省 0）', 'shipInternalHeatFlux', defaults.internalHeatFlux ?? 0, 1)}${physicalInput('内热源总功率（W，按该网格组面元面积均摊；烟囱/机舱请单独建物化；缺省 0）', 'shipInternalHeatPower', defaults.internalHeatPower ?? 0, 100)}`
}

// FacetEB vehicle cabin (Vehicle materials, docs/vehicle-thermal.md): part role and, for the body, the cabin / driving
// parameters. Shown in FacetEB mode only; other modes keep the fixed temperature presets.
function vehicleFields(defaults = {}) {
  if (state.mode !== 'eFacetEB') return ''
  const c = defaults.cabin || {}
  const part = defaults.vehiclePart || ''
  const checked = (value) => (value ? 'checked' : '')
  return `<label class="dialog-field"><span>车辆部件（FacetEB）</span><select class="select" name="vehiclePart"><option value="" ${part ? '' : 'selected'}>不参与座舱（旧算法）</option><option value="body" ${part === 'body' ? 'selected' : ''}>车身（钢板，携带座舱参数）</option><option value="glass" ${part === 'glass' ? 'selected' : ''}>车窗玻璃（透射太阳进座舱）</option><option value="underbody" ${part === 'underbody' ? 'selected' : ''}>底盘/轮胎/灯具（薄壳，发动机热源）</option><option value="hood" ${part === 'hood' ? 'selected' : ''}>引擎盖（内侧 = 发动机舱空气，需发动机模型）</option></select></label>`
    + `<label class="dialog-field"><span>启用座舱（仅车身部件）</span><input name="cabinEnabled" type="checkbox" value="1" ${checked(c.enabled)}></label>`
    + physicalInput('乘员数（0–8）', 'cabinOccupants', c.occupants ?? 0, 1) + physicalInput('每人代谢热（W）', 'cabinMetabolicW', c.metabolicW ?? 100, 1)
    + physicalInput('车速（m/s，0 = 停放）', 'cabinSpeed', c.speed ?? 0, 0.1) + physicalInput('航向（°，留空 = 风垂直于行驶）', 'cabinHeading', c.heading ?? '', 1)
    + physicalInput('风来向（°）', 'cabinWindDirection', c.windDirection ?? '', 1) + physicalInput('关窗通风换气（ACH）', 'cabinVentilationACH', c.ventilationACH ?? 0.5, 0.1)
    + `<label class="dialog-field"><span>开窗缝</span><input name="cabinWindowCracked" type="checkbox" value="1" ${checked(c.windowCracked)}></label>` + physicalInput('开窗缝 ACH', 'cabinCrackedACH', c.crackedACH ?? 10, 1)
    + `<label class="dialog-field"><span>开空调</span><input name="cabinAcOn" type="checkbox" value="1" ${checked(c.acOn)}></label>` + physicalInput('空调制冷功率（W）', 'cabinAcPower', c.acPower ?? 3500, 10)
    + physicalInput('空调设定温度（℃）', 'cabinAcSetpoint', c.acSetpoint ?? 24, 0.5)
    + `<label class="dialog-field"><span>发动机运转</span><input name="cabinEngineOn" type="checkbox" value="1" ${checked(c.engineOn)}></label>` + physicalInput('发动机热功率（W）', 'cabinEnginePower', c.enginePower ?? 15000, 100)
    + physicalInput('进入罩下/底盘的份额', 'cabinEngineShare', c.engineShare ?? 0.3, 0.05)
    + physicalInput('座舱容积（m³）', 'cabinVolume', c.volume ?? 2.8, 0.1) + physicalInput('内饰热容（J·K⁻¹）', 'cabinInteriorCapacity', c.interiorCapacity ?? 80000, 1000)
    + physicalInput('内饰面积（m²）', 'cabinInteriorArea', c.interiorArea ?? 8, 0.5) + physicalInput('内饰换热系数（W·m⁻²·K⁻¹）', 'cabinInteriorH', c.interiorH ?? 8, 0.5)
    + physicalInput('内饰吸收透射太阳的份额', 'cabinAbsorbedFraction', c.absorbedFraction ?? 0.8, 0.05)
    + vehiclePartsFields(c)
}

// ---- Vehicle object dialog: "车辆参数" fold-out, auto-binding of the OBJ groups, per-vehicle body material ----
// 车型预设：名称与座舱容积（m³），用于车辆对象对话框的“车型 / 座舱容积”下拉框。
const VEHICLE_CAR_TYPES = { sedan: { label: '轿车（座舱 2.8 m³）', volume: 2.8 }, suv: { label: 'SUV（座舱 3.6 m³）', volume: 3.6 }, van: { label: '厢式/MPV（座舱 5.0 m³）', volume: 5.0 } }
// 是否启用“自动绑定材料族”：仅新建车辆对象、FacetEB 模式且勾选了 vpAutoBind 时为真。
function vehicleAutoBindEnabled() {
  return !!state.pendingObject && state.mode === 'eFacetEB' && $('#materialObjectType').value === 'Vehicle' && document.querySelector('#vehicleParamFields [name="vpAutoBind"]')?.checked === true
}
// 作用：在新建车辆对象的对话框中生成“车辆参数”折叠区（只构建一次，dataset.built 标记）。
// 内容：车型/容积、乘员、状态与车速（m/s）、发动机、空调、通风、窗膜、车漆、座椅；
// 任一选项变化时重新生成网格组的材质行。仅 FacetEB 模式的新对象可见。
function renderVehicleParams() {
  const section = $('#vehicleParamsSection')
  if (!section) return
  const visible = !!state.pendingObject && state.mode === 'eFacetEB' && $('#materialObjectType').value === 'Vehicle'
  section.hidden = !visible
  if (!visible || $('#vehicleParamFields').dataset.built === '1') return
  const select = (name, label, options, selected) => `<label class="dialog-field"><span>${label}</span><select class="select" name="${name}">${options.map(([value, text]) => `<option value="${value}" ${value === selected ? 'selected' : ''}>${text}</option>`).join('')}</select></label>`
  const check = (name, label, on) => `<label class="dialog-field"><span>${label}</span><input name="${name}" type="checkbox" value="1" ${on ? 'checked' : ''}></label>`
  $('#vehicleParamFields').innerHTML = check('vpAutoBind', '自动绑定材料族（按网格组名；关闭 = 沿用下表默认）', true)
    + select('vpCarType', '车型 / 座舱容积', Object.entries(VEHICLE_CAR_TYPES).map(([id, entry]) => [id, entry.label]), 'sedan')
    + physicalInput('座舱容积（m³，覆盖车型）', 'vpVolume', '', 0.1) + physicalInput('乘员数（0–8）', 'vpOccupants', 0, 1)
    + select('vpState', '状态', [['park', '停放'], ['drive', '行驶']], 'park') + physicalInput('车速（m/s，行驶）', 'vpSpeed', 15, 0.5)
    + physicalInput('航向（°，留空 = 风垂直于行驶）', 'vpHeading', '', 1) + physicalInput('风来向（°）', 'vpWindDirection', '', 1)
    + check('vpEngineOn', '发动机运转', false) + physicalInput('发动机热功率（W，总量 = 功率 × 进车份额 0.3）', 'vpEnginePower', 15000, 100) + check('vpEngineWarm', '热车（发动机起始已热，停放余热衰减）', false)
    + check('vpAcOn', '开空调', false) + physicalInput('空调制冷功率（W）', 'vpAcPower', 3500, 10) + physicalInput('空调设定温度（℃）', 'vpAcSetpoint', 24, 0.5)
    + select('vpVent', '通风', [['closed', '关窗（0.5 ACH）'], ['crack', '开窗缝（10 ACH）'], ['open', '开窗（30 ACH）']], 'closed')
    + select('vpFilm', '窗户膜', [['plain', '普通（前挡风 τ 0.60 / 侧窗 0.70 / 后窗 0.65）'], ['insulated', '隔热膜 / Low-E（τ 0.38–0.45）']], 'plain')
    + select('vpPaint', '车漆颜色', VEHICLE_PAINT_COLORS.map((entry) => [entry.id, entry.label]), 'white')
    + select('vpSeat', '座椅类型', [['fabric', '织物'], ['leather', '皮革']], 'fabric')
  $('#vehicleParamFields').dataset.built = '1'
  for (const element of $$('#vehicleParamFields select, #vehicleParamFields input[name="vpAutoBind"]'))
    element.addEventListener('change', () => { if (state.pendingObject) renderMeshMaterialRows() })
}
// 作用：读取自动绑定所需的车漆颜色（缺省 white）与是否隔热膜；values 为空时直接读表单。
function vehicleBindingOptions(values = null) {
  const read = (name) => (values ? values[name] : document.querySelector(`#vehicleParamFields [name="${name}"]`)?.value)
  return { paint: read('vpPaint') || 'white', film: read('vpFilm') === 'insulated' }
}
// 补充：乘员数限幅 0–8；停放时车速为 0；航向与风来向须同时给出才生效；
// 表单项为空或非数值时取括号内的缺省（发动机 15000 W、空调 3500 W/24 ℃ 等）。
// Body material of a new vehicle object: the vehicle_material preset with the cabin parameters of the form.
function vehicleBodyMaterial(values, name) {
  const n = (key, fallback) => (String(values[key] ?? '').trim() !== '' && Number.isFinite(Number(values[key])) ? Number(values[key]) : fallback)
  const given = String(values.vpHeading ?? '').trim() !== '' && String(values.vpWindDirection ?? '').trim() !== ''
  return createVehicleBodyMaterial(name, { occupants: Math.max(0, Math.min(8, Math.round(n('vpOccupants', 0)))),
    volume: n('vpVolume', (VEHICLE_CAR_TYPES[values.vpCarType] || VEHICLE_CAR_TYPES.sedan).volume), speed: values.vpState === 'drive' ? Math.max(0, n('vpSpeed', 15)) : 0,
    heading: given ? Number(values.vpHeading) : undefined, windDirection: given ? Number(values.vpWindDirection) : undefined, vent: values.vpVent || 'closed',
    engineOn: values.vpEngineOn === '1', enginePower: n('vpEnginePower', 15000), engineWarm: values.vpEngineWarm === '1', acOn: values.vpAcOn === '1',
    acPower: n('vpAcPower', 3500), acSetpoint: n('vpAcSetpoint', 24), seat: values.vpSeat })
}
// Adds the preset spectra / materials the auto-binding refers to (only the missing ones, only for a new vehicle object).
function ensureVehiclePresets() {
  const presets = createMaterialPresets()
  for (const spectrum of VEHICLE_SPECTRA) if (!state.config.spectra.some((entry) => entry.name === spectrum.name)) state.config.spectra.push(structuredClone(spectrum))
  for (const material of presets.materials.filter((entry) => entry.type === 'Vehicle' && entry.name !== 'vehicle_material'))
    if (!state.config.materials.some((entry) => entry.name === material.name)) state.config.materials.push(structuredClone(material))
}

// Interior nodes / engine model fields of a body material (docs/vehicle-thermal.md); the remaining node
// parameters (capacity, hA, radiating area, block/bay capacities) travel in a hidden JSON and keep their values.
function vehiclePartsFields(c = {}) {
  const it = c.interior, en = c.engine
  const checked = (value) => (value ? 'checked' : '')
  const hidden = escapeHtml(JSON.stringify({ interior: it || null, engine: en || null }))
  const share = (id) => it?.[id]?.share ?? vehicleInteriorDefaults()[id].share
  return `<input type="hidden" name="cabinPartsJson" value="${hidden}">`
    + `<label class="dialog-field"><span>内饰分节点（仪表台/座椅/方向盘/内衬，关闭 = 单一集总内饰）</span><input name="cabinInteriorNodes" type="checkbox" value="1" ${checked(it?.enabled)}></label>`
    + `<label class="dialog-field"><span>座椅类型</span><select class="select" name="cabinSeatType"><option value="fabric" ${it?.seatType === 'leather' ? '' : 'selected'}>织物</option><option value="leather" ${it?.seatType === 'leather' ? 'selected' : ''}>皮革</option></select></label>`
    + physicalInput('仪表台吸收份额', 'cabinShareDashboard', share('dashboard'), 0.05) + physicalInput('座椅吸收份额', 'cabinShareSeat', share('seat'), 0.05)
    + physicalInput('方向盘吸收份额', 'cabinShareWheel', share('wheel'), 0.05) + physicalInput('车顶内衬/门板吸收份额（四项之和 = 1）', 'cabinShareLining', share('lining'), 0.05)
    + physicalInput('内饰与壳体内表面的辐射份额 φ', 'cabinShellRadFraction', it?.shellRadFraction ?? 0.45, 0.05)
    + `<label class="dialog-field"><span>发动机/发动机舱模型（发动机块 + 舱内空气 + 引擎盖内侧 + 排气）</span><input name="cabinEngineModel" type="checkbox" value="1" ${checked(en?.enabled)}></label>`
    + `<label class="dialog-field"><span>热车（停放起始发动机已热，余热指数衰减）</span><input name="cabinEngineWarm" type="checkbox" value="1" ${checked(en?.warm)}></label>`
    + physicalInput('热车初始机体温度（℃）', 'cabinEngineWarmTemperature', en?.warmTemperature ?? 90, 1)
    + physicalInput('废热份额：散热器（进舱内空气）', 'cabinEngineRadiatorShare', en?.radiatorShare ?? 0.5, 0.05) + physicalInput('废热份额：发动机块', 'cabinEngineBlockShare', en?.blockShare ?? 0.3, 0.05)
    + physicalInput('废热份额：排气（加到底盘面元；三项之和 = 1）', 'cabinEngineExhaustShare', en?.exhaustShare ?? 0.2, 0.05)
    + physicalInput('防火墙向座舱传热（W·K⁻¹，0 = 不传）', 'cabinFirewallG', en?.firewallG ?? 2, 0.5)
}

// FacetEB wall layers as text: one layer per line "thickness, λ, ρ, cp" (outer to inner).
function wallLayersText(layers) {
  return Array.isArray(layers) ? layers.map((layer) => [layer.thickness, layer.lambda, layer.rho, layer.cp].join(', ')).join('\n') : ''
}

// 作用：解析墙体构造层文本：以换行或分号分行，每行 “厚度, λ, ρ, cp”（空白或逗号分隔，含全角逗号），
// 由外到内；本函数不校验数值有效性，层数上限与合法性由调用方/引擎处理（待核对）。
function parseWallLayers(text) {
  return String(text || '').split(/[\n;]+/).map((row) => row.trim()).filter(Boolean).map((row) => {
    const [thickness, lambda, rho, cp] = row.split(/[\s,，]+/).filter(Boolean).map(Number)
    return { thickness, lambda, rho, cp }
  })
}

// FacetEB water start temperature (absent = first-node air temperature, the old behaviour).
function waterInitialFields(defaults = {}) {
  const mode = defaults.initialTemperatureMode || 'firstAir'
  const option = (value, label) => `<option value="${value}" ${mode === value ? 'selected' : ''}>${label}</option>`
  return `<label class="dialog-field"><span>初始水温（FacetEB）</span><select class="select" name="waterInitialMode">${option('firstAir', '首节点气温（旧行为）')}${option('dailyMeanAir', '起始 24 h 平均气温')}${option('fixed', '指定温度')}</select></label>${physicalInput('指定初始水温（℃）', 'waterInitialTemperature', defaults.initialTemperature ?? 20, 0.1)}`
}

// 作用：自定义光谱的“波谱文件”导入控件；TXT 约定：第 1 行波长，第 2 行反射率，第 3 行可选透射率。
function spectrumFileImportFields(prefix, fileName = '') {
  return `<label class="dialog-field"><span>波谱文件</span><input class="input" id="${prefix}SpectrumFile" type="file" accept=".txt,.dat,.csv,text/plain"></label><input type="hidden" name="fileName" value="${escapeHtml(fileName)}"><div class="notice" id="${prefix}SpectrumFileStatus">${fileName ? `已导入：${escapeHtml(fileName)}` : 'TXT 第 1 行为波长，第 2 行为反射率，第 3 行可选为透射率。'}</div>`
}

// 作用：光谱预设对话框中“物理纹理”（反射率空间方差）的表单字段与说明。
// 说明：纹理文件取自预设列表，文件不在列表中时作为自定义项保留；缺省强度 0.2、重复尺寸 2 m。
// 仅 FacetRT / FacetEB / RayTracing 生效，体元模式不支持（见面板说明）。
function physicalTextureFields(texture = {}) {
  const enabled = Boolean(texture.enabled)
  const fileName = String(texture.fileName || PHYSICAL_TEXTURE_PRESETS[0][0])
  const known = PHYSICAL_TEXTURE_PRESETS.some(([value]) => value === fileName)
  const options = PHYSICAL_TEXTURE_PRESETS.map(([value, label]) =>
    `<option value="${escapeHtml(value)}" ${value === fileName ? 'selected' : ''}>${escapeHtml(label)}</option>`
  ).join('')
  const custom = !known && fileName
    ? `<option value="${escapeHtml(fileName)}" selected>${escapeHtml(fileName)}</option>` : ''
  return `<label class="dialog-field"><span>物理纹理</span><select class="select" name="physicalTextureEnabled"><option value="false" ${enabled ? '' : 'selected'}>关闭（默认）</option><option value="true" ${enabled ? 'selected' : ''}>启用</option></select></label><label class="dialog-field"><span>方差模板</span><select class="select" name="physicalTextureFileName">${options}${custom}</select></label><label class="dialog-field"><span>导入纹理图像</span><input class="input physical-texture-file" type="file" accept=".png,.jpg,.jpeg,.tif,.tiff,image/png,image/jpeg,image/tiff"></label>${physicalInput('方差强度（0–1）', 'physicalTextureStrength', texture.strength ?? 0.2, 0.01)}${physicalInput('反射率变异系数 CV（0–0.5，0 = 按方差强度）', 'physicalTextureRelativeStd', texture.relativeStd ?? 0, 0.01)}${physicalInput('重复尺寸（m）', 'physicalTextureRepeatSize', texture.repeatSize ?? 2, 0.01)}<div class="notice" style="grid-column:1 / -1">仅改变短波反射率的空间方差（按图像亮度，所有波段同一倍率）；按材质面积归一化，平均反射率保持不变。变异系数 CV &gt; 0 时直接指定反射率的相对标准差（例如 0.08 表示 ±8%），图像只决定斑块形状和尺度；为 0 时按“方差强度”缩放，实际 CV 通常只有强度的 1/4 左右。为保证反射率为正且不超过 1−透射率，实际 CV 可能被限低，运行日志会给出实际 CV。热红外发射率和透射率不随此纹理变化。每个三角面取面心处一个值，网格需细于纹理斑块。FacetRT / FacetEB / RayTracing 生效，体元模式不支持。</div>`
}

// 作用：绑定“导入纹理图像”：上传到后端，加入下拉框并选中、启用物理纹理，同时使该文件的纹理缓存失效。
function bindPhysicalTextureImport(rootSelector) {
  const root = $(rootSelector)
  const input = root?.querySelector('.physical-texture-file')
  const select = root?.querySelector('[name="physicalTextureFileName"]')
  if (!input || !select) return
  input.addEventListener('change', async () => {
    const file = input.files?.[0]
    if (!file) return
    try {
      input.disabled = true
      const imported = await api.importTexture({ name: file.name, contentBase64: await readFileBase64(file) })
      if (![...select.options].some((option) => option.value === imported.relativePath)) {
        select.insertAdjacentHTML('beforeend', `<option value="${escapeHtml(imported.relativePath)}">${escapeHtml(imported.relativePath)}</option>`)
      }
      select.value = imported.relativePath
      invalidateTextureCaches(imported.relativePath)
      const enabled = root.querySelector('[name="physicalTextureEnabled"]')
      if (enabled) enabled.value = 'true'
      toast('纹理导入成功', imported.relativePath, 'success')
    } catch (error) {
      input.value = ''
      toast('纹理导入失败', error.message, 'error')
    } finally { input.disabled = false }
  })
}

// 作用：光谱的“开孔率”输入（遮阳网、穿孔篷布）。运行时的等效换算见面板说明：
// R′=(1−O)R、T′=O+(1−O)T；孔中透过的直射光按漫射处理。
function opennessField(value = 0) {
  return `${physicalInput('开孔率（0–0.95）', 'openness', value ?? 0, 0.01)}<div class="notice" style="grid-column:1 / -1">用于遮阳网、穿孔篷布：上方反射率/透射率填纤维本身的值，运行时按开孔率 O 换算为等效值 R′=(1−O)R、T′=O+(1−O)T（热红外同理）。孔中透过的直射光按漫射处理，棚下不形成光斑。仅自定义和导入 TXT 光谱可用。</div>`
}

// 作用：生成纹理文件下拉选项；当前文件不在预设里时追加为已选的自定义项。
function textureFileOptions(presets, fileName) {
  const known = presets.some(([value]) => value === fileName)
  return presets.map(([value, label]) => `<option value="${escapeHtml(value)}" ${value === fileName ? 'selected' : ''}>${escapeHtml(label)}</option>`).join('')
    + (!known && fileName ? `<option value="${escapeHtml(fileName)}" selected>${escapeHtml(fileName)}</option>` : '')
}

// 作用：生成“关闭（默认）/启用”下拉框，值为字符串 'false' / 'true'。
function enabledSelect(label, name, enabled) {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><select class="select" name="${name}"><option value="false" ${enabled ? '' : 'selected'}>关闭（默认）</option><option value="true" ${enabled ? 'selected' : ''}>启用</option></select></label>`
}

// 作用：“光谱分类纹理”表单：分类 PNG 中每种颜色绑定一条光谱，运行时按三角面面心取色并归入最接近的类别；
// 启用后本光谱自身反射率不再使用（见面板说明）。缺省重复尺寸 1.5 m。
function classTextureFields(texture = {}) {
  const fileName = String(texture.fileName || CLASS_TEXTURE_PRESETS[0][0])
  return `${enabledSelect('光谱分类纹理', 'classTextureEnabled', Boolean(texture.enabled))}<label class="dialog-field"><span>分类图像（PNG）</span><select class="select" name="classTextureFileName">${textureFileOptions(CLASS_TEXTURE_PRESETS, fileName)}</select></label><label class="dialog-field"><span>导入分类图像</span><input class="input class-texture-file" type="file" accept=".png,image/png"></label>${physicalInput('分类纹理重复尺寸（m）', 'classTextureRepeatSize', texture.repeatSize ?? 1.5, 0.01)}<div class="class-texture-rows" style="grid-column:1 / -1" data-classes-file="${escapeHtml(fileName)}" data-classes="${escapeHtml(JSON.stringify(texture.classes || []))}"></div><div class="notice" style="grid-column:1 / -1">图像中每种颜色绑定一条光谱：运行时每个三角面按面心取色，归入颜色最接近的类别并使用该类别光谱（含其透射率、热红外、物理纹理和开孔率设置）。未指定光谱的颜色归入最接近的已指定颜色。按对象自身坐标投影，图案随对象移动；需使用无损 PNG。启用后本光谱自身的反射率不再使用。</div>`
}

// 作用：“开孔率纹理”表单：灰度 PNG 的亮度在最小/最大开孔率间线性映射并量化为 levels 档（缺省 8，范围 2–32），
// 启用后覆盖固定开孔率。缺省范围 0–0.5，重复尺寸 1 m。
function opennessTextureFields(texture = {}) {
  const fileName = String(texture.fileName || OPENNESS_TEXTURE_PRESETS[0][0])
  return `${enabledSelect('开孔率纹理', 'opennessTextureEnabled', Boolean(texture.enabled))}<label class="dialog-field"><span>开孔率图像（灰度 PNG）</span><select class="select" name="opennessTextureFileName">${textureFileOptions(OPENNESS_TEXTURE_PRESETS, fileName)}</select></label><label class="dialog-field"><span>导入开孔率图像</span><input class="input openness-texture-file" type="file" accept=".png,image/png"></label>${physicalInput('开孔率纹理重复尺寸（m）', 'opennessTextureRepeatSize', texture.repeatSize ?? 1, 0.01)}${physicalInput('最小开孔率（黑）', 'opennessTextureMin', texture.minOpenness ?? 0, 0.01)}${physicalInput('最大开孔率（白）', 'opennessTextureMax', texture.maxOpenness ?? 0.5, 0.01)}${physicalInput('开孔率档位数（2–32）', 'opennessTextureLevels', texture.levels ?? 8, 1)}<div class="notice" style="grid-column:1 / -1">图像亮度在最小、最大开孔率之间线性映射（黑 = 最小，白 = 最大），量化为若干档；每个三角面按面心取值，运行时按 R′=(1−O)R、T′=O+(1−O)T 换算透射率和反射率（热红外同理）。启用后覆盖上方固定开孔率。仅自定义和导入 TXT 光谱可用。</div>`
}

// 分类图像调色板缓存：键为工程相对路径，值为统计颜色的 Promise。
const imagePaletteCache = new Map()

// Both caches are keyed by project-relative path; omit fileName on project switch.
function invalidateTextureCaches(fileName) {
  if (fileName === undefined) imagePaletteCache.clear()
  else imagePaletteCache.delete(fileName)
  invalidateImageTextures(fileName)
}

// 作用：统计分类纹理 PNG 的颜色，返回 Promise<[{ color: '#rrggbb', share }]>。
// 说明：按 RGB（忽略 alpha）计数，只保留占比 ≥ 0.5% 的颜色，最多 12 种、按占比降序；
// 结果按文件名缓存（imagePaletteCache），加载失败时移出缓存以便重试。
function imagePalette(fileName) {
  if (imagePaletteCache.has(fileName)) return imagePaletteCache.get(fileName)
  const promise = new Promise((resolve, reject) => {
    const image = new Image()
    // DEFERRED[低|性能]: 原尺寸绘制并在主线程逐像素统计颜色，大分类图（如 8K×8K）会长时间阻塞 UI 并占用数百 MB 内存；建议：限制最大边长后缩放采样，或放到 Worker/OffscreenCanvas 中统计 —— 需要引入新的最大边长常量并改变调色板统计口径（缩放采样会影响颜色占比），本轮不改
    image.onload = () => {
      const canvas = document.createElement('canvas')
      canvas.width = image.naturalWidth
      canvas.height = image.naturalHeight
      const context = canvas.getContext('2d', { willReadFrequently: true })
      context.drawImage(image, 0, 0)
      const data = context.getImageData(0, 0, canvas.width, canvas.height).data
      const counts = new Map()
      for (let index = 0; index < data.length; index += 4) {
        const key = (data[index] << 16) | (data[index + 1] << 8) | data[index + 2]
        counts.set(key, (counts.get(key) || 0) + 1)
      }
      const total = data.length / 4
      const colors = [...counts].sort((a, b) => b[1] - a[1]).filter(([, count]) => count / total >= 0.005).slice(0, 12)
        .map(([key, count]) => ({ color: `#${key.toString(16).padStart(6, '0')}`, share: count / total }))
      resolve(colors)
    }
    image.onerror = () => reject(new Error('图像加载失败'))
    image.src = `/api/project/image?path=${encodeURIComponent(fileName)}`
  })
  promise.catch(() => imagePaletteCache.delete(fileName))
  imagePaletteCache.set(fileName, promise)
  return promise
}

// 各分类纹理容器的最新请求序号：用于丢弃过期的异步调色板结果。
const classTextureRequests = new WeakMap()

// 作用：为所选分类图像生成“颜色 → 光谱”绑定行（每种主要颜色一个下拉框）。
// 步骤 1：收集已有绑定（先取界面里的，没有则取 data-classes 里保存的）。
// 步骤 2：读取调色板期间以隐藏域保留绑定，使表单仍可提交（仅当属于同一图像）。
// 步骤 3：调色板返回后重绘；候选光谱排除自身和已启用分类纹理的光谱，推测为避免循环引用（待核对）。
async function renderClassTextureRows(root, currentSpectrumName) {
  const container = root?.querySelector('.class-texture-rows')
  const select = root?.querySelector('[name="classTextureFileName"]')
  if (!container || !select) return
  const requestedFile = select.value
  const request = (classTextureRequests.get(container) || 0) + 1
  classTextureRequests.set(container, request)
  const existing = [...root.querySelectorAll('[name="classColor"]')].map((input, index) => ({
    color: input.value, spectralName: root.querySelectorAll('[name="classSpectral"]')[index]?.value || ''
  }))
  let saved = existing
  if (!saved.length) {
    try { saved = JSON.parse(container.dataset.classes || '[]') } catch { saved = [] }
  }
  // While the palette loads, keep the bindings submittable, but only when they
  // belong to the image being loaded (colours of another image would misclassify).
  const pending = container.dataset.classesFile === requestedFile
    ? saved.map((entry) => `<input type="hidden" name="classColor" value="${escapeHtml(entry.color)}"><input type="hidden" name="classSpectral" value="${escapeHtml(entry.spectralName)}">`).join('')
    : ''
  container.innerHTML = `<div class="notice">正在读取图像颜色…</div>${pending}`
  const isCurrent = () => classTextureRequests.get(container) === request && select.value === requestedFile
  try {
    const palette = await imagePalette(requestedFile)
    if (!isCurrent()) return
    container.dataset.classesFile = requestedFile
    const choices = (state.config.spectra || []).filter((item) => item.name !== currentSpectrumName && !item.classTexture?.enabled)
    container.innerHTML = palette.length ? palette.map(({ color, share }) => {
      const chosen = saved.find((entry) => entry.color === color)?.spectralName || ''
      const options = choices.map((item) => `<option value="${escapeHtml(item.name)}" ${item.name === chosen ? 'selected' : ''}>${escapeHtml(item.label || item.name)} · ${escapeHtml(item.name)}</option>`).join('')
      return `<div style="display:flex;gap:8px;align-items:center;margin:4px 0"><span style="flex:none;width:22px;height:22px;border-radius:4px;border:1px solid rgba(0,0,0,.25);background:${color}"></span><code style="flex:none">${color}</code><small style="flex:none;width:48px">${(share * 100).toFixed(1)}%</small><input type="hidden" name="classColor" value="${color}"><select class="select" name="classSpectral" style="flex:1"><option value="">（未指定：归入最接近的已指定颜色）</option>${options}</select></div>`
    }).join('') : '<div class="notice">图像中没有占比 ≥ 0.5% 的颜色。</div>'
  } catch (error) {
    if (!isCurrent()) return
    container.innerHTML = `<div class="notice">无法读取图像颜色：${escapeHtml(error.message)}</div>${pending}`
  }
}

// 作用：通用的纹理图像导入绑定（分类/开孔率纹理）：上传 → 加入下拉框并选中 → 启用该纹理 → 失效缓存 → 回调。
function bindTextureImport(root, inputSelector, selectName, enabledName, afterImport) {
  const input = root?.querySelector(inputSelector)
  const select = root?.querySelector(`[name="${selectName}"]`)
  if (!input || !select) return
  input.addEventListener('change', async () => {
    const file = input.files?.[0]
    if (!file) return
    try {
      input.disabled = true
      const imported = await api.importTexture({ name: file.name, contentBase64: await readFileBase64(file) })
      if (![...select.options].some((option) => option.value === imported.relativePath)) {
        select.insertAdjacentHTML('beforeend', `<option value="${escapeHtml(imported.relativePath)}">${escapeHtml(imported.relativePath)}</option>`)
      }
      select.value = imported.relativePath
      invalidateTextureCaches(imported.relativePath)
      const enabled = root.querySelector(`[name="${enabledName}"]`)
      if (enabled) enabled.value = 'true'
      toast('纹理导入成功', imported.relativePath, 'success')
      afterImport?.()
    } catch (error) {
      input.value = ''
      toast('纹理导入失败', error.message, 'error')
    } finally { input.disabled = false }
  })
}

// 作用：绑定光谱对话框中的分类纹理与开孔率纹理控件，并首次渲染分类绑定行。
function bindSurfaceTextureFields(rootSelector, currentSpectrumName) {
  const root = $(rootSelector)
  if (!root) return
  const refresh = () => renderClassTextureRows(root, currentSpectrumName)
  root.querySelector('[name="classTextureFileName"]')?.addEventListener('change', refresh)
  bindTextureImport(root, '.class-texture-file', 'classTextureFileName', 'classTextureEnabled', refresh)
  bindTextureImport(root, '.openness-texture-file', 'opennessTextureFileName', 'opennessTextureEnabled')
  refresh()
}

// 作用：由表单数据组装 classTexture 配置（重复尺寸下限 0.01 m，缺省 1.5 m；classColor 与 classSpectral 按序配对）。
function classTextureFromForm(formData) {
  const colors = formData.getAll('classColor').map(String)
  const spectra = formData.getAll('classSpectral').map(String)
  return {
    enabled: formData.get('classTextureEnabled') === 'true',
    fileName: String(formData.get('classTextureFileName') || ''),
    repeatSize: Math.max(0.01, Number(formData.get('classTextureRepeatSize')) || 1.5),
    classes: colors.map((color, index) => ({ color, spectralName: spectra[index] || '' }))
  }
}

// 作用：由表单值组装 opennessTexture 配置：开孔率限幅 0–0.95，最大值不小于最小值，档位数 2–32（缺省 8）。
function opennessTextureFromValues(values) {
  const minimum = Math.max(0, Math.min(0.95, Number(values.opennessTextureMin) || 0))
  return {
    enabled: values.opennessTextureEnabled === 'true',
    fileName: String(values.opennessTextureFileName || ''),
    repeatSize: Math.max(0.01, Number(values.opennessTextureRepeatSize) || 1),
    minOpenness: minimum,
    maxOpenness: Math.max(minimum, Math.min(0.95, Number(values.opennessTextureMax) || 0)),
    levels: Math.max(2, Math.min(32, Math.round(Number(values.opennessTextureLevels) || 8)))
  }
}

// 作用：Three.js 预览纹理表单（仅影响界面预览，不影响求解结果）；缺省重复尺寸 2 m。
function previewTextureFields(texture = {}) {
  const enabled = Boolean(texture.enabled)
  const preset = String(texture.preset || PREVIEW_TEXTURE_PRESETS[0][0])
  const options = PREVIEW_TEXTURE_PRESETS.map(([value, label]) =>
    `<option value="${escapeHtml(value)}" ${value === preset ? 'selected' : ''}>${escapeHtml(label)}</option>`
  ).join('')
  return `<label class="dialog-field"><span>Three.js 预览纹理</span><select class="select" name="previewTextureEnabled"><option value="false" ${enabled ? '' : 'selected'}>关闭</option><option value="true" ${enabled ? 'selected' : ''}>启用</option></select></label><label class="dialog-field"><span>常见外观</span><select class="select" name="previewTexturePreset">${options}</select></label>${physicalInput('纹理重复尺寸（m）', 'previewTextureRepeatSize', texture.repeatSize ?? 2, 0.05)}<div class="notice" style="grid-column:1 / -1">只用于 Three.js 场景预览，不参与 HiStream 求解，也不改变反射率、发射率或模拟结果。</div>`
}

// 作用：由表单值组装 previewTexture 配置（重复尺寸下限 0.05 m）。
function previewTextureFromValues(values) {
  return {
    enabled: values.previewTextureEnabled === 'true',
    preset: String(values.previewTexturePreset || PREVIEW_TEXTURE_PRESETS[0][0]),
    repeatSize: Math.max(0.05, Number(values.previewTextureRepeatSize) || 2)
  }
}

// 作用：由表单值组装 physicalTexture 配置：强度限幅 0–1，相对标准差 CV 限幅 0–0.5，重复尺寸下限 0.01 m。
function physicalTextureFromValues(values) {
  return {
    enabled: values.physicalTextureEnabled === 'true',
    fileName: String(values.physicalTextureFileName || ''),
    strength: Math.max(0, Math.min(1, Number(values.physicalTextureStrength) || 0)),
    relativeStd: Math.max(0, Math.min(0.5, Number(values.physicalTextureRelativeStd) || 0)),
    repeatSize: Math.max(0.01, Number(values.physicalTextureRepeatSize) || 2)
  }
}

// 作用：绑定自定义光谱文件的导入：读取 TXT 文本交后端保存，把返回的相对路径写入隐藏字段 fileName。
function bindSpectrumFileInput(prefix, rootSelector) {
  const root = $(rootSelector)
  const input = root?.querySelector(`#${prefix}SpectrumFile`)
  const hidden = root?.querySelector('[name="fileName"]')
  const status = root?.querySelector(`#${prefix}SpectrumFileStatus`)
  if (!input || !hidden || !status) return
  input.addEventListener('change', async () => {
    const file = input.files?.[0]
    if (!file) return
    try {
      input.disabled = true
      const imported = await api.importSpectrum({ name: file.name, content: await file.text() })
      hidden.value = imported.relativePath
      status.textContent = `已导入：${imported.relativePath}`
      toast('波谱导入成功', imported.relativePath, 'success')
    } catch (error) {
      input.value = ''
      toast('波谱导入失败', error.message, 'error')
    } finally { input.disabled = false }
  })
}

// 作用：按光谱模型 #spectralModel 生成相应的参数输入区（#modelParameterFields）。
// 模型：Prospect（叶片光学模型：Cab、Cw、Cdm、Cs、N）、BSM（土壤光谱模型：SMC、亮度、纬度/经度参数、
// 湿端 TIR 反射率，留空自动）、file（导入 TXT 波谱）、其余为按传感器波段手填的自定义模型。
// 说明：各参数的缺省值写在输入框 value 中（如 Cab 40、N 1.5、SMC 25）；物理含义与单位待核对。
function renderModelParameters(fileName = '') {
  const model = $('#spectralModel').value
  if (model === 'Prospect') {
    $('#modelParameterFields').innerHTML = `${field('Cab', '<input class="input" name="paramCab" type="number" step="0.1" value="40">')}${field('Cw', '<input class="input" name="paramCw" type="number" step="0.001" value="0.01">')}${field('Cdm', '<input class="input" name="paramCdm" type="number" step="0.001" value="0.01">')}${field('Cs', '<input class="input" name="paramCs" type="number" step="0.001" value="0">')}${field('N', '<input class="input" name="paramN" type="number" step="0.1" value="1.5">')}`
  } else if (model === 'BSM') {
    $('#modelParameterFields').innerHTML = `${field('SMC', '<input class="input" name="paramSMC" type="number" step="0.1" value="25">')}${field('亮度', '<input class="input" name="paramBSMBrightness" type="number" step="0.1" value="0.5">')}${field('纬度参数', '<input class="input" name="paramBSMlat" type="number" step="0.1" value="25">')}${field('经度参数', '<input class="input" name="paramBSMlon" type="number" step="0.1" value="45">')}${field('湿端 TIR 反射率', '<input class="input" name="refTirWet" type="number" step="0.005" min="0" max="1" placeholder="留空自动">')}`
  } else if (model === 'file') {
    $('#modelParameterFields').innerHTML = spectrumFileImportFields('material', fileName)
    bindSpectrumFileInput('material', '#materialForm')
  } else {
    const count = configuredSpectralBands().length
    $('#modelParameterFields').innerHTML = `<div class="notice">自定义模型按传感器波段填写反射率和透射率；当前为 ${count} 个波段。</div>`
  }
}

// 作用：收集材质表单中名为 `${name}Value` 的所有数值输入框（反射率/透射率，多波段时有多个），
// 去掉空项后用逗号拼成字符串，供保存材质时写入工程。
function materialSpectrumValues(name) {
  return $$(`#materialForm [name="${name}Value"]`).map((input) => input.value.trim()).filter(Boolean).join(',')
}

// 作用：重建材质对话框里的反射率/透射率输入框。
// 说明：光谱模型为 custom 时按传感器波段数展开成多个输入框（每个波段一个值），
// 其余模型只有单个值；入参为逗号分隔的字符串，缺省值 0.20 / 0.0。
function renderMaterialSpectrumValueFields(reflectance = '0.20', transmittance = '0.0') {
  const multiband = $('#spectralModel').value === 'custom'
  $('#materialReflectanceFields').innerHTML = presetSpectrumFields('反射率', 'reflectance', reflectance, '0.20', multiband)
  $('#materialTransmittanceFields').innerHTML = presetSpectrumFields('透射率', 'transmittance', transmittance, '0.0', multiband)
}

// 作用：按“光谱预设”下拉框的选择刷新材质对话框的光谱部分。
// 选中已有预设：名称只读，回填模型、TIR 反射/透射、波段值、纹理与镜面参数以及 Prospect/BSM 参数；
// 选中“新建”：名称可编辑，各字段恢复缺省。
function applySpectrumPreset() {
  const selected = state.config.spectra.find((item) => item.name === document.querySelector('#spectralPreset').value)
  const nameInput = document.querySelector('#spectralMaterialName')
  if (selected) {
    nameInput.value = selected.name
    nameInput.readOnly = true
    document.querySelector('#spectralModel').value = selected.model || 'custom'
    setFormField('refTir', selected.refTir ?? 0.05)
    setFormField('tauTir', selected.tauTir ?? 0)
    renderModelParameters(selected.fileName || '')
    // The BSM branch of renderModelParameters rebuilds #modelParameterFields, so the wet TIR value has to be
    // applied after it, next to the model parameters.
    setFormField('refTirWet', selected.refTirWet ?? '')
    renderMaterialSpectrumValueFields(selected.reflectance || '0.20', selected.transmittance || '0.0')
    $('#materialPhysicalTextureFields').innerHTML = opennessField(selected.openness) + previewTextureFields(selected.previewTexture) + physicalTextureFields(selected.physicalTexture) + specularParameterFields(selected.specular, false)
    bindPhysicalTextureImport('#materialForm')
    const params = selected.params || {}
    for (const [key, value] of Object.entries({ Cab: params.Cab, Cw: params.Cw, Cdm: params.Cdm, Cs: params.Cs, N: params.N, SMC: params.SMC, BSMBrightness: params.BSMBrightness, BSMlat: params.BSMlat, BSMlon: params.BSMlon })) setFormField('param' + key, value)
  } else {
    nameInput.readOnly = false
    renderModelParameters()
    renderMaterialSpectrumValueFields()
    $('#materialPhysicalTextureFields').innerHTML = opennessField() + previewTextureFields() + physicalTextureFields() + specularParameterFields()
    bindPhysicalTextureImport('#materialForm')
  }
}

// 作用：按当前物体类型重建光谱预设下拉框，并默认选中该类型的推荐预设
// （保持用户已选的预设，若其仍存在）；最后调用 applySpectrumPreset 回填字段。
function renderSpectrumPresetOptions() {
  const type = document.querySelector("#materialObjectType").value
  const preferred = { SolarPanel: "pv_opaque", Vegetation: "green_leaf", Fire: "fire_medium", Fog: "fog_medium", Soil: "soil", Building: "concrete", Human: "human_surface", Vehicle: "vehicle_surface", Ship: "ship_surface", Other: "concrete", Water: "water_surface" }[type]
  const current = document.querySelector("#spectralPreset").value
  const selected = state.config.spectra.some((item) => item.name === current) ? current : preferred
  document.querySelector("#spectralPreset").innerHTML = optionList(state.config.spectra, document.querySelector("#spectralMaterialName").value, selected)
  applySpectrumPreset()
}

// 作用：按“热学预设”下拉框刷新对话框：选中已有预设时回填向阳/背阴温度并锁定名称，
// 选“新建”时名称可编辑。
function applyThermalPreset() {
  const selected = state.config.thermals.find((item) => item.name === document.querySelector("#thermalPreset").value)
  const nameInput = document.querySelector("#thermalMaterialName")
  if (selected) {
    nameInput.value = selected.name
    nameInput.readOnly = true
    setFormField("sunlitTemperature", selected.sunlitTemperature)
    setFormField("shadedTemperature", selected.shadedTemperature)
  } else nameInput.readOnly = false
}

// 作用：按物体类型重建热学预设下拉框并默认选中推荐预设（保持已选，若仍存在），
// 随后应用该预设。
function renderThermalPresetOptions() {
  const type = document.querySelector("#materialObjectType").value
  const preferred = { SolarPanel: "pv_temperature", Vegetation: "vegetation_temperature", Fire: "fire_temperature", Fog: "fog_temperature", Soil: "soil_temperature", Building: "building_temperature", Human: "human_temperature", Vehicle: "vehicle_temperature", Ship: "ship_temperature", Other: "building_temperature", Water: "water_temperature" }[type]
  const current = document.querySelector("#thermalPreset").value
  const selected = state.config.thermals.some((item) => item.name === current) ? current : preferred
  document.querySelector("#thermalPreset").innerHTML = optionList(state.config.thermals, document.querySelector("#thermalMaterialName").value, selected)
  applyThermalPreset()
}

// 作用：渲染“物理参数”区域。物体类型先映射为物理材料类型（physicalTypeForObject）。
// 选中已有物理预设：只显示该预设参数的只读摘要；
// 选“新建”：按类型（Photovoltaic/Vegetation/Water/其余按土壤等固体）生成输入框，
// 缺省值取 createMaterialPresets() 中同类型的内置预设参数。
function renderPhysicalParameters() {
  const type = physicalTypeForObject($('#materialObjectType').value)
  const selected = state.config.materials.find((item) => item.name === $('#physicalMaterialPreset').value)
  const nameInput = $('#physicalMaterialName')
  if (selected) {
    nameInput.value = selected.name
    nameInput.readOnly = true
    $('#physicalParameterFields').innerHTML = `<div class="notice">使用“${escapeHtml(selected.label || selected.name)}”预设：${physicalValues(selected).map(([key, value]) => `${escapeHtml(key)}=${escapeHtml(value)}`).join('，')}</div>`
    return
  }
  nameInput.readOnly = false
  if (nameInput.dataset.newName) nameInput.value = nameInput.dataset.newName
  const defaults = createMaterialPresets().materials.find((item) => type === 'Photovoltaic' ? item.energyModel === 'photovoltaic' : item.type === type && !item.energyModel)?.params || {}
  if (type === 'Photovoltaic') {
    $('#physicalParameterFields').innerHTML = [['参考效率（25℃）','eta25',0.001],['温度系数（K⁻¹）','gamma',0.0001],['背面发电系数','bifaciality',0.01],['面热容（J·m⁻²·K⁻¹）','heatCapacityPerArea',100],['对流换热倍率','convectiveScale',0.1]].map(([label, key, step]) => physicalInput(label, 'pv_' + key, defaults[key], step)).join('')
  } else if (type === 'Vegetation') {
    $('#physicalParameterFields').innerHTML = [
      physicalInput('Vcmax', 'bioVcmax', defaults.Vcmax, 0.1), physicalInput('气孔斜率 m', 'bioM', defaults.m, 0.1),
      physicalInput('Ball-Berry 截距', 'bioBallBerry', defaults.BallBerry, 0.001), physicalInput('光合类型', 'bioType', defaults.Type, 1),
      physicalInput('kV', 'bioKV', defaults.kV, 0.0001), physicalInput('暗呼吸系数', 'bioRdparam', defaults.Rdparam, 0.001),
      `<label class="dialog-field"><span>温度响应参数</span><input class="input" name="bioTparam" value="${escapeHtml(defaults.Tparam)}"></label>`,
      physicalInput('年均温', 'bioTyear', defaults.Tyear, 0.1), physicalInput('beta', 'bioBeta', defaults.beta, 0.001),
      physicalInput('NPQ 系数', 'bioKNPQs', defaults.kNPQs, 0.01), physicalInput('光限制', 'bioQLs', defaults.qLs, 0.01),
      physicalInput('胁迫因子', 'bioStressfactor', defaults.stressfactor, 0.01), physicalInput('温度校正', 'bioTcor', defaults.Tcor, 1)
    ].join('')
  } else if (type === 'Water') {
    $('#physicalParameterFields').innerHTML = [
      physicalInput('表面阻力 rss', 'waterRss', defaults.rss, 1), physicalInput('体积热容', 'waterHeatCapacity', defaults.heatCapacity, 1),
      physicalInput('混合深度', 'waterMixingDepth', defaults.mixingDepth, 0.1), physicalInput('蒸发系数', 'waterEvaporationCoefficient', defaults.evaporationCoefficient, 0.1),
      physicalInput('辐射模型（0 Lambert / 1 Cox–Munk）', 'waterBrdfModel', defaults.brdfModel, 1), physicalInput('水体折射率', 'waterRefractiveIndex', defaults.refractiveIndex, 0.001),
      physicalInput('水面坡度方差（0 随风速）', 'waterSlopeVariance', defaults.slopeVariance, 0.001), physicalInput('漫反射比例', 'waterDiffuseFraction', defaults.diffuseFraction, 0.01),
      waterInitialFields(defaults)
    ].join('')
  } else {
    $('#physicalParameterFields').innerHTML = [
      physicalInput('方法', 'soilMethod', defaults.method, 1), physicalInput('表面阻力 rss', 'soilRss', defaults.rss, 1),
      physicalInput('比热容 cs', 'soilCs', defaults.cs, 1), physicalInput('密度 rhos', 'soilRhos', defaults.rhos, 1),
      physicalInput('导热率 λ', 'soilLambdas', defaults.lambdas, 0.01), physicalInput('初始温度', 'soilTsoil', defaults.Tsoil, 0.1),
      physicalInput('土壤含水量 SMC', 'soilSMC', defaults.SMC, 0.1), physicalInput('饱和含水量', 'soilSatwater', defaults.Satwater, 0.01),
      wallModeFields(defaults), type === 'Ship' ? shipFields(defaults) : '', type === 'Vehicle' ? vehicleFields(defaults) : ''
    ].join('')
  }
}

// 作用：按物体类型过滤可用的物理材料预设，重建下拉框并默认选中推荐预设
// （如 SolarPanel→pv_panel、Vegetation→leaf_c3、Water→water_set；无推荐项时选第一个，
// 没有可选项则为“新建”），再渲染物理参数。
function renderPhysicalPresetOptions() {
  const objectType = $('#materialObjectType').value
  const type = physicalTypeForObject(objectType)
  const materials = state.config.materials.filter((item) => materialMatchesObjectType(item, objectType))
  const preferred = { SolarPanel: 'pv_panel', Vegetation: 'leaf_c3', Fire: 'leaf_c3', Fog: 'leaf_c3', Soil: 'soilset', Building: 'building_surface', Human: 'soil_dry', Vehicle: 'vehicle_material', Ship: 'ship_material', Other: 'soilset', Water: 'water_set' }[objectType]
  $('#physicalMaterialPreset').innerHTML = optionList(materials, $('#physicalMaterialName').value, materials.some((item) => item.name === preferred) ? preferred : materials[0]?.name || '__new__')
  renderPhysicalParameters()
}

// 作用：植被类 OBJ 按网格组名推荐材质绑定：叶片（leaf/needle/叶片…）→ 绿叶光谱与叶冠；
// 树枝（branch/twig/枝条…）→ 木质光谱与刚性枝；树干（trunk/bark/树干…）→ 木质与刚性干；
// 都不匹配则原样返回 fallback。名称匹配不区分大小写，规则为启发式。
function recommendedVegetationMeshBinding(meshName, fallback) {
  const name = String(meshName || '').toLowerCase()
  if (/(leaf|leaves|foliage|needle|叶片|树叶|针叶)/.test(name)) {
    return { ...fallback, spectralName: 'green_leaf', thermalName: 'vegetation_temperature', materialName: 'leaf_c3', canopyName: 'tree_leaf_canopy' }
  }
  if (/(branch|branches|twig|limb|树枝|枝条)/.test(name)) {
    return { ...fallback, spectralName: 'tree_wood', thermalName: 'tree_wood_temperature', materialName: 'tree_branch_wood', canopyName: 'tree_branch_rigid' }
  }
  if (/(trunk|stem|bark|wood|树干|树皮|木质)/.test(name)) {
    return { ...fallback, spectralName: 'tree_wood', thermalName: 'tree_wood_temperature', materialName: 'tree_trunk_wood', canopyName: 'tree_trunk_rigid' }
  }
  return fallback
}

// 作用：为待导入 OBJ 的每个网格组生成一行材质绑定（光谱/热学/物理/冠层/镜面）。
// 步骤：1) 取当前物体类型的推荐预设与对话框当前选择；2) 保存已有的镜面设置避免重绘丢失；
// 3) FacetEB 车辆开启自动绑定时，按组名使用车辆规则并展开可拆分的组（车窗、引擎盖）；
// 4) 植被且非光伏时可按组名推荐叶/枝/干绑定；5) 输出每行的 select 与镜面字段。
// 光伏材质与车辆一律使用刚性冠层 rigid_body。
function renderMeshMaterialRows({ recommendVegetation = false } = {}) {
  const pending = state.pendingObject
  if (!pending) return
  const type = $('#materialObjectType').value
  const preferredSpectrum = { SolarPanel: 'pv_opaque', Vegetation: 'green_leaf', Fire: 'fire_medium', Fog: 'fog_medium', Soil: 'soil', Building: 'concrete', Human: 'human_surface', Vehicle: 'vehicle_surface', Ship: 'ship_surface', Other: 'concrete', Water: 'water_surface' }[type]
  const preferredThermal = { SolarPanel: 'pv_temperature', Vegetation: 'vegetation_temperature', Fire: 'fire_temperature', Fog: 'fog_temperature', Soil: 'soil_temperature', Building: 'building_temperature', Human: 'human_temperature', Vehicle: 'vehicle_temperature', Ship: 'ship_temperature', Other: 'building_temperature', Water: 'water_temperature' }[type]
  const selectedSpectrum = document.querySelector("#spectralPreset").value || preferredSpectrum
  const selectedThermal = document.querySelector("#thermalPreset").value || preferredThermal
  const physicalItems = state.config.materials.filter((item) => materialMatchesObjectType(item, type))
  const preferredPhysical = { SolarPanel: 'pv_panel', Vegetation: 'leaf_c3', Fire: 'leaf_c3', Fog: 'leaf_c3', Soil: 'soilset', Building: 'building_surface', Human: 'soil_dry', Vehicle: 'vehicle_material', Ship: 'ship_material', Other: 'soilset', Water: 'water_set' }[type]
  const selectedPhysical = $('#physicalMaterialPreset').value || preferredPhysical
  const photovoltaic = physicalItems.find((item) => item.name === selectedPhysical)?.energyModel === 'photovoltaic'
  const preferredCanopy = photovoltaic ? 'rigid_body' : type === 'Fire' ? 'fire_medium' : type === 'Fog' ? 'fog_medium' : type === 'Vegetation' ? 'tree_leaf_canopy' : 'rigid_body'
  const savedSpecular = new Map($$('#meshMaterialRows tr').map(row => [row.dataset.meshName, meshSpecularFromRow(row)]))
  // FacetEB vehicles: preset families by group name (project-schema.js VEHICLE_BINDING_RULES); the listed names
  // missing from an older project are offered too and added on save.
  renderVehicleParams()
  const vehicleAuto = vehicleAutoBindEnabled()
  const vehicleOptions = vehicleAuto ? vehicleBindingOptions() : null
  const spectrumItems = vehicleAuto ? [...(state.config.spectra || []), ...VEHICLE_SPECTRA.filter((item) => !(state.config.spectra || []).some((entry) => entry.name === item.name))] : (state.config.spectra || [])
  const vehicleMaterials = vehicleAuto ? createMaterialPresets().materials.filter((item) => item.type === 'Vehicle' && !physicalItems.some((entry) => entry.name === item.name)) : []
  // With auto-binding the splittable groups (glass, bonnet) are listed with their sub-group names.
  let meshNames = pending.meshNames
  if (vehicleAuto && pending.source?.content) {
    const split = splitVehicleObj(pending.source.content).split
    meshNames = meshNames.flatMap((name) => split[name] || [name])
  }
  $('#meshMaterialRows').innerHTML = meshNames.map((name, index) => {
    const defaults = {
      spectralName: selectedSpectrum,
      thermalName: selectedThermal,
      materialName: selectedPhysical,
      canopyName: preferredCanopy
    }
    let binding = recommendVegetation && type === 'Vegetation' && !photovoltaic
      ? recommendedVegetationMeshBinding(name, defaults) : defaults
    if (vehicleAuto) {
      const bound = vehicleMeshBinding(name, { ...vehicleOptions, bodyMaterial: selectedPhysical })
      binding = { ...binding, spectralName: bound.spectralName || binding.spectralName, materialName: bound.materialName, canopyName: 'rigid_body' }
    }
    const spectrumOptions = optionList(spectrumItems, $('#spectralMaterialName').value, binding.spectralName)
    const thermalOptions = optionList(state.config.thermals || [], $('#thermalMaterialName').value, binding.thermalName)
    const physicalOptions = optionList([...physicalItems, ...vehicleMaterials], $('#physicalMaterialName').value, binding.materialName)
    const canopyOptions = bindingOptionList(state.config.canopies || [], binding.canopyName, binding.canopyName)
    return `<tr data-mesh-name="${escapeHtml(name)}"><td title="${escapeHtml(name)}">${escapeHtml(name)}</td><td><select class="select mesh-spectrum" data-index="${index}">${spectrumOptions}</select></td><td><select class="select mesh-thermal" data-index="${index}">${thermalOptions}</select></td><td><select class="select mesh-physical" data-index="${index}">${physicalOptions}</select></td><td><select class="select mesh-canopy" data-index="${index}">${canopyOptions}</select></td><td>${meshSpecularFields({ enabled: savedSpecular.get(name) })}</td></tr>`
  }).join('')
}

// 作用：打开“新对象材质”对话框，按待导入 OBJ 的路径生成唯一的对象/材质/光谱/热学名称，
// 按建议类型选择光谱模型（植被 Prospect、土壤 BSM，其余 custom），并渲染各预设与网格行。
function showMaterialDialog() {
  const pending = state.pendingObject
  if (!pending) return
  $('#materialForm').reset()
  const base = uniqueName(pending.source.path, 'object')
  const suggestedType = pending.suggestedType || 'Vegetation'
  $('#materialObjectName').value = base
  $('#materialObjectType').value = suggestedType
  $('#physicalMaterialName').value = `${base}_material`
  $('#spectralMaterialName').value = `${base}_spectral`
  $('#thermalMaterialName').value = `${base}_temperature`
  $('#physicalMaterialName').dataset.newName = `${base}_material`
  $('#spectralPreset').value = '__new__'
  $('#thermalPreset').value = '__new__'
  $('#physicalMaterialPreset').value = '__new__'
  $('#spectralModel').value = suggestedType === 'Vegetation' ? 'Prospect' : suggestedType === 'Soil' ? 'BSM' : 'custom'
  renderModelParameters(); renderSpectrumPresetOptions(); renderThermalPresetOptions(); renderPhysicalPresetOptions()
  renderMeshMaterialRows({ recommendVegetation: true })
  $('#materialDialog').hidden = false
}

// 作用：关闭材质对话框；discard 为真时释放待导入对象的三维资源并清空 state.pendingObject。
function hideMaterialDialog(discard = true) {
  $('#materialDialog').hidden = true
  if (discard && state.pendingObject) disposeObject(state.pendingObject.object)
  if (discard) state.pendingObject = null
}

// 作用：读取“几何体”对话框表单，规整为数值对象；非法枚举回落到缺省
// （形状 ellipsoid、模式 surface、基准 lai、取向 spherical），细分度 detail 限幅为 1–32。
// 校验：尺寸必须 > 0；chaos 的填充强度与面元面积 > 0；火/雾介质参数须为有限数且
// 消光 ≥ 0、单次散射反照率在 [0,1]、|不对称因子| < 1、温度与发射缩放 ≥ 0；水体位置/偏移须有限。
// 校验失败抛出 Error，由调用方 toast 提示。
function geometryValues() {
  const values = Object.fromEntries(new FormData($('#geometryForm')).entries())
  const result = {
    name: uniqueName(values.geometryName, 'geometry'),
    shape: values.geometryShape === 'cube' ? 'cube' : 'ellipsoid',
    mode: ['surface', 'chaos', 'water', 'fire', 'fog'].includes(values.geometryMode) ? values.geometryMode : 'surface',
    sizeX: Number(values.geometrySizeX), sizeY: Number(values.geometrySizeY), sizeZ: Number(values.geometrySizeZ),
    detail: Math.max(1, Math.min(32, Math.round(Number(values.geometryDetail) || 2))),
    basis: ['lai', 'lad', 'count'].includes(values.chaosBasis) ? values.chaosBasis : 'lai',
    amount: Number(values.chaosAmount), facetArea: Number(values.chaosFacetArea),
    orientation: ['spherical', 'horizontal', 'vertical'].includes(values.chaosOrientation) ? values.chaosOrientation : 'spherical',
    seed: Math.round(Number(values.chaosSeed) || 0),
    positionX: Number(values.mediumPositionX), positionY: Number(values.mediumPositionY), positionZ: Number(values.mediumPositionZ),
    extinction: Number(values.mediumExtinction), scatteringAlbedo: Number(values.mediumScatteringAlbedo),
    asymmetry: Number(values.mediumAsymmetry), temperature: Number(values.mediumTemperature), emissionScale: Number(values.mediumEmissionScale),
    waterPositionX: Number(values.waterPositionX), waterPositionY: Number(values.waterPositionY), waterPositionZ: Number(values.waterPositionZ),
    waterOffsetX: Number(values.waterOffsetX), waterOffsetY: Number(values.waterOffsetY), waterOffsetZ: Number(values.waterOffsetZ)
  }
  if (![result.sizeX, result.sizeY, result.sizeZ].every((value) => Number.isFinite(value) && value > 0)) throw new Error('几何体三个方向的尺寸必须大于 0')
  if (result.mode === 'chaos' && (!Number.isFinite(result.amount) || result.amount <= 0 || !Number.isFinite(result.facetArea) || result.facetArea <= 0)) throw new Error('填充强度和单个面元面积必须大于 0')
  if ((result.mode === 'fire' || result.mode === 'fog') && ![result.positionX, result.positionY, result.positionZ, result.extinction, result.scatteringAlbedo, result.asymmetry, result.temperature, result.emissionScale].every(Number.isFinite)) throw new Error('参与介质参数必须为有效数值')
  if ((result.mode === 'fire' || result.mode === 'fog') && (result.extinction < 0 || result.scatteringAlbedo < 0 || result.scatteringAlbedo > 1 || Math.abs(result.asymmetry) >= 1 || result.temperature < 0 || result.emissionScale < 0)) throw new Error('参与介质参数超出有效范围')
  if (result.mode === 'water' && ![result.waterPositionX, result.waterPositionY, result.waterPositionZ, result.waterOffsetX, result.waterOffsetY, result.waterOffsetZ].every(Number.isFinite)) throw new Error('水体位置与偏移必须为有效数值')
  return result
}

// 作用：估算 chaos（随机面元填充）模式生成的三角形数。
// basis=count：直接取 amount；lai：amount × 水平投影面积 / 单面元面积；
// lad：amount × 体积 / 单面元面积（椭球取 π/4·X·Z 与 π/6·X·Y·Z，立方体取 X·Z 与 X·Y·Z）。
// 结果向上取整。
function chaosTriangleCount(values) {
  if (values.basis === 'count') return Math.round(values.amount)
  const footprint = values.shape === 'cube'
    ? values.sizeX * values.sizeZ
    : Math.PI * values.sizeX * values.sizeZ / 4
  const volume = values.shape === 'cube'
    ? values.sizeX * values.sizeY * values.sizeZ
    : Math.PI * values.sizeX * values.sizeY * values.sizeZ / 6
  return Math.ceil((values.basis === 'lad' ? values.amount * volume : values.amount * footprint) / values.facetArea)
}

// 作用：估算 surface/water 模式的三角形数（用于预览与上限提示）。
// 公式随形状与 detail 变化：立方体 12·detail²（水面 2·detail²），
// 椭球 16·detail·(4·detail−1)（水面 max(16, 16·detail)）；系数的网格划分依据（待核对）。
function surfaceTriangleCount(values) {
  if (values.mode === 'water') return values.shape === 'cube'
    ? 2 * values.detail * values.detail
    : Math.max(16, values.detail * 16)
  return values.shape === 'cube' ? 12 * values.detail * values.detail : 16 * values.detail * (4 * values.detail - 1)
}

// 作用：根据所选“几何模式”刷新几何体对话框：显示/隐藏各字段分组、切换标签与提示文字，
// 并实时给出生成结果的估算说明（三角面元数、水面最终中心、介质体积位置）。
// 说明：模式为 surface/chaos/water/fire/fog；水面不使用“Y 高度”；火/雾时 Y/Z 标签改为
// “Y 尺寸/Z 高度”。表单里的 X/Y/Z 沿用工程旧键序（x=南北向、y=东西向、z=高度），
// 与三维预览中 Y 向上的坐标不同，生成 OBJ 时再互换（见 surfaceGeometryObj）。
function updateGeometryDialog() {
  const form = $('#geometryForm')
  if (!form) return
  const chaos = form.elements.geometryMode.value === 'chaos'
  const medium = form.elements.geometryMode.value === 'fire' || form.elements.geometryMode.value === 'fog'
  const fog = form.elements.geometryMode.value === 'fog'
  const water = form.elements.geometryMode.value === 'water'
  $('#geometryShapeField').hidden = false
  $('#geometrySizeYField').hidden = water
  $('#geometryShapeLabel').textContent = water ? '水面形状' : '包络形状'
  form.elements.geometryShape.options[0].textContent = water ? '圆形 / 椭圆' : '椭球'
  form.elements.geometryShape.options[1].textContent = water ? '矩形' : '立方体'
  $('#geometrySizeYLabel').textContent = medium ? 'Y 尺寸（m）' : 'Y 高度（m）'
  $('#geometrySizeZLabel').textContent = medium ? 'Z 高度（m）' : water ? 'Y 尺寸（m）' : 'Z 尺寸（m）'
  $('#geometrySurfaceFields').hidden = chaos || medium
  $('#geometryChaosFields').hidden = !chaos
  $('#geometryWaterFields').hidden = !water
  $('#geometryMediumFields').hidden = !medium
  $('#geometryMediumNotice').textContent = fog
    ? '坐标统一为 X 南北、Y 向上、Z 东西；雾会按消光系数衰减，并按反照率和非对称因子散射辐射。'
    : '坐标统一为 X 南北、Y 向上、Z 东西；火焰会衰减、散射并按固定温度发射。'
  $('#geometrySurfaceNotice').textContent = water
    ? '生成朝上的水平三角网格水面；水面高程可在对象“分布”中通过 Z 设置。'
    : '生成封闭外壳，适用于建筑等连续表面。立方体按六个面细分，椭球生成连续三角网格。'
  // 步骤：按填充基准切换“用量”输入框的标签，再用 geometryValues 校验并估算三角形数；
  // 校验失败时把错误信息直接显示在估算栏。
  const labels = { lai: 'LAI（m²/m²）', lad: 'LAD（m²/m³）', count: '面元数量' }
  $('#geometryAmountLabel').textContent = labels[form.elements.chaosBasis.value] || labels.lai
  try {
    const values = geometryValues()
    const triangles = values.mode === 'chaos'
      ? chaosTriangleCount(values)
      : surfaceTriangleCount(values)
    $('#geometryEstimate').textContent = medium
      ? `将生成 ${(values.shape === 'ellipsoid' ? '椭球' : '立方体')}${fog ? '雾' : '火焰'}体积；中心平面位置 X/Z=(${values.positionX}, ${values.positionY}) m，底部 Y=${values.positionZ} m。`
      : water
        ? `将生成 ${values.shape === 'ellipsoid' ? '圆形/椭圆' : '矩形'}水面 ${values.sizeX} × ${values.sizeZ} m；最终中心 X/Y/Z=(${values.waterPositionX + values.waterOffsetX}, ${values.waterPositionY + values.waterOffsetY}, ${values.waterPositionZ + values.waterOffsetZ}) m；共 ${Math.max(1, triangles).toLocaleString('zh-CN')} 个三角面元。`
      : `预计生成 ${Math.max(1, triangles).toLocaleString('zh-CN')} 个三角面元；Y 方向为高度，底面位于 Y=0。`
  } catch (error) {
    $('#geometryEstimate').textContent = error.message
  }
}

// 作用：打开“生成几何体”对话框（需先有工程），以 geometry_N 作为缺省对象名并刷新界面。
function showGeometryDialog() {
  if (!state.inputPath) { toast('无法生成几何体', '请先新建或打开工程', 'error'); return }
  const form = $('#geometryForm')
  form.reset()
  form.elements.geometryName.value = `geometry_${(state.config?.objects?.items?.length || 0) + 1}`
  updateGeometryDialog()
  $('#geometryDialog').hidden = false
  setTimeout(() => form.elements.geometryName.focus(), 0)
}

// 作用：给火焰/雾的参与介质对话框填缺省值。雾：包络取整个场景（X/Y 取场景尺寸，
// 高度取 max(5, 场景高度)），消光 0.08、单次散射反照率 0.95、不对称因子 0.85、温度 288 K；
// 火焰：25×25×10 的椭球，消光 1.2、反照率 0.72、不对称因子 0.55、温度 1100 K。
// 介质中心位于场景水平中心，底部高度 0；消光系数单位按 m⁻¹ 理解（待核对）。
function setMediumDefaults(kind) {
  const form = $('#geometryForm')
  const fog = kind === 'fog'
  form.elements.geometryMode.value = fog ? 'fog' : 'fire'
  form.elements.geometryName.value = `${fog ? 'fog' : 'fire'}_${(state.config?.objects?.items?.length || 0) + 1}`
  form.elements.geometryShape.value = 'ellipsoid'
  form.elements.geometrySizeX.value = fog ? state.config.scene.x : 25
  form.elements.geometrySizeY.value = fog ? state.config.scene.y : 25
  form.elements.geometrySizeZ.value = fog ? Math.max(5, state.config.scene.height) : 10
  form.elements.mediumPositionX.value = state.config.scene.x / 2
  form.elements.mediumPositionY.value = state.config.scene.y / 2
  form.elements.mediumPositionZ.value = 0
  form.elements.mediumExtinction.value = fog ? 0.08 : 1.2
  form.elements.mediumScatteringAlbedo.value = fog ? 0.95 : 0.72
  form.elements.mediumAsymmetry.value = fog ? 0.85 : 0.55
  form.elements.mediumTemperature.value = fog ? 288 : 1100
  form.elements.mediumEmissionScale.value = 1
}

// 作用：给水体对话框填缺省值：与场景同尺寸的矩形水面，中心在场景水平中心，
// 高度 0.01 m，偏移为 0，细分度 2。
function setWaterDefaults() {
  const form = $('#geometryForm')
  form.elements.geometryMode.value = 'water'
  form.elements.geometryName.value = `water_${(state.config?.objects?.items?.length || 0) + 1}`
  form.elements.geometryShape.value = 'cube'
  form.elements.geometrySizeX.value = Math.max(1, Number(state.config?.scene?.x) || 60)
  form.elements.geometrySizeY.value = 0.01
  form.elements.geometrySizeZ.value = Math.max(1, Number(state.config?.scene?.y) || 60)
  form.elements.geometryDetail.value = 2
  form.elements.waterPositionX.value = Math.max(1, Number(state.config?.scene?.x) || 60) / 2
  form.elements.waterPositionY.value = Math.max(1, Number(state.config?.scene?.y) || 60) / 2
  form.elements.waterPositionZ.value = 0.01
  form.elements.waterOffsetX.value = 0
  form.elements.waterOffsetY.value = 0
  form.elements.waterOffsetZ.value = 0
}

// 作用：打开参与介质（火/雾）对话框；仅 VoxelRT / VoxelEB 模式支持，其他模式提示并返回。
function showMediumDialog(kind = 'fire') {
  if (!state.inputPath) { toast('无法添加参与介质', '请先新建或打开工程', 'error'); return }
  if (state.mode !== 'eVoxelRT' && state.mode !== 'eVoxelEB') { toast('参与介质不可用', '请先切换到 VoxelRT 或 VoxelEB', 'error'); return }
  const form = $('#geometryForm')
  form.reset()
  setMediumDefaults(kind)
  updateGeometryDialog()
  $('#geometryDialog').hidden = false
  setTimeout(() => form.elements.geometryName.focus(), 0)
}

function hideGeometryDialog() {
  $('#geometryDialog').hidden = true
}

// 作用：把坐标格式化为 OBJ 文本：绝对值小于 5e-9 的置 0（避免 -0 与科学计数法），
// 其余保留 7 位小数并去掉末尾多余的 0。
function objNumber(value) {
  const rounded = Math.abs(value) < 5e-9 ? 0 : value
  return Number(rounded.toFixed(7)).toString()
}

// 作用：生成立方体/椭球的封闭外壳 OBJ 文本（surface 模式，也用于火/雾介质的包络）。
// 说明：three.js 的 Y 轴向上、底面平移到 Y=0；介质模式下表单的 Z 是高度、Y 是进深，故互换。
// 立方体按 detail 在三个方向细分（12·detail² 个三角形）；椭球取单位球的
// (8·detail)×(4·detail) 分段后缩放，三角形数 16·detail·(4·detail−1)，与 surfaceTriangleCount 一致。
// 输出为非索引三角形，每三个顶点一个面。
function surfaceGeometryObj(values) {
  const medium = values.mode === 'fire' || values.mode === 'fog'
  const sizeYUp = medium ? values.sizeZ : values.sizeY
  const sizeZDepth = medium ? values.sizeY : values.sizeZ
  const geometry = values.shape === 'cube'
    ? new THREE.BoxGeometry(values.sizeX, sizeYUp, sizeZDepth, values.detail, values.detail, values.detail)
    : new THREE.SphereGeometry(1, Math.max(8, values.detail * 8), Math.max(4, values.detail * 4))
  if (values.shape === 'cube') geometry.translate(0, sizeYUp / 2, 0)
  else {
    geometry.scale(values.sizeX / 2, sizeYUp / 2, sizeZDepth / 2)
    geometry.translate(0, sizeYUp / 2, 0)
  }
  const triangles = geometry.index ? geometry.toNonIndexed() : geometry
  const positions = triangles.getAttribute('position')
  const lines = [
    '# Stream Studio generated OBJ',
    '# generation: geometry',
    `# envelope: ${values.shape} ${values.sizeX} ${values.sizeY} ${values.sizeZ}`,
    `o ${values.name}`, `g ${values.name}`, 's off'
  ]
  for (let index = 0; index < positions.count; index += 1) {
    lines.push(`v ${objNumber(positions.getX(index))} ${objNumber(positions.getY(index))} ${objNumber(positions.getZ(index))}`)
  }
  for (let index = 0; index < positions.count; index += 3) lines.push(`f ${index + 1} ${index + 2} ${index + 3}`)
  if (triangles !== geometry) triangles.dispose()
  geometry.dispose()
  return `${lines.join('\n')}\n`
}

// 作用：生成朝上（法向 +Y）的水平水面 OBJ，位于 Y=0，高程由对象分布的 z 设置。
// 矩形用 PlaneGeometry(sizeX×sizeZ, detail×detail)；圆/椭圆用 CircleGeometry，
// 分段 max(16, 16·detail)，再按 sizeX/2、sizeZ/2 缩放。
function waterSurfaceObj(values) {
  const detail = Math.max(1, values.detail)
  const geometry = values.shape === 'cube'
    ? new THREE.PlaneGeometry(values.sizeX, values.sizeZ, detail, detail)
    : new THREE.CircleGeometry(1, Math.max(16, detail * 16))
  geometry.rotateX(-Math.PI / 2)
  if (values.shape === 'ellipsoid') geometry.scale(values.sizeX / 2, 1, values.sizeZ / 2)
  const triangles = geometry.index ? geometry.toNonIndexed() : geometry
  const positions = triangles.getAttribute('position')
  const lines = [
    '# Stream Studio generated water surface OBJ',
    '# generation: water-surface',
    `# extent: ${values.sizeX} ${values.sizeZ}`,
    `o ${values.name}`, `g ${values.name}`, 's off'
  ]
  for (let index = 0; index < positions.count; index += 1) {
    lines.push(`v ${objNumber(positions.getX(index))} ${objNumber(positions.getY(index))} ${objNumber(positions.getZ(index))}`)
  }
  for (let index = 0; index < positions.count; index += 3) lines.push(`f ${index + 1} ${index + 2} ${index + 3}`)
  if (triangles !== geometry) triangles.dispose()
  geometry.dispose()
  return `${lines.join('\n')}\n`
}

function normalizeVector([x, y, z]) {
  const length = Math.hypot(x, y, z) || 1
  return [x / length, y / length, z / length]
}

function crossVector(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

// 作用：为 chaos 面元抽取随机法向（单位向量，Y 向上）。orientation：
// horizontal：以 +Y 为中心、倾角在 ±15° 内；vertical：近水平法向，仰角在 ±15° 内；
// spherical：在整个单位球面上均匀（Y 取 [-1,1] 均匀，方位角均匀）。
function chaosNormal(random, orientation) {
  const azimuth = random() * Math.PI * 2
  if (orientation === 'horizontal') {
    const tilt = (random() * 2 - 1) * Math.PI / 12
    return [Math.sin(tilt) * Math.cos(azimuth), Math.cos(tilt), Math.sin(tilt) * Math.sin(azimuth)]
  }
  if (orientation === 'vertical') {
    const elevation = (random() * 2 - 1) * Math.PI / 12
    return [Math.cos(elevation) * Math.cos(azimuth), Math.sin(elevation), Math.cos(elevation) * Math.sin(azimuth)]
  }
  const y = random() * 2 - 1
  const horizontal = Math.sqrt(Math.max(0, 1 - y * y))
  return [horizontal * Math.cos(azimuth), y, horizontal * Math.sin(azimuth)]
}

// 作用：在包络内抽取面元中心。立方体：X/Z 在 ±尺寸/2、Y 在 [0, sizeY] 内均匀；
// 椭球：取单位球内均匀点（半径取 cbrt(random)），缩放到半轴并上移 sizeY/2。
function chaosCenter(random, values) {
  if (values.shape === 'cube') return [(random() - 0.5) * values.sizeX, random() * values.sizeY, (random() - 0.5) * values.sizeZ]
  const direction = chaosNormal(random, 'spherical')
  const radius = Math.cbrt(random())
  return [direction[0] * radius * values.sizeX / 2, values.sizeY / 2 + direction[1] * radius * values.sizeY / 2, direction[2] * radius * values.sizeZ / 2]
}

// 作用：判断点是否在包络内（立方体：|x|≤sizeX/2、0≤y≤sizeY、|z|≤sizeZ/2；
// 椭球：归一化后 x²+y²+z² ≤ 1+1e-7，容许浮点误差）。
function insideEnvelope(point, values) {
  if (values.shape === 'cube') return Math.abs(point[0]) <= values.sizeX / 2 && point[1] >= 0 && point[1] <= values.sizeY && Math.abs(point[2]) <= values.sizeZ / 2
  const x = point[0] / (values.sizeX / 2)
  const y = (point[1] - values.sizeY / 2) / (values.sizeY / 2)
  const z = point[2] / (values.sizeZ / 2)
  return x * x + y * y + z * z <= 1 + 1e-7
}

// 作用：生成 chaos 模式的 OBJ：在包络内随机撒 N 个互不相连的正三角形面元（模拟叶片云）。
// 步骤：1) 用 chaosTriangleCount 得 N，非有限或 <1 报错，超过 200000 报错；
// 2) 由种子建伪随机数发生器（seed 相同则结果可复现）；
// 3) 由单面元面积 A 求正三角形外接圆半径 R=√(4A/(3√3))，三个顶点相隔 120°；
// 4) 逐个面元：抽中心与法向，构造面内正交轴，三个顶点都在包络内才接受，
// 最多重试 80 次，仍放不下则报错（提示减小面元面积或增大尺寸）；5) 每个面元写 3 个顶点和 1 个面。
function chaosGeometryObj(values) {
  const count = chaosTriangleCount(values)
  if (!Number.isFinite(count) || count < 1) throw new Error('计算得到的面元数量必须大于 0')
  if (count > 200000) throw new Error(`预计生成 ${count.toLocaleString('zh-CN')} 个面元，超过 200,000 上限；请降低 LAI/LAD 或增大单个面元面积`)
  const random = seededRandom(values.seed)
  const radius = Math.sqrt(4 * values.facetArea / (3 * Math.sqrt(3)))
  const angles = [0, Math.PI * 2 / 3, Math.PI * 4 / 3]
  const lines = [
    '# Stream Studio generated OBJ', '# generation: geometry-chaos',
    `# envelope: ${values.shape} ${values.sizeX} ${values.sizeY} ${values.sizeZ}`,
    `# facet-area: ${values.facetArea}`, `# triangle-count: ${count}`,
    `o ${values.name}`, `g ${values.name}_chaos`, 's off'
  ]
  let generated = 0
  for (let triangle = 0; triangle < count; triangle += 1) {
    let points = null
    for (let attempt = 0; attempt < 80 && !points; attempt += 1) {
      const center = chaosCenter(random, values)
      const normal = chaosNormal(random, values.orientation)
      const reference = Math.abs(normal[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]
      const axisU = normalizeVector(crossVector(reference, normal))
      const axisV = normalizeVector(crossVector(normal, axisU))
      const candidate = angles.map((angle) => [
        center[0] + radius * (Math.cos(angle) * axisU[0] + Math.sin(angle) * axisV[0]),
        center[1] + radius * (Math.cos(angle) * axisU[1] + Math.sin(angle) * axisV[1]),
        center[2] + radius * (Math.cos(angle) * axisU[2] + Math.sin(angle) * axisV[2])
      ])
      if (candidate.every((point) => insideEnvelope(point, values))) points = candidate
    }
    if (!points) throw new Error(`第 ${triangle + 1} 个面元无法放入包络；请减小单个面元面积或增大几何尺寸`)
    for (const point of points) lines.push(`v ${objNumber(point[0])} ${objNumber(point[1])} ${objNumber(point[2])}`)
    const first = generated * 3 + 1
    lines.push(`f ${first} ${first + 1} ${first + 2}`)
    generated += 1
  }
  return `${lines.join('\n')}\n`
}

// 作用：解析 OBJ 文本，登记为 state.pendingObject（待用户在材质对话框中确认的对象）。
// 说明：收集去重后的网格组名（无名时用材质名或 mesh_N）；预览用的缩放使最大边为 8 个场景单位，
// 中心移到水平原点、底部落在 y=0；metadata.dimensions 缺省记录原始包围盒尺寸（缩放前）。
function preparePendingObject(result, content, metadata = {}) {
  const object = new OBJLoader().parse(content)
  const meshNames = []
  object.traverse((child) => { if (child.isMesh) { const name = child.name || child.material?.name || `mesh_${meshNames.length + 1}`; if (!meshNames.includes(name)) meshNames.push(name) } })
  if (!meshNames.length) throw new Error('OBJ 中没有可用 mesh')
  const box = new THREE.Box3().setFromObject(object)
  const size = box.getSize(new THREE.Vector3())
  const center = box.getCenter(new THREE.Vector3())
  const scale = 8 / Math.max(size.x, size.y, size.z, .001)
  object.position.sub(center).multiplyScalar(scale)
  object.scale.setScalar(scale)
  object.position.y += size.y * scale / 2
  state.pendingObject = { source: { ...result, content }, object, meshNames, dimensions: metadata.dimensions || [size.x, size.y, size.z], ...metadata }
}

// 作用：几何体对话框提交：生成 OBJ → 导入工程 → 建单实例分布 → 登记材质/冠层 → 加入对象列表
// → 保存工程并刷新场景。成功后打开对象属性对话框；失败时 toast 报错，按钮恢复可用。
// 步骤见下方各“步骤 n”。
// 注意：此流程在导入 OBJ 之后才保存工程。保存失败时，内存配置里新加的对象条目与计数会撤销
//   （否则重试会因“对象名称已存在”被拒）；但已写入工程目录的 OBJ 与位置文件不会删除——
//   服务端目前没有删除工程内文件的接口（/api/project/* 只有导入/保存/读取），属于已知遗留，
//   残留文件只占磁盘，不影响工程。
async function generateGeometryObject(event) {
  event.preventDefault()
  const submit = $('#geometryForm button[type="submit"]')
  const drawPlacement = state.drawPlacement
  state.drawPlacement = null
  let addedItem = null
  let projectPersisted = false
  try {
    submit.disabled = true
    ensureProjectState()
    // 步骤 1：读取并校验表单；名称不得与已有对象重复；火/雾介质只能用于 VoxelRT/VoxelEB。
    const values = geometryValues()
    if (state.config.objects.items.some((item) => item.name === values.name)) throw new Error(`对象名称已存在：${values.name}`)
    const medium = values.mode === 'fire' || values.mode === 'fog'
    if (medium && state.mode !== 'eVoxelRT' && state.mode !== 'eVoxelEB') throw new Error('参与介质当前只支持 VoxelRT 或 VoxelEB')
    // 步骤 2：按模式生成 OBJ 文本（chaos / water / 其余外壳），并解析一遍取网格组名，
    // 确认有可用三角面元；解析用的临时对象随即释放。
    const content = values.mode === 'chaos'
      ? chaosGeometryObj(values)
      : values.mode === 'water' ? waterSurfaceObj(values) : surfaceGeometryObj(values)
    const source = new OBJLoader().parse(content)
    const meshNames = []
    source.traverse((child) => { if (child.isMesh) { const name = child.name || child.material?.name || `mesh_${meshNames.length + 1}`; if (!meshNames.includes(name)) meshNames.push(name) } })
    disposeObject(source)
    if (!meshNames.length) throw new Error('生成结果中没有可用三角面元')
    // 步骤 3：确定对象类型（chaos→Vegetation，water→Water，surface→Building），把 OBJ 写入工程。
    const type = values.mode === 'fire' ? 'Fire' : values.mode === 'fog' ? 'Fog' : values.mode === 'chaos' ? 'Vegetation' : values.mode === 'water' ? 'Water' : 'Building'
    const imported = await api.importObj({ name: `PRIM_${values.name}.obj`, content })
    // 步骤 4：建立单实例（count=1，缩放 1）的分布并保存位置文件。位置来源随情形而定：
    // 火/雾用对话框中的介质位置；绘制放置（drawPlacement，非介质非水体）用点击位置；水体用位置+偏移。
    const distribution = {
      ...defaultDistribution({ type }), mode: 'single', basis: 'count', count: 1, scaleMin: 1, scaleMax: 1,
      // Position files expose X north, Z east and Y height. Internal x/y/z keys
      // retain their legacy order: x = north, y = east, z = height.
      ...(medium ? { minX: values.positionX, maxX: values.positionX, minY: values.positionY, maxY: values.positionY, z: values.positionZ } : {}),
      ...(drawPlacement && !medium && type !== 'Water' ? {
        minX: drawPlacement.north, maxX: drawPlacement.north,
        minY: drawPlacement.east, maxY: drawPlacement.east,
        z: drawPlacement.height
      } : {}),
      ...(type === 'Water' ? {
        minX: values.waterPositionX, maxX: values.waterPositionX,
        minY: values.waterPositionY, maxY: values.waterPositionY,
        z: values.waterPositionZ,
        offsetX: values.waterOffsetX, offsetY: values.waterOffsetY, offsetZ: values.waterOffsetZ
      } : {})
    }
    const instances = generateDistribution(distribution)
    const savedDistribution = await api.saveDistribution({ path: imported.positionPath, name: values.name, instances })
    // 步骤 5：确定缺省的光谱/热学/冠层绑定。介质对象使用专属名称（后缀 _spectrum、
    // _temperature、_medium），其余用内置预设（绿叶/水面/混凝土等）。
    const mediumLabel = type === 'Fog' ? '雾' : '火焰'
    const mediumKey = type === 'Fog' ? 'fog' : 'fire'
    const spectralName = medium ? `${values.name}_${mediumKey}_spectrum` : type === 'Vegetation' ? 'green_leaf' : type === 'Water' ? 'water_surface' : 'concrete'
    const thermalName = medium ? `${values.name}_${mediumKey}_temperature` : type === 'Vegetation' ? 'vegetation_temperature' : type === 'Water' ? 'water_temperature' : 'building_temperature'
    const canopyName = medium ? `${values.name}_${mediumKey}_medium` : type === 'Vegetation' ? 'canopy_default' : 'rigid_body'
    // 步骤 6：介质对象登记专属预设：光谱反射/透射为 0（只靠体散射与发射），
    // 热学取固定温度（向阳/背阴相同），冠层写入消光、散射反照率、不对称因子、发射缩放与固定温度。
    if (medium) {
      upsertByName(state.config.spectra, {
        name: spectralName, label: `${values.name} · ${mediumLabel}介质`,
        model: 'custom', reflectance: '0', transmittance: '0', refTir: 0, tauTir: 0, params: {}
      })
      upsertByName(state.config.thermals, {
        name: thermalName, label: `${values.name} · 固定温度`,
        sunlitTemperature: values.temperature, shadedTemperature: values.temperature
      })
      upsertByName(state.config.canopies, {
        name: canopyName, label: `${values.name} · ${mediumLabel}`,
        structureType: values.mode, lai: 0, density: 0, hc: values.sizeZ, G: 0,
        LIDFa: 0, LIDFb: 0, hspot: 0, leafwidth: 0,
        extinction: values.extinction, scatteringAlbedo: values.scatteringAlbedo,
        asymmetry: values.asymmetry, emissionScale: values.emissionScale,
        fixedTemperature: values.temperature
      })
    }
    // 步骤 7：组装对象条目，每个网格组绑定同一组光谱/热学/冠层；介质对象标记为仅辐射传输（radiationOnly）。
    const item = {
      name: values.name, type,
      materialName: type === 'Vegetation' || medium ? 'leaf_c3' : type === 'Water' ? 'water_set' : 'soil_dry', canopyName,
      fileName: imported.path, positionFile: savedDistribution.path,
      shape: values.shape, dimensions: [values.sizeX, values.sizeY, values.sizeZ],
      meshes: meshNames.map((name) => ({
        name, spectralName, thermalName,
        materialName: medium ? 'leaf_c3' : type === 'Water' ? 'water_set' : type === 'Vegetation' ? 'leaf_c3' : 'soil_dry',
        canopyName
      })),
      distribution, instanceCount: 1, sourceKind: 'prim',
      ...(medium ? { medium: { kind: values.mode, radiationOnly: true, coordinateOrder: 'XYZ' } } : {}),
      generation: { type: values.mode === 'fire' ? '火焰' : values.mode === 'fog' ? '雾' : values.mode === 'chaos' ? '几何体混沌' : values.mode === 'water' ? '水体' : '几何体', shape: values.shape, parameters: values }
    }
    // 步骤 8：加入对象列表、保存工程；保存失败则报错，用户取消（返回 null）则静默退出。
    // 随后关闭对话框、选中新对象、重新载入场景并打开属性对话框。
    state.config.objects.items.push(item)
    addedItem = item
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    state.project.configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('PRIM 原型保存失败')
    projectPersisted = true
    hideGeometryDialog()
    state.panel = 'objects'
    state.selectedObjectIndex = state.config.objects.items.length - 1
    $$('.tree-item').forEach((button) => button.classList.toggle('active', button.dataset.panel === 'objects'))
    refreshFromState(false)
    await loadActualScene(state.config)
    renderInspector()
    showObjectAttributeDialog(state.selectedObjectIndex)
    const triangleCount = values.mode === 'chaos' ? chaosTriangleCount(values) : surfaceTriangleCount(values)
    addLog(`${medium ? '参与介质' : 'PRIM 原型'}已生成并加入场景：${values.name}${medium ? ' · VoxelRT / VoxelEB' : ` · ${triangleCount} 个三角面元`}`, 'success')
    toast(medium ? '参与介质对象已创建' : 'PRIM 原型生成完成', `${values.name} · 已显示在场景中`)
  } catch (error) {
    // 工程尚未保存成功就失败：撤销内存里新加的对象条目（保存成功后的刷新类错误不撤销，条目已落盘）。
    if (addedItem && !projectPersisted) {
      const added = state.config.objects.items.indexOf(addedItem)
      if (added >= 0) state.config.objects.items.splice(added, 1)
      state.config.objects.count = state.config.objects.items.length
      state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    }
    toast('几何体生成失败', error.message, 'error')
  }
  finally { submit.disabled = false }
}

// 作用：导入 OBJ 的入口：弹出文件选择，清理 OBJ 文本（sanitizeObjContent），解析为待确认对象，
// 再打开材质对话框；需先有工程。已有未确认的待导入对象会先释放。失败时 toast/日志提示。
async function importObj() {
  if (!state.inputPath) { toast('无法导入 OBJ', '请先新建或打开工程', 'error'); return }
  let result
  try { result = await api.chooseObj() }
  catch (error) { toast('无法导入 OBJ', error.message, 'error'); addLog(`无法导入 OBJ：${error.message}`, 'error'); return }
  if (!result) return
  try {
    const content = sanitizeObjContent(result.content)
    if (state.pendingObject) disposeObject(state.pendingObject.object)
    preparePendingObject(result, content)
    showMaterialDialog()
  } catch (error) { toast('OBJ 解析失败', error.message, 'error') }
}

// 作用：按 name 更新列表中的同名条目（整体替换），不存在则追加。
function upsertByName(list, value) {
  const index = list.findIndex((item) => item.name === value.name)
  if (index >= 0) list[index] = value
  else list.push(value)
}

// 作用：把物理材质表单（FormData 转成的对象）转换成工程里的材质预设条目 {name, type, params}。
// 输入：values 表单值，type 物化类型（Building/Photovoltaic/Membrane/Vegetation/Wood/Soil/Water/
// Ship/Vehicle）；输出的 type 映射为引擎支持的类型：光伏、薄膜→Building（加 energyModel），
// 木质→Vegetation（energyModel=wood）。
// 说明：各类型分别取对应字段并做下限/范围限幅（如薄膜面热容 ≥1、水体蒸发系数限 0–1、
// 折射率 ≥1）；默认分支为土壤/建筑/船舶/载具的固体热参数，可附加墙体模式、船体和座舱参数；
// 最后过滤掉非有限数值，避免缺失字段被序列化成 null。
function physicalFromForm(values, type) {
  let params
  if (type === 'Photovoltaic') {
    params = Object.fromEntries(['eta25','gamma','bifaciality','heatCapacityPerArea','convectiveScale'].map(k=>[k,Number(values['pv_'+k])]))
  } else if (type === 'Membrane') {
    params = {
      heatCapacityPerArea: Math.max(1, Number(values.membraneHeatCapacityPerArea) || 800),
      convectiveScale: Math.max(0.1, Number(values.membraneConvectiveScale) || 1)
    }
  } else if (type === 'Wood') {
    params = {
      method: 1, rss: 1000000000, cs: Math.max(1, Number(values.soilCs)), rhos: Math.max(1, Number(values.soilRhos)),
      lambdas: Math.max(0.001, Number(values.soilLambdas)), Tsoil: Number(values.soilTsoil), SMC: 0, Satwater: 0,
      convectiveScale: Math.max(0.1, Number(values.woodConvectiveScale))
    }
  } else if (type === 'Vegetation') {
    params = {
      Vcmax: Number(values.bioVcmax), m: Number(values.bioM), BallBerry: Number(values.bioBallBerry), Type: Number(values.bioType),
      kV: Number(values.bioKV), Rdparam: Number(values.bioRdparam), Tparam: values.bioTparam, Tyear: Number(values.bioTyear),
      beta: Number(values.bioBeta), kNPQs: Number(values.bioKNPQs), qLs: Number(values.bioQLs),
      stressfactor: Number(values.bioStressfactor), Tcor: Number(values.bioTcor)
    }
  } else if (type === 'Water') {
    params = {
      rss: Math.max(0, Number(values.waterRss)), heatCapacity: Math.max(1, Number(values.waterHeatCapacity)),
      mixingDepth: Math.max(0.01, Number(values.waterMixingDepth)), evaporationCoefficient: Math.max(0, Math.min(1, Number(values.waterEvaporationCoefficient))),
      brdfModel: Number(values.waterBrdfModel) === 1 ? 1 : 0, refractiveIndex: Math.max(1, Number(values.waterRefractiveIndex)),
      slopeVariance: Math.max(0, Number(values.waterSlopeVariance)), diffuseFraction: Math.max(0, Math.min(1, Number(values.waterDiffuseFraction)))
    }
    if (values.waterInitialMode === 'dailyMeanAir' || values.waterInitialMode === 'fixed') params.initialTemperatureMode = values.waterInitialMode
    if (values.waterInitialMode === 'fixed') params.initialTemperature = Number(values.waterInitialTemperature)
  } else {
    params = {
      method: Number(values.soilMethod), rss: Number(values.soilRss), cs: Number(values.soilCs), rhos: Number(values.soilRhos),
      lambdas: Number(values.soilLambdas), Tsoil: Number(values.soilTsoil), SMC: Number(values.soilSMC), Satwater: Number(values.soilSatwater)
    }
    if (values.wallMode === 'wall') Object.assign(params, {
      wallMode: 'wall', thickness: Number(values.wallThickness), indoorTemperature: Number(values.indoorTemperature),
      indoorHeatTransferCoefficient: Number(values.indoorHeatTransferCoefficient)
    })
    else if (values.wallMode === 'shell') params.wallMode = 'shell'
    if (values.wallMode === 'wall' && String(values.wallLayers || '').trim()) params.layers = parseWallLayers(values.wallLayers)
    // 船舶附加参数：航速（≥0）；水线由固定高度或场景水面（+吃水偏移）给出，二者互斥且固定高度优先；
    // 航向与风来向须同时给出才写入；开关量与内热源只在开启/非零时写入，其余沿用引擎缺省。
    if (type === 'Ship') {
      if (values.shipSpeed !== undefined) params.speed = Math.max(0, Number(values.shipSpeed) || 0)
      const given = (key) => String(values[key] ?? '').trim() !== ''
      if (given('shipWaterlineHeight')) {
        params.waterlineHeight = Number(values.shipWaterlineHeight)
        params.waterHeatTransferCoefficient = Number(values.shipWaterHeatTransferCoefficient)
        if (given('shipWaterTemperature')) params.waterTemperature = Number(values.shipWaterTemperature)
      } else if (values.shipWaterlineSource === 'water') {
        params.waterlineSource = 'water'
        params.waterlineOffset = Number(values.shipWaterlineOffset) || 0
        params.waterHeatTransferCoefficient = Number(values.shipWaterHeatTransferCoefficient)
        if (given('shipWaterTemperature')) params.waterTemperature = Number(values.shipWaterTemperature)
      }
      if (given('shipHeading') && given('shipWindDirection')) {
        params.heading = Number(values.shipHeading)
        params.windDirection = Number(values.shipWindDirection)
      }
      if (values.shipWaterShielding === '1') params.waterShielding = true
      if (values.shipWaterFeedback === '1') params.waterFeedback = true
      if (values.shipVoxelWall === '1') params.voxelWall = true
      if (Number(values.shipInternalHeatFlux)) params.internalHeatFlux = Number(values.shipInternalHeatFlux)
      if (Number(values.shipInternalHeatPower)) params.internalHeatPower = Number(values.shipInternalHeatPower)
    }
    // 载具附加参数：vehiclePart 标明部件（车身/玻璃/底盘等）；只有车身（body）部件写入座舱 cabin。
    // 各 num(...) 的第二个参数为缺省值（如容积 2.8 m³、通风 0.5 ACH、开窗缝 10 ACH、空调 3500 W、
    // 设定 24 ℃、发动机 15000 W、进车份额 0.3）；其余单位以对应表单项为准。
    if (type === 'Vehicle' && values.vehiclePart !== undefined) {
      // Cabin / driving parameters of a Vehicle material (FacetEB only; absent fields keep the engine defaults).
      const on = (key) => values[key] === '1' || values[key] === 'on' || values[key] === true
      const given = (key) => String(values[key] ?? '').trim() !== ''
      const num = (key, fallback) => (given(key) && Number.isFinite(Number(values[key])) ? Number(values[key]) : fallback)
      if (values.vehiclePart) params.vehiclePart = values.vehiclePart
      if (values.vehiclePart === 'body') {
        params.cabin = {
          enabled: on('cabinEnabled'), occupants: Math.max(0, Math.round(num('cabinOccupants', 0))), metabolicW: num('cabinMetabolicW', 100),
          speed: Math.max(0, num('cabinSpeed', 0)), ventilationACH: Math.max(0, num('cabinVentilationACH', 0.5)),
          windowCracked: on('cabinWindowCracked'), crackedACH: num('cabinCrackedACH', 10), acOn: on('cabinAcOn'), acPower: num('cabinAcPower', 3500),
          acSetpoint: num('cabinAcSetpoint', 24), engineOn: on('cabinEngineOn'), enginePower: num('cabinEnginePower', 15000),
          engineShare: num('cabinEngineShare', 0.3), volume: num('cabinVolume', 2.8), interiorCapacity: num('cabinInteriorCapacity', 80000),
          interiorArea: num('cabinInteriorArea', 8), interiorH: num('cabinInteriorH', 8), absorbedFraction: num('cabinAbsorbedFraction', 0.8)
        }
        if (given('cabinHeading') && given('cabinWindDirection')) { params.cabin.heading = Number(values.cabinHeading); params.cabin.windDirection = Number(values.cabinWindDirection) }
        // 内饰分节点与发动机模型：仅在对应开关打开时写入 cabin.interior / cabin.engine。
        // 以内置缺省 + 隐藏 JSON 中保留的节点参数为底，再用表单值覆盖，避免界面没有的字段丢失。
        // 内饰四个节点的份额缺省为仪表台 0.30、座椅 0.35、方向盘 0.05、内衬 0.30（合计 1）；
        // 发动机的散热器/机体/排气份额缺省为 0.5/0.3/0.2（合计 1）。
        // Parts model (interior nodes / engine): hidden JSON keeps the node parameters the form does not show.
        let kept = {}
        try { kept = JSON.parse(values.cabinPartsJson || '{}') || {} } catch { kept = {} }
        if (on('cabinInteriorNodes')) {
          const base = { ...vehicleInteriorDefaults(values.cabinSeatType), ...(kept.interior || {}) }
          const seat = VEHICLE_SEAT_TYPES[values.cabinSeatType] || VEHICLE_SEAT_TYPES.fabric
          const seatChanged = base.seatType !== (values.cabinSeatType || 'fabric')
          params.cabin.interior = { ...base, enabled: true, seatType: values.cabinSeatType || 'fabric', shellRadFraction: num('cabinShellRadFraction', 0.45),
            seatEffusivity: seatChanged ? seat.effusivity : base.seatEffusivity,
            dashboard: { ...base.dashboard, share: num('cabinShareDashboard', 0.3) },
            seat: { ...base.seat, share: num('cabinShareSeat', 0.35), ...(seatChanged ? { capacity: seat.capacity } : {}) },
            wheel: { ...base.wheel, share: num('cabinShareWheel', 0.05) }, lining: { ...base.lining, share: num('cabinShareLining', 0.3) } }
        }
        if (on('cabinEngineModel')) {
          params.cabin.engine = { ...vehicleEngineDefaults(), ...(kept.engine || {}), enabled: true, warm: on('cabinEngineWarm'), warmTemperature: num('cabinEngineWarmTemperature', 90),
            radiatorShare: num('cabinEngineRadiatorShare', 0.5), blockShare: num('cabinEngineBlockShare', 0.3), exhaustShare: num('cabinEngineExhaustShare', 0.2), firewallG: num('cabinFirewallG', 2) }
        }
      }
    }
  }
  // FIXED 配套：过滤掉 NaN/Infinity，避免“缺失”被写成 NaN（JSON 里是 null）
  params = Object.fromEntries(Object.entries(params).filter(([, value]) => typeof value !== 'number' || Number.isFinite(value)))
  // 返回材质条目：光伏/薄膜/木质以 energyModel 区分，type 映射为 Building 或 Vegetation。
  if (type === 'Photovoltaic') return {name:values.materialName,type:'Building',energyModel:'photovoltaic',params}
  if (type === 'Membrane') return { name: values.materialName, type: 'Building', energyModel: 'membrane', params }
  return type === 'Wood'
    ? { name: values.materialName, type: 'Vegetation', energyModel: 'wood', params }
    : { name: values.materialName, type, params }
}

// 预设对话框的表单控件小工具：presetSelect 生成下拉框，presetText 生成文本/数值输入框（均转义标签与值）。
function presetSelect(label, id, name, options) {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><select class="select" id="${id}" name="${name}">${options}</select></label>`
}

function presetText(label, name, value, attrs = '') {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><input class="input" name="${name}" value="${escapeHtml(value)}" ${attrs}></label>`
}

// 作用：把逗号分隔的光谱值字符串拆成数组；为空时返回 [fallback]。
function splitSpectrumValues(value, fallback) {
  const values = String(value ?? '').split(',').map((entry) => entry.trim()).filter(Boolean)
  return values.length ? values : [String(fallback)]
}

// 作用：读取传感器配置 sensor.bands（按空白、逗号或分号分隔）作为波段标签列表；
// 未配置时返回占位 ['默认波段']，调用方据此判断是否为多波段。
function configuredSpectralBands() {
  const bands = String(state.config?.sensor?.bands || '').split(/[\s,;]+/).map((entry) => entry.trim()).filter(Boolean)
  return bands.length ? bands : ['默认波段']
}

// 作用：生成反射率/透射率输入框（0–1 数值）。useConfiguredBands 且配置了多个波段时，
// 每个波段一个输入框（标签带波段，单位 nm），值按顺序取自 value，缺失时用第一个值再用 fallback；
// 否则只生成一个输入框。输入框 name 为 `${name}Value`，由 currentSpectrumValues 回收。
function presetSpectrumFields(label, name, value, fallback, useConfiguredBands = false) {
  const sourceValues = splitSpectrumValues(value, fallback)
  const bands = useConfiguredBands ? configuredSpectralBands() : []
  const fieldCount = bands.length > 1 ? bands.length : 1
  return Array.from({ length: fieldCount }, (_, index) => {
    const entry = sourceValues[index] ?? sourceValues[0] ?? String(fallback)
    const fieldLabel = fieldCount > 1 ? `${label}（${bands[index]} nm）` : label
    return presetText(fieldLabel, `${name}Value`, entry, 'type="number" min="0" max="1" step="any" required')
  }).join('')
}

// 作用：收集预设对话框内名为 `${name}Value` 的输入框，去空后用逗号拼接（与材质对话框的
// materialSpectrumValues 对应，只是作用域不同）。
function currentSpectrumValues(name) {
  return $$(`#presetParameterFields [name="${name}Value"]`).map((input) => input.value.trim()).filter(Boolean).join(',')
}

// 作用：按预设类别返回工程配置中的数组：spectrum→spectra，thermal→thermals，canopy→canopies，
// 其余（物理）→materials。
function presetList(kind) {
  if (kind === 'spectrum') return state.config.spectra
  if (kind === 'thermal') return state.config.thermals
  if (kind === 'canopy') return state.config.canopies
  return state.config.materials
}

// 作用：按“预设类别”重绘预设对话框里的参数区。item 为正在编辑的预设（新建时为 null），
// spectralModel 为光谱预设的当前模型；对话框的类别在 #presetKind。
// 分支：光谱 / 热学 / 冠层 / 物理（其余），每个分支生成输入框后即返回；见下方各步骤。
function renderPresetParameterFields(spectralModel = 'custom', item = null) {
  const kind = $('#presetKind').value
  const target = $('#presetParameterFields')
  // 步骤 1：光谱预设。模型 custom（面板数据）/ Prospect / BSM / file（导入波谱 TXT）；
  // 模型参数缺省取内置同模型预设，再用当前预设已有的有效参数覆盖。
  // 切换模型时先把当前输入暂存再重绘（见下方 change 监听），避免丢失已填内容。
  // Prospect 缺省反射/透射 0.08/0.04，其余 0.20/0.0；只有 custom 才按传感器波段展开多个输入框。
  if (kind === 'spectrum') {
    const current = item || {}
    const models = [['custom', '自定义（面板数据）'], ['Prospect', 'PROSPECT 模型'], ['BSM', 'BSM 模型'], ['file', '导入波谱 TXT']]
    const modelOptions = models.map(([value, label]) => `<option value="${value}" ${value === spectralModel ? 'selected' : ''}>${label}</option>`).join('')
    const modelDefault = createMaterialPresets().spectra.find((entry) => entry.model === spectralModel) || createMaterialPresets().spectra[0]
    const defaults = { ...(modelDefault.params || {}), ...finiteParams(current.params) }
    let parameters = ''
    if (spectralModel === 'Prospect') {
      parameters = `${physicalInput('Cab', 'paramCab', defaults.Cab, 0.1)}${physicalInput('Cw', 'paramCw', defaults.Cw, 0.001)}${physicalInput('Cdm', 'paramCdm', defaults.Cdm, 0.001)}${physicalInput('Cs', 'paramCs', defaults.Cs, 0.001)}${physicalInput('N', 'paramN', defaults.N, 0.1)}`
    } else if (spectralModel === 'BSM') {
      parameters = `${physicalInput('土壤含水量 SMC', 'paramSMC', defaults.SMC, 0.1)}${physicalInput('亮度', 'paramBSMBrightness', defaults.BSMBrightness, 0.1)}${physicalInput('纬度参数', 'paramBSMlat', defaults.BSMlat, 0.1)}${physicalInput('经度参数', 'paramBSMlon', defaults.BSMlon, 0.1)}${physicalInput('湿端 TIR 反射率（饱和土壤，留空自动取下限）', 'refTirWet', current.refTirWet ?? '', 0.005)}<div class="notice" style="grid-column:1 / -1">土壤发射率随含水量升高：SMC ≤ 0.05 为上面的 TIR 反射率，SMC 越高越接近这里的湿端值（留空即干端发射率 +0.03，上限 0.995）。发射率 = 1 − TIR 反射率 − TIR 透射率。</div>`
    } else if (spectralModel === 'file') {
      parameters = spectrumFileImportFields('preset', current.fileName || '')
    }
    target.innerHTML = `${presetSelect('光谱模型', 'presetSpectralModel', 'spectralModel', modelOptions)}${presetSpectrumFields('反射率', 'reflectance', current.reflectance, spectralModel === 'Prospect' ? '0.08' : '0.20', spectralModel === 'custom')}${presetSpectrumFields('透射率', 'transmittance', current.transmittance, spectralModel === 'Prospect' ? '0.04' : '0.0', spectralModel === 'custom')}${physicalInput('TIR 反射率', 'refTir', current.refTir ?? 0.05, 0.01)}${physicalInput('TIR 透射率', 'tauTir', current.tauTir ?? 0, 0.01)}${opennessField(current.openness)}${parameters}${previewTextureFields(current.previewTexture)}${physicalTextureFields(current.physicalTexture)}${specularParameterFields(current.specular)}${classTextureFields(current.classTexture)}${opennessTextureFields(current.opennessTexture)}`
    bindPhysicalTextureImport('#presetForm')
    bindSurfaceTextureFields('#presetForm', current.name || $('#presetName').value)
    // 切换光谱模型：收集当前 param* 参数、反射/透射、TIR、纹理、镜面等表单值作为新的 item 再重绘；
    // 表单中缺失或非有限的参数不保留。
    $('#presetSpectralModel').addEventListener('change', (event) => {
      const preservedParams = finiteParams(current.params)
      $$(`#presetParameterFields [name^="param"]`).forEach((input) => {
        const value = finiteInputNumber(input)
        if (value !== null) preservedParams[input.name.slice(5)] = value
      })
      const formData = new FormData($('#presetForm'))
      renderPresetParameterFields(event.target.value, {
        ...current,
        model: event.target.value,
        reflectance: currentSpectrumValues('reflectance'),
        transmittance: currentSpectrumValues('transmittance'),
        refTir: Number($('#presetParameterFields [name="refTir"]').value),
        refTirWet: optionalTirReflectance($('#presetParameterFields [name="refTirWet"]')?.value),
        tauTir: Number($('#presetParameterFields [name="tauTir"]').value),
        openness: Number($('#presetParameterFields [name="openness"]')?.value) || 0,
        fileName: $('#presetParameterFields [name="fileName"]')?.value || '',
        previewTexture: previewTextureFromValues(Object.fromEntries(formData.entries())),
        physicalTexture: physicalTextureFromValues(Object.fromEntries(formData.entries())),
        specular: specularParametersFromValues(Object.fromEntries(formData.entries()), current.specular),
        classTexture: classTextureFromForm(formData),
        opennessTexture: opennessTextureFromValues(Object.fromEntries(formData.entries())),
        params: preservedParams
      })
    })
    if (spectralModel === 'file') bindSpectrumFileInput('preset', '#presetForm')
    return
  }
  if (kind === 'thermal') {
    // 步骤 2：热学预设：阳面/阴面温度（K），新建缺省 305 K / 295 K。
    target.innerHTML = `${physicalInput('阳面温度（K）', 'sunlitTemperature', item?.sunlitTemperature ?? 305, 0.1)}${physicalInput('阴面温度（K）', 'shadedTemperature', item?.shadedTemperature ?? 295, 0.1)}`
    return
  }
  if (kind === 'canopy') {
    // 步骤 3：冠层预设。结构类型：canopy 混沌介质（多孔透射）/ rigid 刚体 / fire / fog；
    // 缺省取内置第一个冠层预设并被已有 item 覆盖。火/雾使用后六项介质参数（消光 m⁻¹、反照率、
    // 不对称因子、发射倍率、固定温度 K）。
    const defaults = { ...createMaterialPresets().canopies[0], ...(item || {}) }
    const structureOptions = [['canopy', '混沌介质（多孔透射）'], ['rigid', '刚体（不透光轮廓）'], ['fire', '火焰（参与介质）'], ['fog', '雾（参与介质）']]
      .map(([value, label]) => `<option value="${value}" ${value === defaults.structureType ? 'selected' : ''}>${label}</option>`).join('')
    target.innerHTML = `${presetSelect('结构类型', 'presetStructureType', 'structureType', structureOptions)}${physicalInput('叶面积指数 LAI', 'lai', defaults.lai, 0.01)}${physicalInput('叶面积密度 LAD', 'density', defaults.density, 0.01)}${physicalInput('介质高度 hc（m）', 'hc', defaults.hc, 0.01)}${physicalInput('投影系数 G（仅 LIDF 无效时使用）', 'G', defaults.G, 0.01)}${physicalInput('叶倾角参数 LIDFa', 'LIDFa', defaults.LIDFa, 0.01)}${physicalInput('叶倾角参数 LIDFb', 'LIDFb', defaults.LIDFb, 0.01)}<div class="notice" id="presetCanopyGHint" style="grid-column:1 / -1"></div>${physicalInput('热点参数 hspot', 'hspot', defaults.hspot, 0.01)}${physicalInput('叶宽 leafwidth（m）', 'leafwidth', defaults.leafwidth, 0.01)}${physicalInput('消光系数（m⁻¹）', 'extinction', defaults.extinction ?? 0, 0.01)}${physicalInput('单次散射反照率', 'scatteringAlbedo', defaults.scatteringAlbedo ?? 0, 0.01)}${physicalInput('散射非对称因子', 'asymmetry', defaults.asymmetry ?? 0, 0.01)}${physicalInput('发射倍率', 'emissionScale', defaults.emissionScale ?? 0, 0.01)}${physicalInput('固定温度（K）', 'fixedTemperature', defaults.fixedTemperature ?? 0, 1)}`
    // 作用：按结构类型与 LIDFa/LIDFb 判断叶倾角模式（canopyLeafAngleMode）；当引擎会由 LIDF
    // 推导投影系数 G 时，G 输入框设为只读并显示说明，输入变化时联动刷新。
    const updateCanopyGField = () => {
      const field = (name) => $(`#presetParameterFields [name="${name}"]`)
      const mode = canopyLeafAngleMode({ structureType: field('structureType')?.value, LIDFa: field('LIDFa')?.value, LIDFb: field('LIDFb')?.value })
      const gInput = field('G')
      // The stored G is kept (old projects stay readable); it is only read-only
      // while the engine derives G(Ω) from LIDFa/LIDFb.
      if (gInput) gInput.readOnly = mode === 'lidf' || mode === 'spherical'
      const hint = $('#presetCanopyGHint')
      if (hint) hint.textContent = CANOPY_G_HINTS[mode]
    }
    $$('#presetParameterFields [name="structureType"], #presetParameterFields [name="LIDFa"], #presetParameterFields [name="LIDFb"]')
      .forEach((input) => { input.addEventListener('input', updateCanopyGField); input.addEventListener('change', updateCanopyGField) })
    updateCanopyGField()
    return
  }
  // 步骤 4：物理（物化）预设。物化类型由 state.presetPhysicalType 决定，缺省值取内置同类预设
  // （光伏/薄膜/木质按 energyModel 匹配，其余按 type 且无 energyModel 的预设），再叠加已有 item.params。
  const typeOptions = [['Building', '建筑表面'], ['Photovoltaic', '光伏板热电耦合'], ['Membrane', '篷布/织物薄膜（FacetEB）'], ['Vegetation', '植被生理生化'], ['Wood', '木质固体热平衡'], ['Soil', '土壤表面物化'], ['Water', '水体物性'], ['Ship', '船舶表面物化'], ['Vehicle', '载具物化（车身/玻璃/底盘，FacetEB 座舱）']].map(([value, label]) => `<option value="${value}" ${value === state.presetPhysicalType ? 'selected' : ''}>${label}</option>`).join('')
  const baseDefaults = createMaterialPresets().materials.find((entry) => state.presetPhysicalType === 'Photovoltaic' ? entry.energyModel === 'photovoltaic' : state.presetPhysicalType === 'Membrane' ? entry.energyModel === 'membrane' : state.presetPhysicalType === 'Wood' ? entry.energyModel === 'wood' : entry.type === state.presetPhysicalType && !entry.energyModel)?.params || {}
  const defaults = { ...baseDefaults, ...(item?.params || {}) }
  // An existing material keeps its own (possibly absent) fields whose engine
  // fallbacks differ from the new-material defaults, so editing it cannot
  // silently switch an old wall to the layered construction or an old water
  // body to the new mixing depth / start temperature.
  if (item) {
    if (!item.params?.wallMode) delete defaults.wallMode
    if (!Array.isArray(item.params?.layers)) delete defaults.layers
    if (!item.params?.initialTemperatureMode) delete defaults.initialTemperatureMode
    if (state.presetPhysicalType === 'Water' && item.params?.mixingDepth === undefined) defaults.mixingDepth = 0.5
  }
  // 步骤 5：按物化类型生成参数输入框（薄膜、光伏、植被、水体、木质，其余为土壤/建筑/船舶/载具的
  // 固体参数 + 墙体模式 + 船体/载具专有字段）；各字段保存时由 physicalFromForm 读回。
  let parameters
  if (state.presetPhysicalType === 'Membrane') {
    parameters = `${physicalInput('面热容（J·m⁻²·K⁻¹）', 'membraneHeatCapacityPerArea', defaults.heatCapacityPerArea, 10)}${physicalInput('对流换热倍率', 'membraneConvectiveScale', defaults.convectiveScale, 0.1)}<div class="notice" style="grid-column:1 / -1">单层薄板：两面共用一个温度，两面同时吸收/发射辐射并与空气对流换热，不计蒸腾。面热容 = 面密度 × 比热容，例如 PVC 涂层帆布 0.6 kg·m⁻² × 1300 ≈ 800；PE 篷布 0.15 × 1900 ≈ 300；HDPE 遮阳网（含孔）约 100–300。仅 FacetEB 使用，VoxelEB 不支持。</div>`
  } else if (state.presetPhysicalType === 'Photovoltaic') {
    parameters = [['参考效率（25℃）','eta25',0.001],['温度系数（K⁻¹）','gamma',0.0001],['背面发电系数','bifaciality',0.01],['面热容（J·m⁻²·K⁻¹）','heatCapacityPerArea',100],['对流换热倍率','convectiveScale',0.1]].map(([label,k,step])=>physicalInput(label,'pv_'+k,defaults[k],step)).join('')+'<div class="notice">使用 FacetEB 与不透明光谱。温度由辐射、对流、储热与发电共同求解；OBJ 正面为电池正面。</div>'
  } else if (state.presetPhysicalType === 'Vegetation') {
    parameters = [
      physicalInput('Vcmax', 'bioVcmax', defaults.Vcmax, 0.1), physicalInput('气孔斜率 m', 'bioM', defaults.m, 0.1),
      physicalInput('Ball-Berry 截距', 'bioBallBerry', defaults.BallBerry, 0.001), physicalInput('光合类型', 'bioType', defaults.Type, 1),
      physicalInput('kV', 'bioKV', defaults.kV, 0.0001), physicalInput('暗呼吸系数', 'bioRdparam', defaults.Rdparam, 0.001),
      presetText('温度响应参数', 'bioTparam', defaults.Tparam), physicalInput('年均温', 'bioTyear', defaults.Tyear, 0.1),
      physicalInput('beta', 'bioBeta', defaults.beta, 0.001), physicalInput('NPQ 系数', 'bioKNPQs', defaults.kNPQs, 0.01),
      physicalInput('光限制', 'bioQLs', defaults.qLs, 0.01), physicalInput('胁迫因子', 'bioStressfactor', defaults.stressfactor, 0.01),
      physicalInput('温度校正', 'bioTcor', defaults.Tcor, 1)
    ].join('')
  } else if (state.presetPhysicalType === 'Water') {
    parameters = `${physicalInput('表面阻力 rss', 'waterRss', defaults.rss, 1)}${physicalInput('体积热容', 'waterHeatCapacity', defaults.heatCapacity, 1)}${physicalInput('混合深度', 'waterMixingDepth', defaults.mixingDepth, 0.1)}${physicalInput('蒸发系数', 'waterEvaporationCoefficient', defaults.evaporationCoefficient, 0.1)}${physicalInput('辐射模型（0 Lambert / 1 Cox–Munk）', 'waterBrdfModel', defaults.brdfModel, 1)}${physicalInput('水体折射率', 'waterRefractiveIndex', defaults.refractiveIndex, 0.001)}${physicalInput('水面坡度方差（0 随风速）', 'waterSlopeVariance', defaults.slopeVariance, 0.001)}${physicalInput('漫反射比例', 'waterDiffuseFraction', defaults.diffuseFraction, 0.01)}${waterInitialFields(defaults)}`
  } else if (state.presetPhysicalType === 'Wood') {
    parameters = `${physicalInput('比热容 cs（J·kg⁻¹·K⁻¹）', 'soilCs', defaults.cs, 1)}${physicalInput('密度 rhos（kg·m⁻³）', 'soilRhos', defaults.rhos, 1)}${physicalInput('导热率 λ（W·m⁻¹·K⁻¹）', 'soilLambdas', defaults.lambdas, 0.01)}${physicalInput('对流换热倍率', 'woodConvectiveScale', defaults.convectiveScale ?? 1, 0.1)}${physicalInput('初始温度（℃）', 'soilTsoil', defaults.Tsoil, 0.1)}<div class="notice">木质固体不计算光合作用和蒸腾，温度由辐射、对流、导热与热储存共同求解。</div>`
  } else {
    parameters = `${physicalInput('方法', 'soilMethod', defaults.method, 1)}${physicalInput('表面阻力 rss', 'soilRss', defaults.rss, 1)}${physicalInput('比热容 cs', 'soilCs', defaults.cs, 1)}${physicalInput('密度 rhos', 'soilRhos', defaults.rhos, 1)}${physicalInput('导热率 λ', 'soilLambdas', defaults.lambdas, 0.01)}${physicalInput('初始温度', 'soilTsoil', defaults.Tsoil, 0.1)}${physicalInput('土壤含水量 SMC', 'soilSMC', defaults.SMC, 0.1)}${physicalInput('饱和含水量', 'soilSatwater', defaults.Satwater, 0.01)}${wallModeFields(defaults)}${state.presetPhysicalType === 'Ship' ? shipFields(defaults) : ''}${state.presetPhysicalType === 'Vehicle' ? vehicleFields(defaults) : ''}`
  }
  // 步骤 6：写入类型下拉框与参数；切换物化类型时更新 state.presetPhysicalType 并按 custom 重绘。
  target.innerHTML = `${presetSelect('物化类型', 'presetPhysicalType', 'physicalType', typeOptions)}${parameters}`
  $('#presetPhysicalType').addEventListener('change', (event) => {
    state.presetPhysicalType = event.target.value
    renderPresetParameterFields('custom')
  })
}

// 作用：打开预设（材质属性）对话框。index ≥ 0 为编辑已有预设（类别与名称锁定），否则为新增；
// 编辑时回填名称、显示名、物化类型，并按预设内容渲染参数区。需先有工程。
function showPresetDialog(kind = 'spectrum', index = -1) {
  if (!state.inputPath) { toast('无法编辑材质属性', '请先新建或打开工程', 'error'); return }
  $('#presetForm').reset()
  const list = presetList(kind)
  const item = Number.isInteger(index) && index >= 0 ? list[index] : null
  state.editingPreset = item ? { kind, index } : null
  $('#presetKind').value = kind
  $('#presetKind').disabled = Boolean(item)
  $('#presetName').readOnly = Boolean(item)
  $('#presetName').value = item?.name || ''
  $('#presetLabel').value = item?.label || ''
  state.presetPhysicalType = item?.energyModel === 'photovoltaic' ? 'Photovoltaic' : item?.energyModel === 'membrane' ? 'Membrane' : item?.energyModel === 'wood' ? 'Wood' : (item?.type || 'Vegetation')
  $('#presetDialogTitle').textContent = item ? `修改材质属性 · ${item.label || item.name}` : '新增材质属性'
  $('#presetSubmitBtn').innerHTML = item ? '<svg><use href="#i-save"/></svg>保存修改' : '<svg><use href="#i-plus"/></svg>加入预设库'
  renderPresetParameterFields(item?.model || 'custom', item)
  $('#presetDialog').hidden = false
  setTimeout(() => (item ? $('#presetParameterFields .input, #presetParameterFields .select') : $('#presetName'))?.focus(), 0)
}

// 作用：关闭预设对话框并恢复类别/名称的可编辑状态，清除编辑中标记。
function hidePresetDialog() {
  $('#presetDialog').hidden = true
  $('#presetKind').disabled = false
  $('#presetName').readOnly = false
  state.editingPreset = null
}

// 光谱模型参数的键名：Prospect 的 Cab/Cw/Cdm/Cs/N，BSM 的 SMC/BSMBrightness/BSMlat/BSMlon。
const SPECTRUM_MODEL_PARAMS = ['Cab', 'Cw', 'Cdm', 'Cs', 'N', 'SMC', 'BSMBrightness', 'BSMlat', 'BSMlon']

// Parameters missing from the form (other spectral models) or stored as null
// must not override model defaults with NaN/0.
function finiteParams(params) {
  return Object.fromEntries(Object.entries(params || {}).filter(([, value]) => value !== null && value !== '' && Number.isFinite(Number(value))))
}

// 作用：以旧参数（仅有限值）为底，用表单中非空且有限的 param* 值覆盖，得到光谱模型参数对象。
function spectrumParamsFromValues(values, previous) {
  const params = finiteParams(previous)
  for (const key of SPECTRUM_MODEL_PARAMS) {
    const raw = String(values[`param${key}`] ?? '').trim()
    if (raw !== '' && Number.isFinite(Number(raw))) params[key] = Number(raw)
  }
  return params
}
// Blank keeps the engine default: the wet (saturated) TIR reflectance is derived
// from the dry one, so soil emissivity still rises with water content.
function optionalTirReflectance(value) {
  if (value === '' || value === undefined || value === null) return undefined
  const numeric = Number(value)
  if (!Number.isFinite(numeric)) return undefined
  return Math.max(0, Math.min(1, numeric))
}

// 作用：把预设对话框表单转成 {list, item}：list 为目标数组，item 为待写入的预设条目。
// 编辑时沿用原名称与类别，新增时名称由 uniqueName 去重，显示名为空则取名称。
// 光谱：纹理（classTexture / opennessTexture）仅在启用或原来已有时写入。
// 冠层：结构类型非法回落 canopy；消光、发射倍率、固定温度 ≥ 0，散射反照率限 [0,1]，
// 不对称因子限 ±0.99。物理：交给 physicalFromForm。
function presetFromForm(values) {
  const editing = state.editingPreset
  const existing = editing ? presetList(editing.kind)[editing.index] : null
  const name = existing?.name || uniqueName(values.presetName, 'material')
  const label = String(values.presetLabel || '').trim() || name
  const kind = editing?.kind || values.presetKind
  if (kind === 'spectrum') {
    const classTexture = values.classTexture
    const opennessTexture = opennessTextureFromValues(values)
    const surfaceTextures = {
      ...(classTexture?.enabled || existing?.classTexture ? { classTexture } : {}),
      ...(opennessTexture.enabled || existing?.opennessTexture ? { opennessTexture } : {})
    }
    return {
      list: state.config.spectra,
      item: { name, label, model: values.spectralModel, fileName: values.fileName || '', reflectance: values.reflectance, transmittance: values.transmittance, refTir: Number(values.refTir), refTirWet: optionalTirReflectance(values.refTirWet), tauTir: Number(values.tauTir), openness: Number(values.openness) || 0, previewTexture: previewTextureFromValues(values), physicalTexture: physicalTextureFromValues(values), specular: specularParametersFromValues(values, existing?.specular), ...surfaceTextures, params: spectrumParamsFromValues(values, existing?.params) }
    }
  }
  if (kind === 'thermal') {
    return { list: state.config.thermals, item: { name, label, sunlitTemperature: Number(values.sunlitTemperature), shadedTemperature: Number(values.shadedTemperature) } }
  }
  if (kind === 'canopy') {
    return {
      list: state.config.canopies,
      item: {
        name, label,
        structureType: ['canopy', 'rigid', 'fire', 'fog'].includes(values.structureType) ? values.structureType : 'canopy',
        lai: Number(values.lai), density: Number(values.density), hc: Number(values.hc), G: Number(values.G),
        LIDFa: Number(values.LIDFa), LIDFb: Number(values.LIDFb), hspot: Number(values.hspot), leafwidth: Number(values.leafwidth),
        extinction: Math.max(0, Number(values.extinction) || 0),
        scatteringAlbedo: Math.max(0, Math.min(1, Number(values.scatteringAlbedo) || 0)),
        asymmetry: Math.max(-0.99, Math.min(0.99, Number(values.asymmetry) || 0)),
        emissionScale: Math.max(0, Number(values.emissionScale) || 0),
        fixedTemperature: Math.max(0, Number(values.fixedTemperature) || 0)
      }
    }
  }
  return { list: state.config.materials, item: { ...physicalFromForm({ ...values, materialName: name }, values.physicalType), label } }
}

// 作用：预设对话框提交：把表单转成预设条目（presetFromForm），写入工程配置并保存。
// 步骤：1) 读表单，光谱预设把多波段的反射/透射输入合并成逗号字符串，file 模型必须已导入波谱；
// 2) 名称在同一列表内查重（编辑时排除自身）；3) 先改内存（编辑替换 / 新增追加）再 saveXml；
// 4) 保存失败则回滚（编辑恢复旧条目，新增则弹出刚加入的条目）。
// saveXml 返回 null 表示保存已被取代（工程已切换或有更新的编辑），此时直接返回、不刷新也不回滚。
async function savePreset(event) {
  event.preventDefault()
  const submit = $('#presetForm button[type="submit"]')
  const formData = new FormData($('#presetForm'))
  const values = Object.fromEntries(formData.entries())
  values.presetKind = state.editingPreset?.kind || values.presetKind
  if (values.presetKind === 'spectrum') {
    values.reflectance = formData.getAll('reflectanceValue').map(String).join(',')
    values.transmittance = formData.getAll('transmittanceValue').map(String).join(',')
    values.classTexture = classTextureFromForm(formData)
    if (values.spectralModel === 'file' && !values.fileName) { toast('请先导入波谱文件', '需要包含波长行和反射率行', 'error'); return }
  }
  const { list, item } = presetFromForm(values)
  const editingIndex = state.editingPreset?.index ?? -1
  if (list.some((entry, index) => index !== editingIndex && entry.name === item.name)) { toast('名称已存在', `请更换内部名称：${item.name}`, 'error'); return }
  const previous = editingIndex >= 0 ? list[editingIndex] : null
  let applied = false
  try {
    submit.disabled = true
    ensureProjectState()
    if (editingIndex >= 0) list[editingIndex] = item
    else list.push(item)
    applied = true
    state.project.configuration = state.config
    markXmlDirty()
    $('#unsavedMark').hidden = false
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('材质属性保存失败')
    hidePresetDialog(); refreshFromState(false); await loadActualScene(state.config); renderInspector()
    addLog(`${editingIndex >= 0 ? '材质属性已修改' : '材质预设已加入工程'}：${item.name}`, 'success')
  } catch (error) {
    if (applied) {
      if (editingIndex >= 0) list[editingIndex] = previous
      else list.pop()
    }
    toast(editingIndex >= 0 ? '修改材质属性失败' : '新增材质属性失败', error.message, 'error')
  } finally { submit.disabled = false }
}

// 作用：材质对话框提交（导入 OBJ 的最后一步）：复制 OBJ、生成实例分布与位置文件、
// 按需新建光谱/热学/物理预设、建立每个网格组的属性绑定、写入对象条目并保存工程。
// stage 变量记录当前阶段，出错时拼进提示信息。出错回滚：对象列表还原（新增则移除，
// 替换则恢复旧条目）、预设列表还原为快照；已复制到工程的 OBJ 与位置文件不会删除
// （服务端没有删除工程内文件的接口，残留文件只占磁盘，不影响工程）。
// 步骤见下方各“步骤 n”。
async function saveImportedObject(event) {
  event.preventDefault()
  const pending = state.pendingObject
  if (!pending) return
  const submit = $('#materialForm button[type="submit"]')
  let stage = '读取 OBJ 属性'
  let previousObject = null
  let presetSnapshot = null
  let objectIndex = -1
  let insertedObject = false
  try {
    submit.disabled = true
    ensureProjectState()
    // 步骤 1：读取表单；多波段反射/透射输入合并为逗号字符串；file 模型必须已导入波谱。
    const formData = new FormData($('#materialForm'))
    const values = Object.fromEntries(formData.entries())
    values.reflectance = formData.getAll('reflectanceValue').map(String).join(',')
    values.transmittance = formData.getAll('transmittanceValue').map(String).join(',')
    if (values.spectralModel === 'file' && !values.fileName) throw new Error('请先导入波谱文件')
    // 步骤 2：把 OBJ 复制进工程。车辆自动绑定且表格行数与原网格数不同（组已按法向拆分）时，
    // 导入拆分后的 OBJ 内容，否则导入原始内容。
    stage = '复制 OBJ 到工程'
    // FacetEB vehicle: split the glass group (front / side / rear) and the bonnet by facet orientation.
    const vehicleAuto = vehicleAutoBindEnabled()
    const rowNames = $$('#meshMaterialRows tr').map((row) => row.dataset.meshName)
    const vehicleSplit = vehicleAuto && rowNames.length !== pending.meshNames.length ? splitVehicleObj(pending.source.content) : null
    const imported = await api.importObj({ name: `${uniqueName(values.objectName, 'object')}.obj`, content: vehicleSplit ? vehicleSplit.content : pending.source.content })
    // 步骤 3：按物体类型与尺寸生成缺省实例分布（defaultDistribution），展开为实例列表。
    stage = '生成实例分布'
    const distribution = defaultDistribution({ type: values.objectType, dimensions: pending.dimensions })
    const instances = generateDistribution(distribution)
    // 步骤 4：把实例位置写入 OBJ 对应的 positions 文件。
    stage = '保存实例位置文件'
    const savedDistribution = await api.saveDistribution({ path: imported.positionPath, name: values.objectName, instances })
    // 步骤 5：由表单构造新的光谱/热学/物理预设，并记录预设列表快照供失败回滚；
    // 只有某一行选了“新建”时才把对应预设写入工程配置（upsertByName）。
    stage = '校验 Mesh 属性映射'
    const spectrum = { name: values.spectralName, model: values.spectralModel, fileName: values.fileName || '', reflectance: values.reflectance, transmittance: values.transmittance, refTir: Number(values.refTir), refTirWet: optionalTirReflectance(values.refTirWet), tauTir: Number(values.tauTir), openness: Number(values.openness) || 0, previewTexture: previewTextureFromValues(values), physicalTexture: physicalTextureFromValues(values), specular: specularParametersFromValues(values, state.config.spectra.find(entry => entry.name === values.spectralName)?.specular), params: { Cab: values.paramCab, Cw: values.paramCw, Cdm: values.paramCdm, Cs: values.paramCs, N: values.paramN, SMC: values.paramSMC, BSMBrightness: values.paramBSMBrightness, BSMlat: values.paramBSMlat, BSMlon: values.paramBSMlon } }
    const thermal = { name: values.thermalName, sunlitTemperature: Number(values.sunlitTemperature), shadedTemperature: Number(values.shadedTemperature) }
    const spectralSelections = $$('.mesh-spectrum'), thermalSelections = $$('.mesh-thermal')
    const physicalSelections = $$('.mesh-physical'), canopySelections = $$('.mesh-canopy')
    const physicalType = physicalTypeForObject(values.objectType)
    const physical = state.config.materials.find((item) => item.name === values.materialPreset) || physicalFromForm(values, physicalType)
    presetSnapshot = { spectra: [...state.config.spectra], thermals: [...state.config.thermals], materials: [...state.config.materials] }
    if (spectralSelections.some((select) => select.value === '__new__')) upsertByName(state.config.spectra, spectrum)
    if (thermalSelections.some((select) => select.value === '__new__')) upsertByName(state.config.thermals, thermal)
    if (values.materialPreset === '__new__' || physicalSelections.some((select) => select.value === '__new__')) upsertByName(state.config.materials, physical)
    const defaultCanopy = values.objectType === 'Fire' ? 'fire_medium' : values.objectType === 'Fog' ? 'fog_medium' : values.objectType === 'Vegetation' ? 'tree_leaf_canopy' : 'rigid_body'
    if (vehicleAuto) {
      // Vehicle parts: add the preset families the table refers to and a body material that carries this car's cabin parameters.
      ensureVehiclePresets()
      upsertByName(state.config.materials, vehicleBodyMaterial(values, `${values.objectName}_body`))
    }
    // 步骤 6：逐网格组生成绑定：下拉框选“新建”则换成刚建的预设名；车辆自动绑定时，规则为
    // paint/other/tarp 且仍选默认车身材料的行绑定到 `${对象名}_body`（带本车座舱参数的车身材料）。
    const meshes = (vehicleAuto ? rowNames : pending.meshNames).map((name, index) => ({
      name,
      spectralName: spectralSelections[index].value === '__new__' ? spectrum.name : spectralSelections[index].value,
      thermalName: thermalSelections[index].value === '__new__' ? thermal.name : thermalSelections[index].value,
      materialName: vehicleAuto && ['paint', 'other', 'tarp'].includes(vehicleMeshBinding(name).rule) && physicalSelections[index].value === (values.materialPreset === '__new__' ? physical.name : values.materialPreset)
        ? `${values.objectName}_body`
        : physicalSelections[index].value === '__new__' ? physical.name : physicalSelections[index].value,
      canopyName: canopySelections[index]?.value || defaultCanopy,
      specularEnabled: meshSpecularFromRow($('#meshMaterialRows').rows[index])
    }))
    // 步骤 7：校验：物化、光谱、热学、冠层引用必须存在，且各网格物化预设须与物体类型匹配。
    if (!physical?.name || !state.config.materials.some((entry) => entry.name === physical.name)) throw new Error('物化属性未定义')
    for (const mesh of meshes) {
      if (!state.config.spectra.some((entry) => entry.name === mesh.spectralName)) throw new Error('光谱属性不存在：' + mesh.spectralName)
      if (!state.config.thermals.some((entry) => entry.name === mesh.thermalName)) throw new Error('温度属性不存在：' + mesh.thermalName)
      const meshPhysical = state.config.materials.find((entry) => entry.name === mesh.materialName)
      if (!materialMatchesObjectType(meshPhysical, values.objectType)) throw new Error('Mesh 物化属性无效：' + mesh.materialName)
      if (!state.config.canopies.some((entry) => entry.name === mesh.canopyName)) throw new Error('Mesh 结构属性不存在：' + mesh.canopyName)
    }
    // 步骤 8：组装对象条目（人/车/船附带缺省移动参数）；同名对象则替换并记下旧条目供回滚，
    // 否则追加；随后同步对象计数与名称表。
    const item = { name: values.objectName, ...objectTypeFields(values.objectType), materialName: meshes[0]?.materialName || physical.name, canopyName: meshes[0]?.canopyName || defaultCanopy, fileName: imported.path, positionFile: savedDistribution.path, shape: pending.shape || 'cube', dimensions: pending.dimensions, meshes, distribution, instanceCount: instances.length, ...(isMobileAgentType(values.objectType) ? { movement: defaultMovement(values.objectType) } : {}), ...(pending.generation ? { generation: pending.generation } : {}) }
    const existing = state.config.objects.items.findIndex((entry) => entry.name === item.name)
    objectIndex = existing >= 0 ? existing : state.config.objects.items.length
    if (existing >= 0) {
      previousObject = state.config.objects.items[existing]
      state.config.objects.items[existing] = item
    } else {
      state.config.objects.items.push(item)
      insertedObject = true
    }
    const itemIndex = objectIndex
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    state.project.configuration = state.config
    markXmlDirty()
    // 步骤 9：保存工程；saveXml 返回 null（被取代）则直接退出；失败则抛出最近一次保存错误。
    stage = '保存 project.json'
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw state.lastSaveError || new Error('工程配置保存失败')

    // 步骤 10：释放待导入对象，关闭对话框，重载三维场景并选中新对象，打开实例分布对话框继续设置。
    stage = '刷新三维场景'
    disposeObject(pending.object); state.pendingObject = null
    hideMaterialDialog(false); refreshFromState(false); await loadActualScene(state.config)
    state.panel = 'objects'; state.selectedObjectIndex = itemIndex
    $$('.tree-item').forEach((button) => button.classList.toggle('active', button.dataset.panel === 'objects'))
    renderInspector(); await showObjectDialog(itemIndex)
    addLog(`OBJ 属性已加入工程：${imported.path}`, 'success'); toast('OBJ 属性配置完成', `${item.name} · 请继续设置实例分布`)
  } catch (error) {
    if (objectIndex >= 0) {
      if (insertedObject) state.config.objects.items.splice(objectIndex, 1)
      else if (previousObject) state.config.objects.items[objectIndex] = previousObject
      state.config.objects.count = state.config.objects.items.length
      state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
      if (state.project) state.project.configuration = state.config
    }
    if (presetSnapshot) {
      state.config.spectra = presetSnapshot.spectra
      state.config.thermals = presetSnapshot.thermals
      state.config.materials = presetSnapshot.materials
    }
    const message = `${stage}失败：${error.message}`
    toast('OBJ 导入失败', message, 'error')
    addLog(message, 'error')
  }
  finally { submit.disabled = false }
}


// 作用：生成属性绑定下拉框的 option 列表。选中项优先 selected，其次 fallback，
// 再次取第一项；列表为空时给出“无可用属性”占位。
function bindingOptionList(items, selected, fallback) {
  if (!items.length) return '<option value="">无可用属性</option>'
  const selectedName = items.some((item) => item.name === selected) ? selected : (items.some((item) => item.name === fallback) ? fallback : items[0].name)
  return items.map((item) => `<option value="${escapeHtml(item.name)}" ${item.name === selectedName ? 'selected' : ''}>${escapeHtml(item.label || item.name)} · ${escapeHtml(item.name)}</option>`).join('')
}

// 作用：在对象属性对话框中显示当前所选物化材质的参数摘要（找不到时提示未定义）。
function renderObjectAttributeSummary() {
  const form = $('#objectAttributeForm')
  const material = state.config?.materials?.find((item) => item.name === form.elements.materialName.value)
  const summary = material ? physicalValues(material).map(([key, value]) => `${key}=${value}`).join('，') : '未找到物化属性定义'
  $('#objectAttributeSummary').textContent = material ? `${material.label || material.name} · ${summary}` : summary
}

// 作用：重建对象属性对话框的物化/冠层下拉框与逐网格组绑定表。
// useCurrent：保留对话框当前选择（否则取对象已保存的绑定）；
// resetBindings：对象类型改变时，全部回到该类型的推荐预设（与材质对话框的推荐表一致）。
// 物化预设只列出与对象类型匹配者（materialMatchesObjectType）。
function renderObjectAttributeOptions(useCurrent = false, resetBindings = false) {
  const item = state.config?.objects?.items?.[state.editingAttributeIndex]
  if (!item) return
  const form = $('#objectAttributeForm')
  const type = form.elements.objectType.value
  const materials = state.config.materials.filter((entry) => materialMatchesObjectType(entry, type))
  const preferredMaterial = { SolarPanel: 'pv_panel', Vegetation: 'leaf_c3', Fire: 'leaf_c3', Fog: 'leaf_c3', Soil: 'soilset', Building: 'building_surface', Human: 'soil_dry', Vehicle: 'vehicle_material', Ship: 'ship_material', Other: 'soilset', Water: 'water_set' }[type]
  const currentMaterial = resetBindings ? preferredMaterial : useCurrent ? form.elements.materialName.value : item.materialName
  form.elements.materialName.innerHTML = bindingOptionList(materials, currentMaterial, preferredMaterial)
  const preferredCanopy = type === 'Fire' ? 'fire_medium' : type === 'Fog' ? 'fog_medium' : type === 'Vegetation' ? 'canopy_default' : 'rigid_body'
  const currentCanopy = resetBindings ? preferredCanopy : item.canopyName
  form.elements.canopyName.innerHTML = bindingOptionList(state.config.canopies || [], currentCanopy, preferredCanopy)
  const preferredSpectrum = { SolarPanel: 'pv_opaque', Vegetation: 'green_leaf', Fire: 'fire_medium', Fog: 'fog_medium', Soil: 'soil', Building: 'concrete', Human: 'human_surface', Vehicle: 'vehicle_surface', Ship: 'ship_surface', Other: 'concrete', Water: 'water_surface' }[type]
  const preferredThermal = { SolarPanel: 'pv_temperature', Vegetation: 'vegetation_temperature', Fire: 'fire_temperature', Fog: 'fog_temperature', Soil: 'soil_temperature', Building: 'building_temperature', Human: 'human_temperature', Vehicle: 'vehicle_temperature', Ship: 'ship_temperature', Other: 'building_temperature', Water: 'water_temperature' }[type]

  const currentMeshes = useCurrent
    ? $$('#objectAttributeMeshRows tr').map((row) => ({
      name: row.dataset.meshName,
      spectralName: resetBindings ? preferredSpectrum : row.querySelector('.attribute-spectrum').value,
      thermalName: resetBindings ? preferredThermal : row.querySelector('.attribute-thermal').value,
      materialName: resetBindings ? preferredMaterial : row.querySelector('.attribute-physical').value,
      canopyName: resetBindings ? preferredCanopy : row.querySelector('.attribute-canopy').value,
      specularEnabled: meshSpecularFromRow(row)
    }))
    : (item.meshes?.length ? item.meshes : [{ ...item, name: item.name }])
  $('#objectAttributeMeshRows').innerHTML = currentMeshes.map((mesh) => `<tr data-mesh-name="${escapeHtml(mesh.name)}"><td title="${escapeHtml(mesh.name)}">${escapeHtml(mesh.name)}</td><td><select class="select attribute-spectrum">${bindingOptionList(state.config.spectra, mesh.spectralName, preferredSpectrum)}</select></td><td><select class="select attribute-thermal">${bindingOptionList(state.config.thermals, mesh.thermalName, preferredThermal)}</select></td><td><select class="select attribute-physical">${bindingOptionList(materials, mesh.materialName || item.materialName, preferredMaterial)}</select></td><td><select class="select attribute-canopy">${bindingOptionList(state.config.canopies || [], mesh.canopyName || item.canopyName, preferredCanopy)}</select></td><td>${meshSpecularFields(meshSpecular(mesh, state.config.spectra, state.config.control, item.spectralName))}</td></tr>`).join('')
  renderObjectAttributeSummary()
}

// 作用：打开第 index 个对象的“OBJ 属性”对话框并回填名称、类型、路径、叶密度来源。
function showObjectAttributeDialog(index) {
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  state.editingAttributeIndex = index
  state.selectedObjectIndex = index
  const form = $('#objectAttributeForm')
  form.reset()
  form.elements.objectName.value = item.name
  form.elements.objectType.value = objectClassification(item)
  form.elements.objectPath.value = item.fileName || ''
  form.elements.leafDensitySource.value = item.leafDensitySource === 'geometry' ? 'geometry' : 'parameter'
  renderObjectAttributeOptions(false)
  $$('.object-card').forEach((card) => card.classList.toggle('active', Number(card.dataset.index) === index))
  $('#objectAttributeDialog').hidden = false
}

// 作用：关闭对象属性对话框并清除编辑索引。
function hideObjectAttributeDialog() {
  $('#objectAttributeDialog').hidden = true
  state.editingAttributeIndex = -1
}

// 作用：保存对象属性对话框：更新对象类型与每个网格组的光谱/热学/物化/冠层绑定。
// 步骤：1) 读表单；2) 校验：火/雾只能用于 VoxelRT/VoxelEB，物化须匹配类型，火焰/雾对象
// （含每个网格组）必须绑定相应 structureType 的冠层，引用的预设须存在；
// 3) 应用：火/雾写 medium 标记（仅辐射传输），人/车/船保留或补默认移动参数（mode 为 random），
// 其余类型删除 medium/movement；叶密度来源仅 geometry 时写入；
// 4) 保存工程，失败则用保存前的快照恢复该对象；null（被取代）直接退出。
async function saveObjectAttributes(event) {
  event.preventDefault()
  const index = state.editingAttributeIndex
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  const form = $('#objectAttributeForm')
  const submit = form.querySelector('button[type="submit"]')
  const type = form.elements.objectType.value
  const materialName = form.elements.materialName.value
  const canopyName = form.elements.canopyName.value
  const material = state.config.materials.find((entry) => entry.name === materialName)
  const canopy = state.config.canopies.find((entry) => entry.name === canopyName)
  const meshes = $$('#objectAttributeMeshRows tr').map((row) => ({
    name: row.dataset.meshName,
    spectralName: row.querySelector('.attribute-spectrum').value,
    thermalName: row.querySelector('.attribute-thermal').value,
    materialName: row.querySelector('.attribute-physical').value,
    canopyName: row.querySelector('.attribute-canopy').value,
    specularEnabled: meshSpecularFromRow(row)
  }))
  try {
    submit.disabled = true
    if ((type === 'Fire' || type === 'Fog') && state.mode !== 'eVoxelRT' && state.mode !== 'eVoxelEB') throw new Error('参与介质当前只支持 VoxelRT 或 VoxelEB')
    if (!materialMatchesObjectType(material, type)) throw new Error('所选物化属性与对象类型不匹配')
    if (!canopy) throw new Error('所选结构参数不存在')
    if (type === 'Fire' && canopy.structureType !== 'fire') throw new Error('火焰对象必须绑定火焰结构参数')
    if (type === 'Fog' && canopy.structureType !== 'fog') throw new Error('雾对象必须绑定雾结构参数')
    if (!meshes.length) throw new Error('OBJ 没有可配置的 Mesh')
    for (const mesh of meshes) {
      if (!state.config.spectra.some((entry) => entry.name === mesh.spectralName)) throw new Error('光谱属性不存在：' + mesh.spectralName)
      if (!state.config.thermals.some((entry) => entry.name === mesh.thermalName)) throw new Error('温度属性不存在：' + mesh.thermalName)
      const meshMaterial = state.config.materials.find((entry) => entry.name === mesh.materialName)
      if (!materialMatchesObjectType(meshMaterial, type)) throw new Error(`Mesh“${mesh.name}”物化属性无效`)
      const meshCanopy = state.config.canopies.find((entry) => entry.name === mesh.canopyName)
      if (!meshCanopy) throw new Error(`Mesh“${mesh.name}”结构属性不存在`)
      if (type === 'Fire' && meshCanopy.structureType !== 'fire') throw new Error(`Mesh“${mesh.name}”必须绑定火焰结构属性`)
      if (type === 'Fog' && meshCanopy.structureType !== 'fog') throw new Error(`Mesh“${mesh.name}”必须绑定雾结构属性`)
    }
    const previous = { type: item.type, classification: item.classification, materialName: item.materialName, canopyName: item.canopyName, leafDensitySource: item.leafDensitySource, meshes: item.meshes, medium: item.medium, movement: item.movement }
    Object.assign(item, objectTypeFields(type), { materialName, canopyName, meshes })
    if (form.elements.leafDensitySource.value === 'geometry') item.leafDensitySource = 'geometry'
    else delete item.leafDensitySource
    if (type === 'Fire' || type === 'Fog') item.medium = { kind: type === 'Fog' ? 'fog' : 'fire', radiationOnly: true, coordinateOrder: 'XYZ' }
    else delete item.medium
    if (isMobileAgentType(type)) item.movement = { ...defaultMovement(type), ...(item.movement || {}), enabled: Boolean(item.movement?.enabled), mode: 'random' }
    else delete item.movement
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) { Object.assign(item, previous); throw new Error('工程配置保存失败') }
    hideObjectAttributeDialog()
    await loadActualScene(state.config)
    refreshFromState(false); renderInspector()
    addLog(`OBJ 属性已更新：${item.name}`, 'success')
    toast('OBJ 属性已更新', item.name)
  } catch (error) { toast('OBJ 属性保存失败', error.message, 'error') }
  finally { submit.disabled = false }
}

// 作用：随机/规则网格分布下，让“数量”和“密度”两个输入框互相保持一致。
// 面积取分布范围 (maxX−minX)×(maxY−minY)；source 为 density 时由密度算数量，否则由数量算密度，
// 并把依据（basis）同步到表单。其他分布模式不处理。
function syncDistributionPopulation(form, source = 'count') {
  const mode = form.elements.distributionMode.value
  if (mode !== 'random' && mode !== 'grid') return
  const minX = finiteNumber(form.elements.distributionMinX.value, 0)
  const maxX = finiteNumber(form.elements.distributionMaxX.value, 1)
  const minY = finiteNumber(form.elements.distributionMinY.value, 0)
  const maxY = finiteNumber(form.elements.distributionMaxY.value, 1)
  const area = distributionArea({ minX, maxX, minY, maxY })
  const population = source === 'density'
    ? populationFromValues(form.elements.distributionCount.value, form.elements.distributionDensity.value, area, 'density')
    : populationFromValues(form.elements.distributionCount.value, form.elements.distributionDensity.value, area, 'count')
  form.elements.distributionBasis.value = source === 'density' ? 'density' : 'count'
  form.elements.distributionCount.value = population.count
  form.elements.distributionDensity.value = distributionDensityInputValue(population.density)
}

// 作用：按分布模式刷新分布表单的可用状态与说明：仅随机/规则网格使用数量与密度，
// 依据为密度时数量只读、为数量时密度只读，手动模式显示坐标文本框。
// 状态行显示实例数估算（密度单位为 个/公顷，面积换算为公顷）。
function updateDistributionFormState(form) {
  const mode = form.elements.distributionMode.value
  const basis = form.elements.distributionBasis.value
  const usesPopulation = mode === 'random' || mode === 'grid'
  form.elements.distributionBasis.disabled = !usesPopulation
  form.elements.distributionCount.disabled = !usesPopulation
  form.elements.distributionDensity.disabled = !usesPopulation
  form.elements.distributionCount.readOnly = usesPopulation && basis === 'density'
  form.elements.distributionDensity.readOnly = usesPopulation && basis === 'count'
  form.elements.distributionPoints.closest('.distribution-points').hidden = mode !== 'manual'

  let detail
  if (mode === 'manual') {
    const count = String(form.elements.distributionPoints.value || '').split(/\r?\n/).map((line) => line.trim()).filter(Boolean).length
    detail = `自定义坐标 · ${count} 个实例`
  } else if (mode === 'single') detail = '单实例 · 位于当前分布范围中心'
  else {
    const distribution = distributionFromForm(form)
    distribution.basis = basis
    distribution.count = finiteNumber(form.elements.distributionCount.value, distribution.count)
    distribution.density = finiteNumber(form.elements.distributionDensity.value, distribution.density)
    distribution.densityUnit = 'hectare'
    const count = distributionTargetCount(distribution)
    const areaHectares = distributionArea(distribution) / SQUARE_METERS_PER_HECTARE
    detail = `${mode === 'grid' ? '规则网格' : '纯随机'} · ${areaHectares.toFixed(2)} 公顷 × ${distribution.density} 个/公顷 · 将生成 ${count} 个实例`
  }
  $('#objectDistributionStatus').textContent = `${detail}；保存后覆盖该 OBJ 自己的位置文件。`
}

// 作用：打开第 index 个对象的“实例分布与移动”对话框。若对象没有 distribution 但有位置文件，
// 则读取该文本作为手动坐标；回填分布表单与移动表单。
async function showObjectDialog(index) {
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  state.editingObjectIndex = index
  const form = $('#objectForm')
  form.reset()
  form.elements.objectName.value = item.name
  form.elements.objectType.value = objectClassification(item)
  form.elements.objectPath.value = item.fileName || ''
  let distribution = item.distribution
  if (!distribution && item.positionFile) {
    try {
      const result = await api.readText(item.positionFile)
      distribution = { ...defaultDistribution(item), mode: 'manual', points: result.content.trim() }
    } catch (error) { toast('位置文件读取失败', error.message, 'error') }
  }
  state.selectedObjectIndex = index
  setDistributionForm(form, distribution || defaultDistribution(item))
  setMovementForm(form, item.movement, item.type)
  updateDistributionFormState(form)
  updateMovementFormState(form)
  renderInspector()
  $('#objectDialog').hidden = false
}

// 作用：从 TXT 导入自定义实例坐标（上限 5 MB）；缺省高度取表单里的 distributionZ，
// 导入后切到手动模式，保存时才写入 positions 文件。
async function importDistributionTxt(file) {
  if (!file) return
  try {
    if (file.size > 5 * 1024 * 1024) throw new Error('TXT 文件不能超过 5 MB')
    const form = $('#objectForm')
    const defaultZ = finiteNumber(form.elements.distributionZ.value, 0)
    const points = parseDistributionPointsText(await file.text(), defaultZ)
    form.elements.distributionMode.value = 'manual'
    form.elements.distributionPoints.value = distributionPointsText(points)
    updateDistributionFormState(form)
    $('#objectDistributionStatus').textContent = `已导入 ${file.name} · ${points.length} 个实例；保存后写入 positions 文件。`
    toast('自定义分布已导入', `${file.name} · ${points.length} 个实例`)
  } catch (error) {
    toast('TXT 导入失败', error.message, 'error')
  } finally {
    $('#distributionTxtFile').value = ''
  }
}

// 作用：关闭实例分布对话框并清除编辑索引。
function hideObjectDialog() {
  $('#objectDialog').hidden = true
  state.editingObjectIndex = -1
}

// 作用：保存“实例分布与移动”对话框：改名、重新生成实例并写位置文件、更新分布与移动参数。
// 步骤：1) 校验名称非空且不与其他对象重名；2) 由表单生成分布（要求密度有效）并展开实例，
// 写入 positions 文件；3) 更新对象条目与计数，保存工程；
// 4) 出错时用进入时的浅拷贝快照恢复对象，并重算对象名列表（保证 names 与 item.name 一致）。
async function saveSceneObject(event) {
  event.preventDefault()
  const index = state.editingObjectIndex
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  const itemSnapshot = { ...item }
  const submit = $('#objectForm button[type="submit"]')
  try {
    submit.disabled = true
    const form = $('#objectForm')
    const nextName = String(form.elements.objectName.value).trim()
    if (!nextName) throw new Error('对象名称不能为空')
    if (state.config.objects.items.some((entry, itemIndex) => itemIndex !== index && entry.name === nextName)) throw new Error(`对象名称已存在：${nextName}`)
    const distribution = distributionFromForm(form, { requireValidDensity: true })
    const instances = generateDistribution(distribution)
    const saved = await api.saveDistribution({ path: item.positionFile, name: nextName, instances })
    item.name = nextName
    item.positionFile = saved.path
    item.distribution = distribution
    const movement = movementFromForm(form)
    if (movement) item.movement = movement
    else delete item.movement
    item.instanceCount = instances.length
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    hideObjectDialog()
    await loadActualScene(state.config)
    refreshFromState(false); renderInspector()
    addLog(`对象分布已更新：${item.name} · ${instances.length} 个实例`, 'success')
    toast('对象已更新', `${item.name} · ${instances.length} 个实例`)
  } catch (error) {
    for (const key of Object.keys(item)) if (!(key in itemSnapshot)) delete item[key]
    Object.assign(item, itemSnapshot)
    // 回滚 item 后需要同步重算对象名列表，否则 objects.names 仍保留失败前的 nextName，与 item.name 不一致
    if (state.config?.objects?.items) {
      state.config.objects.count = state.config.objects.items.length
      state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    }
    toast('对象修改失败', error.message, 'error')
  }
  finally { submit.disabled = false }
}

// 作用：为第 index 个对象生成一个贴合其网格的篷布罩对象并插入其后。
// 步骤：1) 补齐内置预设（tarp_olive_canvas 光谱、tarp_temperature 热学、tarp_fabric 物化、rigid_body 冠层）；
// 2) 生成唯一名称 `${目标名}_tarp[_n]`；3) 调用 api.generateTarpCover 生成罩面 OBJ；
// 4) 新对象沿用目标的位置文件、分布、移动参数与地面投影（groundFootprint），并标记 tarpCoverOf；
// 5) 保存工程并刷新场景，失败则删除新对象并恢复预设列表快照。
// 说明：FacetEB 求解薄膜热平衡；VoxelEB 不支持薄膜，故沿用目标物化属性。
async function addTarpCover(index) {
  const target = state.config?.objects?.items?.[index]
  if (!target?.fileName) { toast('无法生成篷布罩', '该对象没有 OBJ 网格', 'error'); return }
  const c = state.config
  const presets = createMaterialPresets()
  const snapshot = structuredClone({ spectra: c.spectra, thermals: c.thermals, materials: c.materials, canopies: c.canopies })
  for (const [kind, presetName] of [['spectra', 'tarp_olive_canvas'], ['thermals', 'tarp_temperature'], ['materials', 'tarp_fabric'], ['canopies', 'rigid_body']]) {
    if (!c[kind].some((entry) => entry.name === presetName)) c[kind].push(structuredClone(presets[kind].find((entry) => entry.name === presetName)))
  }
  const names = new Set(c.objects.items.map((entry) => entry.name))
  const base = `${uniqueName(target.name, 'object')}_tarp`
  let name = base
  for (let suffix = 2; names.has(name); suffix += 1) name = `${base}_${suffix}`
  // FacetEB solves membranes; VoxelEB rejects them, so keep the target's solid material there.
  const materialName = state.mode === 'eVoxelEB' ? (target.materialName || 'building_surface') : 'tarp_fabric'
  try {
    const cover = await api.generateTarpCover({ fileName: target.fileName, name })
    const mesh = { name: 'tarp_cover', spectralName: 'tarp_olive_canvas', thermalName: 'tarp_temperature', materialName, canopyName: 'rigid_body' }
    c.objects.items.splice(index + 1, 0, {
      name, label: `${target.label || target.name} 篷布罩`, type: ['Vehicle', 'Human', 'Ship', 'Building'].includes(target.type) ? target.type : 'Building',
      ...(target.movement ? { movement: structuredClone(target.movement) } : {}),
      fileName: cover.relativePath, positionFile: target.positionFile, shape: 'cube', dimensions: cover.dimensions,
      spectralName: mesh.spectralName, thermalName: mesh.thermalName, materialName, canopyName: mesh.canopyName, meshes: [mesh],
      distribution: structuredClone(target.distribution || {}), instanceCount: target.instanceCount,
      groundFootprint: target.groundFootprint || cover.targetFootprint,
      tarpCoverOf: target.name
    })
    c.objects.count = c.objects.items.length
    c.objects.names = c.objects.items.map((entry) => entry.name)
    ensureProjectState().configuration = c
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    await loadActualScene(c)
    refreshFromState(false); renderInspector()
    addLog(`已为 ${target.name} 生成篷布罩 ${name}：${cover.triangles} 个三角面，网格 ${cover.cell.toFixed(3)} m`, 'success')
    if (state.mode === 'eVoxelEB') toast('篷布罩已生成', 'VoxelEB 不支持薄膜热平衡，已沿用目标物化属性；如需篷布热平衡请切换到 FacetEB 并改绑 tarp_fabric')
    else toast('篷布罩已生成', name)
  } catch (error) {
    const at = c.objects.items.findIndex((entry) => entry.name === name)
    if (at >= 0) c.objects.items.splice(at, 1)
    c.objects.count = c.objects.items.length
    c.objects.names = c.objects.items.map((entry) => entry.name)
    Object.assign(c, snapshot)
    toast('生成篷布罩失败', error.message, 'error')
  }
}

// 作用：从场景删除第 index 个对象并保存工程；同步修正选中索引与计数。
// 保存失败则把对象插回原位并恢复选中项。注意：不删除磁盘上的 OBJ / 位置文件——
//   服务端没有删除工程内文件的接口，且位置文件/OBJ 可能仍被旧版工程或其他对象引用，
//   所以只从配置中移除条目，文件留在工程目录（已知遗留，不影响计算）。
async function deleteSceneObject(index) {
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  const previousSelectedIndex = state.selectedObjectIndex
  state.config.objects.items.splice(index, 1)
  if (state.selectedObjectIndex === index) state.selectedObjectIndex = -1
  else if (state.selectedObjectIndex > index) state.selectedObjectIndex -= 1
  state.config.objects.count = state.config.objects.items.length
  state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
  try {
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    await loadActualScene(state.config)
    refreshFromState(false); renderInspector()
    addLog(`已从场景删除对象：${item.name}`, 'success')
    toast('对象已删除', item.name)
  } catch (error) {
    state.config.objects.items.splice(index, 0, item)
    state.selectedObjectIndex = previousSelectedIndex
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    toast('删除对象失败', error.message, 'error')
  }
}

// 作用：点击“运行”的入口。依次：需有工程；防止重入（正在运行或正在准备）；
// 检查镜面设置能否运行；有未保存的修改先保存；运行预检（可能需用户确认）；最后启动引擎。
async function runSimulation() {
  if (!state.inputPath) { toast("无法运行", "请先打开场景工程的 project.json", "error"); return }
  if (state.running || state.runPending) return
  const specularError = meshSpecularRunError(state.mode, state.config)
  if (specularError) { toast('无法运行', specularError, 'error'); return }
  state.runPending = true
  try {
    if (state.xmlDirty && !(await saveXml())) return
    if (!(await runPreflightCheck())) return
    await startSimulation()
  } finally { state.runPending = false }
}

// 作用：把秒数格式化为中文时长：<90 秒显示秒，<90 分钟（5400 s）显示分钟，否则显示小时（一位小数）。
function formatDuration(seconds) {
  const value = Math.max(0, Math.round(Number(seconds) || 0))
  if (value < 90) return `${value} 秒`
  if (value < 5400) return `${Math.round(value / 60)} 分钟`
  return `${(value / 3600).toFixed(1)} 小时`
}

// Scale / memory / duration preflight before starting the engine. Risks
// (timeout, facet limit, memory) ask for confirmation; otherwise the summary
// only goes to the log. A failing preflight never blocks the run.
async function runPreflightCheck() {
  // 步骤 1：向服务端请求预检报告；预检接口不可用时只记警告并放行，不阻止运行。
  let report
  try {
    report = await api.preflight({ mode: state.mode, inputPath: state.inputPath })
  } catch (error) {
    addLog(`运行预检不可用：${error.message}`, 'warning')
    return true
  }
  // 步骤 2：汇总规模信息写入日志：面元数/上限与可见性栅格，体元网格与辐射场显存估计，
  // 时间节点、观测方向、波段数，预计耗时（及依据）与单次运行上限（工程设置或服务端默认）。
  const parts = []
  if (report.facet) parts.push(`面元约 ${report.facet.triangles.toLocaleString('zh-CN')} 个（上限 ${report.facet.limit.toLocaleString('zh-CN')}）`, `可见性栅格 ${report.facet.raster}²${report.facet.periodicTiles ? ` · 周期邻块 ${report.facet.periodicTiles}` : ''}`)
  if (report.voxel) parts.push(`体元网格 ${report.voxel.grid.join('×')}（${report.voxel.size} m）`, `辐射场显存上限估计 ${report.voxel.radianceMiB >= 1024 ? (report.voxel.radianceMiB / 1024).toFixed(1) + ' GB' : report.voxel.radianceMiB + ' MB'}`)
  if (report.nodes > 1) parts.push(`${report.nodes} 个时间节点`)
  if (report.angles > 1) parts.push(`${report.angles} 个观测方向`)
  parts.push(`${report.bands} 个波段`)
  parts.push(report.estimateSeconds != null ? `预计约 ${formatDuration(report.estimateSeconds)}（${report.estimateBasis}）` : '暂无历史耗时可参考')
  parts.push(`单次运行上限 ${formatDuration(report.timeoutSeconds)}${report.timeoutSource === 'project' ? '（工程设置）' : '（服务端默认）'}`)
  addLog(`运行预检 · ${parts.join(' · ')}`)
  for (const warning of report.warnings || []) addLog(`运行预检提示：${warning}`, 'warning')
  // 步骤 3：无风险提示则直接放行；有提示时弹确认。若预计耗时超限且有建议上限，
  // 给出三选一：调高上限后运行 / 按当前上限运行 / 取消。
  if (!report.warnings?.length) return true
  const riskText = `运行预检发现以下风险：\n\n${report.warnings.map((item) => '• ' + item).join('\n')}`
  const suggested = Number(report.suggestedTimeoutMinutes) || 0
  if (!suggested) return window.confirm(t(`${riskText}\n\n仍要开始运行吗？`))
  // 预计耗时超过上限：提供把工程上限调到建议值（预计 × 1.5，向上取整到分钟）后再运行。
  const choice = await chooseDialog({
    title: '预计耗时超过单次运行上限', message: riskText,
    actions: [
      { id: 'raise', label: `把上限调到 ${suggested} 分钟并运行`, primary: true },
      { id: 'run', label: '按当前上限运行' },
      { id: 'cancel', label: '取消' }
    ]
  })
  if (choice === 'cancel') return false
  if (choice === 'raise') {
    state.config.control.runTimeoutMinutes = suggested
    ensureProjectState().configuration = state.config
    syncProjectText()
    if (!(await saveXml())) return false
    addLog(`单次运行上限已调到 ${suggested} 分钟（工程设置）`, 'success')
    if ($('#runTimeoutMinutes')) renderInspector()
  }
  return true
}

// 小型多按钮确认框（window.confirm 只有确定/取消两项）。Esc 或点遮罩视为 'cancel'。
function chooseDialog({ title, message, actions }) {
  return new Promise((resolve) => {
    const backdrop = document.createElement('div')
    backdrop.className = 'drawer-backdrop'
    backdrop.innerHTML = `<section class="project-dialog choice-dialog" role="dialog" aria-modal="true" style="grid-template-rows:auto auto auto"><header style="min-height:56px"><h2>${escapeHtml(t(title))}</h2></header><div class="notice" style="margin:16px 18px;white-space:pre-wrap;font-size:12px;line-height:1.6">${escapeHtml(t(message))}</div><footer style="justify-content:flex-end;gap:8px;min-height:58px">${actions.map((action) => `<button class="button ${action.primary ? 'primary' : 'ghost'}" type="button" data-choice="${escapeHtml(action.id)}">${escapeHtml(t(action.label))}</button>`).join('')}</footer></section>`
    const finish = (value) => { document.removeEventListener('keydown', onKey, true); backdrop.remove(); resolve(value) }
    const onKey = (event) => { if (event.key === 'Escape') { event.preventDefault(); finish('cancel') } }
    backdrop.addEventListener('click', (event) => {
      const button = event.target.closest('[data-choice]')
      if (button) finish(button.dataset.choice)
      else if (event.target === backdrop) finish('cancel')
    })
    document.addEventListener('keydown', onKey, true)
    document.body.appendChild(backdrop)
    backdrop.querySelector('.button.primary')?.focus()
  })
}

// 作用：置初始进度状态并调用 api.run 启动引擎；真正的输出与结束由 handleSimulationEvent 处理。
// 启动失败时复位进度；引擎路径未被服务端白名单允许时给出专门提示。
async function startSimulation() {
  try {
    state.startedAt = Date.now(); state.progress = 1; state.progressStage = '正在启动计算引擎'; state.outputDir = state.config?.outDir || state.outputDir; state.runId = ''
    setRunning(true)
    const response = await api.run({ mode: state.mode, executable: state.executable, inputPath: state.inputPath })
    if (response.outputDir) state.outputDir = response.outputDir
    state.runId = response.runId || ''
    if (state.running) state.pid = response.pid
  } catch (error) {
    addLog(error.message, 'error')
    if (isEngineAllowlistError(error)) { addLog(ENGINE_REJECTED_HINT, 'warning'); toast('引擎路径未获允许', ENGINE_REJECTED_HINT, 'error') }
    else toast('无法启动模拟', error.message, 'error')
    state.progress = 0; state.progressStage = '启动失败'; setRunning(false); renderRunProgress(Date.now() - state.startedAt)
  }
}

// 作用：重置模拟环境：请求服务端终止残留的模拟进程，并把运行状态与进度归零。
async function resetSimulation() {
  const button = $('#resetBtn')
  try {
    button.disabled = true
    const result = await api.reset()
    state.pid = null
    state.startedAt = 0
    state.progress = 0
    state.progressStage = '已重置'
    setRunning(false)
    renderRunProgress(0)
    const detail = result.terminated?.length
      ? `已终止：${result.terminated.join('、')}`
      : '未发现残留的模拟进程'
    addLog(`模拟环境已重置；${detail}`, 'success')
    toast('重置完成', '现在可以重新运行模拟')
  } catch (error) {
    addLog(`重置失败：${error.message}`, 'error')
    toast('重置失败', error.message, 'error')
  } finally { button.disabled = false }
}

// 运行模式（state.mode 取值）到界面中文名的映射。
const simulationModeLabels = {
  eFacetRT: '面元辐射传输', eFacetEB: '面元能量平衡',
  eVoxelRT: '体元辐射传输', eVoxelEB: '体元辐射传输与能量平衡',
  eRaytracing: '简单光线追踪'
}

// 作用：能量平衡模式（eFacetEB / eVoxelEB）的运行日志中，写出所选温度方法：
// 土壤（缺省 2=温度廓线传导模型；0 瞬时能量平衡，1 热惯性动态模型）与
// 植被（缺省 0=Ball–Berry 经验法，否则 Farquhar 机制法）。其他模式返回空串。
function simulationMethodSummary() {
  if (!['eFacetEB', 'eVoxelEB'].includes(state.mode)) return ''
  const soil = t(['瞬时能量平衡', '热惯性动态模型', '温度廓线传导模型'][state.config?.control?.soilTemperatureMethod ?? 2])
  const vegetation = (state.config?.control?.vegetationTemperatureMethod ?? 0) === 0
    ? 'Ball–Berry 经验法' : 'Farquhar 机制法'
  return t(`温度方法：土壤=${soil}；植被=${vegetation}`)
}

// 作用：把引擎 stdout 的原始行整理成界面日志行。
// 规则：丢弃以 cube / OBJ placement 开头的行；PROGRESS n 阶段 → 更新进度条并输出“进度 n%”
// （常见英文阶段名翻译为中文）；PROCESS / RESULT / OBSERVATION / Angle Info / Time Info 转为
// 中文前缀；诊断行交给 recordRunQualityLine；其余原样保留。
function normalizedEngineLog(text) {
  const translations = [
    [/Preparing CPU raster buffers/i, '准备面元计算缓冲区'],
    [/Creating Vulkan device and GPU buffers/i, '初始化 GPU 与计算缓冲区'],
    [/(GPU )?visibility graph/i, '构建面元可见性图'],
    [/(CPU |GPU )?sunlight (visibility )?raster/i, '计算太阳光照比例'],
    [/(CPU |GPU )?(Jacobi )?radiosity solve/i, '计算辐射传输'],
    [/diffuse baseline and direct-light enhancement/i, '计算漫射基线与直射增强'],
    [/(Vulkan GPU|CPU) complete/i, '面元辐射传输计算完成']
  ]
  return String(text || '').replace(/\r/g, '').split('\n').filter(Boolean).flatMap((source) => {
    const line = source.trim()
    if (!line || /^(cube|OBJ placement)/i.test(line)) return []
    const progress = line.match(/^PROGRESS\s+([0-9]+)\s+(.+)$/i)
    if (progress) {
      let stage = progress[2]
      for (const [pattern, replacement] of translations) if (pattern.test(stage)) { stage = replacement; break }
      setSimulationProgress(progress[1], stage)
      return [`进度 ${progress[1]}% · ${stage}`]
    }
    const processResult = line.match(/^PROCESS\s+(.+)$/i)
    if (processResult) return [`过程结果 · ${processResult[1]}`]
    const primaryResult = line.match(/^RESULT\s+(.+)$/i)
    if (primaryResult) return [`主结果 · ${primaryResult[1]}`]
    const quality = recordRunQualityLine(line)
    if (quality !== null) return quality ? [quality] : []
    const observation = line.match(/^(?:OBSERVATION|Angle Info:)\s*(.*)$/i)
    if (observation) return [`观测角度 · ${observation[1].replaceAll('_', ' ').trim()}`]
    const time = line.match(/^Time Info:\s*t_?(.*)$/i)
    if (time) return time[1] ? [`时间节点 · ${time[1]}`] : []
    return [line]
  }).join('\n')
}

function emptyRunQuality() {
  return { facet: [], timing: [], voxel: [], scattering: [] }
}

// 识别的引擎诊断行（均记入 state.runQuality，部分同时返回日志文字）：
// FACET_CONVERGENCE（节点、是否收敛、迭代数、温度变化、残差 W/m² 与等效 K）、
// FACET_STEP（忽略）、FACET_TIMING（短波/耦合/出图耗时，单位 s）、
// ENERGY_BALANCE（VoxelEB 节点迭代与未达容差状态数，可附 Monin–Obukhov 诊断）、
// SHORTWAVE_SCATTERING（短波散射耗时与射线截断数）、SHORTWAVE_SPLIT（直散拆分）、
// VEHICLE_CABIN（车辆座舱温度）。行格式依赖引擎输出，字段顺序变化会导致解析失败。
// Engine diagnostics lines -> structured run quality + a readable log line.
// Returns null when the line is not a diagnostics line, '' to hide it.
function recordRunQualityLine(line) {
  const quality = state.runQuality || (state.runQuality = emptyRunQuality())
  let match = line.match(/^FACET_CONVERGENCE\s+(\d+)\s+([01])\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)/)
  if (match) {
    const item = { node: Number(match[1]), converged: match[2] === '1', iterations: Number(match[3]),
      temperatureDelta: Number(match[4]), residual: Number(match[5]), residualK: Number(match[6]) }
    quality.facet.push(item)
    return `收敛 · 节点 ${item.node + 1} ${item.converged ? '已收敛' : '未收敛'} · 迭代 ${item.iterations} · 最大残差 ${item.residual.toFixed(2)} W/m²（${item.residualK.toFixed(2)} K）`
  }
  if (/^FACET_STEP\t/.test(line)) return ''
  match = line.match(/^FACET_TIMING\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)/)
  if (match) {
    quality.timing.push({ node: Number(match[1]), shortwave: Number(match[2]), coupling: Number(match[3]), output: Number(match[4]) })
    return ''
  }
  match = line.match(/^ENERGY_BALANCE\s+node=(\d+)\s+iterations=(\d+).*?statesOutsideTolerance=(\d+)/)
  if (match) {
    const item = { node: Number(match[1]), iterations: Number(match[2]), outside: Number(match[3]) }
    // VoxelEB Monin–Obukhov 诊断（control.stabilityCorrection 开启时才有）。
    const stability = line.match(/\sstability=most\b.*?\szOverL=(\S+)\s+ustar=(\S+)\s+sceneH=(\S+)\s+aeroUpdates=(\d+)/)
    if (stability) Object.assign(item, { zOverL: Number(stability[1]), ustar: Number(stability[2]), sensibleHeat: Number(stability[3]), aeroUpdates: Number(stability[4]) })
    quality.voxel.push(item)
    return `能量平衡 · 节点 ${item.node + 1} · 迭代 ${item.iterations}${item.outside ? ` · ${item.outside} 个状态未达容差` : ' · 已收敛'}${stability ? ` · z/L ${item.zOverL.toFixed(2)} · u* ${item.ustar.toFixed(3)} m/s · 场景 H ${item.sensibleHeat.toFixed(1)} W/m²` : ''}`
  }
  match = line.match(/^SHORTWAVE_SCATTERING\s+node=(\d+).*?elapsedMs=([\d.]+)(?:.*?truncations=(\d+))?/)
  if (match) {
    // VoxelEB 太阳可见性射线（2026-11-23 起出场景为止追踪；达到几何步数预算的射线计数并给出剩余透射率上界）。
    const solar = line.match(/\ssolarTruncations=(\d+)\s+solarResidualSum=(\S+)\s+solarResidualMax=(\S+)/)
    const item = { node: Number(match[1]), milliseconds: Number(match[2]), truncations: Number(match[3] || 0),
      solarTruncations: solar ? Number(solar[1]) : 0, solarResidualSum: solar ? Number(solar[2]) || 0 : 0, solarResidualMax: solar ? Number(solar[3]) || 0 : 0 }
    quality.scattering.push(item)
    return `短波散射 · 节点 ${item.node + 1} · ${(item.milliseconds / 1000).toFixed(2)} s${item.truncations ? ` · 步数截断 ${item.truncations} 条射线` : ''}${item.solarTruncations ? ` · 太阳射线截断 ${item.solarTruncations} 条（剩余透射率最大 ${item.solarResidualMax.toPrecision(3)}）` : ''}`
  }
  match = line.match(/^SHORTWAVE_SPLIT\s+node=(\d+)\s+mode=(\w+)\s+source=(\w+).*?\skt=(\S+)\s+directFraction=(\S+)/)
  if (match) {
    const item = { node: Number(match[1]), mode: match[2], source: match[3], kt: Number(match[4]), directFraction: Number(match[5]) }
    ;(quality.split || (quality.split = [])).push(item)
    if (item.mode !== 'erbs') return ''
    const sourceLabel = item.source === 'measured' ? '实测列' : 'Erbs'
    return `短波直散 · 节点 ${item.node + 1} · 直射份额 ${item.directFraction.toFixed(3)}（${sourceLabel}${item.kt >= 0 ? `，kt ${item.kt.toFixed(3)}` : ''}）`
  }
  match = line.match(/^VEHICLE_CABIN\s+node=(\d+)\s+name=(\S+)\s+cabinC=(\S+)\s+interiorC=(\S+)\s+outdoorC=(\S+)/)
  if (match) {
    // FacetEB vehicle cabin (temperatures only); summarised when the run closes.
    ;(quality.vehicle || (quality.vehicle = [])).push({ node: Number(match[1]), name: match[2], cabinC: Number(match[3]), interiorC: Number(match[4]), outdoorC: Number(match[5]) })
    return ''
  }
  return null
}

// 运行结束时汇总诊断为日志行（{kind, text}）：车辆座舱最高气温、FacetEB 收敛情况与建议、
// 耗时构成、VoxelEB 能量平衡收敛、短波直散拆分、射线截断提示（短波散射与太阳可见性）。
// Summary of the run diagnostics, written to the log when a run closes.
function runQualitySummary(quality) {
  if (!quality) return []
  const lines = []
  if (quality.vehicle?.length) {
    const best = quality.vehicle.reduce((a, b) => (b.cabinC > a.cabinC ? b : a))
    const hottest = quality.vehicle.reduce((a, b) => (b.interiorC > a.interiorC ? b : a))
    const names = new Set(quality.vehicle.map((item) => item.name))
    lines.push({ kind: '', text: `车辆座舱 · ${names.size} 辆 · 最高座舱气温 ${best.cabinC.toFixed(1)} ℃（${best.name}，节点 ${best.node + 1}，室外 ${best.outdoorC.toFixed(1)} ℃，高 ${(best.cabinC - best.outdoorC).toFixed(1)} K）· 最高内饰节点 ${hottest.interiorC.toFixed(1)} ℃（${hottest.name}，节点 ${hottest.node + 1}）` })
  }
  if (quality.facet.length) {
    const failed = quality.facet.filter((item) => !item.converged)
    const worst = quality.facet.reduce((a, b) => (b.residualK > a.residualK ? b : a))
    const maxIterations = Math.max(...quality.facet.map((item) => item.iterations))
    lines.push({ kind: failed.length ? 'warning' : 'success', text: `运行质量 · ${quality.facet.length} 个节点，${failed.length ? `${failed.length} 个未收敛（节点 ${failed.map((item) => item.node + 1).join('、')}）` : '全部收敛'} · 最多迭代 ${maxIterations} · 最大温度等效残差 ${worst.residualK.toFixed(2)} K（节点 ${worst.node + 1}）` })
    if (failed.length) lines.push({ kind: 'warning', text: '建议：提高“最大耦合迭代”，或在振荡时调小“温度松弛系数”。' })
  }
  if (quality.timing.length) {
    const sum = quality.timing.reduce((total, item) => ({ shortwave: total.shortwave + item.shortwave, coupling: total.coupling + item.coupling, output: total.output + item.output }), { shortwave: 0, coupling: 0, output: 0 })
    lines.push({ kind: '', text: `耗时构成 · 短波窗口 ${sum.shortwave.toFixed(1)} s · RT–EB 耦合 ${sum.coupling.toFixed(1)} s · 影像、产品与节点出图 ${sum.output.toFixed(1)} s` })
  }
  if (quality.voxel.length) {
    const outside = quality.voxel.filter((item) => item.outside > 0)
    const maxIterations = Math.max(...quality.voxel.map((item) => item.iterations))
    lines.push({ kind: outside.length ? 'warning' : 'success', text: `运行质量 · ${quality.voxel.length} 个能量平衡节点，${outside.length ? `${outside.length} 个节点有状态未达容差` : '全部收敛'} · 最多迭代 ${maxIterations}/50` })
  }
  const truncations = quality.scattering.reduce((total, item) => total + item.truncations, 0)
  const splits = (quality.split || []).filter((item) => item.mode === 'erbs')
  if (splits.length) {
    const values = splits.map((item) => item.directFraction)
    const measured = splits.filter((item) => item.source === 'measured').length
    lines.push({ kind: '', text: `短波直散拆分 · 逐节点（${measured ? `${measured} 个节点用实测列，` : ''}${splits.length - measured} 个节点用 Erbs 模型）· 直射份额 ${Math.min(...values).toFixed(2)}–${Math.max(...values).toFixed(2)}` })
  }
  if (truncations > 0) lines.push({ kind: 'warning', text: `运行质量 · 短波散射共有 ${truncations} 条射线达到步数上限，建议增大“体元射线步数上限”。` })
  const solarTruncations = quality.scattering.reduce((total, item) => total + (item.solarTruncations || 0), 0)
  if (solarTruncations > 0) {
    const residualMax = Math.max(...quality.scattering.map((item) => item.solarResidualMax || 0))
    lines.push({ kind: 'warning', text: `运行质量 · 太阳可见性共有 ${solarTruncations} 条射线达到几何步数预算（剩余透射率最大 ${residualMax.toPrecision(3)}，对应体元直射最多偏高该比例的法向直射辐照），详见 diagnostics/scattering_node=*.json。` })
  } else if (quality.scattering.length) {
    lines.push({ kind: 'success', text: '运行质量 · 太阳可见性射线全部追踪到出场景或被遮挡（透射率 ≤ 10⁻⁴ 时提前结束，体元直射误差 ≤ 0.01% 法向直射）。' })
  }
  return lines
}

// 作用：处理服务端推送的模拟事件（事件流）。按 event.type 分支：
// status：重连事件流后同步运行状态（仍在运行则接管；已结束则提示刷新结果）；
// started：重置进度与运行质量，记录模式、温度方法与输出选项；
// stdout：引擎原始输出可能被任意切块，只解析完整行，残余缓存在 state.stdoutCarry；
// stderr/error：写错误日志；
// closed：冲刷残余缓存，按退出码判定成功/未收敛/异常，输出质量汇总，
// 成功或未收敛（结果保留）时延时 300 ms 自动打开结果。
function handleSimulationEvent(event) {
  if (event.type === 'status') {
    if (event.running && !state.running) {
      if (event.outputDir) state.outputDir = event.outputDir
      state.runId = event.runId || ''
      state.startedAt = event.startedAt || Date.now()
      state.progressStage = '正在运行（已重新连接）'
      state.pid = event.pid; setRunning(true)
      addLog(`已连接到正在运行的模拟 · PID ${event.pid}`, 'success')
    } else if (!event.running && state.running && state.runId) {
      // A run that had started finished while the event stream was down.
      state.progressStage = '模拟已结束（连接中断期间）'
      addLog('事件流重新连接时模拟已结束，请刷新结果查看最终状态', 'error')
      setRunning(false); renderRunProgress(Date.now() - state.startedAt)
    }
    return
  }
  if (event.type === 'started') {
    if (event.outputDir) state.outputDir = event.outputDir
    state.runId = event.runId || state.runId || ''
    if (!state.startedAt) state.startedAt = Date.now()
    state.progress = 1
    state.progressStage = '正在启动计算引擎'
    state.runQuality = emptyRunQuality()
    state.stdoutCarry = ''
    state.pid = event.pid; setRunning(true)
    addLog(t(`开始模拟 · ${t(simulationModeLabels[state.mode] || state.mode)} · PID ${event.pid}`), 'success')
    const methods = simulationMethodSummary()
    if (methods) addLog(methods)
    addLog(`输出：图像=${state.config?.sensor?.image ? '是' : '否'}；辐射过程=${state.config?.sensor?.radiationProcess ? '是' : '否'}；能量过程=${state.config?.sensor?.energyProcess ? '是' : '否'}`)
  }
  else if (event.type === 'stdout') {
    // Raw engine stdout (event.engine) arrives in arbitrary chunks: parse
    // complete lines only, so a PROGRESS/diagnostics line split across two
    // chunks is not lost. Server status messages are always whole lines.
    let complete = String(event.text || '')
    if (event.engine) {
      const text = (state.stdoutCarry || '') + complete
      const cut = text.lastIndexOf('\n')
      state.stdoutCarry = cut >= 0 ? text.slice(cut + 1) : text
      complete = cut >= 0 ? text.slice(0, cut) : ''
    }
    const message = normalizedEngineLog(complete)
    if (message) addLog(message)
  }
  else if (event.type === 'stderr' || event.type === 'error') addLog(event.text, 'error')
  else if (event.type === 'closed') {
    if (event.outputDir) state.outputDir = event.outputDir
    if (state.stdoutCarry) {
      const tail = normalizedEngineLog(state.stdoutCarry)
      state.stdoutCarry = ''
      if (tail) addLog(tail)
    }
    const success = event.code === 0
    const retained = event.status === 'non_converged'
    const stopped = event.status === 'stopped'
    const timedOut = event.status === 'timeout'
    state.progress = success || retained ? 100 : state.progress
    state.progressStage = success ? '模拟完成'
      : retained ? '模拟未收敛，结果已保留'
      : stopped ? '模拟已停止'
      : timedOut ? '模拟超时终止'
      : '模拟异常结束'
    addLog(success ? `模拟完成，耗时 ${(event.elapsed / 1000).toFixed(2)} 秒`
      : retained ? `模拟未收敛，结果已保留（耗时 ${(event.elapsed / 1000).toFixed(2)} 秒）`
      : stopped ? '模拟已停止'
      : timedOut ? '模拟超时终止'
      : `模拟已结束，退出码 ${event.code}${event.signal ? `，信号 ${event.signal}` : ''}`,
      success || retained ? 'success' : stopped ? '' : 'error')
    // Non-fatal output problems (e.g. ENVI results that could not be converted to TIFF).
    for (const warning of event.warnings || []) addLog(`结果转换提示：${warning}`, 'warning')
    // 引擎结果诊断（如面元 zeroVisibilitySamples 占比过高）；不影响运行状态。
    for (const warning of event.runWarnings || []) addLog(`结果质量提示：${warning}`, 'warning')
    if (success && event.runWarnings?.length) toast('模拟完成（有质量提示）', event.runWarnings[0], 'warning')
    if (success && event.warnings?.length) toast('模拟完成', `${event.warnings.length} 个结果文件未能转换为 TIFF，详见日志`, 'warning')
    if (!success && !retained) {
      toast('模拟结束', stopped ? '模拟已停止' : timedOut ? '模拟超时终止' : '退出码：' + event.code, stopped ? '' : 'error')
    }
    setRunning(false)
    renderRunProgress(event.elapsed)
    const qualityLines = runQualitySummary(state.runQuality)
    for (const line of qualityLines) addLog(line.text, line.kind)
    if (success && qualityLines.some((line) => line.kind === 'warning')) toast('模拟完成（有质量提示）', qualityLines.find((line) => line.kind === 'warning').text, 'warning')
    // Results of a completed (or non-converged but retained) run open
    // directly instead of waiting for "查看模拟结果".
    if (success || event.status === 'non_converged') {
      setTimeout(() => { showResults().catch((error) => addLog(`无法自动打开结果：${error.message}`, 'warning')) }, 300)
    }
  }
}

// 作用：字节数格式化为 B / KB / MB（1024 进制，KB 与 MB 保留一位小数）。
function formatBytes(value) {
  if (value < 1024) return `${value} B`
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`
  return `${(value / 1024 / 1024).toFixed(1)} MB`
}


// —— 结果预览：把引擎输出的 ENVI/TIFF 预览数据绘制为二维图像或风场箭头图 ——
// 作用：在结果预览区显示空状态（标题 + 说明）。
function emptyResultPreview(title, message) {
  $('#resultPreview').innerHTML = `<div class="empty-editor"><svg><use href="#i-layers"/></svg><h3>${escapeHtml(title)}</h3><p>${escapeHtml(message)}</p></div>`
}

// 作用：把服务端返回的 base64 预览像素（每像素 1 字节，已拉伸到 0–255）解码为 Uint8Array。
function decodeResultPixels(result) {
  const source = atob(result.pixels || '')
  return Uint8Array.from(source, (character) => character.charCodeAt(0))
}

function grayscaleResultPixels(bytes, width, height) {
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d')
  const image = context.createImageData(width, height)
  for (let index = 0; index < bytes.length; index += 1) {
    const value = bytes[index]
    const offset = index * 4
    image.data[offset] = value; image.data[offset + 1] = value; image.data[offset + 2] = value; image.data[offset + 3] = 255
  }
  context.putImageData(image, 0, 0)
  return canvas
}

// 作用：把高温异常强度（0–255）画成半透明叠加层：强度越大越红（G、B 分量减小），
// 透明度 160–255；强度为 0 的像素保持全透明。
function thermalHotspotPixels(bytes, width, height) {
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d')
  const image = context.createImageData(width, height)
  for (let index = 0; index < bytes.length; index += 1) {
    const strength = bytes[index] / 255
    if (strength <= 0) continue
    const offset = index * 4
    image.data[offset] = 255
    image.data[offset + 1] = Math.round(214 * (1 - strength))
    image.data[offset + 2] = Math.round(40 * (1 - strength))
    image.data[offset + 3] = Math.round(160 + strength * 95)
  }
  context.putImageData(image, 0, 0)
  return canvas
}

// 作用：把单波段预览结果转成栅格对象 {width, height, bytes, textureCanvas}：
// 灰度画布，若存在 hotspotPixels 再叠加高温异常层。尺寸取预览尺寸（previewWidth/Height）。
function resultRasterFromEnvi(result) {
  const width = result.previewWidth, height = result.previewHeight
  const bytes = decodeResultPixels(result)
  const textureCanvas = grayscaleResultPixels(bytes, width, height)
  if (result.hotspotPixels) {
    const hotspotBytes = Uint8Array.from(atob(result.hotspotPixels), (character) => character.charCodeAt(0))
    textureCanvas.getContext('2d').drawImage(thermalHotspotPixels(hotspotBytes, width, height), 0, 0)
  }
  return { width, height, bytes, textureCanvas }
}

function resultViewActions(allow3D = true) {
  if (!allow3D) return ''
  return '<div class="result-view-actions"><button class="button ghost result-2d-btn" id="result2dBtn" type="button">二维图像</button><button class="button ghost result-3d-btn" id="result3dBtn" type="button">三维分析</button></div>'
}

// 作用：色标数值文字：非有限值显示“—”，0 显示“0”，绝对值 ≥ 1e4 或 < 1e-3 用 3 位小数的科学计数，
// 其余用 5 位有效数字。
function formatColorbarValue(value) {
  if (!Number.isFinite(Number(value))) return '—'
  const numeric = Number(value)
  if (numeric === 0) return '0'
  const absolute = Math.abs(numeric)
  return absolute >= 10000 || absolute < .001 ? numeric.toExponential(3) : numeric.toPrecision(5)
}

// 作用：生成色标 HTML。label 形如“名称 [单位]” 时标题显示为“名称 · 单位”；
// palette 取 gray/hotspot/red/green/blue/diverging 之一，否则用缺省色带。
function resultColorbar(label, minimum, maximum, palette = 'spectral') {
  const text = String(label || '数值')
  const match = text.match(/^(.*?)\s*\[([^\]]+)\]\s*$/)
  const title = match ? `${match[1]} · ${match[2]}` : text
  const paletteClass = ['gray', 'hotspot', 'red', 'green', 'blue', 'diverging'].includes(palette) ? palette : ''
  return `<div class="result-colorbar ${paletteClass}" role="img" aria-label="${escapeHtml(title)} 色标"><div class="result-colorbar-title" title="${escapeHtml(title)}">${escapeHtml(title)}</div><div class="result-colorbar-scale"><div class="result-colorbar-gradient"></div><div class="result-colorbar-labels"><span>${escapeHtml(formatColorbarValue(minimum))}</span><span>${escapeHtml(formatColorbarValue(maximum))}</span></div></div></div>`
}

// 作用：求第 index 个波段的波长（nm）：优先从波段名中的“@ 数字 nm”解析；
// 否则当传感器配置的波段数与结果波段数一致时取配置值；都没有则返回 null。
function resultBandWavelength(result, index) {
  const name = String(result.bandNames?.[index] || '')
  const labelled = Number(name.match(/@\s*([0-9.]+)\s*nm/i)?.[1])
  if (Number.isFinite(labelled)) return labelled
  const configured = sensorBandValues(state.config?.sensor || {})
  return configured.length === result.bands && Number.isFinite(configured[index]) ? configured[index] : null
}

function resultBandLabel(result, index) {
  return result.bandNames?.[index] || `波段 ${index + 1}`
}

// 作用：选出可用于彩色合成的“光学”波段：波长已知且 ≤ 2500 nm 的波段；
// 一个也没有（波长未知）时退回前 5 个波段。
function resultOpticalBands(result) {
  if (!result) return []
  const all = Array.from({ length: result.bands }, (_, index) => ({ index, wavelength: resultBandWavelength(result, index) }))
  const knownOptical = all.filter((item) => Number.isFinite(item.wavelength) && item.wavelength <= 2500)
  return knownOptical.length ? knownOptical : all.slice(0, Math.min(5, all.length))
}

// 作用：给出缺省真彩色的 [R, G, B] 波段索引：取光学波段中最接近 668 / 560 / 475 nm 者；
// 波长未知时按波段顺序取前三个并反序（第 3、2、1 波段作 R、G、B）；
// 光学波段不足 3 个则返回 null（只能灰度显示）。
function defaultColorBands(result) {
  if (!result || result.bands < 3) return null
  const optical = resultOpticalBands(result)
  if (optical.length < 3) return null
  if (!optical.every((item) => Number.isFinite(item.wavelength))) return [optical[2].index, optical[1].index, optical[0].index]
  const closest = (target) => optical.reduce((best, item) => Math.abs(item.wavelength - target) < Math.abs(best.wavelength - target) ? item : best)
  return [closest(668).index, closest(560).index, closest(475).index]
}

// 作用：得到并缓存该结果文件的显示设置 {mode, channels, gray}（存于 state.resultDisplay[path]）。
// 之前的选择若越界或不在光学波段内则回落到缺省；无缺省彩色波段时强制灰度模式。
function normalizedResultDisplay(result, file) {
  const defaults = defaultColorBands(result)
  const opticalIndices = new Set(resultOpticalBands(result).map((item) => item.index))
  const previous = state.resultDisplay[file.path] || {}
  const validBand = (value, fallback) => Number.isInteger(Number(value)) && Number(value) >= 0 && Number(value) < result.bands ? Number(value) : fallback
  const channels = [0, 1, 2].map((channel) => {
    const selected = validBand(previous.channels?.[channel], defaults?.[channel] ?? 0)
    return opticalIndices.has(selected) ? selected : defaults?.[channel] ?? 0
  })
  const gray = validBand(previous.gray, defaults?.[0] ?? result.band ?? 0)
  const settings = { mode: defaults && previous.mode !== 'gray' ? 'color' : 'gray', channels, gray }
  state.resultDisplay[file.path] = settings
  return settings
}

function resultBandOptions(result, selected, indices = Array.from({ length: result.bands }, (_, index) => index)) {
  return indices.map((index) => `<option value="${index}" ${index === selected ? 'selected' : ''}>${escapeHtml(resultBandLabel(result, index))}</option>`).join('')
}

// 作用：生成显示控制区：彩色/灰度切换按钮，彩色模式下 R/G/B 三个波段下拉（仅光学波段），
// 灰度模式下一个波段下拉；附拉伸说明（缺省真彩色 668/560/475 nm；灰度 P2–P98 拉伸）。
function resultDisplayControls(result, settings) {
  const colorAvailable = Boolean(defaultColorBands(result))
  const opticalIndices = resultOpticalBands(result).map((item) => item.index)
  const colorSelectors = ['红色 R', '绿色 G', '蓝色 B'].map((label, channel) => `<label><span>${label}</span><select class="input result-channel" aria-label="${label}" data-channel="${channel}">${resultBandOptions(result, settings.channels[channel], opticalIndices)}</select></label>`).join('')
  const selectors = settings.mode === 'color'
    ? colorSelectors
    : `<label><span>灰度波段</span><select class="input" id="resultGrayBand" aria-label="灰度波段">${resultBandOptions(result, settings.gray)}</select></label>`
  return `<div class="result-display-controls"><div class="result-display-modes"><button class="button ghost ${settings.mode === 'color' ? 'active' : ''}" id="resultColorMode" type="button" ${colorAvailable ? '' : 'disabled'}>彩色图像</button><button class="button ghost ${settings.mode === 'gray' ? 'active' : ''}" id="resultGrayMode" type="button">灰度图像</button></div><div class="result-channel-controls">${selectors}</div>${settings.mode === 'color' ? '<small>默认真彩色：红 668 nm、绿 560 nm、蓝 475 nm；也可选择红边或近红外组成假彩色。</small>' : '<small>选择一个波段，以 P2–P98 灰度拉伸显示。</small>'}</div>`
}

// 作用：把三个单波段栅格（已各自拉伸到 0–255）合成为彩色画布；bytes 只保留第一通道，
// 因此三维分析不用于彩色模式。
function colorCompositeRaster(rasters) {
  const width = rasters[0].width, height = rasters[0].height
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d')
  const image = context.createImageData(width, height)
  for (let index = 0; index < width * height; index += 1) {
    const offset = index * 4
    image.data[offset] = rasters[0].bytes[index]
    image.data[offset + 1] = rasters[1].bytes[index]
    image.data[offset + 2] = rasters[2].bytes[index]
    image.data[offset + 3] = 255
  }
  context.putImageData(image, 0, 0)
  return { width, height, bytes: rasters[0].bytes, textureCanvas: canvas }
}

// 作用：解析模拟时间标签为 {day, hour, minute}。
// 两种写法：纯数字（整数部分为日序，小数部分为当天的比例，按 1440 分钟折算并处理进位）；
// 或文本中形如 “DOY123 … 12-30” 的日序与时分。无法识别返回 null。
// 数字写法即引擎的儒略日 t：整数部分为年积日 DOY、小数部分为当天比例（voxeleb.cpp/fileio.cpp 生成文件名
// DOY<d>_<HH>-<MM> 用同一换算，分钟四舍五入后进位到 1440 则 DOY 加 1）；文本写法正对应该文件名格式。
function simulationTimeParts(value) {
  const text = String(value ?? '').trim()
  if (!text) return null
  const julianTime = Number(text)
  if (Number.isFinite(julianTime)) {
    let day = Math.floor(julianTime)
    let minuteOfDay = Math.round((julianTime - day) * 1440)
    if (minuteOfDay >= 1440) { day += 1; minuteOfDay -= 1440 }
    if (minuteOfDay < 0) { day -= 1; minuteOfDay += 1440 }
    return { day, hour: Math.floor(minuteOfDay / 60), minute: minuteOfDay % 60 }
  }
  const match = text.match(/DOY\s*(\d+).*?(\d{1,2})[-_:](\d{1,2})/i)
  return match ? { day: Number(match[1]), hour: Number(match[2]), minute: Number(match[3]) } : null
}

// 作用：把时间标签显示为 “DOY<日序> HH:MM”；无法解析时把 _to_ 换成“ → ”、下划线换成空格。
function displaySimulationTime(value) {
  const parts = simulationTimeParts(value)
  if (parts) return `DOY${parts.day} ${String(parts.hour).padStart(2, '0')}:${String(parts.minute).padStart(2, '0')}`
  return String(value || '').replaceAll('_to_', ' → ').replaceAll('_', ' ')
}

// 作用：绑定“二维图像 / 三维分析”两个按钮：三维进入 showResult3D，二维按结果类型重画预览。
function bindResultViewActions(result, file, raster, image = false) {
  $('#result3dBtn')?.addEventListener('click', () => showResult3D(raster, file, result))
  $('#result2dBtn')?.addEventListener('click', () => image ? drawImagePreview(result, file) : result.kind === 'tiff' ? drawTiffPreview(result, file) : drawEnviPreview(result, file))
}

// 作用：流场切片（空气温度、风场）的画布显示尺寸：保持宽高比，最大边小于 480 像素时放大到 480，
// 大于 720 时缩小到 720，其余保持原尺寸。
function fluidSliceDisplaySize(width, height) {
  const previewMaximum = Math.max(Number(width) || 0, Number(height) || 0, 1)
  const scale = Math.min(720 / previewMaximum, Math.max(1, 480 / previewMaximum))
  return {
    width: Math.max(1, Math.round(width * scale)),
    height: Math.max(1, Math.round(height * scale))
  }
}

// 作用：绘制 ENVI（或 TIFF，format 区分）结果的二维预览，含色标、显示控制和元信息。
// 步骤：1) 递增请求号并释放旧三维视图（异步竞态保护）；2) 取显示设置，读取所需波段
// （已持有的波段直接复用）；3) 读取完成后若已有更新请求或选中文件已变则放弃；
// 4) 转栅格，彩色模式合成；5) 按波段与拉伸信息生成色标、高温异常图例与元信息；
// 6) 写入预览 HTML 并把栅格画到画布；7) 绑定模式/波段切换（切换后重绘）与三维按钮。
// 坐标约定：影像上北右东；透视投影且 |vza| > 0.001 时改为天空朝上（vza 为观测天顶角，待核对）。
async function drawEnviPreview(result, file, format = 'ENVI') {
  // 步骤 1：请求号用于丢弃过期的异步结果（用户连续切换文件/波段时只有最后一次生效）。
  const previewRequest = ++state.resultPreviewRequest
  disposeResultViewer()
  // 步骤 2：彩色模式读取 R/G/B 三个波段，灰度只读一个；失败时在预览区、toast 与日志中报告。
  const settings = normalizedResultDisplay(result, file)
  const selectedBands = settings.mode === 'color' ? settings.channels : [settings.gray]
  let bandResults
  try {
    bandResults = await Promise.all(selectedBands.map((band) => band === result.band ? result : api.readResult(file.path, band)))
  } catch (error) {
    if (previewRequest === state.resultPreviewRequest) {
      disposeResultViewer()
      emptyResultPreview('结果读取失败', error.message)
      toast('结果读取失败', error.message, 'error')
      addLog(`结果读取失败：${error.message}`, 'error')
    }
    return
  }
  if (previewRequest !== state.resultPreviewRequest || state.selectedResult?.path !== file.path) return
  // 步骤 4：转栅格并保存到 state.resultRaster；仅 ENVI 灰度模式允许三维分析（TIFF、彩色不允许）。
  const rasters = bandResults.map(resultRasterFromEnvi)
  const raster = settings.mode === 'color' ? colorCompositeRaster(rasters) : rasters[0]
  state.resultRaster = raster
  const allow3D = format !== 'TIFF' && settings.mode === 'gray'
  const observationMeta = format === 'TIFF' && file.observationLabel ? '<span>' + escapeHtml(file.observationLabel.replaceAll('_to_', ' → ').replaceAll('_', ' ')) + '</span>' : ''
  // 步骤 5：显示范围优先用读取端给出的 P2–P98 拉伸范围，缺失时退回 min/max；
  // 热红外波段额外显示原始范围；灰度模式下若有高温异常像元，附带热点图例。
  const activeResult = bandResults[0]
  const layerName = resultBandLabel(result, settings.gray)
  const displayMinimum = Number.isFinite(Number(activeResult.stretchMinimum)) ? Number(activeResult.stretchMinimum) : Number(activeResult.minimum)
  const displayMaximum = Number.isFinite(Number(activeResult.stretchMaximum)) ? Number(activeResult.stretchMaximum) : Number(activeResult.maximum)
  const hotspotCount = settings.mode === 'gray' ? Number(activeResult.hotspotCount) || 0 : 0
  const hotspotLegend = hotspotCount > 0
    ? resultColorbar(`高温异常 · ${hotspotCount.toLocaleString('zh-CN')} 像元`, Number(activeResult.hotspotThreshold), Number(activeResult.maximum), 'hotspot') : ''
  const rawRange = settings.mode === 'gray' && activeResult.thermalBand
    ? `<span>原始范围 ${formatColorbarValue(activeResult.minimum)} ～ ${formatColorbarValue(activeResult.maximum)}</span>` : ''
  const grayStretchLabel = activeResult.stretchMode === 'thermal-surface'
    ? '地表细节 P2–P98（天空与无效零值不参与拉伸）'
    : 'P2–P98'
  const colorbars = settings.mode === 'color'
    ? bandResults.map((bandResult, channel) => resultColorbar(`${['R', 'G', 'B'][channel]} · ${resultBandLabel(result, settings.channels[channel])} · P2–P98`, bandResult.stretchMinimum, bandResult.stretchMaximum, ['red', 'green', 'blue'][channel])).join('')
    : resultColorbar(`${layerName} · ${grayStretchLabel}`, displayMinimum, displayMaximum, 'gray')
  const displaySummary = settings.mode === 'color'
    ? `彩色合成：R ${escapeHtml(resultBandLabel(result, settings.channels[0]))} · G ${escapeHtml(resultBandLabel(result, settings.channels[1]))} · B ${escapeHtml(resultBandLabel(result, settings.channels[2]))}`
    : `灰度波段：${escapeHtml(layerName)}`
  // 空气温度切片（文件名 fluid_air_temperature_*）按流场切片的显示尺寸放大并开启图像平滑，
  // 其余结果保持原尺寸、关闭平滑以保留像元边界。
  const isAirTemperatureSlice = format === 'TIFF' && /^fluid_air_temperature_/i.test(String(file.name || ''))
  const displaySize = isAirTemperatureSlice
    ? fluidSliceDisplaySize(result.previewWidth, result.previewHeight)
    : { width: result.previewWidth, height: result.previewHeight }
  const fluidSliceClass = isAirTemperatureSlice ? ' fluid-slice-result-card' : ''
  const fluidCanvasClass = isAirTemperatureSlice ? ' fluid-slice-canvas' : ''
  const perspectiveForwardView = state.project?.configuration?.sensor?.projection === 'perspective'
    && Math.abs(Number(state.project.configuration.sensor.vza) || 0) > 0.001
    && !isAirTemperatureSlice
  const orientationIndicator = perspectiveForwardView
    ? '<div class="result-north-indicator" aria-label="影像上方朝向天空"><strong>UP</strong><span>↑</span></div>'
    : '<div class="result-north-indicator" aria-label="影像上方为正北"><strong>N</strong><span>↑</span></div>'
  const orientationLabel = perspectiveForwardView ? '天空向上 · 地平线水平' : '上北 · 右东'
  // 步骤 6：写入预览卡片 HTML（画布、方位指示、显示控制、色标、视图按钮、元信息），
  // 随后把栅格画到画布。
  $('#resultPreview').innerHTML = `<div class="result-preview-card${fluidSliceClass}"><div class="result-image-stage"><canvas class="result-canvas${fluidCanvasClass}" id="resultCanvas" width="${displaySize.width}" height="${displaySize.height}"></canvas>${orientationIndicator}</div>${resultDisplayControls(result, settings)}${colorbars}${hotspotLegend}${resultViewActions(allow3D)}<div class="result-meta"><span>${result.width} × ${result.height}</span><span>${format} · ${result.bands} 个独立波段</span><span>${orientationLabel}</span><span>${displaySummary}</span>${observationMeta}<span>${settings.mode === 'color' ? '各通道显示 P2–P98' : `灰度显示 ${grayStretchLabel}`}</span>${settings.mode === 'gray' ? `<span>MIN ${displayMinimum.toPrecision(6)}</span><span>MAX ${displayMaximum.toPrecision(6)}</span>` : ''}${rawRange}</div></div>`
  const canvas = $('#resultCanvas')
  const context = canvas.getContext('2d')
  context.imageSmoothingEnabled = isAirTemperatureSlice
  context.drawImage(raster.textureCanvas, 0, 0, displaySize.width, displaySize.height)
  // 步骤 7：绑定显示控制：切换彩色/灰度、更换波段后按新设置重绘；最后绑定二维/三维视图按钮。
  $('#resultColorMode')?.addEventListener('click', () => { settings.mode = 'color'; drawEnviPreview(result, file, format) })
  $('#resultGrayMode')?.addEventListener('click', () => { settings.mode = 'gray'; drawEnviPreview(result, file, format) })
  $$('.result-channel').forEach((select) => select.addEventListener('change', () => {
    settings.channels[Number(select.dataset.channel)] = Number(select.value)
    drawEnviPreview(result, file, format)
  }))
  $('#resultGrayBand')?.addEventListener('change', (event) => {
    settings.gray = Number(event.target.value)
    drawEnviPreview(result, file, format)
  })
  bindResultViewActions(activeResult, file, raster)
}

// 作用：TIFF 结果预览入口，与 ENVI 共用 drawEnviPreview（灰度/彩色合成、色标、2D 画布），
//   仅把格式标记为 'TIFF'（TIFF 下不提供三维分析按钮，且会显示观测方向标签）。
async function drawTiffPreview(result, file) {
  await drawEnviPreview(result, file, 'TIFF')
}

// 作用：风速箭头配色。把风速线性归一化到 [minimum, maximum] 后映射到 HSL 色相：
//   最小值为 225°（蓝），最大值为 0°（红）；范围退化（max <= min）时取中间值 0.5。
function windArrowColor(value, minimum, maximum) {
  const range = maximum - minimum
  const normalized = range > 0 ? Math.max(0, Math.min(1, (value - minimum) / range)) : 0.5
  return `hsl(${(1 - normalized) * 225},82%,42%)`
}

// 作用：绘制水平风场预览——底图为风速栅格（半透明），上面叠加自适应风矢量箭头。
// 输入：result（含 width/height、arrows、arrowStep、stretchMinimum/Maximum）；
//   arrows 每项为 [gridX, gridY, u, v, magnitude]（网格坐标 + 水平风分量 + 风速，m/s）。
// 说明：画布上方为正北、右为正东，故 v（北向分量）取负号换算成画布 y 方向。
// 步骤 1：生成预览卡片 HTML 并取得画布；步骤 2：半透明绘制风速底图；步骤 3：逐个绘制箭头。
function drawWindPreview(result, file) {
  disposeResultViewer()
  const raster = resultRasterFromEnvi(result)
  state.resultRaster = raster
  const displaySize = fluidSliceDisplaySize(result.previewWidth, result.previewHeight)
  const canvasWidth = displaySize.width
  const canvasHeight = displaySize.height
  const observationMeta = file.observationLabel
    ? `<span>${escapeHtml(file.observationLabel.replaceAll('_to_', ' → ').replaceAll('_', ' '))}</span>` : ''
  $('#resultPreview').innerHTML = `<div class="result-preview-card fluid-slice-result-card"><div class="result-image-stage"><canvas class="result-canvas fluid-slice-canvas" id="windFieldCanvas" width="${canvasWidth}" height="${canvasHeight}" role="img" aria-label="自适应风速箭头图"></canvas><div class="result-north-indicator" aria-label="影像上方为正北"><strong>N</strong><span>↑</span></div></div>${resultColorbar('水平风速 P2–P98 [m/s]', result.stretchMinimum, result.stretchMaximum)}<div class="result-meta"><span>${result.width} × ${result.height}</span><span>上北 · 右东</span><span>自适应箭头 ${Number(result.arrows?.length || 0).toLocaleString('zh-CN')} 个</span><span>每隔 ${result.arrowStep} 个网格采样</span>${observationMeta}<span>色标范围：水平风速 P2–P98</span><span>箭头方向：水平风向</span><span>箭头长度与颜色：水平风速</span></div></div>`
  // 步骤 2：底图以 0.34 透明度绘制，避免遮盖箭头。
  const canvas = $('#windFieldCanvas')
  const context = canvas.getContext('2d')
  context.imageSmoothingEnabled = true
  context.globalAlpha = 0.34
  context.drawImage(raster.textureCanvas, 0, 0, canvasWidth, canvasHeight)
  context.globalAlpha = 1
  // 步骤 3：箭头采样间距（画布像素）；最大箭头长度取间距较小者的 78%，避免相邻箭头重叠。
  const spacingX = result.arrowStep * canvasWidth / Math.max(1, result.width)
  const spacingY = result.arrowStep * canvasHeight / Math.max(1, result.height)
  const availableLength = Math.max(5, Math.min(spacingX, spacingY) * 0.78)
  const minimum = Number(result.stretchMinimum)
  const maximum = Number(result.stretchMaximum)
  for (const arrow of result.arrows || []) {
    // 水平风速近零或风速非有限的点不画箭头（方向无意义）。
    const [gridX, gridY, u, v, magnitude] = arrow.map(Number)
    const horizontalSpeed = Math.hypot(u, v)
    if (!(horizontalSpeed > 1e-8) || !Number.isFinite(magnitude)) continue
    const normalized = maximum > minimum
      ? Math.max(0, Math.min(1, (magnitude - minimum) / (maximum - minimum))) : 0.5
    // 箭头长度随风速在 38%–100% 的最大长度之间变化；以格元中心为箭头中点；
    //   箭头头部长度限制在 2.5–7 像素，线宽限制在 1.1–2.1 像素。
    const length = availableLength * (0.38 + 0.62 * normalized)
    const dx = u / horizontalSpeed * length
    const dy = -v / horizontalSpeed * length
    const centerX = (gridX + 0.5) * canvasWidth / result.width
    const centerY = (gridY + 0.5) * canvasHeight / result.height
    const startX = centerX - dx * 0.5, startY = centerY - dy * 0.5
    const endX = centerX + dx * 0.5, endY = centerY + dy * 0.5
    const angle = Math.atan2(dy, dx)
    const headLength = Math.max(2.5, Math.min(7, length * 0.32))
    context.beginPath()
    context.moveTo(startX, startY)
    context.lineTo(endX, endY)
    context.moveTo(endX, endY)
    context.lineTo(endX - headLength * Math.cos(angle - Math.PI / 6), endY - headLength * Math.sin(angle - Math.PI / 6))
    context.moveTo(endX, endY)
    context.lineTo(endX - headLength * Math.cos(angle + Math.PI / 6), endY - headLength * Math.sin(angle + Math.PI / 6))
    context.strokeStyle = windArrowColor(magnitude, minimum, maximum)
    context.lineWidth = Math.max(1.1, Math.min(2.1, availableLength * 0.09))
    context.lineCap = 'round'
    context.lineJoin = 'round'
    context.stroke()
  }
}

// 作用：把普通图像（PNG/JPG 等）转成灰度栅格（调用方存入 state.resultRaster）。
// 说明：长边最多缩小到 768 像素（不放大）；亮度按 Rec.709 系数（0.2126/0.7152/0.0722）计算，
//   返回 { width, height, bytes（0–255 灰度）, textureCanvas }。
function rasterFromImage(image) {
  const scale = Math.min(1, 768 / Math.max(image.naturalWidth, image.naturalHeight, 1))
  const width = Math.max(1, Math.round(image.naturalWidth * scale))
  const height = Math.max(1, Math.round(image.naturalHeight * scale))
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d', { willReadFrequently: true })
  context.drawImage(image, 0, 0, width, height)
  const source = context.getImageData(0, 0, width, height).data
  const bytes = new Uint8Array(width * height)
  for (let index = 0; index < bytes.length; index += 1) {
    const offset = index * 4
    bytes[index] = Math.round(source[offset] * .2126 + source[offset + 1] * .7152 + source[offset + 2] * .0722)
  }
  return { width, height, bytes, textureCanvas: canvas }
}

// 作用：显示图像类结果（result.data 为 base64）。previewRequest 是请求序号，
//   异步解码完成后若用户已切换到别的结果则丢弃本次结果，避免旧请求覆盖新预览。
async function drawImagePreview(result, file) {
  const previewRequest = ++state.resultPreviewRequest
  disposeResultViewer()
  state.resultRaster = null
  const imageUrl = `data:${result.mimeType};base64,${result.data}`
  $('#resultPreview').innerHTML = `<div class="result-preview-card"><div class="result-image-stage"><img class="result-image" id="resultImage" alt="${escapeHtml(file.name)}"></div><div class="result-meta"><span>${escapeHtml(file.name)}</span><span>${escapeHtml(result.mimeType)}</span></div></div>`
  const image = $('#resultImage')
  await new Promise((resolve, reject) => {
    image.onload = () => resolve()
    image.onerror = () => reject(new Error('图像结果无法解码'))
    image.src = imageUrl
  })
  if (previewRequest !== state.resultPreviewRequest || state.selectedResult?.path !== file.path) return
  const raster = rasterFromImage(image)
  state.resultRaster = raster
}

// 作用：释放结果三维视图（three.js）占用的几何体、材质、纹理、渲染器和 WebGL 上下文，
//   并移除 DOM；可重复调用（无 viewer 时仅清除分析预览样式）。
function disposeResultViewer() {
  $('#resultPreview')?.classList.remove('analysis-preview')
  if (!resultViewer) return
  resultViewer.resizeObserver?.disconnect()
  resultViewer.controls.dispose()
  resultViewer.scene.traverse((object) => {
    object.geometry?.dispose?.()
    if (Array.isArray(object.material)) object.material.forEach((material) => material.dispose?.())
    else object.material?.dispose?.()
  })
  resultViewer.texture?.dispose?.()
  resultViewer.renderer.dispose()
  // dispose() keeps the GL context alive; Chromium caps live contexts (~16)
  // and evicts the oldest one, which is the main scene viewport.
  resultViewer.renderer.forceContextLoss()
  resultViewer.renderer.domElement.remove()
  resultViewer = null
}

// 作用：按宿主容器尺寸更新结果三维视图的渲染尺寸与相机宽高比。
function resizeResultViewer() {
  if (!resultViewer) return
  const host = resultViewer.host
  const width = Math.max(1, host.clientWidth), height = Math.max(1, host.clientHeight)
  resultViewer.renderer.setSize(width, height, false)
  resultViewer.camera.aspect = width / height
  resultViewer.camera.updateProjectionMatrix()
}

// 作用：渲染一帧结果三维视图；结果对话框隐藏或无 viewer 时跳过。
function renderResultViewer() {
  if (!resultViewer || $('#resultDialog').hidden) return
  resultViewer.controls.update()
  resultViewer.renderer.render(resultViewer.scene, resultViewer.camera)
}

// 作用：把灰度栅格（ENVI/TIFF/图像结果）显示为整幅三维高度面：像素亮度越大，面越高。
// 输入：raster（{ width, height, bytes(0–255), textureCanvas }）、file、sourceResult（来源结果，
//   用于色标范围与返回二维预览）；输出：写入全局 resultViewer（three.js 场景），无返回值。
// 说明：只是显示用的相对高度（无物理单位），不是真实地形或物理高度。
// 步骤：1 生成卡片与色标；2 场景/灯光/相机/控制器；3 降采样顶点；4 三角形索引；
//   5 贴图纹理与表面网格；6 底板与网格线；7 创建渲染器与“二维视图”切换。
function showResult3D(raster, file, sourceResult = null) {
  if (!raster) return
  disposeResultViewer()
  const rasterResult = ['envi', 'tiff'].includes(sourceResult?.kind) ? sourceResult : null
  const rasterLabel = rasterResult?.bandNames?.[rasterResult.band] || '像素亮度'
  const rasterMinimum = rasterResult?.minimum ?? 0
  const rasterMaximum = rasterResult?.maximum ?? 255
  // 步骤 1：色标范围取源结果的 minimum/maximum；没有源结果（普通图像）时按 0–255。
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${resultColorbar(rasterLabel, rasterMinimum, rasterMaximum, 'gray')}${resultViewActions()}<div class="result-meta"><span>${raster.width} × ${raster.height}</span><span>整幅图像三维分析</span><span>高度按像素亮度映射</span></div></div>`
  // 步骤 2：创建场景、半球光 + 平行光、透视相机和带阻尼的轨道控制器（限制俯仰角、缩放范围）。
  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const resultSun = new THREE.DirectionalLight(0xfff7e8, 3)
  resultSun.position.set(8, 14, 10)
  resultScene.add(resultSun)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .1, 120)
  camera3d.position.set(15, 13, 16)
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true
  controls3d.dampingFactor = .08
  controls3d.maxPolarAngle = Math.PI * .48
  controls3d.minDistance = 5
  controls3d.maxDistance = 55
  controls3d.target.set(0, 1.2, 0)

  // 步骤 3：把栅格降采样为不超过 180×180 的顶点网格以控制三维性能；
  //   世界宽度固定 16，深度按图像宽高比；高度 = 0.08 + 亮度 × 4.12。
  const columns = Math.max(2, Math.min(180, raster.width))
  const rows = Math.max(2, Math.min(180, raster.height))
  const width = 16
  const depth = width * raster.height / Math.max(1, raster.width)
  const positions = [], uvs = [], indices = []
  for (let row = 0; row < rows; row += 1) {
    const sy = Math.round(row * (raster.height - 1) / Math.max(1, rows - 1))
    for (let column = 0; column < columns; column += 1) {
      const sx = Math.round(column * (raster.width - 1) / Math.max(1, columns - 1))
      const brightness = raster.bytes[sy * raster.width + sx] / 255
      positions.push((column / (columns - 1) - .5) * width, .08 + brightness * 4.12, (.5 - row / (rows - 1)) * depth)
      uvs.push(column / (columns - 1), 1 - row / (rows - 1))
    }
  }
  // 步骤 4：每个网格单元拆成两个三角形，并建立几何体与法向量。
  for (let row = 0; row < rows - 1; row += 1) for (let column = 0; column < columns - 1; column += 1) {
    const a = row * columns + column, b = a + 1, c = a + columns, d = c + 1
    indices.push(a, c, b, b, c, d)
  }
  const geometry = new THREE.BufferGeometry()
  geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3))
  geometry.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2))
  geometry.setIndex(indices)
  geometry.computeVertexNormals()
  // 步骤 5：用 2D 预览的着色画布作贴图（最近邻采样，保留像元边界），颜色与二维视图一致。
  const texture = new THREE.CanvasTexture(raster.textureCanvas)
  texture.colorSpace = THREE.SRGBColorSpace
  texture.minFilter = THREE.NearestFilter
  texture.magFilter = THREE.NearestFilter
  texture.generateMipmaps = false
  const surface = new THREE.Mesh(geometry, new THREE.MeshStandardMaterial({ map: texture, roughness: .88, metalness: .02, side: THREE.DoubleSide, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false }))
  surface.castShadow = surface.receiveShadow = true
  resultScene.add(surface)
  // 步骤 6：半透明底板与参考网格线。
  const base = new THREE.Mesh(new THREE.PlaneGeometry(width, depth), new THREE.MeshBasicMaterial({ color: 0xe6edf5, transparent: true, opacity: .8, side: THREE.DoubleSide }))
  base.rotation.x = -Math.PI / 2; base.position.y = -.06
  resultScene.add(base)
  const resultGrid = new THREE.GridHelper(Math.max(width, depth), 16, 0x7895b5, 0xb8c6d6)
  resultGrid.position.y = -.04; resultGrid.material.opacity = .25; resultGrid.material.transparent = true
  resultScene.add(resultGrid)
  // 步骤 7：创建渲染器（像素比最多 2，ACES 色调映射），监听容器尺寸变化；
  //   “二维视图”按钮按来源类型重新调用 drawImagePreview / drawTiffPreview / drawEnviPreview。
  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }), texture }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2))
  resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  resultViewer.renderer.toneMapping = THREE.ACESFilmicToneMapping
  resultViewer.renderer.toneMappingExposure = 1.05
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer)
  resultViewer.resizeObserver.observe(host)
  resizeResultViewer()
  controls3d.update()
  $('#result3dBtn').classList.add('active')
  $('#result2dBtn').addEventListener('click', async () => {
    if (!rasterResult && !sourceResult) return
    try {
      if (!rasterResult) await drawImagePreview(sourceResult, file)
      else if (rasterResult.kind === 'tiff') await drawTiffPreview(rasterResult, file)
      else await drawEnviPreview(rasterResult, file)
    } catch (error) {
      disposeResultViewer()
      emptyResultPreview('结果读取失败', error.message)
      addLog(`结果预览失败：${error.message}`, 'error')
    }
  })

}

// 作用：面元三维视图的控制条 HTML——显示指标按钮（result.metrics）和面方向按钮
//   （both 正反两面 / front 正面 / back 背面），当前选项带 active。
function facetControls(result, metric, side) {
  const metrics = result.metrics || []
  const metricButtons = metrics.map((item) => `<button class="button ghost facet-metric ${item.id === metric ? 'active' : ''}" type="button" data-metric="${escapeHtml(item.id)}">${escapeHtml(item.label)}</button>`).join('')
  return `<div class="facet-controls"><div><span>显示指标</span>${metricButtons}</div><div><span>面方向</span><button class="button ghost facet-side ${side === 'both' ? 'active' : ''}" type="button" data-side="both">正反两面</button><button class="button ghost facet-side ${side === 'front' ? 'active' : ''}" type="button" data-side="front">正面</button><button class="button ghost facet-side ${side === 'back' ? 'active' : ''}" type="button" data-side="back">背面</button></div></div>`
}

// 结果读取接口可能以二进制块回传大数组（Float32Array/Float64Array），与普通数组同等对待。
const isNumberArray = (value) => Array.isArray(value) || ArrayBuffer.isView(value)

// 作用：返回要显示的面值下标。面元值按 [面0正, 面0反, 面1正, 面1反, ...] 交错存放，
//   所以正面下标 = 2·i，背面下标 = 2·i + 1。
function facetIndices(result, side) {
  const total = result.facetCount * 2
  if (side === 'front') return Array.from({ length: result.facetCount }, (_, index) => index * 2)
  if (side === 'back') return Array.from({ length: result.facetCount }, (_, index) => index * 2 + 1)
  return Array.from({ length: total }, (_, index) => index)
}

// 作用：统计所选面方向上的有限值最小/最大值（色标范围）；无有限值时返回 0、0。
function facetStatsForSide(values, facetCount, side) {
  let minimum = Infinity, maximum = -Infinity
  const update = (value) => {
    if (!Number.isFinite(value)) return
    minimum = Math.min(minimum, value); maximum = Math.max(maximum, value)
  }
  for (let facet = 0; facet < facetCount; facet += 1) {
    const front = Number(values[facet * 2])
    const back = Number(values[facet * 2 + 1])
    if (side !== 'back') update(front)
    if (side !== 'front') update(back)
  }
  return Number.isFinite(minimum) ? { minimum, maximum } : { minimum: 0, maximum: 0 }
}

// 作用：按 id 取面元指标，找不到时退回第一个指标。
function facetMetric(result, metric) {
  return result.metrics.find((item) => item.id === metric) || result.metrics[0]
}

// 作用：为指标/面方向按钮绑定重绘（showFacet3D），切换其一时保持另一个不变。
function bindFacetControls(result, file, metric, side) {
  $$('.facet-metric').forEach((button) => button.addEventListener('click', () => showFacet3D(result, file, button.dataset.metric, side)))
  $$('.facet-side').forEach((button) => button.addEventListener('click', () => showFacet3D(result, file, metric, button.dataset.side)))
  $('#facet3dBtn')?.addEventListener('click', () => showFacet3D(result, file, metric, side))
}

// 作用：面元三维视图的操作按钮 HTML（目前只有“面元三维”）。
function facetViewActions() {
  return '<div class="result-view-actions"><button class="button ghost facet-3d-btn active" id="facet3dBtn" type="button">面元三维</button></div>'
}

// 作用：FacetRT/FacetEB 结果的面元三维视图：每个三角面元按所选指标着色。
// 输入：result（facetCount、metrics[].values（正反面交错）、vertexPositions（每面元 3 个顶点
//   × xyz，共 9 个数）、sourceFacetCount）、metric 指标 id、side 面方向。
// 说明：旧结果没有 vertexPositions 时退化为按面元序号排布的柱状图；
//   面元过多时只显示抽样的一部分，界面会提示“原始 N 个，当前显示 M 个”。
// 步骤：1 统计与卡片；2 场景；3 有几何时按真实位置绘制，否则柱状图；4 创建渲染器。
function showFacet3D(result, file, metric = 'radiosity', side = 'both') {
  disposeResultViewer()
  // 步骤 1：确定指标与色标范围、抽样提示文案，并生成预览卡片 HTML。
  const selected = facetMetric(result, metric)
  const stats = facetStatsForSide(selected.values, result.facetCount, side)
  const valueCount = side === 'both' ? result.facetCount * 2 : result.facetCount
  const sourceFacetCount = Math.max(result.facetCount, Number(result.sourceFacetCount) || result.facetCount)
  const samplingLabel = sourceFacetCount > result.facetCount
    ? '面元过多：原始 ' + sourceFacetCount.toLocaleString('zh-CN') + ' 个，当前显示 ' + result.facetCount.toLocaleString('zh-CN') + ' 个'
    : '完整显示 ' + result.facetCount.toLocaleString('zh-CN') + ' 个面'
  const rawPositions = isNumberArray(result.vertexPositions) ? result.vertexPositions : []
  const hasGeometry = rawPositions.length >= result.facetCount * 9
  const sideDescription = side === 'both' ? '三维表面取正反面较大值' : side === 'front' ? '显示正面值' : '显示背面值'
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${resultColorbar(selected.label, stats.minimum, stats.maximum)}${facetControls(result, selected.id, side)}${facetViewActions()}<div class="result-meta"><span>${escapeHtml(samplingLabel)} · ${valueCount.toLocaleString('zh-CN')} 个面值</span><span>颜色：${escapeHtml(selected.label)}</span><span>${sideDescription}</span><span>MIN ${Number(stats.minimum).toPrecision(6)} · MAX ${Number(stats.maximum).toPrecision(6)}</span>${hasGeometry ? '<span>真实 OBJ 面元空间位置</span>' : '<span>旧结果无几何坐标，使用面元序号排列</span>'}</div></div>`
  // 步骤 2：创建场景、灯光、相机和轨道控制器（近远裁剪面 0.01–300，适应小尺度模型）。
  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const resultSun = new THREE.DirectionalLight(0xfff7e8, 3)
  resultSun.position.set(8, 14, 10); resultScene.add(resultSun)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .01, 300)
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true; controls3d.dampingFactor = .08; controls3d.maxPolarAngle = Math.PI * .49
  controls3d.minDistance = .5; controls3d.maxDistance = 300

  // 颜色映射：值线性归一化后用 HSL 色相 (1 − t)·0.67（蓝=小 → 红=大）；
  //   非有限值显示深灰（饱和度 0、亮度 0.16）；范围为 0 时取中间色。
  const range = stats.maximum - stats.minimum
  const colorForValue = (value, color) => {
    const normalized = Number.isFinite(value) ? (range ? (value - stats.minimum) / range : .5) : 0
    color.setHSL((1 - normalized) * .67, Number.isFinite(value) ? .82 : 0, Number.isFinite(value) ? .5 : .16)
    return normalized
  }

  // 步骤 3a：有真实几何时，每个面元按原三角形绘制。以包围盒最大边把模型缩放到 20 个世界单位，
  //   X/Z 以包围盒中心居中、Y 以最小值为 0（Y 向上）；both 模式下每面元取正反面较大值着色。
  if (hasGeometry) {
    let minimum = [Infinity, Infinity, Infinity], maximum = [-Infinity, -Infinity, -Infinity]
    for (let index = 0; index < result.facetCount * 9; index += 3) {
      for (let axis = 0; axis < 3; axis += 1) {
        const value = Number(rawPositions[index + axis])
        if (!Number.isFinite(value)) continue
        minimum[axis] = Math.min(minimum[axis], value); maximum[axis] = Math.max(maximum[axis], value)
      }
    }
    const spanX = Math.max(.001, maximum[0] - minimum[0]), spanY = Math.max(.001, maximum[1] - minimum[1]), spanZ = Math.max(.001, maximum[2] - minimum[2])
    const modelScale = 20 / Math.max(spanX, spanY, spanZ)
    const centerX = (minimum[0] + maximum[0]) / 2, centerZ = (minimum[2] + maximum[2]) / 2
    const positions = new Float32Array(result.facetCount * 9)
    const colors = new Float32Array(result.facetCount * 9)
    const facetColor = new THREE.Color()
    for (let facet = 0; facet < result.facetCount; facet += 1) {
      const front = Number(selected.values[facet * 2])
      const back = Number(selected.values[facet * 2 + 1])
      const value = side === 'front' ? front : side === 'back' ? back : Math.max(front, back)
      colorForValue(value, facetColor)
      for (let corner = 0; corner < 3; corner += 1) {
        const offset = facet * 9 + corner * 3
        positions[offset] = (Number(rawPositions[offset]) - centerX) * modelScale
        positions[offset + 1] = (Number(rawPositions[offset + 1]) - minimum[1]) * modelScale
        positions[offset + 2] = (Number(rawPositions[offset + 2]) - centerZ) * modelScale
        colors[offset] = facetColor.r; colors[offset + 1] = facetColor.g; colors[offset + 2] = facetColor.b
      }
    }
    const geometry = new THREE.BufferGeometry()
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3))
    geometry.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3))
    const surface = new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.DoubleSide, toneMapped: false, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false }))
    surface.castShadow = surface.receiveShadow = true
    resultScene.add(surface)
    const groundWidth = Math.max(.5, spanX * modelScale), groundDepth = Math.max(.5, spanZ * modelScale)
    const base = new THREE.Mesh(new THREE.PlaneGeometry(groundWidth * 1.08, groundDepth * 1.08), new THREE.MeshBasicMaterial({ color: 0xe6edf5, transparent: true, opacity: .8, side: THREE.DoubleSide }))
    base.rotation.x = -Math.PI / 2; base.position.y = -.04; resultScene.add(base)
    const resultGrid = new THREE.GridHelper(Math.max(groundWidth, groundDepth) * 1.08, 22, 0x7895b5, 0xb8c6d6)
    resultGrid.position.y = -.02; resultGrid.scale.z = groundDepth / Math.max(groundWidth, groundDepth); resultGrid.scale.x = groundWidth / Math.max(groundWidth, groundDepth); resultGrid.material.opacity = .25; resultGrid.material.transparent = true; resultScene.add(resultGrid)
    camera3d.position.set(Math.max(16, groundWidth * 1.25), Math.max(12, spanY * modelScale * .85 + 8), Math.max(16, groundDepth * 1.25))
    controls3d.target.set(0, Math.max(.5, spanY * modelScale * .35), 0)
  } else {
    // 步骤 3b：无几何的旧结果——按面值序号排成近似正方形的柱状网格，
    //   柱高 = 0.18 + 归一化值 × 6（非有限值 0.04）。
    const indices = facetIndices(result, side)
    const columns = Math.max(1, Math.ceil(Math.sqrt(indices.length)))
    const rows = Math.max(1, Math.ceil(indices.length / columns))
    const sideLength = 22, cell = sideLength / Math.max(columns, rows)
    const geometry = new THREE.BoxGeometry(Math.max(.04, cell * .72), 1, Math.max(.04, cell * .72))
    const bars = new THREE.InstancedMesh(geometry, new THREE.MeshStandardMaterial({ vertexColors: true, roughness: .82, metalness: .02, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false }), indices.length)
    const matrix = new THREE.Matrix4(), color = new THREE.Color()
    indices.forEach((index, position) => {
      const value = Number(selected.values[index]), normalized = colorForValue(value, color)
      const height = Number.isFinite(value) ? .18 + normalized * 6 : .04
      const column = position % columns, row = Math.floor(position / columns)
      matrix.compose(new THREE.Vector3((column - (columns - 1) / 2) * cell, height / 2, ((rows - 1) / 2 - row) * cell), new THREE.Quaternion(), new THREE.Vector3(1, height, 1))
      bars.setMatrixAt(position, matrix); bars.setColorAt(position, color)
    })
    bars.instanceMatrix.needsUpdate = true; if (bars.instanceColor) bars.instanceColor.needsUpdate = true
    resultScene.add(bars)
    const base = new THREE.Mesh(new THREE.PlaneGeometry(sideLength, sideLength), new THREE.MeshBasicMaterial({ color: 0xe6edf5, transparent: true, opacity: .8, side: THREE.DoubleSide }))
    base.rotation.x = -Math.PI / 2; base.position.y = -.04; resultScene.add(base)
    const resultGrid = new THREE.GridHelper(sideLength, 22, 0x7895b5, 0xb8c6d6)
    resultGrid.position.y = -.02; resultGrid.material.opacity = .25; resultGrid.material.transparent = true; resultScene.add(resultGrid)
    camera3d.position.set(sideLength * .8, sideLength * .75, sideLength * .9); controls3d.target.set(0, 1.8, 0)
  }

  // 步骤 4：创建渲染器并挂接尺寸监听，同时为指标/面方向按钮绑定重绘。
  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }) }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  resultViewer.renderer.toneMapping = THREE.ACESFilmicToneMapping; resultViewer.renderer.toneMappingExposure = 1.05
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer); resultViewer.resizeObserver.observe(host)
  resizeResultViewer(); controls3d.update(); $('#facet3dBtn').classList.add('active'); bindFacetControls(result, file, selected.id, side)
}

// 作用：VoxelRT/VoxelEB 结果的体元三维视图：每个体元为一个立方体，按所选过程变量着色。
// 输入：result（voxelCount、voxelPositions（每体元 xyz）、voxelSize、metrics[].values、
//   sourceVoxelCount、soilProfile、time）、metric 指标 id。
// 说明：体元过多时三维窗口只显示抽样的一部分（结果文件保存全部），界面显示“抽样显示 n / N”；立方体面按太阳入射方向加明暗，
//   实例颜色表示指标值（所以面明暗与指标颜色相乘）。
// 步骤：1 统计取值与位置范围；2 抽样提示与卡片；3 场景；4 立方体面明暗；
//   5 实例化体元；6 地面网格、相机与渲染器。
function showVoxel3D(result, file, metric = result.metrics?.[0]?.id) {
  disposeResultViewer()
  const selected = result.metrics.find((item) => item.id === metric) || result.metrics[0]
  if (!selected || !isNumberArray(result.voxelPositions) || !result.voxelCount) {
    emptyResultPreview('体元过程结果为空', '没有可显示的体元位置或过程变量。')
    return
  }
  // 步骤 1：一次遍历同时统计指标值的有限最小/最大值和体元位置包围盒（色标与居中缩放用）。
  let valueMinimum = Infinity, valueMaximum = -Infinity
  let minimum = [Infinity, Infinity, Infinity], maximum = [-Infinity, -Infinity, -Infinity]
  for (let voxel = 0; voxel < result.voxelCount; voxel += 1) {
    const raw = selected.values[voxel]
    const value = typeof raw === 'number' ? raw : NaN
    if (Number.isFinite(value)) { valueMinimum = Math.min(valueMinimum, value); valueMaximum = Math.max(valueMaximum, value) }
    for (let axis = 0; axis < 3; axis += 1) {
      const position = Number(result.voxelPositions[voxel * 3 + axis])
      if (Number.isFinite(position)) { minimum[axis] = Math.min(minimum[axis], position); maximum[axis] = Math.max(maximum[axis], position) }
    }
  }
  if (!Number.isFinite(valueMinimum)) valueMinimum = valueMaximum = 0
  // 步骤 2：判断是否抽样显示，并生成提示文案与预览卡片 HTML。
  const sourceCount = Math.max(result.voxelCount, Number(result.sourceVoxelCount) || result.voxelCount)
  const sampled = sourceCount > result.voxelCount
  const sampleLabel = sampled
    ? `抽样显示 ${result.voxelCount.toLocaleString('zh-CN')} / ${sourceCount.toLocaleString('zh-CN')} 个体元`
    : `完整显示 ${result.voxelCount.toLocaleString('zh-CN')} 个体元`
  const samplingNotice = sampled
    ? `<div class="result-sampling-notice"><strong>当前为抽样显示</strong><span>结果文件已保存全部 ${sourceCount.toLocaleString('zh-CN')} 个体元；三维窗口抽样显示 ${result.voxelCount.toLocaleString('zh-CN')} 个。</span></div>`
    : ''
  const metricButtons = result.metrics.map((item) => `<button class="button ghost facet-metric ${item.id === selected.id ? 'active' : ''}" type="button" data-metric="${escapeHtml(item.id)}">${escapeHtml(item.label)}</button>`).join('')
  const soilProfileAction = result.soilProfile
    ? '<button class="button ghost" id="soilProfile3dBtn" type="button">多层土壤温度</button>' : ''
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${samplingNotice}${resultColorbar(selected.label, valueMinimum, valueMaximum)}<div class="facet-controls"><div><span>显示指标</span>${metricButtons}</div></div><div class="result-view-actions"><button class="button ghost facet-3d-btn active" id="facet3dBtn" type="button">体元过程</button>${soilProfileAction}</div><div class="result-meta"><span>${escapeHtml(sampleLabel)}</span><span>颜色：${escapeHtml(selected.label)}</span><span>MIN ${Number(valueMinimum).toPrecision(6)} · MAX ${Number(valueMaximum).toPrecision(6)}</span><span>${escapeHtml(result.time || '静态辐射场')}</span></div></div>`

  // 步骤 3：创建场景；包围盒最大边缩放到 20 个世界单位，X/Z 居中、Y 以最小值为 0。
  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const span = maximum.map((value, axis) => Math.max(.001, value - minimum[axis]))
  const modelScale = 20 / Math.max(...span)
  const center = minimum.map((value, axis) => (value + maximum[axis]) / 2)
  // 略微覆盖相邻体元，消除斜视角和抗锯齿造成的地形接缝。
  const boxSize = Math.max(.025, Number(result.voxelSize || 1) * modelScale * 1.002)
  const geometry = new THREE.BoxGeometry(boxSize, boxSize, boxSize)
  // 步骤 4：由 config.light 的太阳天顶角（限制 0–90°）和方位角算出太阳方向，
  //   每个顶点按法向与太阳方向的夹角余弦给明暗 0.32 + 0.68·max(0, cosθ)，
  //   使立方体各面有可分辨的立体感（方位角取向的约定待核对）。
  // BoxGeometry 为六个面保留独立法向；顶点颜色只表达面朝向，实例颜色仍表达计算指标。
  const zenith = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(Number(state.config?.light?.zenith) || 0, 0, 90))
  const azimuth = THREE.MathUtils.degToRad(((Number(state.config?.light?.azimuth) || 0) % 360 + 360) % 360)
  const solarDirection = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  const normals = geometry.getAttribute('normal')
  const vertexCount = geometry.getAttribute('position').count
  const faceColors = new Float32Array(vertexCount * 3)
  for (let vertex = 0; vertex < vertexCount; vertex += 1) {
    const incidence = Math.max(0,
      normals.getX(vertex) * solarDirection.x +
      normals.getY(vertex) * solarDirection.y +
      normals.getZ(vertex) * solarDirection.z)
    const brightness = .32 + .68 * incidence
    faceColors[vertex * 3] = brightness
    faceColors[vertex * 3 + 1] = brightness
    faceColors[vertex * 3 + 2] = brightness
  }
  geometry.setAttribute('color', new THREE.Float32BufferAttribute(faceColors, 3))
  // 说明：与其他结果视图（面元/影像，设 ACES 色调映射）不同，本函数的渲染器没有设置色调映射
  //   （保持默认 NoToneMapping），材质 toneMapped:false 只是显式保证指标色标颜色不被色调映射改变，
  //   所以不设色调映射是有意的、也无害。
  const material = new THREE.MeshBasicMaterial({ color: 0xffffff, vertexColors: true, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false, toneMapped: false })
  // 步骤 5：用实例化网格一次绘制全部体元；每个体元设平移矩阵与指标颜色
  //   （HSL 色相 (1 − t)·0.67，蓝=小 → 红=大，非有限值为深灰）。
  const voxels = new THREE.InstancedMesh(geometry, material, result.voxelCount)
  const matrix = new THREE.Matrix4(), color = new THREE.Color()
  const valueRange = valueMaximum - valueMinimum
  for (let voxel = 0; voxel < result.voxelCount; voxel += 1) {
    const x = (Number(result.voxelPositions[voxel * 3]) - center[0]) * modelScale
    const y = (Number(result.voxelPositions[voxel * 3 + 1]) - minimum[1]) * modelScale
    const z = (Number(result.voxelPositions[voxel * 3 + 2]) - center[2]) * modelScale
    matrix.makeTranslation(x, y, z); voxels.setMatrixAt(voxel, matrix)
    const rawValue = selected.values[voxel]
    const value = typeof rawValue === 'number' ? rawValue : NaN
    const normalized = Number.isFinite(value) ? (valueRange ? (value - valueMinimum) / valueRange : .5) : 0
    color.setHSL((1 - normalized) * .67, Number.isFinite(value) ? .82 : 0, Number.isFinite(value) ? .5 : .16)
    voxels.setColorAt(voxel, color)
  }
  voxels.instanceMatrix.needsUpdate = true
  if (voxels.instanceColor) voxels.instanceColor.needsUpdate = true
  material.needsUpdate = true
  resultScene.add(voxels)
  // 步骤 6：按场景范围画参考网格，相机放在场景外斜上方，再创建渲染器并绑定指标/土壤剖面按钮。
  const groundWidth = Math.max(.5, span[0] * modelScale), groundDepth = Math.max(.5, span[2] * modelScale)
  const grid = new THREE.GridHelper(Math.max(groundWidth, groundDepth) * 1.08, 22, 0x7895b5, 0xb8c6d6)
  grid.position.y = -.02; grid.scale.x = groundWidth / Math.max(groundWidth, groundDepth); grid.scale.z = groundDepth / Math.max(groundWidth, groundDepth); grid.material.opacity = .25; grid.material.transparent = true
  resultScene.add(grid)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .01, 300)
  camera3d.position.set(Math.max(16, groundWidth * 1.2), Math.max(12, span[1] * modelScale + 7), Math.max(16, groundDepth * 1.2))
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true; controls3d.dampingFactor = .08; controls3d.maxPolarAngle = Math.PI * .49; controls3d.minDistance = .5; controls3d.maxDistance = 300
  controls3d.target.set(0, Math.max(.5, span[1] * modelScale * .35), 0)
  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }) }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer); resultViewer.resizeObserver.observe(host)
  resizeResultViewer(); controls3d.update()
  $$('.facet-metric').forEach((button) => button.addEventListener('click', () => showVoxel3D(result, file, button.dataset.metric)))
  $('#facet3dBtn')?.addEventListener('click', () => showVoxel3D(result, file, selected.id))
  $('#soilProfile3dBtn')?.addEventListener('click', () => showSoilProfile3D(result, file))
}

// 作用：土壤层深度的显示文本。单位为 m 时，0 显示“表层 0 m”，小于 0.1 m 的换算成 cm；
//   非有限值显示“未知深度”。
function soilProfileDepthLabel(depth, unit) {
  const numeric = Number(depth)
  if (!Number.isFinite(numeric)) return '未知深度'
  if (unit === 'm') {
    if (numeric === 0) return '表层 0 m'
    if (numeric < .1) return `${formatColorbarValue(numeric * 100)} cm`
  }
  return `${formatColorbarValue(numeric)} ${unit || ''}`.trim()
}

// 作用：VoxelEB 多层土壤温度剖面三维视图：把各土层的温度叠成一摞水平薄片，同一色标显示。
// 输入：result.soilProfile（layerCount、values[层][体元]、depths、depthUnit、temperatureUnit）、
//   result.voxelPositions/voxelSize；selectedLayers 为要显示的层集合（Set，缺省全部层）。
// 说明：层间距是等距展开（不按真实深度比例），标签才是真实土层深度；至少保留一层可见；
//   为控制性能，所有可见层合计最多绘制 320000 个单元，超出时按层均匀抽样。
// 步骤：1 校验与可见层；2 统计有效值与范围；3 土层按钮/抽样/卡片；4 场景与分层单元；
//   5 相机与渲染器；6 绑定土层开关与视图切换按钮。
function showSoilProfile3D(result, file, selectedLayers = null) {
  const profile = result.soilProfile
  disposeResultViewer()
  if (!profile || !Array.isArray(profile.values) || !profile.layerCount ||
      !isNumberArray(result.voxelPositions) || !result.voxelCount) {
    emptyResultPreview('土壤温度剖面为空', '当前结果没有多层土壤温度数据。')
    return
  }
  // 步骤 1：确定可见层；传入的层号不合法时忽略，一层都没有时回退到第 0 层（表层）。
  const allLayers = Array.from({ length: profile.layerCount }, (_, layer) => layer)
  const visibleLayers = selectedLayers instanceof Set
    ? new Set([...selectedLayers].filter((layer) => allLayers.includes(layer)))
    : new Set(allLayers)
  if (!visibleLayers.size) visibleLayers.add(0)

  // 步骤 2：逐层记录有效（有限数值）的体元下标、温度范围和水平位置范围；
  //   X/Z 范围用于居中和缩放（本视图把 voxelPositions 的 X、Z 作为水平面）。
  let valueMinimum = Infinity, valueMaximum = -Infinity
  let minimumX = Infinity, maximumX = -Infinity, minimumZ = Infinity, maximumZ = -Infinity
  const validIndices = Array.from({ length: profile.layerCount }, () => [])
  for (let layer = 0; layer < profile.layerCount; layer += 1) {
    const values = isNumberArray(profile.values[layer]) ? profile.values[layer] : []
    for (let voxel = 0; voxel < Math.min(result.voxelCount, values.length); voxel += 1) {
      // JSON serializes binary NaN/NoData as null.  Number(null) is 0, which
      // previously drew every vegetation/building voxel as a false cold spot
      // through all soil layers.
      const rawValue = values[voxel]
      if (typeof rawValue !== 'number' || !Number.isFinite(rawValue)) continue
      const value = rawValue
      validIndices[layer].push(voxel)
      valueMinimum = Math.min(valueMinimum, value)
      valueMaximum = Math.max(valueMaximum, value)
      const x = Number(result.voxelPositions[voxel * 3])
      const z = Number(result.voxelPositions[voxel * 3 + 2])
      if (Number.isFinite(x)) { minimumX = Math.min(minimumX, x); maximumX = Math.max(maximumX, x) }
      if (Number.isFinite(z)) { minimumZ = Math.min(minimumZ, z); maximumZ = Math.max(maximumZ, z) }
    }
  }
  if (!Number.isFinite(valueMinimum) || !Number.isFinite(minimumX) || !Number.isFinite(minimumZ)) {
    emptyResultPreview('土壤温度剖面为空', '该过程结果中没有有效的土壤体元温度。')
    return
  }

  // 步骤 3：生成土层开关按钮，并按每层均匀抽样计算实际绘制的单元数（上限 320000 个实例）。
  const depthButtons = allLayers.map((layer) => {
    const label = soilProfileDepthLabel(profile.depths?.[layer] ?? layer, profile.depthUnit)
    return `<button class="button ghost soil-profile-layer ${visibleLayers.has(layer) ? 'active' : ''}" type="button" data-layer="${layer}">L${layer + 1} · ${escapeHtml(label)}</button>`
  }).join('')
  const metricButtons = result.metrics.map((item) => `<button class="button ghost facet-metric" type="button" data-metric="${escapeHtml(item.id)}">${escapeHtml(item.label)}</button>`).join('')
  const sourceCount = Math.max(result.voxelCount, Number(result.sourceVoxelCount) || result.voxelCount)
  const soilCount = validIndices.reduce((maximum, indices) => Math.max(maximum, indices.length), 0)
  const unitLabel = profile.temperatureUnit === 'degC' ? '°C' : profile.temperatureUnit
  const activeLayerList = allLayers.filter((layer) => visibleLayers.has(layer))
  const maximumInstances = 320000
  const maximumPerLayer = Math.max(1, Math.floor(maximumInstances / activeLayerList.length))
  const displayIndices = new Map(activeLayerList.map((layer) => {
    const source = validIndices[layer]
    if (source.length <= maximumPerLayer) return [layer, source]
    return [layer, Array.from({ length: maximumPerLayer }, (_, index) =>
      source[Math.min(source.length - 1, Math.floor(index * source.length / maximumPerLayer))])]
  }))
  const instanceCount = [...displayIndices.values()].reduce((sum, indices) => sum + indices.length, 0)
  const sourceLayerInstances = activeLayerList.reduce((sum, layer) => sum + validIndices[layer].length, 0)
  const layerSampled = instanceCount < sourceLayerInstances
  const displaySamplingLabel = layerSampled
    ? `当前显示 ${instanceCount.toLocaleString('zh-CN')} 个分层单元（均匀抽样）` : ''
  const samplingNotice = sourceCount > result.voxelCount || layerSampled
    ? `<div class="result-sampling-notice"><strong>当前为抽样显示</strong><span>结果文件已保存全部 ${sourceCount.toLocaleString('zh-CN')} 个体元；三维窗口当前显示 ${instanceCount.toLocaleString('zh-CN')} 个分层单元。</span></div>`
    : ''
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${samplingNotice}${resultColorbar(`土壤温度 [${unitLabel}]`, valueMinimum, valueMaximum)}<div class="facet-controls"><div><span>土层开关</span>${depthButtons}</div></div><div class="result-view-actions"><button class="button ghost" id="facet3dBtn" type="button">体元过程</button><button class="button ghost active" id="soilProfile3dBtn" type="button">多层土壤温度</button></div><div class="result-meta"><span>${profile.layerCount} 层温度剖面 · ${soilCount.toLocaleString('zh-CN')} 个有效土壤体元</span><span>深度：${escapeHtml((profile.depths || []).map((depth) => soilProfileDepthLabel(depth, profile.depthUnit)).join('、'))}</span><span>统一色标：${formatColorbarValue(valueMinimum)} ～ ${formatColorbarValue(valueMaximum)} ${escapeHtml(unitLabel)}</span><span>垂向等距展开，标签为真实土层深度</span>${displaySamplingLabel ? `<span>${escapeHtml(displaySamplingLabel)}</span>` : ''}<span>${sourceCount > result.voxelCount ? `抽样载入 ${result.voxelCount.toLocaleString('zh-CN')} / ${sourceCount.toLocaleString('zh-CN')} 个体元` : escapeHtml(result.time || '静态温度场')}</span></div></div>`

  // 步骤 4：创建场景；每个体元在每个可见层画一个很薄的方片（厚度为边长的 7%），
  //   表层（第 0 层）位于最上方，每层各配一张参考网格，颜色按统一色标 (1 − t)·0.67 色相映射。
  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const spanX = Math.max(.001, maximumX - minimumX)
  const spanZ = Math.max(.001, maximumZ - minimumZ)
  const modelScale = 20 / Math.max(spanX, spanZ)
  const centerX = (minimumX + maximumX) / 2
  const centerZ = (minimumZ + maximumZ) / 2
  const layerGap = Math.max(.8, Math.min(1.5, 10 / Math.max(1, profile.layerCount - 1)))
  const cellSize = Math.max(.025, Number(result.voxelSize || 1) * modelScale * 1.002)
  const cellHeight = Math.max(.025, cellSize * .07)
  const geometry = new THREE.BoxGeometry(cellSize, cellHeight, cellSize)
  const vertexCount = geometry.getAttribute('position').count
  geometry.setAttribute('color', new THREE.Float32BufferAttribute(new Float32Array(vertexCount * 3).fill(1), 3))
  const material = new THREE.MeshBasicMaterial({ color: 0xffffff, vertexColors: true, toneMapped: false })
  const layers = new THREE.InstancedMesh(geometry, material, instanceCount)
  const matrix = new THREE.Matrix4(), color = new THREE.Color()
  const valueRange = valueMaximum - valueMinimum
  let instance = 0
  for (const layer of activeLayerList) {
    const y = (profile.layerCount - 1 - layer) * layerGap
    for (const voxel of displayIndices.get(layer)) {
      const x = (Number(result.voxelPositions[voxel * 3]) - centerX) * modelScale
      const z = (Number(result.voxelPositions[voxel * 3 + 2]) - centerZ) * modelScale
      matrix.makeTranslation(x, y, z)
      layers.setMatrixAt(instance, matrix)
      const value = Number(profile.values[layer][voxel])
      const normalized = valueRange ? (value - valueMinimum) / valueRange : .5
      color.setHSL((1 - normalized) * .67, .82, .5)
      layers.setColorAt(instance, color)
      instance += 1
    }
    const gridSize = Math.max(spanX, spanZ) * modelScale * 1.04
    const grid = new THREE.GridHelper(gridSize, 20, 0x7895b5, 0xb8c6d6)
    grid.position.y = y - cellHeight
    grid.scale.x = spanX / Math.max(spanX, spanZ)
    grid.scale.z = spanZ / Math.max(spanX, spanZ)
    grid.material.opacity = .16
    grid.material.transparent = true
    resultScene.add(grid)
  }
  layers.instanceMatrix.needsUpdate = true
  if (layers.instanceColor) layers.instanceColor.needsUpdate = true
  resultScene.add(layers)

  // 步骤 5：相机放在整摞薄片外斜上方，创建渲染器并挂接尺寸监听。
  const groundWidth = Math.max(.5, spanX * modelScale)
  const groundDepth = Math.max(.5, spanZ * modelScale)
  const stackHeight = Math.max(layerGap, (profile.layerCount - 1) * layerGap)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .01, 300)
  camera3d.position.set(Math.max(16, groundWidth * 1.2), Math.max(14, stackHeight * 1.2 + 6), Math.max(16, groundDepth * 1.2))
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true; controls3d.dampingFactor = .08; controls3d.maxPolarAngle = Math.PI * .52; controls3d.minDistance = .5; controls3d.maxDistance = 300
  controls3d.target.set(0, stackHeight * .45, 0)
  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }) }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer); resultViewer.resizeObserver.observe(host)
  resizeResultViewer(); controls3d.update()

  // 步骤 6：点击土层按钮切换该层显示（至少保留一层）；指标按钮或“体元过程”回到体元视图。
  $$('.soil-profile-layer').forEach((button) => button.addEventListener('click', () => {
    const next = new Set(visibleLayers)
    const layer = Number(button.dataset.layer)
    if (next.has(layer) && next.size > 1) next.delete(layer)
    else next.add(layer)
    showSoilProfile3D(result, file, next)
  }))
  $$('.facet-metric').forEach((button) => button.addEventListener('click', () => showVoxel3D(result, file, button.dataset.metric)))
  $('#facet3dBtn')?.addEventListener('click', () => showVoxel3D(result, file))
  $('#soilProfile3dBtn')?.addEventListener('click', () => showSoilProfile3D(result, file, visibleLayers))
}

// 作用：解析带表头的 CSV（支持双引号字段与 "" 转义、去除 BOM、跳过空行）。
// 输入：CSV 文本；输出：以表头为键的对象数组，少于 2 行（只有表头）时返回空数组。
// 说明：循环多走一步，用末尾补的换行符把最后一行刷出。
function parseCsvRows(content) {
  const rows = []
  let row = [], cell = '', quoted = false
  const source = String(content || '').replace(/^\uFEFF/, '')
  for (let index = 0; index <= source.length; index += 1) {
    const character = source[index] ?? '\n'
    if (quoted) {
      if (character === '"' && source[index + 1] === '"') { cell += '"'; index += 1 }
      else if (character === '"') quoted = false
      else cell += character
    } else if (character === '"') quoted = true
    else if (character === ',') { row.push(cell); cell = '' }
    else if (character === '\n') {
      row.push(cell.replace(/\r$/, '')); cell = ''
      if (row.some((value) => value !== '')) rows.push(row)
      row = []
    } else cell += character
  }
  if (rows.length < 2) return []
  const headers = rows[0].map((value) => value.trim())
  return rows.slice(1).map((values) => Object.fromEntries(headers.map((header, index) => [header, values[index] ?? ''])))
}

// 作用：从文件名中取 key=数值 形式的角度（如 _SZA=30_ 或 _VZA=12.5.），取不到返回 NaN。
//   用于统计 CSV 缺少角度列时的兜底。
function fileAngleToken(file, key) {
  const match = String(file || '').match(new RegExp(`(?:^|_)${key}=([^_.]+(?:\\.[^_.]+)?)(?=_|\\.|$)`, 'i'))
  return match ? Number(match[1]) : NaN
}

// 作用：CSV 单元转数值；空串返回 NaN（避免 Number('') 变成 0）。
function csvNumeric(value) {
  return String(value ?? '').trim() === '' ? NaN : Number(value)
}

// 作用：把统计 CSV（file、mode、time、vza、vaa、sza、saa、band_index、band_name、
//   count、min、max、mean、stddev）转成行对象，供“分析”图表使用。
// 输入：content 为 CSV 文本；observation 为该次运行的观测设置（planeAzimuth、
//   principalPlane、hemisphere），原样附加到每行。
// 说明：角度列缺失或为空时，从文件名中的 SZA/SAA/VZA/VAA 令牌补；
//   同一方向同一波段只保留最新一行（后出现的覆盖先出现的）。
function statisticsRows(content, observation = {}) {
  const latest = new Map()
  for (const source of parseCsvRows(content)) {
    const row = {
      file: source.file || '', mode: source.mode || 'Unknown', time: source.time || '',
      vza: csvNumeric(source.vza), vaa: csvNumeric(source.vaa), bandIndex: csvNumeric(source.band_index),
      bandName: source.band_name || `Band ${Number(source.band_index) + 1}`,
      count: csvNumeric(source.count), min: csvNumeric(source.min), max: csvNumeric(source.max),
      mean: csvNumeric(source.mean), stddev: csvNumeric(source.stddev)
    }
    row.sza = csvNumeric(source.sza)
    row.saa = csvNumeric(source.saa)
    row.planeAzimuth = csvNumeric(observation.planeAzimuth)
    row.principalPlaneRequested = observation.principalPlane
    row.hemisphereRequested = observation.hemisphere
    if (!Number.isFinite(row.sza)) row.sza = fileAngleToken(row.file, 'SZA')
    if (!Number.isFinite(row.saa)) row.saa = fileAngleToken(row.file, 'SAA')
    if (!Number.isFinite(row.vza)) row.vza = fileAngleToken(row.file, 'VZA')
    if (!Number.isFinite(row.vaa)) row.vaa = fileAngleToken(row.file, 'VAA')
    if (!Number.isFinite(row.bandIndex)) continue
    // VZA=0 is a single physical direction: VAA has no meaning at nadir.
    // Keeping the latest equivalent nadir row also hides stale records from
    // statistics files produced by older append-only engine builds.
    const directionKey = Math.abs(row.vza) < 1e-6
      ? `nadir\u0000${row.mode}\u0000${row.time}\u0000${row.sza}\u0000${row.saa}`
      : row.file
    latest.set(`${directionKey}\u0000${row.bandIndex}`, row)
  }
  return [...latest.values()]
}

// 作用：按 getter 取值并去重（以字符串相等判断，保留首个），再排序；
//   默认按中文区域规则做数字感知的字符串排序。
function uniqueValues(rows, getter, sorter = (a, b) => String(a).localeCompare(String(b), 'zh-CN', { numeric: true })) {
  return [...new Map(rows.map((row) => { const value = getter(row); return [String(value), value] })).values()].sort(sorter)
}

// 以下 analysisOption / analysisSelect：生成分析面板里下拉框（<option>/<select>）的 HTML。
function analysisOption(value, label, selected) {
  return `<option value="${escapeHtml(value)}" ${String(value) === String(selected) ? 'selected' : ''}>${escapeHtml(label)}</option>`
}

function analysisSelect(label, id, values, selected, labeler = (value) => value) {
  return `<label><span>${escapeHtml(label)}</span><select class="input analysis-control" id="${id}">${values.map((value) => analysisOption(value, labeler(value), selected)).join('')}</select></label>`
}

// 以下四个函数：统计行的“波段键/角度键”及其显示文本。
//   波段键 = 序号|名称；角度键 = VZA|VAA（单位：度）；标签中图层号 = 序号 + 1。
function statisticBandKey(row) { return `${row.bandIndex}|${row.bandName}` }
function statisticAngleKey(row) { return `${row.vza}|${row.vaa}` }
function statisticBandLabel(key) { const [index, ...name] = String(key).split('|'); return `${name.join('|')}（图层 ${Number(index) + 1}）` }
function statisticAngleLabel(key) { const [vza, vaa] = String(key).split('|'); return `VZA ${formatColorbarValue(vza)}° / VAA ${formatColorbarValue(vaa)}°` }

// 作用：根据波段名称关键词推断物理量类别（反射率/亮温/辐亮度/温度/其他）。
// 说明：判断顺序有意义——“亮温”必须先于“温度”判断，否则会被误归为温度。
function statisticQuantity(row) {
  const name = String(row?.bandName || '').toLowerCase()
  if (name.includes('reflectance') || name.includes('反射率')) return 'reflectance'
  if (name.includes('brightness temperature') || name.includes('亮温')) return 'brightnessTemperature'
  if (name.includes('radiance') || name.includes('辐亮度')) return 'radiance'
  if (name.includes('temperature') || name.includes('温度')) return 'temperature'
  return 'value'
}

// 物理量类别 → 坐标轴文字（含单位）；反射率无量纲，温度单位 K。
const statisticQuantityLabels = {
  reflectance: '反射率 [-]', brightnessTemperature: '亮温 [K]',
  radiance: '光谱辐亮度', temperature: '温度 [K]', value: '结果值'
}

function statisticValueLabel(row) {
  return statisticQuantityLabels[statisticQuantity(row)] || '结果值'
}

// 作用：把统计行的时间标签转成可排序的数值：优先按“第几天 时:分”换算成分钟，
//   其次按日期时间解析，都失败的排在最后。
function statisticTimeOrder(value) {
  const parts = simulationTimeParts(value)
  if (parts) return parts.day * 1440 + parts.hour * 60 + parts.minute
  const text = String(value || '')
  const timestamp = Date.parse(text.replaceAll('_to_', ' '))
  return Number.isFinite(timestamp) ? timestamp : Number.MAX_SAFE_INTEGER
}

// 作用：取波段名里最后一个数字（通常是波长）作为横轴位置；名称无数字时用序号 + 1。
function numericBandPosition(row) {
  const numbers = String(row.bandName).match(/\d+(?:\.\d+)?/g)
  return numbers?.length ? Number(numbers[numbers.length - 1]) : row.bandIndex + 1
}

// 作用：生成“漂亮”的坐标刻度。步长取 {1, 2, 2.5, 5, 10}×10^n 中不小于 (范围/target) 的最小者。
// 输出：{ ticks, format }，format 负责按步长决定小数位；绝对值 ≥ 1e5 时用科学计数法；
//   刻度少于 3 个时把 target 加倍重试（target 小于 40 时）。
function niceTicks(minimum, maximum, target) {
  const raw = (maximum - minimum) / target
  const magnitude = 10 ** Math.floor(Math.log10(raw))
  const multiple = [1, 2, 2.5, 5, 10].find((value) => value * magnitude >= raw * (1 - 1e-9))
  const step = multiple * magnitude
  const decimals = Math.max(0, -Math.floor(Math.log10(step) + 1e-9) + (multiple === 2.5 ? 1 : 0))
  const ticks = []
  for (let index = Math.ceil(minimum / step - 1e-9); index * step <= maximum + step * 1e-9; index += 1) ticks.push(index * step)
  if (ticks.length < 3 && target < 40) return niceTicks(minimum, maximum, target * 2)
  const large = Math.max(Math.abs(minimum), Math.abs(maximum)) >= 1e5
  return { ticks, format: (value) => large ? value.toExponential(2) : value.toFixed(decimals) }
}

// 作用：把若干条折线序列画成 SVG 折线图（分析面板使用）。
// 输入：series = [{ name, points: [{ x, y, label? }] }]；xLabel/yLabel 为轴标题；
//   xFormatter 为 x 刻度格式化函数（缺省为数值色标格式；自定义时 x 轴按序号均匀放 6 个刻度）。
// 步骤：1 收集有限数据点；2 确定坐标范围并留边；3 刻度与网格；4 各序列折线与数据点；
//   5 图例（多于一条序列时）并拼装 SVG。
function lineAnalysisSvg(series, xLabel, yLabel, xFormatter = formatColorbarValue) {
  const points = series.flatMap((item) => item.points).filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y))
  if (!points.length) return '<div class="analysis-empty">当前筛选条件没有可绘制的数据。</div>'
  // 步骤 2：画布 820×440，四周留白放坐标轴文字；x 只有单一取值时左右各扩 0.5；
  //   y 范围上下各扩 8%（全相等时扩 5% 或至少 0.001），避免折线贴边或除零。
  const width = 820, height = 440, left = 76, right = 28, top = 28, bottom = 62
  let xMin = Math.min(...points.map((point) => point.x)), xMax = Math.max(...points.map((point) => point.x))
  let yMin = Math.min(...points.map((point) => point.y)), yMax = Math.max(...points.map((point) => point.y))
  const singleX = xMin === xMax ? xMin : null
  if (singleX !== null) { xMin -= .5; xMax += .5 }
  if (yMin === yMax) { const pad = Math.max(.001, Math.abs(yMin) * .05); yMin -= pad; yMax += pad }
  else { const pad = (yMax - yMin) * .08; yMin -= pad; yMax += pad }
  const x = (value) => left + (value - xMin) / (xMax - xMin) * (width - left - right)
  const y = (value) => top + (yMax - value) / (yMax - yMin) * (height - top - bottom)
  // 步骤 3：序列配色循环使用四种颜色；下面计算 x/y 刻度并拼出网格线与刻度文字。
  const colors = ['#246fd3', '#ef8c28', '#20a36a', '#9b5de5']
  // Ordinal axes (time) keep their own labels at evenly spaced positions.
  const xAxis = xFormatter !== formatColorbarValue ? null
    : singleX !== null ? { ticks: [singleX], format: (value) => String(Number(value.toPrecision(6))) } : niceTicks(xMin, xMax, 6)
  const xTicks = xAxis ? xAxis.ticks : Array.from({ length: 6 }, (_, index) => xMin + index / 5 * (xMax - xMin))
  const yAxis = niceTicks(yMin, yMax, 5)
  const grid = xTicks.map((value, index) => {
    const gx = x(value)
    return `<line x1="${gx}" y1="${top}" x2="${gx}" y2="${height - bottom}"/><text x="${gx}" y="${height - bottom + 24}" text-anchor="middle">${escapeHtml(xAxis ? xAxis.format(value) : xFormatter(value, index))}</text>`
  }).concat(yAxis.ticks.map((value) => {
    const gy = y(value)
    return `<line x1="${left}" y1="${gy}" x2="${width - right}" y2="${gy}"/><text x="${left - 12}" y="${gy + 4}" text-anchor="end">${escapeHtml(yAxis.format(value))}</text>`
  })).join('')
  // 步骤 4：每条序列按 x 升序连线，并在各数据点画圆点（悬停 <title> 显示标签）。
  const paths = series.map((item, seriesIndex) => {
    const sorted = [...item.points].filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y)).sort((a, b) => a.x - b.x)
    const color = colors[seriesIndex % colors.length]
    const path = sorted.map((point, index) => `${index ? 'L' : 'M'}${x(point.x).toFixed(2)},${y(point.y).toFixed(2)}`).join(' ')
    const dots = sorted.map((point) => `<circle cx="${x(point.x)}" cy="${y(point.y)}" r="4"><title>${escapeHtml(point.label || `${xFormatter(point.x)} · ${formatColorbarValue(point.y)}`)}</title></circle>`).join('')
    return `<g style="--series-color:${color}"><path class="analysis-series-line" d="${path}"/>${dots}</g>`
  }).join('')
  // 步骤 5：多于一条序列时显示图例。
  const legend = series.length > 1 ? `<div class="analysis-legend">${series.map((item, index) => `<span><i style="background:${colors[index % colors.length]}"></i>${escapeHtml(item.name)}</span>`).join('')}</div>` : ''
  return `<div class="analysis-chart-wrap"><svg class="analysis-chart" viewBox="0 0 ${width} ${height}" role="img"><g class="analysis-grid">${grid}</g><line class="analysis-axis" x1="${left}" y1="${height - bottom}" x2="${width - right}" y2="${height - bottom}"/><line class="analysis-axis" x1="${left}" y1="${top}" x2="${left}" y2="${height - bottom}"/><text class="analysis-axis-title" x="${(left + width - right) / 2}" y="${height - 12}" text-anchor="middle">${escapeHtml(xLabel)}</text><text class="analysis-axis-title" transform="translate(18 ${(top + height - bottom) / 2}) rotate(-90)" text-anchor="middle">${escapeHtml(yLabel)}</text>${paths}</svg>${legend}</div>`
}

// 作用：两个方位角（度）之间的最小夹角，结果在 0–180° 之间（处理 360° 回绕）。
function angularDistance(a, b) { return Math.abs(((Number(a) - Number(b) + 540) % 360) - 180) }

// 作用：把一条统计行的观测方向投影到“主平面”（perpendicular=false）或其垂直平面上，
//   返回带符号的观测天顶角（度）：VAA 等于平面方位（垂直面则 +90°）时为 +VZA，
//   相反一侧（+180°）为 −VZA，天底（VZA≈0）为 0；不在该平面上返回 null。
// 说明：平面方位优先取运行时保存的 planeAzimuth；其次仅在非时间序列行取该行 saa。
//   VZA 只接受 5° 的整数倍（原因待核对，推测与主平面角度步长有关）；正负侧与前向/后向的对应（待核对）。
function signedPlaneZenith(row, perpendicular = false) {
  if (!Number.isFinite(row.vza) || !Number.isFinite(row.vaa)) return null
  // Time-series sun azimuths vary per node; the view planes stay at the
  // azimuth stored with that run. Without a snapshot, do not invent a plane.
  const solarAzimuth = Number.isFinite(row.planeAzimuth) ? row.planeAzimuth
    : !row.time && Number.isFinite(row.saa) ? row.saa : NaN
  if (!Number.isFinite(solarAzimuth)) return null
  if (Math.abs(row.vza) < 1e-6) return 0
  if (Math.abs(row.vza / 5 - Math.round(row.vza / 5)) > 1e-5) return null
  const positive = solarAzimuth + (perpendicular ? 90 : 0)
  if (angularDistance(row.vaa, positive) < 1e-3) return row.vza
  if (angularDistance(row.vaa, positive + 180) < 1e-3) return -row.vza
  return null
}

// 作用：把半球多角度观测（每行一个 VZA/VAA 方向的整幅图像均值 mean）画成极坐标补全图。
// 输入：rows（含 vza、vaa、mean，单位 度/度/结果值）；valueLabel 为色标标题。
// 说明：径向 = VZA（线性，0 在圆心）；方位 = VAA，正北在上、顺时针（见 N/E/S/W 标注）。
//   色标为蓝（小）→ 白 → 红（大）的发散色，范围取各方向均值的最小/最大值。
// 步骤：1 过滤并规整方位角到 0–360；2 同一方向求平均；3 画布与范围；
//   4 判断是否为规则的极坐标网格；5 两种插值（规则网格 / 散点）；6 逐扇形单元着色；
//   7 参考圈、方位轴与色标。
function polarAnalysisSvg(rows, valueLabel) {
  const normalizeAzimuth = (angle) => ((Number(angle) % 360) + 360) % 360
  const points = rows
    .filter((row) => Number.isFinite(row.vza) && row.vza >= 0 && Number.isFinite(row.vaa) && Number.isFinite(row.mean))
    .map((row) => ({ ...row, vza: Number(row.vza), vaa: normalizeAzimuth(row.vaa), mean: Number(row.mean) }))
  if (!points.length) return '<div class="analysis-empty">当前筛选条件没有半球观测数据。</div>'

  // 步骤 2：同一方向（天底不区分 VAA）的重复观测取平均，以下的补全方式与 TiRT-EB 一致。
  //   至少需要两个不同的观测天顶角才能做径向插值。
  // Match TiRT-EB's angle-completion mode: average duplicate directions,
  // interpolate periodically around VAA, then interpolate radially along VZA.
  const directionKey = (vza, vaa) => Math.abs(vza) < 1e-6 ? 'nadir'
    : `${Number(vza).toFixed(8)}|${normalizeAzimuth(vaa).toFixed(8)}`
  const buckets = new Map()
  for (const point of points) {
    const key = directionKey(point.vza, point.vaa)
    const bucket = buckets.get(key) || { vza: point.vza, vaa: point.vaa, sum: 0, count: 0 }
    bucket.sum += point.mean
    bucket.count += 1
    buckets.set(key, bucket)
  }
  const averaged = [...buckets.values()].map((bucket) => ({ ...bucket, mean: bucket.sum / bucket.count }))
  const zeniths = [...new Set(averaged.map((point) => point.vza))].sort((a, b) => a - b)
  if (zeniths.length < 2) return '<div class="analysis-empty">角度补全至少需要两个观测天顶角。</div>'

  // 步骤 3：画布 820×460，圆心 (410, 220)，最大半径 178 像素对应最大 VZA；
  //   最大 VZA 必须大于 0。
  const width = 820, height = 460, cx = 410, cy = 220, radius = 178
  const minimum = Math.min(...averaged.map((row) => row.mean)), maximum = Math.max(...averaged.map((row) => row.mean))
  const range = maximum - minimum
  const radialMin = zeniths[0], radialMax = zeniths[zeniths.length - 1]
  if (!(radialMax > 0)) return '<div class="analysis-empty">角度补全需要非零的观测天顶角。</div>'

  // 步骤 4：按天顶角分环并按方位排序；各非天底环的方位集合完全一致才视为规则极坐标网格，
  //   否则（如半球 + 主平面混合）按散点处理。规则网格时绘制范围为 [最小 VZA, 最大 VZA]，
  //   散点时从圆心（0°）开始绘制。
  const samplesByZenith = zeniths.map((vza) => averaged.filter((point) => Math.abs(point.vza - vza) < 1e-8).sort((a, b) => a.vaa - b.vaa))
  // A combined hemisphere + principal-plane run is still scattered data.
  // Repeated zeniths along its two cross-sections must not turn it into a grid.
  const nonNadirRings = samplesByZenith.filter((samples) => samples[0].vza > 1e-6)
  const firstRing = nonNadirRings[0] || []
  const isPolarGrid = firstRing.length > 1 && nonNadirRings.every((samples) =>
    samples.length === firstRing.length && samples.every((sample, index) =>
      angularDistance(sample.vaa, firstRing[index].vaa) < 1e-6))
  const plottedRadialMin = isPolarGrid ? radialMin : 0
  // 散点插值用：把每个方向转成单位向量 [sinθ·sinφ, sinθ·cosφ, cosθ]（x 东、y 北、z 天顶，
  //   θ=VZA，φ=VAA），以球面上的夹角余弦衡量远近。
  const scatteredSamples = averaged.map((point) => {
    const zenithRadians = point.vza * Math.PI / 180
    const azimuthRadians = point.vaa * Math.PI / 180
    return {
      ...point,
      direction: [
        Math.sin(zenithRadians) * Math.sin(azimuthRadians),
        Math.sin(zenithRadians) * Math.cos(azimuthRadians),
        Math.cos(zenithRadians)
      ]
    }
  })
  // 步骤 5a：沿方位角做周期线性插值（环上首尾相接，跨 360° 回绕）；只有一个样本时直接返回。
  const interpolateAzimuth = (samples, target) => {
    if (!samples.length) return NaN
    if (samples.length === 1) return samples[0].mean
    let angle = normalizeAzimuth(target)
    if (angle < samples[0].vaa) angle += 360
    let lower = samples[samples.length - 1]
    let upper = { ...samples[0], vaa: samples[0].vaa + 360 }
    for (let index = 0; index < samples.length - 1; index += 1) {
      if (angle <= samples[index + 1].vaa) {
        lower = samples[index]
        upper = samples[index + 1]
        break
      }
    }
    const fraction = (angle - lower.vaa) / Math.max(upper.vaa - lower.vaa, 1e-9)
    return lower.mean + (upper.mean - lower.mean) * fraction
  }
  // 步骤 5b：规则网格插值——先在每个天顶角环上按方位插值，再沿 VZA 线性插值；
  //   超出 [最小, 最大] VZA 返回 NaN（该单元不绘制），低于最内环时取最内环的值。
  const interpolatePolarGrid = (vza, vaa) => {
    if (vza < radialMin - 1e-9 || vza > radialMax + 1e-9) return NaN
    const radialSamples = zeniths.map((zenith, index) => ({ zenith, value: interpolateAzimuth(samplesByZenith[index], vaa) })).filter((sample) => Number.isFinite(sample.value))
    if (!radialSamples.length) return NaN
    if (radialSamples.length === 1 || vza <= radialSamples[0].zenith) return radialSamples[0].value
    for (let index = 0; index < radialSamples.length - 1; index += 1) {
      if (vza <= radialSamples[index + 1].zenith) {
        const lower = radialSamples[index], upper = radialSamples[index + 1]
        const fraction = (vza - lower.zenith) / Math.max(upper.zenith - lower.zenith, 1e-9)
        return lower.value + (upper.value - lower.value) * fraction
      }
    }
    return radialSamples[radialSamples.length - 1].value
  }
  // 步骤 5c：散点插值——取球面上最近的 8 个样本（距离 = 1 − cos 夹角），
  //   按 1/距离 反距离加权平均；与某样本几乎重合（距离 < 1e-12）时直接取该样本值。
  const interpolateScattered = (vza, vaa) => {
    const zenithRadians = vza * Math.PI / 180
    const azimuthRadians = normalizeAzimuth(vaa) * Math.PI / 180
    const target = [
      Math.sin(zenithRadians) * Math.sin(azimuthRadians),
      Math.sin(zenithRadians) * Math.cos(azimuthRadians),
      Math.cos(zenithRadians)
    ]
    const nearest = []
    for (const sample of scatteredSamples) {
      const cosine = Math.max(-1, Math.min(1, sample.direction[0] * target[0] + sample.direction[1] * target[1] + sample.direction[2] * target[2]))
      const distance = 1 - cosine
      if (nearest.length === 8 && distance >= nearest[nearest.length - 1].distance) continue
      let index = nearest.length
      while (index > 0 && distance < nearest[index - 1].distance) index -= 1
      nearest.splice(index, 0, { distance, value: sample.mean })
      if (nearest.length > 8) nearest.pop()
    }
    if (!nearest.length) return NaN
    if (nearest[0].distance < 1e-12) return nearest[0].value
    let weightedValue = 0, totalWeight = 0
    for (const sample of nearest) {
      const weight = 1 / Math.max(sample.distance, 1e-12)
      weightedValue += sample.value * weight
      totalWeight += weight
    }
    return totalWeight ? weightedValue / totalWeight : NaN
  }
  const interpolateField = isPolarGrid ? interpolatePolarGrid : interpolateScattered
  // 发散色标：归一化值 0 → 蓝 (30,74,161)，0.5 → 近白 (245,245,245)，1 → 红 (190,28,28)，
  //   两段分别线性插值；各方向值相等时取中间的白色。
  const colorFor = (value) => {
    const normalized = Math.max(0, Math.min(1, range ? (value - minimum) / range : .5))
    const stops = [[30, 74, 161], [245, 245, 245], [190, 28, 28]]
    const segment = normalized < .5 ? 0 : 1
    const local = segment ? (normalized - .5) * 2 : normalized * 2
    const left = stops[segment], right = stops[segment + 1]
    return `rgb(${left.map((channel, index) => Math.round(channel + (right[index] - channel) * local)).join(',')})`
  }
  // 极坐标 → 画布坐标：方位 0° 在正上方（北），顺时针增大；以下 sectorPath 生成扇环 SVG 路径。
  const pointOnCircle = (distance, azimuth) => {
    const radians = (azimuth - 90) * Math.PI / 180
    return [cx + Math.cos(radians) * distance, cy + Math.sin(radians) * distance]
  }
  const sectorPath = (innerRadius, outerRadius, startAzimuth, endAzimuth) => {
    const [outerStartX, outerStartY] = pointOnCircle(outerRadius, startAzimuth)
    const [outerEndX, outerEndY] = pointOnCircle(outerRadius, endAzimuth)
    if (innerRadius < 1e-6) return `M${cx},${cy}L${outerStartX.toFixed(2)},${outerStartY.toFixed(2)}A${outerRadius.toFixed(2)},${outerRadius.toFixed(2)} 0 0 1 ${outerEndX.toFixed(2)},${outerEndY.toFixed(2)}Z`
    const [innerEndX, innerEndY] = pointOnCircle(innerRadius, endAzimuth)
    const [innerStartX, innerStartY] = pointOnCircle(innerRadius, startAzimuth)
    return `M${outerStartX.toFixed(2)},${outerStartY.toFixed(2)}A${outerRadius.toFixed(2)},${outerRadius.toFixed(2)} 0 0 1 ${outerEndX.toFixed(2)},${outerEndY.toFixed(2)}L${innerEndX.toFixed(2)},${innerEndY.toFixed(2)}A${innerRadius.toFixed(2)},${innerRadius.toFixed(2)} 0 0 0 ${innerStartX.toFixed(2)},${innerStartY.toFixed(2)}Z`
  }

  // 步骤 6：把圆（或圆环）分成约每 1° VZA 一圈、每 2° 方位一格（共 180 格）的扇形单元，
  //   取单元中心方向的插值着色；插值为 NaN 的单元不画。
  const radialSteps = Math.max(1, Math.ceil(radialMax - plottedRadialMin))
  const azimuthSteps = 180
  const cells = []
  for (let radialIndex = 0; radialIndex < radialSteps; radialIndex += 1) {
    const innerVza = plottedRadialMin + (radialMax - plottedRadialMin) * radialIndex / radialSteps
    const outerVza = plottedRadialMin + (radialMax - plottedRadialMin) * (radialIndex + 1) / radialSteps
    for (let azimuthIndex = 0; azimuthIndex < azimuthSteps; azimuthIndex += 1) {
      const startAzimuth = azimuthIndex * 360 / azimuthSteps
      const endAzimuth = (azimuthIndex + 1) * 360 / azimuthSteps
      const value = interpolateField((innerVza + outerVza) / 2, (startAzimuth + endAzimuth) / 2)
      if (!Number.isFinite(value)) continue
      const path = sectorPath(radius * innerVza / radialMax, radius * outerVza / radialMax, startAzimuth, endAzimuth)
      const color = colorFor(value)
      cells.push(`<path d="${path}" fill="${color}" stroke="${color}" stroke-width="0.35"/>`)
    }
  }

  // 步骤 7：三圈天顶角参考圈（最大 VZA 的 1/3、2/3、1）、N/E/S/W 方位轴，并附发散色标。
  const guideAngles = [radialMax / 3, radialMax * 2 / 3, radialMax]
  const guides = guideAngles.map((angle) => `<circle cx="${cx}" cy="${cy}" r="${radius * angle / radialMax}"/><text x="${cx + 5}" y="${cy - radius * angle / radialMax + 14}">${formatColorbarValue(angle)}°</text>`).join('')
  const axes = [0, 90, 180, 270].map((azimuth) => {
    const radians = (azimuth - 90) * Math.PI / 180
    const x = cx + Math.cos(radians) * radius, y = cy + Math.sin(radians) * radius
    const labels = { 0: 'N / 0°', 90: 'E / 90°', 180: 'S / 180°', 270: 'W / 270°' }
    return `<line x1="${cx}" y1="${cy}" x2="${x}" y2="${y}"/><text x="${cx + Math.cos(radians) * (radius + 28)}" y="${cy + Math.sin(radians) * (radius + 28) + 4}" text-anchor="middle">${labels[azimuth]}</text>`
  }).join('')
  return `<div class="analysis-chart-wrap"><svg class="analysis-chart analysis-polar" viewBox="0 0 ${width} ${height}" role="img" aria-label="方位角周期补全极坐标图"><g class="analysis-polar-field">${cells.join('')}</g><g class="analysis-polar-grid">${guides}${axes}</g></svg>${resultColorbar(`${valueLabel} · 整幅图像均值`, minimum, maximum, 'diverging')}</div>`
}

// 作用：把“图像 A vs 图像 B”的像元对画成散点图（变化分析使用）。
// 输入：sourcePoints = [[A 值, B 值], ...]（由服务端抽样得到）。
// 说明：两个轴共用同一范围——取全部 A、B 值的 P2–P98 后各外扩 5%（全相等时扩 5% 或 0.001），
//   超出范围的点由 clipPath 裁掉；analysis-reference-line 为 1:1 参考线。
function scatterAnalysisSvg(sourcePoints) {
  const points = sourcePoints
    .map((point) => ({ x: Number(point[0]), y: Number(point[1]) }))
    .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y))
  if (!points.length) return '<div class="analysis-empty">没有可绘制的有效像元。</div>'
  const width = 680, height = 520, left = 78, right = 28, top = 28, bottom = 62
  const sortedValues = points.flatMap((point) => [point.x, point.y]).sort((a, b) => a - b)
  const percentile = (fraction) => {
    const position = (sortedValues.length - 1) * fraction
    const lower = Math.floor(position), upper = Math.ceil(position)
    if (lower === upper) return sortedValues[lower]
    const weight = position - lower
    return sortedValues[lower] * (1 - weight) + sortedValues[upper] * weight
  }
  let minimum = percentile(.02)
  let maximum = percentile(.98)
  if (minimum === maximum) { const pad = Math.max(.001, Math.abs(minimum) * .05); minimum -= pad; maximum += pad }
  else { const pad = (maximum - minimum) * .05; minimum -= pad; maximum += pad }
  const x = (value) => left + (value - minimum) / (maximum - minimum) * (width - left - right)
  const y = (value) => top + (maximum - value) / (maximum - minimum) * (height - top - bottom)
  const grid = Array.from({ length: 6 }, (_, index) => {
    const ratio = index / 5
    const gx = left + ratio * (width - left - right), gy = top + ratio * (height - top - bottom)
    const value = minimum + ratio * (maximum - minimum)
    const reverseValue = maximum - ratio * (maximum - minimum)
    return `<line x1="${gx}" y1="${top}" x2="${gx}" y2="${height - bottom}"/><line x1="${left}" y1="${gy}" x2="${width - right}" y2="${gy}"/><text x="${gx}" y="${height - bottom + 24}" text-anchor="middle">${escapeHtml(formatColorbarValue(value))}</text><text x="${left - 12}" y="${gy + 4}" text-anchor="end">${escapeHtml(formatColorbarValue(reverseValue))}</text>`
  }).join('')
  const dots = points.map((point) => `<circle cx="${x(point.x).toFixed(2)}" cy="${y(point.y).toFixed(2)}" r="2"><title>A ${escapeHtml(formatColorbarValue(point.x))} · B ${escapeHtml(formatColorbarValue(point.y))}</title></circle>`).join('')
  return `<div class="analysis-chart-wrap"><svg class="analysis-chart change-scatter-chart" viewBox="0 0 ${width} ${height}" role="img" aria-label="图像 A 与图像 B 像元散点图"><defs><clipPath id="changeScatterClip"><rect x="${left}" y="${top}" width="${width - left - right}" height="${height - top - bottom}"/></clipPath></defs><g class="analysis-grid">${grid}</g><line class="analysis-reference-line" x1="${x(minimum)}" y1="${y(minimum)}" x2="${x(maximum)}" y2="${y(maximum)}"/><line class="analysis-axis" x1="${left}" y1="${height - bottom}" x2="${width - right}" y2="${height - bottom}"/><line class="analysis-axis" x1="${left}" y1="${top}" x2="${left}" y2="${height - bottom}"/><text class="analysis-axis-title" x="${(left + width - right) / 2}" y="${height - 12}" text-anchor="middle">图像 A 像元值</text><text class="analysis-axis-title" transform="translate(18 ${(top + height - bottom) / 2}) rotate(-90)" text-anchor="middle">图像 B 像元值</text><g class="change-scatter-points" clip-path="url(#changeScatterClip)">${dots}</g></svg></div>`
}

// 变化分析只比较 TIFF 类型的结果文件；以下为文件标签与差值色标（对称范围 ±limit）。
function changeResultFiles() {
  return state.resultFiles.filter((file) => file.kind === 'tiff')
}

function changeFileLabel(file) {
  return file?.observationLabel ? `${file.name} · ${file.observationLabel}` : file?.name || ''
}

function changeDifferenceColorbar(limit) {
  return `<div class="result-colorbar change-colorbar" role="img" aria-label="B 减 A 差值色标"><div class="result-colorbar-title">B − A · P2–P98</div><div class="result-colorbar-scale"><div class="result-colorbar-gradient"></div><div class="result-colorbar-labels"><span>${escapeHtml(formatColorbarValue(-limit))}</span><span>0</span><span>${escapeHtml(formatColorbarValue(limit))}</span></div></div></div>`
}

// 作用：绑定变化分析的 A/B 文件、波段、交换按钮；改变 A 或 B 时波段重置为 0，随后重新渲染。
function bindChangeAnalysisControls() {
  const update = (key, value) => {
    state.analysisSelection.change = {
      ...(state.analysisSelection.change || {}),
      [key]: value,
      ...(['firstPath', 'secondPath'].includes(key) ? { band: '0' } : {})
    }
    renderChangeAnalysis()
  }
  $('#changeFirst')?.addEventListener('change', (event) => update('firstPath', event.target.value))
  $('#changeSecond')?.addEventListener('change', (event) => update('secondPath', event.target.value))
  $('#changeBand')?.addEventListener('change', (event) => update('band', event.target.value))
  $('#changeSwap')?.addEventListener('click', () => {
    const selection = state.analysisSelection.change || {}
    state.analysisSelection.change = { ...selection, firstPath: selection.secondPath, secondPath: selection.firstPath }
    renderChangeAnalysis()
  })
}

// 作用：渲染“变化分析”（逐像元 B − A 差异图 + 统计指标 + A/B 散点图）。
// 说明：changeAnalysisRequest 为请求序号，异步返回时若已有新请求或已离开该分析则丢弃旧结果。
// 步骤：1 选定 A/B 文件与波段；2 生成控件与占位卡片；3 调服务端比较；4 填充指标并绘制差值图。
async function renderChangeAnalysis() {
  disposeResultViewer()
  // 步骤 1：A 缺省取第一个 TIFF；B 缺省取与 A 不同的第一个；A/B 组合变化时波段回到 0。
  const requestId = ++state.changeAnalysisRequest
  const files = changeResultFiles()
  const previous = state.analysisSelection.change || {}
  const first = files.find((file) => file.path === previous.firstPath) || files[0]
  const second = files.find((file) => file.path === previous.secondPath && file.path !== first?.path)
    || files.find((file) => file.path !== first?.path)
  const pairChanged = first?.path !== previous.firstPath || second?.path !== previous.secondPath
  const band = pairChanged ? 0 : Math.max(0, Math.floor(Number(previous.band) || 0))
  state.analysisSelection.change = { firstPath: first?.path || '', secondPath: second?.path || '', band: String(band) }
  renderResultFiles()
  // 步骤 2：生成 A/B/波段下拉框和交换按钮，先显示“正在计算…”占位（少于两个 TIFF 时提示不足）。
  const fileValues = files.map((file) => file.path)
  const fileLabel = (path) => changeFileLabel(files.find((file) => file.path === path))
  const controls = `${analysisSelect('图像 A（基准）', 'changeFirst', fileValues, first?.path || '', fileLabel)}${analysisSelect('图像 B（对比）', 'changeSecond', fileValues, second?.path || '', fileLabel)}${analysisSelect('波段', 'changeBand', [band], band, (value) => `图层 ${Number(value) + 1}`)}<button class="button ghost change-swap" id="changeSwap" type="button">交换 A / B</button>`
  const preview = $('#resultPreview')
  preview.classList.add('analysis-preview')
  preview.innerHTML = `<div class="result-preview-card analysis-card change-analysis-card"><div class="analysis-controls change-controls">${controls}</div><div id="changeAnalysisOutput" class="change-analysis-output"><div class="analysis-empty">${files.length < 2 ? '至少需要两个 TIFF 图像结果。' : '正在计算逐像元差异…'}</div></div></div>`
  preview.scrollTop = 0
  bindChangeAnalysisControls()
  if (!first || !second) {
    $('#resultStatus').textContent = '变化分析至少需要两个 TIFF 图像结果'
    return
  }

  // 步骤 3：请求服务端比较（最后一个参数 4000 疑为散点抽样上限，待核对），
  //   结果含有效像元数、平均差、MAE、RMSE、相关系数 R、差值范围与散点样本。
  $('#resultStatus').textContent = `正在比较 ${first.name} 与 ${second.name}...`
  try {
    const result = await api.compareResults(first.path, second.path, band, 4000)
    if (requestId !== state.changeAnalysisRequest || state.analysisType !== 'change') return
    const bandSelect = $('#changeBand')
    bandSelect.innerHTML = Array.from({ length: result.bands }, (_, index) => analysisOption(index, `${result.bandNames[index] || `波段 ${index + 1}`}（图层 ${index + 1}）`, result.band)).join('')
    const correlation = Number.isFinite(result.correlation) ? formatColorbarValue(result.correlation) : '—'
    $('#changeAnalysisOutput').innerHTML = `<div class="change-metrics"><div><span>有效像元</span><strong>${Number(result.validPixels).toLocaleString('zh-CN')}</strong></div><div><span>平均差值</span><strong>${escapeHtml(formatColorbarValue(result.meanDifference))}</strong></div><div><span>MAE</span><strong>${escapeHtml(formatColorbarValue(result.mae))}</strong></div><div><span>RMSE</span><strong>${escapeHtml(formatColorbarValue(result.rmse))}</strong></div><div><span>相关系数 R</span><strong>${escapeHtml(correlation)}</strong></div><div><span>差值范围</span><strong>${escapeHtml(formatColorbarValue(result.differenceMinimum))} ～ ${escapeHtml(formatColorbarValue(result.differenceMaximum))}</strong></div></div><div class="change-figures"><section><h4>差异图（B − A）</h4><div class="change-difference-stage"><canvas class="result-canvas change-difference-canvas" id="changeDifferenceCanvas" width="${result.previewWidth}" height="${result.previewHeight}"></canvas></div>${changeDifferenceColorbar(result.differenceLimit)}</section><section><h4>像元散点图</h4>${scatterAnalysisSvg(result.scatter)}</section></div><div class="result-meta"><span>${result.width} × ${result.height}</span><span>${escapeHtml(result.bandNames[result.band] || `波段 ${result.band + 1}`)}</span><span>显示范围 P2–P98，统计使用全部有效像元</span><span>散点抽样 ${Number(result.scatter.length).toLocaleString('zh-CN')} 个</span></div>`
    // 步骤 4：differencePixels 为 base64 的 RGBA 预览像素（已按差值色标着色），写入画布。
    const source = atob(result.differencePixels || '')
    const bytes = Uint8ClampedArray.from(source, (character) => character.charCodeAt(0))
    const canvas = $('#changeDifferenceCanvas')
    const context = canvas.getContext('2d')
    const image = context.createImageData(result.previewWidth, result.previewHeight)
    image.data.set(bytes)
    context.putImageData(image, 0, 0)
    $('#resultStatus').textContent = `变化分析 · ${result.bandNames[result.band] || `波段 ${result.band + 1}`} · B − A`
  } catch (error) {
    if (requestId !== state.changeAnalysisRequest) return
    $('#changeAnalysisOutput').innerHTML = `<div class="analysis-empty">${escapeHtml(error.message)}</div>`
    $('#resultStatus').textContent = error.message
  }
}

// 作用：统计分析面板的下拉框变化时，保存当前选择并用同一批 rows 重新渲染。
function bindAnalysisControls(rows) {
  $$('.analysis-control').forEach((control) => control.addEventListener('change', () => {
    state.analysisSelection[state.analysisType] = Object.fromEntries($$('.analysis-control').map((item) => [item.id, item.value]))
    renderStatisticsAnalysis(rows, state.analysisType)
  }))
}

// 作用：渲染基于统计 CSV 行的分析图（角度 / 波段 / 时间三类）并写入结果预览区。
// 输入：rows（statisticsRows 的输出）、type（'angle' | 'band' | 'time'）。
//   每行的 mean 是整幅图像的均值（单位随波段名：反射率、亮温 K、辐亮度等）。
// 说明：用户在下拉框里的选择保存在 state.analysisSelection[type]，选项失效时回退到第一项。
// 步骤：1 选定模式并整理可选的波段/时刻/观测角；2 按 type 生成控件与图表；
//   3 写入卡片、滚动到顶部并绑定控件。
function renderStatisticsAnalysis(rows, type = state.analysisType) {
  disposeResultViewer()
  // 步骤 1：统计文件可能含多个模式的行；优先取与当前工程模式同名的（去掉 'e' 前缀后不分大小写比较），
  //   否则取第一个。波段按序号、观测角按 VZA 再 VAA、时刻按模拟时间先后排序。
  const selection = state.analysisSelection[type] || {}
  const modes = uniqueValues(rows, (row) => row.mode)
  const expectedMode = String(state.mode || '').replace(/^e/, '')
  const mode = modes.find((value) => value.toLowerCase() === expectedMode.toLowerCase()) || modes[0]
  const modeRows = rows.filter((row) => row.mode === mode)
  const bandKeys = uniqueValues(modeRows, statisticBandKey, (a, b) => Number(a.split('|')[0]) - Number(b.split('|')[0]))
  const times = uniqueValues(modeRows, (row) => row.time, (a, b) => statisticTimeOrder(a) - statisticTimeOrder(b))
  const angleKeys = uniqueValues(modeRows, statisticAngleKey, (a, b) => Number(a.split('|')[0]) - Number(b.split('|')[0]) || Number(a.split('|')[1]) - Number(b.split('|')[1]))
  const band = bandKeys.includes(selection.analysisBand) ? selection.analysisBand : bandKeys[0]
  const time = times.includes(selection.analysisTime) ? selection.analysisTime : times[0]
  const angle = angleKeys.includes(selection.analysisAngle) ? selection.analysisAngle : angleKeys[0]
  let controls = '', chart = '', description = '', interactionHint = '鼠标停留在数据点上可查看精确值'

  // 步骤 2a（角度分析）：固定波段与时刻，比较不同观测方向的均值。
  //   可用的显示方式：主平面/垂直主平面折线（main）、半球极坐标补全（hemisphere）；
  //   两者都不满足时退化为按 VAA 分组的 VZA 折线（directions）。
  //   是否半球观测优先用运行设置 hemisphereRequested；缺失时用“方向数 ≥ 100”作旧结果的兜底判断。
  if (type === 'angle') {
    const candidates = modeRows.filter((row) => statisticBandKey(row) === band && row.time === time)
    const mainRows = candidates.map((row) => ({ row, signed: signedPlaneZenith(row, false) })).filter((item) => item.signed != null)
    const perpendicularRows = candidates.map((row) => ({ row, signed: signedPlaneZenith(row, true) })).filter((item) => item.signed != null)
    const hasHemisphere = candidates[0]?.hemisphereRequested ?? (candidates.length >= 100)
    // Cross-section and nadir observations are valid hemisphere samples too.
    const hemisphereRows = hasHemisphere ? candidates : []
    const hasPlanes = candidates[0]?.principalPlaneRequested !== false
      && (mainRows.length > 1 || perpendicularRows.length > 1 || (!hasHemisphere && mainRows.length > 0))
    const available = [...(hasPlanes ? ['main'] : []), ...(hemisphereRows.length ? ['hemisphere'] : [])]
    if (!available.length) available.push('directions')
    const anglePlot = available.includes(selection.analysisAnglePlot) ? selection.analysisAnglePlot : available[0]
    const selectedBandRow = candidates[0] || modeRows.find((row) => statisticBandKey(row) === band)
    const valueLabel = statisticValueLabel(selectedBandRow)
    const plotControl = available.length > 1 ? analysisSelect('显示方式', 'analysisAnglePlot', available, anglePlot, (value) => ({ main: '主平面与垂直主平面', hemisphere: '半球极坐标 · 角度补全' }[value])) : ''
    controls = `${analysisSelect('波段', 'analysisBand', bandKeys, band, statisticBandLabel)}${analysisSelect('时刻', 'analysisTime', times, time, (value) => value ? displaySimulationTime(value) : '静态')}${plotControl}`
    if (anglePlot === 'hemisphere') {
      chart = polarAnalysisSvg(hemisphereRows, valueLabel)
      interactionHint = '方位角按 0–360° 周期补全，颜色为观测方向插值结果'
    }
    else if (anglePlot === 'directions') {
      const azimuths = uniqueValues(candidates, (row) => row.vaa, (a, b) => a - b)
      chart = lineAnalysisSvg(azimuths.map((vaa) => ({
        name: `VAA ${vaa}°`,
        points: candidates.filter((row) => row.vaa === vaa).map((row) => ({
          x: row.vza, y: row.mean,
          label: `VZA ${row.vza}° / VAA ${row.vaa}° · ${valueLabel}均值 ${formatColorbarValue(row.mean)}`
        }))
      })), '观测天顶角 [°]', `${valueLabel} · 整幅图像均值`)
      interactionHint = '按结果中的实际观测方位角分组；未将这些方向标为太阳主平面'
    }
    else {
      const planes = [{ name: '太阳主平面', rows: mainRows }, { name: '垂直主平面', rows: perpendicularRows }]
      chart = lineAnalysisSvg(planes.filter((plane) => plane.rows.length).map((plane) => ({
        name: plane.name,
        points: plane.rows.map(({ row, signed }) => ({ x: signed, y: row.mean, label: `${plane.name} · 有符号天顶角 ${signed}° · VZA ${row.vza}° / VAA ${row.vaa}° · ${valueLabel}均值 ${formatColorbarValue(row.mean)}` }))
      })), '有符号观测天顶角 [°]', `${valueLabel} · 整幅图像均值`)
      interactionHint = '两条曲线按该次运行设置的太阳方位角及其 +90° 划分；时序运行保持观测平面固定，悬停查看实际 VZA / VAA'
    }
    description = `角度分析 · ${statisticBandLabel(band)} · ${time ? displaySimulationTime(time) : '静态'} · 整幅图像均值`
  } else if (type === 'band') {
    // 步骤 2b（波段分析）：固定观测角与时刻，横轴为波长。以 2500 nm 为界拆成光学与热红外两张图
    //   （波长取波段名中最后一个数字，假定单位 nm）；同一张图内物理量不一致时不写具体单位。
    const anglesAtTime = uniqueValues(modeRows.filter((row) => row.time === time), statisticAngleKey, (a, b) => Number(a.split('|')[0]) - Number(b.split('|')[0]) || Number(a.split('|')[1]) - Number(b.split('|')[1]))
    const selectedAngle = anglesAtTime.includes(angle) ? angle : anglesAtTime[0]
    const candidates = modeRows.filter((row) => row.time === time && statisticAngleKey(row) === selectedAngle)
    controls = `${analysisSelect('观测角', 'analysisAngle', anglesAtTime, selectedAngle, statisticAngleLabel)}${analysisSelect('时刻', 'analysisTime', times, time, (value) => value ? displaySimulationTime(value) : '静态')}`
    const wavelengthRanges = [
      { label: '光学波段（≤ 2500 nm）', rows: candidates.filter((row) => numericBandPosition(row) <= 2500) },
      { label: '热红外波段（> 2500 nm）', rows: candidates.filter((row) => numericBandPosition(row) > 2500) }
    ].filter((range) => range.rows.length)
    chart = wavelengthRanges.map((range) => {
      const quantities = uniqueValues(range.rows, statisticQuantity)
      const valueLabel = quantities.length === 1 ? (statisticQuantityLabels[quantities[0]] || '结果值') : '结果值'
      const figure = lineAnalysisSvg([{ name: range.label, points: range.rows.map((row) => ({ x: numericBandPosition(row), y: row.mean, label: `${row.bandName} · 整幅图像均值 ${formatColorbarValue(row.mean)}` })) }], '波长 [nm]', `${valueLabel} · 整幅图像均值`)
      return `<section class="analysis-chart-section"><h4>${range.label}</h4>${figure}</section>`
    }).join('')
    description = `波段分析 · ${statisticAngleLabel(selectedAngle)} · ${time ? displaySimulationTime(time) : '静态'} · 整幅图像均值`
  } else {
    // 步骤 2c（时间分析）：固定波段与观测角，横轴为模拟时间；时间轴是序数轴，
    //   刻度文字取离刻度值最近的数据点的时间标签。
    const selectedRows = modeRows.filter((row) => statisticBandKey(row) === band && statisticAngleKey(row) === angle).sort((a, b) => statisticTimeOrder(a.time) - statisticTimeOrder(b.time))
    const selectedBandRow = selectedRows[0] || modeRows.find((row) => statisticBandKey(row) === band)
    const valueLabel = statisticValueLabel(selectedBandRow)
    controls = `${analysisSelect('波段', 'analysisBand', bandKeys, band, statisticBandLabel)}${analysisSelect('观测角', 'analysisAngle', angleKeys, angle, statisticAngleLabel)}`
    chart = lineAnalysisSvg([{ name: '时间变化', points: selectedRows.map((row) => ({ x: statisticTimeOrder(row.time), y: row.mean, label: `${displaySimulationTime(row.time)} · ${valueLabel}均值 ${formatColorbarValue(row.mean)}` })) }], '模拟时刻', `${valueLabel} · 整幅图像均值`, (value) => {
      const row = selectedRows.reduce((closest, item) => !closest || Math.abs(statisticTimeOrder(item.time) - value) < Math.abs(statisticTimeOrder(closest.time) - value) ? item : closest, null)
      return row ? displaySimulationTime(row.time) : ''
    })
    description = `时间分析 · ${statisticBandLabel(band)} · ${statisticAngleLabel(angle)} · 整幅图像均值`
  }
  // 步骤 3：写入分析卡片；滚动位置在下一帧再次置顶（避免内容重排后位置被浏览器恢复）。
  const preview = $('#resultPreview')
  preview.classList.add('analysis-preview')
  preview.innerHTML = `<div class="result-preview-card analysis-card"><div class="analysis-controls">${controls}</div>${chart}<div class="result-meta"><span>${escapeHtml(description)}</span><span>${escapeHtml(interactionHint)}</span></div></div>`
  preview.scrollTop = 0
  requestAnimationFrame(() => { preview.scrollTop = 0 })
  bindAnalysisControls(rows)
}

// 作用：把统计 CSV 结果（result.content 文本、result.observation 观测设置）解析后绘制分析图；
//   没有有效行时提示先运行图像模拟。
function drawStatisticsAnalysis(result) {
  const rows = statisticsRows(result.content, result.observation)
  if (!rows.length) {
    emptyResultPreview('统计结果为空', '请先运行图像模拟，系统会自动生成带模型后缀的统计 CSV。')
    return
  }
  renderStatisticsAnalysis(rows, state.analysisType)
}


// 作用：读取并显示一个结果文件（ENVI/TIFF/风场/面元/体元/图像/CSV 等），按类型分派预览函数。
// 输入：file（列表项）、band（多波段影像的波段序号，从 0 起）。
// 说明：openResultFile 可能被快速连续调用，openRequest 序号与 stale() 用于丢弃过期或对话框已关闭
//   的异步结果。
// 步骤：1 读取；2 兼容旧版面元结果；3 按 kind 分派绘制；4 绑定“用系统程序打开”并更新状态栏。
async function openResultFile(file, band = 0) {
  if (!file) return
  const openRequest = ++state.resultOpenRequest
  const stale = () => openRequest !== state.resultOpenRequest || $('#resultDialog').hidden
  try {
    const displayName = resultFileDisplayName(file)
    state.selectedResult = file
    $$('.result-file').forEach((button) => button.classList.toggle('active', Number(button.dataset.index) === state.resultFiles.indexOf(file)))
    $('#resultStatus').textContent = `正在读取 ${displayName}...`
    let result = await api.readResult(file.path, band)
    if (stale()) return
    // 步骤 2：旧版面元结果没有 kind，只给 radiosity / lightEnhancement / sunlit 三个数组；
    //   这里包装成 kind='facet' 与 metrics 列表，使其走新的面元三维视图。
    if (!result.kind && Number.isInteger(Number(result.facetCount)) &&
        isNumberArray(result.radiosity) && isNumberArray(result.lightEnhancement) &&
        isNumberArray(result.sunlit)) {
      const metrics = [
        { id: 'radiosity', label: 'Radiosity', values: result.radiosity },
        { id: 'lightEnhancement', label: 'Light enhancement', values: result.lightEnhancement },
        { id: 'sunlit', label: 'Sunlit fraction', values: result.sunlit }
      ]
      delete result.radiosity; delete result.lightEnhancement; delete result.sunlit
      result = { ...result, kind: 'facet', path: file.path, metrics }
    }
    // 步骤 3：按 kind 选择预览方式；统计 CSV 仅在“分析”视图（且不是三维分析）下按图表绘制，
    //   其余文本直接显示原文；未知类型提示用外部程序打开。
    if (result.kind === 'envi') await drawEnviPreview(result, file)
    else if (result.kind === 'wind') drawWindPreview(result, file)
    else if (result.kind === 'tiff') await drawTiffPreview(result, file)
    else if (result.kind === 'facet') showFacet3D(result, file)
    else if (result.kind === 'voxel') showVoxel3D(result, file)
    else if (result.kind === 'image') await drawImagePreview(result, file)
    else if (result.kind === 'text' && state.resultView === 'analysis' && state.analysisType !== '3d') drawStatisticsAnalysis(result)
    else if (result.kind === 'text') { disposeResultViewer(); state.resultRaster = null; $('#resultPreview').innerHTML = `<div class="result-preview-card"><pre class="result-text">${escapeHtml(result.content)}</pre><button class="button ghost result-open-external" type="button">用系统程序打开</button></div>` }
    else { disposeResultViewer(); state.resultRaster = null; $('#resultPreview').innerHTML = `<div class="empty-editor"><svg><use href="#i-layers"/></svg><h3>暂不支持内置预览</h3><p>可在资源管理器中定位并使用外部程序打开该文件。</p><button class="button ghost result-open-external" type="button">用系统程序打开</button></div>` }
    $('#resultPreview').querySelector('.result-open-external')?.addEventListener('click', () => api.openPath(file.path))
    // 步骤 4：状态栏文字按结果类型给出简短说明（未列出的类型直接显示文件名）。
    $('#resultStatus').textContent = result.kind === 'envi' ? `ENVI 浮点影像 · ${displayName}` : result.kind === 'wind' ? `自适应风速箭头图 · ${displayName}` : result.kind === 'tiff' ? `TIFF 栅格影像 · ${displayName}` : result.kind === 'facet' ? `面元结果 · ${displayName}` : result.kind === 'text' && state.resultView === 'analysis' ? `${({ angle: '角度', band: '波段', time: '时间' })[state.analysisType] || ''}统计分析 · ${displayName}` : result.kind === 'image' ? `图像结果 · ${displayName}` : displayName
  } catch (error) {
    if (stale()) return
    emptyResultPreview('结果读取失败', error.message)
    $('#resultStatus').textContent = error.message
  }
}

// 作用：判断当前配置是否输出了三维过程结果。能量平衡模式（FacetEB/VoxelEB）
//   辐射过程或能量过程任一开启即可；其他模式只看辐射过程开关。
function threeDimensionalOutputEnabled() {
  const energyMode = state.mode === 'eFacetEB' || state.mode === 'eVoxelEB'
  return energyMode
    ? Boolean(state.config?.sensor?.radiationProcess || state.config?.sensor?.energyProcess)
    : Boolean(state.config?.sensor?.radiationProcess)
}

// 作用：按当前视图筛选结果文件：图像视图 = tiff/envi/image；变化分析 = tiff；
//   三维分析 = facet/process（未启用过程输出时为空）；其余统计分析 = result_statistics*.csv。
function resultFilesForView() {
  if (state.resultView === 'image') return state.resultFiles.filter((file) => ['tiff', 'envi', 'image'].includes(file.kind))
  if (state.analysisType === 'change') return changeResultFiles()
  if (state.analysisType === '3d') return threeDimensionalOutputEnabled() ? state.resultFiles.filter((file) => ['facet', 'process'].includes(file.kind)) : []
  return state.resultFiles.filter((file) => file.kind === 'text' && /(?:^|[\\/])result_statistics(?:_[a-z0-9-]+)?\.csv$/i.test(file.path))
}

// 作用：结果文件显示名；体元流体力学过程文件显示为“体元流体力学 · 时刻”，其余用文件名。
function resultFileDisplayName(file) {
  if (file?.kind === 'process' && file.processType === 'fluid') {
    return `体元流体力学 · ${file.processTime || '静态'}`
  }
  return file?.name || ''
}

// 作用：按当前视图（图像/分析）和分析类型刷新结果对话框的标题与标签页高亮。
function updateResultViewHeader() {
  const analysis = state.resultView === 'analysis'
  $('#resultDialogTitle').textContent = analysis ? '分析' : '图像结果'
  $('#resultImageTab').classList.toggle('active', !analysis)
  $('#resultAnalysisTab').classList.toggle('active', analysis)
  $('#resultAnalysisTabs').hidden = !analysis
  const tabs = { angle: '#angleAnalysisTab', band: '#bandAnalysisTab', time: '#timeAnalysisTab', change: '#changeAnalysisTab', '3d': '#threeAnalysisTab' }
  for (const [type, selector] of Object.entries(tabs)) $(selector).classList.toggle('active', analysis && state.analysisType === type)
}

// 作用：切换结果对话框的“图像结果 / 分析”视图；先释放旧三维视图并清空选中结果，
//   openFirst 为真时自动打开该视图的第一个文件（变化分析则直接渲染）。
async function setResultView(view, openFirst = true) {
  disposeResultViewer()
  state.resultRaster = null
  state.resultView = view === 'analysis' ? 'analysis' : 'image'
  state.selectedResult = null
  updateResultViewHeader()
  renderResultFiles()
  if (state.resultView === 'analysis' && state.analysisType === 'change') {
    if (openFirst) await renderChangeAnalysis()
    return
  }
  const first = resultFilesForView()[0]
  if (openFirst && first) await openResultFile(first)
}

// 作用：切换分析类型（angle/band/time/change/3d，非法值回退为 angle），其余同 setResultView。
async function setAnalysisType(type, openFirst = true) {
  disposeResultViewer()
  state.resultRaster = null
  state.resultView = 'analysis'
  state.analysisType = ['angle', 'band', 'time', 'change', '3d'].includes(type) ? type : 'angle'
  state.selectedResult = null
  updateResultViewHeader()
  renderResultFiles()
  if (state.analysisType === 'change') {
    if (openFirst) await renderChangeAnalysis()
    return
  }
  const first = resultFilesForView()[0]
  if (openFirst && first) await openResultFile(first)
}

// 作用：渲染左侧结果文件列表；当前视图没有文件时显示空状态与所需前置条件。
// 说明：变化分析中点击文件 = 设为 B（点到 A 本身时不变），并把波段重置为 0。
function renderResultFiles() {
  const visibleFiles = resultFilesForView()
  if (!visibleFiles.length) {
    const analysis = state.resultView === 'analysis'
    const is3d = analysis && state.analysisType === '3d'
    const processDisabled = is3d && !threeDimensionalOutputEnabled()
    const analysisLabel = ({ angle: '角度', band: '波段', time: '时间', change: '变化', '3d': '三维' })[state.analysisType]
    const requirement = processDisabled
      ? '请在仿真模拟中启用对应的过程输出。'
      : analysis
        ? ({ angle: '需要“图像输出”和统计 CSV，且至少两个观测角度。', band: '需要“图像输出”和统计 CSV，且至少两个波段。', time: '需要“图像输出”和统计 CSV，且至少两个时间节点。', change: '需要输出目录中至少两幅 TIFF 图像结果。', '3d': '需要过程输出（辐射过程或能量过程）。' })[state.analysisType]
        : '请先运行模拟，然后点击刷新。'
    document.querySelector('#resultFiles').innerHTML = `<div class="result-empty-list">${processDisabled ? '当前模式未启用过程输出。' : analysis ? `尚无${analysisLabel}分析结果。` : '尚无图像结果。'}<br>${requirement}</div>`
    emptyResultPreview(processDisabled ? '尚无三维结果' : analysis ? `尚无${analysisLabel}分析数据` : '尚无图像结果', requirement)
    return
  }
  $('#resultFiles').innerHTML = visibleFiles.map((file) => {
    const index = state.resultFiles.indexOf(file)
    const displayName = t(resultFileDisplayName(file))
    const changeSelection = state.analysisSelection.change || {}
    const changeRole = state.resultView === 'analysis' && state.analysisType === 'change'
      ? (file.path === changeSelection.firstPath ? 'A' : file.path === changeSelection.secondPath ? 'B' : '') : ''
    const extension = changeRole || (file.resultType === 'wind' ? 'WIND' : file.kind === 'envi' ? 'ENVI' : file.kind === 'tiff' ? 'TIFF' : file.kind === 'facet' ? 'FACET' : file.kind === 'text' ? 'CSV' : file.name.split('.').pop())
    const time = new Date(file.modifiedAt).toLocaleString('zh-CN', { hour12: false })
    return `<button class="result-file ${changeRole ? 'active' : ''}" data-index="${index}"><span class="result-file-icon">${escapeHtml(extension)}</span><span><strong title="${escapeHtml(displayName)}">${escapeHtml(displayName)}</strong><small>${file.observationLabel ? escapeHtml(file.observationLabel.replaceAll('_to_', ' → ').replaceAll('_', ' ')) + ' · ' : ''}${formatBytes(file.size)} · ${time}</small></span></button>`
  }).join('')
  $$('.result-file').forEach((button) => button.addEventListener('click', () => {
    const file = state.resultFiles[Number(button.dataset.index)]
    if (state.resultView === 'analysis' && state.analysisType === 'change') {
      const selection = state.analysisSelection.change || {}
      if (file.path !== selection.firstPath) state.analysisSelection.change = { ...selection, secondPath: file.path, band: '0' }
      renderResultFiles()
      renderChangeAnalysis()
    } else openResultFile(file)
  }))
}

// 作用：重新列出输出目录中的结果文件并刷新界面；已选结果仍存在则重新打开，
//   否则（openFirst 为真时）打开第一个。
async function refreshResults(openFirst = false) {
  if (!state.outputDir) throw new Error('当前工程未配置输出目录')
  const result = await api.listResults(state.outputDir)
  state.outputDir = result.directory
  state.resultFiles = result.files
  $('#resultDirectory').textContent = result.directory
  renderResultFiles()
  if (state.resultView === 'analysis' && state.analysisType === 'change') {
    await renderChangeAnalysis()
    return
  }
  const visibleFiles = resultFilesForView()
  const selected = state.selectedResult && visibleFiles.find((file) => file.path === state.selectedResult.path)
  if (selected) await openResultFile(selected)
  else if (openFirst && visibleFiles.length) await openResultFile(visibleFiles[0])
}

// 作用：删除全部结果：三维分析视图删除三维输出，其他视图删除 TIFF 图像结果；
//   删除后刷新列表并提示删除个数。
async function deleteAllResults() {
  try {
    const kind = state.resultView === 'analysis' && state.analysisType === '3d' ? '3d' : 'image'
    const result = await api.deleteResults(state.outputDir, kind)
    disposeResultViewer()
    state.selectedResult = null
    state.resultRaster = null
    await refreshResults(false)
    $('#resultStatus').textContent = '已删除 ' + result.deleted + (kind === '3d' ? ' 个三维输出' : ' 个 TIFF 图像结果')
    addLog('已删除 ' + result.deleted + (kind === '3d' ? ' 个三维输出' : ' 个 TIFF 图像结果'), 'success')
    toast(kind === '3d' ? '三维结果已删除' : '图像结果已删除', result.deleted + ' 个文件')
  } catch (error) {
    toast('删除结果失败', error.message, 'error')
  }
}

// 作用：打开“结果”对话框。view 取 'image'（图像结果）、'analysis'（分析，可带 analysisType）
//   或 '3d'（三维分析）；随后读取输出目录并自动打开第一个结果，失败时在对话框内提示原因。
//   未打开工程时直接提示并返回。
async function showResults(view = 'image', analysisType = null) {
  if (!state.inputPath) { toast('无法查看结果', '请先新建或打开工程', 'error'); return }
  $('#resultDialog').hidden = false
  if (view === '3d') { state.resultView = 'analysis'; state.analysisType = '3d' }
  else if (view === 'analysis') { state.resultView = 'analysis'; if (analysisType) state.analysisType = analysisType }
  else state.resultView = 'image'
  state.selectedResult = null
  updateResultViewHeader()
  try {
    await refreshResults(true)
  } catch (error) {
    state.resultFiles = []
    $('#resultFiles').innerHTML = '<div class="result-empty-list">无法读取输出目录。</div>'
    emptyResultPreview('无法打开模拟结果', error.message)
    $('#resultStatus').textContent = error.message
  }
}

// 作用：关闭结果对话框；resultOpenRequest 加 1 使仍在读取的异步结果作废，并释放三维视图。
function hideResults() {
  state.resultOpenRequest += 1
  disposeResultViewer()
  state.resultRaster = null
  $('#resultDialog').hidden = true
}

// 作用：切换计算模式（RayTracing/VoxelRT/VoxelEB/FacetRT/FacetEB 等）并刷新界面。
// 说明：announce 为真表示由用户操作触发，此时才重映射 GPU 序号、标记工程为未保存、写日志。
//   从面元类模式（含 eRaytracing）切到体元模式视为“面元 → 体元”转换：
//   在 project.voxelization 记录源/目标模式与体元大小（scene.voxel，缺省 1 m），
//   并打开体元预览；OBJ、材质和位置保持不变，体元在运行时稀疏生成。
function selectMode(mode, announce = false) {
  const button = $(`.mode-card[data-mode="${mode}"]`)
  if (!button) return
  const previousMode = state.mode
  const facetMode = previousMode === 'eFacetRT' || previousMode === 'eFacetEB' || previousMode === 'eRaytracing'
  const voxelMode = mode === 'eVoxelRT' || mode === 'eVoxelEB'
  const converted = facetMode && voxelMode
  state.mode = mode
  $$('.mode-card').forEach((item) => item.classList.toggle('active', item === button))
  renderNavBadges()
  if (state.project) {
    state.project.mode = mode
    if (converted) state.project.voxelization = { sourceMode: previousMode, targetMode: mode, voxelSize: Number(state.config?.scene?.voxel) || 1, updatedAt: new Date().toISOString() }
  }
  if (converted) {
    $('#voxelBtn').classList.add('active')
    if (voxelPreview) voxelPreview.visible = true
  }
  if (announce) {
    remapGpuIndex(previousMode, mode)
    if (!state.running) setRunning(false)
    if (state.project) { markXmlDirty(); $('#unsavedMark').hidden = false }
    addLog(converted ? `面元场景已接入${button.querySelector('span').textContent}，体元大小 ${Number(state.config?.scene?.voxel) || 1} m` : `计算模式切换为 ${button.querySelector('span').textContent}`)
    if (converted) toast('面元 → 体元通道已启用', 'OBJ、材质和位置保持不变，运行时生成稀疏体元')
    if (state.project) followQualityPresetMode()
    renderInspector()
  }
}

function showProjectDialog() {
  $('#newProjectMode').value = state.mode
  $('#projectDirectory').value = appStorage.getItem('histreamProjectDirectory') || ''
  $('#projectDialog').hidden = false
  setTimeout(() => ($('#projectDirectory').value ? $('#newProjectName') : $('#projectDirectory')).focus(), 0)
}

function hideProjectDialog() { $('#projectDialog').hidden = true }

// 作用：打开“另存为”对话框。运行中或未打开工程时拒绝；默认保存到当前工程目录的上一级，
//   默认名称为“原名_copy”。
function showSaveAsDialog() {
  if (!state.project || !state.inputPath) {
    toast('无法另存为', '请先新建或打开工程', 'error')
    return
  }
  if (state.running) {
    toast('无法另存为', '模拟运行中，请停止后再试', 'error')
    return
  }
  const sourceDirectory = state.inputPath.replace(/[\\/][^\\/]+$/, '')
  const parentDirectory = sourceDirectory.replace(/[\\/][^\\/]+$/, '') || sourceDirectory
  $('#saveAsDirectory').value = parentDirectory
  $('#saveAsName').value = `${state.project.name || fileName(sourceDirectory)}_copy`
  $('#saveAsSourcePath').textContent = sourceDirectory
  $('#saveAsDialog').hidden = false
  setTimeout(() => { $('#saveAsName').focus(); $('#saveAsName').select() }, 0)
}

function hideSaveAsDialog() { $('#saveAsDialog').hidden = true }

function showReferenceDialog() {
  $('#referenceDialog').hidden = false
  $('#closeReferenceBtn').focus()
}

function hideReferenceDialog() {
  $('#referenceImageDialog').hidden = true
  $('#referenceImage').removeAttribute('src')
  $('#referenceDialog').hidden = true
}

function hideHistreamProxyDialog() {
  $('#histreamProxyDialog').hidden = true
}

// 以下为 histream-proxy（代理/替代模型）对话框：用训练时捕获的登记帧生成数值影像，
//   不调用 histream.exe（原生引擎）。
// 作用：读取下拉框选中的捕获帧；选项 value 是 JSON [episode_id, frame_index]。
function selectedHistreamProxyFrame() {
  const value = $('#histreamProxyFrameSelect').value
  if (!value) throw new Error('请选择训练捕获帧')
  const [episodeId, frameIndex] = JSON.parse(value)
  if (!episodeId || !Number.isInteger(frameIndex)) throw new Error('捕获帧选项无效')
  return { episodeId, frameIndex }
}

// 作用：显示当前所选波段的预览图与标题（波段名 · 单位 · 宽 × 高；shape 为 [高, 宽]）。
function showHistreamProxyBand() {
  const result = histreamProxyState.result
  if (!result) return
  const band = $('#histreamProxyBandSelect').value
  const preview = result.previews?.[band]
  if (!preview) return
  const index = result.band_names.indexOf(band)
  const units = result.band_units[index] || ''
  const image = $('#histreamProxyPreview')
  image.src = preview
  image.alt = `${band} ${units} preview for ${result.episode_id} frame ${result.frame_index}`
  image.hidden = false
  $('#histreamProxyPreviewTitle').textContent = `${band} · ${units} · ${result.shape[1]} × ${result.shape[0]}`
}

// 作用：打开代理对话框并向服务端查询部署清单（api.histreamProxyInfo）。
//   未就绪（info.ready 为假）时只显示原因，不启用运行按钮；就绪时用登记帧填充下拉框。
async function openHistreamProxyDialog() {
  const dialog = $('#histreamProxyDialog')
  dialog.hidden = false
  $('#closeHistreamProxyBtn').focus()
  $('#histreamProxyScope').textContent = '正在检查 histream-proxy 部署清单…'
  $('#histreamProxyStatus').textContent = '正在读取固定场景和模型状态。'
  $('#histreamProxyFrameSelect').disabled = true
  $('#runHistreamProxyBtn').disabled = true
  $('#downloadHistreamProxyBtn').hidden = true
  $('#histreamProxyPreview').hidden = true
  $('#histreamProxyPreview').removeAttribute('src')
  $('#histreamProxyRuntime').textContent = ''
  histreamProxyState.info = null
  histreamProxyState.result = null
  try {
    const info = await api.histreamProxyInfo()
    histreamProxyState.info = info
    if (!info.ready) {
      $('#histreamProxyScope').textContent = `代理尚未就绪：${info.reason || '部署清单或运行文件缺失。'}`
      $('#histreamProxyStatus').textContent = '未启动任何模拟引擎。请先完成训练导出和代理部署。'
      return
    }
    const frames = Array.isArray(info.supportedFrames) ? info.supportedFrames : []
    const select = $('#histreamProxyFrameSelect')
    select.replaceChildren()
    for (const frame of frames) {
      const option = document.createElement('option')
      option.value = JSON.stringify([frame.episode_id, Number(frame.frame_index)])
      option.textContent = `${frame.episode_id} · 帧 ${frame.frame_index} · ${frame.hour_bjt}:00 · ${frame.platform} · ${frame.split}`
      select.append(option)
    }
    select.disabled = frames.length === 0
    $('#runHistreamProxyBtn').disabled = frames.length === 0
    $('#histreamProxyScope').textContent = `${info.captureScope}。模型输入为训练时生成的完整三维体及观测几何；当前普通 project.json 不参与推理。共 ${frames.length} 个已登记帧，输出 ${info.shape?.join(' × ')}，波段：${info.bandNames?.join(' / ')}。`
    $('#histreamProxyStatus').textContent = frames.length ? '就绪。选择登记帧后生成数值影像。' : '部署清单没有可用帧。'
    $('#histreamProxyRuntime').textContent = `backend=${info.backend} · Python=${info.pythonExecutable} · runner=${info.runnerPath} · nativeEngineInvoked=${info.nativeEngineInvoked}`
  } catch (error) {
    $('#histreamProxyScope').textContent = `无法读取代理部署：${error.message}`
    $('#histreamProxyStatus').textContent = '代理检查失败；标准引擎状态未改变。'
  }
}

// 作用：对选中的登记帧运行代理预测，并校验返回的状态：status 必须为 'ok'、
//   backend 为 'histream-proxy'、native_engine_invoked 为 false，否则按失败处理。
//   成功后显示波段预览与下载链接（npz）；无论成败都在 finally 中恢复按钮状态。
async function runHistreamProxyFrame() {
  const button = $('#runHistreamProxyBtn')
  const select = $('#histreamProxyFrameSelect')
  button.disabled = true
  $('#histreamProxyStatus').textContent = '正在遍历完整三维体并合成波段…'
  $('#downloadHistreamProxyBtn').hidden = true
  try {
    const frame = selectedHistreamProxyFrame()
    const result = await api.runHistreamProxy({
      captureId: histreamProxyState.info?.captureId,
      episodeId: frame.episodeId,
      frameIndex: frame.frameIndex
    })
    if (result.status !== 'ok' || result.backend !== 'histream-proxy' || result.native_engine_invoked !== false) {
      throw new Error('代理返回的状态或后端标识无效')
    }
    histreamProxyState.result = result
    showHistreamProxyBand()
    const download = $('#downloadHistreamProxyBtn')
    download.href = result.downloadUrl
    download.download = `${result.download_id}.npz`
    download.hidden = false
    $('#histreamProxyStatus').textContent = `完成：${result.episode_id} 帧 ${result.frame_index} · ${result.shape.join(' × ')} · 命中 ${result.hit_pixels.toLocaleString()} 像素 · 未调用 histream.exe。`
    $('#histreamProxyRuntime').textContent = `split=${result.split} · route=${result.route_id} · thermal=${result.thermal_state} · solar=${result.solar_source || 'engine SPA'} · nativeEngineInvoked=${result.native_engine_invoked}`
  } catch (error) {
    $('#histreamProxyStatus').textContent = `预测失败：${error.message}`
    $('#histreamProxyPreview').hidden = true
    toast('histream-proxy 失败', error.message, 'error')
  } finally {
    button.disabled = !histreamProxyState.info?.ready || !select.value
  }
}

// 作用：放大显示参考图例（取按钮内 img 的 src/alt，标题取 data-reference-title）。
function showReferenceImage(button) {
  const image = button.querySelector('img')
  if (!image) return
  $('#referenceImage').src = image.src
  $('#referenceImage').alt = image.alt
  $('#referenceImageTitle').textContent = button.dataset.referenceTitle || image.alt || '图例预览'
  $('#referenceImageDialog').hidden = false
  $('#closeReferenceImageBtn').focus()
}

function hideReferenceImage() {
  $('#referenceImageDialog').hidden = true
  $('#referenceImage').removeAttribute('src')
}

// 作用：把当前工程另存为新目录并切换到新工程。
// 说明：保存前先同步 XML 文本；generation（工程代次）与 revision（编辑版本）用于检测
//   异步保存期间用户是否换了工程或又改了内容——若已变化，只提示已另存，不加载结果、不改当前工程。
// 步骤：1 取表单值并克隆当前配置/工程；2 补全 DEM 信息；3 调服务端另存为；
//   4 记住父目录、加载新工程。
async function saveProjectAs(event) {
  event.preventDefault()
  const submit = $('#saveAsForm button[type="submit"]')
  const values = Object.fromEntries(new FormData($('#saveAsForm')).entries())
  flushProjectTextSync()
  const generation = state.projectGeneration
  const revision = state.editRevision
  const sourcePath = state.inputPath
  const isCurrent = () => state.projectGeneration === generation && state.inputPath === sourcePath
  try {
    submit.disabled = true
    const config = structuredClone(state.xmlManualEdit || !state.config ? ensureMaterialPresets(parseXml(state.xmlText, state.mode)) : state.config)
    const project = structuredClone(state.project)
    project.configuration = config
    project.mode = state.mode
    await ensureDemInfo(config, sourcePath)
    if (!isCurrent()) return
    const result = await api.saveProjectAs({ ...values, sourcePath, project })
    if (!isCurrent() || state.editRevision !== revision) { addLog(`提交时的工程已另存为：${result.projectDir}；当前工程或后续修改保持不变`, 'warning'); reportUsdSave(result.usd); return }
    appStorage.setItem('histreamProjectDirectory', values.directory)
    hideSaveAsDialog()
    loadXml(result)
    addLog(`工程已另存为：${result.projectDir}`, 'success')
    reportUsdSave(result.usd)
    toast('另存为成功', result.project.name)
  } catch (error) {
    toast('另存为失败', error.message, 'error')
    addLog(`工程另存为失败：${error.message}`, 'error')
  } finally { submit.disabled = false }
}

// 作用：按“新建工程”表单（名称、目录、模式）创建工程，成功后记住目录并加载。
async function createProject(event) {
  event.preventDefault()
  if (!confirmDiscardChanges('新建工程')) return
  const submit = $('#projectForm button[type="submit"]')
  const values = Object.fromEntries(new FormData($('#projectForm')).entries())
  try {
    submit.disabled = true
    const result = await api.createProject(values)
    appStorage.setItem('histreamProjectDirectory', values.directory)
    loadXml(result, false)
    hideProjectDialog()
    addLog(`已创建工程：${result.projectDir}`, 'success')
    toast('工程创建成功', result.project.name)
  } catch (error) { toast('创建工程失败', error.message, 'error') }
  finally { submit.disabled = false }
}

// 模块：界面事件绑定（模块加载时一次性执行，按界面区域分组）。
// 分组 1：计算模式卡片、计算设备/CPU 线程、导航树、视图样式、网格与体元预览按钮。
//   设备下拉框的取值：'gpu:<序号>' 表示 GPU，其余值直接作为后端名；
//   同时写入 control.computeBackend 与 control.facetBackend，然后更新 XML 文本。
$$('.mode-card').forEach((button) => button.addEventListener('click', () => selectMode(button.dataset.mode, true)))
$('#computeDevice').addEventListener('change', (event) => {
  const control = state.config?.control
  if (!control || !event.target.value) return
  const value = event.target.value
  const backend = value.startsWith('gpu:') ? 'gpu' : value
  if (backend === 'gpu') control.gpu = Number(value.slice(4))
  control.computeBackend = backend
  control.facetBackend = backend
  syncProjectText()
  renderComputeDevices()
  addLog(`计算设备：${event.target.selectedOptions[0]?.textContent || value}`)
})
$('#cpuThreads').addEventListener('change', (event) => {
  const control = state.config?.control
  if (!control) return
  control.cpuThreads = normalizeCpuThreads(event.target.value)
  syncProjectText()
  renderComputeDevices()
})
$('#refreshDevicesBtn').addEventListener('click', () => loadComputeDevices(true))
$$('.tree-item').forEach((button) => button.addEventListener('click', () => { state.panel = button.dataset.panel; $$('.tree-item').forEach((item) => item.classList.toggle('active', item === button)); renderInspector() }))
$$('#viewStyle button').forEach((button) => button.addEventListener('click', () => { $$('#viewStyle button').forEach((item) => item.classList.toggle('active', item === button)); applyViewStyle(button.dataset.style) }))
$('#gridBtn').addEventListener('click', () => { if (!grid) return; grid.visible = !grid.visible; $('#gridBtn').classList.toggle('active', grid.visible) })
// 体元预览开关：已显示则关闭；否则调用 buildVoxelPreview，场景无可体元化对象时提示。
$('#voxelBtn').addEventListener('click', () => {
  if (voxelPreview?.visible) {
    voxelPreview.visible = false
    $('#voxelBtn').classList.remove('active')
    return
  }
  try {
    const summary = buildVoxelPreview()
    if (!summary) { toast('体元预览', '场景中没有可体元化的对象', 'warning'); return }
    $('#voxelBtn').classList.add('active')
    toast('体元预览', summary)
    addLog(`体元预览 · ${summary}`)
  } catch (error) {
    toast('体元预览失败', error.message, 'error')
  }
})
// 分组 2：场景对象工具——工具菜单（修改/添加子菜单）、绘制区域/方块、对象编辑开关；
//   点击菜单之外的任何位置关闭菜单。
$('#sceneObjectToolsBtn').addEventListener('click', (event) => {
  event.stopPropagation()
  const menu = $('#sceneObjectToolsMenu')
  if (menu.hidden) {
    menu.hidden = false
    $('#sceneObjectToolsBtn').setAttribute('aria-expanded', 'true')
  } else closeSceneObjectToolsMenu()
})
$('#sceneObjectToolsMenu').addEventListener('click', (event) => event.stopPropagation())
$('#sceneObjectModifyBtn').addEventListener('click', () => toggleSceneObjectSubmenu('Modify'))
$('#sceneObjectAddBtn').addEventListener('click', () => toggleSceneObjectSubmenu('Add'))
$('#drawRegionBtn').addEventListener('click', () => startGeometryDraw('region'))
$('#drawBoxBtn').addEventListener('click', () => startGeometryDraw('box'))
$('#editObjectBtn').addEventListener('click', () => {
  if (!objectEdit.enabled) setObjectEditEnabled(true)
  else { clearObjectSelection(); closeSceneObjectToolsMenu() }
})
document.addEventListener('pointerdown', (event) => {
  if (!event.target.closest?.('.scene-object-tools')) closeSceneObjectToolsMenu()
})
// 对象变换模式按钮（移动/旋转/缩放）：未处于编辑状态时先启用编辑。
// 以下 transformControls 为 three.js 变换控件的事件：拖动时临时关闭轨道相机控制。
$$('#transformModeButtons button').forEach((button) => button.addEventListener('click', () => {
  if (!objectEdit.enabled) setObjectEditEnabled(true)
  setObjectTransformMode(button.dataset.transformMode)
}))
transformControls.addEventListener('dragging-changed', (event) => {
  controls.enabled = !event.value
  if (event.value) objectEdit.pointerStart = null
})
transformControls.addEventListener('mouseDown', () => {
  if (objectEdit.selected) objectEdit.transformStartScale.copy(objectEdit.selected.scale)
})
// 拖动过程中的实时修正：缩放模式下按被拖动的轴的比例（拖动整体手柄时取三轴最大比例）
//   对起始缩放做等比缩放，最小因子 0.01，避免出现非等比缩放或零/负尺度；
//   随后 snapGroundedObject 让带地面足迹的对象按地形高度重新贴地，并刷新选择框。
//   鼠标松开（mouseUp）时保存对象变换。
transformControls.addEventListener('objectChange', () => {
  const object = objectEdit.selected
  if (!object) return
  if (transformControls.mode === 'scale') {
    const start = objectEdit.transformStartScale
    const ratios = {
      X: object.scale.x / Math.max(1e-9, start.x),
      Y: object.scale.y / Math.max(1e-9, start.y),
      Z: object.scale.z / Math.max(1e-9, start.z)
    }
    const axis = String(transformControls.axis || '')
    const factor = Math.max(.01, Number.isFinite(ratios[axis]) ? ratios[axis] : Math.max(ratios.X, ratios.Y, ratios.Z))
    object.scale.copy(start).multiplyScalar(factor)
  }
  snapGroundedObject(object)
  updateObjectSelectionBox()
})
transformControls.addEventListener('mouseUp', () => saveEditedObjectTransform(objectEdit.selected))
// 三维画布的指针事件：绘制几何（捕获阶段，优先于相机控制）与对象选择/双击编辑；
//   绘制中指针取消、右键或 Esc 都会取消绘制。
renderer.domElement.addEventListener('pointerdown', onGeometryDrawPointerDown, true)
renderer.domElement.addEventListener('pointermove', onGeometryDrawPointerMove, true)
renderer.domElement.addEventListener('pointerup', onGeometryDrawPointerUp, true)
renderer.domElement.addEventListener('pointerdown', onObjectEditPointerDown)
renderer.domElement.addEventListener('pointerup', onObjectEditPointerUp)
renderer.domElement.addEventListener('dblclick', onObjectEditDoubleClick)
renderer.domElement.addEventListener('pointercancel', () => { if (geometryDraw.mode) cancelGeometryDraw('绘制已取消') }, true)
renderer.domElement.addEventListener('contextmenu', (event) => {
  if (!geometryDraw.mode) return
  event.preventDefault()
  cancelGeometryDraw('绘制已取消')
}, true)
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && geometryDraw.mode) cancelGeometryDraw('绘制已取消')
})
window.addEventListener('keydown', (event) => {
  if (!objectEdit.enabled) return
  const target = event.target
  if (target instanceof HTMLElement && (target.matches('input, textarea, select') || target.isContentEditable)) return
  const key = event.key.toLowerCase()
  // 对象编辑快捷键：W 移动、E 旋转、R 缩放；无对话框打开时 Esc 先取消选择，再退出编辑模式。
  //   焦点在输入控件时（上面已 return）不响应。
  if (key === 'w') setObjectTransformMode('translate')
  else if (key === 'e') setObjectTransformMode('rotate')
  else if (key === 'r') setObjectTransformMode('scale')
  else if (event.key === 'Escape' && $('#objectAttributeDialog').hidden && $('#objectDialog').hidden) {
    event.preventDefault()
    event.stopImmediatePropagation()
    if (objectEdit.selected) clearObjectSelection()
    else setObjectEditEnabled(false)
  }
})
// 分组 3：顶部工具栏按钮——适应视图、打开/保存/另存为工程、运行、停止、重置，
//   以及“打开示例”“新建工程”“另存为”“参考”等对话框的打开/关闭/提交。
$('#fitBtn').addEventListener('click', fitCamera); $('#openXmlBtn').addEventListener('click', chooseXml); $('#drawerOpenBtn').addEventListener('click', chooseXml); $('#saveXmlBtn').addEventListener('click', saveXml); $('#saveAsBtn').addEventListener('click', showSaveAsDialog); $('#runBtn').addEventListener('click', runSimulation); $('#stopBtn').addEventListener('click', () => { api.stop().catch((error) => { toast('无法停止模拟', error.message, 'error'); addLog(`停止失败：${error.message}`, 'error') }) }); $('#resetBtn').addEventListener('click', resetSimulation)
$('#openExampleBtn').addEventListener('click', showExampleDialog)
$('#exampleForm').addEventListener('submit', openExampleFromDialog)
$('#exampleSelect').addEventListener('change', renderExampleSummary)
$('#cancelExampleBtn').addEventListener('click', hideExampleDialog)
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && !$('#exampleDialog').hidden) hideExampleDialog()
})
$('#newProjectBtn').addEventListener('click', showProjectDialog)
$('#closeProjectBtn').addEventListener('click', hideProjectDialog)
$('#projectDialog').addEventListener('click', (event) => { if (event.target === $('#projectDialog')) hideProjectDialog() })
$('#projectForm').addEventListener('submit', createProject)
$('#closeSaveAsBtn').addEventListener('click', hideSaveAsDialog)
// 保留编辑内容：点击窗外不关闭，只能取消或保存成功后关闭。
$('#saveAsForm').addEventListener('submit', saveProjectAs)
$('#referenceBtn').addEventListener('click', showReferenceDialog)
$('#closeReferenceBtn').addEventListener('click', hideReferenceDialog)
$('#referenceDialog').addEventListener('click', (event) => { if (event.target === $('#referenceDialog')) hideReferenceDialog() })
$('#openHistreamProxyBtn').addEventListener('click', openHistreamProxyDialog)
// 菜单栏：带 data-scene-command 的菜单项在 STREAM 工作区打开时转交给工作区处理
//   （route 返回 true 即已接管）。streamWorkspace 先声明为 null、稍后创建；菜单栏必须先于工作区
//   注册键盘捕获处理器（见 menubar.js 步骤 5，保证 Esc 先关菜单），所以不能调换创建顺序，
//   route 里显式判空，创建完成前的点击视为“未接管”。
let streamWorkspace = null
createMenubar({
  root: $('#menubar'),
  route: (item) => {
    if (!item.dataset.sceneCommand || !streamWorkspace?.isOpen()) return false
    streamWorkspace.command(item.dataset.sceneCommand)
    return true
  }
})
// STREAM（遥感像元尺度）工作区：data-stream-command 的菜单项 'show' 打开工作区，
//   其余命令转发给工作区；data-solution-mode 的菜单项先关闭工作区再切换计算模式。
streamWorkspace = createStreamWorkspace({ toast })
for (const item of document.querySelectorAll('[data-stream-command]')) {
  item.addEventListener('click', () => {
    if (item.dataset.streamCommand === 'show') streamWorkspace.open()
    else streamWorkspace.command(item.dataset.streamCommand)
  })
}
for (const item of document.querySelectorAll('[data-solution-mode]')) {
  item.addEventListener('click', () => {
    streamWorkspace.close()
    selectMode(item.dataset.solutionMode, true)
  })
}

// 快捷键帮助表：[分组名, [[按键, 说明], ...]]；Ctrl+N/O 由 menubar.js 处理，
//   Ctrl+S/F5/F7 由本文件下方的全局 keydown 处理。
const SHORTCUTS = [
  ['项目', [['Ctrl+N', '新建工程'], ['Ctrl+O', '打开工程'], ['Ctrl+S', '保存'], ['Ctrl+Shift+S', '另存为']]],
  ['工作台', [['F7', '打开解析模型（STREAM 像元辐射传输 / 能量平衡）']]],
  ['模拟', [['F5', '运行模拟（三维场景或遥感影像，取决于当前工作台）']]],
  ['遥感影像', [['滚轮', '缩放'], ['拖动', '平移'], ['双击', '适应窗口']]],
  ['菜单与对话框', [['方向键', '在菜单之间和菜单项之间移动'], ['Esc', '关闭菜单或对话框']]]
]

// 作用：帮助菜单。topic='reference' 打开参考图例；'user'/'theory' 通过 api.openHelpDocument
//   打开用户手册/理论手册；'shortcuts' 显示快捷键表；其余显示“关于”（版本与引擎路径
//   来自 api.defaults()，读取失败时显示 —）。
async function showHelp(topic) {
  if (topic === 'reference') { showReferenceDialog(); return }
  if (topic === 'user' || topic === 'theory') {
    try {
      const result = await api.openHelpDocument(topic)
      addLog(`已打开 ${result.path}`)
    } catch (error) { toast('无法打开手册', error.message, 'error') }
    return
  }
  const body = $('#helpBody')
  if (topic === 'shortcuts') {
    $('#helpDialogTitle').textContent = t('快捷键')
    body.innerHTML = SHORTCUTS.map(([group, rows]) => `<section><h3>${escapeHtml(t(group))}</h3><dl>${rows.map(([key, action]) => `<div><dt><kbd>${escapeHtml(key)}</kbd></dt><dd>${escapeHtml(t(action))}</dd></div>`).join('')}</dl></section>`).join('')
  } else {
    let defaults = {}
    try { defaults = await api.defaults() } catch {}
    $('#helpDialogTitle').textContent = t('关于 Stream Studio')
    body.innerHTML = `<div class="about-head"><div class="brandLogo" aria-hidden="true"><span class="logoLab">RTLab</span><span class="logoSlash"></span><span class="logoStream">STREAM</span></div>
      <div><strong>Stream Studio</strong><span>${escapeHtml(t('版本'))} ${escapeHtml(defaults.appVersion || '—')}</span></div></div>
      <p>${escapeHtml(t('三维场景与遥感影像的光学—热红外辐射传输和能量平衡模拟平台。'))}</p>
      <dl class="about-list">
        <div><dt>${escapeHtml(t('三维引擎'))}</dt><dd>HiStream　${escapeHtml(defaults.executable || '—')}</dd></div>
        <div><dt>${escapeHtml(t('遥感影像引擎'))}</dt><dd>STREAM　${escapeHtml(defaults.streamExecutable || '—')}</dd></div>
        <div><dt>${escapeHtml(t('运行环境'))}</dt><dd>Node ${escapeHtml(defaults.versions?.node || '—')}，Three.js ${escapeHtml(defaults.versions?.three || '—')}</dd></div>
        <div><dt>${escapeHtml(t('作者'))}</dt><dd>卞尊健、曹彪、历华、杜永明、肖青、柳钦火</dd></div>
        <div><dt>${escapeHtml(t('许可'))}</dt><dd>PolyForm Noncommercial License 1.0.0${escapeHtml(t('；商业使用须另行取得书面授权'))}</dd></div>
      </dl>`
  }
  $('#helpDialog').hidden = false
  $('#closeHelpBtn').focus()
}
const hideHelp = () => { $('#helpDialog').hidden = true }
for (const item of document.querySelectorAll('[data-help]')) item.addEventListener('click', () => showHelp(item.dataset.help))
$('#closeHelpBtn').addEventListener('click', hideHelp)
$('#helpDialog').addEventListener('click', (event) => { if (event.target === $('#helpDialog')) hideHelp() })
window.addEventListener('keydown', (event) => { if (event.key === 'Escape' && !$('#helpDialog').hidden) hideHelp() })
// 分组 4：各类对话框的关闭（按钮/点击遮罩）与表单提交：histream-proxy、参考图例、
//   几何生成、材质导入、对象属性、场景对象、预设（光谱/热/物理）。
$('#closeHistreamProxyBtn').addEventListener('click', hideHistreamProxyDialog)
$('#histreamProxyDialog').addEventListener('click', (event) => { if (event.target === $('#histreamProxyDialog')) hideHistreamProxyDialog() })
$('#runHistreamProxyBtn').addEventListener('click', runHistreamProxyFrame)
$('#histreamProxyBandSelect').addEventListener('change', showHistreamProxyBand)
$$('.reference-shot').forEach((button) => button.addEventListener('click', () => showReferenceImage(button)))
$('#closeReferenceImageBtn').addEventListener('click', hideReferenceImage)
$('#referenceImageDialog').addEventListener('click', (event) => { if (event.target === $('#referenceImageDialog')) hideReferenceImage() })
$('#closeGeometryBtn').addEventListener('click', hideGeometryDialog)
$('#geometryDialog').addEventListener('click', (event) => { if (event.target === $('#geometryDialog')) hideGeometryDialog() })
$('#geometryForm').addEventListener('input', updateGeometryDialog)
$('#geometryForm').addEventListener('change', (event) => {
  if (event.target.name === 'geometryMode' && (event.target.value === 'fire' || event.target.value === 'fog')) setMediumDefaults(event.target.value)
  if (event.target.name === 'geometryMode' && event.target.value === 'water') setWaterDefaults()
  updateGeometryDialog()
})
$('#geometryForm').addEventListener('submit', generateGeometryObject)
$('#closeMaterialBtn').addEventListener('click', () => hideMaterialDialog(true))
$('#materialDialog').addEventListener('click', (event) => { if (event.target === $('#materialDialog')) hideMaterialDialog(true) })
$('#materialForm').addEventListener('submit', saveImportedObject)
$('#closeObjectAttributeBtn').addEventListener('click', hideObjectAttributeDialog)
$('#objectAttributeDialog').addEventListener('click', (event) => { if (event.target === $('#objectAttributeDialog')) hideObjectAttributeDialog() })
$('#objectAttributeForm').addEventListener('submit', saveObjectAttributes)
$('#objectAttributeType').addEventListener('change', () => renderObjectAttributeOptions(true, true))
// 对象属性：整体材质改变时同步到所有 Mesh 行（各行仍可单独改）；
//   光伏（energyModel='photovoltaic'）材质同时把冠层设为 rigid_body（刚体）。
$('#objectAttributeMaterial').addEventListener('change', () => {
  const materialName = $('#objectAttributeMaterial').value
  // Apply the object-wide choice to all meshes; individual rows remain editable.
  $$('#objectAttributeMeshRows .attribute-physical').forEach((select) => { select.value = materialName })
  if (state.config.materials.find((item) => item.name === materialName)?.energyModel === 'photovoltaic') {
    $('#objectAttributeForm').elements.canopyName.value = 'rigid_body'
    $$('#objectAttributeMeshRows .attribute-canopy').forEach((select) => { select.value = 'rigid_body' })
  }
  renderObjectAttributeSummary()
})
$('#closeObjectBtn').addEventListener('click', hideObjectDialog)
$('#objectDialog').addEventListener('click', (event) => { if (event.target === $('#objectDialog')) hideObjectDialog() })
$('#objectForm').addEventListener('submit', saveSceneObject)
// 场景对象表单：分布数量/密度/范围/基准改变时联动数量与密度（syncDistributionPopulation），
//   并刷新分布与运动表单的可用状态。
$('#objectForm').addEventListener('input', (event) => {
  const form = $('#objectForm')
  if (event.target.name === 'distributionCount') syncDistributionPopulation(form, 'count')
  else if (event.target.name === 'distributionDensity') syncDistributionPopulation(form, 'density')
  else if (['distributionMinX', 'distributionMaxX', 'distributionMinY', 'distributionMaxY'].includes(event.target.name)) syncDistributionPopulation(form, form.elements.distributionBasis.value)
  updateDistributionFormState(form)
  updateMovementFormState(form)
})
$('#objectForm').addEventListener('change', (event) => {
  const form = $('#objectForm')
  if (event.target.name === 'distributionBasis') syncDistributionPopulation(form, event.target.value)
  updateDistributionFormState(form)
  updateMovementFormState(form)
})
$('#distributionTxtFile').addEventListener('change', (event) => importDistributionTxt(event.target.files?.[0]))
// 材质对话框：光谱模型改变时重绘模型参数与反射率/透射率输入（保留已输入的值，缺省 0.20/0.0）。
$('#spectralModel').addEventListener('change', () => {
  const reflectance = materialSpectrumValues('reflectance') || '0.20'
  const transmittance = materialSpectrumValues('transmittance') || '0.0'
  const fileName = $('#materialForm [name="fileName"]')?.value || ''
  renderModelParameters(fileName)
  renderMaterialSpectrumValueFields(reflectance, transmittance)
})
document.querySelector("#spectralPreset").addEventListener("change", () => {
  applySpectrumPreset()
  $$('.mesh-spectrum').forEach((select) => { select.value = $('#spectralPreset').value })
})
document.querySelector("#thermalPreset").addEventListener("change", () => {
  applyThermalPreset()
  $$('.mesh-thermal').forEach((select) => { select.value = $('#thermalPreset').value })
})
// 对象类型改变时设置缺省光谱模型：植被 → Prospect，土壤 → BSM，其他 → custom；
//   太阳能板（SolarPanel）把光谱/热预设置为“新建”，然后重绘各预设选项与 Mesh 行。
$('#materialObjectType').addEventListener('change', () => {
  const type = $('#materialObjectType').value
  if (type === 'SolarPanel') {
    $('#spectralPreset').value = '__new__'
    $('#thermalPreset').value = '__new__'
  }
  $('#spectralModel').value = type === 'Vegetation' ? 'Prospect' : type === 'Soil' ? 'BSM' : 'custom'
  renderModelParameters(); renderSpectrumPresetOptions(); renderThermalPresetOptions(); renderPhysicalPresetOptions(); renderMeshMaterialRows()
})
// 物理材质预设改变：同步到所有 Mesh 行；光伏材质同时把冠层设为 rigid_body。
$('#physicalMaterialPreset').addEventListener('change', () => {
  renderPhysicalParameters()
  const materialName = $('#physicalMaterialPreset').value
  $$('.mesh-physical').forEach((select) => { select.value = materialName })
  if (state.config.materials.find((item) => item.name === materialName)?.energyModel === 'photovoltaic') {
    $$('.mesh-canopy').forEach((select) => { select.value = 'rigid_body' })
  }
})
$('#closePresetBtn').addEventListener('click', hidePresetDialog)
$('#presetDialog').addEventListener('click', (event) => { if (event.target === $('#presetDialog')) hidePresetDialog() })
$('#presetKind').addEventListener('change', () => { state.presetPhysicalType = 'Vegetation'; renderPresetParameterFields() })
$('#presetForm').addEventListener('submit', savePreset)


// 分组 5：结果与分析入口按钮（图像/三维/时间/角度/波段/变化）、结果对话框标签页与操作、
//   日志清空、XML(project.json) 抽屉、全局快捷键、运行计时刷新。
$('#engineBtn').addEventListener('click', chooseEngine)
$('#openOutputBtn').addEventListener('click', () => showResults('image'))
$('#openImageResultsTopBtn').addEventListener('click', () => showResults('image'))
$('#open3dResultsTopBtn').addEventListener('click', () => showResults('3d'))
$('#openTimeAnalysisBtn').addEventListener('click', () => showResults('analysis', 'time'))
$('#openAngleAnalysisBtn').addEventListener('click', () => showResults('analysis', 'angle'))
$('#openBandAnalysisBtn').addEventListener('click', () => showResults('analysis', 'band'))
$('#openChangeAnalysisBtn').addEventListener('click', () => showResults('analysis', 'change'))
$('#resultImageTab').addEventListener('click', () => setResultView('image'))
$('#resultAnalysisTab').addEventListener('click', () => setResultView('analysis'))
$('#angleAnalysisTab').addEventListener('click', () => setAnalysisType('angle'))
$('#bandAnalysisTab').addEventListener('click', () => setAnalysisType('band'))
$('#timeAnalysisTab').addEventListener('click', () => setAnalysisType('time'))
$('#changeAnalysisTab').addEventListener('click', () => setAnalysisType('change'))
$('#threeAnalysisTab').addEventListener('click', () => setAnalysisType('3d'))
$('#closeResultBtn').addEventListener('click', hideResults)
$('#resultDialog').addEventListener('click', (event) => { if (event.target === $('#resultDialog')) hideResults() })
$('#resultDeleteAllBtn').addEventListener('click', deleteAllResults)
$('#resultRefreshBtn').addEventListener('click', async () => { try { await refreshResults(true) } catch (error) { toast('刷新结果失败', error.message, 'error') } })
$('#resultFolderBtn').addEventListener('click', async () => { try { await api.openPath(state.outputDir) } catch (error) { toast('无法打开输出目录', error.message, 'error') } })
$('#clearLogBtn').addEventListener('click', () => { $('#consoleOutput').innerHTML = ''; state.logCount = 0; $('#logCount').textContent = '0' })
// project.json 编辑抽屉：手动编辑后标记未保存并置 xmlManualEdit 标志；点击遮罩关闭。
$('#xmlBtn').addEventListener('click', () => { $('#xmlDrawer').hidden = false }); $('#closeXmlBtn').addEventListener('click', () => { $('#xmlDrawer').hidden = true })
$('#xmlDrawer').addEventListener('click', (event) => { if (event.target === $('#xmlDrawer')) $('#xmlDrawer').hidden = true })
$('#xmlEditor').addEventListener('input', () => { state.xmlText = $('#xmlEditor').value; markXmlDirty(); state.xmlManualEdit = true; $('#unsavedMark').hidden = false; $('#xmlStatus').textContent = '有未保存的修改' })
// 应用手动编辑：解析 project.json → 替换 state.project/config → 保存成功后刷新场景与检查器；
//   解析失败提示“JSON 格式错误”。
$('#applyXmlBtn').addEventListener('click', async () => { try { const project = parseProjectJson(state.xmlText); const mode = project.mode; state.project = project; state.config = ensureMaterialPresets(project.configuration); state.xmlManualEdit = false; selectMode(mode); if (await saveXml()) { refreshFromState(); restoreActualScene(); renderInspector(); $('#xmlDrawer').hidden = true } } catch (error) { toast('JSON 格式错误', error.message, 'error') } })
// 全局快捷键：Ctrl/Cmd+S 保存，Ctrl/Cmd+Shift+S 另存为，F5 运行，F7 打开 STREAM 工作区；
//   Esc 按固定优先级只关闭最上层的一个对话框/抽屉
//   （histream-proxy → 参考图 → 参考 → 结果 → 对象属性 → 场景对象 → 材质 → 几何 → 预设 → 新建工程 → XML 抽屉）。
window.addEventListener('keydown', (event) => { if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') { event.preventDefault(); if (event.shiftKey) showSaveAsDialog(); else saveXml() } if (event.key === 'F5') { event.preventDefault(); runSimulation() } if (event.key === 'F7') { event.preventDefault(); streamWorkspace.open() } if (event.key === 'Escape') { if (!$('#histreamProxyDialog').hidden) hideHistreamProxyDialog(); else if (!$('#referenceImageDialog').hidden) hideReferenceImage(); else if (!$('#referenceDialog').hidden) hideReferenceDialog(); else if (!$('#resultDialog').hidden) hideResults(); else if (!$('#objectAttributeDialog').hidden) hideObjectAttributeDialog(); else if (!$('#objectDialog').hidden) hideObjectDialog(); else if (!$('#materialDialog').hidden) hideMaterialDialog(true); else if (!$('#geometryDialog').hidden) hideGeometryDialog(); else if (!$('#presetDialog').hidden) hidePresetDialog(); else if (!$('#projectDialog').hidden) hideProjectDialog(); else if (!$('#xmlDrawer').hidden) $('#xmlDrawer').hidden = true } })
// 运行中每 250 ms 刷新一次进度/耗时显示。
setInterval(() => { if (state.running) renderRunProgress() }, 250)

// 引擎路径设置的提示与被拒绝时的提示文案：自定义引擎必须先加入服务端环境变量
//   STREAM_STUDIO_ENGINE_ALLOWLIST 并重启，否则运行被拒绝。
const ENGINE_PROMPT = 'HiStream 可执行文件路径（留空恢复内置引擎）。自定义路径须先由管理员加入服务端环境变量 STREAM_STUDIO_ENGINE_ALLOWLIST（多个路径用系统路径分隔符分隔，Windows 为分号 ;），并重启 Stream Studio，否则运行会被拒绝。'
const ENGINE_REJECTED_HINT = '自定义引擎不在允许列表中：请将该路径加入环境变量 STREAM_STUDIO_ENGINE_ALLOWLIST后重启 Stream Studio，或在“引擎”中留空以恢复内置引擎。'

// 作用：选择自定义 HiStream 可执行文件。Facet 模式只显示当前引擎；选择对话框取消（null）则不变；
//   空路径或等于默认路径表示恢复内置引擎。设置后重新加载计算设备列表。
async function chooseEngine() {
  if (state.mode === 'eFacetRT' || state.mode === 'eFacetEB') { toast('HiStream Facet 引擎', state.executable); return }
  let path
  try { path = await api.chooseExecutable(t(ENGINE_PROMPT)) }
  catch (error) { toast('无法设置引擎', error.message, 'error'); return }
  if (path === null) return
  if (!path || path === state.defaultExecutable) {
    state.executable = state.defaultExecutable; state.executableExists = state.defaultExecutableExists
    setRunning(false); addLog(`已恢复内置 HiStream 引擎：${state.executable}`, 'success')
    loadComputeDevices()
    return
  }
  state.executable = path; state.executableExists = true; setRunning(false)
  loadComputeDevices()
  addLog(`HiStream 引擎：${path}`, 'success')
  addLog('自定义引擎须已加入服务端 STREAM_STUDIO_ENGINE_ALLOWLIST，否则运行时会被拒绝', 'warning')
}

// 作用：判断错误是否来自引擎白名单（ENGINE_ALLOWLIST）拒绝，用于给出针对性提示。
function isEngineAllowlistError(error) {
  return String(error?.message || '').includes('ENGINE_ALLOWLIST')
}

// 作用：应用启动初始化（模块末尾调用一次）。
// 步骤：1 用默认配置生成预览场景；2 订阅模拟事件；3 读取服务端默认值（引擎路径、
//   运行超时秒数、histream-proxy 是否配置）并写日志，桥接不可用时报错返回；
//   4 若有上次工程（defaults.projectFile）则自动重新打开，失败只写日志。
async function init() {
  state.config = defaultConfig(); generateWorld(state.config); refreshFromState(false); fitCamera(); renderInspector(); resize()
  api.onSimulationEvent(handleSimulationEvent)
  let defaults
  try {
    defaults = await api.defaults()
    state.executable = defaults.executable; state.executableExists = defaults.executableExists
    state.defaultExecutable = defaults.executable; state.defaultExecutableExists = defaults.executableExists
    state.serverRunTimeoutSeconds = Number(defaults.runTimeoutSeconds) || 0
    $('#openHistreamProxyBtn').hidden = !defaults.histreamProxyConfigured
    setRunning(false)
    addLog(`GPU Radiosity：${defaults.radiosityExecutable}`, defaults.radiosityExecutableExists ? 'success' : 'error')
    addLog(`默认 HiStream：${defaults.executable}`, 'success'); addLog(`Three.js ${defaults.versions.three} · Node ${defaults.versions.node}`)
    loadComputeDevices()
  } catch (error) { state.executableExists = false; setEngineState('桌面桥接不可用', error.message, ''); addLog(error.message, 'error'); return }
  if (!defaults.projectFile) return
  try { loadXml(await api.openProject(defaults.projectFile)) }
  catch (error) { addLog(`无法重新打开上次的工程：${error.message}`, 'error') }
}

// 启动界面本地化；语言切换后回调重绘依赖 t() 的动态内容（检查器、导航徽标、场景范围/航线/太阳提示）。
startLocalization({ onChange: () => {
  renderInspector()
  if (!state.config) return
  renderNavBadges(); updateSceneBoundsGuide(state.config); updateCruiseVisualization(state.config); updateSun(state.config)
} })
init()
