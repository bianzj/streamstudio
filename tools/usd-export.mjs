// ==========================================================================
// 文件：usd-export.mjs
// 功能：把 HiStream/StreamStudio 工程导出为 ASCII OpenUSD（.usda）场景，供编辑器预览与外部 DCC 工具查看；
//   同时在根层 customLayerData 中保存完整工程 JSON 与每个实例的放置记录，以便 usd-import.mjs 回读。
//   坐标轴与编辑器预览一致：Y 向上，X = 北，Z = 东（单位 m，metersPerUnit = 1）。
// 主要模块：
//   1. 文本写出基础：LayerWriter（分块缓冲写文件）、usdString 转义、number/tuple 数值格式化
//      （6 位小数）、uniqueNames（合法且不重名的 prim 名）、writeArray（大数组分块输出）
//   2. 输入解析：readPlacements（位置文件：北、东、高、缩放、旋转）、parseObj（按组收集面与顶点色）
//   3. 原型资产缓存：prototypeAsset 为每个 OBJ 写一份 .usda + .json 元数据，按路径/大小/mtime 缓存
//   4. 预览外观：previewColor 按光谱名/类型挑一个仅用于显示的 UsdPreviewSurface 漫反射色
//   5. sunTransform：由太阳天顶角/方位角构造 DistantLight 的朝向矩阵
//   6. exportProjectUsd：写根层——Prototypes 类（OBJ 引用或基本体）、World/Ground（平面或 DEM 网格）、
//      Sun、Objects（逐放置实例化）、Looks（材料）；最后清理不再使用的缓存资产
// 数据流：project（工程 JSON）+ options（输出路径、资产目录、resolveAssetPath、heightAt）
//   → 原型资产 .usda（assetsDirectory）与根层 .usda（先写 .tmp 再原子改名）→ 返回统计信息。
// 约定：方位角从北（+X）向东（+Z）顺时针；放置旋转 rotation 以度计，USD 中取负号写入 rotateY；
//   heightAt(north, east) 返回相对 DEM 最小值的地形高度（场景局部坐标）；
//   导出格式标识 histream-usd-v1，原型资产布局版本 ASSET_VERSION 变化时缓存重建。
// 关系：与 usd-import.mjs 成对使用；贴地判断来自 src/renderer/src/project-schema.js
//   （isGroundedObject、groundedPlacementHeight、stringifyProject）；列顺序与 tools/runtime-scene.mjs 一致。
// ==========================================================================

// Exports a HiStream project as ASCII OpenUSD in editor-preview axes (Y-up, X = north, Z = east).
import { createHash } from 'node:crypto'
import { closeSync, existsSync, mkdirSync, openSync, readdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync, writeSync } from 'node:fs'
import { basename, dirname, extname, join, relative, resolve } from 'node:path'
import { groundedPlacementHeight, isGroundedObject, stringifyProject } from '../src/renderer/src/project-schema.js'

export const USD_EXPORT_FORMAT = 'histream-usd-v1'
// 常量：ARRAY_CHUNK = 每次拼接写出的数组元素个数（避免一次拼出超长字符串）；
//   ASSET_FILE = 缓存资产文件名模式（名称-12 位十六进制键.usda/.json），清理缓存时只删匹配的文件。
const ARRAY_CHUNK = 8192
// Bump when the prototype asset layout changes so cached assets are rebuilt.
const ASSET_VERSION = 2
const ASSET_FILE = /^[A-Za-z0-9_]+-[0-9a-f]{12}\.(usda|json)$/

// 模块：LayerWriter —— 带缓冲的文本写出器；累计超过 2^23 个字符就落盘一次，
//   避免大场景在内存里拼接整份 .usda。close() 会先 flush。
class LayerWriter {
  constructor(path) {
    this.fd = openSync(path, 'w')
    this.parts = []
    this.length = 0
  }

  write(text) {
    this.parts.push(text)
    this.length += text.length
    if (this.length > 1 << 23) this.flush()
  }

  flush() {
    if (this.parts.length) writeSync(this.fd, this.parts.join(''))
    this.parts = []
    this.length = 0
  }

  close() {
    this.flush()
    closeSync(this.fd)
  }
}

// 作用：把任意值转成 USD 字符串字面量：转义反斜杠、双引号、换行、制表符，其他控制字符写成 \xNN。
function usdString(value) {
  let result = '"'
  for (const character of String(value ?? '')) {
    const code = character.codePointAt(0)
    if (character === '\\') result += '\\\\'
    else if (character === '"') result += '\\"'
    else if (character === '\n') result += '\\n'
    else if (character === '\r') result += '\\r'
    else if (character === '\t') result += '\\t'
    else if (code < 0x20) result += `\\x${code.toString(16).padStart(2, '0')}`
    else result += character
  }
  return result + '"'
}

// 作用：数值格式化——四舍五入到 1e-6，非有限值与 -0 都写成 0，保证输出稳定且可往返。
function number(value) {
  const rounded = Math.round(Number(value) * 1e6) / 1e6
  return Number.isFinite(rounded) && !Object.is(rounded, -0) ? String(rounded) : '0'
}

function tuple(values) {
  return `(${Array.from(values, number).join(', ')})`
}

// 作用：返回命名函数 (文本, 备用名) → 合法 USD 标识符：非字母数字下划线替换为 _，
//   合并连续下划线，首字符不是字母/下划线则前缀 _，重名时追加 _1、_2……（每次调用独立一组）。
function uniqueNames() {
  const used = new Set()
  return (text, fallback) => {
    let name = String(text ?? '').replace(/[^A-Za-z0-9_]/g, '_').replace(/_+/g, '_').replace(/^_+|_+$/g, '')
    if (!name) name = fallback
    if (!/^[A-Za-z_]/.test(name)) name = `_${name}`
    let candidate = name
    for (let suffix = 1; used.has(candidate); suffix += 1) candidate = `${name}_${suffix}`
    used.add(candidate)
    return candidate
  }
}

// 模块：位置文件解析。每行至少 3 个数（北、东、高，m），缩放缺省 1，旋转缺省 0（度）；
//   # 与 // 开头的注释、空行、不足 3 个有限数的行被忽略；分隔符为空白或逗号。
// Same column order as tools/runtime-scene.mjs: north, east, height, scale, rotation.
function readPlacements(content) {
  const placements = []
  for (const sourceLine of String(content || '').replace(/^\uFEFF/, '').split(/\r?\n/)) {
    const line = sourceLine.replace(/#.*/, '').trim()
    if (!line || line.startsWith('//')) continue
    const values = line.split(/[\s,]+/).map(Number)
    if (values.length < 3 || !values.slice(0, 3).every(Number.isFinite)) continue
    placements.push({
      north: values[0], east: values[1], height: values[2],
      scale: Number.isFinite(values[3]) ? values[3] : 1,
      rotation: Number.isFinite(values[4]) ? values[4] : 0
    })
  }
  return placements
}

// 模块：OBJ 解析。输出 points（x,y,z 平铺）、colors（仅当每个顶点都带 RGB 时保留）、
//   groups（组名 → 每面顶点数 counts 与 0 起始的顶点索引 indices）与包围盒 bounds。
//   面索引支持负数（相对当前顶点数）；有效顶点少于 3 的面丢弃；不做三角化（保持 n 边形）。
// Groups follow parseObjGeometry in tools/runtime-scene.mjs, so mesh names
// match the `meshes[].name` entries the GUI assigns materials to.
function parseObj(content) {
  const points = []
  const colors = []
  let everyVertexColored = true
  const groups = new Map()
  let activeGroup = ''
  const groupFor = (name) => {
    if (!groups.has(name)) groups.set(name, { counts: [], indices: [] })
    return groups.get(name)
  }
  const bounds = { minX: Infinity, maxX: -Infinity, minY: Infinity, maxY: -Infinity, minZ: Infinity, maxZ: -Infinity }
  for (const sourceLine of content.split(/\r?\n/)) {
    const cells = sourceLine.trim().split(/\s+/)
    const record = cells[0]
    if ((record === 'o' || record === 'g') && cells[1]) {
      activeGroup = cells[1]
    } else if (record === 'usemtl' && !activeGroup && cells[1]) {
      activeGroup = cells[1]
    } else if (record === 'v' && cells.length >= 4) {
      const x = Number(cells[1]), y = Number(cells[2]), z = Number(cells[3])
      if (![x, y, z].every(Number.isFinite)) continue
      points.push(x, y, z)
      bounds.minX = Math.min(bounds.minX, x); bounds.maxX = Math.max(bounds.maxX, x)
      bounds.minY = Math.min(bounds.minY, y); bounds.maxY = Math.max(bounds.maxY, y)
      bounds.minZ = Math.min(bounds.minZ, z); bounds.maxZ = Math.max(bounds.maxZ, z)
      const rgb = cells.length >= 7 ? cells.slice(4, 7).map(Number) : null
      if (rgb && rgb.every(Number.isFinite)) colors.push(...rgb)
      else { everyVertexColored = false; colors.push(1, 1, 1) }
    } else if (record === 'f' && cells.length >= 4) {
      const vertexCount = points.length / 3
      const face = cells.slice(1).map((cell) => {
        const raw = Number(cell.split('/')[0])
        return raw > 0 ? raw - 1 : vertexCount + raw
      }).filter((index) => Number.isInteger(index) && index >= 0 && index < vertexCount)
      if (face.length < 3) continue
      const group = groupFor(activeGroup)
      group.counts.push(face.length)
      for (const index of face) group.indices.push(index)
    }
  }
  return { points, colors: everyVertexColored && colors.length ? colors : null, groups, bounds }
}

// 作用：把平铺数组写成 USD 数组属性；width 为每元素分量数（1 = 标量，3 = 元组）；
//   按 ARRAY_CHUNK 分块拼接，suffix 用于追加元数据块（如 primvar 的 interpolation）。
function writeArray(writer, type, name, values, width, suffix = '') {
  writer.write(`${type} ${name} = [`)
  const count = values.length / width
  for (let start = 0; start < count; start += ARRAY_CHUNK) {
    const end = Math.min(count, start + ARRAY_CHUNK)
    const items = []
    for (let index = start; index < end; index += 1) {
      items.push(width === 1 ? String(values[index]) : tuple(values.slice(index * width, index * width + width)))
    }
    writer.write((start ? ', ' : '') + items.join(', '))
  }
  writer.write(`]${suffix}\n`)
}

// 作用：把键值对写成 `custom ... histream:<键>` 自定义属性行（布尔 → bool 0/1，数值 → double，
//   其余 → string）；undefined / null / 空串的项跳过。这些属性供 usd-import.mjs 回读材料名等信息。
function attributeLines(indent, attributes) {
  return Object.entries(attributes)
    .filter(([, value]) => value !== undefined && value !== null && value !== '')
    .map(([key, value]) => typeof value === 'boolean'
      ? `${indent}custom bool histream:${key} = ${value ? 1 : 0}\n`
      : typeof value === 'number'
        ? `${indent}custom double histream:${key} = ${number(value)}\n`
        : `${indent}custom string histream:${key} = ${usdString(value)}\n`)
    .join('')
}

// 作用：为 UsdPreviewSurface 选一个仅用于显示的漫反射色（不是辐射传输用的光谱）。
//   按光谱名/模型关键词（叶、木、土、水、光伏）→ 对象类型（建筑、车、船、人）→ 首个反射率值
//   （灰度限幅在 0.2–0.85）的顺序判断；数值是经验配色，不对应测量值。
function previewColor(spectrum, item) {
  const name = `${spectrum?.name || ''} ${spectrum?.label || ''}`.toLowerCase()
  const model = String(spectrum?.model || '').toLowerCase()
  if (model === 'prospect' || /leaf|needle|grass|crop/.test(name)) return [0.18, 0.42, 0.14]
  if (/wood|bark|trunk|branch/.test(name)) return [0.36, 0.25, 0.16]
  if (model === 'bsm' || /soil/.test(name)) return [0.46, 0.38, 0.28]
  if (/water/.test(name)) return [0.24, 0.55, 0.67]
  if (/pv|solar/.test(name)) return [0.05, 0.12, 0.22]
  const type = String(item?.type || '').toLowerCase()
  if (type.includes('build')) return [0.49, 0.52, 0.5]
  if (type.includes('vehicle')) return [0.84, 0.61, 0.2]
  if (type.includes('ship')) return [0.33, 0.48, 0.58]
  if (type.includes('human')) return [0.24, 0.44, 0.84]
  const reflectance = Number(String(spectrum?.reflectance ?? '').split(/[\s,]+/)[0])
  const gray = Number.isFinite(reflectance) ? Math.min(0.85, Math.max(0.2, 0.25 + reflectance)) : 0.5
  return [gray, gray, gray]
}

// 作用：取某网格组对应的材料名集合：优先用 item.meshes 中同名条目的设置，
//   只有一个条目时用它，否则回退到对象本身的 spectralName/thermalName/materialName 等。
function meshMaterial(item, groupName) {
  const mesh = (item.meshes || []).find((entry) => entry.name === groupName)
    || (item.meshes?.length === 1 ? item.meshes[0] : null)
  return {
    spectralName: mesh?.spectralName ?? item.spectralName,
    thermalName: mesh?.thermalName ?? item.thermalName,
    materialName: mesh?.materialName ?? item.materialName,
    canopyName: mesh?.canopyName ?? item.canopyName,
    energyModel: mesh?.energyModel ?? item.energyModel,
    specularEnabled: mesh?.specularEnabled ?? item.specularEnabled
  }
}

// 模块：prototypeAsset(objectPath, assetsDirectory) —— 为 OBJ 生成（或复用）原型资产。
//   缓存键 = sha1(版本|小写路径|大小|mtime) 的前 12 位十六进制；命中且元数据可读则直接返回。
//   输出：{ file, metaFile, meta }，meta 含包围盒 bounds 与每个组的 { prim, source, faces }；
//   OBJ 没有面时返回 null；写入先到 .tmp 再改名，失败时清理临时文件并抛出。
// One cached .usda per OBJ (keyed by path, size and mtime) so saving only rewrites the root layer.
function prototypeAsset(objectPath, assetsDirectory) {
  const info = statSync(objectPath)
  const key = createHash('sha1').update(`${ASSET_VERSION}|${resolve(objectPath).toLowerCase()}|${info.size}|${info.mtimeMs}`)
    .digest('hex').slice(0, 12)
  const stem = (basename(objectPath, extname(objectPath)).replace(/[^A-Za-z0-9_]/g, '_').replace(/_+/g, '_') || 'object').slice(0, 64)
  const file = join(assetsDirectory, `${stem}-${key}.usda`)
  const metaFile = join(assetsDirectory, `${stem}-${key}.json`)
  if (existsSync(file) && existsSync(metaFile)) {
    try { return { file, metaFile, meta: JSON.parse(readFileSync(metaFile, 'utf8')) } } catch {}
  }
  const obj = parseObj(readFileSync(objectPath, 'utf8'))
  if (!obj.groups.size) return null
  mkdirSync(assetsDirectory, { recursive: true })
  const groups = []
  const temporaryPath = `${file}.tmp`
  const writer = new LayerWriter(temporaryPath)
  try {
    writer.write(`#usda 1.0\n(\n    defaultPrim = "Prototype"\n    doc = ${usdString(`Stream Studio prototype from ${objectPath}`)}\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n\n`)
    writer.write('def Xform "Prototype"\n{\n')
    // 步骤：几何整体上移 -minY，使 OBJ 最低顶点落在 y = 0（与编辑器、面元求解器一致）。
    //   下面逐组写 Mesh：顶点按组重新编号（remap），每个 Mesh 只带自己用到的顶点。
    // The editor and facet solvers rest an OBJ on its lowest vertex.
    writer.write('    def Xform "geometry"\n    {\n')
    writer.write(`        double3 xformOp:translate = ${tuple([0, -obj.bounds.minY, 0])}\n`)
    writer.write('        uniform token[] xformOpOrder = ["xformOp:translate"]\n')
    const meshName = uniqueNames()
    const remap = new Int32Array(obj.points.length / 3).fill(-1)
    for (const [groupName, group] of obj.groups) {
      const used = []
      const indices = new Int32Array(group.indices.length)
      for (let position = 0; position < group.indices.length; position += 1) {
        const source = group.indices[position]
        if (remap[source] < 0) { remap[source] = used.length; used.push(source) }
        indices[position] = remap[source]
      }
      const points = new Float64Array(used.length * 3)
      const minimum = [Infinity, Infinity, Infinity], maximum = [-Infinity, -Infinity, -Infinity]
      for (const [target, source] of used.entries()) {
        for (let axis = 0; axis < 3; axis += 1) {
          const value = obj.points[source * 3 + axis]
          points[target * 3 + axis] = value
          minimum[axis] = Math.min(minimum[axis], value)
          maximum[axis] = Math.max(maximum[axis], value)
        }
      }
      const prim = meshName(groupName, 'mesh')
      writer.write(`\n        def Mesh ${usdString(prim)}\n        {\n`)
      writer.write('            uniform bool doubleSided = 1\n')
      writer.write(`            float3[] extent = [${tuple(minimum)}, ${tuple(maximum)}]\n`)
      writeArray(writer, '            int[]', 'faceVertexCounts', group.counts, 1)
      writeArray(writer, '            int[]', 'faceVertexIndices', indices, 1)
      writeArray(writer, '            point3f[]', 'points', points, 3)
      if (obj.colors) {
        const colors = new Float64Array(used.length * 3)
        for (const [target, source] of used.entries()) {
          for (let axis = 0; axis < 3; axis += 1) colors[target * 3 + axis] = obj.colors[source * 3 + axis]
        }
        writeArray(writer, '            color3f[]', 'primvars:displayColor', colors, 3, ' (\n                interpolation = "vertex"\n            )')
      }
      writer.write('            uniform token subdivisionScheme = "none"\n')
      writer.write('        }\n')
      for (const source of used) remap[source] = -1
      groups.push({ prim, source: groupName, faces: group.counts.length })
    }
    writer.write('    }\n}\n')
    writer.close()
    renameSync(temporaryPath, file)
  } catch (error) {
    try { writer.close() } catch {}
    try { if (existsSync(temporaryPath)) unlinkSync(temporaryPath) } catch {}
    throw error
  }
  const meta = { version: ASSET_VERSION, source: objectPath, bounds: obj.bounds, groups }
  writeFileSync(metaFile, JSON.stringify(meta))
  return { file, metaFile, meta }
}

// 模块：sunTransform(zenith, azimuth) —— 返回 DistantLight 的 4x4 变换（USD matrix4d，前三行为 X/Y/Z 轴，第四行为平移 0）。
//   toSun 为指向太阳的单位向量；光源局部 -Z 为照射方向，故局部 +Z 取 toSun；
//   太阳接近天顶（|y| > 0.999）时换用 X 轴作参考，避免叉乘退化。
// Unit vector towards the sun: zenith from +Y, azimuth clockwise from north (+X) towards east (+Z).
function sunTransform(zenithDegrees, azimuthDegrees) {
  const zenith = Number(zenithDegrees) * Math.PI / 180
  const azimuth = Number(azimuthDegrees) * Math.PI / 180
  const toSun = [Math.sin(zenith) * Math.cos(azimuth), Math.cos(zenith), Math.sin(zenith) * Math.sin(azimuth)]
  const reference = Math.abs(toSun[1]) > 0.999 ? [1, 0, 0] : [0, 1, 0]
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
  const normalize = (v) => { const length = Math.hypot(...v) || 1; return v.map((value) => value / length) }
  // A DistantLight shines along its local -Z, so local +Z points at the sun.
  const xAxis = normalize(cross(reference, toSun))
  const yAxis = cross(toSun, xAxis)
  return `( ${tuple([...xAxis, 0])}, ${tuple([...yAxis, 0])}, ${tuple([...toSun, 0])}, (0, 0, 0, 1) )`
}

// 模块：exportProjectUsd(project, options) —— 写出根层并返回统计。
//   options：outputPath、assetsDirectory、resolveAssetPath(相对名 → 绝对路径)、heightAt（可选地形函数）、
//   sourceProjectFile（写入 customLayerData）。返回 { path, prototypes, meshes, faces, instances,
//   skippedObjects, materials }；找不到 OBJ/位置文件的对象被跳过并记录原因。
// heightAt(north, east) returns terrain height above the DEM minimum in local scene coordinates.
export function exportProjectUsd(project, options) {
  // 步骤 1：读取场景尺寸（x = 南北、y = 东西，m）与偏移（offsetX/Y/Z），建立地形高度函数；
  //   尺寸下限 1e-3，避免除零；无地形时 heightAt 恒为 0。
  const configuration = project.configuration
  const scene = configuration.scene
  const offsetX = Number(scene.offsetX) || 0
  const offsetY = Number(scene.offsetY) || 0
  const offsetZ = Number(scene.offsetZ) || 0
  const sizeNorth = Math.max(1e-3, Number(scene.x) || 0)
  const sizeEast = Math.max(1e-3, Number(scene.y) || 0)
  const heightAt = scene.terrain && options.heightAt ? options.heightAt : () => 0
  const spectra = new Map((configuration.spectra || []).map((spectrum) => [spectrum.name, spectrum]))
  const stats = { prototypes: 0, meshes: 0, faces: 0, instances: 0, skippedObjects: [] }
  const usedAssets = new Set()
  const materialPaths = new Map()

  mkdirSync(dirname(options.outputPath), { recursive: true })
  const temporaryPath = `${options.outputPath}.tmp`
  const writer = new LayerWriter(temporaryPath)
  try {
    // 步骤 2：根层头——defaultPrim = World、Y 向上、customLayerData 保存导出格式、模式、
    //   完整工程 JSON 与源工程文件路径（usd-import 据此回读）。
    writer.write(`#usda 1.0
(
    defaultPrim = "World"
    doc = ${usdString(`Stream Studio project export: ${project.name || ''}`)}
    metersPerUnit = 1
    upAxis = "Y"
    customLayerData = {
        dictionary histream = {
            string format = ${usdString(USD_EXPORT_FORMAT)}
            string mode = ${usdString(project.mode)}
            string project = ${usdString(stringifyProject(project))}
            string sourceProjectFile = ${usdString(options.sourceProjectFile || '')}
        }
    }
)

`)

    // 步骤 3：材料登记。materialFor 按光谱名（无则按对象类型）去重，返回 /World/Looks/<名> 路径；
    //   Looks 的内容在最后统一写出。
    // Looks: one UsdPreviewSurface per HiStream spectral record actually used.
    const looks = []
    const lookName = uniqueNames()
    const materialFor = (spectralName, item) => {
      const key = spectralName || `type:${item?.type || 'default'}`
      if (!materialPaths.has(key)) {
        const spectrum = spectra.get(spectralName)
        const name = lookName(spectralName || item?.type, 'material')
        materialPaths.set(key, `/World/Looks/${name}`)
        looks.push({ name, spectralName, color: previewColor(spectrum, item), label: spectrum?.label })
      }
      return materialPaths.get(key)
    }

    // 步骤 4：写 Prototypes（USD class，抽象，不直接显示）。每个工程对象一个原型：
    //   有 OBJ 的用 references 指向缓存资产并在此处覆写材料绑定（绑定写在这里而不是资产里，
    //   因为被引用文件之外的目标会丢失）；无 OBJ 的（盒/椭球、介质、程序化水体）用 Cube/Sphere 按 dimensions 缩放。
    //   介质（Fire/Fog）加 displayOpacity 0.3。
    // Prototypes are abstract (class) so only their instances are imaged.
    writer.write('class "Prototypes"\n{\n')
    const prototypeName = uniqueNames()
    const prototypes = []
    for (const [index, item] of (configuration.objects?.items || []).entries()) {
      const name = prototypeName(item.name, `object_${index}`)
      const path = `/Prototypes/${name}`
      const medium = item.type === 'Fire' || item.type === 'Fog' || Boolean(item.medium)
      const header = attributeLines('        ', {
        objectName: item.name, objectType: item.type, objectIndex: index,
        sourceFile: item.fileName ? options.resolveAssetPath(item.fileName) : '', medium
      })
      const objectPath = item.fileName && !medium && !(item.type === 'Water' && item.sourceKind === 'prim')
        ? options.resolveAssetPath(item.fileName) : ''
      let footprint = null
      if (objectPath) {
        if (!existsSync(objectPath) || !statSync(objectPath).isFile()) {
          stats.skippedObjects.push(`${item.name}: OBJ not found (${objectPath})`)
          continue
        }
        const asset = prototypeAsset(objectPath, options.assetsDirectory)
        if (!asset) {
          stats.skippedObjects.push(`${item.name}: OBJ has no faces`)
          continue
        }
        usedAssets.add(basename(asset.file))
        usedAssets.add(basename(asset.metaFile))
        footprint = item.groundFootprint || asset.meta.bounds
        const reference = relative(dirname(options.outputPath), asset.file).replaceAll('\\', '/')
        writer.write(`    def Xform ${usdString(name)} (\n        prepend references = @./${reference}@\n    )\n    {\n${header}`)
        // Bindings live here, not in the asset: targets outside a referenced file would be dropped.
        writer.write('\n        over "geometry"\n        {\n')
        for (const group of asset.meta.groups) {
          const material = meshMaterial(item, group.source)
          writer.write(`            over ${usdString(group.prim)} (\n                prepend apiSchemas = ["MaterialBindingAPI"]\n            )\n            {\n`)
          writer.write(`                rel material:binding = <${materialFor(material.spectralName, item)}>\n`)
          writer.write(attributeLines('                ', { sourceName: group.source, ...material }))
          writer.write('            }\n')
          stats.meshes += 1
          stats.faces += group.faces
        }
        writer.write('        }\n    }\n')
      } else {
        // Box/ellipsoid primitives, participating media and procedural water use their dimensions.
        const dimensions = Array.isArray(item.dimensions) ? item.dimensions.map(Number) : [1, 1, 1]
        const [sizeX, sizeY, sizeZ] = [0, 1, 2].map((axis) => Math.abs(dimensions[axis]) || 1)
        footprint = item.groundFootprint || { minX: -sizeX / 2, maxX: sizeX / 2, minZ: -sizeZ / 2, maxZ: sizeZ / 2 }
        const ellipsoid = String(item.shape || '').toLowerCase().includes('ellipsoid')
        writer.write(`    def Xform ${usdString(name)}\n    {\n${header}`)
        writer.write(`\n        def ${ellipsoid ? 'Sphere' : 'Cube'} "shape" (\n            prepend apiSchemas = ["MaterialBindingAPI"]\n        )\n        {\n`)
        writer.write(ellipsoid ? '            double radius = 0.5\n' : '            double size = 1\n')
        writer.write(`            double3 xformOp:translate = ${tuple([0, sizeY / 2, 0])}\n`)
        writer.write(`            float3 xformOp:scale = ${tuple([sizeX, sizeY, sizeZ])}\n`)
        writer.write('            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n')
        if (medium) writer.write('            float[] primvars:displayOpacity = [0.3]\n')
        const material = meshMaterial(item, item.meshes?.[0]?.name)
        writer.write(`            rel material:binding = <${materialFor(material.spectralName, item)}>\n`)
        writer.write(attributeLines('            ', material))
        writer.write('        }\n    }\n')
      }
      stats.prototypes += 1
      prototypes.push({ item, index, name, path, footprint })
    }
    writer.write('}\n\n')

    writer.write('def Xform "World"\n{\n')

    // 步骤 5：地面。无地形为 1x1 的平面；有地形时按约 1 m 一格（最多 256x256）采样 heightAt，
    //   四边形顶点顺序为从 +Y 看逆时针，使地面朝上。
    // Ground: flat quad, or a terrain grid sampled from the project DEM.
    const background = scene.background || {}
    const columns = scene.terrain ? Math.min(256, Math.max(1, Math.round(sizeEast))) : 1
    const rows = scene.terrain ? Math.min(256, Math.max(1, Math.round(sizeNorth))) : 1
    const groundPoints = []
    for (let row = 0; row <= rows; row += 1) {
      for (let column = 0; column <= columns; column += 1) {
        const north = sizeNorth * row / rows
        const east = sizeEast * column / columns
        groundPoints.push(offsetX + north, offsetY + heightAt(north, east), offsetZ + east)
      }
    }
    const groundCounts = []
    const groundIndices = []
    for (let row = 0; row < rows; row += 1) {
      for (let column = 0; column < columns; column += 1) {
        const a = row * (columns + 1) + column
        // Counter-clockwise seen from +Y so the ground faces up.
        groundCounts.push(4)
        groundIndices.push(a, a + 1, a + columns + 2, a + columns + 1)
      }
    }
    writer.write('    def Mesh "Ground" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n    {\n')
    writeArray(writer, '        int[]', 'faceVertexCounts', groundCounts, 1)
    writeArray(writer, '        int[]', 'faceVertexIndices', groundIndices, 1)
    writeArray(writer, '        point3f[]', 'points', groundPoints, 3)
    writer.write('        uniform token subdivisionScheme = "none"\n')
    writer.write(`        rel material:binding = <${materialFor(background.spectralName, { type: background.materialType || 'Soil' })}>\n`)
    writer.write(attributeLines('        ', {
      role: 'ground', terrain: Boolean(scene.terrain), demFile: scene.demFile ? options.resolveAssetPath(scene.demFile) : '',
      spectralName: background.spectralName, thermalName: background.thermalName,
      materialName: background.materialName, materialType: background.materialType
    }))
    writer.write('    }\n\n')

    // 步骤 6：太阳 DistantLight（缺省天顶 30°、方位 135°）；天顶/方位原值也写成自定义属性。
    //   intensity = 3、angle = 0.53° 仅为预览用的经验设置。
    const light = configuration.light || {}
    writer.write('    def DistantLight "Sun"\n    {\n')
    writer.write('        float inputs:angle = 0.53\n        float inputs:intensity = 3\n')
    writer.write(`        matrix4d xformOp:transform = ${sunTransform(light.zenith ?? 30, light.azimuth ?? 135)}\n`)
    writer.write('        uniform token[] xformOpOrder = ["xformOp:transform"]\n')
    writer.write(attributeLines('        ', { zenith: Number(light.zenith), azimuth: Number(light.azimuth) }))
    writer.write('    }\n\n')

    writer.write('    def Scope "Objects"\n    {\n')
    // 步骤 7：Objects —— 每个放置点一个 instanceable 的 Xform 引用原型。无位置文件时放在场景中心。
    //   贴地对象（目前为车辆）的高度 = offsetY + groundedPlacementHeight（取足迹下最高地形点）；
    //   其余用位置文件的高度；再统一加上放置点处的地形高度 ground。
    //   旋转以 -rotation（度）写入 rotateY，即取与 USD 绕 Y 轴正向相反的符号（符号约定待核对）；
    //   原始 (north, east, height)、旋转、缩放另存为 histream:placement* 属性，未改动的实例可精确回读。
    for (const { item, index, name, path, footprint } of prototypes) {
      let placements = [{ north: offsetX + sizeNorth / 2, east: offsetZ + sizeEast / 2, height: offsetY, scale: 1, rotation: 0 }]
      if (item.positionFile) {
        const positionPath = options.resolveAssetPath(item.positionFile)
        if (!existsSync(positionPath)) {
          stats.skippedObjects.push(`${item.name}: position file not found (${positionPath})`)
          continue
        }
        placements = readPlacements(readFileSync(positionPath, 'utf8'))
      }
      writer.write(`        def Xform ${usdString(name)}\n        {\n`)
      writer.write(attributeLines('            ', {
        objectName: item.name, objectIndex: index,
        positionFile: item.positionFile ? options.resolveAssetPath(item.positionFile) : ''
      }))
      const width = String(Math.max(1, placements.length - 1)).length
      for (const [placementIndex, placement] of placements.entries()) {
        const north = placement.north - offsetX
        const east = placement.east - offsetZ
        const ground = heightAt(north, east)
        const height = isGroundedObject(item)
          ? offsetY + groundedPlacementHeight(footprint,
            { north, east, scale: placement.scale, rotation: placement.rotation }, heightAt)
          : placement.height
        writer.write(`\n            def Xform "i${String(placementIndex).padStart(width, '0')}" (\n                instanceable = true\n                prepend references = <${path}>\n            )\n            {\n`)
        writer.write(`                double3 xformOp:translate = ${tuple([placement.north, height + ground, placement.east])}\n`)
        writer.write(`                float xformOp:rotateY = ${number(-placement.rotation)}\n`)
        writer.write(`                float3 xformOp:scale = ${tuple([placement.scale, placement.scale, placement.scale])}\n`)
        writer.write('                uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY", "xformOp:scale"]\n')
        // The source record, so an unchanged instance round-trips exactly.
        writer.write(`                custom double3 histream:placement = ${tuple([placement.north, placement.east, placement.height])}\n`)
        writer.write(`                custom double histream:placementRotation = ${number(placement.rotation)}\n`)
        writer.write(`                custom double histream:placementScale = ${number(placement.scale)}\n`)
        writer.write('            }\n')
        stats.instances += 1
      }
      writer.write('        }\n')
    }
    writer.write('    }\n\n')

    writer.write('    def Scope "Looks"\n    {\n')
    // 步骤 8：Looks —— 每个用到的光谱记录对应一个 Material + UsdPreviewSurface（roughness 0.8），
    //   漫反射色来自 previewColor；spectralName/label 作为自定义属性保留。
    for (const look of looks) {
      writer.write(`        def Material ${usdString(look.name)}\n        {\n`)
      writer.write(`            token outputs:surface.connect = </World/Looks/${look.name}/PreviewSurface.outputs:surface>\n`)
      writer.write(attributeLines('            ', { spectralName: look.spectralName, label: look.label }))
      writer.write('\n            def Shader "PreviewSurface"\n            {\n')
      writer.write('                uniform token info:id = "UsdPreviewSurface"\n')
      writer.write(`                color3f inputs:diffuseColor = ${tuple(look.color)}\n`)
      writer.write('                float inputs:roughness = 0.8\n')
      writer.write('                token outputs:surface\n            }\n        }\n')
    }
    writer.write('    }\n}\n')
    writer.close()
    renameSync(temporaryPath, options.outputPath)
  } catch (error) {
    try { writer.close() } catch {}
    try { if (existsSync(temporaryPath)) unlinkSync(temporaryPath) } catch {}
    throw error
  }
  // 步骤 9：清理缓存目录中符合 ASSET_FILE 命名但本次导出没有用到的旧资产。
  if (existsSync(options.assetsDirectory)) {
    for (const name of readdirSync(options.assetsDirectory)) {
      if (ASSET_FILE.test(name) && !usedAssets.has(name)) {
        try { unlinkSync(join(options.assetsDirectory, name)) } catch {}
      }
    }
  }
  return { path: options.outputPath, ...stats, materials: materialPaths.size }
}
