// Weather-forcing metadata panel of the STREAM image workspace (docs/forcing-metadata.md).
// The scene stores only temperature.forcing = { metadata, files }: metadata is undefined (ERA5 default, unchanged behaviour),
// a preset name, a project metadata file path, or an inline object once edited. The editor works on a full copy of the
// metadata document ("working") and writes the smallest equivalent value back into the scene after every edit.
const SOURCE_TYPES = [
  ['era5', 'ERA5'], ['era5-land', 'ERA5-Land'], ['gldas-noah-0.25', 'GLDAS Noah 0.25°'], ['gldas-noah-1.0', 'GLDAS Noah 1°'],
  ['station-csv', '气象站点 CSV'], ['custom', '自定义']
]
const PRESET_IDS = SOURCE_TYPES.filter(([id]) => id !== 'custom').map(([id]) => id)
const TEMPERATURE_UNITS = ['K', 'degC', 'degF']
const PRESSURE_UNITS = ['Pa', 'hPa', 'kPa', 'mmHg', 'inHg']
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
const STATE_KINDS = { instant: '瞬时', previous: '保持到下一记录' }
const FLUX_KINDS = { instant: '瞬时', interval_mean: '时段平均', accumulated: '时段累积', cumulative: '自重置累计' }
const STAMPS = { instant: '时刻', interval_start: '时段起点', interval_end: '时段终点', interval_mid: '时段中点' }
const TIME_FORMATS = { iso8601: 'ISO 8601', epoch_seconds: '纪元秒', epoch_milliseconds: '纪元毫秒', yyyymmddhh: 'YYYYMMDDHH', yyyymmddhhmm: 'YYYYMMDDHHMM' }
const FORM_NAMES = { gridded: '网格', station: '站点' }
const RADIATION = ['shortwaveDown', 'longwaveDown']

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

export function createForcingPanel({ api, getForm, getProject, getPack, markDirty, root }) {
  const $ = (selector) => root.querySelector(selector)
  const state = {
    initialized: false, committedKey: '', type: 'era5', base: 'era5', working: null, path: '', dirty: false, open: false,
    loading: false, token: 0, result: null, stale: false, busy: '', status: null, presets: new Map()
  }
  const form = () => getForm().temperature

  // ---------- state <-> scene ----------
  const samePreset = (document, name) => state.presets.has(name) && canonical({ ...document, preset: undefined }) === canonical({ ...state.presets.get(name), preset: undefined })
  function currentValue() {
    if (!state.working) return form().forcing?.metadata
    if (state.path && !state.dirty) return state.path
    if (state.base && samePreset(state.working, state.base)) return state.base === 'era5' ? undefined : state.base
    return structuredClone(state.working)
  }
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

  async function presetDocument(name) {
    if (!state.presets.has(name)) state.presets.set(name, (await api.forcingPreset(name)).metadata)
    return structuredClone(state.presets.get(name))
  }
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
  const touch = () => { state.dirty = true; state.stale = true; commit() }
  function fixUnit(path, units, fallback) {
    const current = getAt(state.working, `${path}.unit`)
    if (!units.includes(current)) setAt(state.working, `${path}.unit`, fallback ?? units[0])
  }
  function setField(target) {
    const kind = target.dataset.fmType || 'text'
    const raw = target.value
    let value
    if (kind === 'number') { const parsed = Number(raw); value = raw.trim() !== '' && Number.isFinite(parsed) ? parsed : undefined }
    else if (kind === 'list') { const parts = raw.split(',').map((item) => item.trim()).filter(Boolean); value = parts.length > 1 ? parts : parts[0] }
    else value = raw === '' ? undefined : raw
    setAt(state.working, target.dataset.fm, value)
  }
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
  const stampValue = () => {
    const stamps = RADIATION.map((key) => state.working?.variables?.[key]?.stamp)
    return stamps[0] === stamps[1] ? stamps[0] || '' : 'mixed'
  }
  function setStamp(value) {
    for (const key of RADIATION) if (state.working.variables?.[key]) setAt(state.working, `variables.${key}.stamp`, value || undefined)
    if (state.working.variables?.precipitation?.stamp !== undefined && value) state.working.variables.precipitation.stamp = value
  }
  function toggleOptional(name, on) {
    const v = state.working.variables
    if (!on) delete v[name]
    else if (!v[name]) v[name] = name === 'skinTemperature' ? { source: '', unit: 'K' } : { source: '', unit: 'm3 m-3' }
  }

  // ---------- project metadata ----------
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
  async function reset() {
    if (!state.base) return
    state.path = ''; state.dirty = false; state.working = null; state.result = null
    state.working = { ...(await presetDocument(state.base)), preset: state.base }
    commit(); render()
  }
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
  function onChange(event) {
    const target = event.target
    if (target.dataset.fmSource !== undefined) { void chooseType(target.value); return true }
    if (target.dataset.fmStamp !== undefined && state.working) { setStamp(target.value); touch(); render(); return true }
    if (target.dataset.fmOptional !== undefined && state.working) { toggleOptional(target.dataset.fmOptional, target.checked); touch(); render(); return true }
    if (target.dataset.fmFallback !== undefined && state.working) { setAt(state.working, 'variables.longwaveDown.fallback', target.checked ? 'brutsaert' : undefined); touch(); render(); return true }
    if (target.dataset.fmOpen !== undefined) return true
    return onField(event)
  }
  function onInput(event) {
    if (event.target.dataset.fmSource !== undefined || event.target.dataset.fmStamp !== undefined || event.target.dataset.fmOptional !== undefined) return true
    return onField(event)
  }
  async function onToggle(event) {
    const details = event.target.closest?.('[data-fm-details]')
    if (!details) return
    state.open = details.open
    if (state.open && !state.working && !state.loading) await load()
  }

  // ---------- rendering ----------
  const isStation = () => (state.working ? (state.working.form === 'station' || (!state.working.form && state.working.preset === 'station-csv')) : state.type === 'station-csv')
  const field = (path, value, { type = 'text', placeholder = '', title = '', width = '' } = {}) =>
    `<input class="input" data-fm="${path}" data-fm-type="${type}" ${type === 'number' ? 'type="number" step="any"' : 'type="text"'} value="${escapeText(Array.isArray(value) ? value.join(', ') : value ?? '')}"${placeholder ? ` placeholder="${escapeText(placeholder)}"` : ''}${title ? ` title="${escapeText(title)}"` : ''}${width ? ` style="width:${width}"` : ''}>`
  const choice = (path, entries, current, { blank = '' } = {}) => {
    const list = Array.isArray(entries) ? entries.map((item) => [item, item]) : Object.entries(entries)
    const known = list.some(([key]) => key === current)
    return `<select class="select" data-fm="${path}">${blank !== '' ? `<option value=""${current ? '' : ' selected'}>${blank}</option>` : ''}${list.map(([key, label]) => `<option value="${escapeText(key)}"${key === current ? ' selected' : ''}>${escapeText(label)}</option>`).join('')}${current && !known ? `<option value="${escapeText(current)}" selected>${escapeText(current)}</option>` : ''}</select>`
  }

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
    : `<label><span>时间变量名</span>${field('time.variable', w.time?.variable, { type: 'list', placeholder: 'time' })}</label>`}
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
    html: () => '<div id="swForcing" class="sw-forcing"></div>',
    afterRender() { syncFromForm(); render() },
    onInput, onChange, onClick, onToggle,
    isStation, render,
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
