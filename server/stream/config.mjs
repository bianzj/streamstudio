// STREAM image project (stream-image-project v2) and engine configuration (stream-image-config v2).
// STREAM projects are independent of HiStream: inputs are never merged into the 3D project.json.
import { existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { basename, dirname, extname, isAbsolute, join, relative, resolve, sep } from 'node:path'
import { isUnifiedProject } from '../../src/renderer/src/project-unified.js'

export const PROJECT_FILE = 'stream-project.json'
const COMPONENT_KEYS = ['soilSunlit', 'soilShaded', 'leafSunlit', 'leafShaded']
const COMPONENT_LABELS = { soilSunlit: '光照土壤温度', soilShaded: '阴影土壤温度', leafSunlit: '光照叶片温度', leafShaded: '阴影叶片温度' }
const OVERPASS = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?Z$/
// 空串/空白/null 视为“未提供”，避免 Number('') === 0 被当成有效取值。
const hasNumber = (value) => value !== null && value !== undefined && String(value).trim() !== '' && Number.isFinite(Number(value))

// Local date + clock + UTC offset (v1 projects) -> ISO UTC overpass.
export function localToUtc(date, time, timezone) {
  const day = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(date || ''))
  const clock = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(String(time || ''))
  if (!day || !clock) return ''
  // FIXED[低] Bug: Number(null) 与 Number('') 都是 0（有限值），旧工程 timezone 为 null/空串时按 UTC+0 而不是注释里的默认 +8 换算，过境时刻偏 8 小时；与 workspace.js 的 localToUtc 是重复实现；已修复：timezone 为 null/undefined/空白串或非有限数时取默认 +8，不再被 Number() 折成 0（与 workspace.js 的重复实现未合并）
  const offset = timezone === null || timezone === undefined || String(timezone).trim() === '' || !Number.isFinite(Number(timezone)) ? 8 : Number(timezone)
  const ms = Date.UTC(Number(day[1]), Number(day[2]) - 1, Number(day[3]), Number(clock[1]), Number(clock[2]), Number(clock[3] || 0)) - offset * 3600000
  return new Date(ms).toISOString().replace(/\.\d{3}Z$/, 'Z')
}

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

function filePaths(scene) {
  const inversion = scene.inversion || {}
  const observationFields = (Array.isArray(inversion.observations) ? inversion.observations : [])
    .flatMap((item) => [item?.value, item?.vza, item?.vaa])
  const viewFields = (Array.isArray(scene.assimilation?.views) ? scene.assimilation.views : []).flatMap((item) => [item?.vza, item?.vaa])
  const fields = [...Object.values(scene.surface || {}), ...Object.values(scene.view || {}),
    ...COMPONENT_KEYS.map((key) => scene.temperature?.components?.[key]), ...observationFields, inversion.mask, ...viewFields]
  return fields.filter((field) => field && typeof field === 'object' && String(field.file || '').trim())
}

export function mapStreamScenePaths(scene, convert) {
  for (const field of filePaths(scene)) field.file = convert(field.file)
  const components = scene.temperature?.components
  if (components && String(components.file || '').trim()) components.file = convert(components.file)
  const era5 = scene.temperature?.era5
  if (era5 && String(era5.pack || '').trim()) era5.pack = convert(era5.pack)
  const assimilation = scene.assimilation
  if (assimilation && typeof assimilation === 'object') {
    for (const key of ['data', 'optical']) if (String(assimilation[key] || '').trim()) assimilation[key] = convert(assimilation[key])
    for (const view of Array.isArray(assimilation.views) ? assimilation.views : [])
      if (view && String(view.data || '').trim()) view.data = convert(view.data)
  }
  if (String(scene.outputRoot || '').trim()) scene.outputRoot = convert(scene.outputRoot, 'output')
  return scene
}

export function createProjectStore({ normalizeHostPath, windowsPath }) {
  const readDocument = (path) => {
    try { return JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) } catch { return null }
  }

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
      // FIXED[低] Bug: startsWith('..') 会把工程目录下以“..”开头的子目录（如 "..data\x.tif"）误判为在工程外而存成绝对路径；已修复：改为排除 inside === '..' 或以 '..' + sep 开头的路径
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
    writeFileSync(target, JSON.stringify(project, null, 2) + '\n', 'utf8')
    return readProjectFile(target)
  }

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

  function bands(list, label, low, high) {
    if (!Array.isArray(list)) return []
    if (list.length > 64) throw new Error(`${label}波段不能超过 64 个`)
    return list.map((item, index) => {
      const name = String(item?.name || `B${index + 1}`).replace(/[,{}\r\n]/g, ' ').trim().slice(0, 40) || `B${index + 1}`
      const center = Number(item?.center)
      const fwhm = Number(item?.fwhm ?? 0)
      if (!Number.isFinite(center) || center < low || center > high) throw new Error(`${label}波段 ${name} 的中心波长须在 ${low}–${high} 之间`)
      if (!Number.isFinite(fwhm) || fwhm < 0 || fwhm > (high - low)) throw new Error(`${label}波段 ${name} 的带宽无效`)
      if (label === '热红外' && fwhm > 2 * (center - 0.1)) throw new Error(`${label}波段 ${name} 的带宽不能超过 ${Number((2 * (center - 0.1)).toFixed(3))} μm`)
      return { name, center, fwhm }
    })
  }

  function existingFile(value, label) {
    const path = resolve(normalizeHostPath(String(value || '').trim().replace(/^"|"$/g, '')))
    if (!String(value || '').trim() || !isAbsolute(path)) throw new Error(`请选择${label}`)
    if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`${label}不存在：${windowsPath(path)}`)
    return windowsPath(path)
  }

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

  // Run mode: 直接模拟 or 同化. Only the data and the observation kind are user choices; observation and background
  // errors, iteration and controls are engine defaults (models/stream/app/assimilation.cpp).
  function assimilationKind(scene) {
    const source = scene.assimilation
    if (!source || typeof source !== 'object' || source.mode !== 'assimilate') return 'none'
    return ['brightness', 'lst', 'components'].includes(source.observation) ? source.observation : 'brightness'
  }

  function buildAssimilation(source, observation, { thermalEnabled, opticalEnabled }) {
    const label = observation === 'brightness' ? '亮温观测文件' : observation === 'lst' ? '地表温度观测文件' : '组分温度观测文件'
    const block = { observation, data: existingFile(source.data, label) }
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
      vza: inputField(item.vza, `附加视角 ${index + 1} 天顶角 VZA`, { required: true }),
      vaa: inputField(item.vaa, `附加视角 ${index + 1} 方位角 VAA`, { required: true })
    }))
    return block
  }

  function buildEngineConfig(input, { outputDir, dataDir }) {
    const scene = migrateScene(input)
    if (!scene) throw new Error('缺少影像模拟配置')
    const overpass = String(scene.overpass || '').trim()
    const match = OVERPASS.exec(overpass)
    if (!match || Number.isNaN(Date.parse(overpass))) throw new Error('过境时刻须为 UTC，格式 YYYY-MM-DDTHH:MM:SSZ')
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

    const surfaceInput = scene.surface || {}
    const temperatureInput = scene.temperature || {}
    const mode = temperatureInput.mode === 'eb' ? 'eb' : 'rt'
    const surface = { type: inputField(surfaceInput.type, '地表类型（IGBP）栅格', { constant: false, required: true }) }
    for (const [key, label, required] of [['lai', '叶面积指数 LAI', true], ['soilMoisture', '土壤水分 SM', false], ['dem', '高程 DEM', false]]) {
      if (key === 'soilMoisture' && mode === 'eb' && temperatureInput.soilMoisture?.source !== 'user') continue
      const field = inputField(surfaceInput[key], label, { required })
      if (field) surface[key] = field
    }
    if (scene.locationSource === 'input') {
      surface.latitude = inputField(surfaceInput.latitude, '纬度', { required: true })
      surface.longitude = inputField(surfaceInput.longitude, '经度', { required: true })
    }
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
    // Optional BSM soil brightness of the scene; omitted keeps classes.json (0.25).
    const brightness = Number(surfaceInput.soilBrightness?.value ?? surfaceInput.soilBrightness)
    if (Number.isFinite(brightness) && brightness > 0) {
      if (brightness < 0.05 || brightness > 1) throw new Error('地表土壤亮度须在 0.05–1 之间')
      surface.soilBrightness = brightness
    }
    // DEFERRED[中] 错误处理: VZA/VAA、纬度/经度常数不做范围检查（引擎 config.cpp 也不检查），VZA 输入 120 或纬度 200 会静默算出错误结果；此外无法区分角度/弧度误填；建议：常数时校验 VZA∈[0,90)、VAA∈[0,360)、纬度∈[-90,90]、经度∈[-180,360)，栅格时在引擎中按像元检查
    const view = { vza: inputField(scene.view?.vza, '观测天顶角 VZA', { required: true }), vaa: inputField(scene.view?.vaa, '观测方位角 VAA', { required: true }) }

    let temperature
    if (mode === 'eb') {
      const source = temperatureInput.soilMoisture?.source === 'user' ? 'user' : 'era5'
      // DEFERRED[低] 错误处理: 这里的校验比引擎 config.cpp 宽松：soilHeatMethod/temperatureMethod 原样透传任意字符串，diffuseFraction 不限 [-1,1]，spectralBinNm 不限 1–100 的整数，heatRoughnessKb 不限 ≤20，soilEmissivityK 允许 ≤0；这些错误要等任务出队、建好输出目录后才由引擎报出；建议：在提交时按引擎相同规则校验（白名单 + 范围）
      temperature = { mode: 'eb', era5: { pack: existingFile(temperatureInput.era5?.pack, 'ERA5 数据包') }, soilMoisture: { source },
        elevationCorrection: temperatureInput.elevationCorrection !== false,
        energy: {
          scheme: 'tirt',
          // Preserve supported overrides; omitted fields keep the native engine's existing defaults.
          ...Object.fromEntries(['stomatalG0', 'minWindSpeed', 'soilEmissivity', 'soilEmissivityWet', 'leafEmissivity',
            'soilConductivity', 'soilVolumetricCapacity', 'soilHeatFraction', 'maxStabilityIterations']
            .filter((key) => hasNumber(temperatureInput.energy?.[key]))
            .map((key) => [key, Number(temperatureInput.energy[key])])),
          soilHeatMethod: temperatureInput.energy?.soilHeatMethod || 'surface',
          temperatureMethod: temperatureInput.energy?.temperatureMethod || 'robust',
          // FIXED[中] Bug: Number('') 与 Number(null) 都是有限值 0，表单清空后 diffuseFraction 变成 0（而不是默认 0.2），spectralBinNm/referenceHeight/soilEmissivityK/soilResistance.value 变成 0 并导致引擎报“参数超出有效范围”；已修复：下列取值统一改用 hasNumber()，空串/空白/null 视为未提供，保持原默认值或省略该字段
          diffuseFraction: hasNumber(temperatureInput.energy?.diffuseFraction) ? Number(temperatureInput.energy.diffuseFraction) : 0.2,
          enforceSolarLimit: temperatureInput.energy?.enforceSolarLimit !== false,
          spectralBinNm: hasNumber(temperatureInput.energy?.spectralBinNm) ? Number(temperatureInput.energy.spectralBinNm) : 50,
          soilThermal: temperatureInput.energy?.soilThermal === 'moisture' ? 'moisture' : 'constant',
          // Soil directional emissivity and the aerodynamic heat-roughness of the bare-soil path.
          soilEmissivityModel: temperatureInput.energy?.soilEmissivityModel === 'constant' ? 'constant' : 'hapke',
          ...(hasNumber(temperatureInput.energy?.soilEmissivityK)
            ? { soilEmissivityK: Number(temperatureInput.energy.soilEmissivityK) } : {}),
          surfaceTemperatureNormalisation:
            temperatureInput.energy?.surfaceTemperatureNormalisation === 'directional' ? 'directional' : 'retrieval',
          ...(hasNumber(temperatureInput.energy?.heatRoughnessKb)
            ? { heatRoughnessKb: Number(temperatureInput.energy.heatRoughnessKb) } : {}),
          ...(hasNumber(temperatureInput.energy?.referenceHeight)
            ? { referenceHeight: Number(temperatureInput.energy.referenceHeight) } : {}),
          ...(hasNumber(temperatureInput.energy?.maxAerodynamicResistance)
            ? { maxAerodynamicResistance: Number(temperatureInput.energy.maxAerodynamicResistance) } : {}),
          ...(Number.isInteger(Number(temperatureInput.energy?.maxIterations)) && Number(temperatureInput.energy.maxIterations) > 0
            ? { maxIterations: Number(temperatureInput.energy.maxIterations) } : {}),
          soilResistance: {
            method: temperatureInput.energy?.soilResistance?.method === 'sellers' ? 'sellers' : 'constant',
            value: hasNumber(temperatureInput.energy?.soilResistance?.value) ? Number(temperatureInput.energy.soilResistance.value) : 200
          }
        } }
    } else {
      const components = {}
      // LSCT inversion and the RT assimilation need the component temperatures as background even without thermal imaging.
      if (thermalEnabled || inversionTarget === 'lsct' || assimilation !== 'none') {
        const input = temperatureInput.components || {}
        // DEFERRED[中] 错误处理: 组分温度常数要求 K，但不校验范围；用户按 °C 输入 25 会被当成 25 K，热红外结果完全错误而无提示；建议：常数限定在约 150–400 K，低于 150 时提示“是否误填为 °C”
        if (input.source === 'package') components.file = existingFile(input.file, '组分温度文件（components.tif）')
        else for (const key of COMPONENT_KEYS) components[key] = inputField(input[key], `${COMPONENT_LABELS[key]}（K）`, { required: true })
      }
      temperature = { mode: 'rt', components }
    }

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
    const region = scene.region
    if (region && region.enabled) {
      const values = ['startRow', 'endRow', 'startColumn', 'endColumn'].map((key) => Number(region[key]))
      if (!values.every((value) => Number.isInteger(value) && value >= 0)) throw new Error('行列范围须为非负整数')
      const [startRow, endRow, startColumn, endColumn] = values
      if (startRow >= endRow || startColumn >= endColumn) throw new Error('行列范围的终点须大于起点（终点不包含）')
      config.region = { startRow, endRow, startColumn, endColumn }
    }
    return config
  }

  return { readProjectFile, saveProjectFile, buildEngineConfig }
}
