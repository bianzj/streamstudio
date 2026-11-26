// ==========================================================================
// 文件：workspace.js
// 功能：STREAM 遥感影像模拟工作台（Vite + 原生 JS 前端）。一个工程对应一个过境时刻的影像模拟：
//       RT 模式直接给定四组分温度（Tss/Tsh/Tcs/Tch）；EB 模式由逐小时气象强迫求解能量平衡；
//       同化模式用观测（亮温/地表温度/组分温度）更新背景场。本文件只负责界面、表单规范化与校验、
//       与服务端交互和结果查看，不含物理计算（物理在 models/stream 引擎中）。
// 主要模块：
//   1. 常量与预设：传感器波段预设、IGBP 地类、产品显示信息、波段名映射、色带、输入字段声明表
//   2. 表单规范化与导出：旧版单景表单迁移、normalizeScene / exportScene、同化设置规范化
//   3. EB 起转计划与气象数据包校验：ebPlan、packProblems、packWarnings（规则与引擎一致）
//   4. 坐标与绘图小工具：UTM 反算、色带查找表、比例尺取整、float32 解码、草稿读取
//   5. createStreamWorkspace 闭包：布局与参数面板 → 运行方式（同化）→ 输入事件、栅格探测与校验
//      → 工程保存/打开 → 运行控制 → 结果查看器（产品、色带、拉伸、直方图、统计、读数）
//      → 日志 → 事件绑定 → 气象数据包登记管理 → 引擎事件处理 → 对外命令
// 数据流：表单 form → exportScene / runPayload → streamApi（./api.js，对应服务端 /api/stream/*，
//       由 server/stream/config.mjs 生成 stream-image-config）→ STREAM 引擎（models/stream）
//       → 输出目录中的 GeoTIFF 与摘要 → 事件通道 handleEvent → showRun → 查看器读取栅格并显示。
// 与其他文件：./api.js（HTTP 与 SSE 客户端）；../storage.js（appStorage 本地存储）；
//       ./forcing-panel.js（气象强迫元数据面板，EB 模式使用）；
//       main.js 调用 createStreamWorkspace({ toast }) 创建，并使用返回的 open/openProject/command。
// 重要约定：温度单位 K（界面可切换为 °C，仅用于显示）；过境时刻为 ISO UTC 字符串，
//       displayTimezone 仅用于显示当地时间；角度单位度，方位角自正北顺时针；
//       光学波段中心波长/半高宽单位 nm，热红外单位 μm；栅格行列从 0 开始；
//       气象强迫节点均为整点时次（EB 起转规则见 ebPlan）。
// ==========================================================================

// STREAM remote-sensing image workspace: one overpass per image, with component temperatures either
// given (RT) or solved by a forcing-driven energy balance (EB). Independent of the HiStream 3-D project.
// 模块：依赖与本地存储键。
// STORAGE_KEY 保存表单草稿，PROJECT_KEY 保存当前工程；LEGACY_STORAGE_KEYS 为旧版本使用的键，
// 读取草稿时若新键为空则回退到旧键（见 loadDraft）。
import { streamApi } from './api.js'
import { appStorage } from '../storage.js'
import { createForcingPanel } from './forcing-panel.js'

const STORAGE_KEY = 'streamStudioImage'
const LEGACY_STORAGE_KEYS = ['streamStudioScene', 'histreamStreamScene']
const PROJECT_KEY = 'streamStudioImageProject'

// 模块：传感器波段预设。光学条目为 [波段名, 中心波长 nm, 半高宽 nm]；
// 热红外条目为 [波段名, 中心波长 μm, 半高宽 μm]。用户选“自定义”时不使用这些预设。
const OPTICAL_PRESETS = {
  sentinel2: { label: 'Sentinel-2 MSI', bands: [['B02', 490, 65], ['B03', 560, 35], ['B04', 665, 30], ['B05', 705, 15], ['B06', 740, 15], ['B07', 783, 20], ['B08', 842, 115], ['B8A', 865, 20], ['B11', 1610, 90], ['B12', 2190, 180]] },
  landsat: { label: 'Landsat 8/9 OLI', bands: [['B1', 443, 16], ['B2', 482, 60], ['B3', 561, 57], ['B4', 655, 37], ['B5', 865, 28], ['B6', 1609, 85], ['B7', 2201, 187]] },
  modis: { label: 'MODIS 1–7', bands: [['B1', 645, 50], ['B2', 858.5, 35], ['B3', 469, 20], ['B4', 555, 20], ['B5', 1240, 20], ['B6', 1640, 24], ['B7', 2130, 50]] },
  rgbnir: { label: '红绿蓝 + 近红外', bands: [['Blue', 490, 60], ['Green', 560, 40], ['Red', 665, 30], ['NIR', 842, 100]] }
}
const THERMAL_PRESETS = {
  landsat: { label: 'Landsat 8/9 TIRS', bands: [['B10', 10.9, 0.6], ['B11', 12.0, 1.0]] },
  modis: { label: 'MODIS 31/32', bands: [['B31', 11.03, 0.5], ['B32', 12.02, 0.5]] },
  slstr: { label: 'Sentinel-3 SLSTR S8/S9', bands: [['S8', 10.85, 0.9], ['S9', 12.0, 1.0]] },
  broadband: { label: '单波段 10.5 μm', bands: [['TIR', 10.5, 0]] }
}
// 模块：运行模式与同化观测类型的显示名（键即写入配置的取值）。
const RUN_MODES = { simulate: '直接模拟', assimilate: '同化' }
const ASSIMILATION_OBSERVATIONS = { brightness: '亮温', lst: '地表温度', components: '组分温度' }
// "file path or constant" text inputs of the run-mode panel.
const parseSource = (text) => {
  const value = String(text ?? '').trim()
  if (!value) return {}
  return /^[-+]?\d+(\.\d+)?$/.test(value) ? { value: Number(value) } : { file: value }
}
const sourceText = (source) => source?.file ? source.file : source?.value !== undefined && source?.value !== '' ? String(source.value) : ''
// 模块：IGBP 地类编码 → 中文名，用于分地类统计与地表类型提示；18 与 255 均按“填充值”处理。
const IGBP = {
  0: '未分类', 1: '常绿针叶林', 2: '常绿阔叶林', 3: '落叶针叶林', 4: '落叶阔叶林', 5: '混交林', 6: '郁闭灌丛', 7: '稀疏灌丛',
  8: '木本稀树草原', 9: '稀树草原', 10: '草地', 11: '永久湿地', 12: '农田', 13: '城市与建成区', 14: '农田与自然植被镶嵌',
  15: '冰雪', 16: '裸地', 17: '水体', 18: '填充值', 255: '填充值'
}
// 模块：引擎输出文件（GeoTIFF）的显示元数据。键为输出文件名；
// label 为界面显示名，kind 为产品类别（光学/热红外/通量/不确定度等），
// unit 为单位，digits 为读数小数位，colormap 为该产品的缺省色带。
const PRODUCTS = {
  'optical_reflectance.tif': { label: '光学反射率', kind: 'optical', unit: '', digits: 4, colormap: 'gray' },
  'tir_brightness_temperature.tif': { label: '热红外亮温', kind: 'thermal', unit: 'K', digits: 2, colormap: 'inferno' },
  'tir_surface_temperature.tif': { label: '热红外方向温度', kind: 'thermal', unit: 'K', digits: 2, colormap: 'inferno' },
  'components.tif': { label: '组分温度', kind: 'thermal', unit: 'K', digits: 2, colormap: 'inferno' },
  'eb_fluxes.tif': { label: '能量平衡通量', kind: 'flux', unit: 'W m⁻²', digits: 1, colormap: 'rdylbu' },
  'eb_forcing.tif': { label: '气象强迫（过境时刻）', kind: 'forcing', unit: '', digits: 2, colormap: 'viridis' },
  'solar_geometry.tif': { label: '太阳天顶角与方位角', kind: 'angle', unit: '°', digits: 2, colormap: 'viridis' },
  'lst.tif': { label: '反演 · 地表温度 LST', kind: 'thermal', unit: 'K', digits: 2, colormap: 'inferno' },
  'lst_uncertainty.tif': { label: '反演 · LST 不确定度 1σ', kind: 'sigma', unit: 'K', digits: 3, colormap: 'viridis' },
  'lst_qa.tif': { label: '反演 · LST 质量与残差', kind: 'qa', unit: '', digits: 3, colormap: 'viridis' },
  'lsct.tif': { label: '反演 · 组分温度 LSCT', kind: 'thermal', unit: 'K', digits: 2, colormap: 'inferno' },
  'lsct_uncertainty.tif': { label: '反演 · LSCT 不确定度 1σ', kind: 'sigma', unit: 'K', digits: 3, colormap: 'viridis' },
  'lsct_qa.tif': { label: '反演 · LSCT 质量、残差与 DFS', kind: 'qa', unit: '', digits: 3, colormap: 'viridis' },
  'lsct_fractions.tif': { label: '反演 · 组分有效权重占比', kind: 'fraction', unit: '', digits: 3, colormap: 'viridis' },
  'lsct_radiance_fractions.tif': { label: '反演 · 组分辐射贡献占比', kind: 'fraction', unit: '', digits: 3, colormap: 'viridis' },
  'analysis_components.tif': { label: '同化 · 分析组分温度', kind: 'thermal', unit: 'K', digits: 2, colormap: 'inferno' },
  'background_components.tif': { label: '同化 · 背景组分温度', kind: 'thermal', unit: 'K', digits: 2, colormap: 'inferno' },
  'analysis_uncertainty.tif': { label: '同化 · 分析不确定度 1σ', kind: 'sigma', unit: '', digits: 3, colormap: 'viridis' },
  'analysis_qa.tif': { label: '同化 · 质量、O−B/O−A 与 DFS', kind: 'qa', unit: '', digits: 3, colormap: 'viridis' },
  'analysis_controls.tif': { label: '同化 · 分析控制变量（土壤水分/Vcmax/LAI）', kind: 'forcing', unit: '', digits: 3, colormap: 'viridis' },
  'background_controls.tif': { label: '同化 · 背景控制变量', kind: 'forcing', unit: '', digits: 3, colormap: 'viridis' },
  'analysis_fluxes.tif': { label: '同化 · 分析能量通量', kind: 'flux', unit: 'W m⁻²', digits: 1, colormap: 'rdylbu' }
}
// 模块：波段描述 → 中文显示名。左侧正则匹配引擎写入 GeoTIFF 的英文波段描述，
// 右侧为界面显示名；bandLabel() 按顺序取第一个匹配项，未匹配时做简单英译中替换。
const BAND_LABELS = [
  [/^Tss sunlit soil temperature \(K\)$/, '光照土壤 Tss'], [/^Tsh shaded soil temperature \(K\)$/, '阴影土壤 Tsh'],
  [/^Tcs sunlit leaf temperature \(K\)$/, '光照叶片 Tcs'], [/^Tch shaded leaf temperature \(K\)$/, '阴影叶片 Tch'],
  [/^net radiation/, '净辐射 Rn'], [/^sensible heat/, '显热 H'], [/^latent heat/, '潜热 LE'], [/^ground heat/, '土壤热通量 G'],
  [/^iterations$/, '迭代次数'], [/^converged$/, '收敛（1 是 0 否）'],
  [/^Obukhov length/, 'Obukhov 长度 L m'], [/^friction velocity/, '摩擦速度 u* m/s'],
  [/^stability converged$/, '稳定度收敛'], [/^stability residual/, '稳定度残差 1/m'],
  [/^stability limited$/, '稳定度限幅'], [/^energy balance residual/, '能量残差 W/m²'],
  [/^gustiness residual/, '阵风一致性残差 W/m²'], [/^air density/, '空气密度 kg/m³'],
  [/^aerodynamic roughness/, '气动粗糙度 m'], [/^aerodynamic displacement/, '零平面位移 m'],
  [/^aerodynamic canopy weight/, '冠层混合权重'], [/^shortwave partition limited$/, '短波直散限幅'],
  [/^air temperature/, '气温 K'], [/^vapour pressure/, '水汽压 hPa'], [/^surface pressure/, '气压 hPa'], [/^wind speed/, '风速 m/s'],
  [/^downward shortwave/, '下行短波 W/m²'], [/^downward longwave/, '下行长波 W/m²'], [/^soil moisture/, '土壤水分 m³/m³'],
  [/^LST \(K\)$/, 'LST'], [/^LST uncertainty/, 'LST 1σ K'], [/^quality flags$/, '质量标记（位）'], [/^residual RMS/, '残差 RMS K'],
  [/^observations used$/, '参与观测数'], [/^effective emissivity$/, '有效发射率'], [/^degrees of freedom for signal$/, '信号自由度 DFS'],
  [/^Tss uncertainty/, 'Tss 1σ K'], [/^Tsh uncertainty/, 'Tsh 1σ K'], [/^Tcs uncertainty/, 'Tcs 1σ K'], [/^Tch uncertainty/, 'Tch 1σ K'],
  [/^Tss radiance share$/, 'Tss 辐射占比'], [/^Tsh radiance share$/, 'Tsh 辐射占比'], [/^Tcs radiance share$/, 'Tcs 辐射占比'], [/^Tch radiance share$/, 'Tch 辐射占比'],
  [/^analysis Tss/, '分析 Tss'], [/^analysis Tsh/, '分析 Tsh'], [/^analysis Tcs/, '分析 Tcs'], [/^analysis Tch/, '分析 Tch'],
  [/^background Tss/, '背景 Tss'], [/^background Tsh/, '背景 Tsh'], [/^background Tcs/, '背景 Tcs'], [/^background Tch/, '背景 Tch'],
  [/^Tss analysis 1-sigma/, 'Tss 1σ K'], [/^Tsh analysis 1-sigma/, 'Tsh 1σ K'], [/^Tcs analysis 1-sigma/, 'Tcs 1σ K'], [/^Tch analysis 1-sigma/, 'Tch 1σ K'],
  [/^soil moisture analysis 1-sigma/, '土壤水分 1σ'], [/^Vcmax25 analysis 1-sigma/, 'Vcmax 1σ'], [/^LAI analysis 1-sigma/, 'LAI 1σ'],
  [/^QA flags$/, '质量标记（位）'], [/^mean O-B thermal/, 'O−B 均值 K'], [/^mean O-A thermal/, 'O−A 均值 K'], [/^RMS O-A thermal/, 'O−A RMS K'],
  [/^analysis soil moisture/, '分析土壤水分'], [/^analysis Vcmax25/, '分析 Vcmax'], [/^analysis LAI$/, '分析 LAI'],
  [/^background soil moisture/, '背景土壤水分'], [/^background Vcmax25/, '背景 Vcmax'], [/^background LAI$/, '背景 LAI'],
  [/^analysis net radiation/, '分析净辐射 Rn'], [/^analysis sensible heat/, '分析显热 H'], [/^analysis latent heat/, '分析潜热 LE'], [/^analysis ground heat/, '分析土壤热通量 G']
]
// 模块：多波段合成方案。targets 为三个目标波长（nm），依次对应 R、G、B 通道。
const COMPOSITES = [
  { key: 'true', label: '真彩色', targets: [655, 560, 482] },
  { key: 'cir', label: '标准假彩色', targets: [865, 655, 560] },
  { key: 'swir', label: '短波红外合成', targets: [2200, 865, 655] }
]
// 模块：色带定义。stops 为等间距的颜色节点，buildLut() 把它们插值成 256 级查找表。
const COLORMAPS = {
  gray: { label: '灰度', stops: ['#000000', '#ffffff'] },
  viridis: { label: 'Viridis', stops: ['#440154', '#482878', '#3e4989', '#31688e', '#26828e', '#1f9e89', '#35b779', '#6ece58', '#b5de2b', '#fde725'] },
  inferno: { label: 'Inferno', stops: ['#000004', '#1b0c41', '#4a0c6b', '#781c6d', '#a52c60', '#cf4446', '#ed6925', '#fb9b06', '#f7d13d', '#fcffa4'] },
  turbo: { label: 'Turbo', stops: ['#30123b', '#4145ab', '#4675ed', '#39a2fc', '#1bcfd4', '#24eca6', '#61fc6c', '#a4fc3b', '#d1e834', '#f3c63a', '#fe9b2d', '#f36315', '#d93806', '#b11901', '#7a0403'] },
  rdylbu: { label: '冷暖', stops: ['#313695', '#4575b4', '#74add1', '#abd9e9', '#e0f3f8', '#ffffbf', '#fee090', '#fdae61', '#f46d43', '#d73027', '#a50026'] }
}
// 模块：显示拉伸方式（p2/p1 为按分位数截断的线性拉伸，manual 用手动上下限）。
const STRETCHES = { p2: '2% 线性', p1: '1% 线性', minmax: '最小–最大', manual: '手动' }

// 模块：表单输入字段的声明表。每项的 path 是表单对象里的点分路径；
// mode 为缺省取值方式（file=栅格文件，value=常数）；constant:false 表示只能用文件；
// band:true 表示可额外指定波段序号。几组字段合并为 ALL_FIELDS，
// 供 normalizeScene / exportScene 统一做规范化与导出。
const SURFACE_FIELDS = [
  { path: 'surface.type', label: '地表类型', unit: 'IGBP', required: true, constant: false, mode: 'file' },
  { path: 'surface.lai', label: '叶面积指数', unit: 'm²/m²', required: true, mode: 'file' },
  { path: 'surface.soilMoisture', label: '土壤水分', unit: 'm³/m³', placeholder: '留空取 0.25', mode: 'value' },
  { path: 'surface.dem', label: '高程', unit: 'm', placeholder: '留空取 0', mode: 'value' }
]
const LOCATION_FIELDS = [
  { path: 'surface.latitude', label: '纬度', unit: '°N', mode: 'file' },
  { path: 'surface.longitude', label: '经度', unit: '°E', mode: 'file' }
]
const GEOMETRY_FIELDS = [
  { path: 'view.vza', label: '观测天顶角', unit: '°', required: true, placeholder: '0–90', mode: 'value' },
  { path: 'view.vaa', label: '观测方位角', unit: '°', required: true, placeholder: '正北顺时针', mode: 'value' }
]
const COMPONENT_FIELDS = [
  { path: 'temperature.components.soilSunlit', label: '光照土壤 Tss', unit: 'K', required: true, band: true, mode: 'file' },
  { path: 'temperature.components.soilShaded', label: '阴影土壤 Tsh', unit: 'K', required: true, band: true, mode: 'file' },
  { path: 'temperature.components.leafSunlit', label: '光照叶片 Tcs', unit: 'K', required: true, band: true, mode: 'file' },
  { path: 'temperature.components.leafShaded', label: '阴影叶片 Tch', unit: 'K', required: true, band: true, mode: 'file' }
]
const PACKAGE_FIELD = { path: 'temperature.componentsPackage', label: '组分温度文件', unit: '4 波段 K', required: true, constant: false, mode: 'file' }
const ALL_FIELDS = [...SURFACE_FIELDS, ...LOCATION_FIELDS, ...GEOMETRY_FIELDS, ...COMPONENT_FIELDS, PACKAGE_FIELD]

// 模块：通用小工具（HTML 转义、点分路径读写、文件名/波段名解析）。
// wavelengthOf 从波段描述中提取形如“@ 665 nm”的中心波长，失败返回 NaN。
const escapeHtml = (value) => String(value ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]))
const presetBands = (preset) => preset.bands.map(([name, center, fwhm]) => ({ name, center, fwhm }))
const getPath = (object, path) => path.split('.').reduce((value, key) => value?.[key], object)
function setPath(object, path, value) {
  const keys = path.split('.')
  const last = keys.pop()
  keys.reduce((current, key) => (current[key] ??= {}), object)[last] = value
}
const fileName = (path) => String(path || '').split(/[\\/]/).pop()
const shortBand = (name) => String(name || '').split(/\s+/)[0]
const wavelengthOf = (name) => Number(String(name).match(/@\s*([0-9.]+)\s*nm/i)?.[1])
const bandLabel = (name) => {
  const text = String(name || '')
  const known = BAND_LABELS.find(([pattern]) => pattern.test(text))
  if (known) return known[1]
  return text.replace(/\s*\(K\)/, '').replace(/ reflectance/, ' 反射率').replace(/ brightness temperature/, ' 亮温').replace(/ surface temperature/, ' 方向温度')
}
const pad = (value) => String(value).padStart(2, '0')

// 作用：把任意保存值规范成 {mode, file, value, band} 的字段对象。
// 说明：已有文件路径则视为 file 模式，有数值则视为 value 模式，否则用字段缺省模式。
function normalizeField(value, mode) {
  const source = value && typeof value === 'object' ? value : {}
  const file = String(source.file ?? '')
  const hasValue = source.value !== undefined && source.value !== null && String(source.value) !== ''
  return { mode: source.mode || (file ? 'file' : hasValue ? 'value' : mode), file, value: hasValue ? source.value : '', band: source.band ?? '' }
}

// Local date + clock + UTC offset of the former single-scene form -> UTC overpass.
function localToUtc(date, time, timezone) {
  const day = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(date || ''))
  const clock = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(String(time || ''))
  if (!day || !clock) return ''
  const hasTimezone = timezone !== null && timezone !== undefined && String(timezone).trim() !== ''
  const offset = hasTimezone && Number.isFinite(Number(timezone)) ? Number(timezone) : 8
  const ms = Date.UTC(Number(day[1]), Number(day[2]) - 1, Number(day[3]), Number(clock[1]), Number(clock[2]), Number(clock[3] || 0)) - offset * 3600000
  return new Date(ms).toISOString().replace(/\.\d{3}Z$/, 'Z')
}

// 作用：把旧版“单场景表单”（有 inputs/time 字段、无 surface/overpass）迁移为当前表单结构。
// 说明：旧版热红外组分温度来自 thermal.components，迁移后按 RT 模式（temperature.mode='rt'）处理；
// 已含 surface 或 overpass 的新格式原样返回。
function migrateLegacy(scene) {
  if (!scene || typeof scene !== 'object' || scene.surface || scene.overpass) return scene
  const inputs = scene.inputs || {}
  return {
    name: scene.name, outputRoot: scene.outputRoot,
    overpass: localToUtc(scene.time?.date, scene.time?.time, scene.time?.timezone),
    displayTimezone: scene.time?.timezone ?? 8,
    surface: { type: inputs.type, lai: inputs.lai, soilMoisture: inputs.soilMoisture, dem: inputs.dem, latitude: inputs.latitude, longitude: inputs.longitude },
    locationSource: scene.locationSource, view: { vza: inputs.vza, vaa: inputs.vaa }, region: scene.region,
    optical: scene.optical, thermal: { enabled: scene.thermal?.enabled, preset: scene.thermal?.preset, bands: scene.thermal?.bands },
    temperature: { mode: 'rt', components: { source: 'fields', ...(scene.thermal?.components || {}) } }
  }
}

// 作用：生成表单的缺省值。缺省过境时刻为当天 03:00 UTC，显示时区 UTC+8；
// 缺省组分温度（K）：光照土壤 315、阴影土壤 303、光照叶片 305、阴影叶片 300。
function defaults() {
  const today = new Date().toISOString().slice(0, 10)
  return {
    name: 'image01', outputRoot: '', overpass: `${today}T03:00:00Z`, displayTimezone: 8, threads: 0,
    assimilation: defaultAssimilation(),
    surface: { type: {}, lai: {}, soilMoisture: {}, dem: {}, latitude: {}, longitude: {} },
    locationSource: 'georef',
    view: { vza: { value: 0 }, vaa: { value: 0 } },
    region: { enabled: false, startRow: 0, endRow: 1, startColumn: 0, endColumn: 1 },
    optical: { enabled: true, preset: 'landsat', bands: presetBands(OPTICAL_PRESETS.landsat) },
    thermal: { enabled: false, preset: 'landsat', bands: presetBands(THERMAL_PRESETS.landsat) },
    temperature: {
      mode: 'rt',
      components: { source: 'fields', soilSunlit: { value: 315 }, soilShaded: { value: 303 }, leafSunlit: { value: 305 }, leafShaded: { value: 300 } },
      componentsPackage: {}, era5: { pack: '' }, soilMoisture: { source: 'era5' }, elevationCorrection: true, spinupHours: DEFAULT_SPINUP_NODES
    }
  }
}

// 作用：把保存的草稿/工程（可能是旧版本）规范成完整表单，缺项用 defaults() 补齐。
// 步骤 1：旧格式迁移；步骤 2：逐层与缺省值合并；
// 步骤 3：旧的 temperature.components.file 转成组分温度文件字段；
// 步骤 4：过境时刻不合法则回退缺省，缺秒补“:00”；
// 步骤 5：统一规范化所有输入字段、波段列表与同化设置。
function normalizeScene(saved) {
  const base = defaults()
  const scene = migrateLegacy(saved && typeof saved === 'object' ? saved : {}) || {}
  const temperature = scene.temperature || {}
  const form = { ...base, ...scene,
    surface: { ...base.surface, ...scene.surface }, view: { ...base.view, ...scene.view },
    region: { ...base.region, ...scene.region }, optical: { ...base.optical, ...scene.optical }, thermal: { ...base.thermal, ...scene.thermal },
    temperature: { ...base.temperature, ...temperature, components: { ...base.temperature.components, ...temperature.components },
      era5: { ...base.temperature.era5, ...temperature.era5 }, soilMoisture: { ...base.temperature.soilMoisture, ...temperature.soilMoisture } } }
  if (!form.temperature.componentsPackage?.file && temperature.components?.file) form.temperature.componentsPackage = { file: temperature.components.file }
  delete form.temperature.components.file
  delete form.inputs; delete form.time
  if (!/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?Z$/.test(String(form.overpass || ''))) form.overpass = base.overpass
  else if (/T\d{2}:\d{2}Z$/.test(form.overpass)) form.overpass = form.overpass.replace(/Z$/, ':00Z')
  for (const field of ALL_FIELDS) setPath(form, field.path, normalizeField(getPath(form, field.path), field.mode))
  form.optical.bands = (form.optical.bands || []).map((band) => ({ ...band }))
  form.thermal.bands = (form.thermal.bands || []).map((band) => ({ ...band }))
  form.assimilation = normalizeAssimilation(scene.assimilation, scene.inversion)
  delete form.inversion
  return form
}

function defaultAssimilation() {
  return { mode: 'simulate', observation: 'brightness', data: '', optical: '', views: [] }
}

// 作用：把字段对象转成引擎配置的“来源对象”：有文件则 {file[, band]}，有有限数值则 {value}，否则 {}。
const sourceObject = (value) => {
  if (!value || typeof value !== 'object') return {}
  if (String(value.file || '').trim()) return { file: String(value.file).trim(), ...(Number(value.band) > 0 ? { band: Number(value.band) } : {}) }
  return value.value !== undefined && value.value !== null && String(value.value).trim() !== '' && Number.isFinite(Number(value.value)) ? { value: Number(value.value) } : {}
}

// Earlier versions had a separate LST/LSCT "温度反演" panel; a saved retrieval with one observation file becomes an
// assimilation of that file (LST product -> 地表温度, brightness -> 亮温). Everything else starts as 直接模拟.
function normalizeAssimilation(saved, legacyInversion) {
  const base = defaultAssimilation()
  if (saved && typeof saved === 'object' && (saved.mode === 'simulate' || saved.mode === 'assimilate')) {
    return {
      mode: saved.mode,
      observation: Object.hasOwn(ASSIMILATION_OBSERVATIONS, saved.observation) ? saved.observation : base.observation,
      data: String(saved.data || '').trim(), optical: String(saved.optical || '').trim(),
      views: (Array.isArray(saved.views) ? saved.views : []).slice(0, 16).map((item) => ({
        data: String(item?.data || '').trim(), vza: sourceObject(item?.vza), vaa: sourceObject(item?.vaa)
      }))
    }
  }
  const legacy = legacyInversion && typeof legacyInversion === 'object' ? legacyInversion : null
  const first = Array.isArray(legacy?.observations) ? legacy.observations.find((item) => item?.value?.file) : null
  if (legacy && ['lst', 'lsct'].includes(legacy.target) && first)
    return { ...base, mode: 'assimilate', observation: first.kind === 'lst' ? 'lst' : 'brightness', data: String(first.value.file) }
  return base
}

// 作用：把表单字段导出为配置里的来源对象（file 模式输出 {file[, band]}，value 模式输出 {value}，空值输出 {}）。
function exportField(field) {
  if (field.mode === 'file') return field.file.trim() ? { file: field.file.trim(), ...(Number(field.band) > 0 ? { band: Number(field.band) } : {}) } : {}
  return field.value !== '' && field.value !== null ? { value: Number(field.value) } : {}
}

// 作用：把表单深拷贝并转成提交给服务端的场景配置；输入字段改为来源对象，
// 组分温度文件移到 temperature.components.file，并删除仅界面使用的 componentsPackage。
function exportScene(form) {
  const scene = structuredClone(form)
  for (const field of ALL_FIELDS) setPath(scene, field.path, exportField(getPath(form, field.path)))
  const packageFile = form.temperature.componentsPackage.file.trim()
  if (packageFile) scene.temperature.components.file = packageFile
  delete scene.temperature.componentsPackage
  // Empty = engine default (7 nodes); the engine/server reject anything outside 0–240 (validate() catches it first).
  if (scene.temperature.spinupHours === '' || scene.temperature.spinupHours === null || scene.temperature.spinupHours === undefined) delete scene.temperature.spinupHours
  return scene
}

// EB spin-up, the same rule as the engine (models/stream/app/image.cpp, 2026-11-24): temperature.spinupHours = number of
// whole-hour forcing nodes before the overpass (default 7, 0–240, non-integers round up). The march starts at the
// spinupHours-th whole hour counted backwards from the last whole hour that is more than 5 min before the overpass
// (never a fixed clock time); a pack that does not cover it starts at its earliest covered hour (warning only).
// 常量：EB 起转节点缺省数（个整点时次）与一小时的毫秒数。
const DEFAULT_SPINUP_NODES = 7
const HOUR_MS = 3600000

// '' / undefined -> default; returns NaN when the value is not a number in 0–240.
function spinupNodes(value) {
  if (value === '' || value === null || value === undefined) return DEFAULT_SPINUP_NODES
  const number = Number(value)
  if (!Number.isFinite(number) || number < 0 || number > 240) return NaN
  return Math.ceil(number - 1e-9)
}

// 作用：毫秒时间戳 → 不带毫秒的 ISO UTC 字符串。
const isoHour = (ms) => new Date(ms).toISOString().replace(/\.\d{3}Z$/, 'Z')

// 模块：EB 逐时推进（起转）计划，规则与引擎 models/stream/app/image.cpp 保持一致。
// 输入：overpass（ISO UTC 字符串）、nodesValue（起转节点数，空则用缺省 7）、pack（数据包索引，可缺）。
// 输出：null（输入非法）或对象，其中小时量均为“自 1970-01-01T00Z 起的整小时数”：
//   requested=要求的起转节点数；usable=过境前实际可用的整点节点数；nodeCount=usable+1（含过境节点）；
//   desiredStart=按要求应起算的整点；start=按数据包起点截断后的实际起点；
//   lastNodeHour=过境前最后一个整点（须早于过境时刻 5 分钟以上）；lastHour=引擎读取的最后一个整点；
//   firstHour=引擎读取的第一个整点（ERA5-Land 需再早一小时）；marchStart=起算时刻（ms）；
//   startsLate=数据包不够长而被迫晚起算。
// 说明：起点按数据包覆盖截断，与引擎 image.cpp 步骤 3c 一致：起点之前（ERA5-Land 再早一小时）至 lastHour 必须被数据包
//   连续覆盖，否则起点逐小时后移；整年数据包（yearEdge：fullYear 且年份等于过境年）也一样，因为引擎的
//   pack.covers 是严格检查，“年边界外沿用最近一小时”只在后面步骤 3d 载入数据时生效。yearEdge 只影响两件事：
//   ① 起点后移到过境小时后“无可用时次”不报错（packProblems 跳过）；② 数据包末端早于 lastHour（过境靠近年末）时
//   引擎的覆盖检查对任何起点都失败，起点退到过境整点。此前这里对 yearEdge 完全不截断，与引擎不一致，
//   靠近年初/年末的过境会多算出起转时次；已按引擎行为修正（仅影响界面计划与提示）。
// Plan of the EB march for an overpass and pack: requested / usable nodes, first node, hours the engine reads.
// `pack` may be missing (only the requested window is described). Hours are integer hours since the epoch.
function ebPlan(overpass, nodesValue, pack) {
  const time = Date.parse(overpass)
  const requested = spinupNodes(nodesValue)
  if (!Number.isFinite(time) || !Number.isFinite(requested)) return null
  const overpassHour = Math.floor(time / HOUR_MS)
  const lastNodeHour = Math.ceil((time - 300000) / HOUR_MS) - 1
  const lastHour = Math.ceil(time / HOUR_MS) + 1
  const land = pack?.product === 'era5-land'
  const desiredStart = requested > 0 ? lastNodeHour - (requested - 1) : overpassHour
  let start = Math.min(desiredStart, overpassHour)
  const yearEdge = Boolean(pack?.fullYear) && pack.year === new Date(time).getUTCFullYear()
  const packStart = pack?.period ? Math.floor(Date.parse(pack.period.start) / HOUR_MS) : null
  const packEnd = pack?.period ? Math.floor(Date.parse(pack.period.end) / HOUR_MS) : null
  if (packStart !== null) {
    // 引擎的覆盖检查要求 [起点−lead, lastHour] 全在包内；末端不够时任何起点都不满足，起点退到过境整点。
    if (yearEdge && packEnd !== null && packEnd < lastHour) start = overpassHour
    else start = Math.min(Math.max(start, packStart + (land ? 1 : 0)), overpassHour)
  }
  const usable = requested > 0 ? Math.max(0, lastNodeHour - start + 1) : 0
  const firstHour = start - (land ? 1 : 0)
  return { requested, usable, nodeCount: usable + 1, desiredStart, start, lastHour, firstHour, packStart, packEnd, yearEdge,
    marchStart: requested > 0 ? start * HOUR_MS : time, startsLate: requested > 0 && start > desiredStart }
}

// Blocking problems: the pack must cover the march start (or its fallback) through the hour after the overpass and must
// leave at least one whole hour before the overpass when spin-up is requested.
function packProblems(pack, overpass, moistureSource, nodesValue) {
  const problems = []
  if (!pack.complete) problems.push(`必需变量缺失或不连续：${[...pack.missingVariables, ...pack.incompleteVariables].join('、')}`)
  const plan = ebPlan(overpass, nodesValue, pack)
  if (plan && pack.period && !plan.yearEdge) {
    const label = (hour) => isoHour(hour * HOUR_MS).slice(0, 16).replace('T', ' ')
    if (plan.requested > 0 && plan.usable === 0 && plan.packStart <= plan.lastHour)
      problems.push(`数据包在过境时刻之前没有可用的整点时次（数据包 ${pack.period.start.slice(0, 16).replace('T', ' ')} – ${pack.period.end.slice(0, 16).replace('T', ' ')} UTC），无法起转；请补充过境前的逐小时数据，或把起转节点数设为 0`)
    else if (plan.packStart > plan.firstHour || plan.packEnd < plan.lastHour)
      problems.push(`数据包时段 ${pack.period.start.slice(0, 16).replace('T', ' ')} – ${pack.period.end.slice(0, 16).replace('T', ' ')} UTC 未覆盖本次所需的 ${label(plan.firstHour)} – ${label(plan.lastHour)} UTC`)
  }
  if (moistureSource === 'era5' && pack.kind !== 'stream-forcing-pack' && !pack.variables?.swvl1) problems.push('数据包没有 swvl1，土壤水分请改用地表输入')
  return problems
}

// Non-blocking: the pack starts later than the requested spin-up (the engine starts at the pack's earliest hour).
function packWarnings(pack, overpass, nodesValue) {
  const plan = ebPlan(overpass, nodesValue, pack)
  if (!plan || !plan.startsLate || plan.usable === 0) return []
  return [`数据包不覆盖要求的 ${plan.requested} 个起转时次，实际从 ${isoHour(plan.start * HOUR_MS).slice(0, 16).replace('T', ' ')} UTC 起算，只有 ${plan.usable} 个（土壤热历史偏短，建议补充更早的逐小时数据）`]
}
function packPeriodLabel(pack) {
  if (pack.fullYear) return `${pack.year} 整年`
  if (!pack.period) return String(pack.year)
  const start = pack.period.start.slice(0, 10), end = pack.period.end.slice(0, 10)
  return start === end ? `${start}（${pack.period.hours || ''} 时次）` : `${start} – ${end}`
}

// 作用：WGS84 的 UTM 平面坐标 → 经纬度（横轴墨卡托反算的级数展开，k0=0.9996）。
// 输入：x 东坐标（m，含 500000 假东）、y 北坐标（m，南半球含 10000000 假北）、带号 zone、south 是否南半球。
// 输出：[纬度°, 经度°]；中央经线 = (zone-1)*6-177°。用于由影像地理参考换算像元经纬度（光标读数）。
function utmToGeographic(x, y, zone, south) {
  const a = 6378137, f = 1 / 298.257223563, k0 = 0.9996
  const e2 = f * (2 - f), ep2 = e2 / (1 - e2)
  const e1 = (1 - Math.sqrt(1 - e2)) / (1 + Math.sqrt(1 - e2))
  const mu = (south ? y - 10000000 : y) / k0 / (a * (1 - e2 / 4 - 3 * e2 * e2 / 64 - 5 * e2 ** 3 / 256))
  const phi = mu + (3 * e1 / 2 - 27 * e1 ** 3 / 32) * Math.sin(2 * mu) + (21 * e1 * e1 / 16 - 55 * e1 ** 4 / 32) * Math.sin(4 * mu)
    + (151 * e1 ** 3 / 96) * Math.sin(6 * mu) + (1097 * e1 ** 4 / 512) * Math.sin(8 * mu)
  const sin = Math.sin(phi), cos = Math.cos(phi), tan = Math.tan(phi)
  const n = a / Math.sqrt(1 - e2 * sin * sin), t = tan * tan, c = ep2 * cos * cos
  const r = a * (1 - e2) / (1 - e2 * sin * sin) ** 1.5
  const d = (x - 500000) / (n * k0)
  const latitude = phi - (n * tan / r) * (d * d / 2 - (5 + 3 * t + 10 * c - 4 * c * c - 9 * ep2) * d ** 4 / 24
    + (61 + 90 * t + 298 * c + 45 * t * t - 252 * ep2 - 3 * c * c) * d ** 6 / 720)
  const longitude = (d - (1 + 2 * t + c) * d ** 3 / 6 + (5 - 2 * c + 28 * t - 3 * c * c + 8 * ep2 + 24 * t * t) * d ** 5 / 120) / cos
  return [latitude * 180 / Math.PI, (zone - 1) * 6 - 177 + longitude * 180 / Math.PI]
}

// GDAL without proj.db writes a citation ("WGS 84 / UTM zone 50N|...") instead of an EPSG code.
function utmZone(crs) {
  const text = String(crs || '')
  const code = Number(text.match(/EPSG:(\d+)/)?.[1])
  if (code > 32600 && code <= 32660) return { zone: code - 32600, south: false }
  if (code > 32700 && code <= 32760) return { zone: code - 32700, south: true }
  const named = text.match(/WGS\s*84\s*\/\s*UTM zone (\d+)\s*([NS])/i)
  return named ? { zone: Number(named[1]), south: named[2].toUpperCase() === 'S' } : null
}
const isGeographic = (crs) => /EPSG:4326/.test(String(crs || ''))
const crsLabel = (crs) => String(crs || '').split('|')[0].trim()

// 作用：EPSG:3857（Web Mercator）栅格上“投影米 → 地面米”的缩放系数 cos(纬度)，纬度取影像中心；其他坐标系返回 1。
// 说明：墨卡托 y（球半径 R = 6378137 m）对应纬度 φ = atan(sinh(y/R))，cos φ = 1/cosh(y/R)；
//   影像中心 y = originY − |pixelSizeY| × 原栅格高度 / 2（sourceHeight 来自服务端，缺省退回显示高度×step）。
function mercatorGroundScale(layer) {
  if (!/EPSG:3857/.test(layer?.crs || '')) return 1
  const height = Number(layer.sourceHeight) || (Number(layer.height) || 0) * (Number(layer.step) || 1)
  const centerY = Number(layer.originY) - Math.abs(Number(layer.pixelSizeY) || Number(layer.pixelSizeX) || 0) * height / 2
  const scale = 1 / Math.cosh(centerY / 6378137)
  return Number.isFinite(scale) && scale > 0 ? scale : 1
}

// 作用：由栅格坐标系和坐标 (x, y) 求 [纬度°, 经度°]；EPSG:4326 直接换序，WGS84/UTM 走反算，其他返回 null。
function geographic(crs, x, y) {
  if (isGeographic(crs)) return [y, x]
  const utm = utmZone(crs)
  return utm ? utmToGeographic(x, y, utm.zone, utm.south) : null
}

// 作用：把等间距的颜色节点（#rrggbb）线性插值为 256 级 RGB 查找表（长度 256×3）。
function buildLut(stops) {
  const colors = stops.map((hex) => [1, 3, 5].map((offset) => parseInt(hex.slice(offset, offset + 2), 16)))
  const lut = new Uint8ClampedArray(256 * 3)
  for (let index = 0; index < 256; index += 1) {
    const t = index / 255 * (colors.length - 1)
    const low = Math.min(colors.length - 2, Math.floor(t)), f = t - low
    for (let k = 0; k < 3; k += 1) lut[index * 3 + k] = colors[low][k] + (colors[low + 1][k] - colors[low][k]) * f
  }
  return lut
}
const LUTS = Object.fromEntries(Object.entries(COLORMAPS).map(([key, map]) => [key, buildLut(map.stops)]))

// 作用：取不超过 target 的“整齐”长度（1/2/5/10 × 10^n），用于比例尺。
function niceLength(target) {
  const power = 10 ** Math.floor(Math.log10(target))
  return [1, 2, 5, 10].map((step) => step * power).reverse().find((value) => value <= target) || power
}

// 作用：把 base64 编码的小端 float32 数组解码为 Float32Array；末尾不足 4 字节的残余丢弃。
function decodeFloat32(base64) {
  const binary = atob(base64 || '')
  const length = binary.length - (binary.length % 4)
  const bytes = new Uint8Array(length)
  for (let index = 0; index < length; index += 1) bytes[index] = binary.charCodeAt(index)
  return new Float32Array(bytes.buffer)
}

// 作用：读取本地保存的表单草稿（新键优先，其次旧键），解析失败则返回缺省表单。
function loadDraft() {
  try {
    const saved = appStorage.getItem(STORAGE_KEY) ?? LEGACY_STORAGE_KEYS.map((key) => appStorage.getItem(key)).find(Boolean)
    return normalizeScene(JSON.parse(saved || 'null'))
  } catch { return normalizeScene(null) }
}

// 模块：STREAM 遥感影像模拟工作台的入口。
// 输入：toast（消息提示回调）、onClose（返回三维场景工作台的回调）；输出：{ open, openProject, close, command, isOpen }。
// 说明：工作台的状态（表单、工程、运行任务、栅格缓存、显示参数）都封装在本闭包内，
// 界面以 innerHTML 重绘 + 事件委托的方式更新，不依赖框架。
export function createStreamWorkspace({ toast, onClose = () => {} }) {
  const api = streamApi
  const root = document.getElementById('streamWorkspace')
  const $ = (selector) => root.querySelector(selector)
  // 状态：form=当前表单（初值为本地草稿）；project=当前工程 {path,name,description}；dirty=有未保存修改；
  // editRevision=每次编辑加 1，保存时据此判断保存期间表单是否又被改动；
  // projectGeneration=切换/新建/打开工程时加 1，jobGeneration=每次提交运行或切换工程时加 1，
  // 异步回调靠它们判断结果是否已过期（见各处 isCurrent）；
  // running/starting=任务运行中/正在提交；currentJob=当前任务 id（'pending' 表示请求尚未返回）；
  // pendingEvents=任务 id 尚未确定时先缓存的事件；packs=已登记的气象数据包；
  // inspections=栅格探测结果（路径 → 信息）；rasterCache=已读取的结果栅格缓存；view=影像查看器状态。
  let form = loadDraft()
  let project = (() => { try { return JSON.parse(appStorage.getItem(PROJECT_KEY) || 'null') } catch { return null } })()
  let dirty = false
  let editRevision = 0
  let projectGeneration = 0
  let jobGeneration = 0
  let rendered = false
  let globalBound = false
  let status = null
  let running = false
  let starting = false
  let run = null
  let packs = []
  const forcing = createForcingPanel({ api, getForm: () => form, getProject: () => project, getPack: () => packs.find((item) => item.path === form.temperature.era5.pack), markDirty: () => markDirty(), root })
  let currentJob = null
  let currentJobOutput = null
  let pendingEvents = []
  let unsubscribe = null
  let inspections = new Map()
  let inspectTimer = null
  let inspectToken = 0
  let classStats = null
  let logCount = 0
  const diagnostics = new Map()
  const rasterCache = new Map()
  const view = { product: '', display: '', colormap: 'gray', stretch: 'p2', manual: null, celsius: false,
    interpolation: 'smooth', layers: [], image: null, scale: 1, x: 0, y: 0, fitted: '', typeLayer: null, token: 0 }

  // 模块：整体布局（顶部栏 / 左侧参数栏 / 中间影像与日志 / 右侧运行与统计）。
  // ---------- layout ----------
  // 作用：一次性写入工作台的 HTML 骨架（含气象强迫数据包登记对话框），然后
  // bind() 绑定事件、renderParams() 填充左侧参数栏、renderHeader() 刷新标题与未保存标记。
  function render() {
    root.innerHTML = `
      <header class="sw-header">
        <div class="sw-heading">
          <div class="sw-brand"><div class="brandLogo" aria-label="RTLab STREAM"><span class="logoLab">RTLab</span><span class="logoSlash" aria-hidden="true"></span><span class="logoStream">STREAM</span></div></div>
          <button class="button ghost sw-back" type="button" data-action="back" title="返回三维场景工作台"><svg><use href="#i-chevron"/></svg>三维场景</button>
          <div class="sw-title"><h1>遥感影像模拟 · STREAM</h1><p id="swProject"></p></div>
          <span class="unsaved" id="swDirty" hidden>未保存</span>
        </div>
        <div class="sw-actions">
          <span class="sw-engine" id="swEngine"><i></i><span>正在检测引擎</span></span>
          <button class="button ghost" type="button" data-action="era5" title="登记与管理逐时气象强迫数据"><svg><use href="#i-layers"/></svg>气象强迫数据</button>
          <button class="button primary" type="button" data-action="run" id="swRun" title="F5"><svg><use href="#i-play"/></svg><span id="swRunLabel">运行模拟</span></button>
          <button class="button danger" type="button" data-action="stop" id="swStop" disabled><svg><use href="#i-stop"/></svg>停止</button>
        </div>
      </header>
      <div class="sw-body">
        <aside class="sw-params" id="swParams" aria-label="模拟参数"></aside>
        <section class="sw-stage">
          <div class="sw-toolbar">
            <label class="sw-tool"><span>结果</span><select class="select" id="swProduct" disabled><option>暂无结果</option></select></label>
            <label class="sw-tool"><span>显示</span><select class="select" id="swDisplay" disabled></select></label>
            <label class="sw-tool"><span>色带</span><select class="select" id="swColormap">${Object.entries(COLORMAPS).map(([key, map]) => `<option value="${key}">${map.label}</option>`).join('')}</select></label>
            <label class="sw-tool"><span>拉伸</span><select class="select" id="swStretch">${Object.entries(STRETCHES).map(([key, label]) => `<option value="${key}">${label}</option>`).join('')}</select></label>
            <div class="segmented sw-interp" id="swInterp" title="影像重采样方式"><button type="button" data-interp="smooth" class="active">平滑</button><button type="button" data-interp="pixel">像素</button></div>
            <div class="segmented sw-unit" id="swUnit" hidden><button type="button" data-unit="K" class="active">K</button><button type="button" data-unit="C">°C</button></div>
            <div class="sw-tool-spacer"></div>
            <button class="icon-button" type="button" data-action="fullscreen" title="全屏查看影像"><svg><use href="#i-expand"/></svg></button>
            <button class="icon-button" type="button" data-action="fit" title="适应窗口"><svg><use href="#i-fit"/></svg></button>
            <button class="button ghost sw-one" type="button" data-action="actual" title="一个屏幕像素对应一个栅格像元">1:1</button>
          </div>
          <div class="sw-map" id="swMap">
            <canvas id="swCanvas"></canvas>
            <div class="sw-empty" id="swEmpty"><strong>还没有结果影像</strong><span>在左侧设置地表输入、过境时刻、温度来源和传感器波段，然后点击“运行模拟”；以前的结果可在输出目录中查看。</span></div>
            <div class="sw-overlay sw-legend" id="swLegend" hidden></div>
            <div class="sw-overlay sw-scale" id="swScale" hidden><i></i><span></span></div>
            <div class="sw-overlay sw-north" id="swNorth" hidden title="北向（栅格为北向上）"><svg viewBox="0 0 20 28"><path d="M10 2 L17 24 L10 19 L3 24 Z"/></svg><span>N</span></div>
          </div>
          <div class="sw-readout" id="swReadout"><span class="sw-muted">将光标移到影像上查看像元值；滚轮缩放，拖动平移。</span></div>
          <div class="sw-log">
            <div class="sw-log-head"><strong>运行日志</strong><span id="swLogCount">0</span><button class="text-button" type="button" data-action="clear-log">清空</button></div>
            <div class="sw-log-body" id="swLog" role="log"></div>
          </div>
        </section>
        <aside class="sw-results" aria-label="运行与统计">
          <section class="sw-card">
            <h3>运行</h3>
            <div class="sw-run-state" id="swRunState" data-state="idle"><i></i><strong>尚未运行</strong><span></span></div>
            <div class="sw-progress"><i id="swProgress"></i></div>
            <div class="sw-run-dir"><span id="swRunDir" title="">—</span><button class="icon-button" type="button" data-action="open-output" title="打开输出目录" disabled><svg><use href="#i-folder"/></svg></button></div>
          </section>
          <section class="sw-card"><h3>像元</h3><dl class="sw-counts" id="swCounts"><div><dt>暂无</dt><dd></dd></div></dl></section>
          <section class="sw-card"><h3>拉伸与直方图</h3><canvas class="sw-histogram" id="swHistogram" width="300" height="76"></canvas>
            <div class="sw-manual" id="swManual"><label>下限<input class="input" type="number" step="any" id="swMin"></label><label>上限<input class="input" type="number" step="any" id="swMax"></label></div></section>
          <section class="sw-card"><h3>波段统计</h3><div class="sw-table-wrap"><table class="sw-table" id="swBandStats"></table></div></section>
          <section class="sw-card sw-grow"><h3>分地类统计</h3><p class="sw-note" id="swClassNote">运行后按 IGBP 地类统计当前显示波段。</p><div class="sw-table-wrap"><table class="sw-table" id="swClassStats"></table></div></section>
        </aside>
      </div>
      <div class="sw-modal" id="swEra5" hidden role="dialog" aria-label="气象强迫数据">
        <div class="sw-modal-card">
          <header class="sw-modal-head"><h2>气象强迫数据</h2><button class="icon-button" type="button" data-action="era5-close" title="关闭">×</button></header>
          <p class="sw-hint">数据包是一组逐小时气象强迫单层 NetCDF，以 ERA5 / ERA5-Land 为用例（变量名按 ERA5 约定），其他逐时气象强迫（如 GLDAS）按同样的变量名与单位整理后也可使用；可只覆盖一天，也可覆盖整年（可为多个文件）；空间范围只需盖住影像并外扩约 2 个格点。EB 读取起转起点（过境前第 N 个整点时次，缺省 N = 7；ERA5-Land 另需再早一小时）到过境后 1 小时；数据包不含起点时从其最早时次起算并给出提示，但过境时刻之前至少要有 1 个整点时次。土壤（光照/阴影）、叶片与土壤热历史的初温都取起点时刻的 2 m 气温 t2m，不使用 skt、stl1、stl2。必需变量：u10、v10、t2m、d2m、sp、ssrd、strd；可选：skt（地类含水体时用作水面温度，缺省时水体改取气温并提示）、swvl1（土壤水分取气象强迫时必需）、z（按 DEM 订正气温气压）。登记只读取时间轴，生成索引，不复制数据。</p>
          <div class="sw-era5-list" id="swEra5List"></div>
          <div class="sw-era5-new">
            <label><span>名称</span><input class="input" id="swEra5Label" maxlength="64" placeholder="例如 华北 2019"></label>
            <label><span>产品</span><select class="select" id="swEra5Product"><option value="auto">按网格自动识别</option><option value="era5">ERA5（0.25°）</option><option value="era5-land">ERA5-Land（0.1°）</option><option value="gldas-noah-0.25">GLDAS Noah 0.25°（3 小时）</option><option value="gldas-noah-1.0">GLDAS Noah 1°（3 小时）</option><option value="current">按当前面板里的元数据（自定义网格数据）</option></select></label>
            <button class="button primary" type="button" data-action="era5-register">选择 NetCDF 文件并登记…</button>
          </div>
          <p class="sw-field-info" id="swEra5Status"></p>
        </div>
      </div>`
    rendered = true
    bind()
    renderParams()
    renderHeader()
  }

  // 作用：生成一个输入字段行。constant!==false 的字段可在“栅格 / 常数”间切换，
  // 否则只能填 GeoTIFF 路径；spec.band 为真时附带波段序号输入。输入：字段声明 spec（见 SURFACE_FIELDS）。
  function fieldRow(spec) {
    const value = getPath(form, spec.path)
    const constant = spec.constant !== false
    return `<div class="sw-field" data-field="${spec.path}" data-mode="${constant ? value.mode : 'file'}">
      <div class="sw-field-head">
        <label for="f-${spec.path}">${escapeHtml(spec.label)}${spec.required ? '<b title="必填">*</b>' : ''}<small>${escapeHtml(spec.unit || '')}</small></label>
        ${constant ? `<div class="sw-seg" role="group" aria-label="${escapeHtml(spec.label)}来源"><button type="button" data-mode="file">栅格</button><button type="button" data-mode="value">常数</button></div>` : ''}
      </div>
      <div class="sw-field-file${spec.band ? ' with-band' : ''}">
        <input class="input" id="f-${spec.path}" data-role="file" value="${escapeHtml(value.file)}" placeholder="GeoTIFF 绝对路径" spellcheck="false">
        ${spec.band ? `<input class="input" data-role="band" type="number" min="0" step="1" value="${escapeHtml(value.band)}" placeholder="波段" title="温度波段序号；留空时单波段取第 1 波段，24 波段按 UTC 小时自动选择">` : ''}
        <button class="icon-button" type="button" data-role="browse" title="选择文件"><svg><use href="#i-folder"/></svg></button>
      </div>
      ${constant ? `<div class="sw-field-value"><input class="input" data-role="value" type="number" step="any" value="${escapeHtml(value.value)}" placeholder="${escapeHtml(spec.placeholder || '')}"><span>${escapeHtml(spec.unit || '')}</span></div>` : ''}
      <p class="sw-field-info" data-role="info"></p>
    </div>`
  }

  // 作用：生成光学（nm）或热红外（μm）波段表：名称、中心波长、半高宽，可逐行删除。
  function bandTable(kind) {
    const unit = kind === 'optical' ? 'nm' : 'μm'
    return `<table class="sw-bands"><thead><tr><th>波段</th><th>中心 ${unit}</th><th>半高宽 ${unit}</th><th></th></tr></thead>
      <tbody data-bands="${kind}">${form[kind].bands.map((band, index) => `<tr data-index="${index}">
        <td><input class="input" data-band="name" value="${escapeHtml(band.name)}" maxlength="40"></td>
        <td><input class="input" data-band="center" type="number" step="any" value="${escapeHtml(band.center)}"></td>
        <td><input class="input" data-band="fwhm" type="number" min="0" step="any" value="${escapeHtml(band.fwhm)}"></td>
        <td><button class="icon-button" type="button" data-remove-band title="删除波段">×</button></td></tr>`).join('')}</tbody></table>`
  }

  // 作用：生成传感器预设下拉框，当前选中项取 form[kind].preset，末项为“自定义”。
  function presetSelect(kind) {
    const presets = kind === 'optical' ? OPTICAL_PRESETS : THERMAL_PRESETS
    return `<select class="select" data-preset="${kind}">${Object.entries(presets).map(([key, preset]) => `<option value="${key}"${key === form[kind].preset ? ' selected' : ''}>${escapeHtml(preset.label)}</option>`).join('')}<option value="custom"${form[kind].preset === 'custom' ? ' selected' : ''}>自定义</option></select>`
  }

  // 作用：生成一个可折叠的参数分组；toggle 非空时在标题栏放启用开关（绑定到该表单路径，如 optical.enabled）。
  function group(id, title, body, { toggle = '', open = true } = {}) {
    return `<section class="sw-group${open ? ' open' : ''}" data-group="${id}">
      <div class="sw-group-head">
        <button type="button" class="sw-group-toggle" aria-expanded="${open}"><svg><use href="#i-chevron"/></svg>${title}</button>
        <span class="sw-group-summary" data-summary="${id}"></span>
        ${toggle ? `<label class="sw-switch" title="启用"><input type="checkbox" data-path="${toggle}"${getPath(form, toggle) ? ' checked' : ''}><i></i></label>` : ''}
      </div>
      <div class="sw-group-body">${body}</div>
    </section>`
  }

  // 模块：运行方式（直接模拟 / 同化）。同化模式下选择观测类型（亮温/地表温度/组分温度）与观测文件，
  // 亮温同化可再附加其他观测视角（每个视角一个亮温 GeoTIFF 加天顶角、方位角，最多 16 个）。
  // ---------- run mode: 直接模拟 / 同化 ----------
  // Only the data and what they are is asked for; observation / background errors, iteration and the controls
  // (RT: four component temperatures, EB: soil moisture + Vcmax, + LAI with reflectances) are engine defaults.
  // 作用：生成第 index 个附加观测视角的编辑卡片（亮温文件、天顶角 °、方位角 °；后两者可填常数或栅格）。
  function viewCard(item, index) {
    const text = (name, value, placeholder) => `<input class="input" data-view-field="${name}" value="${escapeHtml(value)}" placeholder="${escapeHtml(placeholder)}" spellcheck="false">`
    return `<div class="sw-obs" data-view="${index}">
      <div class="sw-obs-head"><span class="sw-obs-title">附加视角 ${index + 1}</span><button class="icon-button" type="button" data-remove-view title="删除视角">×</button></div>
      <div class="sw-path">${text('data', item.data || '', '该视角亮温 GeoTIFF（波段同热红外波段）')}<button class="icon-button" type="button" data-view-browse title="选择文件"><svg><use href="#i-folder"/></svg></button></div>
      <div class="sw-row-4">
        <label><span>天顶角</span>${text('vza', sourceText(item.vza), '常数（°）或栅格')}</label>
        <label><span>方位角</span>${text('vaa', sourceText(item.vaa), '常数（°）或栅格')}</label>
      </div>
    </div>`
  }

  // 作用：生成“运行方式”分组：模式切换、观测类型、观测数据与可选光学反射率路径、附加视角列表。
  function assimilationGroup() {
    const as = form.assimilation
    const modeButton = (value, label) => `<button type="button" data-run-mode="${value}"${as.mode === value ? ' class="active"' : ''}>${label}</button>`
    const kindButton = (value, label) => `<button type="button" data-assim-observation="${value}"${as.observation === value ? ' class="active"' : ''}>${label}</button>`
    const path = (key, placeholder) => `<div class="sw-path"><input class="input" data-assim="${key}" value="${escapeHtml(as[key] || '')}" placeholder="${escapeHtml(placeholder)}" spellcheck="false"><button class="icon-button" type="button" data-assim-browse="${key}" title="选择文件"><svg><use href="#i-folder"/></svg></button></div>`
    return group('assimilation', '运行方式', `
      <div class="segmented sw-block-seg">${Object.entries(RUN_MODES).map(([key, label]) => modeButton(key, label)).join('')}</div>
      <div id="swAssimBody"${as.mode === 'assimilate' ? '' : ' hidden'}>
        <div class="segmented sw-block-seg sw-seg-3">${Object.entries(ASSIMILATION_OBSERVATIONS).map(([key, label]) => kindButton(key, label)).join('')}</div>
        <label class="sw-row"><span>观测数据</span>${path('data', '')}</label>
        <label class="sw-row"><span>光学反射率</span>${path('optical', '可选；给出时同时更新 LAI')}</label>
        <div id="swAssimViews"${as.observation === 'brightness' ? '' : ' hidden'}>
          <div class="sw-obs-list" id="swViewList">${as.views.map(viewCard).join('')}</div>
          <div class="sw-obs-actions"><button class="button ghost sw-add" type="button" data-add-view><svg><use href="#i-plus"/></svg>添加其他视角</button></div>
        </div>
        <p class="sw-hint" id="swAssimHint"></p>
      </div>
      <p class="sw-hint" id="swAssimSummary"></p>`)
  }

  function rerenderViews() {
    const list = $('#swViewList')
    if (list) list.innerHTML = form.assimilation.views.map(viewCard).join('')
  }

  const assimilating = () => form.assimilation.mode === 'assimilate'

  // 作用：按当前表单刷新运行方式相关的界面状态（不重绘整个参数栏）。
  // 步骤 1：显隐同化面板与附加视角；步骤 2：按观测类型改写观测文件框的占位提示；
  // 步骤 3：生成同化摘要说明（背景场为 EB 或 RT；更新哪些控制变量）；
  // 步骤 4：改写温度分组标题与运行按钮文案，并按引擎/数据是否就绪启停按钮。
  function syncAssimilationUI() {
    const summary = $('#swAssimSummary')
    const button = $('#swRun')
    const label = $('#swRunLabel')
    if (!summary || !button || !label) return
    const as = form.assimilation
    const eb = form.temperature.mode === 'eb'
    const on = assimilating()
    $('#swAssimBody').hidden = !on
    $('#swAssimViews').hidden = as.observation !== 'brightness'
    for (const item of root.querySelectorAll('[data-run-mode]')) item.classList.toggle('active', item.dataset.runMode === as.mode)
    for (const item of root.querySelectorAll('[data-assim-observation]')) item.classList.toggle('active', item.dataset.assimObservation === as.observation)
    const dataInput = $('[data-assim="data"]')
    if (dataInput) dataInput.placeholder = as.observation === 'brightness'
      ? `亮温 GeoTIFF，${form.thermal.bands.length || 'N'} 个波段，依次对应热红外波段`
      : as.observation === 'lst' ? '地表温度 GeoTIFF，1 个波段（K）' : '组分温度 GeoTIFF，4 个波段 Tss/Tsh/Tcs/Tch（K）'
    const hint = $('#swAssimHint')
    if (on) {
      const controls = eb ? '土壤水分与 Vcmax（组分温度由能量平衡给出）' : '四组分温度 Tss/Tsh/Tcs/Tch'
      summary.textContent = `同化${ASSIMILATION_OBSERVATIONS[as.observation]}：以${eb ? 'EB 能量平衡' : 'RT 给定组分温度'}为背景，更新${controls}${as.optical ? ' 与 LAI' : ''}；输出 analysis_* 与 background_*，模拟影像按分析场生成。`
      hint.textContent = '观测为 NaN 的像元视为缺测。观测误差、背景误差与迭代参数使用内置默认值；雅可比由模型内置的可微算子精确给出。'
    } else summary.textContent = '直接模拟：按温度来源正向生成光学与热红外影像。'
    const temperatureTitle = root.querySelector('[data-group="temperature"] .sw-group-toggle')
    if (temperatureTitle) temperatureTitle.lastChild.textContent = on ? '背景场' : '温度来源'
    label.textContent = on ? '运行同化' : '运行模拟'
    button.title = 'F5'
    button.disabled = running || !(status?.executableExists && status?.dataReady)
    const element = $('[data-summary="assimilation"]')
    if (element) element.textContent = on ? `同化 · ${ASSIMILATION_OBSERVATIONS[as.observation]}` : '直接模拟'
  }

  // 作用：处理运行方式面板内的 input 事件（观测路径、视角字段）；返回值表示事件是否已被本面板消费。
  // Returns true when the event belonged to the run-mode panel.
  function onAssimilationInput(event) {
    const target = event.target
    if (target.dataset.assim) {
      form.assimilation[target.dataset.assim] = target.value.trim()
      markDirty()
      syncAssimilationUI()
      return true
    }
    const card = target.closest('[data-view]')
    if (!card || !target.dataset.viewField) return false
    const item = form.assimilation.views[Number(card.dataset.view)]
    const field = target.dataset.viewField
    item[field] = field === 'data' ? target.value.trim() : parseSource(target.value)
    markDirty()
    return true
  }

  // 作用：处理运行方式面板内的点击（切换模式/观测类型、添加或删除视角、浏览文件）；返回值表示是否已消费。
  // 说明：附加视角上限 16 个，与 normalizeAssimilation 的截断一致。
  async function onAssimilationClick(button) {
    if (button.dataset.runMode) {
      form.assimilation.mode = button.dataset.runMode
      // Brightness assimilation reads one band per thermal band, so the thermal bands must be active.
      if (assimilating() && form.assimilation.observation === 'brightness' && !form.thermal.enabled) enableThermal()
      markDirty()
      syncAssimilationUI()
      return true
    }
    if (button.dataset.assimObservation) {
      form.assimilation.observation = button.dataset.assimObservation
      if (form.assimilation.observation === 'brightness' && !form.thermal.enabled) enableThermal()
      markDirty()
      syncAssimilationUI()
      return true
    }
    if (button.hasAttribute('data-add-view')) {
      if (form.assimilation.views.length >= 16) { toast('视角过多', '附加观测视角不能超过 16 个', 'error'); return true }
      form.assimilation.views.push({ data: '', vza: {}, vaa: {} })
      rerenderViews()
      markDirty()
      return true
    }
    if (button.hasAttribute('data-remove-view')) {
      form.assimilation.views.splice(Number(button.closest('[data-view]').dataset.view), 1)
      rerenderViews()
      markDirty()
      return true
    }
    if (button.dataset.assimBrowse || button.hasAttribute('data-view-browse')) {
      try {
        const result = await api.pick('file')
        if (!result?.path) return true
        const input = button.dataset.assimBrowse
          ? $(`[data-assim="${button.dataset.assimBrowse}"]`)
          : button.closest('[data-view]').querySelector('[data-view-field="data"]')
        input.value = result.path
        input.dispatchEvent(new Event('input', { bubbles: true }))
      } catch (error) { toast('无法打开选择窗口', error.message, 'error') }
      return true
    }
    return false
  }

  function enableThermal() {
    form.thermal.enabled = true
    const toggle = root.querySelector('[data-path="thermal.enabled"]')
    if (toggle) { toggle.checked = true; toggle.dispatchEvent(new Event('change', { bubbles: true })) }
  }
  // 作用：把表单中的 ISO UTC 过境时刻拆成日期（YYYY-MM-DD）和时间（HH:MM:SS，缺秒补 :00），供日期/时间输入框使用。
  function overpassParts() {
    const text = String(form.overpass || '')
    return { date: text.slice(0, 10), time: text.slice(11, 19).padEnd(8, ':00').slice(0, 8) }
  }

  // 作用：按“显示时区”（小时，可为小数）把 UTC 过境时刻换成当地时间字符串，仅用于界面提示。
  function localOverpass() {
    const time = Date.parse(form.overpass)
    if (!Number.isFinite(time)) return ''
    const offset = Number(form.displayTimezone) || 0
    const local = new Date(time + offset * 3600000)
    return `${local.getUTCFullYear()}-${pad(local.getUTCMonth() + 1)}-${pad(local.getUTCDate())} ${pad(local.getUTCHours())}:${pad(local.getUTCMinutes())}:${pad(local.getUTCSeconds())}`
  }

  // 作用：生成气象数据包下拉选项；已选但索引读不到的包保留一个“无法读取”选项，避免静默丢失。
  function packOptions() {
    const selected = form.temperature.era5.pack
    const known = packs.some((pack) => pack.path === selected)
    return `<option value="">${packs.length ? '选择气象数据包…' : '尚未登记气象数据包'}</option>` +
      packs.map((pack) => `<option value="${escapeHtml(pack.path)}"${pack.path === selected ? ' selected' : ''}>${escapeHtml(pack.label)}（${escapeHtml(String(pack.product || '').toUpperCase())} ${escapeHtml(packPeriodLabel(pack))}${pack.external ? '，工程自带' : ''}${pack.complete ? '' : '，不可用'}）</option>`).join('') +
      (selected && !known ? `<option value="${escapeHtml(selected)}" selected>${escapeHtml(fileName(selected))}（无法读取）</option>` : '')
  }

  // 作用：生成“温度来源”分组：RT 模式（四组分分别输入，或 4 波段组分温度文件）与
  // EB 模式（气象强迫元数据、数据包、土壤水分来源、DEM 订正、起转节点数）。
  function temperatureGroup() {
    const t = form.temperature
    const segment = (attribute, value, label, current) => `<button type="button" data-${attribute}="${value}"${current === value ? ' class="active"' : ''}>${label}</button>`
    return group('temperature', '温度来源', `
      <div id="swRt"${t.mode === 'rt' ? '' : ' hidden'}>
        <div class="segmented sw-block-seg">
          ${segment('components-source', 'fields', '分别输入四组分', t.components.source)}
          ${segment('components-source', 'package', '组分温度文件', t.components.source)}
        </div>
        <div id="swComponentFields"${t.components.source === 'package' ? ' hidden' : ''}>${COMPONENT_FIELDS.map(fieldRow).join('')}
          <p class="sw-hint">单位 K。单波段文件取第 1 波段；24 波段逐小时文件按过境时刻的 UTC 小时自动选波段。</p></div>
        <div id="swComponentPackage"${t.components.source === 'package' ? '' : ' hidden'}>${fieldRow(PACKAGE_FIELD)}
          <p class="sw-hint">4 波段 GeoTIFF，依次为 Tss、Tsh、Tcs、Tch（K），例如 EB 运行输出的 components.tif。</p></div>
        <p class="sw-hint">组分温度只在启用热红外波段时使用。</p>
      </div>
      <div id="swEb"${t.mode === 'eb' ? '' : ' hidden'}>
        ${forcing.html()}
        <label class="sw-row" data-forcing-pack-row><span>气象数据包</span><div class="sw-path"><select class="select" data-era5-pack>${packOptions()}</select><button class="icon-button" type="button" data-action="era5" title="登记与管理气象强迫数据"><svg><use href="#i-layers"/></svg></button></div></label>
        <p class="sw-field-info" id="swEra5Info" data-forcing-pack-row></p>
        <div class="sw-row"><span>土壤水分</span><div class="segmented">
          ${segment('moisture-source', 'era5', '气象强迫 swvl1', t.soilMoisture.source)}
          ${segment('moisture-source', 'user', '地表输入', t.soilMoisture.source)}
        </div></div>
        <label class="sw-check"><input type="checkbox" data-path="temperature.elevationCorrection"${t.elevationCorrection ? ' checked' : ''}> 按 DEM 与气象强迫网格的地形高度差订正气温和气压（需提供 DEM，数据包含 z）</label>
        <label class="sw-row"><span>起转节点数</span><input class="input" data-path="temperature.spinupHours" data-number type="number" min="0" max="240" step="1" value="${escapeHtml(t.spinupHours)}" placeholder="缺省 ${DEFAULT_SPINUP_NODES}"></label>
        <p class="sw-hint">EB 从过境前第 N 个整点时次（逐时气象强迫）起推进到过境时刻，缺省 ${DEFAULT_SPINUP_NODES}，范围 0–240；与表面热历史保留的 7 h 对应，空气温度已对结果约束很强，不必起转太久。起点永远相对过境时刻往前数，不是固定的 00 UTC 或固定的 24 h；数据包不覆盖时从其最早时次起算并提示。</p>
        <p class="sw-hint" id="swEbWindow"></p>
        <p class="sw-field-info" id="swEbWarn"></p>
      </div>`)
  }

  // 作用：重绘左侧参数栏，各分组顺序：模拟模式 → 运行方式 → 位置与过境时刻 → 观测几何 → 地表输入
  // → 温度来源 → 光学波段 → 热红外波段 → 计算范围；随后同步各分组状态与提示。
  function renderParams() {
    const { date, time } = overpassParts()
    // STREAM 工程独立：所有输入只属于本影像模拟工程，不读取三维工程的 shared 段。
    const inputLabel = '<div class="sw-group-label"><span>影像模拟输入</span><small>独立工程</small></div>'
    const modeCards = `<div class="sw-mode-label">模拟模式<small>决定组分温度来源</small></div>
      <div class="sw-mode-cards">
        <button type="button" class="mode-card${form.temperature.mode === 'rt' ? ' active' : ''}" data-temperature-mode="rt">
          <svg><use href="#i-camera"/></svg><span>RT · 已知组分温度</span><small>直接给定 Tss / Tsh / Tls / Tlh</small>
        </button>
        <button type="button" class="mode-card${form.temperature.mode === 'eb' ? ' active' : ''}" data-temperature-mode="eb">
          <svg><use href="#i-sun"/></svg><span>EB · 气象强迫能量平衡</span><small>逐时次求解四组分温度</small>
        </button>
      </div>`
    $('#swParams').innerHTML = [
      modeCards,
      assimilationGroup(),
      inputLabel,
      group('location', '位置与过境时刻', `
        <div class="segmented sw-block-seg" data-location>
          <button type="button" data-location-source="georef"${form.locationSource !== 'input' ? ' class="active"' : ''}>由地表类型的地理参考换算</button>
          <button type="button" data-location-source="input"${form.locationSource === 'input' ? ' class="active"' : ''}>输入经纬度</button>
        </div>
        <div id="swLocationFields"${form.locationSource === 'input' ? '' : ' hidden'}>${LOCATION_FIELDS.map(fieldRow).join('')}</div>
        <div class="sw-row-3">
          <label><span>过境日期（UTC）</span><input class="input" data-overpass="date" type="date" value="${escapeHtml(date)}"></label>
          <label><span>过境时刻（UTC）</span><input class="input" data-overpass="time" type="time" step="1" value="${escapeHtml(time)}"></label>
          <label><span>显示时区</span><input class="input" data-path="displayTimezone" data-number type="number" min="-12" max="14" step="0.5" value="${escapeHtml(form.displayTimezone)}"></label>
        </div>
        <p class="sw-hint" id="swLocalTime"></p>
        <p class="sw-hint">一次模拟对应一个过境时刻。太阳位置按 NREL SPA 逐像元计算。</p>
        <p class="sw-hint sw-shared-note">经纬度、时区与过境时刻只属于本影像模拟工程。</p>`),
      group('geometry', '观测几何', GEOMETRY_FIELDS.map(fieldRow).join('') + '<p class="sw-hint">方位角从正北顺时针；相对方位角 = 观测方位角 − 太阳方位角。</p><p class="sw-hint sw-shared-note">观测天顶角/方位角只属于本影像模拟工程。</p>'),
      group('surface', '地表输入', SURFACE_FIELDS.map(fieldRow).join('') + '<p class="sw-hint">地表类型栅格决定模拟网格与坐标系，其余栅格须与之同网格。17 水体与 18/255 填充值不参与光学计算；城市（13）按不透水地面 + 绿地冠层的四组分处理。</p><p class="sw-hint sw-shared-note">高程 DEM 只用于本影像模拟工程。</p>'),
      temperatureGroup(),
      group('optical', '光学波段', `
        <label class="sw-row"><span>传感器</span>${presetSelect('optical')}</label>
        ${bandTable('optical')}
        <button class="button ghost sw-add" type="button" data-add-band="optical"><svg><use href="#i-plus"/></svg>添加波段</button>
        <p class="sw-hint">400–2400 nm，按高斯光谱响应与参考太阳光谱加权平均。</p>`, { toggle: 'optical.enabled' }),
      group('thermal', '热红外波段', `
        <label class="sw-row"><span>传感器</span>${presetSelect('thermal')}</label>
        ${bandTable('thermal')}
        <button class="button ghost sw-add" type="button" data-add-band="thermal"><svg><use href="#i-plus"/></svg>添加波段</button>
        <p class="sw-hint">由“温度来源”提供的组分温度合成亮温与方向温度。</p>`, { toggle: 'thermal.enabled' }),
      group('region', '计算范围', `
        <label class="sw-check"><input type="checkbox" data-path="region.enabled"${form.region.enabled ? ' checked' : ''}> 只计算部分行列</label>
        <div class="sw-row-4" id="swRegion"${form.region.enabled ? '' : ' hidden'}>
          ${[['startRow', '起始行'], ['endRow', '结束行'], ['startColumn', '起始列'], ['endColumn', '结束列']].map(([key, label]) => `<label><span>${label}</span><input class="input" data-path="region.${key}" data-number type="number" min="0" step="1" value="${escapeHtml(form.region[key])}"></label>`).join('')}
        </div>
        <label class="sw-row"><span>线程数</span><input class="input" data-path="threads" data-number type="number" min="0" max="256" step="1" value="${escapeHtml(form.threads)}" placeholder="0 = 全部"></label>
        <p class="sw-hint">行列号从 0 开始，终点不包含；范围外输出空值。线程数为 0 时使用全部 CPU；与三维模拟同时运行时可适当减少。</p>`, { open: form.region.enabled })
    ].join('')
    syncGroups()
    renderInspections()
    syncAssimilationUI()
    forcing.afterRender()
  }

  // 作用：表单变化后同步各派生界面，不重绘整个参数栏：分组启用状态、土壤水分行显隐、
  // 气象数据包提示与告警、起转窗口说明、当地时间提示、各分组标题栏的摘要文字。
  // 输入：当前 form、数据包列表 packs、栅格探测结果 inspections；无返回值。
  function syncGroups() {
    for (const kind of ['optical', 'thermal']) {
      $(`[data-group="${kind}"]`)?.classList.toggle('disabled', !form[kind].enabled)
    }
    const t = form.temperature
    const moistureRow = $('[data-field="surface.soilMoisture"]')
    if (moistureRow) moistureRow.hidden = t.mode === 'eb' && t.soilMoisture.source === 'era5'
    const pack = packs.find((item) => item.path === t.era5.pack)
    const plan = ebPlan(form.overpass, t.spinupHours, pack)
    const warnings = pack ? packWarnings(pack, form.overpass, t.spinupHours) : []
    const info = $('#swEra5Info')
    if (info) {
      const problems = !pack ? (t.era5.pack ? ['无法读取所选数据包索引'] : []) : packProblems(pack, form.overpass, t.soilMoisture.source, t.spinupHours)
      info.textContent = pack && pack.kind === 'stream-forcing-pack' ? [packPeriodLabel(pack), `${pack.files} 个文件`, pack.stepHours > 1 ? `记录间隔 ${pack.stepHours} h` : '', ...problems].filter(Boolean).join('；') : pack ? [packPeriodLabel(pack), `${pack.files} 个文件`, pack.variables?.skt ? '含 skt（水体用）' : '无 skt（水体改取气温）', pack.variables?.z ? '含 z' : '无 z（不做高程订正）', ...problems].join('；') : problems.join('；')
      info.dataset.level = problems.length ? 'error' : pack ? 'ok' : ''
    }
    const warning = $('#swEbWarn')
    if (warning) { warning.textContent = warnings.join('；'); warning.dataset.level = warnings.length ? 'warning' : '' }
    // 步骤：显示 EB 起转窗口说明（起点、时次数与引擎规则一致，数值来自 ebPlan）。
    const window = $('#swEbWindow')
    if (window) {
      const { time } = overpassParts()
      window.textContent = !plan ? (Number.isNaN(spinupNodes(t.spinupHours)) ? '起转节点数须在 0–240 之间' : '')
        : plan.requested === 0
          ? `不起转：只在过境时刻 ${time} UTC 求解 1 个时次。`
          : `起转 ${plan.usable} 个整点时次${plan.usable < plan.requested ? `（请求 ${plan.requested} 个）` : ''}：从 ${isoHour(plan.marchStart).slice(0, 16).replace('T', ' ')} UTC 推进到过境时刻 ${time} UTC，共 ${plan.nodeCount} 个时次（含过境时刻，与引擎一致）。土壤、叶片与土壤热历史的初温都取起点时刻的 2 m 气温；土壤热历史（7 h）与土柱由此自洽推进。`
    }
    const localTime = $('#swLocalTime')
    if (localTime) localTime.textContent = `显示时区 UTC${Number(form.displayTimezone) >= 0 ? '+' : ''}${Number(form.displayTimezone) || 0} 的当地时间：${localOverpass() || '—'}`
    // 步骤：刷新各分组标题栏右侧的一行摘要。
    const typeInfo = inspections.get(form.surface.type.file.trim())
    const summaries = {
      task: form.name || '',
      surface: typeInfo?.width ? `${typeInfo.width} × ${typeInfo.height}` : '',
      location: String(form.overpass || '').replace('T', ' ').replace(/:\d{2}Z$/, ' UTC'),
      geometry: `${form.view.vza.mode === 'value' ? `${form.view.vza.value || 0}°` : '栅格'} / ${form.view.vaa.mode === 'value' ? `${form.view.vaa.value || 0}°` : '栅格'}`,
      temperature: t.mode === 'eb' ? `EB · ${forcing.isStation() ? '站点 CSV' : pack ? pack.label : '未选气象数据包'}` : `RT · ${t.components.source === 'package' ? '组分温度文件' : '四组分'}`,
      optical: form.optical.enabled ? `${form.optical.bands.length} 个波段` : '未启用',
      thermal: form.thermal.enabled ? `${form.thermal.bands.length} 个波段` : '未启用',
      region: form.region.enabled ? `${form.region.startRow}–${form.region.endRow} 行` : '整景'
    }
    for (const [key, text] of Object.entries(summaries)) {
      const element = $(`[data-summary="${key}"]`)
      if (element) element.textContent = text
    }
    syncAssimilationUI()
  }

  // 作用：刷新顶部工程名、工程路径与“未保存”标记。
  function renderHeader() {
    if (!rendered) return
    const name = form.name || project?.name || '影像模拟工程'
    $('#swProject').textContent = project?.path ? `${name}　${project.path}` : `${name}（尚未保存为工程）`
    $('#swProject').title = project?.path || ''
    $('#swDirty').hidden = !dirty
  }

  // 模块：参数输入的事件处理、栅格探测与校验。
  // ---------- inputs ----------
  // 作用：表单被修改后的统一入口：标记未保存、递增 editRevision、把草稿写入本地存储并刷新派生界面。
  function markDirty() {
    dirty = true
    editRevision += 1
    appStorage.setItem(STORAGE_KEY, JSON.stringify(exportScene(form)))
    renderHeader()
    syncGroups()
  }

  // 作用：输入路径变化后延迟 350 ms 再探测栅格，合并连续输入。
  function scheduleInspect() {
    clearTimeout(inspectTimer)
    inspectTimer = setTimeout(inspectInputs, 350)
  }

  // 作用：向服务端探测所有已填写的栅格文件（尺寸、坐标系、波段数、数据类型），结果存入 inspections。
  // 说明：用 inspectToken 丢弃过期响应，避免慢请求覆盖新的结果；失败时清空探测结果而不报错。
  async function inspectInputs() {
    const token = ++inspectToken
    const paths = [...new Set(ALL_FIELDS.map((spec) => getPath(form, spec.path)).filter((field) => field.mode === 'file' && field.file.trim()).map((field) => field.file.trim()))]
    if (!paths.length) { inspections = new Map(); renderInspections(); return }
    try {
      const { rasters } = await api.inspect(paths)
      if (token !== inspectToken) return
      inspections = new Map(rasters.map((info, index) => [paths[index], info]))
    } catch {
      if (token !== inspectToken) return
      inspections = new Map()
    }
    renderInspections()
    syncGroups()
  }

  // 作用：把栅格探测结果转成界面提示 { text, level }，level 为 ''、ok、warning 或 error。
  // 输入：info 为该文件的探测结果；reference 为地表类型栅格的探测结果（要求其余栅格与之同网格）。
  function describeRaster(info, reference) {
    if (!info) return { text: '', level: '' }
    if (!info.exists) return { text: info.error || '文件不存在', level: 'error' }
    if (!info.width) return { text: info.error ? `无法读取：${info.error}` : `${info.format || '栅格'}，由引擎读取`, level: info.error ? 'warning' : '' }
    // DEFERRED[低] Bug: 只有 EPSG:4326 会显示为度，CGCS2000(4490)、NAD83(4269) 等其它地理坐标系的像元大小会被标成“m”；建议：服务端头部探测同时返回 geographic 标志或单位，前端据此显示 —— inspectRasters 的 readTiffHeader 目前只回 crs/pixelSize，需服务端补 unit/geographic 字段；前端自己维护 EPSG 地理坐标系映射表属于新增约定，本轮不改
    const size = info.pixelSizeX ? `，${Number(info.pixelSizeX.toPrecision(6))} ${/EPSG:4326/.test(info.crs) ? '°' : 'm'}` : ''
    const text = `${info.width} × ${info.height} 像元${size}，${info.crs || '无坐标系'}，${info.bands} 波段，${info.dataType}`
    if (reference?.width && (reference.width !== info.width || reference.height !== info.height)) {
      return { text: `网格 ${info.width} × ${info.height} 与地表类型 ${reference.width} × ${reference.height} 不一致`, level: 'error' }
    }
    return { text, level: 'ok' }
  }

  // 作用：把探测提示写入各输入字段行，并清除上一次校验留下的 invalid 标记。
  function renderInspections() {
    if (!rendered) return
    const reference = inspections.get(form.surface.type.file.trim())
    for (const spec of ALL_FIELDS) {
      const row = $(`[data-field="${spec.path}"]`)
      if (!row) continue
      const field = getPath(form, spec.path)
      const info = row.querySelector('[data-role="info"]')
      const result = field.mode === 'file' && field.file.trim() ? describeRaster(inspections.get(field.file.trim()), spec.path === 'surface.type' ? null : reference) : { text: '', level: '' }
      info.textContent = result.text
      info.dataset.level = result.level
      row.classList.remove('invalid')
    }
  }

  // 作用：左侧参数栏的 input 事件委托。按优先级依次分派：气象强迫面板 → 运行方式面板 →
  // 过境日期/时间 → 带 data-path 的通用字段 → 栅格/常数字段行 → 波段表单元格。
  // 说明：手改波段表会把对应传感器预设切到“自定义”。
  function onParamInput(event) {
    const target = event.target
    if (forcing.onInput(event)) return
    if (onAssimilationInput(event)) return
    if (target.dataset.overpass) {
      const { date, time } = overpassParts()
      const next = target.dataset.overpass === 'date' ? { date: target.value, time } : { date, time: target.value }
      const clock = String(next.time || '').length === 5 ? `${next.time}:00` : String(next.time || '')
      if (/^\d{4}-\d{2}-\d{2}$/.test(next.date) && /^\d{2}:\d{2}:\d{2}$/.test(clock)) {
        form.overpass = `${next.date}T${clock}Z`
        markDirty()
      }
      return
    }
    if (target.dataset.path) {
      const value = target.type === 'checkbox' ? target.checked : target.dataset.number !== undefined ? (target.value === '' ? '' : Number(target.value)) : target.value
      setPath(form, target.dataset.path, value)
      if (target.dataset.path === 'region.enabled') $('#swRegion').hidden = !value
      if (target.dataset.path === 'optical.enabled' || target.dataset.path === 'thermal.enabled' || target.dataset.path === 'temperature.spinupHours') syncGroups()
      markDirty()
      return
    }
    const fieldRowElement = target.closest('[data-field]')
    if (fieldRowElement && target.dataset.role) {
      const field = getPath(form, fieldRowElement.dataset.field)
      field[target.dataset.role] = target.value
      markDirty()
      if (target.dataset.role === 'file') scheduleInspect()
      return
    }
    const bandRow = target.closest('tr[data-index]')
    if (bandRow && target.dataset.band) {
      const kind = bandRow.parentElement.dataset.bands
      const band = form[kind].bands[Number(bandRow.dataset.index)]
      // An emptied numeric cell stays '' so validation reports it instead of silently using 0.
      band[target.dataset.band] = target.dataset.band === 'name' ? target.value
        : target.value.trim() === '' ? '' : Number(target.value)
      form[kind].preset = 'custom'
      const select = $(`[data-preset="${kind}"]`)
      if (select) select.value = 'custom'
      markDirty()
    }
  }

  // 作用：左侧参数栏的 click 事件委托：分组折叠、栅格/常数切换、位置来源、温度模式/组分来源/土壤水分来源、
  // 浏览文件或目录、添加/删除波段。
  async function onParamClick(event) {
    if (forcing.onClick(event)) return
    const button = event.target.closest('button')
    if (!button) return
    if (await onAssimilationClick(button)) return
    const groupElement = button.closest('.sw-group')
    if (button.classList.contains('sw-group-toggle')) {
      const open = !groupElement.classList.contains('open')
      groupElement.classList.toggle('open', open)
      button.setAttribute('aria-expanded', String(open))
      return
    }
    const row = button.closest('[data-field]')
    if (button.dataset.mode && row) {
      getPath(form, row.dataset.field).mode = button.dataset.mode
      row.dataset.mode = button.dataset.mode
      markDirty()
      scheduleInspect()
      return
    }
    if (button.dataset.locationSource) {
      form.locationSource = button.dataset.locationSource
      for (const item of root.querySelectorAll('[data-location-source]')) item.classList.toggle('active', item === button)
      $('#swLocationFields').hidden = form.locationSource !== 'input'
      markDirty()
      return
    }
    const choice = [['temperatureMode', 'temperature-mode'], ['componentsSource', 'components-source'], ['moistureSource', 'moisture-source']]
      .find(([key]) => button.dataset[key])
    if (choice) {
      const [key, attribute] = choice
      const value = button.dataset[key]
      if (key === 'temperatureMode') form.temperature.mode = value
      else if (key === 'componentsSource') form.temperature.components.source = value
      else form.temperature.soilMoisture.source = value
      for (const item of root.querySelectorAll(`[data-${attribute}]`)) item.classList.toggle('active', item === button)
      $('#swRt').hidden = form.temperature.mode !== 'rt'
      $('#swEb').hidden = form.temperature.mode !== 'eb'
      $('#swComponentFields').hidden = form.temperature.components.source === 'package'
      $('#swComponentPackage').hidden = form.temperature.components.source !== 'package'
      markDirty()
      scheduleInspect()
      return
    }
    if (button.dataset.role === 'browse' || button.hasAttribute('data-browse-folder')) {
      const folder = button.hasAttribute('data-browse-folder')
      try {
        const result = await api.pick(folder ? 'folder' : 'file')
        if (!result?.path) return
        const input = folder ? $('[data-path="outputRoot"]') : row?.querySelector('[data-role="file"]')
        if (!input) return
        input.value = result.path
        input.dispatchEvent(new Event('input', { bubbles: true }))
      } catch (error) { toast('无法打开选择窗口', error.message, 'error') }
      return
    }
    if (button.dataset.addBand) {
      const kind = button.dataset.addBand
      form[kind].bands.push(kind === 'optical' ? { name: `B${form[kind].bands.length + 1}`, center: 665, fwhm: 30 } : { name: `T${form[kind].bands.length + 1}`, center: 10.5, fwhm: 1 })
      form[kind].preset = 'custom'
      rerenderBands(kind)
      markDirty()
      return
    }
    if (button.hasAttribute('data-remove-band')) {
      const tr = button.closest('tr')
      const kind = tr.parentElement.dataset.bands
      form[kind].bands.splice(Number(tr.dataset.index), 1)
      form[kind].preset = 'custom'
      rerenderBands(kind)
      markDirty()
    }
  }

  // 作用：只重绘某类波段表并同步预设下拉框的选中值。
  function rerenderBands(kind) {
    const groupBody = $(`[data-group="${kind}"] .sw-group-body`)
    groupBody.querySelector('table.sw-bands').outerHTML = bandTable(kind)
    groupBody.querySelector('[data-preset]').value = form[kind].preset
  }

  // 作用：左侧参数栏的 change 事件委托：气象强迫面板、气象数据包选择、传感器预设（选预设时整体替换波段表）。
  function onParamChange(event) {
    if (forcing.onChange(event)) return
    if (event.target.dataset.era5Pack !== undefined) {
      form.temperature.era5.pack = event.target.value
      markDirty()
      return
    }
    const preset = event.target.dataset.preset
    if (!preset) return
    form[preset].preset = event.target.value
    const presets = preset === 'optical' ? OPTICAL_PRESETS : THERMAL_PRESETS
    if (presets[event.target.value]) form[preset].bands = presetBands(presets[event.target.value])
    rerenderBands(preset)
    markDirty()
  }

  // 作用：返回当前配置下实际参与运行的输入字段声明，供 validate 检查必填项。
  // 说明：EB 且土壤水分取自气象强迫时不再要求地表土壤水分；仅 RT 且启用热红外或同化时才需要组分温度输入；
  // 经纬度仅在“输入经纬度”时必填。
  function fieldsInUse() {
    const t = form.temperature
    const fields = [...SURFACE_FIELDS.filter((spec) => spec.path !== 'surface.soilMoisture' || !(t.mode === 'eb' && t.soilMoisture.source === 'era5')),
      ...GEOMETRY_FIELDS.map((spec) => spec), ...(form.locationSource === 'input' ? LOCATION_FIELDS.map((spec) => ({ ...spec, required: true })) : [])]
    if (t.mode === 'rt' && (form.thermal.enabled || assimilating())) fields.push(...(t.components.source === 'package' ? [PACKAGE_FIELD] : COMPONENT_FIELDS))
    return fields
  }

  // 作用：运行前校验表单，返回问题文本数组（空数组表示通过），并给缺失字段行加 invalid 标记。
  // 步骤 1：必填输入与文件是否存在；步骤 2：波段与同化观测配置；步骤 3：EB 的起转节点数与气象数据包。
  function validate() {
    const problems = []
    const flag = (path, message) => { problems.push(message); $(`[data-field="${path}"]`)?.classList.add('invalid') }
    for (const row of root.querySelectorAll('.sw-field.invalid')) row.classList.remove('invalid')
    if (!form.surface.type.file.trim()) flag('surface.type', '缺少地表类型栅格')
    for (const spec of fieldsInUse()) {
      const field = getPath(form, spec.path)
      if (spec.required && spec.path !== 'surface.type' && (field.mode === 'file' || spec.constant === false ? !field.file.trim() : field.value === '')) flag(spec.path, `缺少${spec.label}`)
      const info = field.mode === 'file' && field.file.trim() ? inspections.get(field.file.trim()) : null
      if (info && !info.exists) flag(spec.path, `${spec.label}：${info.error || '文件不存在'}`)
    }
    const as = form.assimilation
    if (!form.optical.enabled && !form.thermal.enabled && !assimilating()) problems.push('请至少启用光学或热红外波段')
    if (assimilating()) {
      const label = ASSIMILATION_OBSERVATIONS[as.observation]
      if (!as.data) problems.push(`同化${label}需要观测数据文件`)
      if (as.observation === 'brightness' && !form.thermal.enabled) problems.push('同化亮温需要启用热红外波段（观测文件每个波段对应一个热红外波段）')
      if (as.optical && !form.optical.enabled) problems.push('光学反射率观测需要启用光学波段')
      if (as.observation === 'brightness')
        as.views.forEach((item, index) => {
          if (!item.data) return
          if (!item.vza.file && item.vza.value === undefined) problems.push(`附加视角 ${index + 1} 缺少天顶角`)
          if (!item.vaa.file && item.vaa.value === undefined) problems.push(`附加视角 ${index + 1} 缺少方位角`)
        })
    }
    if (form.optical.enabled && !form.optical.bands.length) problems.push('请至少添加一个光学波段')
    if (form.thermal.enabled && !form.thermal.bands.length) problems.push('请至少添加一个热红外波段')
    if (!Number.isFinite(Date.parse(form.overpass))) problems.push('请填写过境日期和时刻（UTC）')
    const t = form.temperature
    if (t.mode === 'eb') {
      const pack = packs.find((item) => item.path === t.era5.pack)
      if (Number.isNaN(spinupNodes(t.spinupHours))) problems.push('起转节点数须在 0–240 之间（留空使用缺省 7）')
      problems.push(...forcing.problems().map((text) => `气象强迫：${text}`))
      // 站点 CSV 来源不需要数据包；其余来源必须选择数据包，选中且索引可读时再校验其时段与变量。
      if (!forcing.isStation()) {
        if (!t.era5.pack) problems.push('EB 模式请选择气象数据包')
        else if (pack) problems.push(...packProblems(pack, form.overpass, t.soilMoisture.source, t.spinupHours).map((text) => `气象强迫：${text}`))
      }
    }
    return problems
  }

  // 作用：由表单生成提交给服务端的运行负载（场景配置 + projectPath）。
  // 说明：非“输入经纬度”时去掉经纬度字段；输出根目录有工程时取工程文件所在目录下的 output；
  // 删除仅界面使用的预设名与旧的 inversion；非同化模式不带 assimilation。
  function runPayload() {
    const scene = exportScene(form)
    if (scene.locationSource !== 'input') { delete scene.surface.latitude; delete scene.surface.longitude }
    delete scene.outputRoot
    // DEFERRED[低] 可维护性: projectPath 被放进 scene（streamApi.run 再包成 { scene }），服务端 submitRun 读的是 data.projectPath，所以服务端按工程推导输出目录的分支永远不会执行，实际依赖这里用字符串拼出的 outputRoot；两端各有一套推导逻辑；建议：只在服务端按工程路径推导，前端以顶层字段传 projectPath —— 需要同时改 streamApi.run 的请求体形状和服务端读取方式，属改变现有 POST 语义，本轮不动
    const projectPath = project?.path || ''
    if (projectPath) {
      const separator = projectPath.includes('\\') ? '\\' : '/'
      scene.outputRoot = `${projectPath.replace(/[\\/][^\\/]*$/, '')}${separator}output`
    }
    delete scene.optical.preset
    delete scene.thermal.preset
    delete scene.inversion
    if (scene.assimilation?.mode !== 'assimilate') delete scene.assimilation
    return { ...scene, projectPath }
  }

  // 模块：工程的新建、保存、打开。工程存放场景配置（stream-project.json），运行输出放在工程目录下的 output。
  // ---------- project ----------
  // 作用：设置当前工程并同步到本地存储（next 为 null 表示未保存工程）。
  function setProject(next) {
    project = next
    if (next) appStorage.setItem(PROJECT_KEY, JSON.stringify(next))
    else appStorage.removeItem(PROJECT_KEY)
  }

  function confirmDiscard() {
    return !dirty || window.confirm('当前影像模拟配置有未保存的修改，继续将放弃这些修改。')
  }

  // 作用：切换工程前使所有进行中的异步结果失效：递增各代号与探测令牌、取消探测计时、丢弃当前任务与缓存事件。
  // 说明：不会停止服务端已提交的任务，只是不再让界面接收其结果。
  function invalidateProject() {
    projectGeneration += 1
    jobGeneration += 1
    inspectToken += 1
    clearTimeout(inspectTimer)
    currentJob = null
    currentJobOutput = null
    pendingEvents = []
    starting = false
    setRunning(false)
  }

  // 作用：新建空白工程。mode 为 'rt' 或 'eb' 时预设温度模式（EB 同时启用热红外波段）；其他值保持缺省。
  // 说明：有未保存修改时先确认；新工程尚无路径，首次运行时会先要求保存。
  function newProject(mode) {
    if (!confirmDiscard()) return
    invalidateProject()
    form = normalizeScene(null)
    if (mode === 'eb' || mode === 'rt') form.temperature.mode = mode
    if (mode === 'eb') form.thermal.enabled = true
    setProject(null)
    dirty = false
    appStorage.setItem(STORAGE_KEY, JSON.stringify(exportScene(form)))
    inspections = new Map()
    if (!rendered) render()
    renderParams()
    renderHeader()
    showRun(null)
    show()
  }

  // 作用：保存工程。已有路径且非另存为时直接覆盖保存；否则选择目录，目标已有工程时询问是否覆盖。
  // 说明：保存是异步的，用 generation/revision 快照处理竞态：保存期间若切换了工程则丢弃结果；
  // 若保存期间表单又被修改，则保存后仍保持“未保存”状态。
  async function saveProject(saveAs = false) {
    const generation = projectGeneration
    const revision = editRevision
    const savedProject = project
    const isCurrent = () => generation === projectGeneration && project === savedProject
    try {
      let result
      const data = { name: project?.name || '', description: project?.description || '', scene: exportScene(form) }
      if (project?.path && !saveAs) result = await api.saveProject({ ...data, path: project.path })
      else {
        const picked = await api.pick('folder')
        if (!picked?.path || !isCurrent()) return
        // DEFERRED[低] 可维护性: 靠匹配中文错误文本 /已存在$/ 判断“文件已存在”，服务端改动文案就会让覆盖确认失效；建议：服务端返回结构化错误码（如 { code: 'EEXIST' }），前端按错误码判断 —— 需服务端新增错误码字段，属改变现有接口语义，本轮不动
        try { result = await api.saveProject({ ...data, directory: picked.path, overwrite: false }) }
        catch (error) {
          if (!isCurrent()) return
          if (!/已存在$/.test(error.message) || !window.confirm(`${picked.path} 中已有影像模拟工程，是否覆盖？`)) throw error
          result = await api.saveProject({ ...data, directory: picked.path, overwrite: true })
        }
      }
      if (!isCurrent()) {
        if (generation === projectGeneration && project?.path === result.path && revision !== editRevision) { dirty = true; renderHeader() }
        return
      }
      setProject({ path: result.path, name: result.name, description: result.description })
      if (revision === editRevision) {
        form.name = result.name || form.name
        dirty = false
      } else dirty = true
      renderHeader()
      toast('影像模拟工程已保存', result.path)
      addLog(`工程已保存：${result.path}`, 'success')
    } catch (error) { toast('保存失败', error.message, 'error') }
  }

  // 作用：打开服务端读出的工程：规范化场景、重置查看器与探测结果、刷新数据包列表。
  // 说明：migrated 为真表示读到的是旧版单景工程，已换算为 UTC 过境时刻，标为未保存，保存时另写新格式文件。
  async function openProject({ path, name, description, scene, migrated }) {
    if (rendered && !confirmDiscard()) return
    invalidateProject()
    form = normalizeScene(scene)
    // 工程名是持久名称；旧工程的 scene.name 可能只是历史运行标签，以工程名为准。
    if (name) form.name = name
    setProject(migrated ? { name, description } : { path, name, description })
    dirty = Boolean(migrated)
    appStorage.setItem(STORAGE_KEY, JSON.stringify(exportScene(form)))
    if (!rendered) render()
    showRun(null)
    inspections = new Map()
    renderParams()
    renderHeader()
    show()
    addLog(`已打开影像模拟工程：${path}`, 'success')
    void refreshPacks()
    if (migrated) addLog('该工程是旧版单景工程（当地时刻 + 时区），已换算为 UTC 过境时刻；保存时写为新的 stream-project.json，原文件不变。')
    if (description) addLog(description)
  }

  // 作用：弹出工程选择窗口并打开所选工程。
  async function pickProject() {
    try {
      const picked = await api.pick('project')
      if (!picked?.path) return
      await openProject(await api.openProject(picked.path))
    } catch (error) { toast('无法打开工程', error.message, 'error') }
  }

  // 模块：运行控制（提交任务、状态刷新、结果展示）。
  // ---------- run ----------
  // 作用：校验并提交一次运行。
  // 步骤 1：占位，防止重复提交；步骤 2：工程未保存则先保存（输出目录依赖工程路径）；
  // 步骤 3：重新探测输入并校验，不通过则提示并终止；步骤 4：提交任务，记下任务 id 与输出目录；
  // 步骤 5：回放任务 id 确定前缓存的事件。每个异步步骤后都检查 isCurrent，避免切换工程后串扰。
  async function startRun() {
    // Reserve this project's submission before any asynchronous preparation.
    if (running || starting) return
    starting = true
    const generation = projectGeneration
    const jobToken = ++jobGeneration
    view.token += 1
    const isCurrent = () => generation === projectGeneration && jobToken === jobGeneration
    if (!project?.path) {
      await saveProject(false)
      if (!isCurrent()) return
      if (!project?.path) { starting = false; return }
    }
    await inspectInputs()
    if (!isCurrent()) return
    const problems = validate()
    if (problems.length) {
      toast('参数不完整', problems.slice(0, 3).join('；'), 'error')
      for (const problem of problems) addLog(problem, 'error')
      starting = false
      return
    }
    try {
      clearLog()
      setRunState('running', '开始运行', 0)
      currentJob = 'pending'
      pendingEvents = []
      let job
      try { job = await api.run(runPayload()) }
      catch (error) { if (isCurrent()) currentJob = null; throw error }
      if (!isCurrent()) return
      currentJob = job.id
      currentJobOutput = job.outputDir || null
      setRunning(true)
      setRunState('running', status?.running ? '等待中' : '启动引擎', 0)
      addLog(`开始运行（${form.temperature.mode === 'eb' ? 'EB · 气象强迫能量平衡' : 'RT · 已知组分温度'}${assimilating() ? ` + 同化${ASSIMILATION_OBSERVATIONS[form.assimilation.observation]}` : ''}）`)
      const buffered = pendingEvents.filter((event) => event.job?.id === job.id)
      pendingEvents = []
      for (const event of buffered) handleEvent(event)
    } catch (error) {
      if (!isCurrent()) return
      if (String(error.message || '').includes('模拟准备已取消')) {
        setRunState('idle', '已取消', 0)
        addLog('模拟准备已取消')
        return
      }
      setRunState('failed', '未能启动')
      addLog(error.message, 'error')
      toast('无法开始模拟', error.message, 'error')
    } finally { if (isCurrent()) starting = false }
  }

  // 作用：设置运行中标记，并联动“运行/停止”按钮（引擎或参数目录未就绪时运行按钮保持禁用）。
  function setRunning(value) {
    running = value
    if (!rendered) return
    $('#swRun').disabled = value || !(status?.executableExists && status?.dataReady)
    $('#swStop').disabled = !value
  }

  // 作用：更新右侧“运行”卡片的状态点、主文案、细节和进度条（progress 为 0–100 的百分数，非数值时保持不变）。
  function setRunState(state, text, progress, detail = '') {
    const element = $('#swRunState')
    element.dataset.state = state
    element.querySelector('strong').textContent = text
    element.querySelector('span').textContent = detail
    if (Number.isFinite(progress)) $('#swProgress').style.width = `${Math.max(0, Math.min(100, progress))}%`
  }

  // 作用：向服务端查询引擎与任务队列状态，更新“引擎就绪”指示与运行态。
  // 说明：事件流可能中断，因此这里兜底：若本工程的任务已不在队列中但仍未收到结束事件，
  // 就当作已结束并读取其输出目录里的运行结果。
  async function refreshStatus() {
    const generation = projectGeneration
    const jobToken = jobGeneration
    try {
      const next = await api.status()
      if (generation !== projectGeneration || jobToken !== jobGeneration) return
      status = next
      const ready = status.executableExists && status.dataReady
      const engine = $('#swEngine')
      engine.dataset.state = ready ? 'ready' : 'missing'
      engine.querySelector('span').textContent = ready ? 'STREAM 就绪' : '未找到引擎'
      engine.title = ready ? status.executable : `未找到 ${status.executable} 或 stream-defined 参数目录，请先运行 npm run build:engine`
      const output = $('[data-path="outputRoot"]')
      if (output) output.placeholder = status.defaultOutputRoot
      // Status summaries have no project identity; only a submitted job belongs here.
      const mine = [status.running, ...status.queue].find((job) => job?.kind === 'image' && job.id === currentJob)
      setRunning(Boolean(mine) || (currentJob === 'pending' && running))
      if (mine) setRunState('running', mine.stage || '运行中', mine.progress)
      else if (Number.isInteger(currentJob)) {
        // The job ended while the event stream was down (job-closed lost): clear it and load the final result.
        const outputDir = currentJobOutput
        currentJob = null
        currentJobOutput = null
        const stillCurrent = () => generation === projectGeneration && jobToken === jobGeneration && currentJob === null
        if (!outputDir) setRunState('idle', '运行已结束', 0)
        else {
          try {
            const info = await api.runInfo(outputDir)
            if (stillCurrent()) showRun(info)
          } catch {
            if (stillCurrent()) setRunState('idle', '运行已结束', 0, '未能读取运行结果，请在结果列表中查看')
          }
        }
      }
    } catch (error) {
      if (generation !== projectGeneration || jobToken !== jobGeneration) return
      $('#swEngine').dataset.state = 'missing'
      $('#swEngine').querySelector('span').textContent = '引擎状态未知'
      $('#swEngine').title = error.message
    }
  }

  // 作用：重新读取已登记的气象数据包列表（读取失败视为空），并刷新下拉框、提示与登记对话框。
  async function refreshPacks() {
    try { packs = (await api.era5Packs(form.temperature.era5.pack ? [form.temperature.era5.pack] : [])).packs || [] } catch { packs = [] }
    const select = $('[data-era5-pack]')
    if (select) select.innerHTML = packOptions()
    syncGroups()
    renderEra5List()
  }

  // 作用：展示一次运行结果（next 为 null 则清空）：重置查看器令牌与分类统计缓存，
  // 刷新输出目录、状态、像元统计和结果列表；运行完成后异步加载分地类统计。
  function showRun(next) {
    view.token += 1
    run = next
    classStats = null
    view.typeLayer = null
    if (!rendered) return
    const directory = $('#swRunDir')
    directory.textContent = run?.outputDir ? fileName(run.outputDir) : '—'
    directory.title = run?.outputDir || ''
    root.querySelector('[data-action="open-output"]').disabled = !run?.outputDir
    if (!run) {
      setRunState('idle', '尚未运行', 0)
      renderCounts(null)
      renderProducts([])
      return
    }
    const label = { completed: '完成', failed: '失败', stopped: '已停止', running: '运行中' }[run.status] || run.status
    const when = run.startedAt ? new Date(run.startedAt).toLocaleString('zh-CN', { hour12: false }) : ''
    setRunState(run.status === 'completed' ? 'done' : run.status === 'running' ? 'running' : 'failed', label, run.status === 'completed' ? 100 : undefined,
      [when, run.elapsed ? `耗时 ${(run.elapsed / 1000).toFixed(1)} s` : ''].filter(Boolean).join('，'))
    renderCounts(run.summary)
    renderProducts(run.products || [])
    if (run.status === 'completed') void loadClassStats(run.outputDir)
  }

  // 作用：把运行摘要 summary 渲染成“像元”卡片的键值表（过境时刻、温度来源、EB/反演/同化统计、网格、各类像元数）。
  // 说明：summary.inversion 为旧版反演摘要，仍保留显示以兼容旧结果。
  function renderCounts(summary) {
    const counts = summary?.counts
    const grid = summary?.grid
    const temperature = summary?.temperature
    const rows = counts ? [
      ['过境时刻', summary.overpass ? summary.overpass.replace('T', ' ').replace('Z', ' UTC') : '—'],
      ['温度来源', temperature?.mode === 'eb' ? `EB · ${String(temperature.era5?.product || '').toUpperCase()} ${temperature.era5?.year || ''}` : 'RT'],
      ...(temperature?.mode === 'eb' ? [['能量平衡时次', (temperature.nodes || []).length],
        ...(Number.isFinite(temperature.spinupNodesUsed) ? [['起转时次', temperature.spinupNodesUsed < temperature.spinupNodesRequested ? `${temperature.spinupNodesUsed}（请求 ${temperature.spinupNodesRequested}，数据包不足）` : temperature.spinupNodesUsed]] : []),
        ['EB 求解像元', temperature.solvedPixels],
        ['过境时刻收敛', temperature.convergedPixels], ['水体（气象强迫 skt）', temperature.waterPixels]] : []),
      ...(summary.inversion ? [['反演', `${String(summary.inversion.target || '').toUpperCase()} · ${(summary.inversion.observations || []).length} 个观测`],
        ['反演成功', summary.inversion.counts?.retrievedPixels], ['反演失败', summary.inversion.counts?.failedPixels],
        ['云/掩膜', summary.inversion.counts?.cloudPixels],
        ['平均残差', Number.isFinite(summary.inversion.meanResidualK) ? `${summary.inversion.meanResidualK.toFixed(3)} K` : '—'],
        ...(summary.inversion.target === 'lsct' ? [['平均 DFS', Number.isFinite(summary.inversion.meanDegreesOfFreedom) ? summary.inversion.meanDegreesOfFreedom.toFixed(2) : '—'],
          ['信息量不足', summary.inversion.counts?.lowSupportPixels]] : [])] : []),
      ...(summary.assimilation ? (() => {
        const as = summary.assimilation
        const kinds = { brightness: '亮温', lst: '地表温度', components: '组分温度' }
        const k = (value) => Number.isFinite(value) ? `${value.toFixed(3)} K` : '—'
        return [['运行方式', `同化${kinds[as.observation] || ''} · ${as.background === 'eb' ? 'EB 背景' : 'RT 背景'}`],
          ['控制变量', (as.controls || []).join('、')],
          ['同化像元', as.analysedPixels], ['无观测', as.noObservationPixels], ['失败', as.failedPixels], ['未收敛', as.notConvergedPixels],
          ['平均 O−B', k(as.meanOmbK)], ['平均 O−A', k(as.meanOmaK)],
          ['平均 DFS', Number.isFinite(as.meanDegreesOfFreedom) ? as.meanDegreesOfFreedom.toFixed(2) : '—'], ['信息量不足', as.lowSupportPixels]]
      })() : []),
      ['网格', grid ? `${grid.width} × ${grid.height}` : '—'],
      ['光学有效', counts.opticalPixels], ['热红外有效', counts.thermalPixels],
      ['水体与填充', counts.waterOrFillPixels], ['未知地类', counts.unknownTypePixels],
      ['输入无效', counts.invalidInputPixels], ['夜间', counts.nightPixels]
    ] : [['暂无结果', '']]
    $('#swCounts').innerHTML = rows.map(([label, value]) => `<div><dt>${label}</dt><dd>${typeof value === 'number' ? value.toLocaleString('en-US') : escapeHtml(value ?? '—')}</dd></div>`).join('')
  }

  // 模块：结果影像查看器（产品与波段选择、色带与拉伸、缩放平移、直方图、像元读数、统计）。
  // ---------- viewer ----------
  // 作用：按文件名取产品显示信息（见 PRODUCTS），未登记的产品用默认值（4 位小数、viridis）。
  function productInfo(path = view.product) { return PRODUCTS[fileName(path)] || { label: fileName(path), kind: 'other', unit: '', digits: 4, colormap: 'viridis' } }

  // 作用：按 PRODUCTS 中的顺序列出结果 GeoTIFF；没有结果时重置查看器与提示文字。
  function renderProducts(products) {
    const tiffs = products.filter((path) => /\.tif$/i.test(path))
      .sort((a, b) => Object.keys(PRODUCTS).indexOf(fileName(a)) - Object.keys(PRODUCTS).indexOf(fileName(b)))
    const select = $('#swProduct')
    select.innerHTML = tiffs.length ? tiffs.map((path) => `<option value="${escapeHtml(path)}">${escapeHtml(productInfo(path).label)}</option>`).join('') : '<option>暂无结果</option>'
    select.disabled = !tiffs.length
    $('#swDisplay').disabled = !tiffs.length
    $('#swEmpty').hidden = Boolean(tiffs.length)
    if (!tiffs.length) {
      view.token += 1
      view.product = ''
      view.display = ''
      view.layers = []
      view.image = null
      view.fitted = ''
      $('#swDisplay').innerHTML = ''
      $('#swUnit').hidden = true
      $('#swReadout').innerHTML = '<span class="sw-muted">将光标移到影像上查看像元值；滚轮缩放，拖动平移，双击适应窗口。</span>'
      $('#swClassNote').textContent = '运行后按 IGBP 地类统计当前显示波段。'
      draw()
      for (const id of ['#swLegend', '#swScale', '#swNorth']) $(id).hidden = true
      $('#swBandStats').innerHTML = ''
      $('#swClassStats').innerHTML = ''
      drawHistogram()
      return
    }
    const sameFile = tiffs.find((path) => fileName(path) === fileName(view.product))
    const keep = tiffs.includes(view.product) ? view.product : sameFile || tiffs[0]
    select.value = keep
    void selectProduct(keep, Boolean(sameFile))
  }

  // 作用：读取产品第 1 波段以取得全部波段描述（波段名列表）；该栅格随后留在缓存中。
  async function bandNamesOf(path) {
    const first = await raster(path, 0)
    return first.bandNames || []
  }

  // 作用：为合成方案的三个目标波长，在波段名里（用 wavelengthOf 取“@ 波长 nm”）各找最近的波段。
  // 判据：偏差不超过 max(60 nm, 6% 目标波长)，且三个波段互不相同，否则返回 null（不提供该合成）。
  // 输出：三个波段下标（对应 R、G、B）或 null。
  function compositeBands(names, composite) {
    const picks = composite.targets.map((target) => {
      let best = -1, distance = Infinity
      names.forEach((name, index) => {
        const wavelength = wavelengthOf(name)
        if (Number.isFinite(wavelength) && Math.abs(wavelength - target) < distance) { distance = Math.abs(wavelength - target); best = index }
      })
      return distance <= Math.max(60, target * 0.06) ? best : -1
    })
    return picks.every((index) => index >= 0) && new Set(picks).size === 3 ? picks : null
  }

  // 作用：切换要查看的结果产品，并重建“显示”下拉框（光学产品先列出可用合成，再逐波段列出）。
  // 说明：keepDisplay 为真且原显示项仍存在时保持不变；换产品时恢复该产品的缺省色带并清除手动拉伸；
  // 用 view.token 丢弃过期的异步结果；仅热红外类产品显示 K/°C 切换。
  async function selectProduct(path, keepDisplay = false) {
    const token = ++view.token
    const changed = fileName(view.product) !== fileName(path)
    view.product = path
    const info = productInfo(path)
    let names = []
    try { names = await bandNamesOf(path) } catch (error) { if (token === view.token) addLog(error.message, 'error'); return }
    if (token !== view.token || view.product !== path) return
    const options = []
    if (info.kind === 'optical') {
      for (const composite of COMPOSITES) {
        const bands = compositeBands(names, composite)
        if (bands) options.push({ value: `rgb:${composite.key}`, label: `${composite.label}（${bands.map((index) => shortBand(names[index])).join('/')}）` })
      }
    }
    names.forEach((name, index) => options.push({ value: `band:${index}`, label: bandLabel(name) }))
    const display = $('#swDisplay')
    display.innerHTML = options.map((option) => `<option value="${option.value}">${escapeHtml(option.label)}</option>`).join('')
    if (!(keepDisplay && options.some((option) => option.value === view.display))) view.display = options[0]?.value || 'band:0'
    display.value = view.display
    if (changed) {
      view.colormap = info.colormap
      $('#swColormap').value = view.colormap
      view.manual = null
    }
    $('#swUnit').hidden = info.kind !== 'thermal'
    await showView()
  }

  // 作用：栅格缓存（键为“路径#波段”，值为 Promise）。超过 24 条时按插入顺序淘汰最早的条目。
  // DEFERRED[中] 性能: 缓存按条目数（24）而不是字节数限制，每条最多是 2048×2048 的 Float32（约 16 MB）加上直方图/统计，最坏情况常驻约 400 MB 渲染进程内存；缓存键只有路径和波段，同一路径的输入文件（如 typeFile）被改写后仍显示旧数据；建议：按总字节数做 LRU（例如 128 MB），切换运行时清空，键里加入文件 mtime/size —— 需要引入新的容量上限常量并改缓存键（要额外请求文件 mtime/size 字段），属数值常量与接口契约变更，本轮不动
  function rememberRaster(key, promise) {
    rasterCache.set(key, promise)
    while (rasterCache.size > 24) rasterCache.delete(rasterCache.keys().next().value)
    return promise
  }

  // 作用：读取某产品某波段的栅格（带缓存）：服务端返回 base64 的 float32，解码为 data（Float32Array，无数据像元按非有限值处理），
  // 同时携带宽高、统计量（minimum/maximum 与 p1/p2/p98/p99 分位数）、直方图、波段名、坐标系和像元大小。
  // 说明：读取失败时撤销缓存项，便于重试。
  function raster(path, band) {
    const key = `${path}#${band}`
    if (rasterCache.has(key)) return rasterCache.get(key)
    return rememberRaster(key, api.raster(path, band).then((result) => ({ ...result, data: decodeFloat32(result.values), values: undefined }))
      .catch((error) => { rasterCache.delete(key); throw error }))
  }

  // 作用：解析显示下拉框的取值：'rgb:<合成键>' 为三波段合成，'band:<下标>' 为单波段。
  function displayBands() {
    if (view.display.startsWith('rgb:')) {
      const composite = COMPOSITES.find((item) => item.key === view.display.slice(4))
      return { rgb: true, composite }
    }
    return { rgb: false, band: Number(view.display.slice(5)) || 0 }
  }

  // 作用：按当前产品与显示项读取所需栅格（合成取三个），然后建图、适应窗口或重绘，并刷新图例、统计和直方图。
  // 说明：每次调用递增 view.token，读取返回后若令牌已变说明用户又做了新选择，丢弃本次结果。
  async function showView() {
    const token = ++view.token
    const path = view.product
    if (!path) return
    const selection = displayBands()
    try {
      let layers
      if (selection.rgb) {
        const names = await bandNamesOf(path)
        const bands = selection.composite ? compositeBands(names, selection.composite) : null
        if (!bands) return
        layers = await Promise.all(bands.map((band) => raster(path, band)))
      } else layers = [await raster(path, selection.band)]
      if (token !== view.token) return
      view.layers = layers
      $('#swColormap').disabled = selection.rgb
      buildImage()
      const signature = `${layers[0].width}x${layers[0].height}`
      if (view.fitted !== signature) { view.fitted = signature; fit() } else draw()
      renderLegend()
      renderBandStats()
      renderClassStats()
      drawHistogram()
      void loadTypeLayer()
    } catch (error) {
      if (token === view.token) { addLog(`无法读取结果影像：${error.message}`, 'error'); toast('无法读取结果影像', error.message, 'error') }
    }
  }

  // 说明：栅格内部以 K 存温度；仅热红外类产品在“°C”模式下换算显示（toDisplay：K→显示值，fromDisplay：显示值→K），
  // unitLabel 给出当前单位文字。
  const toDisplay = (value) => view.celsius && productInfo().kind === 'thermal' ? value - 273.15 : value
  const fromDisplay = (value) => view.celsius && productInfo().kind === 'thermal' ? value + 273.15 : value
  const unitLabel = () => productInfo().kind === 'thermal' ? (view.celsius ? '°C' : 'K') : productInfo().unit

  // 作用：求某图层的拉伸区间 [低, 高]：手动（仅单波段）、最小–最大、p1–p99 或缺省的 p2–p98 分位数。
  function stretchRange(layer) {
    const stats = layer.stats
    if (view.stretch === 'manual' && view.manual && view.layers.length === 1) return [view.manual[0], view.manual[1]]
    if (view.stretch === 'minmax') return [stats.minimum, stats.maximum]
    if (view.stretch === 'p1') return [stats.p1, stats.p99]
    return [stats.p2, stats.p98]
  }

  // 作用：把当前图层按拉伸区间转成屏幕用的离屏画布 view.image（每个栅格像元对应一个画布像素）。
  // 说明：单波段经色带查找表着色；三个图层视为 R/G/B，各通道独立线性拉伸；
  // 非有限值（NaN，即无数据）像元设为全透明；区间宽度为 0 时用 1 防止除零。
  function buildImage() {
    const layers = view.layers
    if (!layers.length) { view.image = null; return }
    const { width, height } = layers[0]
    const canvas = document.createElement('canvas')
    canvas.width = width; canvas.height = height
    const context = canvas.getContext('2d')
    const image = context.createImageData(width, height)
    const ranges = layers.map(stretchRange)
    const lut = LUTS[view.colormap] || LUTS.gray
    const pixels = image.data
    for (let index = 0; index < width * height; index += 1) {
      const offset = index * 4
      if (layers.length === 3) {
        let valid = true
        for (let channel = 0; channel < 3; channel += 1) {
          const value = layers[channel].data[index]
          if (!Number.isFinite(value)) { valid = false; break }
          const [low, high] = ranges[channel]
          pixels[offset + channel] = Math.max(0, Math.min(255, (value - low) / ((high - low) || 1) * 255))
        }
        pixels[offset + 3] = valid ? 255 : 0
      } else {
        const value = layers[0].data[index]
        if (!Number.isFinite(value)) { pixels[offset + 3] = 0; continue }
        const [low, high] = ranges[0]
        const level = Math.round(Math.max(0, Math.min(1, (value - low) / ((high - low) || 1))) * 255) * 3
        pixels[offset] = lut[level]; pixels[offset + 1] = lut[level + 1]; pixels[offset + 2] = lut[level + 2]; pixels[offset + 3] = 255
      }
    }
    context.putImageData(image, 0, 0)
    view.image = canvas
  }

  // 作用：返回影像区的显示尺寸 [宽, 高]（CSS 像素）。
  function canvasSize() {
    const map = $('#swMap')
    return [map.clientWidth, map.clientHeight]
  }

  // 作用：缩放并居中使整幅影像适应窗口（四周留 24 px 边距）。
  function fit() {
    if (!view.image) { draw(); return }
    const [width, height] = canvasSize()
    view.scale = Math.min((width - 24) / view.image.width, (height - 24) / view.image.height)
    view.x = (width - view.image.width * view.scale) / 2
    view.y = (height - view.image.height * view.scale) / 2
    draw()
  }

  // 作用：以画布坐标 (cx, cy) 为中心缩放，使光标下的像元位置保持不动；缩放倍数限制在 0.05–64。
  function zoomAt(factor, cx, cy) {
    if (!view.image) return
    const next = Math.max(0.05, Math.min(64, view.scale * factor))
    view.x = cx - (cx - view.x) * next / view.scale
    view.y = cy - (cy - view.y) * next / view.scale
    view.scale = next
    draw()
  }

  // 作用：按当前平移/缩放把 view.image 画到画布，并按设备像素比提高清晰度；
  // “平滑/像素”由 view.interpolation 决定，并在影像外缘画一圈淡色边框。
  function draw() {
    if (!rendered) return
    const canvas = $('#swCanvas')
    const [width, height] = canvasSize()
    const ratio = window.devicePixelRatio || 1
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
      canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio)
    }
    const context = canvas.getContext('2d')
    context.setTransform(ratio, 0, 0, ratio, 0, 0)
    context.clearRect(0, 0, width, height)
    if (!view.image) return
    context.imageSmoothingEnabled = view.interpolation === 'smooth'
    context.imageSmoothingQuality = 'high'
    context.drawImage(view.image, view.x, view.y, view.image.width * view.scale, view.image.height * view.scale)
    context.strokeStyle = 'rgba(255,255,255,.18)'
    context.lineWidth = 1
    context.strokeRect(Math.round(view.x) - .5, Math.round(view.y) - .5, Math.round(view.image.width * view.scale) + 1, Math.round(view.image.height * view.scale) + 1)
    renderScale()
  }

  // 作用：刷新比例尺和指北针。仅当栅格有像元大小且坐标系为 WGS84/UTM 或 EPSG:3857 时显示比例尺；
  // 比例尺目标长度约 140 屏幕像素，取整齐的长度（见 niceLength）。
  // 说明：layer.step 为服务端 rasters.mjs 返回的抽样步长：step = ceil(max(宽, 高) / 2048)，显示数据每 step 个原始像元取一个，
  //   所以屏幕上一个显示像元对应 pixelSizeX × step 米（EPSG:3857 还要乘 cos 纬度）。
  function renderScale() {
    const layer = view.layers[0]
    const scaleElement = $('#swScale')
    const projected = layer && layer.pixelSizeX && (utmZone(layer.crs) || /EPSG:3857/.test(layer.crs || ''))
    $('#swNorth').hidden = !(layer && layer.pixelSizeX)
    if (!projected) { scaleElement.hidden = true; return }
    // EPSG:3857 的像元大小是投影米，地面距离 = 投影米 × cos(纬度)（中纬度下不修正会偏大 30–50%），
    // 按影像中心纬度修正；UTM 的像元大小已是地面米，系数为 1。
    const metresPerScreenPixel = layer.pixelSizeX * layer.step * mercatorGroundScale(layer) / view.scale
    const length = niceLength(metresPerScreenPixel * 140)
    scaleElement.hidden = false
    scaleElement.querySelector('i').style.width = `${length / metresPerScreenPixel}px`
    scaleElement.querySelector('span').textContent = length >= 1000 ? `${length / 1000} km` : `${length} m`
  }

  // 作用：把栅格值按当前单位格式化（非有限值显示“—”）。
  function formatValue(value, digits) {
    if (!Number.isFinite(value)) return '—'
    return toDisplay(value).toFixed(digits)
  }

  // 作用：刷新图例：三波段合成显示各通道的波段名与拉伸区间；单波段显示色带与低/中/高三个刻度。
  function renderLegend() {
    const legend = $('#swLegend')
    const layers = view.layers
    if (!layers.length) { legend.hidden = true; return }
    const info = productInfo()
    legend.hidden = false
    if (layers.length === 3) {
      legend.innerHTML = `<div class="sw-legend-rgb">${['R', 'G', 'B'].map((channel, index) => {
        const [low, high] = stretchRange(layers[index])
        return `<div><b class="ch-${channel}">${channel}</b><span>${escapeHtml(shortBand(layers[index].bandNames?.[layers[index].band]))}</span><em>${formatValue(low, 3)}–${formatValue(high, 3)}</em></div>`
      }).join('')}</div><small>${escapeHtml(STRETCHES[view.stretch === 'manual' ? 'p2' : view.stretch])}，各通道独立拉伸</small>`
      return
    }
    const [low, high] = stretchRange(layers[0])
    const stops = COLORMAPS[view.colormap].stops
    legend.innerHTML = `<div class="sw-legend-title">${escapeHtml(shortBand(layers[0].bandNames?.[layers[0].band]))} ${escapeHtml(info.label)}${unitLabel() ? `（${unitLabel()}）` : ''}</div>
      <div class="sw-ramp" style="background:linear-gradient(90deg,${stops.join(',')})"></div>
      <div class="sw-ramp-labels"><span>${formatValue(low, info.digits)}</span><span>${formatValue((low + high) / 2, info.digits)}</span><span>${formatValue(high, info.digits)}</span></div>`
  }

  // 作用：绘制直方图。单波段：柱高取计数的平方根（压缩动态范围），红线标出当前拉伸的上下限，
  // 并同步手动上下限输入框（输入框有焦点时不覆盖）；三波段合成：三条折线叠加，横轴取三者的共同范围。
  function drawHistogram() {
    if (!rendered) return
    const canvas = $('#swHistogram')
    const context = canvas.getContext('2d')
    const ratio = window.devicePixelRatio || 1
    const width = canvas.clientWidth || 300, height = canvas.clientHeight || 76
    canvas.width = width * ratio; canvas.height = height * ratio
    context.setTransform(ratio, 0, 0, ratio, 0, 0)
    context.clearRect(0, 0, width, height)
    const manual = $('#swManual')
    const layer = view.layers.length === 1 ? view.layers[0] : null
    manual.hidden = !layer
    if (view.layers.length === 3 && view.layers.every((item) => item.histogram)) {
      const minimum = Math.min(...view.layers.map((item) => item.stats.minimum))
      const maximum = Math.max(...view.layers.map((item) => item.stats.maximum))
      const colors = ['rgba(229,72,77,.75)', 'rgba(48,164,108,.75)', 'rgba(62,99,221,.75)']
      view.layers.forEach((item, channel) => {
        const bins = item.histogram, peak = Math.max(...bins) || 1
        context.strokeStyle = colors[channel]; context.lineWidth = 1.4
        context.beginPath()
        bins.forEach((count, index) => {
          const value = item.stats.minimum + (index + .5) / bins.length * (item.stats.maximum - item.stats.minimum)
          const x = (value - minimum) / ((maximum - minimum) || 1) * width
          const y = height - 12 - Math.sqrt(count / peak) * (height - 16)
          if (index) context.lineTo(x, y); else context.moveTo(x, y)
        })
        context.stroke()
      })
      context.fillStyle = '#536a82'; context.font = '10px "Segoe UI", "Microsoft YaHei"'
      context.fillText(minimum.toFixed(3), 2, height - 1)
      const maxText = maximum.toFixed(3)
      context.fillText(maxText, width - context.measureText(maxText).width - 2, height - 1)
      return
    }
    if (!layer || !layer.histogram) {
      context.fillStyle = '#71849a'; context.font = '11px "Segoe UI", "Microsoft YaHei"'
      context.fillText('暂无数据', 8, height / 2 + 4)
      return
    }
    const bins = layer.histogram
    const peak = Math.max(...bins) || 1
    const barWidth = width / bins.length
    context.fillStyle = 'rgba(36,111,211,.55)'
    bins.forEach((count, index) => {
      const barHeight = Math.max(count ? 1 : 0, Math.sqrt(count / peak) * (height - 14))
      context.fillRect(index * barWidth, height - 12 - barHeight, Math.max(1, barWidth - .5), barHeight)
    })
    const { minimum, maximum } = layer.stats
    const [low, high] = stretchRange(layer)
    const position = (value) => (value - minimum) / ((maximum - minimum) || 1) * width
    context.strokeStyle = '#d94b5b'; context.lineWidth = 1.5
    for (const value of [low, high]) { const x = Math.max(1, Math.min(width - 1, position(value))); context.beginPath(); context.moveTo(x, 2); context.lineTo(x, height - 12); context.stroke() }
    context.fillStyle = '#536a82'; context.font = '10px "Segoe UI", "Microsoft YaHei"'
    const digits = productInfo().digits
    context.fillText(formatValue(minimum, digits), 2, height - 1)
    const maxText = formatValue(maximum, digits)
    context.fillText(maxText, width - context.measureText(maxText).width - 2, height - 1)
    if (document.activeElement !== $('#swMin')) $('#swMin').value = Number.isFinite(low) ? toDisplay(low).toFixed(digits) : ''
    if (document.activeElement !== $('#swMax')) $('#swMax').value = Number.isFinite(high) ? toDisplay(high).toFixed(digits) : ''
  }

  // 作用：为“波段统计”表选出对应当前产品的统计行（来自运行摘要 run.summary），无对应摘要则为空。
  // 说明：行下标与波段下标一一对应，用于高亮当前显示的波段；热红外两个产品按波段名区分亮温/方向温度。
  function summaryRows() {
    const summary = run?.summary
    if (!summary) return []
    const file = fileName(view.product)
    if (file === 'optical_reflectance.tif') return summary.optical || []
    if (file === 'tir_brightness_temperature.tif') return (summary.thermal || []).filter((item) => /brightness/i.test(item.name))
    if (file === 'tir_surface_temperature.tif') return (summary.thermal || []).filter((item) => /surface/i.test(item.name))
    if (file === 'solar_geometry.tif') return summary.solarGeometry || []
    if (file === 'components.tif') return summary.components || []
    if (file === 'eb_fluxes.tif') return summary.fluxes || []
    if (/^(lst|lsct)(_uncertainty|_qa|_fractions)?\.tif$/.test(file)) {
      const stats = summary.inversion?.statistics || []
      if (file === 'lst.tif') return stats.filter((item) => /^LST \(K\)$/.test(item.name))
      if (file === 'lst_uncertainty.tif') return stats.filter((item) => /^LST uncertainty/.test(item.name))
      if (file === 'lsct.tif') return stats.filter((item) => /temperature \(K\)$/.test(item.name))
    }
    if (file === 'analysis_components.tif') return (summary.assimilation?.statistics || []).filter((item) => /^analysis T/.test(item.name))
    if (file === 'analysis_qa.tif') return (summary.assimilation?.statistics || []).filter((item) => !/^analysis T/.test(item.name))
    return []
  }

  // 作用：渲染“波段统计”表（最小/平均/最大），并高亮正在显示的波段；点击某行可切换显示该波段（见 bind）。
  function renderBandStats() {
    const rows = summaryRows()
    const info = productInfo()
    const active = new Set(view.layers.map((layer) => layer.band))
    const unit = unitLabel()
    const named = ['components.tif', 'eb_fluxes.tif', 'lst.tif', 'lst_uncertainty.tif', 'lsct.tif', 'analysis_components.tif', 'analysis_qa.tif'].includes(fileName(view.product))
    $('#swBandStats').innerHTML = rows.length ? `<thead><tr><th>波段</th><th>最小</th><th>平均</th><th>最大</th></tr></thead><tbody>${rows.map((item, index) => `
      <tr class="${active.has(index) ? 'active' : ''}" data-band-row="${index}" title="${escapeHtml(item.name)}，有效像元 ${Number(item.validPixels || 0).toLocaleString('en-US')}">
        <td>${escapeHtml(named ? bandLabel(item.name) : shortBand(item.name))}</td><td>${formatValue(item.minimum, info.digits)}</td><td>${formatValue(item.mean, info.digits)}</td><td>${formatValue(item.maximum, info.digits)}</td></tr>`).join('')}</tbody>
      ${unit ? `<tfoot><tr><td colspan="4">单位：${unit}</td></tr></tfoot>` : ''}` : ''
  }

  // 作用：向服务端请求按 IGBP 地类的统计；返回时若已切换到其他运行结果则丢弃。
  async function loadClassStats(outputDir) {
    const owner = run
    $('#swClassNote').textContent = '正在按地类统计…'
    try {
      const result = await api.classStats(outputDir)
      if (run !== owner || run?.outputDir !== outputDir) return
      classStats = result
      renderClassStats()
    } catch (error) {
      if (run === owner && run?.outputDir === outputDir) $('#swClassNote').textContent = `无法统计：${error.message}`
    }
  }

  // 作用：渲染“分地类统计”表：每个地类的面积（km²）与当前显示波段的均值（单波段另给标准差），按有效像元数降序。
  // 说明：面积优先取服务端给出的 areaKm2，否则用像元数 × 单像元面积 pixelAreaKm2 估算。
  function renderClassStats() {
    const table = $('#swClassStats')
    const note = $('#swClassNote')
    const product = classStats?.products?.[fileName(view.product)]
    if (!classStats || !product || !view.layers.length) {
      table.innerHTML = ''
      if (classStats && view.product) note.textContent = '当前结果不提供分地类统计。'
      return
    }
    const bands = view.layers.map((layer) => layer.band)
    const info = productInfo()
    const area = classStats.pixelAreaKm2
    const rows = classStats.classes
      .map((item) => ({ ...item, values: bands.map((band) => product.bands[band]?.[item.code]) }))
      .filter((item) => item.values.some(Boolean))
      .sort((a, b) => (b.values[0]?.count || 0) - (a.values[0]?.count || 0))
    note.textContent = `${bands.length === 1 ? '均值 ± 标准差' : '各通道均值'}${unitLabel() ? `，单位 ${unitLabel()}` : ''}；面积按有效像元计。`
    table.innerHTML = `<thead><tr><th>地类</th><th>面积 km²</th>${bands.map((band) => `<th>${escapeHtml(shortBand(view.layers.find((layer) => layer.band === band)?.bandNames?.[band]))}</th>`).join('')}</tr></thead>
      <tbody>${rows.map((item) => {
        const count = item.values[0]?.count || 0
        const areaKm2 = Number.isFinite(item.values[0]?.areaKm2) ? item.values[0].areaKm2 : area ? count * area : null
        return `<tr><td title="IGBP ${item.code}"><i class="sw-class-code">${item.code}</i>${escapeHtml(IGBP[item.code] || `地类 ${item.code}`)}</td>
          <td>${areaKm2 !== null ? areaKm2.toLocaleString('en-US', { maximumFractionDigits: 1 }) : '—'}</td>
          ${item.values.map((value) => `<td>${value ? (bands.length === 1 ? `${formatValue(value.mean, info.digits)}<small> ± ${(value.std).toFixed(info.digits)}</small>` : formatValue(value.mean, info.digits)) : '—'}</td>`).join('')}</tr>`
      }).join('')}</tbody>`
  }

  // 作用：读取本次运行的地表类型栅格（运行时 typeBand 从 1 起计，故减 1），供光标读数显示地类；失败静默忽略。
  async function loadTypeLayer() {
    const owner = run
    const typeFile = run?.typeFile
    if (!typeFile || view.typeLayer) return
    try {
      const layer = await raster(typeFile, (owner.typeBand || 1) - 1)
      if (run === owner && run?.typeFile === typeFile) view.typeLayer = layer
    } catch {}
  }

  // 作用：光标读数：由光标位置反算像元行列，显示原栅格行列、投影坐标 X/Y（米）、经纬度、地类和各波段取值。
  // 步骤 1：屏幕坐标 → 查看器像元（减平移、除缩放）；步骤 2：乘抽样步长 layer.step 还原到原栅格行列；
  // 步骤 3：用原点与像元大小求像元中心坐标（北向上，y 向下递减），再换算经纬度。
  function readout(event) {
    const layer = view.layers[0]
    const element = $('#swReadout')
    if (!layer || !view.image) return
    const rect = $('#swMap').getBoundingClientRect()
    const column = Math.floor((event.clientX - rect.left - view.x) / view.scale)
    const row = Math.floor((event.clientY - rect.top - view.y) / view.scale)
    if (column < 0 || row < 0 || column >= layer.width || row >= layer.height) {
      element.innerHTML = '<span class="sw-muted">光标在影像范围外</span>'
      return
    }
    const index = row * layer.width + column
    const sourceColumn = column * layer.step, sourceRow = row * layer.step
    const parts = [`<span><em>行列</em>${sourceRow}, ${sourceColumn}</span>`]
    if (Number.isFinite(layer.originX) && layer.pixelSizeX) {
      const x = layer.originX + (sourceColumn + .5) * layer.pixelSizeX
      const y = layer.originY - (sourceRow + .5) * layer.pixelSizeY
      const geographicLayer = isGeographic(layer.crs) || layer.unit === '°'
      const geo = geographicLayer ? geographic(layer.crs, x, y) || [y, x] : geographic(layer.crs, x, y)
      if (!geographicLayer) parts.push(`<span title="${escapeHtml(crsLabel(layer.crs))}"><em>X / Y</em>${x.toLocaleString('en-US', { maximumFractionDigits: 1 })}, ${y.toLocaleString('en-US', { maximumFractionDigits: 1 })} m</span>`)
      if (geo) parts.push(`<span><em>经纬度</em>${Math.abs(geo[0]).toFixed(5)}°${geo[0] < 0 ? 'S' : 'N'}, ${Math.abs(geo[1]).toFixed(5)}°${geo[1] < 0 ? 'W' : 'E'}</span>`)
    }
    const type = view.typeLayer && view.typeLayer.width === layer.width && view.typeLayer.height === layer.height ? view.typeLayer.data[index] : null
    if (Number.isFinite(type)) parts.push(`<span><em>地类</em>${type} ${escapeHtml(IGBP[type] || '')}</span>`)
    const info = productInfo()
    const values = view.layers.map((item) => `${escapeHtml(shortBand(item.bandNames?.[item.band]))} <strong>${formatValue(item.data[index], info.digits)}</strong>`)
    parts.push(`<span class="sw-value">${values.join('　')}${unitLabel() ? ` ${unitLabel()}` : ''}</span>`)
    element.innerHTML = parts.join('')
  }

  // 模块：运行日志。普通行最多保留 500 条；GDAL/PROJ 库提示合并成一个可折叠块并计数。
  // ---------- log ----------
  function timestamp() { return new Date().toLocaleTimeString('zh-CN', { hour12: false }) }

  function addLog(text, level = '') {
    if (!rendered) return
    const log = $('#swLog')
    if (level === 'diagnostic') {
      diagnostics.set(text, (diagnostics.get(text) || 0) + 1)
      let block = log.querySelector('.sw-diagnostics')
      if (!block) {
        block = document.createElement('details')
        block.className = 'sw-diagnostics'
        log.append(block)
      }
      const total = [...diagnostics.values()].reduce((sum, count) => sum + count, 0)
      block.innerHTML = `<summary><time>${timestamp()}</time><span>GDAL/PROJ 库提示 ${total} 条（${diagnostics.size} 种），展开查看</span></summary>${[...diagnostics].map(([message, count]) => `<div>${escapeHtml(message)}${count > 1 ? ` <b>×${count}</b>` : ''}</div>`).join('')}`
      return
    }
    const line = document.createElement('div')
    line.className = `sw-log-line ${level}`
    line.innerHTML = `<time>${timestamp()}</time><span>${escapeHtml(text)}</span>`
    log.append(line)
    logCount += 1
    $('#swLogCount').textContent = String(logCount)
    while (log.childElementCount > 500) log.firstElementChild.remove()
    log.scrollTop = log.scrollHeight
  }

  function clearLog() {
    $('#swLog').replaceChildren()
    diagnostics.clear()
    logCount = 0
    $('#swLogCount').textContent = '0'
  }

  // 模块：界面事件绑定。全局级监听（全屏变化、快捷键、关闭前保存草稿）只绑定一次（globalBound）。
  // ---------- events ----------
  // 作用：渲染后一次性绑定各类事件。
  // 步骤 1：根节点按钮委托（返回、运行/停止、数据包登记、适应窗口、全屏、1:1、打开输出目录、清空日志）；
  // 步骤 2：左侧参数栏的 input/change/click/toggle；步骤 3：查看器控件（产品、显示、色带、拉伸、手动上下限、单位、插值）；
  // 步骤 4：影像区交互（滚轮缩放、拖动平移、双击适应、光标读数、尺寸变化重绘）；步骤 5：全局快捷键与关闭前保存草稿。
  function bind() {
    root.addEventListener('click', (event) => {
      const action = event.target.closest('[data-action]')?.dataset.action
      if (!action) return
      if (action === 'back') close()
      else if (action === 'run') void startRun()
      else if (action === 'stop') api.stop(currentJob).catch((error) => addLog(error.message, 'error'))
      else if (action === 'era5') openEra5()
      else if (action === 'era5-close') $('#swEra5').hidden = true
      else if (action === 'era5-register') void registerEra5()
      else if (action === 'era5-use') { form.temperature.era5.pack = event.target.closest('[data-pack]').dataset.pack; $('[data-era5-pack]').innerHTML = packOptions(); markDirty(); $('#swEra5').hidden = true }
      else if (action === 'era5-remove') void removeEra5(event.target.closest('[data-pack]').dataset.pack)
      else if (action === 'fit') fit()
      else if (action === 'fullscreen') {
        const map = $('#swMap')
        if (document.fullscreenElement) void document.exitFullscreen()
        else void map.requestFullscreen?.()
      }
      else if (action === 'actual' && view.image) { const [width, height] = canvasSize(); zoomAt(1 / view.scale, width / 2, height / 2) }
      else if (action === 'open-output' && run?.outputDir) void api.openPath(run.outputDir)
      else if (action === 'clear-log') clearLog()
    })
    const params = $('#swParams')
    params.addEventListener('input', onParamInput)
    params.addEventListener('change', onParamChange)
    params.addEventListener('click', onParamClick)
    params.addEventListener('toggle', (event) => { void forcing.onToggle(event) }, true)
    $('#swProduct').addEventListener('change', (event) => selectProduct(event.target.value))
    $('#swDisplay').addEventListener('change', (event) => { view.display = event.target.value; view.manual = null; void showView() })
    $('#swColormap').addEventListener('change', (event) => { view.colormap = event.target.value; buildImage(); draw(); renderLegend() })
    $('#swStretch').addEventListener('change', (event) => {
      view.stretch = event.target.value
      if (view.stretch === 'manual' && !view.manual && view.layers.length === 1) {
        const stats = view.layers[0].stats
        view.manual = [stats.p2, stats.p98]
      }
      refreshDisplay()
    })
    for (const id of ['#swMin', '#swMax']) {
      $(id).addEventListener('change', () => {
        const minText = $('#swMin').value.trim(), maxText = $('#swMax').value.trim()
        const low = minText === '' ? NaN : fromDisplay(Number(minText)), high = maxText === '' ? NaN : fromDisplay(Number(maxText))
        if (!Number.isFinite(low) || !Number.isFinite(high) || high <= low) { toast('拉伸范围无效', '上限须大于下限', 'error'); drawHistogram(); return }
        view.manual = [low, high]
        view.stretch = 'manual'
        $('#swStretch').value = 'manual'
        refreshDisplay()
      })
    }
    $('#swUnit').addEventListener('click', (event) => {
      const button = event.target.closest('[data-unit]')
      if (!button) return
      view.celsius = button.dataset.unit === 'C'
      for (const item of $('#swUnit').children) item.classList.toggle('active', item === button)
      renderLegend(); renderBandStats(); renderClassStats(); drawHistogram()
    })
    $('#swInterp').addEventListener('click', (event) => {
      const button = event.target.closest('[data-interp]')
      if (!button) return
      view.interpolation = button.dataset.interp === 'pixel' ? 'pixel' : 'smooth'
      for (const item of $('#swInterp').children) item.classList.toggle('active', item === button)
      draw()
    })
    if (!globalBound) document.addEventListener('fullscreenchange', () => {
      if (rendered) setTimeout(() => fit(), 80)
    })
    $('#swBandStats').addEventListener('click', (event) => {
      const row = event.target.closest('[data-band-row]')
      if (!row) return
      view.display = `band:${row.dataset.bandRow}`
      $('#swDisplay').value = view.display
      view.manual = null
      void showView()
    })
    // 步骤 4：影像区交互。滚轮每格缩放 1.2 倍；左键拖动平移；移动时同时刷新光标读数。
    const map = $('#swMap')
    let drag = null
    map.addEventListener('wheel', (event) => {
      event.preventDefault()
      const rect = map.getBoundingClientRect()
      zoomAt(event.deltaY < 0 ? 1.2 : 1 / 1.2, event.clientX - rect.left, event.clientY - rect.top)
    }, { passive: false })
    map.addEventListener('pointerdown', (event) => {
      if (!view.image || event.button !== 0) return
      drag = { x: event.clientX, y: event.clientY, viewX: view.x, viewY: view.y }
      map.setPointerCapture(event.pointerId)
      map.classList.add('dragging')
    })
    map.addEventListener('pointermove', (event) => {
      if (drag) { view.x = drag.viewX + event.clientX - drag.x; view.y = drag.viewY + event.clientY - drag.y; draw() }
      readout(event)
    })
    const endDrag = () => { drag = null; map.classList.remove('dragging') }
    map.addEventListener('pointerup', endDrag)
    map.addEventListener('pointercancel', endDrag)
    map.addEventListener('dblclick', fit)
    new ResizeObserver(() => { if (!root.hidden) draw() }).observe(map)
    // 步骤 5：快捷键（捕获阶段，工作台显示时才响应）：Ctrl/⌘+S 保存，Ctrl/⌘+Shift+S 另存为，F5 运行。
    if (!globalBound) window.addEventListener('keydown', (event) => {
      if (root.hidden) return
      const key = event.key.toLowerCase()
      if ((event.ctrlKey || event.metaKey) && key === 's') { event.preventDefault(); event.stopImmediatePropagation(); void saveProject(event.shiftKey) }
      else if (event.key === 'F5') { event.preventDefault(); event.stopImmediatePropagation(); void startRun() }
    }, true)
    if (!globalBound) window.addEventListener('beforeunload', () => { if (dirty) appStorage.setItem(STORAGE_KEY, JSON.stringify(exportScene(form))) })
    globalBound = true
  }

  // 作用：拉伸或色带变化后，不重新读取栅格，只重建图像并刷新图例与直方图。
  function refreshDisplay() {
    buildImage(); draw(); renderLegend(); drawHistogram()
  }

  // 作用：显示工作台：必要时先渲染，订阅服务端事件（只订阅一次），并刷新引擎状态、数据包列表与栅格探测。
  function show() {
    if (!rendered) render()
    root.hidden = false
    document.body.classList.add('scene-workspace-open')
    if (!unsubscribe) unsubscribe = api.onEvent(handleEvent)
    void refreshStatus()
    void refreshPacks()
    scheduleInspect()
    requestAnimationFrame(() => (view.image ? draw() : null))
  }

  function open() {
    show()
  }

  // 作用：隐藏工作台并通知外部（返回三维场景工作台）；不会停止正在运行的任务。
  function close() {
    root.hidden = true
    document.body.classList.remove('scene-workspace-open')
    onClose()
  }

  // 模块：气象强迫数据包的登记、列出与移除。数据包只是对 NetCDF 文件的索引，不复制数据；
  // 为兼容旧工程与引擎接口，字段与接口名仍沿用 era5 命名，实际也可用于其他逐时气象强迫。
  // ---------- forcing packs (project/engine field name: era5) ----------
  // 作用：渲染已登记数据包列表（名称、产品/时段、文件数、是否完整可用，以及选用/移除操作）。
  function renderEra5List() {
    if (!rendered) return
    const list = $('#swEra5List')
    if (!packs.length) {
      list.innerHTML = `<p class="sw-note">还没有登记的气象数据包。登记目录：${escapeHtml(status?.era5Directory || '')}</p>`
      return
    }
    list.innerHTML = `<table class="sw-table"><thead><tr><th>名称</th><th>产品 / 时段</th><th>文件</th><th>状态</th><th></th></tr></thead><tbody>${packs.map((pack) => {
      const problems = [...pack.missingVariables, ...pack.incompleteVariables]
      const optional = ['skt', 'swvl1', 'z'].filter((name) => pack.variables?.[name]).join('、')
      return `<tr data-pack="${escapeHtml(pack.path)}"${pack.path === form.temperature.era5.pack ? ' class="active"' : ''} title="${escapeHtml(pack.path)}">
        <td>${escapeHtml(pack.label)}</td><td>${escapeHtml(String(pack.product || '').toUpperCase())} ${escapeHtml(packPeriodLabel(pack))}</td><td>${pack.files}</td>
        <td>${pack.complete ? `可用${optional ? `（含 ${escapeHtml(optional)}）` : ''}` : `<span class="sw-bad">缺失或不连续：${escapeHtml(problems.join('、'))}</span>`}</td>
        <td class="sw-actions-cell"><button class="text-button" type="button" data-action="era5-use"${pack.complete ? '' : ' disabled'}>选用</button>${pack.external ? '<span class="sw-muted">工程自带</span>' : '<button class="text-button" type="button" data-action="era5-remove">移除</button>'}</td></tr>`
    }).join('')}</tbody></table>`
  }

  // 作用：打开“气象强迫数据”对话框并刷新列表。
  function openEra5() {
    if (!rendered) render()
    show()
    $('#swEra5').hidden = false
    void refreshPacks()
  }

  // 作用：选择 NetCDF 文件并提交登记任务（登记进度经事件通道回到 handleEvent）。
  // 说明：产品选“按当前面板里的元数据”时，改用强迫面板当前的自定义元数据（需为网格类来源）。
  async function registerEra5() {
    try {
      const picked = await api.pick('era5')
      if (!picked?.paths?.length) return
      const product = $('#swEra5Product').value
      const data = { files: picked.paths, product, label: $('#swEra5Label').value }
      if (product === 'current') {
        const metadataDocument = forcing.currentDocument()
        if (!metadataDocument || forcing.isStation()) throw new Error('请先在“气象强迫”面板选择网格类来源并核对元数据')
        data.metadata = metadataDocument
        data.projectPath = project?.path || ''
        delete data.product
      }
      // 登记是后台任务，进度与结果经 SSE 事件回到 handleEvent，这里不使用返回值。
      await api.registerEra5(data)
      $('#swEra5Status').textContent = `开始登记气象强迫数据：${picked.paths.length} 个文件`
      $('#swEra5Status').dataset.level = ''
      addLog(`气象强迫数据登记：${picked.paths.length} 个文件`)
    } catch (error) { toast('无法登记气象数据包', error.message, 'error') }
  }

  // 作用：移除数据包索引（不删除原始气象数据）；若它正被当前表单选用则同时清空选择。
  async function removeEra5(path) {
    if (!window.confirm(`移除气象数据包索引？\n${path}\n只删除索引文件，不删除原始气象数据。`)) return
    try {
      await api.removeEra5(path)
      if (form.temperature.era5.pack === path) { form.temperature.era5.pack = ''; markDirty() }
      await refreshPacks()
    } catch (error) { toast('无法移除', error.message, 'error') }
  }

  // 模块：服务端事件（SSE）处理。事件类型：status、job-queued、job-started、job-progress、job-log、job-closed。
  // ---------- engine events (/api/stream/events) ----------
  // 作用：处理服务端事件。
  // 步骤 1：status 事件触发状态刷新；步骤 2：数据包登记任务（kind='era5'）的进度、日志与结束，
  // 结束后刷新列表，若当前尚未选包且新包完整则自动选用；
  // 步骤 3：影像任务事件——任务 id 还没返回时先缓存（见 startRun），非当前任务的事件忽略；
  // 步骤 4：开始/排队/进度/日志更新界面；步骤 5：结束时读取运行结果展示，读取失败则用事件携带的结果兜底。
  function handleEvent(event) {
    if (!rendered) return
    if (event.type === 'status') { if (status) void refreshStatus(); return }
    const job = event.job
    if (!job) return
    if (job.kind === 'era5') {
      const element = $('#swEra5Status')
      if (event.type === 'job-progress') element.textContent = `${job.stage} ${Math.round(job.progress)}%`
      else if (event.type === 'job-log') { element.textContent = event.text; element.dataset.level = event.level === 'error' ? 'error' : '' ; addLog(`气象强迫：${event.text}`, event.level || '') }
      else if (event.type === 'job-closed') {
        const pack = event.result?.pack
        const ok = event.status === 'completed' && pack
        element.textContent = ok ? `已登记：${pack.label}（${String(pack.product).toUpperCase()} ${packPeriodLabel(pack)}）${pack.complete ? '' : '，必需变量缺失或不连续，不能用于 EB'}` : `登记失败，详见运行日志`
        element.dataset.level = ok && pack.complete ? 'ok' : 'error'
        void refreshPacks()
        if (ok && pack.complete && !form.temperature.era5.pack) { form.temperature.era5.pack = pack.path; $('[data-era5-pack]').innerHTML = packOptions(); markDirty() }
      }
      return
    }
    if (currentJob === 'pending') { pendingEvents.push(event); return }
    if (job.id !== currentJob) return
    if (event.type === 'job-started') {
      setRunning(true)
      setRunState('running', '启动引擎', 0)
      addLog(`引擎已启动，输出目录：${job.outputDir}`)
    } else if (event.type === 'job-queued') setRunState('running', '等待中', 0, '等待前一次运行完成')
    else if (event.type === 'job-progress') setRunState('running', job.stage || '运行中', job.progress, `${Math.round(job.progress)}%`)
    else if (event.type === 'job-log') addLog(event.text, event.level || '')
    // 步骤 5：任务结束：清除当前任务，写日志，异步读取输出目录的运行信息并展示。
    else if (event.type === 'job-closed') {
      const generation = projectGeneration
      const jobToken = jobGeneration
      const isCurrent = () => generation === projectGeneration && jobToken === jobGeneration && currentJob === null
      setRunning(false)
      currentJob = null
      currentJobOutput = null
      const ok = event.status === 'completed'
      addLog(ok ? `模拟完成，耗时 ${(event.elapsed / 1000).toFixed(1)} 秒` : event.status === 'stopped' ? '模拟已停止' : `模拟失败，退出码 ${event.code ?? '—'}`, ok ? 'success' : 'error')
      if (job.outputDir) {
        void (async () => {
          try {
            const info = await api.runInfo(job.outputDir)
            if (isCurrent()) showRun({ ...info, elapsed: event.elapsed })
          } catch { if (isCurrent()) showRun({ outputDir: job.outputDir, status: event.status, products: event.result?.products, summary: event.result?.summary, elapsed: event.elapsed }) }
        })()
      } else setRunState(event.status === 'stopped' ? 'idle' : 'failed', event.status === 'stopped' ? '已取消' : '失败', 0)
      if (!ok && event.status !== 'stopped') toast('遥感影像模拟失败', '详见运行日志', 'error')
      else if (ok && root.hidden) toast('遥感影像模拟完成', job.outputDir)
    }
  }

  // 作用：供外部（菜单/主界面）调用的命令入口：new/new-rt/new-eb 新建工程，mode-rt/mode-eb 切换温度模式，
  // open/save/save-as 工程操作，era5 打开数据包对话框，run 运行。
  function command(name) {
    if (name === 'new' || name === 'new-rt') newProject('rt')
    else if (name === 'new-eb') newProject('eb')
    else if (name === 'mode-rt' || name === 'mode-eb') {
      const mode = name === 'mode-eb' ? 'eb' : 'rt'
      if (form.temperature.mode !== mode) {
        form.temperature.mode = mode
        markDirty() // markDirty 内已把表单写入 appStorage，无需再单独写一次
      }
      if (!rendered) render()
      else renderParams()
      renderHeader()
      show()
    }
    else if (name === 'open') void pickProject()
    else if (name === 'save') void saveProject(false)
    else if (name === 'save-as') void saveProject(true)
    else if (name === 'era5') openEra5()
    else if (name === 'run') void startRun()
  }

  return { open, openProject, close, command, isOpen: () => rendered && !root.hidden }
}
