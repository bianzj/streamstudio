// ==========================================================================
// 文件：config.mjs
// 功能：STREAM 影像模拟（stream-image-project v2）的工程文件读写，以及由工作台的 scene
//   生成并校验引擎配置（stream-image-config v2）。校验在提交时完成，避免任务出队、输出目录
//   建好之后才由引擎报错
// 主要模块：
//   1. 基础常量与小工具：PROJECT_FILE、OVERPASS/validUtcParts（过境时刻）、spinupNodes、
//      iterationCount、hasNumber
//   2. 能量平衡参数校验：ENERGY_KEYS、validateEnergy、unknownEnergyFields
//   3. 旧版工程迁移：localToUtc、migrateScene（v1 stream-scene-project → v2）
//   4. 路径映射：filePaths、mapStreamScenePaths（读工程转绝对路径，存工程转相对路径）
//   5. createProjectStore 工厂：readProjectFile / saveProjectFile、输入字段与波段校验
//      （inputField、bands、existingFile）、buildInversion、buildAssimilation、
//      buildEngineConfig、sceneWarnings
// 数据流：界面 scene → migrateScene → buildEngineConfig（校验 + 规整）→ 引擎 JSON 配置，
//   由 server/stream/index.mjs 写盘并启动 stream.exe；气象强迫块来自 forcing.mjs
// 与其他文件的关系：校验范围与 models/stream/app/config.cpp 保持一致；
//   字段含义见 models/stream/README.md 的 stream-image-config v2 一节
// 约定：过境时刻为 UTC（ISO，以 Z 结尾）；温度 K；角度度；波长光学 nm、热红外 μm；
//   路径在引擎配置中为 Windows 绝对路径，在工程文件中尽量用相对工程目录的写法
// ==========================================================================

// STREAM image project (stream-image-project v2) and engine configuration (stream-image-config v2).
// STREAM projects are independent of HiStream: inputs are never merged into the 3D project.json.
import { existsSync, mkdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync } from 'node:fs'
import { basename, dirname, extname, isAbsolute, join, relative, resolve, sep } from 'node:path'
import { isUnifiedProject } from '../../src/renderer/src/project-unified.js'
import { createForcingRules, looksLikeMetadataPath } from './forcing.mjs'

// STREAM 工程文件名；COMPONENT_KEYS/LABELS 为四组分温度（光照/阴影土壤、光照/阴影叶片）
// 的键名与界面显示名；OVERPASS 为过境时刻的严格 ISO UTC 格式（秒可省略，必须以 Z 结尾）。
export const PROJECT_FILE = 'stream-project.json'
const COMPONENT_KEYS = ['soilSunlit', 'soilShaded', 'leafSunlit', 'leafShaded']
const COMPONENT_LABELS = { soilSunlit: '光照土壤温度', soilShaded: '阴影土壤温度', leafSunlit: '光照叶片温度', leafShaded: '阴影叶片温度' }
const OVERPASS = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?Z$/
// 作用：对 OVERPASS 匹配结果做日历有效性校验（下面两行英文注释说明了原因）。
// 输入：正则 match；输出：布尔值。年份限制 1900–2200 与引擎 forcing/utc.cpp 一致。
// Strict calendar check of an OVERPASS match: Date.parse accepts 2019-02-30, 2019-04-31 and T24:00:00Z (rolled over by V8),
// which the engine rejects, so round-trip the parts through Date.UTC and reject any that change.
function validUtcParts(match) {
  const [year, month, day, hour, minute] = match.slice(1, 6).map(Number)
  const second = match[6] === undefined ? 0 : Number(match[6])
  if (year < 1900 || year > 2200) return false  // the engine (forcing/utc.cpp) accepts 1900–2200 only
  if (hour > 23 || minute > 59 || second > 59 || month < 1 || month > 12 || day < 1) return false
  const date = new Date(Date.UTC(year, month - 1, day, hour, minute, second))
  date.setUTCFullYear(year)  // years 0–99 are mapped to 1900+ by Date.UTC
  return date.getUTCFullYear() === year && date.getUTCMonth() === month - 1 && date.getUTCDate() === day
}
// 空串/空白/null 视为“未提供”，避免 Number('') === 0 被当成有效取值。
const hasNumber = (value) => value !== null && value !== undefined && String(value).trim() !== '' && Number.isFinite(Number(value))
// 作用：校验起转节点数 temperature.spinupHours 并取整，返回整数个“时次”（不是小时数）。
// 范围 0–240，与引擎一致；向上取整，减 1e-9 是为了吸收浮点误差（7.0000000001 仍得 7）。
function spinupNodes(value) {
  const number = Number(value)
  if (!Number.isFinite(number) || number < 0 || number > 240) throw new Error('起转节点数（temperature.spinupHours）须在 0–240 之间，缺省 7')
  return Math.ceil(number - 1e-9)
}

// 作用：校验能量平衡迭代次数 maxIterations：1–10000 的整数，否则报错。
function iterationCount(value) {
  const number = Number(value)
  if (!Number.isInteger(number) || number < 1 || number > 10000) throw new Error('能量平衡迭代次数 maxIterations 须为 1–10000 的整数')
  return number
}

// 模块：temperature.energy 的键白名单 ENERGY_KEYS 与数值校验 validateEnergy。
// validateEnergy 只拒绝引擎本身会拒绝的取值；空串/null/undefined 视为未给出，
// true/[]/{} 等不会被 Number() 悄悄转成 1/0/NaN。
// FIX(2026-11-24 S1): the engine's temperature.energy rules (config.cpp), checked at submission so a bad value fails here and
// not after the job has left the queue and its output folder exists. Only values the engine rejects are rejected.
export const ENERGY_KEYS = ['scheme', 'soilHeatMethod', 'temperatureMethod', 'diffuseFraction', 'enforceSolarLimit', 'iceMelt',
  'soilResistance', 'stomatalG0', 'minWindSpeed', 'maxAerodynamicResistance', 'referenceHeight', 'heatRoughnessKb',
  'soilEmissivity', 'soilEmissivityWet', 'soilEmissivityModel', 'soilEmissivityK', 'surfaceTemperatureNormalisation',
  'leafEmissivity', 'soilConductivity', 'soilVolumetricCapacity', 'soilThermal', 'soilHeatFraction', 'spectralBinNm',
  'maxIterations', 'maxStabilityIterations']
const isNumeric = (value) => typeof value === 'number' || (typeof value === 'string' && value.trim() !== '')
export function validateEnergy(energy) {
  if (!energy || typeof energy !== 'object') return
  const check = (key, ok, message) => {
    const value = energy[key]
    if (value === undefined || value === null || String(value).trim() === '') return
    // true / [] / {} would turn into 1 / 0 / NaN through Number(); only real numbers (or numeric text) count.
    if (!isNumeric(value) || !Number.isFinite(Number(value)) || !ok(Number(value))) throw new Error(`${key}：${message}`)
  }
  const choice = (key, allowed) => {
    if (energy[key] !== undefined && energy[key] !== null && energy[key] !== '' && !allowed.includes(energy[key]))
      throw new Error(`${key} 须为 ${allowed.join('、')} 之一`)
  }
  choice('soilHeatMethod', ['surface', 'depth', 'direct'])
  choice('temperatureMethod', ['robust', 'direct'])
  check('diffuseFraction', (v) => v === -1 || (v >= 0 && v <= 1), '须为 -1（Erbs）或 0 到 1 之间的数')
  check('spectralBinNm', (v) => Number.isInteger(v) && v >= 1 && v <= 100, '须为 1–100 的整数（nm）')
  check('heatRoughnessKb', (v) => v <= 20, '须不大于 20（≤0 表示按地类）')
  check('soilEmissivityK', (v) => v > 0, '须大于 0')
  check('soilHeatFraction', (v) => v >= 0 && v <= 1, '须在 0 与 1 之间')
  check('leafEmissivity', (v) => v > 0 && v <= 1, '须在 (0, 1] 之间')
  check('stomatalG0', (v) => v > 0, '须大于 0')
  check('minWindSpeed', (v) => v >= 0, '须不小于 0')
  check('referenceHeight', (v) => v > 0, '须大于 0')
  check('soilConductivity', (v) => v > 0, '须大于 0')
  check('soilVolumetricCapacity', (v) => v > 0, '须大于 0')
}
// Fields of temperature.energy that are not known to the engine mapping; they are ignored (never an error, so old
// projects keep running) and reported back to the user as warnings.
export function unknownEnergyFields(scene) {
  const energy = scene?.temperature?.energy
  if (!energy || typeof energy !== 'object' || Array.isArray(energy)) return []
  return Object.keys(energy).filter((key) => !ENERGY_KEYS.includes(key))
}

// 作用：把旧版（v1）工程的本地日期、时钟、时区（相对 UTC 的小时数）换算成 ISO UTC 过境时刻。
// 时区缺省或无效时按 +8（北京时间）处理，保持旧工程行为；格式不合法返回空串。
// Local date + clock + UTC offset (v1 projects) -> ISO UTC overpass.
export function localToUtc(date, time, timezone) {
  const day = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(date || ''))
  const clock = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(String(time || ''))
  if (!day || !clock) return ''
  const offset = timezone === null || timezone === undefined || String(timezone).trim() === '' || !Number.isFinite(Number(timezone)) ? 8 : Number(timezone)
  const ms = Date.UTC(Number(day[1]), Number(day[2]) - 1, Number(day[3]), Number(clock[1]), Number(clock[2]), Number(clock[3] || 0)) - offset * 3600000
  return new Date(ms).toISOString().replace(/\.\d{3}Z$/, 'Z')
}

// 作用：把 v1 单景工程的 scene 迁移为 v2 形态（surface/view/temperature 等分块）。
// v1 没有能量平衡，温度一律按 RT 模式并以 components.source='fields' 读取组分温度。
// v1 (stream-scene-project) form -> v2 form. v2 forms pass through unchanged.
export function migrateScene(scene) {
  if (!scene || typeof scene !== 'object') return null
  if (scene.surface || scene.overpass) return scene
  const inputs = scene.inputs || {}
  const thermal = scene.thermal || {}
  return {
    name: scene.name, outputRoot: scene.outputRoot,
    overpass: localToUtc(scene.time?.date, scene.time?.time, scene.time?.timezone),
    surface: { type: inputs.type, lai: inputs.lai, soilMoisture: inputs.soilMoisture, dem: inputs.dem, latitude: inputs.latitude, longitude: inputs.longitude },
    locationSource: scene.locationSource,
    view: { vza: inputs.vza, vaa: inputs.vaa },
    region: scene.region,
    optical: scene.optical,
    thermal: { enabled: thermal.enabled, preset: thermal.preset, bands: thermal.bands },
    temperature: { mode: 'rt', components: { source: 'fields', ...(thermal.components || {}) } }
  }
}

// 作用：收集 scene 中所有“栅格输入字段”对象（地表、观测几何、组分、反演观测与掩膜、
// 同化视角几何、天空长波），只保留带 file 路径的字段，供 mapStreamScenePaths 批量改写路径。
function filePaths(scene) {
  const inversion = scene.inversion || {}
  const observationFields = (Array.isArray(inversion.observations) ? inversion.observations : [])
    .flatMap((item) => [item?.value, item?.vza, item?.vaa])
  const viewFields = (Array.isArray(scene.assimilation?.views) ? scene.assimilation.views : []).flatMap((item) => [item?.vza, item?.vaa])
  const fields = [...Object.values(scene.surface || {}), ...Object.values(scene.view || {}),
    ...COMPONENT_KEYS.map((key) => scene.temperature?.components?.[key]), ...observationFields, inversion.mask, ...viewFields,
    scene.skyLongwave]
  return fields.filter((field) => field && typeof field === 'object' && String(field.file || '').trim())
}

// 作用：就地把 scene 里出现的所有路径用 convert(path[, 'output']) 改写，并返回 scene。
// 覆盖：栅格字段、组分文件、ERA5/气象强迫数据包与站点文件及元数据路径（仅当看起来是路径时）、
// 同化的数据/误差/QA 文件与各视角文件、outputRoot（第二参数 'output'）。
// 用途：读工程时相对路径 → 绝对路径；保存时绝对路径 → 工程目录内的相对路径。
export function mapStreamScenePaths(scene, convert) {
  for (const field of filePaths(scene)) field.file = convert(field.file)
  const components = scene.temperature?.components
  if (components && String(components.file || '').trim()) components.file = convert(components.file)
  const era5 = scene.temperature?.era5
  if (era5 && String(era5.pack || '').trim()) era5.pack = convert(era5.pack)
  const forcing = scene.temperature?.forcing
  if (forcing && typeof forcing === 'object') {
    if (String(forcing.pack || '').trim()) forcing.pack = convert(forcing.pack)
    if (Array.isArray(forcing.files)) forcing.files = forcing.files.map((file) => String(file || '').trim() ? convert(file) : file)
    if (looksLikeMetadataPath(forcing.metadata)) forcing.metadata = convert(forcing.metadata)
  }
  if (looksLikeMetadataPath(scene.temperature?.forcingMetadata)) scene.temperature.forcingMetadata = convert(scene.temperature.forcingMetadata)
  const assimilation = scene.assimilation
  if (assimilation && typeof assimilation === 'object') {
    for (const key of ['data', 'optical', 'uncertainty', 'covariance', 'qa', 'opticalUncertainty', 'opticalQa'])
      if (String(assimilation[key] || '').trim()) assimilation[key] = convert(assimilation[key])
    for (const view of Array.isArray(assimilation.views) ? assimilation.views : [])
      for (const key of ['data', 'uncertainty', 'qa'])
        if (view && String(view[key] || '').trim()) view[key] = convert(view[key])
  }
  if (String(scene.outputRoot || '').trim()) scene.outputRoot = convert(scene.outputRoot, 'output')
  return scene
}

// 模块：STREAM 工程存取与引擎配置生成的工厂。注入主机路径换算 normalizeHostPath 和
// Windows 路径显示 windowsPath；内部包含工程读写（readProjectFile/saveProjectFile）、
// 各输入块校验与组装（inputField/bands/buildInversion 等）以及运行配置生成。
export function createProjectStore({ normalizeHostPath, windowsPath, forcingDirectories = () => [] }) {
  const forcingRules = createForcingRules({ normalizeHostPath, windowsPath })
  const readDocument = (path) => {
    try { return JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) } catch { return null }
  }

  // 作用：读取 STREAM 工程文件（目录则依次找 stream-project.json 与 project.json）。
  // 步骤：解析 JSON → 统一工程直接返回 null（STREAM 不通过它打开）→ 校验 kind 与版本
  // （v1 stream-scene-project 或 v2 stream-image-project）→ 迁移为 v2 → 把相对路径转成绝对路径。
  // 输出：{ path, name, description, migrated, scene } 或 null（不是 STREAM 工程时）。
  // 统一工程（project.json，format: stream-studio-project）里 STREAM 的输入在 engines.stream，
  // 地理/时间/观测几何等共享输入在 shared；这里编译成工作台认识的 scene。
  function readProjectFile(value) {
    let path = resolve(normalizeHostPath(String(value || '').replace(/^"|"$/g, '')))
    if (existsSync(path) && statSync(path).isDirectory()) {
      path = [join(path, PROJECT_FILE), join(path, 'project.json')].find((candidate) => existsSync(candidate)) || join(path, PROJECT_FILE)
    }
    if (!existsSync(path) || !statSync(path).isFile() || extname(path).toLowerCase() !== '.json') return null
    const source = readDocument(path)
    if (!source) return null
    const baseDir = dirname(path)
    // STREAM 工程与三维工程独立：统一 project.json 不作为 STREAM 工程打开。
    if (isUnifiedProject(source)) return null
    if (source?.kind === 'stream-scene-project') {
      if (source.schemaVersion !== 1) throw new Error(`不支持的 STREAM 单景工程版本：${source.schemaVersion}`)
    } else if (source?.kind === 'stream-image-project') {
      if (source.schemaVersion !== 2) throw new Error(`不支持的 STREAM 影像模拟工程版本：${source.schemaVersion}`)
    } else return null
    const scene = migrateScene(structuredClone(source.scene))
    if (!scene) throw new Error('STREAM 工程缺少 scene 配置')
    mapStreamScenePaths(scene, (file) => windowsPath(resolve(baseDir, normalizeHostPath(String(file).trim()))))
    return { path: windowsPath(path), name: String(source.name || basename(baseDir)), description: String(source.description || ''),
      migrated: source.kind === 'stream-scene-project', scene }
  }

  // 作用：保存 STREAM 工程文件（kind=stream-image-project, schemaVersion=2），返回重新读取的结果。
  // 步骤：确定目标路径 → 迁移 scene → 路径换成相对工程目录的写法（目录外保持绝对路径）
  // → 已存在的文件必须本身是 STREAM 工程才允许覆盖（overwrite=false 时一律拒绝）→ 写盘。
  // Paths inside the project folder are stored relative to it.
  function saveProjectFile(data) {
    const target = data.path
      ? resolve(normalizeHostPath(String(data.path).trim()))
      : join(resolve(normalizeHostPath(String(data.directory || '').trim())), PROJECT_FILE)
    if (!isAbsolute(target) || extname(target).toLowerCase() !== '.json') throw new Error('工程文件须为 .json 的绝对路径')
    const scene = data.scene && typeof data.scene === 'object' ? migrateScene(structuredClone(data.scene)) : null
    if (!scene) throw new Error('缺少影像模拟配置')
    const baseDir = dirname(target)
    const stored = structuredClone(scene)
    mapStreamScenePaths(stored, (file) => {
      const absolute = resolve(normalizeHostPath(String(file).trim()))
      const inside = relative(baseDir, absolute)
      return inside && inside !== '..' && !inside.startsWith(`..${sep}`) && !isAbsolute(inside) ? inside.replaceAll('\\', '/') : windowsPath(absolute)
    })

    // STREAM 工程独立保存：不与同目录的 project.json 合并。
    if (existsSync(target)) {
      const existing = readDocument(target)
      if (!['stream-image-project', 'stream-scene-project'].includes(existing?.kind))
        throw new Error(`${windowsPath(target)} 不是 STREAM 影像模拟工程，不能覆盖；请选择其他目录`)
      if (data.overwrite === false) throw new Error(`${windowsPath(target)} 已存在`)
    }
    mkdirSync(baseDir, { recursive: true })
    const project = { kind: 'stream-image-project', schemaVersion: 2, name: String(data.name || basename(baseDir)), description: String(data.description || ''), scene: stored }
    // Atomic replace: a crash/ENOSPC must not leave a half-written project.
    const temporary = `${target}.${process.pid}.tmp`
    try {
      writeFileSync(temporary, JSON.stringify(project, null, 2) + '\n', 'utf8')
      renameSync(temporary, target)
    } catch (error) {
      try { if (existsSync(temporary)) unlinkSync(temporary) } catch {}
      throw error
    }
    return readProjectFile(target)
  }

  // 作用：把界面传来的“栅格输入字段”规整为引擎格式：{ file, band? } 或 { value }。
  // 规则：给了文件（且 mode 不是 'value'）必须是存在的绝对路径，band 为 0 起的整数，0 不写出；
  // 否则取常数 value（constant=false 时不允许常数）；都没有时 required 报错，否则返回 undefined。
  function inputField(value, label, { constant = true, required = false } = {}) {
    const source = value && typeof value === 'object' ? value : {}
    const file = normalizeHostPath(String(source.file || '').trim().replace(/^"|"$/g, ''))
    if (file && source.mode !== 'value') {
      if (!isAbsolute(file)) throw new Error(`${label}须填写绝对路径`)
      const path = resolve(file)
      if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`${label}文件不存在：${windowsPath(path)}`)
      const band = source.band === undefined || source.band === '' ? 0 : Number(source.band)
      if (!Number.isInteger(band) || band < 0 || band > 100000) throw new Error(`${label}的波段序号无效`)
      return { file: windowsPath(path), ...(band ? { band } : {}) }
    }
    if (source.value !== undefined && source.value !== null && String(source.value).trim() !== '') {
      if (!constant) throw new Error(`${label}必须提供栅格文件`)
      const number = Number(source.value)
      if (!Number.isFinite(number)) throw new Error(`${label}常数无效`)
      return { value: number }
    }
    if (required) throw new Error(`缺少${label}`)
    return undefined
  }

  // 作用：校验并规整波段列表（最多 64 个）。center 须在 [low, high]，fwhm 在 [0, high−low]。
  // 名称中的逗号、花括号、换行替换为空格（避免破坏下游文本参数），截断到 40 字符。
  // 热红外额外要求 fwhm ≤ 2·(center − 0.1)（μm），即波段下沿不低于 0.1 μm（待核对来源）。
  function bands(list, label, low, high) {
    if (!Array.isArray(list)) return []
    if (list.length > 64) throw new Error(`${label}波段不能超过 64 个`)
    return list.map((item, index) => {
      const name = String(item?.name || `B${index + 1}`).replace(/[,{}\r\n]/g, ' ').trim().slice(0, 40) || `B${index + 1}`
      const blank = (value) => typeof value === 'string' && value.trim() === ''
      const center = blank(item?.center) ? NaN : Number(item?.center)
      const fwhm = blank(item?.fwhm) ? NaN : Number(item?.fwhm ?? 0)
      if (!Number.isFinite(center) || center < low || center > high) throw new Error(`${label}波段 ${name} 的中心波长须在 ${low}–${high} 之间`)
      if (!Number.isFinite(fwhm) || fwhm < 0 || fwhm > (high - low)) throw new Error(`${label}波段 ${name} 的带宽无效`)
      if (label === '热红外' && fwhm > 2 * (center - 0.1)) throw new Error(`${label}波段 ${name} 的带宽不能超过 ${Number((2 * (center - 0.1)).toFixed(3))} μm`)
      return { name, center, fwhm }
    })
  }

  // 作用：要求 value 是已存在文件的绝对路径，返回 Windows 形式路径，否则抛出带 label 的错误。
  function existingFile(value, label) {
    const path = resolve(normalizeHostPath(String(value || '').trim().replace(/^"|"$/g, '')))
    if (!String(value || '').trim() || !isAbsolute(path)) throw new Error(`请选择${label}`)
    if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`${label}不存在：${windowsPath(path)}`)
    return windowsPath(path)
  }

  // 作用：组装反演配置块（inversion）：观测列表（1–32 个）、各观测的波长（3–20 μm）、
  // σ（缺省 0.5 K，范围 0.01–10 K）以及可选的观测几何；其余参数的范围校验见下文。
  // LST / LSCT retrieval block, validated with the same ranges as the engine (models/stream/app/config.cpp).
  function buildInversion(input, target) {
    const source = input && typeof input === 'object' ? input : {}
    const list = Array.isArray(source.observations) ? source.observations : []
    if (!list.length) throw new Error('反演至少需要一个热红外观测')
    if (list.length > 32) throw new Error('反演观测不能超过 32 个')
    const range = (value, fallback, low, high, label) => {
      const number = hasNumber(value) ? Number(value) : fallback
      if (!(number >= low && number <= high)) throw new Error(`${label}须在 ${low}–${high} 之间`)
      return number
    }
    const observations = list.map((item, index) => {
      const name = String(item?.name || `obs${index + 1}`).replace(/[,{}\r\n]/g, ' ').trim().slice(0, 40) || `obs${index + 1}`
      const label = `观测 ${name}`
      const observation = {
        name, kind: item?.kind === 'lst' ? 'lst' : 'brightness',
        value: inputField(item?.value, `${label}（K）`, { required: true }),
        wavelength: range(item?.wavelength, NaN, 3, 20, `${label}的波长（μm）`),
        sigma: range(item?.sigma, 0.5, 0.01, 10, `${label}的误差 σ（K）`)
      }
      const vza = inputField(item?.vza, `${label}的天顶角`)
      const vaa = inputField(item?.vaa, `${label}的方位角`)
      if (vza) observation.vza = vza
      if (vaa) observation.vaa = vaa
      return observation
    })
    const block = { target, observations,
      prior: { sd: range(source.prior?.sd, 5, 0.1, 100, '背景场误差（K）'), correlation: range(source.prior?.correlation, 0, 0, 0.95, '背景场相关系数') },
      emissivitySd: range(source.emissivitySd, 0.01, 0, 0.1, '发射率误差'),
      maxIterations: Math.trunc(range(source.maxIterations, 20, 1, 100, '反演迭代次数')) }
    const window = Math.trunc(range(source.window, 1, 1, 9, '反演窗口'))
    if (window % 2 === 0) throw new Error('反演窗口须为奇数')
    block.window = window
    const mask = inputField(source.mask, '反演云/无效掩膜', { constant: false })
    if (mask) block.mask = mask
    return block
  }

  // 作用：判断是否同化。scene.assimilation.mode 为 'assimilate' 时返回观测类型
  // （brightness/lst/components，非法值按 brightness），否则返回 'none'（直接模拟）。
  // Run mode: 直接模拟 or 同化. Only the data and the observation kind are user choices; observation and background
  // Explicit observation errors/QA override engine defaults; controls remain engine-managed.
  function assimilationKind(scene) {
    const source = scene.assimilation
    if (!source || typeof source !== 'object' || source.mode !== 'assimilate') return 'none'
    return ['brightness', 'lst', 'components'].includes(source.observation) ? source.observation : 'brightness'
  }

  // 作用：生成引擎配置的 assimilation 块并做提交时校验。
  // 输入：界面 assimilation、观测类型、热红外/光学是否启用；输出：块对象，不合法时抛错。
  // 要点：数据文件必须存在；uncertainty 与 covariance 二选一；σ 与 LST 波长（3–25 μm）须为
  // 正数；rejectQaBits 为 uint16；观测层级固定（亮温 canopy，其余 surface），暂无 TOA 大气算子；
  // 观测/先验 sourceIds 不得重叠（否则无法假设误差独立）；附加视角最多 16 个且同一文件不得重复。
  function buildAssimilation(source, observation, { thermalEnabled, opticalEnabled }) {
    const label = observation === 'brightness' ? '亮温观测文件' : observation === 'lst' ? '地表温度观测文件' : '组分温度观测文件'
    const block = { observation, data: existingFile(source.data, label) }
    for (const key of ['uncertainty', 'covariance', 'qa', 'opticalUncertainty', 'opticalQa'])
      if (String(source[key] || '').trim()) block[key] = existingFile(source[key], `同化 ${key}`)
    if (block.uncertainty && block.covariance) throw new Error('uncertainty 与 covariance 只能选一个')
    for (const key of ['sigmaBrightnessK', 'sigmaLstK', 'sigmaComponentK', 'sigmaReflectance', 'lstWavelengthUm']) {
      if (source[key] === undefined) continue
      const value = source[key]
      if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0)
        throw new Error(`assimilation.${key} 须为正数`)
      if (key === 'lstWavelengthUm' && (value < 3 || value > 25)) throw new Error('LST 波长须在 3–25 μm')
      block[key] = value
    }
    if (source.rejectQaBits !== undefined) {
      if (!Number.isInteger(source.rejectQaBits) || source.rejectQaBits < 0 || source.rejectQaBits > 65535)
        throw new Error('rejectQaBits 须为 uint16 位掩码')
      block.rejectQaBits = source.rejectQaBits
    }
    const expectedLevel = observation === 'brightness' ? 'canopy' : 'surface'
    if (source.observationLevel !== undefined && source.observationLevel !== expectedLevel)
      throw new Error(`observationLevel 须为 ${expectedLevel}；尚无 TOA 大气算子`)
    block.observationLevel = expectedLevel
    for (const key of ['sourceIds', 'priorSourceIds']) {
      if (source[key] === undefined) continue
      if (!Array.isArray(source[key]) || source[key].some(id => typeof id !== 'string' || !id.trim()))
        throw new Error(`${key} 须为非空来源 ID 数组`)
      block[key] = source[key]
    }
    if ((block.sourceIds || []).some(id => (block.priorSourceIds || []).includes(id)))
      throw new Error('观测与先验 source IDs 重叠，不能假设误差独立')
    if (observation === 'brightness' && !thermalEnabled) throw new Error('同化亮温需要启用热红外波段（观测文件每个波段对应一个热红外波段）')
    if (String(source.optical || '').trim()) {
      if (!opticalEnabled) throw new Error('光学反射率观测需要启用光学波段（文件每个波段对应一个光学波段）')
      block.optical = existingFile(source.optical, '光学反射率观测文件')
    }
    const views = observation === 'brightness' && Array.isArray(source.views) ? source.views : []
    if (views.length > 16) throw new Error('附加观测视角不能超过 16 个')
    const used = views.filter((item) => String(item?.data || '').trim())
    if (used.length) block.views = used.map((item, index) => ({
      data: existingFile(item.data, `附加视角 ${index + 1} 亮温文件`),
      ...Object.fromEntries(['uncertainty', 'qa'].filter(key => String(item[key] || '').trim())
        .map(key => [key, existingFile(item[key], `附加视角 ${index + 1} ${key}`)])),
      vza: inputField(item.vza, `附加视角 ${index + 1} 天顶角 VZA`, { required: true }),
      vaa: inputField(item.vaa, `附加视角 ${index + 1} 方位角 VAA`, { required: true })
    }))
    const observationFiles = [block.data, ...(block.views || []).map(view => view.data)]
    if (new Set(observationFiles.map(path => path.toLowerCase())).size !== observationFiles.length)
      throw new Error('同一个亮温文件不能作为多个独立视角重复使用')
    return block
  }

  // 模块：由 scene 生成引擎配置 stream-image-config v2（本文件的核心函数）。
  // 输入：scene（可为 v1 旧工程）与输出目录、引擎数据目录；输出：可直接序列化的配置对象。
  // 步骤 1：迁移与过境时刻校验 → 2：模拟类型与波段 → 3：地表栅格、地形、土壤亮度 →
  // 4：观测几何与位置范围 → 5：温度来源（EB 或 RT）→ 6：线程、反演、同化、天空长波、区域。
  function buildEngineConfig(input, { outputDir, dataDir }) {
    const scene = migrateScene(input)
    if (!scene) throw new Error('缺少影像模拟配置')
    // 步骤 1：过境时刻须为真实存在的 UTC 时刻；无秒时补 :00（见后面 config 的 overpass）。
    const overpass = String(scene.overpass || '').trim()
    const match = OVERPASS.exec(overpass)
    if (!match || !validUtcParts(match)) throw new Error('过境时刻须为存在的 UTC 日期时间，格式 YYYY-MM-DDTHH:MM:SSZ，年份 1900–2200（不接受 2019-02-30、T24:00 等）')
    // 步骤 2：确定启用哪些模拟。光学、热红外、反演、同化至少启用一个；反演与同化互斥
    // （同化时 inversionTarget 一律为 none）。波段范围：光学 400–2400 nm，热红外 3–20 μm。
    const opticalEnabled = Boolean(scene.optical?.enabled)
    const thermalEnabled = Boolean(scene.thermal?.enabled)
    const assimilation = assimilationKind(scene)
    // The standalone LST / LSCT retrieval stays engine-accessible but is not combined with assimilation.
    const inversionTarget = assimilation === 'none' && ['lst', 'lsct'].includes(scene.inversion?.target) ? scene.inversion.target : 'none'
    if (!opticalEnabled && !thermalEnabled && inversionTarget === 'none' && assimilation === 'none') throw new Error('请至少启用光学、热红外模拟或同化')
    const opticalBands = opticalEnabled ? bands(scene.optical?.bands, '光学', 400, 2400) : []
    const thermalBands = thermalEnabled ? bands(scene.thermal?.bands, '热红外', 3, 20) : []
    if (opticalEnabled && !opticalBands.length) throw new Error('请至少添加一个光学波段')
    if (thermalEnabled && !thermalBands.length) throw new Error('请至少添加一个热红外波段')

    // 步骤 3：地表输入。类型栅格与 LAI 必填；EB 且土壤水分来源不是 'user' 时不要求土壤水分栅格
    // （由气象强迫提供，待核对）；DEM 可选；纬度/经度仅在 locationSource 为 'input' 时必填，
    // 否则引擎由地表类型栅格的地理参考换算（见 README）。
    const surfaceInput = scene.surface || {}
    const temperatureInput = scene.temperature || {}
    const mode = temperatureInput.mode === 'eb' ? 'eb' : 'rt'
    const surface = { type: inputField(surfaceInput.type, '地表类型（IGBP）栅格', { constant: false, required: true }) }
    for (const [key, label, required] of [['lai', '叶面积指数 LAI', true], ['soilMoisture', '土壤水分 SM', false], ['dem', '高程 DEM', false]]) {
      if (key === 'soilMoisture' && mode === 'eb' && temperatureInput.soilMoisture?.source !== 'user') continue
      // EB + source=user means the raster is the actual driver; do not let the
      // engine silently fall back to 0.25 when it is missing.
      const requiredForField = key === 'soilMoisture' && mode === 'eb' && temperatureInput.soilMoisture?.source === 'user' ? true : required
      const field = inputField(surfaceInput[key], label, { required: requiredForField })
      if (field) surface[key] = field
    }
    if (scene.locationSource === 'input') {
      surface.latitude = inputField(surfaceInput.latitude, '纬度', { required: true })
      surface.longitude = inputField(surfaceInput.longitude, '经度', { required: true })
    }
    // 地形模式：single 单一坡面（坡度、坡向可为常数）；facet/composite 面元法（界面里的 composite
    // 按 facet 传给引擎）；hill 山包与 ridge 山脊需要坡度/密度（及走向）栅格；
    // ridge 的 endFraction（端面比例）限制在 0–0.5。
    // Optional receiving-surface orientation for the shortwave: one plane for the scene (single) or a
    // per-pixel slope/aspect field from the DEM (composite).
    const terrainInput = surfaceInput.terrain
    if (terrainInput && typeof terrainInput === 'object' && String(terrainInput.mode || 'none').trim() !== 'none') {
      const terrainMode = String(terrainInput.mode).trim()
      if (terrainMode === 'single') {
        const slope = inputField(terrainInput.slope, '单一坡坡度', { constant: true, required: true })
        const aspect = inputField(terrainInput.aspect, '单一坡坡向', { constant: true, required: true })
        surface.terrain = { mode: 'single', slope, aspect }
      } else if (terrainMode === 'facet' || terrainMode === 'composite') {
        surface.terrain = { mode: 'facet' }
      } else if (terrainMode === 'hill') {
        const slope = inputField(terrainInput.slope, '山包坡度栅格', { constant: false, required: true })
        const density = inputField(terrainInput.density, '山包密度栅格', { constant: false, required: true })
        surface.terrain = { mode: 'hill', slope, density }
      } else if (terrainMode === 'ridge') {
        const slope = inputField(terrainInput.slope, '山脊坡度栅格', { constant: false, required: true })
        const aspect = inputField(terrainInput.aspect, '山脊走向栅格', { constant: false, required: true })
        const density = inputField(terrainInput.density, '山脊密度栅格', { constant: false, required: true })
        const endFraction = Number(terrainInput.endFraction ?? 0)
        if (!(endFraction >= 0 && endFraction <= 0.5)) throw new Error('山脊端面比例 endFraction 须在 0–0.5 之间')
        surface.terrain = { mode: 'ridge', slope, aspect, density, endFraction }
      } else {
        throw new Error('地形辐射模式须为 none、single、facet、hill 或 ridge')
      }
    }
    // 土壤亮度（BSM 参数）：给出时限制在 0.05–1，省略则沿用 classes.json 的 0.25。
    // Optional BSM soil brightness of the scene; omitted keeps classes.json (0.25).
    // FIX(2026-11-24 S3): a given value must be valid; 0 / negative / non-numeric used to be dropped silently (default 0.25).
    const brightnessInput = surfaceInput.soilBrightness?.value ?? surfaceInput.soilBrightness
    if (brightnessInput !== undefined && brightnessInput !== null && String(brightnessInput).trim() !== '') {
      const brightness = Number(brightnessInput)
      if (!Number.isFinite(brightness) || brightness < 0.05 || brightness > 1) throw new Error('地表土壤亮度 soilBrightness 须为 0.05–1 之间的数（省略则用默认 0.25）')
      surface.soilBrightness = brightness
    }
    // 步骤 4：观测几何与位置的常数范围检查（栅格由引擎逐像元检查）：VZA ∈ [0, 90)、
    // VAA ∈ [−360, 360]、纬度 ∈ [−90, 90]、经度 ∈ [−180, 360]，与 README 一致。
    // Constant view geometry and location must be physical (the engine checks the same ranges; rasters are
    // checked per pixel by the engine).
    const constantInRange = (field, low, high, label, { includeHigh = true } = {}) => {
      if (!field || field.file !== undefined || field.value === undefined) return field
      const ok = field.value >= low && (includeHigh ? field.value <= high : field.value < high)
      if (!ok) throw new Error(`${label}须在 ${low}–${high}${includeHigh ? '' : '（不含）'} 度之间`)
      return field
    }
    if (surface.latitude) constantInRange(surface.latitude, -90, 90, '纬度')
    if (surface.longitude) constantInRange(surface.longitude, -180, 360, '经度')
    const view = {
      vza: constantInRange(inputField(scene.view?.vza, '观测天顶角 VZA', { required: true }), 0, 90, '观测天顶角 VZA', { includeHigh: false }),
      vaa: constantInRange(inputField(scene.view?.vaa, '观测方位角 VAA', { required: true }), -360, 360, '观测方位角 VAA')
    }
    // Thermal emission settings shared by EB and RT (the engine reads them from temperature.energy in both modes).
    const emission = (energy = {}) => ({
      soilEmissivityModel: energy.soilEmissivityModel === 'constant' ? 'constant' : 'hapke',
      ...(hasNumber(energy.soilEmissivityK) ? { soilEmissivityK: Number(energy.soilEmissivityK) } : {}),
      surfaceTemperatureNormalisation: energy.surfaceTemperatureNormalisation === 'directional' ? 'directional' : 'retrieval',
      ...Object.fromEntries(['soilEmissivity', 'soilEmissivityWet'].filter((key) => hasNumber(energy[key])).map((key) => [key, Number(energy[key])]))
    })

    // 步骤 5：温度来源。mode='eb'：由逐小时气象强迫做能量平衡，需要气象强迫块、起转节点数
    // （缺省由引擎取 7）与 energy 参数；否则 'rt'：直接给定四组分温度（或 components.tif）。
    // 两种模式都带 emission（土壤发射率设置），因为引擎在两种模式下都从 temperature.energy 读取。
    let temperature
    if (mode === 'eb') {
      validateEnergy(temperatureInput.energy)
      // EB：土壤水分来源只有 'user'（取自上面的栅格）或 'era5'（取自强迫数据）两种。
      // energy 中数值型覆盖项只在界面给出有效数字时才写入，未写的字段由引擎采用其缺省值。
      const source = temperatureInput.soilMoisture?.source === 'user' ? 'user' : 'era5'
      // 提交时按 validateEnergy 校验引擎会拒绝的取值；未知字段不阻断旧工程，只由 sceneWarnings 报告。
      temperature = { mode: 'eb', ...forcingRules.buildForcing(temperatureInput, { projectPath: input.projectPath, directories: forcingDirectories() }), soilMoisture: { source },
        elevationCorrection: temperatureInput.elevationCorrection !== false,
        // EB spin-up = number of whole-hour ERA5 nodes before the overpass (engine default 7 when omitted; 0–240; the
        // engine rounds non-integers up).
        ...(temperatureInput.spinupHours !== undefined && temperatureInput.spinupHours !== null && String(temperatureInput.spinupHours).trim() !== ''
          ? { spinupHours: spinupNodes(temperatureInput.spinupHours) } : {}),
        energy: {
          scheme: 'tirt',
          // Preserve supported overrides; omitted fields keep the native engine's existing defaults.
          ...Object.fromEntries(['stomatalG0', 'minWindSpeed', 'leafEmissivity',
            'soilConductivity', 'soilVolumetricCapacity', 'soilHeatFraction', 'maxStabilityIterations']
            .filter((key) => hasNumber(temperatureInput.energy?.[key]))
            .map((key) => [key, Number(temperatureInput.energy[key])])),
          soilHeatMethod: temperatureInput.energy?.soilHeatMethod || 'surface',
          temperatureMethod: temperatureInput.energy?.temperatureMethod || 'robust',
          diffuseFraction: hasNumber(temperatureInput.energy?.diffuseFraction) ? Number(temperatureInput.energy.diffuseFraction) : 0.2,
          enforceSolarLimit: temperatureInput.energy?.enforceSolarLimit !== false,
          // FIX(2026-11-24): snow / ice classes are capped at 273.15 K by default; only an explicit false is sent.
          ...(temperatureInput.energy?.iceMelt === false ? { iceMelt: false } : {}),
          spectralBinNm: hasNumber(temperatureInput.energy?.spectralBinNm) ? Number(temperatureInput.energy.spectralBinNm) : 50,
          soilThermal: temperatureInput.energy?.soilThermal === 'moisture' ? 'moisture' : 'constant',
          // Soil directional emissivity and the aerodynamic heat-roughness of the bare-soil path.
          ...emission(temperatureInput.energy),
          ...(hasNumber(temperatureInput.energy?.heatRoughnessKb)
            ? { heatRoughnessKb: Number(temperatureInput.energy.heatRoughnessKb) } : {}),
          ...(hasNumber(temperatureInput.energy?.referenceHeight)
            ? { referenceHeight: Number(temperatureInput.energy.referenceHeight) } : {}),
          ...(hasNumber(temperatureInput.energy?.maxAerodynamicResistance)
            ? { maxAerodynamicResistance: Number(temperatureInput.energy.maxAerodynamicResistance) } : {}),
          // FIX(2026-11-24 S4): an invalid maxIterations is an error, not silently the engine default.
          ...(temperatureInput.energy?.maxIterations !== undefined && temperatureInput.energy?.maxIterations !== null && String(temperatureInput.energy.maxIterations).trim() !== ''
            ? { maxIterations: iterationCount(temperatureInput.energy.maxIterations) } : {}),
          soilResistance: {
            method: temperatureInput.energy?.soilResistance?.method === 'sellers' ? 'sellers' : 'constant',
            value: hasNumber(temperatureInput.energy?.soilResistance?.value) ? Number(temperatureInput.energy.soilResistance.value) : 200
          }
        } }
    } else {
      // RT：组分温度（K）。source 为 'package' 时用 4 波段 components.tif，否则四个组分逐项给出
      // （常数或栅格）。不需要热红外成像、反演或同化时不要求组分温度。
      const components = {}
      // LSCT inversion and the RT assimilation need the component temperatures as background even without thermal imaging.
      if (thermalEnabled || inversionTarget === 'lsct' || assimilation !== 'none') {
        const input = temperatureInput.components || {}
        // 组分温度常数按 K 校验（150–400 K）；见下列循环。
        if (input.source === 'package') components.file = existingFile(input.file, '组分温度文件（components.tif）')
        else for (const key of COMPONENT_KEYS) {
          const field = inputField(input[key], `${COMPONENT_LABELS[key]}（K）`, { required: true })
          // 常数必须按开尔文给出；25 之类的摄氏度输入会让热红外结果完全错误。
          if (field && field.value !== undefined && (field.value < 150 || field.value > 400)) {
            throw new Error(`${COMPONENT_LABELS[key]}常数须在 150–400 K 之间（请确认没有把 ℃ 当 K 填写）`)
          }
          components[key] = field
        }
      }
      // RT 模式没有短波辐射输入，只接受固定直散比（-1/Erbs 仅 EB 可用）。
      // 之前完全没把 energy.diffuseFraction 传给引擎，工作台的 C5 修复不可达。
      const rtEnergy = emission(temperatureInput.energy)
      if (hasNumber(temperatureInput.energy?.diffuseFraction)) {
        const diffuseFraction = Number(temperatureInput.energy.diffuseFraction)
        if (!(diffuseFraction >= 0 && diffuseFraction <= 1)) throw new Error('RT 模式的 diffuseFraction 须为 0–1（-1/Erbs 仅 EB 模式可用）')
        rtEnergy.diffuseFraction = diffuseFraction
      }
      temperature = { mode: 'rt', components, energy: rtEnergy }
    }

    // 步骤 6：汇总配置。threads 限制在 0–256 的整数（0 的含义由引擎决定，待核对）；
    // overpass 无秒时补 ':00'，保证引擎收到统一格式。
    const config = {
      kind: 'stream-image-config', schemaVersion: 2,
      outputDir: windowsPath(outputDir), dataDir: windowsPath(dataDir),
      overpass: match[6] ? overpass : overpass.replace(/Z$/, ':00Z'),
      surface, view,
      optical: { enabled: opticalEnabled, bands: opticalBands },
      thermal: { enabled: thermalEnabled, bands: thermalBands },
      temperature,
      threads: Math.max(0, Math.min(256, Math.trunc(Number(scene.threads) || 0)))
    }
    if (inversionTarget !== 'none') config.inversion = buildInversion(scene.inversion, inversionTarget)
    if (assimilation !== 'none') config.assimilation = buildAssimilation(scene.assimilation, assimilation, { thermalEnabled, opticalEnabled })
    // Reflected sky of the thermal brightness: omitted = engine "auto" (EB: ERA5 strd; RT: Swinbank clear sky of the
    // shaded component temperature); { mode: 'none' } = legacy; { value } W m-2 or { file, band } overrides.
    const skyInput = scene.skyLongwave
    if (skyInput && typeof skyInput === 'object') {
      if (skyInput.mode === 'none') config.skyLongwave = { mode: 'none' }
      else {
        const field = inputField(skyInput, '下行长波 skyLongwave（W m⁻²）')
        if (field?.value !== undefined && !(field.value >= 0 && field.value <= 700)) throw new Error('下行长波 skyLongwave 常数须在 0–700 W m⁻²')
        if (field) config.skyLongwave = field
      }
    }
    // 处理区域（可选）：行列号 0 起，终点不含；缺项或非整数、起点 ≥ 终点都会报错。
    const region = scene.region
    if (region && region.enabled) {
      const values = ['startRow', 'endRow', 'startColumn', 'endColumn'].map((key) => (String(region[key] ?? '').trim() === '' ? NaN : Number(region[key])))
      if (!values.every((value) => Number.isInteger(value) && value >= 0)) throw new Error('行列范围须为非负整数')
      const [startRow, endRow, startColumn, endColumn] = values
      if (startRow >= endRow || startColumn >= endColumn) throw new Error('行列范围的终点须大于起点（终点不包含）')
      config.region = { startRow, endRow, startColumn, endColumn }
    }
    return config
  }

  // Non-blocking findings about a scene: fields the engine mapping does not know and therefore drops silently.
  function sceneWarnings(input) {
    const scene = input?.scene && typeof input.scene === 'object' ? input.scene : input
    const unknown = unknownEnergyFields(scene)
    return unknown.length ? [`temperature.energy 中的 ${unknown.join('、')} 不是引擎支持的字段，已忽略（请检查拼写）`] : []
  }
  // 对外接口：读/写工程、生成引擎配置、生成非阻断性警告。
  return { readProjectFile, saveProjectFile, buildEngineConfig, sceneWarnings }
}
