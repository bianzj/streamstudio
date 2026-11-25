// ==========================================================================
// 文件：project-schema.js
// 功能：HiStream 工程（project.json）的数据模型：常量、默认值、规范化/迁移、校验，
//   以及转换为引擎读取的场景 XML。前端界面、server.mjs 和 tools 脚本共用本文件。
// 主要模块：
//   1. 工程格式常量、模式列表与基础数值工具（number 把 null/空串视为缺失值）
//   2. 地形环境缺省值、CPU 线程数、运行质量档位预设（qualityPreset*）
//   3. 传感器：波段（sensorBandValues）、观测角（sensorViewAngles）、巡航航点
//   4. 光谱与表面：旧光伏光谱清理、纹理规范化、开孔率（穿孔板）光学
//   5. 车辆部件：光谱、座舱/发动机缺省、OBJ 组名自动绑定与前挡/侧窗/后窗/引擎盖拆分
//   6. 内置预设库（createMaterialPresets）与新工程缺省（createDefaultProject）
//   7. normalizeProject / parseProjectJson / stringifyProject：规范化、迁移旧工程
//   8. validateProject：工程合法性校验，返回错误文本列表
//   9. projectToXml 及 objectXml/spectrumXml/materialXml 等：导出引擎使用的 XML
// 数据流：project.json → parseProjectJson/normalizeProject → validateProject → projectToXml → 引擎。
// 与其他文件：依赖 material-specular.js（镜面项）；被 main.js、project-unified.js、server.mjs、
//   tools/ 下的脚本引用；车辆与地形环境的说明见 docs/vehicle-thermal.md、
//   docs/histream-terrain-environment.md；引擎侧解析在 models/histream/src/base。
// 重要约定：旧工程缺字段时尽量保持旧行为，新工程使用新缺省（见各处“旧工程”说明）；
//   长度 m；气象 Tsold 为 K，材质 Tsoil、室内温度为 ℃；角度为度（见各字段说明）。
// ==========================================================================

import { normalizeSpecular, meshSpecular } from './material-specular.js'

// 模块：工程格式常量与模式列表。
// PROJECT_SCHEMA_VERSION 为当前工程 schema 版本；PROJECT_MODES 为工程支持的计算模式（带 e 前缀的内部名）。
export const PROJECT_SCHEMA_VERSION = 6
export const PROJECT_FORMAT = 'histream-project'
export const isProjectFormat = (format) => format === PROJECT_FORMAT

export const PROJECT_MODES = ['eVoxelEB', 'eFacetEB', 'eFacetRT', 'eVoxelRT', 'eRaytracing']
export const DEFAULT_SCENE_MODEL = 'models/default_building.obj'
export const DEFAULT_SENSOR_BANDS = [475, 560, 668, 717, 842, 10500]

// null and blank strings are missing values, not 0 (Number(null) === Number('') === 0).
const number = (value, fallback) => {
  if (value === null || value === undefined || (typeof value === 'string' && value.trim() === '')) return fallback
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : fallback
}

// 模块：地形环境（空气/土壤沿高度的外推）参数缺省值。
// referenceHeight：温湿度参考高度（m，相对场景零面）；temperatureLapseRate：K m-1；
// vaporScaleHeight：水汽压指数尺度高度（m）；drainageHeight：沟谷基准面高度（m）；
// soilDry/soilValley：干燥/沟谷处土壤含水量（m3 m-3）；soilScaleHeight：土壤水衰减尺度（m）。
// 公式与引擎侧实现见 docs/histream-terrain-environment.md、terrain_environment.h。
export const TERRAIN_ENVIRONMENT_DEFAULTS = Object.freeze({
  airEnabled: false, soilEnabled: false,
  referenceHeight: 2, temperatureLapseRate: 0.0065, vaporScaleHeight: 2000,
  drainageHeight: 0, soilDry: 0.08, soilValley: 0.35, soilScaleHeight: 30
})

// 作用：把用户/旧工程的地形环境配置补全为完整对象（缺省取 TERRAIN_ENVIRONMENT_DEFAULTS）。
// 说明：布尔项（airEnabled/soilEnabled）借 normalizeSpecular 的 enabled 解析；其余项转为有限数，否则取缺省。
export function normalizeTerrainEnvironment(source = {}) {
  const result = { ...TERRAIN_ENVIRONMENT_DEFAULTS, ...source }
  for (const [key, fallback] of Object.entries(TERRAIN_ENVIRONMENT_DEFAULTS)) {
    result[key] = typeof fallback === 'boolean'
      ? normalizeSpecular({ enabled: source?.[key] ?? false }).enabled
      : number(source?.[key], fallback)
  }
  return result
}
export const MAX_SENSOR_BANDS = 800
// control.cpuThreads: default 32; engines clamp it to the machine's logical processors.
export const DEFAULT_CPU_THREADS = 32
export const normalizeCpuThreads = (value) => {
  const threads = Math.round(Number(value))
  return Number.isFinite(threads) && threads >= 1 ? Math.min(1024, threads) : DEFAULT_CPU_THREADS
}

// 运行质量三档预设（control.qualityPreset）。每档只管理所选模式实际读取的参数：
//   RayTracing: samples + raytracingDepth；VoxelRT: samples + voxelCollisionOrders；
//   VoxelEB: samples + shortwaveScatteringOrders + radiationMaxSteps；FacetEB: couplingIterations；
//   FacetRT 没有受档位管理的参数。'custom' 表示用户手动改过受管参数（旧工程缺省也是 custom）。
// 数值由 tools/regression 冻结算例标定（以“高精度”为参考，标定表见用户手册 13.2）：
//   - VoxelEB 的像元采样数几乎不增加耗时（8→128 约 +0.3 s），而 16 次采样时影像边角像元可能全部落在
//     计算域外而输出 0，因此快速档 VoxelEB 也用 32；耗时主要随短波散射阶数增加（每阶约 +50%）。
//   - FacetEB 的耦合迭代是上限，收敛即停止：标定算例 16–19 次（松弛 0.5）或约 11 次（松弛 0.8）收敛。
export const QUALITY_PRESETS = ['fast', 'standard', 'high', 'custom']
export const QUALITY_PRESET_LABELS = { fast: '快速预览', standard: '标准', high: '高精度', custom: '自定义' }
// 各档位的通用参数值；radiationMaxSteps 为随场景尺寸计算的函数（见 autoRadiationMaxSteps）。
const QUALITY_PRESET_VALUES = {
  fast: { samples: 16, raytracingDepth: 8, voxelCollisionOrders: 2, shortwaveScatteringOrders: 1, radiationMaxSteps: () => 64, couplingIterations: 20 },
  standard: { samples: 32, raytracingDepth: 16, voxelCollisionOrders: 3, shortwaveScatteringOrders: 2, radiationMaxSteps: (scene) => autoRadiationMaxSteps(scene), couplingIterations: 60 },
  high: { samples: 128, raytracingDepth: 32, voxelCollisionOrders: 5, shortwaveScatteringOrders: 3, radiationMaxSteps: (scene) => Math.max(512, autoRadiationMaxSteps(scene, 3)), couplingIterations: 100 }
}
// 个别模式与通用档位值不同的参数。
const QUALITY_PRESET_MODE_VALUES = {
  fast: { eVoxelEB: { samples: 32 } }
}
// 各模式受档位管理的 control 字段（与文件上方档位说明一致）。
const QUALITY_PRESET_FIELDS = {
  eRaytracing: ['samples', 'raytracingDepth'],
  eVoxelRT: ['samples', 'voxelCollisionOrders'],
  eVoxelEB: ['samples', 'shortwaveScatteringOrders', 'radiationMaxSteps'],
  eFacetEB: ['couplingIterations'],
  eFacetRT: []
}
export const qualityPresetFields = (mode) => QUALITY_PRESET_FIELDS[mode] || []

// VoxelEB 体元射线步数：场景对角线穿过的体元数 × factor，取 16 的倍数，限制在 64–4096。
export function autoRadiationMaxSteps(scene = {}, factor = 1.5) {
  const voxel = Math.max(0.01, number(scene.voxel, 1))
  const diagonal = Math.hypot(number(scene.x, 60), number(scene.y, 60), number(scene.height, 15)) / voxel
  return Math.max(64, Math.min(4096, Math.ceil(diagonal * factor / 16) * 16))
}

// 某档在指定模式下应写入的参数（只含该模式读取的字段）。
export function qualityPresetValues(preset, mode, scene = {}) {
  const values = QUALITY_PRESET_VALUES[preset]
  if (!values) return {}
  const merged = { ...values, ...(QUALITY_PRESET_MODE_VALUES[preset]?.[mode] || {}) }
  return Object.fromEntries(qualityPresetFields(mode).map((key) => [key, typeof merged[key] === 'function' ? merged[key](scene) : merged[key]]))
}

// 作用：判断 control 中受管字段是否与某档位在该模式下的取值完全一致（用于推断当前档位）。
export function qualityPresetMatches(preset, mode, control = {}, scene = {}) {
  if (!QUALITY_PRESET_VALUES[preset]) return false
  return Object.entries(qualityPresetValues(preset, mode, scene)).every(([key, value]) => Number(control[key]) === value)
}

// 工程级单次运行上限（分钟）：0 / 空 = 使用服务端默认；否则 1–1440。
export function normalizeRunTimeoutMinutes(value) {
  const minutes = Math.round(number(value, 0))
  return minutes >= 1 ? Math.min(1440, minutes) : 0
}

// 数值工具：cleanNumber 保留 6 位小数以去除浮点噪声；normalizedAzimuth 把方位角归一到 [0, 360)。
const cleanNumber = (value) => Number(Number(value).toFixed(6))
const normalizedAzimuth = (value) => ((Number(value) % 360) + 360) % 360

// 作用：生成传感器巡航（多位置观测）的位置序列，路线为 line（往返直线）/rectangle（矩形环绕）/random（随机游走）。
// 输入：sensor 巡航参数（positionX/Y 起点、height、cruiseHeading 航向角 °、cruiseStep 步长 m、cruiseCount 点数）、scene 场景尺寸（x/y，m）。
// 输出：[{x, y, z, heading}]，x/y 对场景范围取模（周期边界），z ≥ 0.01 m，heading 为 [0, 360) 度。
// 说明：随机路线用固定种子的线性同余发生器，同一工程每次生成的轨迹可复现。
export function sensorCruisePositions(sensor = {}, scene = {}) {
  const width = Math.max(0.000001, number(scene.x, 60))
  const depth = Math.max(0.000001, number(scene.y, 60))
  const startX = ((number(sensor.positionX, width / 2) % width) + width) % width
  const startY = ((number(sensor.positionY, depth / 2) % depth) + depth) % depth
  const height = Math.max(0.01, number(sensor.height, 3000))
  const headingDegrees = normalizedAzimuth(number(sensor.cruiseHeading, 90))
  const step = Math.max(0.01, number(sensor.cruiseStep, 25))
  const count = Math.max(1, Math.min(10000, Math.round(number(sensor.cruiseCount, 20))))
  const route = ['line', 'rectangle', 'random'].includes(sensor.cruiseRoute) ? sensor.cruiseRoute : 'line'
  const wrap = (value, extent) => ((value % extent) + extent) % extent
  // 生成一个航点：x/y 周期回绕到场景范围内，z 下限 0.01 m，heading 归一到 [0, 360)。
  const point = (x, y, heading, z = height) => ({ x: cleanNumber(wrap(x, width)), y: cleanNumber(wrap(y, depth)), z: cleanNumber(Math.max(0.01, z)), heading: cleanNumber(normalizedAzimuth(heading)) })

  // 矩形路线：以起点为中心、沿周长按 cruiseStep 等弧长取点，依次走上边、右边、下边、左边（航向 0/90/180/270）。
  if (route === 'rectangle') {
    const rectangleWidth = Math.max(step, Math.min(width, number(sensor.cruiseRectangleWidth, Math.min(width * 0.25, 500))))
    const rectangleHeight = Math.max(step, Math.min(depth, number(sensor.cruiseRectangleHeight, Math.min(depth * 0.25, 500))))
    const perimeter = 2 * (rectangleWidth + rectangleHeight)
    return Array.from({ length: count }, (_, index) => {
      const distance = index * step % perimeter
      if (distance < rectangleWidth) return point(startX - rectangleWidth / 2 + distance, startY - rectangleHeight / 2, 0)
      if (distance < rectangleWidth + rectangleHeight) return point(startX + rectangleWidth / 2, startY - rectangleHeight / 2 + distance - rectangleWidth, 90)
      if (distance < rectangleWidth * 2 + rectangleHeight) return point(startX + rectangleWidth / 2 - (distance - rectangleWidth - rectangleHeight), startY + rectangleHeight / 2, 180)
      return point(startX - rectangleWidth / 2, startY + rectangleHeight / 2 - (distance - rectangleWidth * 2 - rectangleHeight), 270)
    })
  }

  // 随机路线：每步随机取航向（最后一点保持上一航向），按 cruiseStep 前进，固定种子保证可复现（LCG 常数 1664525/1013904223）。
  if (route === 'random') {
    let seed = Math.max(1, Math.round(number(sensor.cruiseRandomSeed, 1))) >>> 0
    const random = () => {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0
      return seed / 4294967296
    }
    const result = []
    let x = startX, y = startY, currentHeading = headingDegrees
    for (let index = 0; index < count; index += 1) {
      if (index < count - 1) currentHeading = random() * 360
      result.push(point(x, y, currentHeading))
      const heading = currentHeading * Math.PI / 180
      x = wrap(x + step * Math.cos(heading), width)
      y = wrap(y + step * Math.sin(heading), depth)
    }
    return result
  }

  // 直线路线（缺省）：起点到目标点（缺省为沿航向走 count-1 步）之间往返，按 cruiseStep 等距取点，
  // 到端点后折返（周期 2·length），折返段航向反向 180°。
  const targetX = number(sensor.cruiseTargetX, startX + Math.cos(headingDegrees * Math.PI / 180) * step * Math.max(1, count - 1))
  const targetY = number(sensor.cruiseTargetY, startY + Math.sin(headingDegrees * Math.PI / 180) * step * Math.max(1, count - 1))
  const targetZ = Math.max(0.01, number(sensor.cruiseTargetZ, height))
  let deltaX = targetX - startX, deltaY = targetY - startY, deltaZ = targetZ - height
  let length = Math.hypot(deltaX, deltaY, deltaZ)
  // 起点与目标重合时退化为沿 cruiseHeading 方向的水平直线，避免除以 0。
  if (length < 0.000001) {
    const heading = headingDegrees * Math.PI / 180
    deltaX = Math.cos(heading) * step
    deltaY = Math.sin(heading) * step
    deltaZ = 0
    length = step
  }
  const horizontalLength = Math.hypot(deltaX, deltaY)
  const forwardHeading = horizontalLength < 0.000001 ? headingDegrees : normalizedAzimuth(Math.atan2(deltaY, deltaX) * 180 / Math.PI)
  const period = 2 * length
  return Array.from({ length: count }, (_, index) => {
    const phase = index * step % period
    const forward = phase < length
    const distance = forward ? phase : period - phase
    return point(startX + deltaX / length * distance, startY + deltaY / length * distance, forward ? forwardHeading : forwardHeading + 180, height + deltaZ / length * distance)
  })
}

// 可移动对象类型（人、车辆、船）。
const isMobileAgentType = (type) => type === 'Human' || type === 'Vehicle' || type === 'Ship'

// 贴地对象：目前只有车辆，其放置高度由地形接触点决定（见 groundedPlacementHeight）。
export const isGroundedObject = (item = {}) => item.type === 'Vehicle'

// Height above the terrain at the placement centre at which an untilted rigid
// footprint dropped from above first touches the terrain. `footprint` is the
// unscaled OBJ extent in X/Z; heightAt(north, east) is the terrain elevation.
export function groundedPlacementHeight(footprint, placement, heightAt) {
  const scale = Number(placement.scale) > 0 ? Number(placement.scale) : 1
  const radians = -(Number(placement.rotation) || 0) * Math.PI / 180
  const cosine = Math.cos(radians), sine = Math.sin(radians)
  const centre = heightAt(placement.north, placement.east)
  const steps = (min, max) => Math.min(32, Math.max(2, Math.ceil((max - min) * scale / 0.25)))
  const nx = steps(footprint.minX, footprint.maxX), nz = steps(footprint.minZ, footprint.maxZ)
  let top = centre
  for (let i = 0; i <= nx; i += 1) for (let k = 0; k <= nz; k += 1) {
    const x = (footprint.minX + (footprint.maxX - footprint.minX) * i / nx) * scale
    const z = (footprint.minZ + (footprint.maxZ - footprint.minZ) * k / nz) * scale
    top = Math.max(top, heightAt(placement.north + cosine * x + sine * z, placement.east - sine * x + cosine * z))
  }
  return top - centre
}
// 兼容旧工程：把标签里的“火焰/烟羽”改为“火焰”。
const withoutSmokeLabel = (item = {}) => ({ ...item, label: String(item.label || '').replaceAll('火焰/烟羽', '火焰') })
// 水体预设的旧名称 → 规范名称映射，用于读取旧工程时统一名称。
const WATER_PRESET_ALIASES = {
  water_radiation: 'water_surface',
  water_temperature_model: 'water_temperature',
  water_model: 'water_set'
}

const canonicalPresetName = (name) => WATER_PRESET_ALIASES[name] || name

// 作用：按规范名合并预设条目；同名冲突时规范名条目优先，同优先级后者覆盖前者。
function canonicalPresetItems(items = []) {
  const merged = new Map()
  for (const item of items) {
    const name = canonicalPresetName(item?.name)
    if (!name) continue
    const priority = item.name === name ? 1 : 0
    const previous = merged.get(name)
    // DEFERRED[低] 数据一致性: 旧别名条目（如 water_radiation）与规范名条目同时存在时，别名条目被无提示丢弃，用户只改过别名条目时这些修改会丢失；未修复：保留为重命名副本（如 name_legacy）会引入新的命名/可见性语义，需产品确认冲突合并策略
    if (!previous || priority >= previous.priority) merged.set(name, { priority, item: { ...item, name } })
  }
  return [...merged.values()].map(({ item }) => item)
}

// 作用：规范化单个材质条目（统一别名）；木质（wood）材质补全 Vegetation 类型和默认参数，
// 若缺少固体热容 cs/specificHeat 则取内置预设的 cs。
function normalizeMaterialItem(item = {}) {
  const name = canonicalPresetName(item.name)
  const wood = item.energyModel === 'wood' || name === 'tree_branch_wood' || name === 'tree_trunk_wood'
  if (!wood) return { ...item, name }
  const fallback = createMaterialPresets().materials.find((entry) => entry.name === name)
  const source = item.params && typeof item.params === 'object' ? item.params : {}
  const solidHeatCapacity = number(source.cs ?? source.specificHeat, undefined)
  const params = { ...(fallback?.params || {}), ...source }
  if (solidHeatCapacity === undefined && fallback?.params?.cs !== undefined) params.cs = fallback.params.cs
  return {
    ...item,
    name,
    type: 'Vegetation',
    energyModel: 'wood',
    params
  }
}

// 作用：补全对象的随机运动参数（缺省按车辆/船与行人区分；speed m s-1，range 活动范围 m（待核对），turnInterval s）。
function normalizeMovement(item = {}) {
  const vehicle = item.type === 'Vehicle' || item.type === 'Ship'
  const defaults = {
    speed: vehicle ? 8.3 : 1.3,
    speedVariation: vehicle ? 1 : 0.2,
    range: vehicle ? 50 : 20,
    turnInterval: vehicle ? 8 : 5
  }
  const movement = item.movement || {}
  return {
    enabled: Boolean(movement.enabled),
    mode: 'random',
    speed: Math.max(0, number(movement.speed, defaults.speed)),
    speedVariation: Math.max(0, number(movement.speedVariation, defaults.speedVariation)),
    range: Math.max(0.1, number(movement.range, defaults.range)),
    turnInterval: Math.max(0.1, number(movement.turnInterval, defaults.turnInterval)),
    seed: Math.round(number(movement.seed, 1))
  }
}

// Sensor band list.  The HiStream engine reproduces these exact rules in
// ProjectJson::sensorBands() (models/histream/src/base/projectjson.cpp); the
// server rasterizes Facet per-band records with this list, so change both
// sides together (rounding, end-append, custom-band capacity, sort, 800 cap).
export function sensorBandValues(sensor = {}) {
  const customValues = String(sensor.bands || '').split(/[\s,;]+/).map(Number)
    .filter((value) => Number.isFinite(value) && value > 0)
  const customCount = new Set(customValues.map((value) => cleanNumber(value).toFixed(6))).size
  const values = []
  if (sensor.continuousBands) {
    const start = number(sensor.bandStart, 400)
    const end = number(sensor.bandEnd, 2500)
    const step = number(sensor.bandStep, 10)
    if (start > 0 && end >= start && step > 0) {
      const count = Math.floor((end - start) / step + 1e-9) + 1
      const generatedCapacity = Math.max(0, MAX_SENSOR_BANDS - customCount)
      const generatedCount = Math.min(count, generatedCapacity)
      for (let index = 0; index < generatedCount; index += 1) values.push(cleanNumber(start + index * step))
      if (count <= generatedCapacity && values.length < generatedCapacity && Math.abs(start + (count - 1) * step - end) > 1e-6) values.push(cleanNumber(end))
    }
  }
  values.push(...customValues)
  const unique = []
  const keys = new Set()
  for (const value of values) {
    const cleaned = cleanNumber(value)
    const key = cleaned.toFixed(6)
    if (!keys.has(key)) { keys.add(key); unique.push(cleaned) }
  }
  // HiStream uses band 0 as the representative 3D/process result.  Keep the
  // merged list in physical wavelength order so an appended TIR band cannot
  // unexpectedly become the first band of a continuous VNIR/SWIR run.
  return unique.sort((a, b) => a - b).slice(0, MAX_SENSOR_BANDS)
}

// 作用：生成传感器的观测方向列表 [[VZA, VAA], ...]（度），并去重。
// 输入：sensor（vza/vaa 单方向，principalPlane 主平面/垂直主平面扫描，hemisphere 半球采样）、
//   sunAzimuth 太阳方位角（度，主平面方位以它为基准）。
// 说明：三种方式可叠加；天顶角限制在 [0, 89.999]；所有 VZA=0 的方向只保留第一个。
export function sensorViewAngles(sensor = {}, sunAzimuth = 0) {
  const angles = []
  const keys = new Set()
  const add = (vza, vaa) => {
    const zenith = Math.max(0, Math.min(89.999, cleanNumber(vza)))
    // Nadir has no directional azimuth, but retaining the requested plane
    // azimuth gives the camera a continuous roll at VZA=0 instead of rotating
    // the sampling grid abruptly relative to the neighbouring ±5° views.
    const azimuth = cleanNumber(normalizedAzimuth(vaa))
    // All nadir azimuths describe the same observation; keep the first
    // (principal-plane) one so the angle chart has exactly one VZA=0 point.
    const key = zenith < 1e-6 ? 'nadir' : `${zenith.toFixed(4)},${azimuth.toFixed(4)}`
    if (!keys.has(key)) { keys.add(key); angles.push([zenith, azimuth]) }
  }
  if (!sensor.principalPlane && !sensor.hemisphere) {
    add(number(sensor.vza, 0), number(sensor.vaa, 0))
  }
  // 主平面扫描：沿太阳方位平面（offset 0）和垂直平面（offset 90），VZA 从 -75° 到 75° 每 5° 一个；
  // 负的带符号天顶角表示对侧，方位角加 180°。
  if (sensor.principalPlane) {
    for (const offset of [0, 90]) {
      for (let signedZenith = -75; signedZenith <= 75; signedZenith += 5) {
        add(Math.abs(signedZenith), number(sunAzimuth, 0) + offset + (signedZenith < 0 ? 180 : 0))
      }
    }
  }
  // 半球采样：128 个等面积方向，用黄金角螺旋分布方位角，cosZenith 在 [cos75°, 1] 上均匀。
  if (sensor.hemisphere) {
    const goldenAngle = Math.PI * (3 - Math.sqrt(5))
    const minimumCosZenith = Math.cos(75 * Math.PI / 180)
    for (let index = 0; index < 128; index += 1) {
      // TiRT-EB-style view cap: equal-area directions span VZA 0..75 degrees,
      // rather than extending the observation set to the 90-degree horizon.
      const cosZenith = 1 - (1 - minimumCosZenith) * index / 127
      add(Math.acos(cosZenith) * 180 / Math.PI, index * goldenAngle * 180 / Math.PI)
    }
  }
  return angles
}

// 作用：从光谱库中剔除旧版内置、且未被引用也未被改动的光伏光谱（单晶/多晶/双面），
//   并把 pv_opaque 的旧标签统一改为“太阳能板光谱”；被工程引用或被用户改过的条目一律保留。
// 输入：spectra 光谱条目数组，configuration 整个工程配置（用于收集被引用的 spectralName）。
// Retain bound or edited legacy spectra so existing simulations do not change.
export function simplifySolarSpectra(spectra, configuration = {}) {
  const references = new Set([configuration.scene?.background?.spectralName])
  for (const item of configuration.objects?.items || []) {
    references.add(item.spectralName)
    for (const mesh of item.meshes || []) references.add(mesh.spectralName)
  }
  const retired = {
    pv_monocrystalline: ['单晶硅太阳能板光谱', 'pv_monocrystalline_silicon.txt', 0.06, 0.10],
    pv_polycrystalline: ['多晶硅太阳能板光谱', 'pv_polycrystalline_silicon.txt', 0.09, 0.11],
    pv_bifacial: ['双面光伏组件光谱', 'pv_bifacial_glass.txt', 0.07, 0.09]
  }
  return spectra.filter((item) => {
    const old = retired[item.name]
    if (!old || references.has(item.name)) return true
    // 仅当条目的标签、文件、反/透射率、TIR 参数、纹理、镜面项都与旧版内置值完全一致时才视为“未改动”。
    const unchanged = item.label === old[0] && item.model === 'file'
      && item.fileName === `assets/spectral-library/${old[1]}`
      && Number(item.reflectance) === old[2] && Number(item.transmittance) === 0
      && Number(item.refTir) === old[3] && Number(item.tauTir) === 0
      && !Object.keys(item.params || {}).length
      && !item.physicalTexture?.enabled && !item.physicalTexture?.fileName
      && !normalizeSpecular(item.specular).enabled
      && item.previewTexture?.enabled === true && item.previewTexture?.preset === 'solar-panel'
      && Number(item.previewTexture?.repeatSize) === 0.18
    return !unchanged
  }).map((item) => item.name === 'pv_opaque' && ['光伏板不透明光谱', '光伏板通用不透明光谱'].includes(item.label)
    ? { ...item, label: '太阳能板光谱' } : item)
}

// 穿孔板开孔率（孔洞面积占比）的上限；surfaceOpenness 与纹理开孔率均以此限幅。
export const MAX_SURFACE_OPENNESS = 0.95

// 作用：规范化面元表面纹理配置 classTexture（分类纹理）与 opennessTexture（开孔率纹理）。
// 输出：只包含源对象中存在的纹理项；分类颜色限 #rrggbb 且最多 16 类，开孔率分级 2–32 级，
//   minOpenness ≤ maxOpenness ≤ MAX_SURFACE_OPENNESS；repeatSize 为纹理重复尺寸（单位待核对）。
// Class texture: PNG whose colours select a spectrum per face. Openness
// texture: grey PNG whose brightness selects an openness level per face.
// Both are applied when a run is staged (tools/surface-textures.mjs).
function normalizeSurfaceTextures(item = {}) {
  const result = {}
  const classSource = item.classTexture
  if (classSource && typeof classSource === 'object') {
    result.classTexture = {
      enabled: Boolean(classSource.enabled),
      fileName: String(classSource.fileName || ''),
      repeatSize: Math.max(0.01, number(classSource.repeatSize, 1.5)),
      classes: (Array.isArray(classSource.classes) ? classSource.classes : [])
        .filter((entry) => /^#?[0-9a-f]{6}$/i.test(String(entry?.color || '').trim()))
        .slice(0, 16)
        .map((entry) => ({ color: `#${String(entry.color).trim().replace(/^#/, '').toLowerCase()}`, spectralName: String(entry.spectralName || '') }))
    }
  }
  const opennessSource = item.opennessTexture
  if (opennessSource && typeof opennessSource === 'object') {
    const minimum = Math.max(0, Math.min(MAX_SURFACE_OPENNESS, number(opennessSource.minOpenness, 0)))
    result.opennessTexture = {
      enabled: Boolean(opennessSource.enabled),
      fileName: String(opennessSource.fileName || ''),
      repeatSize: Math.max(0.01, number(opennessSource.repeatSize, 1)),
      minOpenness: minimum,
      maxOpenness: Math.max(minimum, Math.min(MAX_SURFACE_OPENNESS, number(opennessSource.maxOpenness, 0.5))),
      levels: Math.max(2, Math.min(32, Math.round(number(opennessSource.levels, 8))))
    }
  }
  return result
}

// 读取光谱条目的开孔率 openness，限幅到 [0, MAX_SURFACE_OPENNESS]，缺省 0（实心面）。
export function surfaceOpenness(spectrum = {}) {
  return Math.max(0, Math.min(MAX_SURFACE_OPENNESS, number(spectrum.openness, 0)))
}

// Holes of fraction O pass radiation unchanged, so a perforated sheet acts as a
// continuous facet with R' = (1-O)R and T' = O + (1-O)T (energy conserving).
// Light through the holes becomes diffuse: no sunflecks under the sheet.
export function porousOptics(reflectance, transmittance, openness) {
  const o = Math.max(0, Math.min(MAX_SURFACE_OPENNESS, Number(openness) || 0))
  const r = Math.max(0, Number(reflectance) || 0)
  const t = Math.max(0, Number(transmittance) || 0)
  return { reflectance: (1 - o) * r, transmittance: o + (1 - o) * t }
}

// 作用：对逐波段的反射率/透射率列表（空格、逗号或分号分隔的字符串）逐项应用 porousOptics。
// 说明：两个列表长度不同时，长度为 1 的列表对所有波段广播；输出为逗号分隔的 6 位小数字符串。
function porousList(reflectance, transmittance, openness) {
  const split = (value) => String(value ?? '').split(/[\s,;]+/).filter(Boolean).map(Number)
  const r = split(reflectance), t = split(transmittance)
  const count = Math.max(r.length, t.length, 1)
  const at = (values, index) => values[values.length === 1 ? 0 : index] ?? values[0] ?? 0
  const pairs = Array.from({ length: count }, (_, index) => porousOptics(at(r, index), at(t, index), openness))
  const format = (value) => String(Number(value.toFixed(6)))
  return {
    reflectance: pairs.map((pair) => format(pair.reflectance)).join(','),
    transmittance: pairs.map((pair) => format(pair.transmittance)).join(',')
  }
}

// 作用：把带开孔率的光谱条目转换为求解器实际使用的等效光谱（openness 为 0 时原样返回）。
// 步骤：1) 对反/透射率应用孔洞公式；2) 对热红外 refTir/tauTir 同样处理；
//   3) model 为 file 时读入光谱文件，转换后写成新文件并替换 fileName。
// Returns the solver spectrum for a porous sheet; `readSpectrumFile` returns
// the 2-3 row TXT for model "file" and `writeSpectrumFile` stores the result.
export function effectivePorousSpectrum(spectrum, { readSpectrumFile, writeSpectrumFile } = {}) {
  const openness = surfaceOpenness(spectrum)
  if (!(openness > 0)) return spectrum
  // 热红外：refTir 缺省 0.05、tauTir 缺省 0。
  const tir = porousOptics(spectrum.refTir ?? 0.05, spectrum.tauTir ?? 0, openness)
  const round = (value) => Number(value.toFixed(6))
  const result = { ...spectrum, ...porousList(spectrum.reflectance, spectrum.transmittance, openness), refTir: round(tir.reflectance), tauTir: round(tir.transmittance) }
  delete result.openness
  if (spectrum.model === 'file' && readSpectrumFile && writeSpectrumFile) {
    const rows = String(readSpectrumFile(spectrum.fileName)).replace(/^\uFEFF/, '').split(/\r?\n/)
      .map((line) => line.replace(/#.*$/, '').trim()).filter(Boolean)
    // 光谱文件格式：第 1 行波长，第 2 行反射率，第 3 行透射率（缺省 0）；# 之后为注释。
    const wavelengths = rows[0] || ''
    const converted = porousList(rows[1] || '0', rows[2] || '0', openness)
    result.fileName = writeSpectrumFile(`# porous sheet, openness ${openness}\n${wavelengths}\n${converted.reflectance.replaceAll(',', ' ')}\n${converted.transmittance.replaceAll(',', ' ')}\n`)
  }
  return result
}

// ---------------------------------------------------------------------------------------------------------------
// FacetEB vehicle parts (docs/vehicle-thermal.md section 2-4). All values are typical values to be verified
// (标注“典型值待核对”). Paint / glass / tyre / trim spectra are broadband: reflectance = rho_sol, transmittance =
// tau_sol, refTir = 1 - emissivity (TIR transmittance 0).
const vehicleSpectrum = (name, label, reflectance, transmittance, refTir, preview = 'metal') => ({ name, label, model: 'custom', reflectance: String(reflectance),
  transmittance: String(transmittance), refTir, tauTir: 0, previewTexture: { enabled: preview !== '', preset: preview || 'metal', repeatSize: 1.5 },
  physicalTexture: { enabled: false, fileName: '', strength: 0.15, repeatSize: 1.5 }, params: {} })
// Paint colours (rho_sol, emissivity 0.88): white 0.65, silver 0.45, black 0.06, red 0.30, blue 0.22, grey 0.30,
// near-infrared "cool" black 0.25 (black in the visible, reflective pigments in the NIR).
export const VEHICLE_PAINT_COLORS = [
  { id: 'white', label: '白', reflectance: 0.65 }, { id: 'silver', label: '银', reflectance: 0.45 }, { id: 'black', label: '黑', reflectance: 0.06 },
  { id: 'red', label: '红', reflectance: 0.30 }, { id: 'blue', label: '蓝', reflectance: 0.22 }, { id: 'grey', label: '灰', reflectance: 0.30 },
  { id: 'black_cool', label: '黑（近红外冷漆）', reflectance: 0.25 }
]
// 车漆颜色 id → 光谱预设名；未知颜色回退为 white。
export const vehiclePaintSpectrumName = (color) => `vehicle_paint_${VEHICLE_PAINT_COLORS.some((entry) => entry.id === color) ? color : 'white'}`
// 车辆部件光谱预设：7 种车漆 + 前/侧/后挡风玻璃与隔热膜、灯具、轮胎、饰件、裸露金属。
// 各数值均为典型值，待核对（见上方说明与 docs/vehicle-thermal.md）。
export const VEHICLE_SPECTRA = [
  ...VEHICLE_PAINT_COLORS.map((entry) => vehicleSpectrum(vehiclePaintSpectrumName(entry.id), `车漆·${entry.label}（ρ_sol≈${entry.reflectance}，ε≈0.88）`, entry.reflectance, 0, 0.12)),
  // Windshield: laminated solar-control glass tau 0.60, rho 0.25 -> absorptance 0.15. Side: tempered tau 0.70 (rho 0.08, abs 0.22).
  // Rear: tempered privacy glass tau 0.65 (rho 0.10, abs 0.25). Films: heat-insulating / low-E, tau 0.35-0.45.
  vehicleSpectrum('vehicle_glass_front', '前挡风夹层玻璃（τ_sol 0.60，吸收≈0.15，ε 0.9）', 0.25, 0.60, 0.1, ''),
  vehicleSpectrum('vehicle_glass_side', '侧窗钢化玻璃（τ_sol 0.70，ε 0.9）', 0.08, 0.70, 0.1, ''),
  vehicleSpectrum('vehicle_glass_rear', '后窗钢化玻璃（τ_sol 0.65，ε 0.9）', 0.10, 0.65, 0.1, ''),
  vehicleSpectrum('vehicle_glass_front_film', '前挡风隔热膜/Low-E（τ_sol 0.45）', 0.30, 0.45, 0.1, ''),
  vehicleSpectrum('vehicle_glass_side_film', '侧/后窗隔热膜（τ_sol 0.38）', 0.32, 0.38, 0.1, ''),
  vehicleSpectrum('vehicle_lamp_glass', '灯具玻璃/透镜（不透入座舱，ε 0.9）', 0.08, 0, 0.1, ''),
  vehicleSpectrum('vehicle_tyre_rubber', '轮胎橡胶（ρ_sol 0.05，ε 0.9）', 0.05, 0, 0.1),
  vehicleSpectrum('vehicle_trim_grey', '塑料饰件·灰（ρ_sol 0.20，ε 0.9）', 0.20, 0, 0.1),
  vehicleSpectrum('vehicle_metal_bare', '裸露金属（ρ_sol 0.55，ε 0.2；排气/底盘金属件可选）', 0.55, 0, 0.8)
]
// Interior nodes of the cabin (dashboard, seat, steering wheel, roof lining + door panels): share of the absorbed
// transmitted sun (dashboard 0.3 = most strongly lit, absorptance ~0.9; seats 0.35; wheel 0.05; lining/doors 0.3),
// thermal capacity (J/K), effective hA to the cabin air (W/K, (h_conv + h_rad) A with h ~ 8 W m-2 K-1), radiating area (m2,
// weights of the exchange with the shell inner surface through the wall boundary). Seat: fabric / leather differ in capacity and contact effusivity (J m-2 K-1 s-0.5).
export const VEHICLE_SEAT_TYPES = { fabric: { capacity: 35000, effusivity: 200 }, leather: { capacity: 50000, effusivity: 450 } }
export function vehicleInteriorDefaults(seatType = 'fabric') {
  const seat = VEHICLE_SEAT_TYPES[seatType] || VEHICLE_SEAT_TYPES.fabric
  return { enabled: true, seatType: VEHICLE_SEAT_TYPES[seatType] ? seatType : 'fabric', shellRadFraction: 0.45, seatEffusivity: seat.effusivity,
    dashboard: { share: 0.30, capacity: 10000, hcA: 8, radArea: 1 }, seat: { share: 0.35, capacity: seat.capacity, hcA: 24, radArea: 3 },
    wheel: { share: 0.05, capacity: 2000, hcA: 2, radArea: 0.25 }, lining: { share: 0.30, capacity: 60000, hcA: 40, radArea: 8 } }
}
// Engine block + engine-bay air: Q_eng = enginePower x engineShare split into radiator (to the bay air stream),
// block and exhaust (underbody shell); firewallG = bulkhead conductance bay -> cabin (W/K).
export function vehicleEngineDefaults() {
  return { enabled: true, warm: false, warmTemperature: 90, blockCapacity: 70000, bayCapacity: 15000, blockToBay: 40, bayVent0: 0.04, bayVentK: 0.004,
    radiatorShare: 0.5, blockShare: 0.3, exhaustShare: 0.2, firewallG: 2 }
}
// Auto-binding table: OBJ group name -> material family. First matching rule wins. `spectral` / `material` are
// preset names; `paint` = the paint colour spectrum; `body` = the per-vehicle body material (carries cabin
// parameters); `keep` = keep the spectrum already chosen (tarp). `glass` rules are refined by facet orientation
// (splitVehicleObj: front / side / rear) when the OBJ has headlight and tail-light groups.
export const VEHICLE_BINDING_RULES = [
  { id: 'hood', label: '引擎盖', pattern: /(^|[_\s-])(hood|bonnet)([_\s-]|$)/i, spectral: 'paint', material: 'vehicle_hood' },
  { id: 'windshield', label: '前挡风', pattern: /(windshield|windscreen|front[_\s-]*(window|glass|screen)|(windows?|glass)[_\s-]*front)/i, spectral: 'vehicle_glass_front', material: 'vehicle_glass_laminated' },
  { id: 'rearWindow', label: '后窗', pattern: /(rear[_\s-]*(window|glass|screen)|back[_\s-]*window|(windows?|glass)[_\s-]*rear)/i, spectral: 'vehicle_glass_rear', material: 'vehicle_glass' },
  { id: 'sideWindow', label: '侧窗', pattern: /(side[_\s-]*(window|glass)|door[_\s-]*(window|glass)|(windows?|glass)[_\s-]*side)/i, spectral: 'vehicle_glass_side', material: 'vehicle_glass' },
  { id: 'glass', label: '车窗玻璃', pattern: /(^|[_\s-])(windows?|glass|glazing)([_\s-]|$)/i, spectral: 'vehicle_glass_side', material: 'vehicle_glass' },
  { id: 'lamp', label: '灯具玻璃', pattern: /(head[_\s-]*(light|lamp)|tail[_\s-]*(light|lamp)|light|lamp)/i, spectral: 'vehicle_lamp_glass', material: 'vehicle_lamp' },
  { id: 'tarpWheel', label: '篷布车轮', pattern: /part_2__covered_car/i, spectral: 'vehicle_tyre_rubber', material: 'vehicle_tyre' },
  { id: 'tarp', label: '篷布（保持原有篷布处理）', pattern: /covered_car/i, spectral: 'keep', material: 'body' },
  { id: 'tyre', label: '轮胎', pattern: /(^|[_\s-])(black|tyres?|tires?|wheels?|rubber)([_\s-]|$)/i, spectral: 'vehicle_tyre_rubber', material: 'vehicle_tyre' },
  { id: 'trim', label: '塑料饰件', pattern: /(^|[_\s-])(grey|gray|trim|plastic|bumper|grille)([_\s-]|$)/i, spectral: 'vehicle_trim_grey', material: 'vehicle_trim' },
  { id: 'paint', label: '车身漆面', pattern: /(white|blue|red|silver|green|yellow|orange|brown|paint|body|car|chassis)/i, spectral: 'paint', material: 'body' },
  { id: 'other', label: '其余（车身/底盘）', pattern: /.*/, spectral: 'paint', material: 'body' }
]
// Binding of one group: { rule, spectralName, materialName }; spectralName null = keep the current choice.
export function vehicleMeshBinding(meshName, { paint = 'white', film = false, bodyMaterial = 'vehicle_material' } = {}) {
  const rule = VEHICLE_BINDING_RULES.find((entry) => entry.pattern.test(String(meshName || ''))) || VEHICLE_BINDING_RULES[VEHICLE_BINDING_RULES.length - 1]
  let spectralName = rule.spectral === 'paint' ? vehiclePaintSpectrumName(paint) : rule.spectral === 'keep' ? null : rule.spectral
  // 开启隔热膜时，前挡风换为前膜光谱，侧窗/后窗换为侧/后膜光谱。
  if (film && spectralName === 'vehicle_glass_front') spectralName = 'vehicle_glass_front_film'
  else if (film && (spectralName === 'vehicle_glass_side' || spectralName === 'vehicle_glass_rear')) spectralName = 'vehicle_glass_side_film'
  return { rule: rule.id, spectralName, materialName: rule.material === 'body' ? bodyMaterial : rule.material }
}
// Preset spectra / materials the auto-binding refers to, added to a project only when a vehicle object is created.
export function vehiclePresetNames() {
  return { spectra: VEHICLE_SPECTRA.map((item) => item.name), materials: ['vehicle_material', 'vehicle_glass', 'vehicle_glass_laminated', 'vehicle_hood', 'vehicle_tyre', 'vehicle_trim', 'vehicle_lamp', 'vehicle_underbody'] }
}

// Splits the glass group of a vehicle OBJ into <name>_front / _side / _rear (windshield, side windows, rear window)
// and adds <paint>_hood to the paint group, by facet orientation in the vehicle frame. Forward = tail-light group
// centroid -> headlight group centroid (horizontal; y is up). Without both light groups nothing is split.
// Returns { content, split: { Windows: ['Windows_front', ...] }, forward, counts } (content unchanged if no split).
// 步骤（函数内部）：1) 解析 OBJ，按 g/o 分组；2) 用前/尾灯组质心定车头方向 forward；
//   3) 玻璃面元按法线和位置分前/侧/后；4) 车漆组中取引擎盖面元；5) 重新输出各组。
export function splitVehicleObj(content, { glassRule = VEHICLE_BINDING_RULES.find((r) => r.id === 'glass'), paintRule = VEHICLE_BINDING_RULES.find((r) => r.id === 'paint') } = {}) {
  // 步骤 1：解析 OBJ 文本：收集顶点坐标（用于面元几何）、按 g/o 行分组，顶点类数据提到文件头 head。
  const lines = String(content).split(/\r?\n/)
  const vertices = []
  const groups = []                       // { name, lines: [] }; faces keep order
  let head = [], current = null
  for (const line of lines) {
    if (line.startsWith('v ')) vertices.push(line.slice(2).trim().split(/\s+/).slice(0, 3).map(Number))
    // Vertex data may be interleaved with the faces: hoist it (indices are absolute, order per type is kept).
    if (/^(v|vn|vt|vp)\s/.test(line)) { head.push(line); continue }
    if (/^[go]\s/.test(line)) { current = { name: line.slice(2).trim(), header: line, lines: [] }; groups.push(current); continue }
    if (current) current.lines.push(line); else head.push(line)
  }
  const faceOf = (line) => line.slice(2).trim().split(/\s+/).map((token) => {
    const index = parseInt(token.split('/')[0], 10)
    return vertices[index < 0 ? vertices.length + index : index - 1]
  })
  const centroid = (points) => points.reduce((sum, p) => [sum[0] + p[0] / points.length, sum[1] + p[1] / points.length, sum[2] + p[2] / points.length], [0, 0, 0])
  const normalOf = (points) => {       // Newell
    let n = [0, 0, 0]
    for (let i = 0; i < points.length; i += 1) {
      const a = points[i], b = points[(i + 1) % points.length]
      n = [n[0] + (a[1] - b[1]) * (a[2] + b[2]), n[1] + (a[2] - b[2]) * (a[0] + b[0]), n[2] + (a[0] - b[0]) * (a[1] + b[1])]
    }
    const length = Math.hypot(...n) || 1
    return n.map((value) => value / length)
  }
  const groupCentroid = (group) => {
    const points = group.lines.filter((l) => l.startsWith('f ')).flatMap(faceOf)
    return points.length ? centroid(points) : null
  }
  const find = (re) => groups.find((group) => re.test(group.name))
  // 步骤 2：用前灯组与尾灯组的质心确定车头方向 forward（水平，y 轴向上）。
  //   缺任一灯组、或两灯质心水平距离不足 0.5（OBJ 坐标单位，视为 m，待核对）时不拆分。
  const headGroup = find(/head[_\s-]*(light|lamp)/i), tailGroup = find(/tail[_\s-]*(light|lamp)/i)
  const none = { content, split: {}, forward: null, counts: {} }
  if (!headGroup || !tailGroup) return none
  const headC = groupCentroid(headGroup), tailC = groupCentroid(tailGroup)
  if (!headC || !tailC) return none
  let forward = [headC[0] - tailC[0], 0, headC[2] - tailC[2]]
  const forwardLength = Math.hypot(...forward)
  if (!(forwardLength > 0.5)) return none
  forward = forward.map((value) => value / forwardLength)
  // lateral：水平且垂直于 forward 的车辆侧向单位向量。
  const lateral = [-forward[2], 0, forward[0]]        // up (0,1,0) x forward: horizontal, to the vehicle's side
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
  const glassGroups = groups.filter((group) => glassRule.pattern.test(group.name) && !/head|tail|light|lamp/i.test(group.name) && group.lines.some((l) => l.startsWith('f ')))
  const counts = {}, split = {}
  const emitted = []
  let windshieldBase = null
  const glassInfo = new Map()
  // 步骤 3：玻璃面元分类：法线与 lateral 的夹角余弦绝对值 > 0.6 为侧窗；
  //   其余按面元质心沿 forward 的位置相对玻璃组中点分为前挡/后窗。
  //   同时记录最靠前的前挡点 windshieldBase，供引擎盖判定使用。
  for (const group of glassGroups) {
    const faces = group.lines.filter((l) => l.startsWith('f '))
    const all = faces.flatMap(faceOf)
    const middle = dot(centroid(all), forward)
    const classes = { front: [], side: [], rear: [] }
    for (const line of faces) {
      const points = faceOf(line), n = normalOf(points), c = centroid(points)
      const side = Math.abs(dot(n, lateral)) > 0.6
      classes[side ? 'side' : dot(c, forward) >= middle ? 'front' : 'rear'].push(line)
    }
    glassInfo.set(group, classes)
    const front = classes.front.flatMap(faceOf)
    if (front.length) {
      const base = front.reduce((best, p) => (dot(p, forward) > dot(best, forward) ? p : best), front[0])
      if (!windshieldBase || dot(base, forward) > dot(windshieldBase, forward)) windshieldBase = base
    }
  }
  // 步骤 4：逐组输出；玻璃组按分类拆成 _front/_side/_rear；车漆组中满足引擎盖条件的面元拆成 _hood。
  for (const group of groups) {
    const classes = glassInfo.get(group)
    if (classes) {
      const parts = ['front', 'side', 'rear'].filter((key) => classes[key].length)
      if (parts.length) {
        split[group.name] = parts.map((key) => `${group.name}_${key}`)
        for (const key of parts) { counts[`${group.name}_${key}`] = classes[key].length; emitted.push({ header: `g ${group.name}_${key}`, lines: [...group.lines.filter((l) => !l.startsWith('f ')), ...classes[key]] }) }
        continue
      }
    }
    // Paint group: the bonnet = upward facets forward of the windshield base and not above it.
    if (windshieldBase && paintRule.pattern.test(group.name) && !/head|tail|light|lamp|window|glass|tyre|tire|wheel|black|grey|gray|trim/i.test(group.name) &&
        !VEHICLE_BINDING_RULES.slice(0, 8).some((rule) => rule.pattern.test(group.name))) {
      const hood = [], rest = []
      for (const line of group.lines) {
        if (!line.startsWith('f ')) { rest.push(line); continue }
        const points = faceOf(line), n = normalOf(points), c = centroid(points)
        const isHood = n[1] > 0.5 && dot(c, forward) > dot(windshieldBase, forward) - 0.05 && c[1] < windshieldBase[1] + 0.12
        ;(isHood ? hood : rest).push(line)
      }
      if (hood.length) {
        split[group.name] = [group.name, `${group.name}_hood`]
        counts[`${group.name}_hood`] = hood.length
        counts[group.name] = rest.filter((l) => l.startsWith('f ')).length
        emitted.push({ header: group.header, lines: rest })
        emitted.push({ header: `g ${group.name}_hood`, lines: [...group.lines.filter((l) => !l.startsWith('f ')), ...hood] })
        continue
      }
    }
    emitted.push({ header: group.header, lines: group.lines })
  }
  // 步骤 5：没有任何组被拆分则原样返回；否则按 head + 各组顺序重新拼接 OBJ。
  if (!Object.keys(split).length) return none
  const out = [...head]
  for (const group of emitted) out.push(group.header, ...group.lines)
  return { content: out.join('\n'), split, forward, counts }
}
// splitVehicleObj 产生的组名后缀 → VEHICLE_BINDING_RULES 的规则 id。
export const VEHICLE_SPLIT_BINDINGS = { _front: 'windshield', _side: 'sideWindow', _rear: 'rearWindow', _hood: 'hood' }
// 作用：基于 vehicle_material 预设生成一辆车专属的车身材质（座舱/行驶/发动机状态写入 params.cabin）。
// 说明：窗微开/全开的换气次数 crackedACH 取 10/30（单位待核对）；interior/engine 为 false 时删除对应子项。
// Body material of one vehicle (the vehicle_material preset with this car's cabin / driving / engine state).
// vent: 'closed' | 'crack' | 'open'; speed > 0 = driving; heading and windDirection together (degrees) or both absent.
export function createVehicleBodyMaterial(name, { volume = 2.8, occupants = 0, speed = 0, heading, windDirection, vent = 'closed', engineOn = false, enginePower = 15000,
  engineWarm = false, acOn = false, acPower = 3500, acSetpoint = 24, seat = 'fabric', interior = true, engine = true } = {}) {
  const base = structuredClone(createMaterialPresets().materials.find((entry) => entry.name === 'vehicle_material'))
  const cabin = { ...base.params.cabin, enabled: true, occupants, volume, speed, windowCracked: vent !== 'closed', crackedACH: vent === 'open' ? 30 : 10,
    engineOn, enginePower, acOn, acPower, acSetpoint }
  if (interior) cabin.interior = vehicleInteriorDefaults(seat); else delete cabin.interior
  if (engine) cabin.engine = { ...vehicleEngineDefaults(), warm: engineWarm }; else delete cabin.engine
  if (Number.isFinite(heading) && Number.isFinite(windDirection)) { cabin.heading = heading; cabin.windDirection = windDirection }
  return { ...base, name, label: `${name} 车身（座舱）`, params: { ...base.params, cabin } }
}

// 模块：内置预设库（新工程的初始内容，也是 normalizeProject 补全缺失预设的来源）。
// 输出：{ spectra, canopies, thermals, materials }：
//   spectra 光谱（反/透射率、热红外 refTir/tauTir，model 为 custom/Prospect/BSM/file）；
//   canopies 冠层/结构（LAI、叶倾角分布参数、刚体、火焰/雾等参与介质）；
//   thermals 光照面/阴影面的固定表面温度（K）；
//   materials 物化参数（建筑多层墙体、光伏、土壤、植被生理、木质、车辆、船舶、水体）。
// 说明：数值多为典型值，用户可在界面编辑；注释中的文献/规范出处为原作者标注。
export function createMaterialPresets() {
  return {
    spectra: [
      { name: 'pv_opaque', label: '太阳能板光谱', model: 'custom', reflectance: '0.08', transmittance: '0', refTir: 0.1, tauTir: 0, previewTexture: { enabled: true, preset: 'solar-panel', repeatSize: 0.18 } },
      { name: 'soil', label: '典型土壤', model: 'BSM', reflectance: '0.20', transmittance: '0.0', refTir: 0.05, tauTir: 0, params: { SMC: 25, BSMBrightness: 0.5, BSMlat: 25, BSMlon: 45 } },
      { name: 'green_leaf', label: '健康绿叶', model: 'Prospect', reflectance: '0.08', transmittance: '0.04', refTir: 0.03, tauTir: 0, params: { Cab: 40, Cw: 0.01, Cdm: 0.01, Cs: 0, N: 1.5 } },
      { name: 'dry_leaf', label: '干燥叶片', model: 'Prospect', reflectance: '0.16', transmittance: '0.08', refTir: 0.05, tauTir: 0, params: { Cab: 15, Cw: 0.003, Cdm: 0.02, Cs: 0, N: 1.8 } },
      { name: 'maize_leaf', label: '玉米健康叶片', model: 'Prospect', reflectance: '0.07', transmittance: '0.04', refTir: 0.025, tauTir: 0, params: { Cab: 48, Cw: 0.015, Cdm: 0.009, Cs: 0, N: 1.45 } },
      { name: 'wheat_leaf', label: '小麦健康叶片', model: 'Prospect', reflectance: '0.08', transmittance: '0.045', refTir: 0.03, tauTir: 0, params: { Cab: 42, Cw: 0.012, Cdm: 0.01, Cs: 0, N: 1.5 } },
      { name: 'rice_leaf', label: '水稻健康叶片', model: 'Prospect', reflectance: '0.075', transmittance: '0.045', refTir: 0.025, tauTir: 0, params: { Cab: 45, Cw: 0.014, Cdm: 0.009, Cs: 0, N: 1.5 } },
      { name: 'senescent_crop_leaf', label: '衰老作物叶片', model: 'Prospect', reflectance: '0.18', transmittance: '0.09', refTir: 0.045, tauTir: 0, params: { Cab: 8, Cw: 0.004, Cdm: 0.02, Cs: 0.2, N: 1.8 } },
      { name: 'broadleaf_tree_leaf', label: '阔叶树叶片', model: 'Prospect', reflectance: '0.07', transmittance: '0.04', refTir: 0.025, tauTir: 0, params: { Cab: 52, Cw: 0.016, Cdm: 0.012, Cs: 0, N: 1.55 } },
      { name: 'conifer_needle', label: '针叶树针叶', model: 'Prospect', reflectance: '0.09', transmittance: '0.025', refTir: 0.035, tauTir: 0, params: { Cab: 38, Cw: 0.011, Cdm: 0.016, Cs: 0, N: 1.75 } },
      { name: 'tree_wood', label: '树干与树枝木质部', model: 'custom', reflectance: '0.18', transmittance: '0.0', refTir: 0.06, tauTir: 0, params: {} },
      { name: 'soil_dry_spectral', label: '干燥壤土光谱', model: 'BSM', reflectance: '0.28', transmittance: '0.0', refTir: 0.07, tauTir: 0, params: { SMC: 8, BSMBrightness: 0.68, BSMlat: 25, BSMlon: 45 } },
      { name: 'soil_wet_spectral', label: '湿润壤土光谱', model: 'BSM', reflectance: '0.12', transmittance: '0.0', refTir: 0.035, tauTir: 0, params: { SMC: 38, BSMBrightness: 0.35, BSMlat: 25, BSMlon: 45 } },
      { name: 'concrete', label: '混凝土', model: 'custom', reflectance: '0.35', transmittance: '0.0', refTir: 0.08, tauTir: 0, previewTexture: { enabled: true, preset: 'concrete', repeatSize: 2 }, physicalTexture: { enabled: false, fileName: 'assets/texture-library/concrete_variance.jpg', strength: 0.2, repeatSize: 2 }, params: {} },
      { name: 'asphalt', label: '沥青路面', model: 'custom', reflectance: '0.12', transmittance: '0.0', refTir: 0.05, tauTir: 0, previewTexture: { enabled: true, preset: 'gravel', repeatSize: 3 }, physicalTexture: { enabled: false, fileName: 'assets/texture-library/asphalt_variance.jpg', strength: 0.2, repeatSize: 3 }, params: {} },
      { name: 'red_brick', label: '红砖墙面', model: 'custom', reflectance: '0.28', transmittance: '0.0', refTir: 0.07, tauTir: 0, previewTexture: { enabled: true, preset: 'red-brick', repeatSize: 1.2 }, physicalTexture: { enabled: false, fileName: 'assets/texture-library/brick_variance.jpg', strength: 0.2, repeatSize: 2 }, params: {} },
      { name: 'human_surface', label: '人员典型光谱', model: 'custom', reflectance: '0.30', transmittance: '0.0', refTir: 0.02, tauTir: 0, params: {} },
      { name: 'vehicle_surface', label: '载具典型光谱', model: 'custom', reflectance: '0.20', transmittance: '0.0', refTir: 0.08, tauTir: 0, previewTexture: { enabled: true, preset: 'metal', repeatSize: 1.5 }, physicalTexture: { enabled: false, fileName: 'assets/texture-library/metal_variance.jpg', strength: 0.15, repeatSize: 1.5 }, params: {} },
      { name: 'vehicle_glass', label: '车窗玻璃（τ_sol≈0.65）', model: 'custom', reflectance: '0.08', transmittance: '0.65', refTir: 0.1, tauTir: 0, previewTexture: { enabled: false, preset: 'concrete', repeatSize: 1 }, physicalTexture: { enabled: false, fileName: '', strength: 0.15, repeatSize: 1.5 }, params: {} },
      ...VEHICLE_SPECTRA,
      { name: 'ship_surface', label: '船舶典型光谱', model: 'custom', reflectance: '0.22', transmittance: '0.0', refTir: 0.06, tauTir: 0, params: {} },
      { name: 'water_surface', label: '水体光谱', model: 'custom', reflectance: '0.04', transmittance: '0.0', refTir: 0.02, tauTir: 0, params: {} },
      { name: 'tarp_olive_canvas', label: '篷布（军绿帆布）', model: 'file', fileName: 'assets/spectral-library/tarp_olive_canvas.txt', reflectance: '0.12', transmittance: '0.02', refTir: 0.10, tauTir: 0, previewTexture: { enabled: true, preset: 'canvas', repeatSize: 0.6 }, physicalTexture: { enabled: false, fileName: 'assets/texture-library/canvas_weave_variance.jpg', strength: 0.15, repeatSize: 0.5 }, params: {} },
      { name: 'shade_net_black', label: '遮阳网（黑色 HDPE，开孔 30%）', model: 'custom', reflectance: '0.05', transmittance: '0.02', refTir: 0.06, tauTir: 0, openness: 0.3, previewTexture: { enabled: true, preset: 'shade-net', repeatSize: 0.3 }, params: {} }
      ,{ name: 'fire_medium', label: '火焰介质', model: 'custom', reflectance: '0.0', transmittance: '0.0', refTir: 0, tauTir: 0, params: {} }
      ,{ name: 'fog_medium', label: '雾介质', model: 'custom', reflectance: '0.0', transmittance: '0.0', refTir: 0, tauTir: 0, params: {} }
    ],
    // 冠层/结构预设：structureType 为 canopy（混沌介质）、rigid（刚体）、fire/fog（参与介质，
    //   extinction 消光、scatteringAlbedo 散射反照率、asymmetry 不对称因子、fixedTemperature 固定温度 K）。
    canopies: [
      { name: 'canopy_default', label: '混沌介质（多孔透射）', structureType: 'canopy', lai: 3, density: 1, hc: 2, G: 0.5, LIDFa: -0.35, LIDFb: -0.15, hspot: 0.2, leafwidth: 0.1 },
      { name: 'tree_leaf_canopy', label: '树叶（多孔透射）', structureType: 'canopy', lai: 3, density: 1, hc: 2, G: 0.5, LIDFa: -0.35, LIDFb: -0.15, hspot: 0.2, leafwidth: 0.08 },
      { name: 'tree_branch_rigid', label: '树枝（刚性木质）', structureType: 'rigid', lai: 0, density: 1, hc: 2, G: 0.5, LIDFa: 0, LIDFb: 0, hspot: 0, leafwidth: 0 },
      { name: 'tree_trunk_rigid', label: '树干（刚性木质）', structureType: 'rigid', lai: 0, density: 1, hc: 2, G: 0.5, LIDFa: 0, LIDFb: 0, hspot: 0, leafwidth: 0 },
      { name: 'rigid_body', label: '刚体（仅轮廓不透光）', structureType: 'rigid', lai: 0, density: 0, hc: 2, G: 0.5, LIDFa: -0.35, LIDFb: -0.15, hspot: 0, leafwidth: 0.1 },
      { name: 'fire_medium', label: '火焰（参与介质）', structureType: 'fire', lai: 0, density: 0, hc: 3, G: 0, LIDFa: 0, LIDFb: 0, hspot: 0, leafwidth: 0, extinction: 1.2, scatteringAlbedo: 0.72, asymmetry: 0.55, emissionScale: 1, fixedTemperature: 1100 },
      { name: 'fog_medium', label: '雾（参与介质）', structureType: 'fog', lai: 0, density: 0, hc: 10, G: 0, LIDFa: 0, LIDFb: 0, hspot: 0, leafwidth: 0, extinction: 0.08, scatteringAlbedo: 0.95, asymmetry: 0.85, emissionScale: 1, fixedTemperature: 288 }
    ],
    // 表面温度预设：sunlitTemperature/shadedTemperature 分别为受光面/阴影面温度（K，用途待核对）。
    thermals: [
      { name: 'soil_temperature', label: '土壤温度', sunlitTemperature: 305, shadedTemperature: 295 },
      { name: 'vegetation_temperature', label: '植被温度', sunlitTemperature: 300, shadedTemperature: 296 },
      { name: 'tree_wood_temperature', label: '树干与树枝温度', sunlitTemperature: 303, shadedTemperature: 297 },
      { name: 'building_temperature', label: '建筑表面温度', sunlitTemperature: 310, shadedTemperature: 300 },
      { name: 'human_temperature', label: '人员表面温度', sunlitTemperature: 310, shadedTemperature: 307 },
      { name: 'vehicle_temperature', label: '载具表面温度', sunlitTemperature: 315, shadedTemperature: 305 },
      { name: 'ship_temperature', label: '船舶表面温度', sunlitTemperature: 313, shadedTemperature: 303 },
      { name: 'pv_temperature', label: '太阳能板初始温度', sunlitTemperature: 318, shadedTemperature: 303 },
      { name: 'water_temperature', label: '水体初始温度', sunlitTemperature: 295, shadedTemperature: 295 },
      { name: 'tarp_temperature', label: '篷布初始温度', sunlitTemperature: 318, shadedTemperature: 303 }
      ,{ name: 'fire_temperature', label: '火焰固定温度', sunlitTemperature: 1100, shadedTemperature: 1100 }
      ,{ name: 'fog_temperature', label: '雾介质温度', sunlitTemperature: 288, shadedTemperature: 288 }
    ],
    // 物化参数预设：type 为 Building/Vegetation/Soil/Vehicle/Ship/Water；
    //   wallMode 为 wall 的条目按 layers（由外到内）逐层求解导热，厚度 m、λ W m-1 K-1、ρ kg m-3、cp J kg-1 K-1。
    materials: [
      // Representative dry concrete surface; users can edit these thermal properties.
      // wallMode 'wall': FacetEB solves the layered wall (params.layers, outer to inner) with a 22 °C
      // interior ('shell' = thin two-sided facet). New-project construction, typical values of
      // GB 50176-2016 Appendix B / ISO 10456: 0.10 m reinforced concrete (λ 1.74, ρ 2500, cp 920),
      // 0.08 m EPS insulation (λ 0.04, ρ 20, cp 1380), 0.02 m lime-cement plaster (λ 0.87, ρ 1700,
      // cp 1050); U ≈ 0.44 W m⁻² K⁻¹. cs/rhos/lambdas stay the VoxelEB (semi-infinite solid) values.
      { name: 'building_surface', label: '建筑表面', type: 'Building', params: { method: 2, rss: 1000000000, cs: 880, rhos: 2300, lambdas: 1.4, Tsoil: 25, SMC: 0, Satwater: 0, wallMode: 'wall', thickness: 0.2, indoorTemperature: 22, indoorHeatTransferCoefficient: 8,
        layers: [{ thickness: 0.1, lambda: 1.74, rho: 2500, cp: 920 }, { thickness: 0.08, lambda: 0.04, rho: 20, cp: 1380 }, { thickness: 0.02, lambda: 0.87, rho: 1700, cp: 1050 }] } },
      // Flat roof: 0.05 m fine-aggregate concrete protection over the membrane (λ 1.51, ρ 2300, cp 920),
      // 0.10 m XPS (λ 0.030, ρ 35, cp 1380), 0.15 m reinforced concrete slab (λ 1.74, ρ 2500, cp 920);
      // U ≈ 0.28 W m⁻² K⁻¹ (GB 50176-2016 Appendix B typical values).
      { name: 'building_roof', label: '建筑屋顶', type: 'Building', params: { method: 2, rss: 1000000000, cs: 880, rhos: 2300, lambdas: 1.4, Tsoil: 25, SMC: 0, Satwater: 0, wallMode: 'wall', thickness: 0.3, indoorTemperature: 22, indoorHeatTransferCoefficient: 8,
        layers: [{ thickness: 0.05, lambda: 1.51, rho: 2300, cp: 920 }, { thickness: 0.1, lambda: 0.03, rho: 35, cp: 1380 }, { thickness: 0.15, lambda: 1.74, rho: 2500, cp: 920 }] } },
      // External thermal insulation composite system (ETICS, JGJ 144): 5 mm polymer render (λ 0.93,
      // ρ 1800, cp 1050), 0.08 m EPS, 0.20 m reinforced concrete, 0.02 m plaster; U ≈ 0.43 W m⁻² K⁻¹.
      // The light render over insulation gives the largest outer-surface daily range.
      { name: 'building_wall_etics', label: '建筑外墙（外保温薄抹灰）', type: 'Building', params: { method: 2, rss: 1000000000, cs: 880, rhos: 2300, lambdas: 1.4, Tsoil: 25, SMC: 0, Satwater: 0, wallMode: 'wall', thickness: 0.305, indoorTemperature: 22, indoorHeatTransferCoefficient: 8,
        layers: [{ thickness: 0.005, lambda: 0.93, rho: 1800, cp: 1050 }, { thickness: 0.08, lambda: 0.04, rho: 20, cp: 1380 }, { thickness: 0.2, lambda: 1.74, rho: 2500, cp: 920 }, { thickness: 0.02, lambda: 0.87, rho: 1700, cp: 1050 }] } },
      { name: 'pv_panel', label: '通用太阳能板（FacetEB）', type: 'Building', energyModel: 'photovoltaic', params: { eta25: 0.21, gamma: -0.0035, bifaciality: 0, heatCapacityPerArea: 12000, convectiveScale: 1 } },
      { name: 'pv_panel_mono', label: '单晶硅太阳能板（FacetEB）', type: 'Building', energyModel: 'photovoltaic', params: { eta25: 0.22, gamma: -0.0034, bifaciality: 0, heatCapacityPerArea: 12000, convectiveScale: 1 } },
      { name: 'pv_panel_poly', label: '多晶硅太阳能板（FacetEB）', type: 'Building', energyModel: 'photovoltaic', params: { eta25: 0.19, gamma: -0.0038, bifaciality: 0, heatCapacityPerArea: 12500, convectiveScale: 1 } },
      { name: 'pv_panel_bifacial', label: '双面太阳能板（FacetEB）', type: 'Building', energyModel: 'photovoltaic', params: { eta25: 0.215, gamma: -0.0034, bifaciality: 0.75, heatCapacityPerArea: 12500, convectiveScale: 1.1 } },
      // Areal heat capacity = areal density x specific heat (PVC-coated canvas ~0.6 kg m-2 x 1300 J kg-1 K-1).
      { name: 'tarp_fabric', label: '篷布织物薄膜（FacetEB）', type: 'Building', energyModel: 'membrane', params: { heatCapacityPerArea: 800, convectiveScale: 1 } },
      { name: 'shade_net_fabric', label: '遮阳网薄膜（FacetEB）', type: 'Building', energyModel: 'membrane', params: { heatCapacityPerArea: 250, convectiveScale: 1 } },
      {
        name: 'leaf_c3', label: 'C3 植被', type: 'Vegetation',
        params: { Vcmax: 80, m: 9, BallBerry: 0.01, Type: 3, kV: 0.6396, Rdparam: 0.015, Tparam: '0.2,0.3,288,313,328', Tyear: 25, beta: 0.507, kNPQs: 0, qLs: 1, stressfactor: 1, Tcor: 0 }
      },
      {
        name: 'leaf_c4', label: 'C4 植被', type: 'Vegetation',
        params: { Vcmax: 60, m: 4, BallBerry: 0.01, Type: 4, kV: 0.7, Rdparam: 0.012, Tparam: '0.2,0.3,288,313,328', Tyear: 25, beta: 0.507, kNPQs: 0, qLs: 1, stressfactor: 1, Tcor: 0 }
      },
      {
        name: 'tree_branch_wood', label: '树枝木质部', type: 'Vegetation', energyModel: 'wood',
        params: { method: 1, rss: 1000000000, cs: 1700, rhos: 550, lambdas: 0.14, Tsoil: 25, SMC: 0, Satwater: 0, convectiveScale: 1.5 }
      },
      {
        name: 'tree_trunk_wood', label: '树干木质部', type: 'Vegetation', energyModel: 'wood',
        params: { method: 1, rss: 1000000000, cs: 1700, rhos: 650, lambdas: 0.16, Tsoil: 25, SMC: 0, Satwater: 0, convectiveScale: 1.2 }
      },
      // Moist loam at SMC 25 %: cs/rhos are the bulk values at that moisture (dry bulk density
      // 1350 kg m⁻³ + 250 kg m⁻³ water, C = 2.08e6 J m⁻³ K⁻¹ = 1.1e6 dry + 1.05e6 water) and
      // λ 1.0 W m⁻¹ K⁻¹ (Johansen 1975 / Campbell & Norman 1998 loam at θ 0.25); thermal inertia
      // √(λC) ≈ 1440 J m⁻² K⁻¹ s⁻½ (Murray & Verhoef 2007 ≈ 1380). The former SCOPE values
      // (1180, 1800, 1.55 → 1814) remain the engine fallback for missing fields.
      { name: 'soilset', label: '典型壤土', type: 'Soil', params: { method: 2, rss: 2000, cs: 1300, rhos: 1600, lambdas: 1.0, Tsoil: 25, SMC: 25, Satwater: 0.45 } },
      { name: 'soil_dry', label: '干燥土壤', type: 'Soil', params: { method: 2, rss: 4000, cs: 1000, rhos: 1600, lambdas: 0.8, Tsoil: 30, SMC: 10, Satwater: 0.38 } },
      { name: 'soil_wet', label: '湿润土壤', type: 'Soil', params: { method: 2, rss: 800, cs: 1400, rhos: 1900, lambdas: 2.0, Tsoil: 22, SMC: 35, Satwater: 0.50 } },
      // FacetEB vehicle (docs/vehicle-thermal.md): the body is a 1 mm steel sheet + 6 mm trim/insulation wall whose indoor
      // temperature is the lumped cabin air; glass is a 4.8 mm pane (tau_sol from its spectrum); underbody/tyres/lights
      // are thin shells. cabin.enabled switches the cabin node on; VoxelEB/RT modes ignore these fields. Old projects keep
      // their Building-type vehicle_material (fixed-temperature presets, bitwise old FacetEB results).
      { name: 'vehicle_material', label: '载具车身物化', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 25, SMC: 0, Satwater: 0,
        wallMode: 'wall', thickness: 0.007, indoorTemperature: 22, indoorHeatTransferCoefficient: 7, vehiclePart: 'body',
        layers: [{ thickness: 0.001, lambda: 50, rho: 7850, cp: 460 }, { thickness: 0.006, lambda: 0.04, rho: 60, cp: 1400 }],
        cabin: { enabled: true, occupants: 0, metabolicW: 100, speed: 0, ventilationACH: 0.5, windowCracked: false, crackedACH: 10, acOn: false, acPower: 3500, acSetpoint: 24,
          engineOn: false, enginePower: 15000, engineShare: 0.3, volume: 2.8, interiorCapacity: 80000, interiorArea: 8, interiorH: 8, absorbedFraction: 0.8,
          interior: vehicleInteriorDefaults('fabric'), engine: vehicleEngineDefaults() } } },
      // Laminated windshield (2.1 mm glass + 0.76 mm PVB + 2.1 mm glass); tempered side/rear glass is vehicle_glass.
      { name: 'vehicle_glass_laminated', label: '载具前挡风夹层玻璃', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 840, rhos: 2500, lambdas: 1, Tsoil: 25, SMC: 0, Satwater: 0,
        wallMode: 'wall', thickness: 0.00496, indoorTemperature: 22, indoorHeatTransferCoefficient: 8, vehiclePart: 'glass',
        layers: [{ thickness: 0.0021, lambda: 1, rho: 2500, cp: 840 }, { thickness: 0.00076, lambda: 0.2, rho: 1070, cp: 2000 }, { thickness: 0.0021, lambda: 1, rho: 2500, cp: 840 }] } },
      // Bonnet: 0.8 mm steel + 10 mm fibre insulation liner; the indoor side is the engine-bay air (needs cabin.engine).
      { name: 'vehicle_hood', label: '载具引擎盖（内侧=发动机舱空气）', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 25, SMC: 0, Satwater: 0,
        wallMode: 'wall', thickness: 0.0108, indoorTemperature: 22, indoorHeatTransferCoefficient: 10, vehiclePart: 'hood',
        layers: [{ thickness: 0.0008, lambda: 50, rho: 7850, cp: 460 }, { thickness: 0.01, lambda: 0.04, rho: 60, cp: 1400 }] } },
      { name: 'vehicle_tyre', label: '载具轮胎薄壳', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 25, SMC: 0, Satwater: 0,
        vehiclePart: 'underbody', heatCapacityPerArea: 12000 } },
      { name: 'vehicle_trim', label: '载具塑料饰件薄壳', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 25, SMC: 0, Satwater: 0,
        vehiclePart: 'underbody', heatCapacityPerArea: 3750 } },
      { name: 'vehicle_lamp', label: '载具灯具薄壳', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 25, SMC: 0, Satwater: 0,
        vehiclePart: 'underbody', heatCapacityPerArea: 5400 } },
      { name: 'vehicle_glass', label: '载具玻璃', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 840, rhos: 2500, lambdas: 1, Tsoil: 25, SMC: 0, Satwater: 0,
        wallMode: 'wall', thickness: 0.0048, indoorTemperature: 22, indoorHeatTransferCoefficient: 8, vehiclePart: 'glass',
        layers: [{ thickness: 0.0048, lambda: 1, rho: 2500, cp: 840 }] } },
      { name: 'vehicle_underbody', label: '载具内饰/底盘薄壳', type: 'Vehicle', params: { method: 2, rss: 1000000000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 25, SMC: 0, Satwater: 0,
        vehiclePart: 'underbody', heatCapacityPerArea: 6000 } },
      // FacetEB hull: one 12 mm shipbuilding steel plate (λ 50, ρ 7850, cp 460: typical mild/hull
      // steel, e.g. EN 1993-1-2 / ISO 10456 steel) with a 22 °C interior; speed (m/s) adds the
      // ship's own motion to the wind (relative wind); waterlineHeight (world y, m; absent = off)
      // puts the hull facets below it in contact with the water. method/lambdas/rhos/cs stay the
      // VoxelEB (semi-infinite solid) values; voxelWall (new-project default; absent = off, old projects keep the
      // semi-infinite hull) makes VoxelEB use the same plate (layers folded into one equivalent plate) with the
      // indoor side and internalHeatFlux; VoxelEB also reads speed, heading/windDirection and the
      // fixed waterlineHeight + waterHeatTransferCoefficient (hull voxels below it, scene water temperature).
      { name: 'ship_material', label: '船舶表面物化', type: 'Ship', params: { method: 1, rss: 4000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 30, SMC: 0, Satwater: 0,
        wallMode: 'wall', thickness: 0.012, indoorTemperature: 22, indoorHeatTransferCoefficient: 8, layers: [{ thickness: 0.012, lambda: 50, rho: 7850, cp: 460 }], voxelWall: true, speed: 0 } },
      // mixingDepth 2 m and a start temperature at the mean air temperature of the first 24 h:
      // new-project defaults (engine fallbacks for absent fields stay 0.5 m / first-node air).
      { name: 'water_set', label: '水体物化参数', type: 'Water', params: { rss: 0, heatCapacity: 4186000, mixingDepth: 2, evaporationCoefficient: 1, brdfModel: 1, refractiveIndex: 1.333, slopeVariance: 0, diffuseFraction: 0.02, initialTemperatureMode: 'dailyMeanAir' } }
    ]
  }
}

// 作用：生成新工程的完整缺省对象（schemaVersion、folders 与 configuration 各节的缺省值）。
// 输入：options（mode、name、sceneX/Y/Height、offset、voxelSize 等；缺省场景 60×60×15 m，体元 1 m）。
// 说明：新工程缺省与旧工程缺字段时的处理不同——旧工程的缺省在 normalizeProject 中单独处理，
//   以保证旧工程结果不变（见下方 diffuseFractionMode、stabilityCorrection 等字段说明）。
export function createDefaultProject(options = {}) {
  const mode = PROJECT_MODES.includes(options.mode) ? options.mode : 'eVoxelEB'
  const now = new Date().toISOString()
  const presets = createMaterialPresets()
  return {
    schemaVersion: PROJECT_SCHEMA_VERSION,
    format: PROJECT_FORMAT,
    name: String(options.name || '未命名工程'),
    mode,
    createdAt: options.createdAt || now,
    updatedAt: now,
    folders: {
      models: 'models',
      positions: 'positions',
      spectral: 'spectral',
      meteorology: 'meteorology',
      terrain: 'terrain',
      output: 'output'
    },
    configuration: {
      outDir: 'output',
      // 场景节：x/y/height 为场景范围（m），offset 为偏移（m），voxel 为体元边长（m），
      //   terrain/demFile/demInfo/terrainEnvironment 为地形，background 为场景背景的光谱/热/物化绑定。
      scene: {
        x: number(options.sceneX, 60),
        y: number(options.sceneY, 60),
        height: number(options.sceneHeight, 15),
        offsetX: number(options.sceneOffsetX, 0),
        offsetY: number(options.sceneOffsetY, 0),
        offsetZ: number(options.sceneOffsetZ, 0),
        voxel: number(options.voxelSize, 1),
        voxelFillThreshold: number(options.voxelFillThreshold, 0.05),
        terrain: false,
        demFile: '',
        demInfo: null,
        terrainEnvironment: { ...TERRAIN_ENVIRONMENT_DEFAULTS },
        background: {
          spectralName: 'soil', thermalName: 'soil_temperature',
          materialType: 'Soil', materialName: 'soilset',
          specularEnabled: false,
          heterogeneityEnabled: false, angularEffectStrength: 0.5
        }
      },
      // diffuseFractionMode：VoxelEB/FacetEB 气象短波的直散拆分。新工程默认 'erbs'（逐节点按实测直射/散射列或
      // Erbs 晴空指数模型）；旧工程缺字段在 normalizeProject 中按 'fixed'（全时段直射比例）处理，结果不变。
      light: { zenith: 30, azimuth: 135, direct: 0.8, diffuse: 0.2, skyTemperature: 250, shortwaveIrradiance: 0, diffuseFractionMode: 'erbs' },
      // 传感器节：x/y 为影像尺寸（像元数，待核对）；bands 为空格分隔的波段中心波长（nm）；
      //   continuousBands 开启时按 bandStart/bandEnd/bandStep 生成连续波段；
      //   positionX/Y/height 为传感器位置（m）；cruise* 为巡航路线参数（见 sensorCruisePositions）。
      sensor: {
        x: 512, y: 512, bands: DEFAULT_SENSOR_BANDS.join(' '), projection: 'parallel',
        vza: 0, vaa: 0,
        continuousBands: false, bandStart: 400, bandEnd: 2500, bandStep: 10,
        principalPlane: false, hemisphere: false,
        positionX: number(options.sceneX, 60) / 2,
        positionY: number(options.sceneY, 60) / 2,
        height: 3000,
        fov: 60,
        cruiseEnabled: false,
        cruiseHeading: 90,
        cruiseRoute: 'line',
        cruiseTargetX: number(options.sceneX, 60) * 0.75,
        cruiseTargetY: number(options.sceneY, 60) / 2,
        cruiseTargetZ: 3000,
        cruiseRectangleWidth: Math.min(number(options.sceneX, 60) * 0.25, 500),
        cruiseRectangleHeight: Math.min(number(options.sceneY, 60) * 0.25, 500),
        cruiseRandomSeed: 1,
        cruiseStep: 25,
        cruiseCount: 20,
        image: true, process: false, radiationProcess: false, energyProcess: false, temperature: true, albedo: false
      },
      // 运行控制节：采样数、深度、计算后端（gpu/cpu）、线程数、质量档位及各模式的迭代/步数上限。
      control: {
        depth: 4, raytracingDepth: 16, samples: 32, gpu: 0, specularEnabled: true,
        facetBackend: 'gpu',
        computeBackend: 'gpu',
        cpuThreads: DEFAULT_CPU_THREADS,
        heterogeneousVoxel: false,
        periodicTraversalCount: 0,
        skyboxEnabled: false,
        // Spectral-batch solver: same physics as 'traditional' (verified on the
        // VoxelRT/VoxelEB/FacetRT smoke cases, means equal to 1e-5) but
        // 1.1-1.6x faster; 'traditional' stays selectable for verification.
        radiationSolver: 'accelerated',
        spectralAccelerationWidth: 100,
        // 新工程按“标准”档：VoxelRT 3 次真实碰撞、VoxelEB 2 阶短波散射与按场景对角线的步数上限、
        // FacetEB 最多 60 次耦合迭代。缺这些字段的旧工程在 normalizeProject 中仍按旧值
        // （碰撞 2 次 / 散射 1 阶 / 步数 64）处理，档位记为 custom，不改动已有参数。
        qualityPreset: 'standard',
        runTimeoutMinutes: 0,
        shortwaveScatteringOrders: 2,
        voxelCollisionOrders: 3,
        radiationMaxSteps: autoRadiationMaxSteps({ x: options.sceneX, y: options.sceneY, height: options.sceneHeight, voxel: options.voxelSize }),
        // FacetEB temperature step damping. Smoke cases (crop/urban) converge
        // in 9-12 coupling iterations at 0.8 versus 16-19 at the former 0.5;
        // 1.0 is faster still but takes multi-kelvin steps on dense canopies.
        couplingIterations: options.mode === 'eFacetEB' ? 60 : 20, temperatureTolerance: 0.05, temperatureRelaxation: 0.8,
        // 新建 VoxelEB 工程默认开启 Monin–Obukhov 稳定度迭代（回归：收敛稳定、耗时 +0–10%）；
        // FacetEB 仍默认中性。旧工程缺字段按 false（中性）处理，见 normalizeProject。
        stabilityCorrection: options.mode === 'eVoxelEB',
        soilTemperatureMethod: 2,
        // 土壤方法 1 热历史：新建工程 'full'（8 个显式节点 + 指数模态远期历史）；
        // 旧工程缺字段按 'truncated'（原 8 节点截断，逐字节不变），见 normalizeProject。
        soilInertiaHistory: 'full',
        vegetationTemperatureMethod: 0
      },
      // 流体（风场/烟羽）节：默认关闭；voxelSize 不小于 0.1 m，timeStep 单位 s（待核对）。
      fluid: {
        enabled: false,
        voxelSize: Math.max(0.1, number(options.voxelSize, 1)),
        timeStep: 5,
        pressureIterations: 20,
        windDirection: 0,
        buoyancy: 0.033,
        dragCoefficient: 0.3,
        thermalCoupling: 1,
        diffusivity: 0.1,
        smokeEmission: 0.02,
        maxVelocity: 30,
        outputWind: false,
        outputAirTemperature: false,
        outputHeight: 2
      },
      objects: { count: 0, names: [], items: [] },
      spectra: presets.spectra,
      canopies: presets.canopies,
      thermals: presets.thermals,
      materials: presets.materials,
      // 气象节：path 为气象文件，start/end 为使用的时间节点范围，dTime 为时间步长（s），
      //   latitude/longitude 为站点经纬度（°），year/timezone 供太阳几何使用（气象文件按当地时间存日序）。
      meteo: {
        path: 'defined/meteo.txt', start: 1, end: 48,
        z: 15, Tsold: 300, SatWater: 0.45, dTime: 1800, Ca: 380,
        latitude: 40, longitude: 116,
        // Meteo files store day-of-year in local time; solar geometry needs both.
        year: 2019, timezone: 8
      },
      // 大气节：默认关闭；启用时按 model/waterVapor/aerosol/visibility 查 lutFile 查找表（单位待核对）。
      atmosphere: {
        enabled: false,
        model: 'midlatitude-summer',
        waterVapor: 2,
        aerosol: 'rural',
        visibility: 23,
        lutFile: 'assets/atmosphere/simple_modtran_lut.csv'
      }
    }
  }
}

// 作用：把任意版本的工程对象（含旧工程）规范化为当前 schema 的完整工程；不修改传入对象。
// 输入：value 解析后的 project.json 对象；输出：补全默认值、统一预设别名、限幅后的新对象。
// 步骤：1) 深拷贝输入并取默认工程；2) 预算传感器/光照/气象派生量；3) 规范化光谱；
//   4) 规范化对象（别名、分布密度、火/雾介质、移动参数）；5) 逐节合并默认值与输入并限幅。
// 约定：旧工程缺字段时尽量保持旧行为（缺省值与新工程不同的字段在下面逐处说明）。
export function normalizeProject(value = {}) {
  // 注意：server.mjs 也会在系统 Node 下调用本函数，而 package.json 未声明 engines；
  // structuredClone 需要 Node>=17。工程对象本身就是 JSON 可序列化数据，这里回退到 JSON 往返，
  // 避免旧版 Node 下加载工程直接抛 TypeError。
  const input =
    typeof structuredClone === 'function' ? structuredClone(value) : JSON.parse(JSON.stringify(value))
  const defaults = createDefaultProject(input)
  const { inputFile: _legacyInputFile, ...source } = input
  // 步骤 2：取出各节输入，并预先算出传感器位置、巡航参数、直射比例、气象起止节点等派生量。
  const configuration = input.configuration || {}
  const sourceSensor = configuration.sensor || {}
  const normalizedSceneX = Math.max(0.1, number(configuration.scene?.x, defaults.configuration.scene.x))
  const normalizedSceneY = Math.max(0.1, number(configuration.scene?.y, defaults.configuration.scene.y))
  const normalizedSensorX = number(sourceSensor.positionX, normalizedSceneX / 2)
  const normalizedSensorY = number(sourceSensor.positionY, normalizedSceneY / 2)
  const normalizedCruiseHeading = normalizedAzimuth(number(sourceSensor.cruiseHeading, 90))
  const normalizedCruiseStep = Math.max(0.01, number(sourceSensor.cruiseStep, 25))
  const normalizedCruiseCount = Math.max(1, Math.min(10000, Math.round(number(sourceSensor.cruiseCount, 20))))
  const fallbackCruiseSpan = normalizedCruiseStep * Math.max(1, normalizedCruiseCount - 1)
  const sourceLight = configuration.light || {}
  const directFraction = Math.max(0, Math.min(1, number(sourceLight.direct, defaults.configuration.light.direct)))
  const sourceMeteo = configuration.meteo || {}
  const meteoStart = Math.max(0, Math.round(number(sourceMeteo.start, defaults.configuration.meteo.start)))
  const meteoEnd = Math.max(meteoStart + 1, Math.round(number(sourceMeteo.end, defaults.configuration.meteo.end)))
  const energyMode = input.mode === 'eFacetEB' || input.mode === 'eVoxelEB'
  // 旧工程只有 sensor.process 一个过程输出开关；后面按模式把它拆给 radiationProcess/energyProcess。
  const legacyProcess = Boolean(sourceSensor.process)
  // 步骤 3：光谱规范化。先合并别名条目、剔除旧版内置且未使用的光伏光谱；
  //   工程没有光谱时用默认预设。
  const sourceSpectra = simplifySolarSpectra(canonicalPresetItems(Array.isArray(configuration.spectra) && configuration.spectra.length
    ? configuration.spectra : defaults.configuration.spectra
  ), configuration)
  // 逐条光谱：补全开孔率、表面纹理、镜面项、预览/物理纹理；内置的混凝土/沥青/红砖/载具缺预览纹理时补内置值；
  //   BSM 土壤光谱补全 SMC/BSMBrightness/BSMlat/BSMlon。
  const spectra = sourceSpectra.map((item) => {
    const { openness: _openness, classTexture: _classTexture, opennessTexture: _opennessTexture, ...withoutOpenness } = withoutSmokeLabel(item)
    const openness = surfaceOpenness(item)
    const surfaceTextures = normalizeSurfaceTextures(item)
    const normalizedItem = { ...(openness > 0 ? { ...withoutOpenness, openness } : withoutOpenness), ...surfaceTextures, specular: normalizeSpecular(item.specular) }
    const builtInPreviewTexture = {
      concrete: { enabled: true, preset: 'concrete', repeatSize: 2 },
      asphalt: { enabled: true, preset: 'gravel', repeatSize: 3 },
      red_brick: { enabled: true, preset: 'red-brick', repeatSize: 1.2 },
      vehicle_surface: { enabled: true, preset: 'metal', repeatSize: 1.5 }
    }[String(item.name || '')]
    const sourcePreviewTexture = item.previewTexture || builtInPreviewTexture || {}
    const previewTexture = {
      enabled: Boolean(sourcePreviewTexture.enabled),
      preset: String(sourcePreviewTexture.preset || 'concrete'),
      repeatSize: Math.max(0.05, number(sourcePreviewTexture.repeatSize, 2))
    }
    const sourceTexture = item.physicalTexture || {}
    const physicalTexture = {
      enabled: Boolean(sourceTexture.enabled),
      fileName: String(sourceTexture.fileName || ''),
      strength: Math.max(0, Math.min(1, number(sourceTexture.strength, 0.2))),
      relativeStd: Math.max(0, Math.min(0.5, number(sourceTexture.relativeStd, 0))),
      repeatSize: Math.max(0.01, number(sourceTexture.repeatSize, 2))
    }
    if (item.model !== 'BSM') return { ...normalizedItem, previewTexture, physicalTexture }
    const params = item.params || {}
    let latitude = number(params.BSMlat, 25)
    let longitude = number(params.BSMlon, 45)
    // 0/0 collapses BSM onto the very dark third basis spectrum and is not a useful soil default.
    if (Math.abs(latitude) < 1e-9 && Math.abs(longitude) < 1e-9) {
      latitude = 25
      longitude = 45
    }
    return {
      ...normalizedItem,
      previewTexture,
      physicalTexture,
      params: {
        ...params,
        SMC: number(params.SMC, 25),
        BSMBrightness: number(params.BSMBrightness, 0.5),
        BSMlat: latitude,
        BSMlon: longitude
      }
    }
  })
  const configuredFluidOutputHeight = number(
    configuration.fluid?.outputHeight,
    defaults.configuration.fluid.outputHeight
  )
  // 步骤 4：对象规范化：预设名转规范名、网格绑定补全、分布密度迁移、火/雾介质坐标迁移、移动参数。
  const sourceObjects = configuration.objects || {}
  const objectItems = (Array.isArray(sourceObjects.items) ? sourceObjects.items : defaults.configuration.objects.items)
    .filter((item) => item && typeof item === 'object' && !Array.isArray(item))
    .map((item) => {
      const normalized = {
        ...item,
        materialName: canonicalPresetName(item.materialName),
        spectralName: canonicalPresetName(item.spectralName),
        thermalName: canonicalPresetName(item.thermalName),
        meshes: Array.isArray(item.meshes) ? item.meshes.filter((mesh) => mesh && typeof mesh === 'object' && !Array.isArray(mesh)).map((mesh) => {
          const materialName = canonicalPresetName(mesh.materialName || item.materialName)
          const material = (configuration.materials || []).find((entry) => canonicalPresetName(entry.name) === materialName)
          const wood = mesh.energyModel === 'wood' || material?.energyModel === 'wood'
            || materialName === 'tree_branch_wood' || materialName === 'tree_trunk_wood'
          const { specular: _legacyMeshSpecular, ...meshBinding } = mesh
          return {
            ...meshBinding,
            specularEnabled: meshSpecular({ ...mesh, spectralName: canonicalPresetName(mesh.spectralName) }, spectra, configuration.control, canonicalPresetName(item.spectralName)).enabled,
            spectralName: canonicalPresetName(mesh.spectralName),
            thermalName: canonicalPresetName(mesh.thermalName),
            materialName,
            canopyName: mesh.canopyName || item.canopyName,
            ...(wood ? { energyModel: 'wood' } : {})
          }
        }) : item.meshes
      }
      // 分布密度迁移：统一换算为“每公顷”（densityUnit = 'hectare'）。
      //   旧工程密度按 ×10000 换算（推断为每平方米，待核对）；basis 为 count 时由总数除以分布面积；
      //   squareKilometer 单位的值除以 100。
      if (item.distribution && typeof item.distribution === 'object' && !Array.isArray(item.distribution)) {
        const densityUnit = item.distribution.densityUnit
        const legacyDensity = !['hectare', 'squareKilometer'].includes(densityUnit)
        const distributionArea = Math.abs(number(item.distribution.maxX, 0) - number(item.distribution.minX, 0))
          * Math.abs(number(item.distribution.maxY, 0) - number(item.distribution.minY, 0))
        const migratedDensity = item.distribution.basis === 'count' && distributionArea > 0
          ? number(item.distribution.count, 1) / distributionArea * 10000
          : number(item.distribution.density, 0.02) * 10000
        const perHectareDensity = densityUnit === 'squareKilometer'
          ? number(item.distribution.density, 20000) / 100
          : number(item.distribution.density, 200)
        normalized.distribution = {
          ...item.distribution,
          // DEFERRED[低] 数据一致性: 迁移结果 toFixed(2) 后再 Math.max(0.01)，稀疏分布（如 0.3 株/km² = 0.003/ha）被抬高到 0.01/ha，密度放大数倍且不可逆；未修复：toFixed(2) 精度策略属禁止修改项，需单独决策
          density: legacyDensity
            ? Math.max(0.01, Number(migratedDensity.toFixed(2)))
            : Math.max(0.01, Number(perHectareDensity.toFixed(2))),
          densityUnit: 'hectare'
        }
      }
      // 火/雾介质：统一为 XYZ 坐标次序（coordinateOrder）并标记仅参与辐射；
      //   旧工程不是 XYZ 次序时交换 Y/Z 轴（尺寸、生成参数 sizeY/Z、positionY/Z）。
      if (item.type === 'Fire' || item.type === 'Fog') {
        const fog = item.type === 'Fog' || item.medium?.kind === 'fog'
        const standardAxes = item.medium?.coordinateOrder === 'XYZ'
        const dimensions = Array.isArray(item.dimensions) ? [...item.dimensions] : [1, 1, 1]
        const parameters = { ...(item.generation?.parameters || {}) }
        if (!standardAxes) {
          ;[dimensions[1], dimensions[2]] = [dimensions[2], dimensions[1]]
          ;[parameters.sizeY, parameters.sizeZ] = [parameters.sizeZ, parameters.sizeY]
          ;[parameters.positionY, parameters.positionZ] = [parameters.positionZ, parameters.positionY]
        }
        normalized.dimensions = dimensions
        normalized.medium = { ...(item.medium || {}), kind: fog ? 'fog' : 'fire', radiationOnly: true, coordinateOrder: 'XYZ' }
        normalized.generation = { ...(item.generation || {}), type: fog ? '雾' : '火焰', parameters }
      }
      // 可移动类型（人/车/船）补全 movement 参数，其余类型删除 movement。
      if (isMobileAgentType(item.type)) normalized.movement = normalizeMovement(item)
      else delete normalized.movement
      return normalized
    })
  // 步骤 5：以默认工程为底，依次覆盖 input 的顶层字段与各配置节，逐节规范化并限幅。
  // DEFERRED[低] 兼容性: normalizeProject/parseProjectJson 不检查 value.schemaVersion（只有 validateProject 检查），高于 PROJECT_SCHEMA_VERSION 的工程被直接改写为 6 并在保存时降级（未知字段虽经 ...source 保留，但新语义可能被这里的迁移规则误改）；未修复：拒绝/只读打开更高版本涉及 schema 兼容策略，按要求保留待决策
  return {
    ...defaults,
    ...source,
    format: PROJECT_FORMAT,
    schemaVersion: PROJECT_SCHEMA_VERSION,
    mode: PROJECT_MODES.includes(input.mode) ? input.mode : defaults.mode,
    folders: { ...defaults.folders, ...(input.folders || {}) },
    configuration: {
      ...defaults.configuration,
      ...configuration,
      // 场景节：展开输入后规范化偏移、terrainEnvironment 与 background（预设名转规范名，镜面开关由 meshSpecular 判定）。
      scene: {
        ...defaults.configuration.scene,
        // DEFERRED[中] 数据一致性: scene.x/y/height/voxel/voxelFillThreshold 原样展开，不做 number()/下限校验；上方算出的 normalizedSceneX/Y 只用于传感器，场景本身仍可能是 0、负数、NaN 或字符串并写回 project.json 交给引擎（light.zenith/azimuth、sensor.x/y/vza/vaa、control.depth/samples 同样未校验）；未修复：各字段的合法范围/取整规则属产品与引擎语义，未定义统一规则前不擅自夹紧
        ...(configuration.scene || {}),
        terrainEnvironment: normalizeTerrainEnvironment(configuration.scene?.terrainEnvironment),
        offsetX: number(configuration.scene?.offsetX, defaults.configuration.scene.offsetX),
        offsetY: number(configuration.scene?.offsetY, defaults.configuration.scene.offsetY),
        offsetZ: number(configuration.scene?.offsetZ, defaults.configuration.scene.offsetZ),
        background: {
          ...defaults.configuration.scene.background,
          ...(configuration.scene?.background || {}),
          spectralName: canonicalPresetName(configuration.scene?.background?.spectralName || defaults.configuration.scene.background.spectralName),
          specularEnabled: meshSpecular({
            ...(configuration.scene?.background || {}),
            spectralName: canonicalPresetName(configuration.scene?.background?.spectralName || defaults.configuration.scene.background.spectralName)
          }, spectra, configuration.control).enabled,
          thermalName: canonicalPresetName(configuration.scene?.background?.thermalName || defaults.configuration.scene.background.thermalName),
          materialName: canonicalPresetName(configuration.scene?.background?.materialName || defaults.configuration.scene.background.materialName),
          heterogeneityEnabled: Boolean(configuration.scene?.background?.heterogeneityEnabled),
          angularEffectStrength: Math.max(0, Math.min(1, number(
            configuration.scene?.background?.angularEffectStrength,
            defaults.configuration.scene.background.angularEffectStrength
          )))
        }
      },
      // shortwaveIrradiance: horizontal global shortwave (W m-2) scaling the
      // 2.5-6 um solar term of RayTracing/VoxelRT/FacetRT; 0 = model default.
      light: { ...defaults.configuration.light, ...sourceLight, direct: directFraction, diffuse: 1 - directFraction,
        shortwaveIrradiance: Math.max(0, Math.min(2000, number(sourceLight.shortwaveIrradiance, 0))),
        diffuseFractionMode: sourceLight.diffuseFractionMode === 'erbs' ? 'erbs' : 'fixed' },
      // 传感器节：缺少巡航目标时按航向与步长×(点数-1) 推算；矩形巡航宽高缺省为场景尺寸的 1/4（≤500 m）；
      //   视场角限制在 0.1–120°；radiationProcess/energyProcess 缺字段时由旧 process 开关按模式推断。
      sensor: {
        ...defaults.configuration.sensor,
        ...sourceSensor,
        positionX: normalizedSensorX,
        positionY: normalizedSensorY,
        continuousBands: Boolean(sourceSensor.continuousBands),
        bandStart: number(sourceSensor.bandStart, 400),
        bandEnd: number(sourceSensor.bandEnd, 2500),
        bandStep: Math.max(0.000001, number(sourceSensor.bandStep, 10)),
        principalPlane: Boolean(sourceSensor.principalPlane),
        hemisphere: Boolean(sourceSensor.hemisphere),
        fov: Math.max(0.1, Math.min(120, number(sourceSensor.fov, 60))),
        cruiseEnabled: Boolean(sourceSensor.cruiseEnabled),
        cruiseHeading: normalizedCruiseHeading,
        cruiseRoute: ['line', 'rectangle', 'random'].includes(sourceSensor.cruiseRoute) ? sourceSensor.cruiseRoute : 'line',
        cruiseTargetX: number(sourceSensor.cruiseTargetX, normalizedSensorX + Math.cos(normalizedCruiseHeading * Math.PI / 180) * fallbackCruiseSpan),
        cruiseTargetY: number(sourceSensor.cruiseTargetY, normalizedSensorY + Math.sin(normalizedCruiseHeading * Math.PI / 180) * fallbackCruiseSpan),
        cruiseTargetZ: Math.max(0.01, number(sourceSensor.cruiseTargetZ, Math.max(0.01, number(sourceSensor.height, 3000)))),
        cruiseRectangleWidth: Math.max(normalizedCruiseStep, number(sourceSensor.cruiseRectangleWidth, Math.min(normalizedSceneX * 0.25, 500))),
        cruiseRectangleHeight: Math.max(normalizedCruiseStep, number(sourceSensor.cruiseRectangleHeight, Math.min(normalizedSceneY * 0.25, 500))),
        cruiseRandomSeed: Math.max(1, Math.round(number(sourceSensor.cruiseRandomSeed, 1))),
        cruiseStep: normalizedCruiseStep,
        cruiseCount: normalizedCruiseCount,
        radiationProcess: sourceSensor.radiationProcess ?? (!energyMode && legacyProcess),
        energyProcess: sourceSensor.energyProcess ?? (energyMode && legacyProcess)
      },
      // 运行控制节：默认值 + 输入，再逐项取整限幅；缺字段的旧工程沿用旧值
      //   （碰撞阶 2 / 散射阶 1 / 步数 64），并记为 custom 档位，不改动已有参数。
      control: {
        ...defaults.configuration.control,
        ...(configuration.control || {}),
        heterogeneousVoxel: Boolean(configuration.control?.heterogeneousVoxel),
        // 旧工程没有档位记录：视为自定义，不改动其已有参数。
        qualityPreset: QUALITY_PRESETS.includes(configuration.control?.qualityPreset)
          ? configuration.control.qualityPreset : 'custom',
        runTimeoutMinutes: normalizeRunTimeoutMinutes(configuration.control?.runTimeoutMinutes),
        // RayTracing scattering-order budget, separate from "depth" (which
        // VoxelRT uses as its DDA step budget). Russian roulette from bounce 3.
        raytracingDepth: Math.max(1, Math.min(64, Math.round(number(
          configuration.control?.raytracingDepth, 16
        )))),
        // Legacy projects used a separate switch plus periodicNeighborCount.
        // New projects use one 0..20 value, where zero means disabled.
        periodicBoundary: undefined,
        periodicNeighborCount: undefined,
        periodicTraversalCount: Math.max(0, Math.min(20, Math.round(number(
          configuration.control?.periodicTraversalCount
            ?? (configuration.control?.periodicBoundary
              ? configuration.control?.periodicNeighborCount : 0),
          0
        )))),
        skyboxEnabled: Boolean(configuration.control?.skyboxEnabled),
        facetBackend: ['cpu', 'auto'].includes(configuration.control?.facetBackend)
          ? configuration.control.facetBackend : 'gpu',
        computeBackend: ['cpu', 'auto'].includes(configuration.control?.computeBackend)
          ? configuration.control.computeBackend : 'gpu',
        cpuThreads: normalizeCpuThreads(configuration.control?.cpuThreads),
        radiationSolver: configuration.control?.radiationSolver === 'traditional'
          ? 'traditional' : 'accelerated',
        spectralAccelerationWidth: Math.max(1, Math.min(1000, number(
          configuration.control?.spectralAccelerationWidth, 100
        ))),
        shortwaveScatteringOrders: Math.max(1, Math.min(3, Math.round(number(
          configuration.control?.shortwaveScatteringOrders, 1
        )))),
        voxelCollisionOrders: Math.max(2, Math.min(5, Math.round(number(
          configuration.control?.voxelCollisionOrders, 2
        )))),
        radiationMaxSteps: Math.max(1, Math.min(4096, Math.round(number(
          configuration.control?.radiationMaxSteps, 64
        )))),
        stabilityCorrection: configuration.control?.stabilityCorrection === true,
        soilTemperatureMethod: Math.max(0, Math.min(2, Math.round(number(
          configuration.control?.soilTemperatureMethod,
          configuration.materials?.find((item) => item.type === 'Soil')?.params?.method ?? 2
        )))),
        soilInertiaHistory: configuration.control?.soilInertiaHistory === 'full' ? 'full' : 'truncated',
        vegetationTemperatureMethod: Math.max(0, Math.min(1, Math.round(number(
          configuration.control?.vegetationTemperatureMethod, 0
        ))))
      },
      // 流体节：voxelSize 缺省沿用场景体元；timeStep ≥ 0.1、pressureIterations 取 2–100；
      //   windDirection 归一到 [0, 360)；其余物理系数下限为 0；outputHeight 必须 > 0，否则回退默认值。
      fluid: {
        ...defaults.configuration.fluid,
        ...(configuration.fluid || {}),
        enabled: Boolean(configuration.fluid?.enabled),
        voxelSize: Math.max(0.1, number(
          configuration.fluid?.voxelSize,
          number(configuration.scene?.voxel, defaults.configuration.scene.voxel)
        )),
        timeStep: Math.max(0.1, number(configuration.fluid?.timeStep, defaults.configuration.fluid.timeStep)),
        pressureIterations: Math.max(2, Math.min(100, Math.round(number(configuration.fluid?.pressureIterations, defaults.configuration.fluid.pressureIterations)))),
        windDirection: ((number(configuration.fluid?.windDirection, defaults.configuration.fluid.windDirection) % 360) + 360) % 360,
        buoyancy: Math.max(0, number(configuration.fluid?.buoyancy, defaults.configuration.fluid.buoyancy)),
        dragCoefficient: Math.max(0, number(configuration.fluid?.dragCoefficient, defaults.configuration.fluid.dragCoefficient)),
        thermalCoupling: Math.max(0, number(configuration.fluid?.thermalCoupling, defaults.configuration.fluid.thermalCoupling)),
        diffusivity: Math.max(0, number(configuration.fluid?.diffusivity, defaults.configuration.fluid.diffusivity)),
        smokeEmission: Math.max(0, number(configuration.fluid?.smokeEmission, defaults.configuration.fluid.smokeEmission)),
        maxVelocity: Math.max(0.1, number(configuration.fluid?.maxVelocity, defaults.configuration.fluid.maxVelocity)),
        outputWind: Boolean(configuration.fluid?.outputWind),
        outputAirTemperature: Boolean(configuration.fluid?.outputAirTemperature),
        // Earlier builds clamped this value to 0 for flat scenes.
        outputHeight: configuredFluidOutputHeight > 0
          ? configuredFluidOutputHeight
          : defaults.configuration.fluid.outputHeight
      },
      // 大气节：model 仅允许 tropical/midlatitude-summer/midlatitude-winter，aerosol 仅 rural/urban；
      //   waterVapor 限幅 0.5–5，visibility 限幅 10–50（单位待核对）。
      atmosphere: {
        ...defaults.configuration.atmosphere,
        ...(configuration.atmosphere || {}),
        enabled: Boolean(configuration.atmosphere?.enabled),
        model: ['tropical', 'midlatitude-summer', 'midlatitude-winter'].includes(configuration.atmosphere?.model)
          ? configuration.atmosphere.model : defaults.configuration.atmosphere.model,
        waterVapor: Math.max(0.5, Math.min(5, number(
          configuration.atmosphere?.waterVapor,
          defaults.configuration.atmosphere.waterVapor
        ))),
        aerosol: ['rural', 'urban'].includes(configuration.atmosphere?.aerosol)
          ? configuration.atmosphere.aerosol : defaults.configuration.atmosphere.aerosol,
        visibility: Math.max(10, Math.min(50, number(
          configuration.atmosphere?.visibility,
          defaults.configuration.atmosphere.visibility
        )))
      },
      // 对象节：用规范化后的 objectItems 覆盖 items，并同步 count 与 names。
      objects: {
        ...defaults.configuration.objects, ...sourceObjects, items: objectItems,
        count: objectItems.length,
        names: objectItems.map((item) => item.name)
      },
      spectra,
      // 冠层预设：structureType 兼容数字写法（1=rigid，2=fire，3=fog，其余为 canopy）；
      //   火焰/雾的消光、散射反照率、不对称因子、发射比例与固定温度按类型取缺省并限幅。
      canopies: (Array.isArray(configuration.canopies) && configuration.canopies.length ? configuration.canopies : defaults.configuration.canopies)
        .map((item) => {
          const rawType = String(item.structureType ?? 'canopy').toLowerCase()
          const structureType = rawType === '1' || rawType === 'rigid' ? 'rigid'
            : rawType === '2' || rawType === 'fire' ? 'fire'
              : rawType === '3' || rawType === 'fog' ? 'fog' : 'canopy'
          return {
            ...withoutSmokeLabel(item), structureType,
            extinction: Math.max(0, number(item.extinction, structureType === 'fire' ? 1.2 : structureType === 'fog' ? 0.08 : 0)),
            scatteringAlbedo: Math.max(0, Math.min(1, number(item.scatteringAlbedo, structureType === 'fog' ? 0.95 : structureType === 'fire' ? 0.72 : 0))),
            asymmetry: Math.max(-0.99, Math.min(0.99, number(item.asymmetry, structureType === 'fog' ? 0.85 : structureType === 'fire' ? 0.55 : 0))),
            emissionScale: Math.max(0, number(item.emissionScale, structureType === 'fire' || structureType === 'fog' ? 1 : 0)),
            fixedTemperature: Math.max(0, number(item.fixedTemperature, structureType === 'fire' ? 1100 : structureType === 'fog' ? 288 : 0))
          }
        }),
      // 热预设与物化预设：统一别名（canonicalPresetItems）；材质另经 normalizeMaterialItem 补全木质参数。
      thermals: canonicalPresetItems(Array.isArray(configuration.thermals) && configuration.thermals.length ? configuration.thermals : defaults.configuration.thermals)
        .map(withoutSmokeLabel),
      materials: canonicalPresetItems(Array.isArray(configuration.materials) && configuration.materials.length ? configuration.materials : defaults.configuration.materials).map(normalizeMaterialItem),
      // 气象节：start/end 取整且 end > start；Tsold 初始土温（K，限幅 150–400）；SatWater 0–1；
      //   dTime 为时间步长（s，≥ 1）；Ca 为大气 CO2 浓度（限幅 100–2000，单位待核对）；
      //   latitude/longitude 为 ° ；timezone 为小时。
      meteo: {
        ...defaults.configuration.meteo,
        ...sourceMeteo,
        start: meteoStart,
        end: meteoEnd,
        z: Math.max(0, number(sourceMeteo.z, defaults.configuration.meteo.z)),
        // DEFERRED[中] 数据一致性: Tsold 以 K 计，但材质 params.Tsoil 用 ℃（25），用户按 ℃ 填写 27 时被静默夹到 150 K，初始土温差约 150 K 且无提示；未修复：℃/K 单位约定与温度区间属禁止修改项，需产品确认
        Tsold: Math.max(150, Math.min(400, number(sourceMeteo.Tsold, defaults.configuration.meteo.Tsold))),
        SatWater: Math.max(0, Math.min(1, number(sourceMeteo.SatWater, defaults.configuration.meteo.SatWater))),
        dTime: Math.max(1, number(sourceMeteo.dTime, defaults.configuration.meteo.dTime)),
        Ca: Math.max(100, Math.min(2000, number(sourceMeteo.Ca, defaults.configuration.meteo.Ca))),
        latitude: Math.max(-90, Math.min(90, number(sourceMeteo.latitude, defaults.configuration.meteo.latitude))),
        longitude: Math.max(-180, Math.min(180, number(sourceMeteo.longitude, defaults.configuration.meteo.longitude))),
        year: Math.max(-2000, Math.min(6000, Math.round(number(sourceMeteo.year, defaults.configuration.meteo.year)))),
        timezone: Math.max(-12, Math.min(14, number(sourceMeteo.timezone, defaults.configuration.meteo.timezone)))
      }
    }
  }
}

// 作用：解析 project.json 文本并规范化；非 JSON、非对象根节点、非本格式的 format 会抛错。
export function parseProjectJson(content) {
  let value
  try {
    value = JSON.parse(String(content || ''))
  } catch (error) {
    throw new Error(`project.json 格式错误：${error.message}`)
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('project.json 根节点必须是对象')
  if (value.format && !isProjectFormat(value.format)) throw new Error(`不支持的工程格式：${value.format}`)
  return normalizeProject(value)
}

// 作用：规范化后序列化为 2 空格缩进、末尾换行的 JSON 文本（写回 project.json）。
export function stringifyProject(value) {
  return `${JSON.stringify(normalizeProject(value), null, 2)}\n`
}

// 作用：校验工程并返回 { valid, errors, project }，project 为规范化后的工程。
// 步骤：1) 根节点/format/schemaVersion/mode 检查；2) 规范化后检查场景、传感器、地形环境；
//   3) 光伏与薄膜材质的模式限制；4) 墙体、车辆、船舶、水体材质参数范围；5) 光谱的开孔率与纹理。
// 说明：只收集错误文本，不抛异常；各数值范围来自引擎能稳定求解的区间（待核对）。
export function validateProject(value) {
  const errors = []
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return { valid: false, errors: ['project.json 根节点必须是对象'], project: normalizeProject() }
  }
  if (value.format && !isProjectFormat(value.format)) errors.push(`format 必须是 ${PROJECT_FORMAT}`)
  // 步骤 1：schemaVersion 缺省视为 1；高于当前版本或非整数则报错。
  const sourceVersion = Number(value.schemaVersion ?? 1)
  if (!Number.isInteger(sourceVersion) || sourceVersion < 1 || sourceVersion > PROJECT_SCHEMA_VERSION) {
    errors.push(`schemaVersion 仅支持 1-${PROJECT_SCHEMA_VERSION}`)
  }
  if (!PROJECT_MODES.includes(value.mode)) errors.push('运行模式无效')
  if (!value.configuration || typeof value.configuration !== 'object' || Array.isArray(value.configuration)) {
    errors.push('configuration 必须是对象')
  }
  // 步骤 2：以下检查都针对规范化后的工程 c = project.configuration。
  const project = normalizeProject(value)
  const c = project.configuration
  if (!(c.scene.x > 0) || !(c.scene.y > 0) || !(c.scene.height >= 0)) errors.push('场景尺寸无效')
  if (![c.scene.offsetX, c.scene.offsetY, c.scene.offsetZ].every(Number.isFinite)) errors.push('场景 XYZ 偏移必须为有效数值')
  if (!(c.scene.voxel > 0)) errors.push('体素尺寸必须大于 0')
  // 地形水热经验场：仅 VoxelEB 支持；尺度高度必须 > 0，含水量满足 0 ≤ soilDry ≤ soilValley ≤ 1。
  const environment = c.scene.terrainEnvironment
  if ((environment.airEnabled || environment.soilEnabled) && project.mode !== 'eVoxelEB')
    errors.push('地形水热经验场仅支持 VoxelEB')
  if (!(environment.vaporScaleHeight > 0) || !(environment.soilScaleHeight > 0) ||
      environment.soilDry < 0 || environment.soilValley < environment.soilDry || environment.soilValley > 1)
    errors.push('地形水热参数要求尺度高度大于 0，且 0 ≤ 干端含水量 ≤ 沟谷含水量 ≤ 1')
  if (!(Number(c.sensor.x) > 0) || !(Number(c.sensor.y) > 0)) errors.push('传感器分辨率无效')
  if (!sensorBandValues(c.sensor).length) errors.push('至少需要一个有效波段')
  if (!sensorViewAngles(c.sensor, c.light.azimuth).length) errors.push('至少需要一个有效观测角')
  if (!Array.isArray(c.objects.items)) errors.push('objects.items 必须是数组')
  // 步骤 3：光伏材质检查（参数范围；不能作背景地面；使用光伏时需 eFacetEB，VoxelEB 不支持热电耦合）。
  const pv = c.materials.filter(m => m.energyModel === 'photovoltaic')
  for (const m of pv) {
    const p = m.params || {}
    if (![p.eta25,p.gamma,p.bifaciality,p.heatCapacityPerArea,p.convectiveScale].every(Number.isFinite) || !(p.eta25>=0 && p.eta25<=0.5 && p.gamma>=-0.02 && p.gamma<=0 && p.bifaciality>=0 && p.bifaciality<=1 && p.heatCapacityPerArea>=0 && p.convectiveScale>0)) errors.push('光伏参数无效：'+m.name)
  }
  const names = new Set(pv.map(m=>m.name))
  if (names.has(c.scene.background.materialName)) errors.push('光伏板请作为场景对象添加，背景地面不支持光伏材质')
  const used = c.objects.items.some(o => (o.meshes?.length ? o.meshes : [o]).some(m=>names.has(m.materialName || o.materialName)))
  if (used && project.mode === 'eVoxelEB') errors.push('光伏热电耦合请选择 eFacetEB 模式')
  // 薄膜（篷布/遮阳网）材质检查：面热容与对流倍率 > 0；不能作背景地面；热平衡需 eFacetEB。
  const membranes = c.materials.filter(m => m.energyModel === 'membrane')
  for (const m of membranes) {
    const p = m.params || {}
    if (!(Number(p.heatCapacityPerArea) > 0) || !(Number(p.convectiveScale) > 0)) errors.push('薄膜参数无效（面热容与对流倍率必须大于 0）：' + m.name)
  }
  const membraneNames = new Set(membranes.map(m => m.name))
  if (membraneNames.has(c.scene.background.materialName)) errors.push('篷布/薄膜请作为场景对象添加，背景地面不支持薄膜材质')
  const membraneUsed = c.objects.items.some(o => (o.meshes?.length ? o.meshes : [o]).some(m => membraneNames.has(m.materialName || o.materialName)))
  // VoxelEB treats every non-leaf solid as semi-infinite, so a thin sheet would be wrong there.
  if (membraneUsed && project.mode === 'eVoxelEB') errors.push('篷布/薄膜热平衡请选择 eFacetEB 模式')
  // 步骤 4：多层墙体（wallMode = 'wall'）检查：厚度 ≤ 5 m、室内温度/换热系数合法，
  //   layers 为 1–6 层且总厚度 ≤ 5 m、每层 λ/ρ/cp 大于 0。
  for (const m of c.materials.filter((item) => item.params?.wallMode === 'wall' && !item.energyModel && item.type !== 'Water' && item.type !== 'Vegetation')) {
    const p = m.params
    const thickness = Number(p.thickness ?? 0.2), indoor = Number(p.indoorTemperature ?? 22), h = Number(p.indoorHeatTransferCoefficient ?? 8)
    if (!(thickness > 0 && thickness <= 5) || !(indoor > -70 && indoor < 120) || !(h > 0) || !(Number(p.lambdas ?? 1.4) > 0) || !(Number(p.rhos ?? 2300) > 0) || !(Number(p.cs ?? 880) > 0))
      errors.push('墙体模式参数无效（0 < 厚度 ≤ 5 m，室内换热系数、导热率、密度、比热容大于 0）：' + m.name)
    if (p.layers !== undefined) {
      const layers = Array.isArray(p.layers) ? p.layers : []
      const total = layers.reduce((sum, layer) => sum + Number(layer?.thickness), 0)
      if (!layers.length || layers.length > 6 || !(total <= 5) || layers.some((layer) => !(Number(layer?.thickness) > 0) || !(Number(layer?.lambda) > 0) || !(Number(layer?.rho) > 0) || !(Number(layer?.cp) > 0)))
        errors.push('墙体构造层无效（1–6 层，每层厚度、λ、ρ、cp 大于 0，总厚度 ≤ 5 m）：' + m.name)
    }
  }
  // 车辆材质检查：vehiclePart 取值，body 材质的座舱（cabin）、内饰（interior）、发动机（engine）参数范围。
  for (const m of c.materials.filter((item) => item.type === 'Vehicle' && !item.energyModel)) {
    const p = m.params || {}
    if (p.vehiclePart !== undefined && p.vehiclePart !== '' && !['body', 'glass', 'underbody', 'hood'].includes(p.vehiclePart))
      errors.push('载具部件应为 body、glass、underbody 或 hood：' + m.name)
    const k = p.cabin
    if (k && p.vehiclePart === 'body') {
      // inRange：值缺省（undefined/null/空串）时用 fallback，再判断是否为有限数且在 [lo, hi] 内。
      const inRange = (value, lo, hi, fallback) => { const v = value === undefined || value === null || value === '' ? fallback : Number(value); return Number.isFinite(v) && v >= lo && v <= hi }
      // 内饰四个节点（仪表台/座椅/方向盘/顶棚车门）的受光份额之和必须为 1，热容、hA、辐射面积需在范围内。
      if (k.interior?.enabled) {
        const it = k.interior, nodes = ['dashboard', 'seat', 'wheel', 'lining'].map((id) => it[id] || {})
        const shares = nodes.reduce((sum, node) => sum + (Number(node.share) || 0), 0)
        if (!inRange(it.shellRadFraction, 0, 0.95, 0.45) || !inRange(it.seatEffusivity, 1, 1e4, 300) || Math.abs(shares - 1) > 1e-3 ||
            nodes.some((node) => !inRange(node.share, 0, 1, 0.25) || !inRange(node.capacity, 10, 1e8, 10000) || !inRange(node.hcA, 0.01, 1e4, 4) || !inRange(node.radArea, 0, 1e3, 1)))
          errors.push('车辆内饰节点参数无效（份额之和 = 1，热容 ≥ 10 J/K，hA > 0）：' + m.name)
      }
      // 发动机：散热器/机体/排气三份额之和必须为 1；热容、换热与舱通风系数需在范围内。
      if (k.engine?.enabled) {
        const en = k.engine
        const split = (Number(en.radiatorShare ?? 0.5)) + (Number(en.blockShare ?? 0.3)) + (Number(en.exhaustShare ?? 0.2))
        if (!inRange(en.warmTemperature, -20, 200, 90) || !inRange(en.blockCapacity, 100, 1e9, 70000) || !inRange(en.bayCapacity, 10, 1e9, 15000) ||
            !inRange(en.blockToBay, 0.01, 1e5, 40) || !inRange(en.bayVent0, 0, 100, 0.04) || !inRange(en.bayVentK, 0, 100, 0.004) ||
            !inRange(en.firewallG, 0, 1000, 2) || Math.abs(split - 1) > 1e-3 || !(Number(en.bayVent0 ?? 0.04) + Number(en.bayVentK ?? 0.004) > 0))
          errors.push('车辆发动机/发动机舱参数无效（散热器+机体+排气份额之和 = 1，热容与换热 > 0）：' + m.name)
      }
      if (!inRange(k.occupants, 0, 8, 0) || !inRange(k.speed, 0, 100, 0) || !inRange(k.metabolicW, 0, 1000, 100) || !inRange(k.volume, 0.3, 100, 2.8) ||
          !inRange(k.interiorCapacity, 100, 1e8, 80000) || !inRange(k.interiorArea, 0.01, 1000, 8) || !inRange(k.interiorH, 0.01, 1000, 8) ||
          !inRange(k.absorbedFraction, 0, 1, 0.8) || !inRange(k.ventilationACH, 0, 100, 0.5) || !inRange(k.crackedACH, 0, 200, 10) ||
          !inRange(k.acPower, 0, 2e5, 3500) || !inRange(k.acSetpoint, -20, 50, 24) || !inRange(k.enginePower, 0, 1e6, 15000) || !inRange(k.engineShare, 0, 1, 0.3))
        errors.push('车辆座舱参数超出范围（乘员 0–8、车速 0–100 m/s、吸收份额 0–1 等）：' + m.name)
      const given = (value) => value !== undefined && value !== null && value !== ''
      if (given(k.heading) !== given(k.windDirection)) errors.push('车辆航向与风向需同时给出：' + m.name)
    }
  }
  // 船舶材质检查：航速、水线（高度或水面吃水）、水侧换热系数与水温、航向/风向（罗经方位）、内热源功率。
  for (const m of c.materials.filter((item) => item.type === 'Ship' && !item.energyModel)) {
    const p = m.params || {}
    const given = (value) => value !== undefined && value !== null && value !== ''
    if (given(p.speed) && !(Number(p.speed) >= 0 && Number(p.speed) <= 100)) errors.push('船舶航速无效（0–100 m/s）：' + m.name)
    if (given(p.waterlineHeight) && (!Number.isFinite(Number(p.waterlineHeight)) || !(Number(p.waterHeatTransferCoefficient ?? 500) > 0) ||
      (given(p.waterTemperature) && !(Number(p.waterTemperature) > -20 && Number(p.waterTemperature) < 60))))
      errors.push('船舶水线参数无效（水线高度为有限数，水侧换热系数大于 0，水温 -20–60 ℃）：' + m.name)
    // Real draft: waterline = the scene water surface (+ offset); wetted fraction per facet.
    const fromWater = !given(p.waterlineHeight) && p.waterlineSource === 'water'
    if (given(p.waterlineSource) && !['height', 'water'].includes(p.waterlineSource)) errors.push('船舶水线来源无效（height / water）：' + m.name)
    if (fromWater && (!Number.isFinite(Number(p.waterlineOffset ?? 0)) || !(Number(p.waterHeatTransferCoefficient ?? 500) > 0) ||
      (given(p.waterTemperature) && !(Number(p.waterTemperature) > -20 && Number(p.waterTemperature) < 60))))
      errors.push('船舶吃水（水面）参数无效（水线偏移为有限数，水侧换热系数大于 0，水温 -20–60 ℃）：' + m.name)
    if ((p.waterShielding || p.waterFeedback) && !given(p.waterlineHeight) && !fromWater) errors.push('船舶水体遮蔽/水温反馈需要水线（水线高度或水面吃水）：' + m.name)
    if (p.waterFeedback && given(p.waterTemperature)) errors.push('船舶水温反馈使用场景水体温度，不能同时给定固定水温：' + m.name)
    if (given(p.heading) !== given(p.windDirection)) errors.push('船舶航向与风向需同时给出：' + m.name)
    if (given(p.heading) && !(Number(p.heading) >= 0 && Number(p.heading) <= 360 && Number(p.windDirection) >= 0 && Number(p.windDirection) <= 360))
      errors.push('船舶航向/风向无效（0–360°，航向与风来向均为罗经方位：北 0°，顺时针）：' + m.name)
    if (given(p.internalHeatFlux) && !(Math.abs(Number(p.internalHeatFlux)) <= 1e5)) errors.push('船舶内热源面功率无效（|q| ≤ 1e5 W/m²）：' + m.name)
    if (given(p.internalHeatPower) && !(Math.abs(Number(p.internalHeatPower)) <= 1e9)) errors.push('船舶内热源总功率无效（|P| ≤ 1e9 W）：' + m.name)
  }
  // 水体材质检查：初始水温模式只能是 firstAir/dailyMeanAir/fixed，fixed 时水温需在 -50–100 ℃ 内。
  for (const m of c.materials.filter((item) => item.type === 'Water' && item.params?.initialTemperatureMode !== undefined)) {
    const p = m.params
    if (!['firstAir', 'dailyMeanAir', 'fixed'].includes(p.initialTemperatureMode) || (p.initialTemperatureMode === 'fixed' && !(Number(p.initialTemperature ?? 20) > -50 && Number(p.initialTemperature ?? 20) < 100)))
      errors.push('初始水温设置无效（首节点气温 / 起始 24 h 平均气温 / -50–100 ℃ 指定值）：' + m.name)
  }
  // 步骤 5：光谱检查：开孔率与纹理仅支持 custom/TXT 光谱；分类纹理需 PNG、类别光谱存在且不能嵌套分类纹理。
  for (const s of c.spectra) {
    if (surfaceOpenness(s) > 0 && (s.model === 'Prospect' || s.model === 'BSM')) errors.push('开孔率仅支持自定义或导入 TXT 光谱：' + s.name)
    const classTexture = s.classTexture
    if (classTexture?.enabled) {
      if (!/\.png$/i.test(classTexture.fileName)) errors.push('分类纹理需为 PNG 图像：' + s.name)
      if (!classTexture.classes.some((entry) => entry.spectralName)) errors.push('分类纹理至少需要一个类别光谱：' + s.name)
      for (const entry of classTexture.classes) {
        if (!entry.spectralName) continue
        const target = c.spectra.find((candidate) => candidate.name === entry.spectralName)
        if (!target) errors.push(`分类纹理引用了不存在的光谱：${s.name} → ${entry.spectralName}`)
        else if (target.classTexture?.enabled) errors.push(`分类纹理的类别光谱不能再启用分类纹理：${s.name} → ${entry.spectralName}`)
      }
    }
    const opennessTexture = s.opennessTexture
    if (opennessTexture?.enabled) {
      if (!/\.png$/i.test(opennessTexture.fileName)) errors.push('开孔率纹理需为 PNG 图像：' + s.name)
      if (s.model === 'Prospect' || s.model === 'BSM') errors.push('开孔率纹理仅支持自定义或导入 TXT 光谱：' + s.name)
    }
  }
  return { valid: errors.length === 0, errors, project }
}

// XML 文本转义（& < > " '）；null/undefined 视为空串。
const xmlEscape = (value) => String(value ?? '')
  .replaceAll('&', '&amp;')
  .replaceAll('<', '&lt;')
  .replaceAll('>', '&gt;')
  .replaceAll('"', '&quot;')
  .replaceAll("'", '&apos;')

// 作用：生成场景 XML 中一个 <object> 节点（对象的网格、光谱、热、冠层、物化名称以及模型/位置文件）。
// 输入：item 工程对象，index 序号（无名称时生成 object_n），resolvePath 把工程内相对路径转为运行用路径。
// 说明：有 meshes 时按网格逐项用逗号拼接名称；未指定的光谱/热/材质/冠层名取缺省预设。
function objectXml(item, index, resolvePath = (value) => value) {
  const rawName = item.name || `object_${index + 1}`
  const name = xmlEscape(rawName)
  const dimensions = Array.isArray(item.dimensions) && item.dimensions.length >= 3 ? item.dimensions.join(',') : '1,1,1'
  const meshes = Array.isArray(item.meshes) && item.meshes.length ? item.meshes : []
  const meshNames = meshes.length ? meshes.map((mesh) => mesh.name).join(',') : (item.meshNames || rawName)
  const spectralNames = meshes.length ? meshes.map((mesh) => mesh.spectralName).join(',') : (item.spectralName || 'soil')
  const thermalNames = meshes.length ? meshes.map((mesh) => mesh.thermalName).join(',') : (item.thermalName || 'soil_temperature')
  // 引擎侧对象类型：火/雾等参与介质写成 Vegetation；人/车/船写成 Building；其余沿用 item.type。
  const mediumObject = item.type === 'Fire' || item.type === 'Fog' || Boolean(item.medium)
  const engineType = mediumObject ? 'Vegetation' : isMobileAgentType(item.type) ? 'Building' : (item.type || 'Vegetation')
  const defaultMaterial = engineType === 'Vegetation' ? 'leaf_c3' : engineType === 'Water' ? 'water_set' : 'soilset'
  const materialName = item.materialName || item.bioName || defaultMaterial
  const canopyName = item.canopyName || (item.type === 'Fire' ? 'fire_medium' : item.type === 'Fog' ? 'fog_medium' : engineType === 'Vegetation' ? 'canopy_default' : 'rigid_body')
  const materialNames = meshes.length ? meshes.map((mesh) => mesh.materialName || materialName).join(',') : materialName
  const canopyNames = meshes.length ? meshes.map((mesh) => mesh.canopyName || canopyName).join(',') : canopyName
  const resolvedFileName = item.fileName ? resolvePath(item.fileName) : ''
  const resolvedPosition = item.positionFile ? resolvePath(item.positionFile) : ''
  // Participating media use HiStream's filled primitive voxelization.  The OBJ
  // remains in project.json only as a Three.js preview mesh.
  const fileName = resolvedFileName && !mediumObject ? `\n        <fileName>${xmlEscape(resolvedFileName)}</fileName>\n        <objectfile>${xmlEscape(resolvedFileName)}</objectfile>` : ''
  const mediumKind = mediumObject ? `\n        <mediumKind>${item.type === 'Fog' || item.medium?.kind === 'fog' ? 'fog' : 'fire'}</mediumKind>` : ''
  const position = resolvedPosition ? `\n        <objectPosition>${xmlEscape(resolvedPosition)}</objectPosition>\n        <isdisfromFile>1</isdisfromFile>` : '\n        <isdisfromFile>0</isdisfromFile>'
  return `      <object objName="${name}">
        <types>${xmlEscape(engineType)}</types>
        <meshNames>${xmlEscape(meshNames)}</meshNames>
        <meshSpecularEnabled>${(meshes.length ? meshes : [item]).map(mesh => mesh.specularEnabled ? 1 : 0).join(',')}</meshSpecularEnabled>
        <spectralNames>${xmlEscape(spectralNames)}</spectralNames>
        <thermalNames>${xmlEscape(thermalNames)}</thermalNames>
        <canopyNames>${xmlEscape(canopyNames)}</canopyNames>
        <bioNames>${xmlEscape(materialNames)}</bioNames>
        <propNames>${xmlEscape(materialNames)}</propNames>
        <shapeTypes>${xmlEscape(item.shape || 'cube')}</shapeTypes>
        <shapes>${xmlEscape(dimensions)}</shapes>
        <isLarge>0</isLarge>${mediumKind}${fileName}${position}
      </object>`
}

// 作用：生成一条光谱的 XML（model 为 custom/Prospect/BSM/file，非法值回退 custom）。
// 说明：common 为所有类型共用的镜面、反/透射率、热红外（tau_TIR/ref_TIR）和物理纹理字段；
//   反/透射率可为空格或逗号分隔的逐波段字符串，缺省 0.20 / 0.0；Prospect/BSM 各自追加模型参数。
function spectrumXml(item, index, resolvePath = (value) => value) {
  const model = ['custom', 'Prospect', 'BSM', 'file'].includes(item.model) ? item.model : 'custom'
  const texture = item.physicalTexture || {}
  const specular = normalizeSpecular(item.specular)
  const textureFile = texture.fileName ? resolvePath(texture.fileName) : ''
  const optical = (value, fallback) => (value === null || value === undefined || (typeof value === 'string' && value.trim() === '') ? fallback : value)
  const common = `<specularEnabled>${specular.enabled ? 1 : 0}</specularEnabled><specularFraction>${specular.fraction}</specularFraction><specularRoughness>${specular.roughness}</specularRoughness><reflectance>${xmlEscape(optical(item.reflectance, '0.20'))}</reflectance><transmittance>${xmlEscape(optical(item.transmittance, '0.0'))}</transmittance>\n        <tau_TIR>${number(item.tauTir, 0)}</tau_TIR><ref_TIR>${number(item.refTir, 0.05)}</ref_TIR>${item.refTirWet === null || item.refTirWet === undefined || item.refTirWet === '' ? '' : `<ref_TIR_wet>${number(item.refTirWet, -1)}</ref_TIR_wet>`}\n        <physicalTextureEnabled>${texture.enabled ? 1 : 0}</physicalTextureEnabled><physicalTextureFile>${xmlEscape(textureFile)}</physicalTextureFile><physicalTextureStrength>${Math.max(0, Math.min(1, number(texture.strength, 0.2)))}</physicalTextureStrength><physicalTextureRepeatSize>${Math.max(0.01, number(texture.repeatSize, 2))}</physicalTextureRepeatSize>`
  const params = item.params || {}
  if (model === 'Prospect') return `<spectral id="${index + 1}" name="${xmlEscape(item.name)}" type="Prospect">\n        ${common}\n        <Cab>${number(params.Cab, 40)}</Cab><Cw>${number(params.Cw, 0.01)}</Cw><Cdm>${number(params.Cdm, 0.01)}</Cdm><Cs>${number(params.Cs, 0)}</Cs><N>${number(params.N, 1.5)}</N>\n      </spectral>`
  if (model === 'BSM') return `<spectral id="${index + 1}" name="${xmlEscape(item.name)}" type="BSM">\n        ${common}\n        <SMC>${number(params.SMC, 25)}</SMC><BSMBrightness>${number(params.BSMBrightness, 0.5)}</BSMBrightness><BSMlat>${number(params.BSMlat, 25)}</BSMlat><BSMlon>${number(params.BSMlon, 45)}</BSMlon>\n      </spectral>`
  if (model === 'file') return `<spectral id="${index + 1}" name="${xmlEscape(item.name)}" type="file">\n        ${common}<spectral_file>${xmlEscape(resolvePath(item.fileName || ''))}</spectral_file>\n      </spectral>`
  return `<spectral id="${index + 1}" name="${xmlEscape(item.name)}" type="custom">\n        ${common}<spectral_file>${xmlEscape(resolvePath(item.fileName || ''))}</spectral_file>\n      </spectral>`
}

// 作用：生成一条冠层/结构预设的 XML；structureType 非法时回退 canopy，
//   消光、散射反照率、不对称因子等参与介质参数限幅（下限 0、反照率 0–1、不对称 ±0.99）。
function canopyXml(item) {
  const structureType = ['canopy', 'rigid', 'fire', 'fog'].includes(item.structureType) ? item.structureType : 'canopy'
  return `<canopy name="${xmlEscape(item.name)}"><structureType>${structureType}</structureType><lai>${number(item.lai, 3)}</lai><density>${number(item.density, 1)}</density><hc>${number(item.hc, 2)}</hc><G>${number(item.G, 0.5)}</G><LIDFa>${number(item.LIDFa, -0.35)}</LIDFa><LIDFb>${number(item.LIDFb, -0.15)}</LIDFb><hspot>${number(item.hspot, 0.2)}</hspot><leafwidth>${number(item.leafwidth, 0.1)}</leafwidth><extinction>${Math.max(0, number(item.extinction, 0))}</extinction><scatteringAlbedo>${Math.max(0, Math.min(1, number(item.scatteringAlbedo, 0)))}</scatteringAlbedo><asymmetry>${Math.max(-0.99, Math.min(0.99, number(item.asymmetry, 0)))}</asymmetry><emissionScale>${Math.max(0, number(item.emissionScale, 0))}</emissionScale><fixedTemperature>${Math.max(0, number(item.fixedTemperature, 0))}</fixedTemperature></canopy>`
}

// 作用：生成一条固定表面温度预设的 XML（受光/阴影温度，缺省 305 K / 295 K）。
// DEFERRED[低] 数据一致性: 温度预设以 K 计，但 thermals 在 normalizeProject 中没有任何数值/范围校验，用户按 ℃ 填写 30 会被当作 30 K 静默导出；未修复：温度区间/单位换算属物理约定，按禁止项保留待决策
function thermalXml(item, index) {
  return `<thermal id="${index + 1}" name="${xmlEscape(item.name)}"><sunlitTemperature>${number(item.sunlitTemperature, 305)}</sunlitTemperature><shadedTemperature>${number(item.shadedTemperature, 295)}</shadedTemperature></thermal>`
}

// 下面三个函数生成 FacetEB 的可选 XML 片段：waterInitialXml 水体初始水温模式，
//   wallLayersXml 墙体层（每层“厚度 λ ρ cp”，层间用分号），materialXml 内再组合船舶/车辆等片段。
// FacetEB optional tags; absent fields write nothing, so old XML stays unchanged.
function waterInitialXml(params) {
  if (params.initialTemperatureMode === 'dailyMeanAir') return '<waterInitialMode>dailyMeanAir</waterInitialMode>'
  if (params.initialTemperatureMode === 'fixed') return `<waterInitialMode>fixed</waterInitialMode><waterInitialTemperature>${number(params.initialTemperature, 20)}</waterInitialTemperature>`
  return ''
}

function wallLayersXml(layers) {
  if (!Array.isArray(layers) || !layers.length) return ''
  return `<wallLayers>${layers.map((layer) => [layer?.thickness, layer?.lambda, layer?.rho, layer?.cp].map((value) => number(value, 0)).join(' ')).join(';')}</wallLayers>`
}

// 作用：生成一条物化参数（<property>）的 XML，按类型分派：
//   光伏 → surfaceEnergy（参数键值直接展开）；Vegetation → leafBio（光合/气孔参数）；
//   Water → waterSet；其余（Soil/Building/Vehicle/Ship/木质/薄膜）→ soilSet 并追加墙体/船舶/车辆片段。
// 说明：可选片段缺省时不写，保证旧工程导出的 XML 不变。
function materialXml(item) {
  const params = item.params || {}
  const name = xmlEscape(item.name)
  // DEFERRED[低] 数据一致性: energyModel 为 wood（type 已被归一化为 Vegetation）的材质在此走 leafBio 分支，导出为叶片参数（cs/rhos/lambdas 不出现在 XML）；membrane 落到 soilSet 分支。FacetEB 引擎从 project.json 自行生成 surfaceEnergy（facetrtcore.cpp:2469 已区分 wood），但凡直接读取本 XML 的路径（如 VoxelEB 的 fileio.cpp 的 leafBio 解析）会把木质部当成叶片；未修复：需先确认 VoxelEB 是否消费此 XML，属跨引擎数据源决策
  if (item.energyModel === 'photovoltaic') return '<property><surfaceEnergy name="'+name+'"><photovoltaic>1</photovoltaic><vegetation>0</vegetation>'+Object.entries(params).filter(([k])=>typeof k === 'string' && /^[A-Za-z_][\w.-]*$/.test(k)).map(([k,v])=>'<'+k+'>'+xmlEscape(v)+'</'+k+'>').join('')+'</surfaceEnergy></property>'
  if (item.type === 'Vegetation') return `<property><leafBio name="${name}"><Vcmax>${number(params.Vcmax, 80)}</Vcmax><m>${number(params.m, 9)}</m><BallBerry>${number(params.BallBerry, 0.01)}</BallBerry><Type>${number(params.Type, 3)}</Type><kV>${number(params.kV, 0.6396)}</kV><Rdparam>${number(params.Rdparam, 0.015)}</Rdparam><Tparam>${xmlEscape(params.Tparam || '0.2,0.3,288,313,328')}</Tparam><Tyear>${number(params.Tyear, 25)}</Tyear><beta>${number(params.beta, 0.507)}</beta><kNPQs>${number(params.kNPQs, 0)}</kNPQs><qLs>${number(params.qLs, 1)}</qLs><stressfactor>${number(params.stressfactor, 1)}</stressfactor><Tcor>${number(params.Tcor, 0)}</Tcor></leafBio></property>`
  if (item.type === 'Water') return `<property><waterSet name="${name}"><rss>${number(params.rss, 0)}</rss><heatCapacity>${number(params.heatCapacity, 4186000)}</heatCapacity><mixingDepth>${number(params.mixingDepth, 0.5)}</mixingDepth><evaporationCoefficient>${number(params.evaporationCoefficient, 1)}</evaporationCoefficient><brdfModel>${number(params.brdfModel, 1) ? 'CoxMunk' : 'Lambert'}</brdfModel><refractiveIndex>${number(params.refractiveIndex, 1.333)}</refractiveIndex><slopeVariance>${Math.max(0, number(params.slopeVariance, 0))}</slopeVariance><diffuseFraction>${Math.max(0, Math.min(1, number(params.diffuseFraction, 0.02)))}</diffuseFraction>${waterInitialXml(params)}</waterSet></property>`
  // soilSet 分支：下面先组装可选的墙体（wall）、船舶（ship）、车辆（vehicle）片段，最后拼成 soilSet。
  // DEFERRED[低] Bug: Tsoil 缺省值写成 0.25，其余位置（预设材质）均为 25 ℃，缺字段时初温相差两个数量级；未修复：默认数值常量属禁止修改项，需产品确认单位后统一
  // FacetEB wall/roof slab (wallMode 'wall'); absent = thin shell, so old XML stays unchanged.
  const wall = params.wallMode === 'wall' && !item.energyModel
    ? `<wallMode>wall</wallMode><thickness>${number(params.thickness, 0.2)}</thickness><indoorTemperature>${number(params.indoorTemperature, 22)}</indoorTemperature><indoorHeatTransferCoefficient>${number(params.indoorHeatTransferCoefficient, 8)}</indoorHeatTransferCoefficient>${wallLayersXml(params.layers)}`
    : ''
  // FacetEB ship hull: relative wind (speed > 0) and waterline contact; absent = off.
  const hasNumber = (value) => value !== undefined && value !== null && value !== '' && Number.isFinite(Number(value))
  const ship = item.type === 'Ship' && !item.energyModel
    ? [
        Number(params.speed) > 0 ? `<shipSpeed>${number(params.speed, 0)}</shipSpeed>` : '',
        hasNumber(params.waterlineHeight)
          ? `<waterlineHeight>${number(params.waterlineHeight, 0)}</waterlineHeight><waterHeatTransferCoefficient>${number(params.waterHeatTransferCoefficient, 500)}</waterHeatTransferCoefficient>${hasNumber(params.waterTemperature) ? `<waterTemperature>${number(params.waterTemperature, 20)}</waterTemperature>` : ''}`
          : params.waterlineSource === 'water'
            ? `<waterlineSource>water</waterlineSource><waterlineOffset>${number(params.waterlineOffset, 0)}</waterlineOffset><waterHeatTransferCoefficient>${number(params.waterHeatTransferCoefficient, 500)}</waterHeatTransferCoefficient>${hasNumber(params.waterTemperature) ? `<waterTemperature>${number(params.waterTemperature, 20)}</waterTemperature>` : ''}`
            : '',
        hasNumber(params.heading) && hasNumber(params.windDirection) ? `<shipHeading>${number(params.heading, 0)}</shipHeading><shipWindDirection>${number(params.windDirection, 0)}</shipWindDirection>` : '',
        params.waterShielding ? '<waterShielding>1</waterShielding>' : '',
        params.waterFeedback ? '<waterFeedback>1</waterFeedback>' : '',
        params.voxelWall ? '<voxelWall>1</voxelWall>' : '',
        Number(params.internalHeatFlux) ? `<internalHeatFlux>${number(params.internalHeatFlux, 0)}</internalHeatFlux>` : '',
        Number(params.internalHeatPower) ? `<internalHeatPower>${number(params.internalHeatPower, 0)}</internalHeatPower>` : ''
      ].join('')
    : ''
  // FacetEB vehicle part / cabin parameters (type Vehicle); absent = off, old XML unchanged.
  const vehiclePartCode = { body: 1, glass: 2, underbody: 3, hood: 4 }[params.vehiclePart]
  const cabin = params.cabin || {}
  // Parts model (optional; absent = lumped interior / engine heat on the underbody, old XML unchanged).
  // csv：按 ids 顺序取 source[id][key]（缺省 fallback），用逗号拼接成列表。
  const csv = (source, key, ids, fallback) => ids.map((id) => number(source?.[id]?.[key], fallback)).join(',')
  const interiorXml = (it) => it && it.enabled
    ? `<vcIntModel>1</vcIntModel><vcIntShares>${csv(it, 'share', ['dashboard', 'seat', 'wheel', 'lining'], 0.25)}</vcIntShares><vcIntCapacities>${csv(it, 'capacity', ['dashboard', 'seat', 'wheel', 'lining'], 10000)}</vcIntCapacities><vcIntHcA>${csv(it, 'hcA', ['dashboard', 'seat', 'wheel', 'lining'], 4)}</vcIntHcA><vcIntRadArea>${csv(it, 'radArea', ['dashboard', 'seat', 'wheel', 'lining'], 1)}</vcIntRadArea><vcShellRadFraction>${number(it.shellRadFraction, 0.45)}</vcShellRadFraction><vcSeatEffusivity>${number(it.seatEffusivity, 300)}</vcSeatEffusivity>`
    : ''
  const engineXml = (en) => en && en.enabled
    ? `<vcEngModel>1</vcEngModel><vcEngWarm>${en.warm ? 1 : 0}</vcEngWarm><vcEngWarmTemp>${number(en.warmTemperature, 90)}</vcEngWarmTemp><vcEngCaps>${number(en.blockCapacity, 70000)},${number(en.bayCapacity, 15000)}</vcEngCaps><vcEngBlockBay>${number(en.blockToBay, 40)}</vcEngBlockBay><vcEngBayVent>${number(en.bayVent0, 0.04)},${number(en.bayVentK, 0.004)}</vcEngBayVent><vcEngSplit>${number(en.radiatorShare, 0.5)},${number(en.blockShare, 0.3)},${number(en.exhaustShare, 0.2)}</vcEngSplit><vcFirewallG>${number(en.firewallG, 2)}</vcFirewallG>`
    : ''
  const vehicle = item.type === 'Vehicle' && vehiclePartCode && !item.energyModel
    ? `<vehiclePart>${vehiclePartCode}</vehiclePart>` + (vehiclePartCode === 1 && params.cabin
      ? `<vcEnabled>${cabin.enabled ? 1 : 0}</vcEnabled><vcSpeed>${number(cabin.speed, 0)}</vcSpeed><vcOccupants>${number(cabin.occupants, 0)}</vcOccupants><vcMetabolicW>${number(cabin.metabolicW, 100)}</vcMetabolicW><vcVolume>${number(cabin.volume, 2.8)}</vcVolume><vcInteriorCapacity>${number(cabin.interiorCapacity, 80000)}</vcInteriorCapacity><vcInteriorArea>${number(cabin.interiorArea, 8)}</vcInteriorArea><vcInteriorH>${number(cabin.interiorH, 8)}</vcInteriorH><vcAbsorbedFraction>${number(cabin.absorbedFraction, 0.8)}</vcAbsorbedFraction><vcVentilationAch>${number(cabin.ventilationACH, 0.5)}</vcVentilationAch><vcCrackedAch>${number(cabin.crackedACH, 10)}</vcCrackedAch><vcWindowCracked>${cabin.windowCracked ? 1 : 0}</vcWindowCracked><vcAcOn>${cabin.acOn ? 1 : 0}</vcAcOn><vcAcPower>${number(cabin.acPower, 3500)}</vcAcPower><vcAcSetpoint>${number(cabin.acSetpoint, 24)}</vcAcSetpoint><vcAcBand>${number(cabin.acBand, 2)}</vcAcBand><vcEngineOn>${cabin.engineOn ? 1 : 0}</vcEngineOn><vcEnginePower>${number(cabin.enginePower, 15000)}</vcEnginePower><vcEngineShare>${number(cabin.engineShare, 0.3)}</vcEngineShare>${hasNumber(cabin.heading) && hasNumber(cabin.windDirection) ? `<vcHeading>${number(cabin.heading, 0)}</vcHeading><vcWindDirection>${number(cabin.windDirection, 0)}</vcWindDirection>` : ''}${interiorXml(cabin.interior)}${engineXml(cabin.engine)}`
      : '')
    : ''
  // 土壤方法 1 远期热历史（control.soilInertiaHistory = 'full'）；缺省不写，旧 XML 不变。
  const inertia = params.inertiaHistory === 'full' ? '<inertiaHistory>full</inertiaHistory>' : ''
  // soilSet 缺省值 cs 1180、rhos 1800、lambdas 1.55 为引擎回退值（见 soilset 预设处说明）；
  //   Soil 材质的 method/inertiaHistory 由 projectToXml 按 control 覆盖。
  return `<property><soilSet name="${name}"><method>${number(params.method, 1)}</method><rss>${number(params.rss, 2000)}</rss><cs>${number(params.cs, 1180)}</cs><rhos>${number(params.rhos, 1800)}</rhos><lambdas>${number(params.lambdas, 1.55)}</lambdas><Tsoil>${number(params.Tsoil, 0.25)}</Tsoil><SMC>${number(params.SMC, 25)}</SMC><Satwater>${number(params.Satwater, 0.45)}</Satwater>${inertia}${wall}${ship}${vehicle}</soilSet></property>`
}

// 作用：把工程转换为 HiStream 引擎读取的场景 XML 文本（<HiStreamProject>）。
// 输入：value 工程对象；paths 可选 { outputDir, definedDir, resolveProjectPath, atmosphereLutPath }，
//   用于改写输出目录、defined 目录、相对路径和大气查找表路径。
// 步骤：1) 规范化并取过程输出开关；2) 解析路径；3) 生成 objects/spectra/canopies/thermals/materials；
//   4) 解析背景与 DEM；5) 计算传感器波段、观测角与巡航航点；6) 拼接 Control/Geometry/Atmosphere/Scene/Attribute。
export function projectToXml(value, paths = {}) {
  const project = normalizeProject(value)
  const c = project.configuration
  // 步骤 1：能量平衡模式下，过程输出开关为辐射过程或能量过程任一开启；辐射模式只看 radiationProcess。
  const energyMode = project.mode === 'eFacetEB' || project.mode === 'eVoxelEB'
  const radiationProcess = Boolean(c.sensor.radiationProcess)
  const energyProcess = Boolean(c.sensor.energyProcess)
  const activeProcess = energyMode ? (radiationProcess || energyProcess) : radiationProcess
  // 步骤 2：输出/defined 目录与路径解析；defined 文件沿用 definedDir 中已有的路径分隔符风格。
  const outputDir = paths.outputDir || c.outDir || 'output'
  const definedDir = paths.definedDir || 'defined'
  const resolveProjectPath = paths.resolveProjectPath || ((value) => value)
  const slash = String(definedDir).includes('\\') ? '\\' : '/'
  const definedFile = (name) => `${String(definedDir).replace(/[\\/]$/, '')}${slash}${name}`
  // 步骤 3：对象 XML：只有 VoxelRT/VoxelEB 支持火/雾等参与介质对象，其他模式过滤掉。
  const objects = (c.objects.items || [])
    .filter((item) => project.mode === 'eVoxelRT' || project.mode === 'eVoxelEB' || !(item.type === 'Fire' || item.type === 'Fog' || item.medium))
    .map((item, index) => objectXml(item, index, resolveProjectPath)).join('\n')
  const spectra = c.spectra.map((item, index) => spectrumXml(item, index, resolveProjectPath)).join('\n      ')
  const canopies = c.canopies.map(canopyXml).join('\n      ')
  const thermals = c.thermals.map(thermalXml).join('\n      ')
  // 土壤材质的 method 与远期热历史由 control.soilTemperatureMethod/soilInertiaHistory 统一覆盖。
  const materials = c.materials.map((item) => materialXml(item.type === 'Soil' ? {
    ...item,
    params: { ...(item.params || {}), method: c.control.soilTemperatureMethod, inertiaHistory: c.control.soilInertiaHistory }
  } : item)).join('\n      ')
  // 步骤 4：背景材质类型取其材质条目的 type，找不到时用 background.materialType，再缺省 Soil。
  const background = c.scene.background || {}
  const backgroundMaterial = c.materials.find((item) => item.name === background.materialName)
  const backgroundType = backgroundMaterial?.type || background.materialType || 'Soil'
  // Preserve the selected DEM path while disabled. The isDEM switch controls
  // whether terrain and object elevation offsets are applied.
  const demFile = c.scene.demFile
    ? `\n    <DEM>${xmlEscape(resolveProjectPath(c.scene.demFile))}</DEM>` : ''
  // 步骤 5：传感器：波段列表、透视/平行投影、巡航航点与观测角（与 sensorBandValues 等函数保持一致）。
  const bands = sensorBandValues(c.sensor)
  const perspective = c.sensor.projection === 'perspective'
  const cruiseEnabled = perspective && Boolean(c.sensor.cruiseEnabled)
  const cruisePositions = cruiseEnabled ? sensorCruisePositions(c.sensor, c.scene) : []
  // 透视相机的方位角：有巡航时取首个航点航向 + vaa + 180°（含义待核对），否则直接用 vaa。
  const effectivePerspectiveAzimuth = cruiseEnabled
    ? normalizedAzimuth(number(cruisePositions[0]?.heading, c.sensor.cruiseHeading) + number(c.sensor.vaa, 0) + 180)
    : number(c.sensor.vaa, 0)
  const viewAngles = perspective
    ? [[number(c.sensor.vza, 0), effectivePerspectiveAzimuth]]
    : sensorViewAngles(c.sensor, c.light.azimuth)
  const viewAngleXml = viewAngles.map((angle) => `<viewAngles>${angle[0]},${angle[1]}</viewAngles>`).join('')
  // 巡航航点 XML：uavPositions 记录路线参数，每个 <position> 为 x,y,z（m），heading 为航向（°）。
  const cruiseXml = cruisePositions.length
    ? `\n        <uavPositions enabled="1" route="${xmlEscape(c.sensor.cruiseRoute || 'line')}" heading="${normalizedAzimuth(number(c.sensor.cruiseHeading, 90))}" step="${number(c.sensor.cruiseStep, 25)}" count="${cruisePositions.length}" targetX="${number(c.sensor.cruiseTargetX, c.sensor.positionX)}" targetY="${number(c.sensor.cruiseTargetY, c.sensor.positionY)}" targetZ="${number(c.sensor.cruiseTargetZ, c.sensor.height)}">${cruisePositions.map((position) => `<position heading="${number(position.heading, 0)}">${position.x},${position.y},${position.z}</position>`).join('')}</uavPositions>`
    : ''
  const atmosphere = c.atmosphere || {}
  const atmosphereLut = paths.atmosphereLutPath || atmosphere.lutFile || 'assets/atmosphere/simple_modtran_lut.csv'
  // 步骤 6：拼接 XML。Control 为运行控制（含各 is* 输出开关），Geometry 含光照与传感器，
  //   Atmosphere 为大气，Scene 为场景尺寸、背景与对象，之后为 Attribute（光谱/冠层/热/物化）。
  return `<?xml version="1.0" encoding="UTF-8"?>
<HiStreamProject schemaVersion="${PROJECT_SCHEMA_VERSION}" mode="${xmlEscape(project.mode)}">
  <Control>
    <outDir>${xmlEscape(outputDir)}</outDir>
    <rayTracingDepth>${number(c.control.depth, 4)}</rayTracingDepth>
    <specularReflectionEnabled>${c.control.specularEnabled !== false ? 1 : 0}</specularReflectionEnabled>
    <sampleCount>${number(c.control.samples, 32)}</sampleCount>
    <periodicNeighborCount>${Math.max(0, Math.min(20, Math.round(number(c.control.periodicTraversalCount, 0))))}</periodicNeighborCount>
    <skyboxEnabled>${c.control.skyboxEnabled ? 1 : 0}</skyboxEnabled>
    <facetBackend>${['cpu', 'auto'].includes(c.control.facetBackend) ? c.control.facetBackend : 'gpu'}</facetBackend>
    <computeBackend>${['cpu', 'auto'].includes(c.control.computeBackend) ? c.control.computeBackend : 'gpu'}</computeBackend>
    <cpuThreads>${normalizeCpuThreads(c.control.cpuThreads)}</cpuThreads>
    <radiationSolver>${c.control.radiationSolver === 'accelerated' ? 'accelerated' : 'traditional'}</radiationSolver>
    <spectralAccelerationWidth>${Math.max(1, Math.min(1000, number(c.control.spectralAccelerationWidth, 100)))}</spectralAccelerationWidth>
    <GPU>${number(c.control.gpu, 0)}</GPU>
    <couplingIterations>${number(c.control.couplingIterations, project.mode === 'eFacetEB' ? 60 : 20)}</couplingIterations>
    <temperatureTolerance>${number(c.control.temperatureTolerance, 0.05)}</temperatureTolerance>
    <stabilityCorrection>${c.control.stabilityCorrection === true ? 1 : 0}</stabilityCorrection>
    <temperatureRelaxation>${number(c.control.temperatureRelaxation, 0.8)}</temperatureRelaxation>
    <soilTemperatureMethod>${Math.max(0, Math.min(2, Math.round(number(c.control.soilTemperatureMethod, 2))))}</soilTemperatureMethod>${c.control.soilInertiaHistory === 'full' ? '\n    <soilInertiaHistory>1</soilInertiaHistory>' : ''}
    <shortwaveScatteringOrders>${c.control.shortwaveScatteringOrders}</shortwaveScatteringOrders>
    <voxelCollisionOrders>${c.control.voxelCollisionOrders}</voxelCollisionOrders>
    <radiationMaxSteps>${c.control.radiationMaxSteps}</radiationMaxSteps>
    <vegetationTemperatureMethod>${Math.max(0, Math.min(1, Math.round(number(c.control.vegetationTemperatureMethod, 0))))}</vegetationTemperatureMethod>
    <isDEM>${c.scene.terrain ? 1 : 0}</isDEM>
    <isImage>${c.sensor.image ? 1 : 0}</isImage>
    <isProcess>${activeProcess ? 1 : 0}</isProcess>
    <isRadiationProcess>${radiationProcess ? 1 : 0}</isRadiationProcess>
    <isEnergyProcess>${energyProcess ? 1 : 0}</isEnergyProcess>
    <isTemperature>${c.sensor.temperature ? 1 : 0}</isTemperature>
    <isAlbedo>${c.sensor.albedo ? 1 : 0}</isAlbedo>
    <isDisplay>0</isDisplay>
    <isOrth>0</isOrth>
    <isUAVtrave>${cruiseEnabled ? 1 : 0}</isUAVtrave>
  </Control>
  <Geometry>
    <Light>
      <light name="Solar">
        <lightAngle><lightAngle>${number(c.light.zenith, 30)},${number(c.light.azimuth, 135)}</lightAngle></lightAngle>
        <skyTemperature>${number(c.light.skyTemperature, 250)}</skyTemperature>
        <directScatteringRatio>${number(c.light.direct, 0.8)}</directScatteringRatio>
        <shortwaveIrradiance>${Math.max(0, number(c.light.shortwaveIrradiance, 0))}</shortwaveIrradiance>
        <diffuseFractionMode>${c.light.diffuseFractionMode === 'erbs' ? 'erbs' : 'fixed'}</diffuseFractionMode>
        <esunFileName>${xmlEscape(definedFile('Esun_.dat'))}</esunFileName>
        <eskyFileName>${xmlEscape(definedFile('Esky_.dat'))}</eskyFileName>
      </light>
    </Light>
    <Sensor>
      <sensor name="MainSensor" type="Multispectral sensor">
        <pixelResolutionX>${number(c.sensor.x, 512)}</pixelResolutionX>
        <pixelResolutionY>${number(c.sensor.y, 512)}</pixelResolutionY>
        <controlBand>${xmlEscape((bands.length ? bands : DEFAULT_SENSOR_BANDS).join(','))}</controlBand>
        <continuousBand enabled="${c.sensor.continuousBands ? 1 : 0}"><customBands>${xmlEscape(c.sensor.bands || '')}</customBands><start>${number(c.sensor.bandStart, 400)}</start><end>${number(c.sensor.bandEnd, 2500)}</end><step>${Math.max(0.000001, number(c.sensor.bandStep, 10))}</step></continuousBand>
        <projection>${c.sensor.projection === 'perspective' ? 'perspective' : 'parallel'}</projection>
        <sensorPosition>${number(c.sensor.positionX, number(c.scene.x, 60) / 2)},${number(c.sensor.positionY, number(c.scene.y, 60) / 2)},${number(c.sensor.height, 3000)}</sensorPosition>
        <FOV>${Math.max(0.1, Math.min(120, number(c.sensor.fov, 60)))}</FOV>${cruiseXml}
        <observationSampling principalPlane="${c.sensor.principalPlane ? 1 : 0}" hemisphere="${c.sensor.hemisphere ? 1 : 0}" hemisphereCount="128" customVza="${number(c.sensor.vza, 0)}" customVaa="${number(c.sensor.vaa, 0)}" />
        <viewAngle><viewAngle type="custom">${viewAngleXml}</viewAngle></viewAngle>
      </sensor>
    </Sensor>
  </Geometry>
  <Atmosphere>
    <enabled>${atmosphere.enabled ? 1 : 0}</enabled>
    <model>${xmlEscape(atmosphere.model || 'midlatitude-summer')}</model>
    <waterVapor>${Math.max(0.5, Math.min(5, number(atmosphere.waterVapor, 2)))}</waterVapor>
    <aerosol>${xmlEscape(atmosphere.aerosol || 'rural')}</aerosol>
    <visibility>${Math.max(10, Math.min(50, number(atmosphere.visibility, 23)))}</visibility>
    <lutFile>${xmlEscape(atmosphereLut)}</lutFile>
  </Atmosphere>
  <Scene>
    <TerrainEnvironment ${Object.keys(TERRAIN_ENVIRONMENT_DEFAULTS).map((key) => { const value = c.scene.terrainEnvironment[key]; return `${key}="${typeof value === 'boolean' ? (value ? 1 : 0) : xmlEscape(value)}"` }).join(' ')} />
    <sceneSizeX>${number(c.scene.x, 8)}</sceneSizeX>
    <sceneSizeY>${number(c.scene.y, 8)}</sceneSizeY>
    <Height>${number(c.scene.height, 4)}</Height>
    <offsetX>${number(c.scene.offsetX, 0)}</offsetX>
    <offsetY>${number(c.scene.offsetY, 0)}</offsetY>
    <offsetZ>${number(c.scene.offsetZ, 0)}</offsetZ>
    <voxelSize>${number(c.scene.voxel, 1)}</voxelSize>
    <voxelFillThreshold>${number(c.scene.voxelFillThreshold, 0.05)}</voxelFillThreshold>
    <bgSpectral>${xmlEscape(background.spectralName || 'soil')}</bgSpectral>
    <backgroundSpecularEnabled>${background.specularEnabled ? 1 : 0}</backgroundSpecularEnabled>
    <bgThermal>${xmlEscape(background.thermalName || 'soil_temperature')}</bgThermal>
    <bgBioType>${xmlEscape(backgroundType)}</bgBioType>
    <bgBioName>${xmlEscape(background.materialName || 'soilset')}</bgBioName>
    <backgroundHeterogeneity>${background.heterogeneityEnabled ? 'Hapke' : 'None'}</backgroundHeterogeneity>
    <backgroundAngularStrength>${background.heterogeneityEnabled ? Math.max(0, Math.min(1, number(background.angularEffectStrength, 0.5))) : 0}</backgroundAngularStrength>${demFile}
    <Object>
${objects}
    </Object>
  </Scene>
  <Attribute>
    <Spectral>
      ${spectra}
    </Spectral>
    <Thermal>${thermals}</Thermal>
    <Canopy>${canopies}</Canopy>
    <Biochemistry>${materials}</Biochemistry>
    <Aerodynamics><aerodynamic name="default"><type>0</type><L>0</L><ustar>0.3</ustar><hc_veg>1</hc_veg><hc_build>1</hc_build><lai>0</lai><leaf_width>0.1</leaf_width><stepsizeatmos>1000</stepsizeatmos></aerodynamic></Aerodynamics>
  </Attribute>
  <Meteorology>
    <startTimeNode>${number(c.meteo.start, 1)}</startTimeNode><endTimeNode>${number(c.meteo.end, 48)}</endTimeNode>
    <z>${number(c.meteo.z, 15)}</z><Tsold>${number(c.meteo.Tsold, 300)}</Tsold><SatWater>${number(c.meteo.SatWater, 0.45)}</SatWater><dTime>${number(c.meteo.dTime, 1800)}</dTime>
    <filePath>${xmlEscape(paths.meteoPath || definedFile('meteo.txt'))}</filePath>
    <Latitude>${number(c.meteo.latitude, 40)}</Latitude><Longitude>${number(c.meteo.longitude, 116)}</Longitude>
    <Year>${Math.round(number(c.meteo.year, 2019))}</Year><TimeZone>${number(c.meteo.timezone, 8)}</TimeZone>
  </Meteorology>
</HiStreamProject>
`
}
