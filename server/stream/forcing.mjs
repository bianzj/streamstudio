// ==========================================================================
// 文件：forcing.mjs
// 功能：STREAM 服务端的气象强迫（forcing）元数据处理：把工程字段 temperature.forcing 校验并
//   转成引擎配置里的 era5 / forcing 块，同时提供 /api/stream/forcing/* 路由共用的路径与
//   大小规则（引擎一侧的规则见 docs/forcing-metadata.md）
// 主要模块：
//   1. 常量与判断：预设名 FORCING_PRESETS、大小上限、isPresetName、looksLikeMetadataPath、isInside
//   2. createForcingRules 工厂内的路径与文件校验：absolutePath、existingFile、packFile、
//      stationFiles、metadataFile
//   3. 元数据文档校验：checkDocument、assertSafeStationFiles、assertInlineSize、formOf
//   4. resolveMetadata（元数据来源解析）与 buildForcing（生成引擎配置块）
//   5. importMetadataFile：编辑器导入任意位置的元数据文件（只读回编辑器，不交给引擎）
// 数据流：config.mjs 的 buildEngineConfig 在 EB 模式调用 buildForcing → { era5?, forcing? }；
//   路由层使用 resolveMetadata / importMetadataFile 等做预检查
// 与其他文件的关系：被 server/stream/config.mjs 引用（looksLikeMetadataPath 也用于路径改写）；
//   元数据语义见 docs/forcing-metadata.md；数据包索引由 stream era5-register / forcing-register 生成
// 约定：元数据可以是预设名、.json 文件路径或内联对象；形态 form 为 'gridded'（网格）或
//   'station'（站点 CSV）；路径中不得含 ".." 或 NUL
// ==========================================================================

// Weather-forcing metadata plumbing for the STREAM server: project field (temperature.forcing) -> engine config, plus the
// path / size rules shared by the /api/stream/forcing/* routes. Engine rules live in docs/forcing-metadata.md.
import { existsSync, readFileSync, realpathSync, statSync } from 'node:fs'
import { isAbsolute, relative, resolve, sep } from 'node:path'

// 内置预设名（与 README 列出的预设一致）、元数据文件名、
// 内联元数据与元数据文件的字节上限、站点文件个数上限；PARENT_SEGMENT 用于拦截 ".." 路径段。
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

// 作用：判断 child 是否位于 parent 目录之内（含相同路径），用于限制元数据文件的位置。
export function isInside(parent, child) {
  const relation = relative(resolve(parent), resolve(child))
  return relation === '' || (relation !== '..' && !relation.startsWith(`..${sep}`) && !isAbsolute(relation))
}

// 作用：优先取真实路径（解析符号链接），失败（如不存在）时退回普通绝对路径。
function realOrResolved(path) {
  try { return realpathSync(path) } catch { return resolve(path) }
}

// 模块：强迫规则工厂。normalizeHostPath 把主机路径规整为本机形式，windowsPath 用于错误信息
// 与回写给引擎的路径。返回值汇总了本文件所有对外函数。clean 去掉首尾空白和引号。
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

  // 作用：内联元数据序列化后不得超过 MAX_INLINE_METADATA_BYTES（64 KiB），防止请求过大。
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

  // 作用：元数据文档的基本形态检查：必须是 JSON 对象，kind（若有）为
  // stream-forcing-metadata，preset（若有）为已知预设，站点文件路径安全；通过则原样返回。
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

  // 作用：校验气象数据包索引：绝对路径的已存在 .json（≤ 64 MiB），且 kind 为
  // stream-era5-pack 或 stream-forcing-pack；返回绝对路径。
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

  // 作用：校验站点 CSV 文件列表（最多 64 个，空项忽略），逐个要求绝对路径且存在；
  // 返回 Windows 形式路径数组。
  function stationFiles(list) {
    if (list === undefined || list === null) return []
    if (!Array.isArray(list)) throw new Error('站点文件（forcing.files）须为数组')
    if (list.length > MAX_FORCING_FILES) throw new Error(`站点文件不能超过 ${MAX_FORCING_FILES} 个`)
    return list.filter((item) => String(item ?? '').trim() !== '').map((item) => windowsPath(existingFile(item, '站点数据文件')))
  }

  // 数据形态：文档显式写了 form 就用它，否则预设为 station-csv 时是 'station'，其余按 'gridded'。
  const formOf = (document) => document?.form === 'station' || document?.form === 'gridded' ? document.form : document?.preset === 'station-csv' ? 'station' : 'gridded'

  // 作用：把 raw 元数据（预设名/文件路径/内联对象/空）解析成 { metadata, document, form }。
  // 说明：内联对象会深拷贝；文件路径只允许在工程目录、数据包目录或 directories 给出的目录内
  // （由 metadataFile 检查），避免读取任意位置的文件。
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

  // 步骤：1) 读取 forcing 块与旧字段 era5.pack、站点文件、元数据；2) 校验数据包与元数据；
  // 3) 站点形态必须有站点 CSV（或元数据已登记 station.stations[].file），网格形态必须有数据包；
  // 4) forcing.pack 与旧字段 era5.pack 同时存在时以 forcing 为准（与引擎一致），只输出 forcing 块；
  //    只写了旧字段 era5.pack（forcing 里没有 pack）时输出 era5 块；其余写入 forcing 块（pack/files/metadata）。
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
    // 引擎（models/stream/app/config.cpp）取 pack 的规则是：forcing.pack 非空就用它，否则才回退到 era5.pack。
    // 所以两者同时存在时 forcing 优先，era5 块对引擎毫无作用，这里不再输出（避免重复且互相矛盾的两份路径，
    // 也不再校验被忽略的旧字段）；只有旧字段单独存在（或 forcing 里没写 pack）时才输出 era5 块让引擎回退使用。
    if (legacyPack && !forcingPack) out.era5 = { pack: windowsPath(packPath) }
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
