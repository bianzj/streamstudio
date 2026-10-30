// Unified Stream Studio project file ("stream-studio-project").
//
// 一个 project.json 同时承载两个引擎的输入：
//
//   shared            地理 / DEM / 时间 / 天空与大气 / 观测几何 / 输出 / 计算设备 —— 两个引擎共读
//   engines.histream  三维场景（histream.exe）独有输入
//   engines.stream    遥感影像（stream.exe）独有输入
//
// 引擎二进制和既有运行链路不认识这个格式：读写两侧各自在边界上编译/合并，
// 参见 unifiedToHistreamProject / histreamProjectToUnified 与
// unifiedToStreamScene / streamProjectToUnified。因此 GUI 仍然只看到旧的
// project.json / stream-project.json 结构，不需要跟着改。
//
// 光谱波段刻意没有放进共享层：HiStream 的 sensor.bands 是渲染波长列表（nm，含热红外的
// 10500），STREAM 的 optical/thermal.bands 是带半高宽的传感器响应函数（nm / µm）。
// 两者语义不同，强行合并会在任一侧保存时互相覆盖，所以各自留在 engines.* 里。
//
// 本模块是纯函数，既被浏览器（gui）引用，也被 server.mjs / server/stream 引用，不要引入 fs 或 DOM。

import { createDefaultProject, isProjectFormat, normalizeProject, PROJECT_FORMAT, PROJECT_SCHEMA_VERSION } from './project-schema.js'

export const UNIFIED_FORMAT = 'stream-studio-project'
export const UNIFIED_SCHEMA_VERSION = 1
export const IMAGE_FOLDER = 'images'

const COMPONENT_KEYS = ['soilSunlit', 'soilShaded', 'leafSunlit', 'leafShaded']

const object = (value) => (value && typeof value === 'object' && !Array.isArray(value) ? value : {})
const array = (value) => (Array.isArray(value) ? value : [])
const clone = (value) => (value === undefined ? undefined : JSON.parse(JSON.stringify(value)))
// FIXED[中] 数据一致性: Number(null)/Number('')/Number(false) 都等于 0，被当作有效值而不回退；纬度/经度/时区、观测角、天顶角等为 null 或空串时会变成 0（例如位置跑到 0°N 0°E、时区变成 UTC），与 project-schema.js 的 number() 行为不一致；已修复：先按 number() 口径把 null/undefined/空白字符串视为缺失并回退，再解析数值
const numeric = (value, fallback) => {
  if (value === null || value === undefined || (typeof value === 'string' && value.trim() === '')) return fallback
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : fallback
}
const text = (value, fallback = '') => (value === null || value === undefined || value === '' ? fallback : String(value))
const cleanNumber = (value, fallback) => Number(numeric(value, fallback).toFixed(6))
const trimPath = (value) => String(value ?? '').trim().replace(/^"|"$/g, '')

export const isUnifiedProject = (value) =>
  Boolean(value) && typeof value === 'object' && !Array.isArray(value) && value.format === UNIFIED_FORMAT

// 'unified' | 'histream' | 'stream-image' | 'stream-scene' | ''
export function projectFormatOf(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return ''
  if (isUnifiedProject(value)) return 'unified'
  if (value.kind === 'stream-image-project') return 'stream-image'
  if (value.kind === 'stream-scene-project') return 'stream-scene'
  if (isProjectFormat(value.format) || value.configuration) return 'histream'
  return ''
}

// ---------------------------------------------------------------------------
// STREAM 影像工程（scene）默认值。与 src/stream/workspace.js 的 defaults() 保持一致，
// 差别只在于这里两个波段开关默认关闭（未配置时不要伪造波段）。
// ---------------------------------------------------------------------------
export function defaultStreamScene(options = {}) {
  const today = new Date().toISOString().slice(0, 10)
  return {
    name: text(options.name, 'image01'),
    outputRoot: '',
    overpass: `${today}T03:00:00Z`,
    displayTimezone: 8,
    threads: 0,
    surface: { type: {}, lai: {}, soilMoisture: {}, dem: {}, latitude: {}, longitude: {} },
    locationSource: 'georef',
    view: { vza: { value: 0 }, vaa: { value: 0 } },
    region: { enabled: false, startRow: 0, endRow: 1, startColumn: 0, endColumn: 1 },
    optical: { enabled: false, preset: 'landsat', bands: [] },
    thermal: { enabled: false, preset: 'landsat', bands: [] },
    temperature: {
      mode: 'rt',
      components: { source: 'fields', soilSunlit: { value: 315 }, soilShaded: { value: 303 }, leafSunlit: { value: 305 }, leafShaded: { value: 300 } },
      era5: { pack: '' },
      soilMoisture: { source: 'era5' },
      elevationCorrection: true
    }
  }
}

// ---------------------------------------------------------------------------
// 默认结构
// ---------------------------------------------------------------------------
function defaultHistreamEngine() {
  const configuration = createDefaultProject().configuration
  const scene = { ...configuration.scene }
  delete scene.demFile
  delete scene.demInfo
  const sensor = { ...configuration.sensor }
  delete sensor.vza
  delete sensor.vaa
  const control = { ...configuration.control }
  delete control.cpuThreads
  delete control.gpu
  delete control.computeBackend
  const meteo = { ...configuration.meteo }
  delete meteo.latitude
  delete meteo.longitude
  delete meteo.year
  delete meteo.timezone
  return {
    enabled: true,
    mode: 'eVoxelEB',
    scene,
    sensor,
    control,
    fluid: { ...configuration.fluid },
    objects: { ...configuration.objects },
    spectra: clone(configuration.spectra),
    canopies: clone(configuration.canopies),
    thermals: clone(configuration.thermals),
    materials: clone(configuration.materials),
    meteo
  }
}

export function normalizeUnified(value = {}) {
  const source = object(value)
  const defaults = createDefaultProject()
  const configuration = defaults.configuration
  const shared = object(source.shared)
  const engines = object(source.engines)
  const location = object(shared.location)
  const time = object(shared.time)
  const sky = object(shared.sky)
  const observation = object(shared.observation)
  const output = object(shared.output)
  const compute = object(shared.compute)
  const dem = object(location.dem)

  const histreamDefaults = defaultHistreamEngine()
  const sourceHistream = object(engines.histream)
  const streamDefaults = defaultStreamScene({ name: text(source.name, defaults.name) })
  const sourceStream = object(engines.stream)

  return {
    format: UNIFIED_FORMAT,
    schemaVersion: UNIFIED_SCHEMA_VERSION,
    name: text(source.name, defaults.name),
    description: text(source.description),
    createdAt: text(source.createdAt, defaults.createdAt),
    updatedAt: text(source.updatedAt, new Date().toISOString()),
    folders: { ...defaults.folders, images: IMAGE_FOLDER, ...object(source.folders) },
    shared: {
      location: {
        latitude: cleanNumber(location.latitude, configuration.meteo.latitude),
        longitude: cleanNumber(location.longitude, configuration.meteo.longitude),
        timezone: cleanNumber(location.timezone, configuration.meteo.timezone),
        dem: { file: trimPath(dem.file), band: Math.max(0, Math.trunc(numeric(dem.band, 0))) },
        demInfo: location.demInfo ?? null
      },
      time: {
        overpass: text(time.overpass, streamDefaults.overpass),
        year: Math.trunc(numeric(time.year, configuration.meteo.year))
      },
      sky: {
        mode: sky.mode === 'from-time' ? 'from-time' : 'manual',
        zenith: cleanNumber(sky.zenith, configuration.light.zenith),
        azimuth: cleanNumber(sky.azimuth, configuration.light.azimuth),
        direct: cleanNumber(sky.direct, configuration.light.direct),
        diffuse: cleanNumber(sky.diffuse, configuration.light.diffuse),
        skyTemperature: cleanNumber(sky.skyTemperature, configuration.light.skyTemperature)
      },
      atmosphere: { ...configuration.atmosphere, ...object(shared.atmosphere) },
      observation: {
        vza: cleanNumber(observation.vza, configuration.sensor.vza),
        vaa: cleanNumber(observation.vaa, configuration.sensor.vaa)
      },
      output: { root: trimPath(output.root) || configuration.outDir },
      compute: {
        cpuThreads: Math.max(1, Math.min(1024, Math.round(numeric(compute.cpuThreads, configuration.control.cpuThreads)))),
        device: Math.max(0, Math.trunc(numeric(compute.device, configuration.control.gpu))),
        backend: ['cpu', 'auto', 'gpu'].includes(compute.backend) ? compute.backend : configuration.control.computeBackend
      },
      bands: { optical: clone(array(object(shared.bands).optical)), thermal: clone(array(object(shared.bands).thermal)) }
    },
    engines: {
      histream: mergeHistreamEngine(histreamDefaults, sourceHistream),
      stream: mergeStreamEngine(streamDefaults, sourceStream)
    }
  }
}

function mergeHistreamEngine(base, source) {
  // FIXED[低] 可维护性: source 中的 spectra/canopies/thermals/materials 数组以及 scene.background、objects.items 等嵌套对象按引用并入结果，未 clone（默认值一侧却 clone 了）；调用方修改归一化结果会回写到输入对象；已修复：入口对 source 做 clone()，后续合并全部基于副本
  const input = clone(object(source))
  const next = { ...base, ...input }
  next.enabled = input.enabled === undefined ? base.enabled : Boolean(input.enabled)
  next.mode = ['eVoxelEB', 'eFacetEB', 'eFacetRT', 'eVoxelRT', 'eRaytracing'].includes(input.mode) ? input.mode : base.mode
  next.scene = { ...base.scene, ...object(input.scene) }
  next.sensor = { ...base.sensor, ...object(input.sensor) }
  next.control = { ...base.control, ...object(input.control) }
  next.meteo = { ...base.meteo, ...object(input.meteo) }
  next.fluid = { ...base.fluid, ...object(input.fluid) }
  next.objects = { ...base.objects, ...object(input.objects) }
  for (const key of ['spectra', 'canopies', 'thermals', 'materials']) {
    next[key] = Array.isArray(input[key]) ? input[key] : base[key]
  }
  return next
}

function mergeStreamEngine(base, source) {
  const next = { ...base, ...source }
  next.enabled = source.enabled === undefined ? false : Boolean(source.enabled)
  next.surface = { ...base.surface, ...object(source.surface) }
  next.view = { ...base.view, ...object(source.view) }
  next.region = { ...base.region, ...object(source.region) }
  next.optical = { ...base.optical, ...object(source.optical) }
  next.thermal = { ...base.thermal, ...object(source.thermal) }
  const temperature = object(source.temperature)
  next.temperature = {
    ...base.temperature,
    ...temperature,
    components: { ...base.temperature.components, ...object(temperature.components) },
    era5: { ...base.temperature.era5, ...object(temperature.era5) },
    soilMoisture: { ...base.temperature.soilMoisture, ...object(temperature.soilMoisture) }
  }
  return next
}

// ---------------------------------------------------------------------------
// HiStream 旧工程  <->  统一工程
// ---------------------------------------------------------------------------
function histreamEngineSection(project) {
  const configuration = clone(object(project.configuration))
  const scene = { ...object(configuration.scene) }
  delete scene.demFile
  delete scene.demInfo
  const sensor = { ...object(configuration.sensor) }
  delete sensor.vza
  delete sensor.vaa
  const control = { ...object(configuration.control) }
  delete control.cpuThreads
  delete control.gpu
  delete control.computeBackend
  const meteo = { ...object(configuration.meteo) }
  delete meteo.latitude
  delete meteo.longitude
  delete meteo.year
  delete meteo.timezone
  return {
    enabled: true,
    mode: project.mode,
    scene, sensor, control,
    // FIXED[低] 可维护性: fluid/objects 直接引用旧工程对象（objects.items 及其 placements 共享），scene/sensor/control 只是浅拷贝（scene.background 共享），而 spectra 等却做了深拷贝；统一工程被修改时会连带改动仍在内存中的 HiStream 工程；已修复：入口改为 clone(configuration)，fluid/objects/scene 等均基于副本
    fluid: object(configuration.fluid),
    objects: object(configuration.objects),
    spectra: clone(array(configuration.spectra)),
    canopies: clone(array(configuration.canopies)),
    thermals: clone(array(configuration.thermals)),
    materials: clone(array(configuration.materials)),
    meteo
  }
}

function sharedFromHistream(project, previous = {}) {
  const configuration = object(project.configuration)
  const meteo = object(configuration.meteo)
  const scene = object(configuration.scene)
  const light = object(configuration.light)
  const sensor = object(configuration.sensor)
  const control = object(configuration.control)
  const previousShared = object(previous.shared)
  const demFile = trimPath(scene.demFile)
  const previousDem = object(object(previousShared.location).dem)
  const sameDem = demFile.replaceAll('\\', '/') === trimPath(previousDem.file).replaceAll('\\', '/')
  return {
    // 保留上一份工程里由 STREAM 侧写入、HiStream 没有对应字段的内容（过境时刻等）。
    ...previousShared,
    location: {
      latitude: cleanNumber(meteo.latitude, 40),
      longitude: cleanNumber(meteo.longitude, 116),
      timezone: cleanNumber(meteo.timezone, 8),
      // DEFERRED[中] 数据一致性: HiStream 侧清除 DEM（demFile 为空）时这里回退为 previousDem，统一工程里的 DEM 永远删不掉，下次打开又被恢复；下一行 scene.demInfo 为 null 时 ?? 同样回退到旧 DEM 的 demInfo，可能与当前 DEM 不匹配；未修复：需区分“HiStream 未提供”与“显式清空”，属 demFile 回退语义的产品决策
      dem: demFile ? { file: demFile, band: sameDem ? Math.max(0, Math.trunc(numeric(previousDem.band, 0))) : 0 } : previousDem,
      demInfo: scene.demInfo ?? object(previousShared.location).demInfo ?? null
    },
    // FIXED[低] 数据一致性: year 缺省 2024，与 project-schema 默认 2019、normalizeUnified 使用的 configuration.meteo.year 不一致；下方 cpuThreads 也缺少 1024 上限（normalizeUnified 有）；已修复：cpuThreads 增加 1024 上限；year 默认数值属禁止修改项，未改动
    time: { ...object(previousShared.time), year: Math.trunc(numeric(meteo.year, 2024)) },
    sky: {
      // FIXED[中] 数据一致性: 每次从 HiStream 保存都强制 mode: 'manual' 并丢弃 previousShared.sky，STREAM 侧设置的 'from-time'（按过境时刻计算太阳位置）被静默覆盖；已修复：先展开 previousShared.sky 并保留其 mode（仅合法值），再覆盖角度字段
      ...object(previousShared.sky),
      mode: object(previousShared.sky).mode === 'from-time' ? 'from-time' : 'manual',
      zenith: cleanNumber(light.zenith, 30),
      azimuth: cleanNumber(light.azimuth, 135),
      direct: cleanNumber(light.direct, 0.8),
      diffuse: cleanNumber(light.diffuse, 0.2),
      skyTemperature: cleanNumber(light.skyTemperature, 250)
    },
    atmosphere: { ...object(configuration.atmosphere) },
    observation: {
      ...object(previousShared.observation),
      vza: cleanNumber(sensor.vza, 0),
      vaa: cleanNumber(sensor.vaa, 0)
    },
    output: { ...object(previousShared.output), root: trimPath(configuration.outDir) || 'output' },
    compute: {
      ...object(previousShared.compute),
      cpuThreads: Math.max(1, Math.min(1024, Math.round(numeric(control.cpuThreads, 32)))),
      device: Math.max(0, Math.trunc(numeric(control.gpu, 0))),
      backend: ['cpu', 'auto', 'gpu'].includes(control.computeBackend) ? control.computeBackend : 'gpu'
    },
    bands: object(previousShared.bands)
  }
}

// 旧 HiStream 工程 -> 统一工程。previous 用于保留另一个引擎的内容（保存时合并）。
export function histreamProjectToUnified(legacy, previous = {}) {
  const project = normalizeProject(legacy || {})
  const previousSource = object(previous)
  return normalizeUnified({
    format: UNIFIED_FORMAT,
    schemaVersion: UNIFIED_SCHEMA_VERSION,
    name: project.name,
    description: text(previousSource.description),
    createdAt: project.createdAt,
    updatedAt: new Date().toISOString(),
    folders: { ...project.folders, images: text(object(previousSource.folders).images, IMAGE_FOLDER) },
    shared: sharedFromHistream(project, previousSource),
    engines: {
      histream: histreamEngineSection(project),
      stream: object(previousSource.engines).stream
    }
  })
}

// 已有统一工程被 HiStream 侧保存时调用：只更新共享层里 HiStream 拥有的字段和 engines.histream。
export const applyHistreamToUnified = (unified, legacy) => histreamProjectToUnified(legacy, unified)

// 统一工程 -> 旧 HiStream 工程（交给引擎与现有 GUI 代码的形态）。
export function unifiedToHistreamProject(unified) {
  const project = normalizeUnified(unified)
  const shared = project.shared
  const engine = project.engines.histream
  return normalizeProject({
    format: PROJECT_FORMAT,
    schemaVersion: PROJECT_SCHEMA_VERSION,
    name: project.name,
    mode: engine.mode,
    createdAt: project.createdAt,
    updatedAt: project.updatedAt,
    folders: {
      models: project.folders.models,
      positions: project.folders.positions,
      spectral: project.folders.spectral,
      meteorology: project.folders.meteorology,
      terrain: project.folders.terrain,
      output: project.folders.output
    },
    configuration: {
      outDir: shared.output.root,
      scene: {
        ...engine.scene,
        demFile: shared.location.dem.file,
        demInfo: shared.location.demInfo ?? null
      },
      // DEFERRED[低] 数据一致性: shared.sky.mode 为 'from-time' 时仍把存储的手动 zenith/azimuth 交给 HiStream，三维场景的太阳位置与 STREAM 按过境时刻计算的不一致且无提示；未修复：按 overpass+经纬度计算太阳角属物理/产品语义，需先确认 from-time 下的预期行为
      light: {
        zenith: shared.sky.zenith,
        azimuth: shared.sky.azimuth,
        direct: shared.sky.direct,
        diffuse: shared.sky.diffuse,
        skyTemperature: shared.sky.skyTemperature
      },
      sensor: { ...engine.sensor, vza: shared.observation.vza, vaa: shared.observation.vaa },
      control: {
        ...engine.control,
        cpuThreads: shared.compute.cpuThreads,
        gpu: shared.compute.device,
        computeBackend: shared.compute.backend
      },
      fluid: engine.fluid,
      objects: engine.objects,
      spectra: engine.spectra,
      canopies: engine.canopies,
      thermals: engine.thermals,
      materials: engine.materials,
      meteo: {
        ...engine.meteo,
        latitude: shared.location.latitude,
        longitude: shared.location.longitude,
        year: shared.time.year,
        timezone: shared.location.timezone
      },
      atmosphere: shared.atmosphere
    }
  })
}

// ---------------------------------------------------------------------------
// STREAM 影像工程（scene）  <->  统一工程
// ---------------------------------------------------------------------------
const hasValue = (field) => {
  const source = object(field)
  return source.value !== undefined && source.value !== null && String(source.value).trim() !== ''
}
const hasFile = (field) => Boolean(trimPath(object(field).file))
const fieldValue = (field, fallback) => (hasValue(field) ? numeric(object(field).value, fallback) : fallback)

// 统一工程里的 STREAM 段：scene 去掉已经提升到共享层的字段。
// 只有常数经纬度、常数观测角和 DEM 文件进共享层；栅格经纬度/观测角和常数高程没有共享层对应字段，留在这一段。
// outputRoot / threads 刻意留在这一段：STREAM 的影像输出根目录（默认在“文档”下）
// 与三维工程的 output/runs 不是同一个概念，线程数 0 也表示“用满全部核心”，
// 合并进共享层会静默改掉三维工程的输出位置与线程数。
export function streamSceneToEngine(scene) {
  const source = normalizeStreamScene(scene)
  const surface = { ...source.surface }
  if (!hasValue(surface.dem) || hasFile(surface.dem)) delete surface.dem
  if (!hasFile(surface.latitude)) delete surface.latitude
  if (!hasFile(surface.longitude)) delete surface.longitude
  const view = {}
  for (const key of ['vza', 'vaa']) if (hasFile(source.view[key])) view[key] = clone(source.view[key])
  return {
    enabled: true,
    name: source.name,
    outputRoot: source.outputRoot,
    threads: source.threads,
    surface,
    view,
    locationSource: source.locationSource,
    region: source.region,
    optical: source.optical,
    thermal: source.thermal,
    temperature: source.temperature
  }
}

export function normalizeStreamScene(value = {}) {
  const scene = object(value)
  const base = defaultStreamScene({ name: text(scene.name, 'image01') })
  const temperature = object(scene.temperature)
  const { componentsPackage: legacyComponentsPackage, ...temperatureRest } = temperature
  const components = { ...base.temperature.components, ...object(temperature.components) }
  const componentsPackage = object(legacyComponentsPackage)
  if (trimPath(componentsPackage.file)) components.file = trimPath(componentsPackage.file)
  // FIXED[低] 数据一致性: componentsPackage 位于 temperature 上而不是 components 上，这里的 delete 删错对象；下方 ...temperature 仍把 temperature.componentsPackage 原样带入结果并持久化到统一工程，与 components.file 形成两份可能不一致的路径；已修复：从 temperature 解构中剔除 componentsPackage（不再 delete 无用的 components.componentsPackage），只保留归一化后的 components.file
  return {
    ...base,
    ...scene,
    name: text(scene.name, base.name),
    surface: { ...base.surface, ...object(scene.surface) },
    view: { ...base.view, ...object(scene.view) },
    region: { ...base.region, ...object(scene.region) },
    optical: { ...base.optical, ...object(scene.optical), bands: clone(array(object(scene.optical).bands ?? base.optical.bands)) },
    thermal: { ...base.thermal, ...object(scene.thermal), bands: clone(array(object(scene.thermal).bands ?? base.thermal.bands)) },
    temperature: {
      ...base.temperature,
      ...temperatureRest,
      components,
      era5: { ...base.temperature.era5, ...object(temperature.era5) },
      soilMoisture: { ...base.temperature.soilMoisture, ...object(temperature.soilMoisture) }
    }
  }
}

function sharedFromStream(scene, previous = {}) {
  const previousShared = object(previous.shared)
  const previousLocation = object(previousShared.location)
  const previousDem = object(previousLocation.dem)
  const demFile = trimPath(object(scene.surface.dem).file)
  const latitude = object(scene.surface.latitude)
  const longitude = object(scene.surface.longitude)
  // DEFERRED[低] 数据一致性: 与 sharedFromHistream 相同，STREAM 侧清空 DEM 文件后仍回退为 previousDem，共享 DEM 无法删除，下次 unifiedToStreamScene 又会把它写回 surface.dem；未修复：需区分“未提供”和“显式清空”，属 demFile 回退语义的产品决策
  const dem = demFile ? { file: demFile, band: Math.max(0, Math.trunc(numeric(object(scene.surface.dem).band, 0))) } : previousDem
  const demUnchanged = trimPath(dem.file) === trimPath(previousDem.file) && numeric(dem.band, 0) === numeric(previousDem.band, 0)
  return {
    ...previousShared,
    location: {
      // 只有常数经纬度能进共享层；栅格逐像元经纬度留在 engines.stream.surface。
      latitude: hasValue(latitude) ? cleanNumber(latitude.value, 40) : cleanNumber(previousLocation.latitude, 40),
      longitude: hasValue(longitude) ? cleanNumber(longitude.value, 116) : cleanNumber(previousLocation.longitude, 116),
      // DEFERRED[中] 数据一致性: scene 已经过 normalizeStreamScene，默认值 displayTimezone=8、overpass=今天 03:00Z、view.vza/vaa={value:0} 总是存在，因此这里与下方 overpass、observation 的 previous 回退分支永远不会生效；旧 STREAM 工程缺这些字段时，会用 8 / 今天 / 0° 覆盖统一工程中 HiStream 侧设置的时区和观测角；未修复：是否用默认值覆盖既有共享值属产品决策，需原始 legacyScene 字段存在性判断
      timezone: cleanNumber(scene.displayTimezone, numeric(previousLocation.timezone, 8)),
      dem,
      // demInfo 是三维侧对旧 DEM 的解析结果；换了文件就清空，三维侧打开时会重新读取。
      demInfo: demUnchanged ? previousLocation.demInfo ?? null : null
    },
    time: {
      ...object(previousShared.time),
      overpass: text(scene.overpass, object(previousShared.time).overpass || '')
    },
    sky: object(previousShared.sky),
    atmosphere: object(previousShared.atmosphere),
    observation: {
      ...object(previousShared.observation),
      vza: cleanNumber(fieldValue(scene.view.vza, numeric(object(previousShared.observation).vza, 0)), 0),
      vaa: cleanNumber(fieldValue(scene.view.vaa, numeric(object(previousShared.observation).vaa, 0)), 0)
    },
    output: object(previousShared.output),
    compute: object(previousShared.compute),
    bands: object(previousShared.bands)
  }
}

// 旧 STREAM 影像工程（scene）-> 统一工程。previous 用于保留 HiStream 侧内容。
export function streamProjectToUnified(legacyScene, previous = {}) {
  const scene = normalizeStreamScene(legacyScene)
  const previousSource = object(previous)
  const project = normalizeUnified({
    format: UNIFIED_FORMAT,
    schemaVersion: UNIFIED_SCHEMA_VERSION,
    name: text(previousSource.name, scene.name),
    description: text(previousSource.description),
    createdAt: text(previousSource.createdAt, new Date().toISOString()),
    updatedAt: new Date().toISOString(),
    folders: object(previousSource.folders),
    shared: sharedFromStream(scene, previousSource),
    engines: {
      histream: object(previousSource.engines).histream,
      stream: streamSceneToEngine(scene)
    }
  })
  // 共享层里 STREAM 拥有的字段回写到 scene 侧（surface.dem / 经纬度 / view / outputRoot / threads）
  return project
}

export const applyStreamToUnified = (unified, legacyScene) => streamProjectToUnified(legacyScene, unified)

// 统一工程 -> 旧 STREAM 影像工程（scene），交给 buildEngineConfig 与 STREAM 工作台。
export function unifiedToStreamScene(unified) {
  const project = normalizeUnified(unified)
  const shared = project.shared
  const engine = project.engines.stream
  const sharedDem = shared.location.dem.file ? { file: shared.location.dem.file, ...(shared.location.dem.band ? { band: shared.location.dem.band } : {}) } : {}
  // FIXED[低] 数据一致性: rasterField 只保留 file/band，engine.surface/view 中栅格字段的其它属性（如 scale、nodata 或将来新增的键）在打开统一工程时被丢弃，下次保存即永久丢失；已修复：先展开 field 再规范化 file/band（band 为 0 时删除该键，其余属性保留）
  const rasterField = (field) => {
    const next = { ...field, file: trimPath(field.file) }
    const band = numeric(field.band, 0)
    if (band) next.band = band
    else delete next.band
    return next
  }
  const surfaceField = (field, sharedValue) => (hasFile(field) ? rasterField(field) : sharedValue)
  const scene = normalizeStreamScene({
    ...engine,
    name: engine.name || project.name,
    outputRoot: engine.outputRoot,
    overpass: shared.time.overpass,
    displayTimezone: shared.location.timezone,
    threads: engine.threads,
    surface: {
      ...engine.surface,
      // 常数高程只存在 engines.stream 里，优先于共享 DEM 文件。
      dem: hasValue(engine.surface.dem) ? { value: numeric(engine.surface.dem.value, 0) } : sharedDem,
      latitude: surfaceField(object(engine.surface.latitude), { value: shared.location.latitude }),
      longitude: surfaceField(object(engine.surface.longitude), { value: shared.location.longitude })
    },
    locationSource: engine.locationSource,
    view: {
      vza: surfaceField(object(engine.view.vza), { value: shared.observation.vza }),
      vaa: surfaceField(object(engine.view.vaa), { value: shared.observation.vaa })
    },
    optical: engine.optical,
    thermal: engine.thermal,
    temperature: engine.temperature
  })
  return scene
}

// ---------------------------------------------------------------------------
// 便捷入口
// ---------------------------------------------------------------------------
export function createUnifiedProject(options = {}) {
  const histream = options.histream === false ? { enabled: false } : {}
  const stream = options.stream ? { enabled: true, name: text(options.name, 'image01') } : { enabled: false }
  return normalizeUnified({
    format: UNIFIED_FORMAT,
    schemaVersion: UNIFIED_SCHEMA_VERSION,
    name: text(options.name, '未命名工程'),
    createdAt: options.createdAt,
    engines: { histream, stream }
  })
}

export function stringifyUnifiedProject(value) {
  return `${JSON.stringify(normalizeUnified(value), null, 2)}\n`
}

export function parseUnifiedProject(content) {
  let value
  try {
    value = JSON.parse(String(content || ''))
  } catch (error) {
    throw new Error(`project.json 格式错误：${error.message}`)
  }
  if (!isUnifiedProject(value)) throw new Error('不是 Stream Studio 统一工程（缺少 format: stream-studio-project）')
  // DEFERRED[低] 兼容性: 未校验 schemaVersion，更高版本的统一工程会被当作 v1 归一化并在保存时降级、丢弃新字段；未修复：版本更高时拒绝/只读打开属 schema 兼容策略，按要求保留待决策
  return normalizeUnified(value)
}
