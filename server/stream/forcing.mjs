// Weather-forcing metadata plumbing for the STREAM server: project field (temperature.forcing) -> engine config, plus the
// path / size rules shared by the /api/stream/forcing/* routes. Engine rules live in docs/forcing-metadata.md.
import { existsSync, readFileSync, realpathSync, statSync } from 'node:fs'
import { isAbsolute, relative, resolve, sep } from 'node:path'

export const FORCING_PRESETS = ['era5', 'era5-land', 'gldas-noah-0.25', 'gldas-noah-1.0', 'station-csv']
export const FORCING_METADATA_FILE = 'forcing-metadata.json'
export const MAX_INLINE_METADATA_BYTES = 64 * 1024     // inline object inside a project / request
export const MAX_METADATA_FILE_BYTES = 256 * 1024      // metadata file on disk
export const MAX_FORCING_FILES = 64
const PARENT_SEGMENT = /(^|[\\/])\.\.([\\/]|$)/

export const isPlainObject = (value) => Boolean(value) && typeof value === 'object' && !Array.isArray(value)
export const isPresetName = (value) => typeof value === 'string' && FORCING_PRESETS.includes(value.trim())
// A string metadata value is a path (not a preset name) when it has a separator or ends in .json.
export const looksLikeMetadataPath = (value) => typeof value === 'string' && value.trim() !== '' && !isPresetName(value) && /[\\/]|\.json$/i.test(value.trim())

export function isInside(parent, child) {
  const relation = relative(resolve(parent), resolve(child))
  return relation === '' || (relation !== '..' && !relation.startsWith(`..${sep}`) && !isAbsolute(relation))
}

function realOrResolved(path) {
  try { return realpathSync(path) } catch { return resolve(path) }
}

export function createForcingRules({ normalizeHostPath, windowsPath }) {
  const clean = (value) => normalizeHostPath(String(value ?? '').trim().replace(/^"|"$/g, ''))

  // Absolute, normalised, no ".." segments in what the user typed, no NUL.
  function absolutePath(value, label) {
    const text = clean(value)
    if (!text) throw new Error(`请选择${label}`)
    if (text.includes('\0') || PARENT_SEGMENT.test(text)) throw new Error(`${label}路径不能包含 ".."`)
    if (!isAbsolute(text)) throw new Error(`${label}须填写绝对路径`)
    return resolve(text)
  }

  function existingFile(value, label) {
    const path = absolutePath(value, label)
    if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`${label}不存在：${windowsPath(path)}`)
    return path
  }

  function assertInlineSize(object) {
    const bytes = Buffer.byteLength(JSON.stringify(object), 'utf8')
    if (bytes > MAX_INLINE_METADATA_BYTES) throw new Error(`气象强迫元数据过大（${bytes} 字节，上限 ${MAX_INLINE_METADATA_BYTES}）`)
  }

  // Station registry files may be relative to the metadata file, but never climb out of it.
  function assertSafeStationFiles(document) {
    const stations = document?.station?.stations
    if (stations === undefined) return
    if (!Array.isArray(stations) || stations.length > 256) throw new Error('气象强迫元数据 station.stations 须为不超过 256 项的数组')
    for (const entry of stations) {
      if (!isPlainObject(entry)) throw new Error('气象强迫元数据 station.stations 的每一项须为对象')
      if (entry.file === undefined || entry.file === null || entry.file === '') continue
      const file = String(entry.file)
      if (file.includes('\0') || PARENT_SEGMENT.test(file)) throw new Error(`站点文件路径不能包含 "..": ${file}`)
    }
  }

  function checkDocument(document, label) {
    if (!isPlainObject(document)) throw new Error(`${label}必须是 JSON 对象`)
    if (document.kind !== undefined && document.kind !== 'stream-forcing-metadata') throw new Error(`${label}的 kind 必须是 "stream-forcing-metadata"`)
    if (document.preset !== undefined && !isPresetName(document.preset)) throw new Error(`${label}的 preset "${document.preset}" 未知；可用：${FORCING_PRESETS.join('、')}`)
    assertSafeStationFiles(document)
    return document
  }

  // Metadata file: absolute .json inside one of the allowed directories (project folder, forcing-metadata folder, pack folder).
  function metadataFile(value, allowedDirectories) {
    const path = absolutePath(value, '气象强迫元数据文件')
    if (!/\.json$/i.test(path)) throw new Error('气象强迫元数据文件须为 .json')
    const real = realOrResolved(path)
    const allowed = allowedDirectories.filter(Boolean).some((directory) => isInside(realOrResolved(directory), real))
    if (!allowed) throw new Error(`气象强迫元数据文件只能位于工程目录、元数据目录或数据包所在目录内：${windowsPath(path)}`)
    if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`气象强迫元数据文件不存在：${windowsPath(path)}`)
    if (statSync(path).size > MAX_METADATA_FILE_BYTES) throw new Error(`气象强迫元数据文件过大（上限 ${MAX_METADATA_FILE_BYTES} 字节）`)
    let document
    try { document = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) } catch { throw new Error(`气象强迫元数据文件无法解析为 JSON：${windowsPath(path)}`) }
    checkDocument(document, '气象强迫元数据文件')
    return { path, document }
  }

  function packFile(value) {
    // Pack indexes keep the long-standing rule (any absolute existing file); it must still be a pack index.
    const text = clean(value)
    if (!text || text.includes('\0') || !isAbsolute(text)) throw new Error('请选择气象数据包')
    const path = resolve(text)
    if (!existsSync(path) || !statSync(path).isFile()) throw new Error(`气象数据包不存在：${windowsPath(path)}`)
    if (!/\.json$/i.test(path)) throw new Error('气象数据包索引须为 .json')
    if (statSync(path).size > 64 * 1024 * 1024) throw new Error('气象数据包索引过大')
    let document
    try { document = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) } catch { throw new Error(`气象数据包索引无法解析：${windowsPath(path)}`) }
    if (!['stream-era5-pack', 'stream-forcing-pack'].includes(document?.kind)) throw new Error(`不是气象数据包索引：${windowsPath(path)}`)
    return path
  }

  function stationFiles(list) {
    if (list === undefined || list === null) return []
    if (!Array.isArray(list)) throw new Error('站点文件（forcing.files）须为数组')
    if (list.length > MAX_FORCING_FILES) throw new Error(`站点文件不能超过 ${MAX_FORCING_FILES} 个`)
    return list.filter((item) => String(item ?? '').trim() !== '').map((item) => windowsPath(existingFile(item, '站点数据文件')))
  }

  const formOf = (document) => document?.form === 'station' || document?.form === 'gridded' ? document.form : document?.preset === 'station-csv' ? 'station' : 'gridded'

  // Returns { metadata, document, form }; metadata is what the engine receives (preset name, file path or inline object).
  function resolveMetadata(raw, { projectPath = '', packPath = '', directories = [] } = {}) {
    if (raw === undefined || raw === null || raw === '') return { metadata: undefined, document: null, form: 'gridded' }
    if (isPlainObject(raw)) {
      assertInlineSize(raw)
      checkDocument(raw, '气象强迫元数据')
      return { metadata: structuredClone(raw), document: raw, form: formOf(raw) }
    }
    if (typeof raw !== 'string') throw new Error('气象强迫元数据须为预设名、元数据文件路径或对象')
    const text = raw.trim()
    if (isPresetName(text)) return { metadata: text, document: { preset: text }, form: text === 'station-csv' ? 'station' : 'gridded' }
    if (!looksLikeMetadataPath(text)) throw new Error(`未知的气象强迫预设 "${text}"；可用：${FORCING_PRESETS.join('、')}`)
    const projectDirectory = projectPath ? resolve(clean(projectPath), '..') : ''
    const packDirectory = packPath ? resolve(packPath, '..') : ''
    const { path, document } = metadataFile(text, [projectDirectory, packDirectory, ...directories])
    return { metadata: windowsPath(path), document, form: formOf(document) }
  }

  // temperature input (project form) -> { era5?, forcing? } for the engine config.
  function buildForcing(temperatureInput, { projectPath = '', directories = [] } = {}) {
    const input = temperatureInput || {}
    const forcing = isPlainObject(input.forcing) ? input.forcing : {}
    const legacyPack = String(input.era5?.pack ?? '').trim()
    const forcingPack = String(forcing.pack ?? '').trim()
    const raw = forcing.metadata ?? input.forcingMetadata
    const files = stationFiles(forcing.files)
    const packValue = forcingPack || legacyPack
    const packPath = packValue ? packFile(packValue) : ''
    const resolved = resolveMetadata(raw, { projectPath, packPath, directories })
    const registeredStations = Array.isArray(resolved.document?.station?.stations) && resolved.document.station.stations.some((entry) => entry?.file)
    if (resolved.form === 'station') {
      if (!files.length && !registeredStations) throw new Error('站点形态的气象强迫需要站点 CSV 文件（或在元数据 station.stations 中登记 file）')
    } else if (!packPath) throw new Error('请选择气象数据包')
    const out = {}
    if (legacyPack) out.era5 = { pack: windowsPath(packFile(legacyPack)) }
    const block = {}
    if (forcingPack) block.pack = windowsPath(packPath)
    if (files.length) block.files = files
    if (resolved.metadata !== undefined) block.metadata = resolved.metadata
    if (Object.keys(block).length) out.forcing = block
    return out
  }

  // Import: a metadata document the user picked anywhere on disk is only read back to the editor (never handed to the
  // engine from here), so it needs no directory rule - just the size cap and the shape of a metadata document.
  function importMetadataFile(value) {
    const path = existingFile(value, '气象强迫元数据文件')
    if (!/\.json$/i.test(path)) throw new Error('气象强迫元数据文件须为 .json')
    if (statSync(path).size > MAX_METADATA_FILE_BYTES) throw new Error(`气象强迫元数据文件过大（上限 ${MAX_METADATA_FILE_BYTES} 字节）`)
    let document
    try { document = JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, '')) } catch { throw new Error(`气象强迫元数据文件无法解析为 JSON：${windowsPath(path)}`) }
    if (!isPlainObject(document) || (document.kind !== 'stream-forcing-metadata' && !isPlainObject(document.variables) && document.preset === undefined))
      throw new Error(`不是气象强迫元数据文件：${windowsPath(path)}`)
    checkDocument(document, '气象强迫元数据文件')
    return { path, document }
  }

  return { buildForcing, resolveMetadata, readMetadataFile: metadataFile, importMetadataFile, checkDocument, existingFile, packFile, stationFiles, absolutePath, assertInlineSize, formOf }
}
