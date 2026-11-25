// ==========================================================================
// 文件：stream/forcing-panel.js
// 功能：STREAM 影像工作区中“气象强迫元数据”面板：选择强迫数据源（ERA5/ERA5-Land/GLDAS/站点 CSV/自定义），
//       编辑各物理量的变量名、单位、取值类型与观测高度，调用引擎校验并显示解析摘要。
// 主要模块：
//   1. 枚举/单位常量与小工具（路径存取 getAt/setAt、canonical 比较）
//   2. summaryLines：把引擎检查结果 resolved 转成摘要行
//   3. createForcingPanel：状态（state）与场景同步（currentValue/commit/syncFromForm）、预设加载、
//      编辑联动（normalise）、校验/保存/载入/还原/站点文件操作
//   4. 事件入口（onClick/onChange/onInput/onToggle）与渲染（variableRows/stationSection/editor/render）
// 数据流：scene.temperature.forcing = { metadata, files } ↔ state.working（完整元数据副本）→
//       streamApi.checkForcing/saveForcingMetadata/readForcingMetadata（见 stream/api.js）→ server/stream
// 关系：由 STREAM 工作区（workspace.js，待核对）创建并转发事件；字段含义见 docs/forcing-metadata.md
// 约定：metadata 为 undefined 表示 ERA5 缺省（旧工程行为不变）；高度单位 m；温度/气压/风速的源单位见各单位表
// ==========================================================================

// Weather-forcing metadata panel of the STREAM image workspace (docs/forcing-metadata.md).
// The scene stores only temperature.forcing = { metadata, files }: metadata is undefined (ERA5 default, unchanged behaviour),
// a preset name, a project metadata file path, or an inline object once edited. The editor works on a full copy of the
// metadata document ("working") and writes the smallest equivalent value back into the scene after every edit.
// 常量：数据源类型（id, 显示名）；除 custom 外的 id 同时是服务端预设名（PRESET_IDS）。
// 下面各枚举/单位表对应 docs/forcing-metadata.md 中元数据字段的可选值与界面文案。
const SOURCE_TYPES = [
  ['era5', 'ERA5'], ['era5-land', 'ERA5-Land'], ['gldas-noah-0.25', 'GLDAS Noah 0.25°'], ['gldas-noah-1.0', 'GLDAS Noah 1°'],
  ['station-csv', '气象站点 CSV'], ['custom', '自定义']
]
const PRESET_IDS = SOURCE_TYPES.filter(([id]) => id !== 'custom').map(([id]) => id)
const TEMPERATURE_UNITS = ['K', 'degC', 'degF']
const PRESSURE_UNITS = ['Pa', 'hPa', 'kPa', 'mmHg', 'inHg']
// 湿度类型表：类型 → [显示名, 可选单位, 切换类型时的缺省单位]
const HUMIDITY = {
  dewpoint: ['露点', TEMPERATURE_UNITS, 'K'], specific: ['比湿', ['kg kg-1', 'g kg-1'], 'kg kg-1'], mixingRatio: ['混合比', ['kg kg-1', 'g kg-1'], 'kg kg-1'],
  relative: ['相对湿度', ['%', '1'], '%'], vapourPressure: ['水汽压', PRESSURE_UNITS, 'hPa']
}
const PRESSURE_TYPES = { surface: '地面气压', seaLevel: '海平面气压', none: '无（按海拔估算）' }
const WIND_TYPES = { components: '分量 u / v', speedDirection: '风速 + 风向', speed: '仅风速' }
const WIND_UNITS = ['m s-1', 'km h-1', 'kt', 'mph']
const POWER_UNITS = ['W m-2', 'kW m-2', 'MJ m-2 h-1', 'MJ m-2 d-1', 'cal cm-2 min-1']
const ENERGY_UNITS = ['J m-2', 'kJ m-2', 'MJ m-2', 'Wh m-2', 'kWh m-2', 'cal cm-2']
const SOIL_UNITS = ['m3 m-3', '%', 'kg m-2']
// kind（量的时间含义）、stamp（记录时间戳含义）、时间格式等枚举的显示名；
// 通量类 kind：instant 瞬时、interval_mean 时段平均（功率）、accumulated 时段累积、
// cumulative 自重置时刻起的累计（后两者用能量单位）。
const STATE_KINDS = { instant: '瞬时', previous: '保持到下一记录' }
const FLUX_KINDS = { instant: '瞬时', interval_mean: '时段平均', accumulated: '时段累积', cumulative: '自重置累计' }
const STAMPS = { instant: '时刻', interval_start: '时段起点', interval_end: '时段终点', interval_mid: '时段中点' }
const TIME_FORMATS = { iso8601: 'ISO 8601', epoch_seconds: '纪元秒', epoch_milliseconds: '纪元毫秒', yyyymmddhh: 'YYYYMMDDHH', yyyymmddhhmm: 'YYYYMMDDHHMM' }
const FORM_NAMES = { gridded: '网格', station: '站点' }
// 辐射类变量：向下短波、向下长波（共用 kind/stamp 的联动规则）
const RADIATION = ['shortwaveDown', 'longwaveDown']

// 小工具：HTML 转义、按点号路径取值/赋值（setAt 会按需创建中间对象/数组，value 为 undefined 时删除键）、
// canonical（键排序后的 JSON，用于比较两份元数据是否等价）、预设/路径判别、按 kind 选功率或能量单位表、
// number（限制小数位的字符串化）。
const escapeText = (value) => String(value ?? '').replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char])
const getAt = (object, path) => path.split('.').reduce((node, key) => (node === null || node === undefined ? undefined : node[key]), object)
function setAt(object, path, value) {
  const keys = path.split('.')
  let node = object
  keys.slice(0, -1).forEach((key, index) => {
    if (node[key] === null || typeof node[key] !== 'object') node[key] = /^\d+$/.test(keys[index + 1]) ? [] : {}
    node = node[key]
  })
  if (value === undefined) delete node[keys.at(-1)]
  else node[keys.at(-1)] = value
}
const canonical = (value) => JSON.stringify(value, (_key, item) => item && typeof item === 'object' && !Array.isArray(item)
  ? Object.fromEntries(Object.keys(item).sort().map((key) => [key, item[key]])) : item)
const isPreset = (value) => typeof value === 'string' && PRESET_IDS.includes(value)
const isPathValue = (value) => typeof value === 'string' && value.trim() !== '' && !isPreset(value)
const unitsFor = (kind) => (kind === 'accumulated' || kind === 'cumulative' ? ENERGY_UNITS : POWER_UNITS)
const number = (value, digits = 4) => (Number.isFinite(Number(value)) ? String(Number(Number(value).toFixed(digits))) : String(value))

// One-line summary of the engine's forcing-check `resolved` block (what will actually be used).
// 作用：把引擎 forcing 检查返回的 resolved 块转成面板里显示的摘要行（实际将使用什么）。
// 输入：resolved（metadata/variables/heightCorrection/derived/ignored）；无则返回空数组。
// 内容：元数据名称与步长、各变量的来源/单位/类型、风高度对数律订正系数、气温递减率订正量（K）、
//       换算说明与未使用的字段。
export function summaryLines(resolved) {
  if (!resolved) return []
  const lines = []
  const meta = resolved.metadata || {}
  lines.push(`元数据：${meta.name || '—'}（${FORM_NAMES[meta.form] || meta.form || '—'}，${Number(meta.stepHours) > 0 ? `步长 ${number(meta.stepHours)} h` : '步长自动检测'}）`)
  const variables = resolved.variables || {}
  const describe = (label, item, extra = '') => item && lines.push(`${label}：${item.source || '—'}（${[item.unit, extra].filter(Boolean).join('，')}）`)
  describe('气温', variables.airTemperature, variables.airTemperature?.height_m ? `${number(variables.airTemperature.height_m)} m` : '')
  describe('湿度', variables.humidity, HUMIDITY[variables.humidity?.type]?.[0] || variables.humidity?.type)
  describe('气压', variables.pressure, PRESSURE_TYPES[variables.pressure?.type] || '')
  describe('风速', variables.windSpeed, WIND_TYPES[variables.windSpeed?.type] || '')
  describe('向下短波', variables.shortwaveDown, FLUX_KINDS[variables.shortwaveDown?.kind] || '')
  describe('向下长波', variables.longwaveDown, FLUX_KINDS[variables.longwaveDown?.kind] || '')
  const wind = resolved.heightCorrection?.wind
  if (wind && Number.isFinite(Number(wind.from_m))) {
    if (Math.abs(Number(wind.from_m) - Number(wind.to_m)) <= 0.01) lines.push(`风：${number(wind.from_m)} m，与引擎约定高度一致，不订正`)
    else if (wind.mode === 'none') lines.push(`风：${number(wind.from_m)} m，引擎约定 ${number(wind.to_m)} m，未订正`)
    else lines.push(`风：${number(wind.from_m)} m → 对数律订正到 ${number(wind.to_m)} m（系数 ${number(wind.factor, 3)}）`)
  }
  const air = resolved.heightCorrection?.airTemperature
  if (air?.applied) lines.push(`气温：按递减率订正 ${number(air.shiftK, 3)} K`)
  for (const item of resolved.derived || []) lines.push(`换算：${item}`)
  if ((resolved.ignored || []).length) lines.push(`未使用：${resolved.ignored.join('、')}`)
  return lines
}

// 模块：气象强迫元数据面板（工厂函数）。
// 输入：api（stream/api.js 的 streamApi）、getForm（返回当前表单，取 .temperature）、
//       getProject/getPack（当前工程、ERA5 数据包）、markDirty（通知外层有未保存改动）、root（面板 DOM）。
// 核心状态 state：type（数据源类型）、base（所基于的预设）、working（元数据文档的完整副本）、
//       path（工程元数据文件路径）、dirty、token（异步加载的取消令牌）、result（检查结果）。
// 场景里只保存 temperature.forcing.metadata 的最小等价值（见 currentValue）。
export function createForcingPanel({ api, getForm, getProject, getPack, markDirty, root }) {
  const $ = (selector) => root.querySelector(selector)
  const state = {
    initialized: false, committedKey: '', type: 'era5', base: 'era5', working: null, path: '', dirty: false, open: false,
    loading: false, token: 0, result: null, stale: false, busy: '', status: null, presets: new Map()
  }
  const form = () => getForm().temperature

  // ---------- state <-> scene ----------
  // 判断 document 是否与预设 name 等价（忽略 preset 字段），用于决定能否回写成预设名。
  const samePreset = (document, name) => state.presets.has(name) && canonical({ ...document, preset: undefined }) === canonical({ ...state.presets.get(name), preset: undefined })
  // 作用：求要写回场景的 metadata 值：未加载 → 原值；有未改动的文件路径 → 路径；
  //       与基础预设等价 → 预设名（ERA5 缺省则为 undefined，保持旧工程行为）；否则为 working 的完整副本。
  function currentValue() {
    if (!state.working) return form().forcing?.metadata
    if (state.path && !state.dirty) return state.path
    if (state.base && samePreset(state.working, state.base)) return state.base === 'era5' ? undefined : state.base
    return structuredClone(state.working)
  }
  // 作用：把 currentValue() 写回 temperature.forcing（metadata 为空则删除；files 为空则删除；
  //       forcing 整体为空则删除），并记录 committedKey；silent 为真时不触发 markDirty。
  function commit({ silent = false } = {}) {
    const temperature = form()
    const next = { ...(temperature.forcing || {}) }
    const value = currentValue()
    if (value === undefined || value === '') delete next.metadata
    else next.metadata = value
    if (!Array.isArray(next.files) || !next.files.length) delete next.files
    if (Object.keys(next).length) temperature.forcing = next
    else delete temperature.forcing
    state.committedKey = canonical(temperature.forcing?.metadata)
    if (!silent) markDirty()
  }
  // 作用：场景表单变化后（如打开工程）重新同步面板状态；若 metadata 与上次提交一致则什么也不做。
  // 分支：空 → ERA5 缺省；预设名 → 对应预设；字符串 → 自定义的元数据文件路径；对象 → 内联自定义文档。
  // 非 ERA5 且尚无 working 时异步 load()。
  function syncFromForm() {
    const value = form().forcing?.metadata
    const key = canonical(value)
    if (state.initialized && key === state.committedKey) return
    Object.assign(state, { initialized: true, committedKey: key, working: null, path: '', dirty: false, result: null, stale: false, status: null, token: state.token + 1 })
    if (value === undefined || value === null || value === '') Object.assign(state, { type: 'era5', base: 'era5' })
    else if (isPreset(value)) Object.assign(state, { type: value, base: value })
    else if (typeof value === 'string') Object.assign(state, { type: 'custom', base: null, path: value })
    else Object.assign(state, { type: value.preset && isPreset(value.preset) ? value.preset : 'custom', base: isPreset(value.preset) ? value.preset : null, working: structuredClone(value), dirty: true })
    if (state.type !== 'era5' && !state.working) void load()
  }

  // 作用：取预设文档（首次向服务端请求并缓存），返回深拷贝以免编辑污染缓存。
  async function presetDocument(name) {
    if (!state.presets.has(name)) state.presets.set(name, (await api.forcingPreset(name)).metadata)
    return structuredClone(state.presets.get(name))
  }
  // 注意：用 token 防止过期响应覆盖较新的选择；失败时在 state.status 显示错误。
  // Fill `working` from the stored value (preset name, metadata file path or default ERA5).
  async function load() {
    const token = ++state.token
    state.loading = true; render()
    try {
      let document
      if (state.path) {
        const project = getProject()
        const read = await api.readForcingMetadata({ projectPath: project?.path || '', path: state.path })
        if (!read.exists) throw new Error(`元数据文件不存在：${state.path}`)
        document = read.metadata
        state.base = isPreset(document.preset) ? document.preset : null
        state.type = state.base || 'custom'
        if (state.base) await presetDocument(state.base)
      } else {
        document = await presetDocument(state.base || 'era5')
        document.preset = state.base || 'era5'
      }
      if (token !== state.token) return
      state.working = document
      state.status = null
    } catch (error) {
      if (token !== state.token) return
      state.status = { level: 'error', text: error.message }
    } finally {
      if (token === state.token) { state.loading = false; render() }
    }
  }

  // 作用：用户切换数据源类型。custom：以当前文档（或 ERA5 预设）为起点，去掉 preset 字段并标记 dirty；
  //       其他：丢弃修改，载入对应预设；最后提交并重绘。
  async function chooseType(type) {
    state.token += 1
    state.type = type; state.result = null; state.stale = false; state.status = null
    if (type === 'custom') {
      state.loading = false
      if (!state.working) { // start from the ERA5 layout
        try { state.working = await presetDocument(state.base || 'era5') } catch (error) { state.status = { level: 'error', text: error.message }; render(); return }
      }
      delete state.working.preset
      state.working.name = 'custom'
      state.base = null; state.path = ''; state.dirty = true
    } else {
      state.base = type; state.path = ''; state.dirty = false; state.working = null
      try {
        const document = await presetDocument(type)
        state.working = { ...document, preset: type }
      } catch (error) { state.status = { level: 'error', text: error.message } }
    }
    commit(); render()
  }

  // ---------- editing ----------
  // 任何编辑：标记 dirty、使旧的检查结果过期（stale）并提交到场景。
  const touch = () => { state.dirty = true; state.stale = true; commit() }
  // 若 path 处的单位不在允许列表内，则改为 fallback（缺省取列表首项）。
  function fixUnit(path, units, fallback) {
    const current = getAt(state.working, `${path}.unit`)
    if (!units.includes(current)) setAt(state.working, `${path}.unit`, fallback ?? units[0])
  }
  // 作用：把输入控件的值写进 working；data-fm 为点号路径，data-fm-type 为 text/number/list；
  //       空值删除该键；list 以逗号拆分（单个元素时存为字符串）。
  function setField(target) {
    const kind = target.dataset.fmType || 'text'
    const raw = target.value
    let value
    if (kind === 'number') { const parsed = Number(raw); value = raw.trim() !== '' && Number.isFinite(parsed) ? parsed : undefined }
    else if (kind === 'list') { const parts = raw.split(',').map((item) => item.trim()).filter(Boolean); value = parts.length > 1 ? parts : parts[0] }
    else value = raw === '' ? undefined : raw
    setAt(state.working, target.dataset.fm, value)
  }
  // 作用：下拉框（类型/种类/单位）改变后做联动修正，使文档保持自洽：
  //   - 湿度类型 → 切换到该类型的合法单位；气压类型 none 时去掉 source；
  //   - 风类型 → 重建对应通道（u/v、speed/direction、仅 speed），保留 height_m；
  //   - 辐射 kind → 按功率/能量选合法单位；
  //   - 土壤湿度单位为 kg m-2 时补 thickness_m（缺省 0.1 m），否则删除；
  //   - 形态 station/gridded → 补齐对应的 station/time/spatial/grid 缺省段，去掉另一形态的字段。
  function normalise(path) {
    const w = state.working
    const v = w.variables || (w.variables = {})
    if (path === 'variables.humidity.type') {
      const info = HUMIDITY[v.humidity?.type]
      if (info) fixUnit('variables.humidity', info[1], info[2])
    } else if (path === 'variables.pressure.type') {
      if (v.pressure?.type === 'none') delete v.pressure.source
      else if (!v.pressure.unit) v.pressure.unit = 'Pa'
    } else if (path === 'variables.wind.type') {
      const old = v.wind || {}
      const channel = (name) => old[name] || (name === 'direction' ? { source: '', unit: 'deg' } : { source: '', unit: 'm s-1' })
      const next = { type: old.type }
      if (old.height_m !== undefined) next.height_m = old.height_m
      if (old.type === 'components') Object.assign(next, { u: channel('u'), v: channel('v') })
      else if (old.type === 'speedDirection') Object.assign(next, { speed: channel('speed'), direction: channel('direction'), convention: old.convention || 'from' })
      else Object.assign(next, { speed: old.speed || old.u || channel('speed') })
      v.wind = next
    } else if (/^variables\.(shortwaveDown|longwaveDown)\.kind$/.test(path)) {
      const key = path.split('.')[1]
      fixUnit(`variables.${key}`, unitsFor(v[key]?.kind))
    } else if (path === 'variables.soilMoisture.unit') {
      if (v.soilMoisture?.unit === 'kg m-2') { if (v.soilMoisture.thickness_m === undefined) v.soilMoisture.thickness_m = 0.1 } else delete v.soilMoisture.thickness_m
    } else if (path === 'form') {
      if (w.form === 'station') {
        w.station = { delimiter: ',', comment: '#', columns: { id: 'station_id', latitude: 'lat', longitude: 'lon', elevation: 'elev' }, ...(w.station || {}) }
        w.time = { column: 'time', format: 'iso8601', ...Object.fromEntries(Object.entries(w.time || {}).filter(([key]) => key !== 'variable')) }
        w.spatial = { interpolation: 'idw', idwPower: 2, maxStations: 4, ...(w.spatial || {}) }
      } else {
        w.grid = w.grid || { latitude: ['lat', 'latitude'], longitude: ['lon', 'longitude'] }
        w.time = { variable: ['valid_time', 'time'], ...Object.fromEntries(Object.entries(w.time || {}).filter(([key]) => !['column', 'format'].includes(key))) }
        w.spatial = { interpolation: 'bilinear', ...Object.fromEntries(Object.entries(w.spatial || {}).filter(([key]) => key === 'lapseRateKPerKm')) }
      }
    }
  }
  // 作用：面板内带 data-fm 的控件事件入口，返回是否已处理。
  // 说明：下拉/复选框在 input 事件时忽略，等 change 再处理；文本类控件在 input 时处理，change 时忽略；
  //       下拉框改变属于“结构性”修改，会 normalise 并整体重绘。
  function onField(event) {
    const target = event.target
    if (target.dataset.fm === undefined || !state.working) return false
    if (event.type === 'input' && (target.tagName === 'SELECT' || target.type === 'checkbox')) return true
    if (event.type === 'change' && target.tagName !== 'SELECT') return target.tagName === 'INPUT'
    setField(target)
    const structural = target.tagName === 'SELECT'
    if (structural) normalise(target.dataset.fm)
    touch()
    if (structural) render()
    return true
  }
  // 辐射变量时间戳的统一值：两者一致取该值，不一致返回 'mixed'。
  const stampValue = () => {
    const stamps = RADIATION.map((key) => state.working?.variables?.[key]?.stamp)
    return stamps[0] === stamps[1] ? stamps[0] || '' : 'mixed'
  }
  // 把时间戳同时设置到向下短波/长波，降水若已有 stamp 也一并更新。
  function setStamp(value) {
    for (const key of RADIATION) if (state.working.variables?.[key]) setAt(state.working, `variables.${key}.stamp`, value || undefined)
    if (state.working.variables?.precipitation?.stamp !== undefined && value) state.working.variables.precipitation.stamp = value
  }
  // 开关可选变量（skinTemperature 地表温度 K、soilMoisture 土壤湿度 m3 m-3）：关则删除，开则建空通道。
  function toggleOptional(name, on) {
    const v = state.working.variables
    if (!on) delete v[name]
    else if (!v[name]) v[name] = name === 'skinTemperature' ? { source: '', unit: 'K' } : { source: '', unit: 'm3 m-3' }
  }

  // ---------- project metadata ----------
  // 作用：调用引擎检查当前元数据（api.checkForcing），结果存入 state.result 供 summaryLines 显示。
  // 输入：metadata（未设置时按 ERA5 数据包产品取 era5-land 或 era5）、projectPath；
  //       站点形态附带 files，网格形态附带 era5.pack。
  async function check() {
    const value = currentValue()
    const pack = getPack()
    const stationForm = isStation()
    const data = { metadata: value === undefined ? (pack?.product === 'era5-land' ? 'era5-land' : 'era5') : value, projectPath: getProject()?.path || '' }
    if (stationForm) data.files = form().forcing?.files || []
    else if (form().era5?.pack) data.pack = form().era5.pack
    state.busy = 'check'; state.status = null; render()
    try {
      state.result = await api.checkForcing(data)
      state.stale = false
    } catch (error) { state.result = null; state.status = { level: 'error', text: error.message } }
    state.busy = ''; render()
  }
  // 作用：把 working 保存为工程元数据文件（api.saveForcingMetadata → forcing-metadata.json），
  //       成功后 state.path 指向该文件、dirty 清零，并把场景里的 metadata 改写为该路径。
  // 前提：工程已保存（有 project.path），否则提示先保存工程。
  async function save() {
    const project = getProject()
    if (!project?.path) { state.status = { level: 'error', text: '请先保存工程，再保存工程元数据' }; render(); return }
    state.busy = 'save'; state.status = null; render()
    try {
      if (!state.working) await load()
      const saved = await api.saveForcingMetadata({ projectPath: project.path, metadata: state.working })
      state.path = saved.path; state.dirty = false
      commit()
      state.status = { level: 'ok', text: `已保存为工程元数据：${saved.path}` }
    } catch (error) { state.status = { level: 'error', text: error.message } }
    state.busy = ''; render()
  }
  // 作用：选择并载入外部元数据文件到 working（视为未保存的修改，需“保存为工程元数据”才写入工程）。
  //       state.token 自增以作废进行中的 load()。
  async function importFile() {
    try {
      const picked = await api.pick('metadata')
      if (picked.cancelled) return
      const read = await api.readForcingMetadata({ import: true, path: picked.path })
      state.token += 1
      state.working = read.metadata
      state.base = isPreset(read.metadata.preset) ? read.metadata.preset : null
      state.type = state.base || 'custom'
      if (state.base) { try { await presetDocument(state.base) } catch {} }
      state.path = ''; state.dirty = true; state.result = null
      state.status = { level: 'ok', text: `已载入：${read.path}（保存为工程元数据后写入工程）` }
      commit(); render()
    } catch (error) { state.status = { level: 'error', text: error.message }; render() }
  }
  // 作用：丢弃所有修改，把 working 还原为所基于的预设（无预设基础则什么也不做）。
  async function reset() {
    if (!state.base) return
    state.path = ''; state.dirty = false; state.working = null; state.result = null
    state.working = { ...(await presetDocument(state.base)), preset: state.base }
    commit(); render()
  }
  // 作用：选择站点 CSV 文件并追加到 temperature.forcing.files（去重）；使旧检查结果过期。
  async function addStationFiles() {
    try {
      const picked = await api.pick('station')
      if (picked.cancelled) return
      const temperature = form()
      const files = [...(temperature.forcing?.files || [])]
      for (const path of picked.paths || [picked.path]) if (!files.includes(path)) files.push(path)
      temperature.forcing = { ...(temperature.forcing || {}), files }
      state.stale = true; commit(); render()
    } catch (error) { state.status = { level: 'error', text: error.message }; render() }
  }

  // ---------- events ----------
  // 作用：处理面板内带 data-fm-action 的点击：校验/保存/载入/还原/添加或移除站点文件与站点登记行。
  //       返回 true 表示事件已由本面板处理。
  function onClick(event) {
    const target = event.target.closest('[data-fm-action]')
    if (!target || !root.contains(target)) return false
    const action = target.dataset.fmAction
    const index = Number(target.dataset.index)
    if (action === 'check') void check()
    else if (action === 'save') void save()
    else if (action === 'import') void importFile()
    else if (action === 'reset') void reset()
    else if (action === 'add-files') void addStationFiles()
    else if (action === 'remove-file') {
      const temperature = form()
      const files = (temperature.forcing?.files || []).filter((_, i) => i !== index)
      temperature.forcing = { ...(temperature.forcing || {}), files }
      state.stale = true; commit(); render()
    } else if (action === 'add-station' && state.working) {
      const list = state.working.station?.stations || []
      setAt(state.working, 'station.stations', [...list, { id: `station-${list.length + 1}` }])
      touch(); render()
    } else if (action === 'remove-station' && state.working) {
      const list = (state.working.station?.stations || []).filter((_, i) => i !== index)
      setAt(state.working, 'station.stations', list.length ? list : undefined)
      touch(); render()
    }
    return true
  }
  // 作用：处理 change 事件：来源类型、辐射时间戳、可选变量开关、长波回退（brutsaert 晴空参数化）、
  //       以及其余字段（交给 onField）。data-fm-open 的事件只拦截不处理。
  function onChange(event) {
    const target = event.target
    if (target.dataset.fmSource !== undefined) { void chooseType(target.value); return true }
    if (target.dataset.fmStamp !== undefined && state.working) { setStamp(target.value); touch(); render(); return true }
    if (target.dataset.fmOptional !== undefined && state.working) { toggleOptional(target.dataset.fmOptional, target.checked); touch(); render(); return true }
    if (target.dataset.fmFallback !== undefined && state.working) { setAt(state.working, 'variables.longwaveDown.fallback', target.checked ? 'brutsaert' : undefined); touch(); render(); return true }
    if (target.dataset.fmOpen !== undefined) return true
    return onField(event)
  }
  // 作用：处理 input 事件：来源/时间戳/可选开关只在 change 时处理，这里仅拦截；其余字段交给 onField。
  function onInput(event) {
    if (event.target.dataset.fmSource !== undefined || event.target.dataset.fmStamp !== undefined || event.target.dataset.fmOptional !== undefined) return true
    return onField(event)
  }
  // 作用：折叠区 <details> 展开/收起时记录 state.open；首次展开且尚未加载时读取元数据。
  async function onToggle(event) {
    const details = event.target.closest?.('[data-fm-details]')
    if (!details) return
    state.open = details.open
    if (state.open && !state.working && !state.loading) await load()
  }

  // ---------- rendering ----------
  // 是否为站点（CSV）形态：优先看 working.form，缺省时按预设 station-csv 判断；未加载时按所选类型判断。
  // 以下为渲染部分：用模板字符串拼 HTML；字段控件通过 data-fm（点号路径）与 data-fm-type 绑定到 working。
  const isStation = () => (state.working ? (state.working.form === 'station' || (!state.working.form && state.working.preset === 'station-csv')) : state.type === 'station-csv')
  // field：生成文本/数值输入框（列表值以逗号分隔显示）；choice：生成下拉框，
  // 当前值不在候选中时追加一项以免丢失（兼容手写的元数据）。
  const field = (path, value, { type = 'text', placeholder = '', title = '', width = '' } = {}) =>
    `<input class="input" data-fm="${path}" data-fm-type="${type}" ${type === 'number' ? 'type="number" step="any"' : 'type="text"'} value="${escapeText(Array.isArray(value) ? value.join(', ') : value ?? '')}"${placeholder ? ` placeholder="${escapeText(placeholder)}"` : ''}${title ? ` title="${escapeText(title)}"` : ''}${width ? ` style="width:${width}"` : ''}>`
  const choice = (path, entries, current, { blank = '' } = {}) => {
    const list = Array.isArray(entries) ? entries.map((item) => [item, item]) : Object.entries(entries)
    const known = list.some(([key]) => key === current)
    return `<select class="select" data-fm="${path}">${blank !== '' ? `<option value=""${current ? '' : ' selected'}>${blank}</option>` : ''}${list.map(([key, label]) => `<option value="${escapeText(key)}"${key === current ? ' selected' : ''}>${escapeText(label)}</option>`).join('')}${current && !known ? `<option value="${escapeText(current)}" selected>${escapeText(current)}</option>` : ''}</select>`
  }

  // 作用：生成“物理量”表格行：气温、湿度、气压、风、向下短波/长波，以及可选的表皮温度、土壤水分。
  // 列：物理量 / 表示方式(type) / 源变量或列名 / 源单位 / 取值类型(kind) / 观测高度 height_m。
  // 说明：单位表随类型联动（湿度类型、辐射 kind）；气压类型为 none 时源变量列留空。
  function variableRows(w) {
    const v = w.variables || {}
    const rows = []
    const channel = (label, path, { unitList, kinds, type, height, mute = false } = {}) => {
      const item = getAt(w, path) || {}
      const kindList = kinds === 'flux' ? FLUX_KINDS : kinds === 'state' ? STATE_KINDS : null
      rows.push(`<tr><td>${label}</td>
        <td>${type ? choice(`${path}.type`, type, item.type) : ''}</td>
        <td>${mute ? '' : field(`${path}.source`, item.source, { type: 'list', placeholder: '变量名 / 列名' })}</td>
        <td>${unitList ? choice(`${path}.unit`, unitList, item.unit) : ''}</td>
        <td>${kindList ? choice(`${path}.kind`, kindList, item.kind || 'instant') : ''}</td>
        <td>${height ? field(`${path}.height_m`, item.height_m, { type: 'number', placeholder: '未声明' }) : ''}</td></tr>`)
    }
    channel('气温', 'variables.airTemperature', { unitList: TEMPERATURE_UNITS, kinds: 'state', height: true })
    const humidity = v.humidity || {}
    channel('湿度', 'variables.humidity', { unitList: HUMIDITY[humidity.type]?.[1] || [], kinds: 'state', type: Object.fromEntries(Object.entries(HUMIDITY).map(([key, info]) => [key, info[0]])), height: true })
    const pressure = v.pressure || {}
    channel('气压', 'variables.pressure', { unitList: PRESSURE_UNITS, kinds: 'state', type: PRESSURE_TYPES, mute: pressure.type === 'none' })
    const wind = v.wind || {}
    rows.push(`<tr><td>风</td><td>${choice('variables.wind.type', WIND_TYPES, wind.type)}</td><td></td><td></td><td></td><td>${field('variables.wind.height_m', wind.height_m, { type: 'number', placeholder: '未声明' })}</td></tr>`)
    if (wind.type === 'components') { channel('　风 u', 'variables.wind.u', { unitList: WIND_UNITS, kinds: 'state' }); channel('　风 v', 'variables.wind.v', { unitList: WIND_UNITS, kinds: 'state' }) }
    else if (wind.type === 'speedDirection') {
      channel('　风速', 'variables.wind.speed', { unitList: WIND_UNITS, kinds: 'state' })
      channel('　风向', 'variables.wind.direction', { unitList: ['deg'], kinds: 'state' })
    } else channel('　风速', 'variables.wind.speed', { unitList: WIND_UNITS, kinds: 'state' })
    for (const [name, label] of [['shortwaveDown', '向下短波'], ['longwaveDown', '向下长波']]) {
      const item = v[name] || {}
      channel(label, `variables.${name}`, { unitList: unitsFor(item.kind), kinds: 'flux' })
    }
    for (const [name, label, units] of [['skinTemperature', '表皮温度（可选）', TEMPERATURE_UNITS], ['soilMoisture', '土壤水分（可选）', SOIL_UNITS]]) {
      const on = Boolean(v[name])
      if (!on) { rows.push(`<tr class="sw-fm-optional"><td>${label}</td><td colspan="5"><label class="sw-check"><input type="checkbox" data-fm-optional="${name}"> 使用</label></td></tr>`); continue }
      const item = v[name]
      rows.push(`<tr><td><label class="sw-check"><input type="checkbox" data-fm-optional="${name}" checked> ${label}</label></td><td></td>
        <td>${field(`variables.${name}.source`, item.source, { type: 'list', placeholder: '变量名 / 列名' })}</td>
        <td>${choice(`variables.${name}.unit`, units, item.unit)}</td>
        <td>${name === 'soilMoisture' && item.unit === 'kg m-2' ? field('variables.soilMoisture.thickness_m', item.thickness_m, { type: 'number', placeholder: '厚度 m' }) : ''}</td><td></td></tr>`)
    }
    return rows.join('')
  }

  // 作用：生成站点形态的附加区块：CSV 文件列表、分隔符与列名映射、站点登记表（id/经纬度/海拔/文件）、
  //       空间插值方式（最近站 / 反距离加权）及其参数。
  function stationSection(w) {
    const files = form().forcing?.files || []
    const stations = w.station?.stations || []
    return `<div class="sw-fm-block"><h4>站点数据</h4>
      <div class="sw-fm-files">${files.length ? files.map((path, index) => `<div class="sw-fm-file" title="${escapeText(path)}"><span>${escapeText(path.split(/[\\/]/).pop())}</span><button class="text-button" type="button" data-fm-action="remove-file" data-index="${index}">移除</button></div>`).join('') : '<p class="sw-hint">尚未选择站点 CSV 文件；也可在下面的站点表里登记各站文件。</p>'}</div>
      <button class="button ghost" type="button" data-fm-action="add-files">选择站点 CSV 文件…</button>
      <div class="sw-fm-grid">
        <label><span>分隔符</span>${choice('station.delimiter', { ',': '逗号 ,', ';': '分号 ;', '\t': '制表符' }, w.station?.delimiter ?? ',')}</label>
        ${[['id', '站点 ID 列'], ['latitude', '纬度列'], ['longitude', '经度列'], ['elevation', '海拔列']].map(([key, label]) => `<label><span>${label}</span>${field(`station.columns.${key}`, w.station?.columns?.[key], { placeholder: '可选' })}</label>`).join('')}
      </div>
      <table class="sw-table sw-fm-table"><thead><tr><th>站点 ID</th><th>纬度 (°)</th><th>经度 (°)</th><th>海拔 (m)</th><th>文件</th><th></th></tr></thead><tbody>
        ${stations.map((item, index) => `<tr>${[['id', 'text'], ['latitude', 'number'], ['longitude', 'number'], ['elevation_m', 'number'], ['file', 'text']].map(([key, type]) => `<td>${field(`station.stations.${index}.${key}`, item[key], { type })}</td>`).join('')}<td><button class="text-button" type="button" data-fm-action="remove-station" data-index="${index}">删除</button></td></tr>`).join('')}
      </tbody></table>
      <button class="button ghost sw-add" type="button" data-fm-action="add-station">添加站点</button>
      <p class="sw-hint">站点经纬度、海拔优先取这里的登记，其次取 CSV 中的对应列；登记表的文件路径相对元数据文件所在目录，也可写绝对路径。</p>
      <div class="sw-fm-grid">
        <label><span>插值方式</span>${choice('spatial.interpolation', { nearest: '最近站', idw: '反距离加权' }, w.spatial?.interpolation || 'idw')}</label>
        <label><span>最大距离（km，0 不限）</span>${field('spatial.maxDistanceKm', w.spatial?.maxDistanceKm, { type: 'number', placeholder: '0' })}</label>
        <label><span>最多站数（≤ 4）</span>${field('spatial.maxStations', w.spatial?.maxStations, { type: 'number', placeholder: '4' })}</label>
        <label><span>距离权重指数</span>${field('spatial.idwPower', w.spatial?.idwPower, { type: 'number', placeholder: '2' })}</label>
      </div></div>`
  }

  // 作用：生成元数据编辑器 HTML：名称与形态、时间（步长、UTC 偏移、辐射时间戳）、物理量表、
  //       站点区块（站点形态）、风/气温高度订正与 DEM 递减率。
  function editor(w) {
    const stationForm = isStation()
    const longwave = w.variables?.longwaveDown || {}
    return `<div class="sw-fm-editor">
      <div class="sw-fm-grid">
        <label><span>元数据名称</span>${field('name', w.name)}</label>
        ${state.type === 'custom' ? `<label><span>数据形态</span>${choice('form', { gridded: '网格 NetCDF', station: '站点 CSV' }, stationForm ? 'station' : 'gridded')}</label>` : ''}
      </div>
      ${w.description ? `<p class="sw-hint">${escapeText(w.description)}</p>` : ''}
      <div class="sw-fm-block"><h4>时间</h4><div class="sw-fm-grid">
        <label><span>时间步长（小时）</span>${field('time.stepHours', w.time?.stepHours, { type: 'number', placeholder: '自动检测' })}</label>
        <label><span>UTC 偏移（小时）</span>${field('time.utcOffsetHours', w.time?.utcOffsetHours, { type: 'number', placeholder: '0' })}</label>
        <label><span>辐射时间戳约定</span><select class="select" data-fm-stamp>${[['', '按元数据'], ...Object.entries(STAMPS)].map(([key, label]) => `<option value="${key}"${stampValue() === key ? ' selected' : ''}>${label}</option>`).join('')}${stampValue() === 'mixed' ? '<option value="mixed" selected>各量不同</option>' : ''}</select></label>
        ${stationForm
    ? `<label><span>时间列名</span>${field('time.column', w.time?.column, { placeholder: 'time' })}</label><label><span>时间格式</span>${choice('time.format', TIME_FORMATS, w.time?.format || 'iso8601')}</label>`
    : `<label><span>时间变量名</span>${field('time.variable', w.time?.variable, { type: 'list', placeholder: 'time' })}</label>
        <label><span>纬度坐标名</span>${field('grid.latitude', w.grid?.latitude, { type: 'list', placeholder: 'lat' })}</label>
        <label><span>经度坐标名</span>${field('grid.longitude', w.grid?.longitude, { type: 'list', placeholder: 'lon' })}</label>`}
      </div></div>
      <div class="sw-fm-block"><h4>物理量</h4>
        <table class="sw-table sw-fm-table"><thead><tr><th>物理量</th><th>表示方式</th><th>源变量 / 列名</th><th>源单位</th><th>取值类型</th><th>高度（m）</th></tr></thead><tbody>${variableRows(w)}</tbody></table>
        <label class="sw-check"><input type="checkbox" data-fm-fallback${longwave.fallback === 'brutsaert' ? ' checked' : ''}> 没有向下长波数据时按晴空参数化估算（不含云，阴天偏低，会给出警告）</label>
      </div>
      ${stationForm ? stationSection(w) : ''}
      <div class="sw-fm-block"><h4>高度订正</h4><div class="sw-fm-grid">
        <label><span>风：订正方式</span>${choice('heightCorrection.wind.mode', { log: '对数律', none: '不订正' }, w.heightCorrection?.wind?.mode || 'log')}</label>
        <label><span>风：目标高度（m）</span>${field('heightCorrection.wind.targetHeight_m', w.heightCorrection?.wind?.targetHeight_m, { type: 'number', placeholder: '10' })}</label>
        <label><span>风：粗糙度（m）</span>${field('heightCorrection.wind.roughness_m', w.heightCorrection?.wind?.roughness_m, { type: 'number', placeholder: '0.03' })}</label>
        <label><span>气温：订正方式</span>${choice('heightCorrection.airTemperature.mode', { none: '不订正', lapse: '递减率' }, w.heightCorrection?.airTemperature?.mode || 'none')}</label>
        <label><span>气温：目标高度（m）</span>${field('heightCorrection.airTemperature.targetHeight_m', w.heightCorrection?.airTemperature?.targetHeight_m, { type: 'number', placeholder: '2' })}</label>
        <label><span>气温：递减率（K/m）</span>${field('heightCorrection.airTemperature.lapseRate_K_per_m', w.heightCorrection?.airTemperature?.lapseRate_K_per_m, { type: 'number', placeholder: '0.0065' })}</label>
        <label><span>DEM 订正递减率（K/km）</span>${field('spatial.lapseRateKPerKm', w.spatial?.lapseRateKPerKm, { type: 'number', placeholder: '6.5' })}</label>
      </div><p class="sw-hint">目标高度是引擎的参考约定（风 10 m、气温 2 m）；源数据高度与之不同时，在此选择订正方式。未声明高度的变量按默认高度处理并给出警告。</p></div>
    </div>`
  }

  // 作用：生成状态与校验结果区：状态消息、过期提示、通过/未通过、错误/警告/备注与 resolved 摘要。
  function resultBlock() {
    const lines = []
    if (state.status) lines.push(`<div class="sw-fm-line" data-level="${state.status.level}">${escapeText(state.status.text)}</div>`)
    const result = state.result
    if (result) {
      if (state.stale) lines.push('<div class="sw-fm-line" data-level="warning">元数据或数据已修改，请重新校验</div>')
      lines.push(`<div class="sw-fm-line" data-level="${result.ok ? 'ok' : 'error'}">${result.ok ? '校验通过' : '校验未通过'}</div>`)
      for (const text of result.errors || []) lines.push(`<div class="sw-fm-line" data-level="error">${escapeText(text)}</div>`)
      for (const text of result.warnings || []) lines.push(`<div class="sw-fm-line" data-level="warning">${escapeText(text)}</div>`)
      for (const text of result.notes || []) lines.push(`<div class="sw-fm-line" data-level="note">${escapeText(text)}</div>`)
      for (const text of summaryLines(result.resolved)) lines.push(`<div class="sw-fm-line" data-level="summary">${escapeText(text)}</div>`)
    }
    return lines.length ? `<div class="sw-fm-result">${lines.join('')}</div>` : ''
  }

  // 作用：重绘 #swForcing：来源下拉、说明、路径提示、可折叠的编辑器与按钮、校验结果。
  // 说明：站点形态时隐藏外层的 ERA5 数据包行并清空其提示；ERA5 缺省且未展开时折叠细节。
  function render() {
    const element = $('#swForcing')
    if (!element) return
    const stationForm = isStation()
    for (const row of root.querySelectorAll('[data-forcing-pack-row]')) row.hidden = stationForm
    const era5 = state.type === 'era5'
    const w = state.working
    const pathNote = state.path && !state.dirty ? `<p class="sw-hint">使用工程元数据文件：${escapeText(state.path)}</p>` : state.path ? '<p class="sw-hint">元数据已修改，尚未保存回工程元数据文件（运行时使用当前修改）。</p>' : ''
    const body = state.loading ? '<p class="sw-hint">正在读取元数据…</p>' : w ? editor(w) : ''
    const buttons = `<div class="sw-fm-actions">
      <button class="button" type="button" data-fm-action="check"${state.busy ? ' disabled' : ''}>${state.busy === 'check' ? '校验中…' : '校验'}</button>
      <button class="button" type="button" data-fm-action="save"${state.busy || !w ? ' disabled' : ''}>保存为工程元数据</button>
      <button class="button ghost" type="button" data-fm-action="import">载入元数据文件…</button>
      ${state.base && w ? '<button class="button ghost" type="button" data-fm-action="reset">还原为预设</button>' : ''}</div>`
    element.innerHTML = `<div class="sw-row"><span>气象强迫来源</span>
        <select class="select" data-fm-source>${SOURCE_TYPES.map(([id, label]) => `<option value="${id}"${state.type === id ? ' selected' : ''}>${label}</option>`).join('')}</select></div>
      <p class="sw-hint">${era5 ? '默认来源，无需填写元数据；换用其他数据源时在这里选择来源并核对各物理量的变量名、单位和观测高度。' : '核对各物理量的变量名、单位、取值类型和观测高度；改了数据源，只要元数据对得上就可以直接使用。'}</p>
      ${pathNote}
      <details class="sw-fm-details" data-fm-details${era5 && !state.open ? '' : ' open'}><summary>${era5 ? '高级：气象强迫元数据细节' : '气象强迫元数据'}</summary>${body}</details>
      ${buttons}${resultBlock()}`
    const info = $('#swEra5Info'); if (info && stationForm) { info.textContent = ''; info.dataset.level = '' }
  }

  return {
    // 对外接口：html/afterRender 供外层嵌入；onInput/onChange/onClick/onToggle 为事件入口（返回是否已处理）；
    // currentDocument 返回当前元数据副本；problems 返回阻止运行的问题；label 返回来源简称。
    html: () => '<div id="swForcing" class="sw-forcing"></div>',
    afterRender() { syncFromForm(); render() },
    onInput, onChange, onClick, onToggle,
    isStation, render,
    currentDocument: () => (state.working ? structuredClone(state.working) : null),
    // Problems that block a run (the pack row of gridded sources is validated by the workspace).
    problems() {
      const list = []
      if (isStation() && !(form().forcing?.files || []).length && !(state.working?.station?.stations || []).some((item) => item?.file)) list.push('气象站点来源需要选择站点 CSV 文件')
      if (state.working && !String(state.working.name || '').trim()) list.push('气象强迫元数据缺少名称')
      return list
    },
    label: () => (isStation() ? '站点 CSV' : state.type === 'era5' ? '' : (SOURCE_TYPES.find(([id]) => id === state.type)?.[1] || ''))
  }
}
