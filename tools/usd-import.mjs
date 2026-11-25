// ==========================================================================
// 文件：usd-import.mjs
// 功能：把 histream-usd 清单（manifest：OBJ 几何、网格组材料、实例放置）转换为 Stream Studio 工程的
//   对象列表（configuration.objects.items），是 usd-export.mjs 的逆过程。
// 主要模块：
//   1. 小工具：simplify（名称归一化：小写并去掉非字母数字，用于宽松匹配）、round（6 位小数）、
//      isPrimitiveItem（盒/椭球、介质、程序化水体——没有 OBJ 文件的对象）、parseEmbeddedProject
//   2. buildProjectFromUsd(baseProject, manifest, options)：
//      a. 若清单内嵌了导出时的工程 JSON，则把基础工程缺少的 spectra/thermals/materials/canopies 补进来
//      b. 为每个网格组挑选光谱/热/材料/冠层（meshFor：USD 自定义属性 > 同名模板 > 同光谱兄弟网格 > 缺省）
//      c. 按几何分组放置记录，转成位置文件（北、东、离地高度、缩放、旋转）并生成手动分布的对象
//      d. 用规范化后的工程返回，并统计对象数、实例数与警告
// 数据流：manifest（geometries、placements、skipped、histream.project）+ baseProject + options
//   （writePositionFile、heightAt）→ 新工程与 stats。
// 约定：placement 的 north/east 为场景坐标，y 为 USD 高度；写位置文件时 高度 = y - 该点地形高度；
//   单个对象最多保留 100000 个实例（MAX_INSTANCES），超出的丢弃并给警告。
// 关系：manifest 由 USD 读取端生成（本文件之外，待核对）；字段名与 usd-export.mjs 写出的
//   histream:* 自定义属性对应（objectName、objectType、spectralName、materialName 等）。
// ==========================================================================

// Turns a histream-usd manifest (OBJ files + placements) into the object list of a Stream Studio project.
import { normalizeProject } from '../src/renderer/src/project-schema.js'

const simplify = (value) => String(value ?? '').toLowerCase().replace(/[^a-z0-9]+/g, '')
const round = (value) => {
  const rounded = Math.round(Number(value) * 1e6) / 1e6
  return Number.isFinite(rounded) && !Object.is(rounded, -0) ? rounded : 0
}
// MAX_INSTANCES：单个对象允许导入的最大实例数；超出部分被丢弃并写入警告。
const MAX_INSTANCES = 100000

// 作用：判断对象是否没有 OBJ 几何（无 fileName、火焰/雾、介质、基本体水体），
//   与 usd-export.mjs 中写成 Cube/Sphere 的判据一致。
function isPrimitiveItem(item) {
  return !item.fileName || item.type === 'Fire' || item.type === 'Fog' || Boolean(item.medium)
    || (item.type === 'Water' && item.sourceKind === 'prim')
}

// 作用：读取 manifest.histream.project 中导出时嵌入的工程 JSON 并规范化；缺失或解析失败返回 null。
function parseEmbeddedProject(manifest) {
  if (!manifest.histream?.project) return null
  try { return normalizeProject(JSON.parse(manifest.histream.project)) } catch { return null }
}

// 模块：buildProjectFromUsd(baseProject, manifest, options) —— 返回 { project, stats }。
//   不修改传入的 baseProject（先 structuredClone 再规范化）；
//   stats = { objects, instances, restoredFromMetadata（是否用了内嵌工程）, warnings }。
// writePositionFile(name, text) returns the written path; heightAt(north, east) is terrain height in local scene coordinates.
export function buildProjectFromUsd(baseProject, manifest, options) {
  const project = normalizeProject(structuredClone(baseProject))
  const configuration = project.configuration
  const scene = configuration.scene
  const offsetX = Number(scene.offsetX) || 0
  const offsetZ = Number(scene.offsetZ) || 0
  const heightAt = scene.terrain && options.heightAt ? options.heightAt : () => 0
  const embedded = parseEmbeddedProject(manifest)
  const stats = { objects: 0, instances: 0, restoredFromMetadata: Boolean(embedded), warnings: [...(manifest.skipped || [])] }

  // 步骤 1：从内嵌工程补全基础工程中缺失的 spectra / thermals / materials / canopies 记录（按 name 去重）；
  //   然后建立对象模板列表（内嵌的在前，当前工程的在后）与按名称（含宽松匹配）查找的函数。
  // Records referenced by the USD scene but missing from this project come from the embedded project.
  if (embedded) {
    for (const key of ['spectra', 'thermals', 'materials', 'canopies']) {
      const names = new Set((configuration[key] || []).map((record) => record.name))
      for (const record of embedded.configuration[key] || []) {
        if (!names.has(record.name)) configuration[key].push(record)
      }
    }
  }
  const templates = [...(embedded?.configuration.objects?.items || []), ...(configuration.objects?.items || [])]
  const findTemplate = (name) => templates.find((item) => item.name === name)
    || templates.find((item) => simplify(item.name) && simplify(item.name) === simplify(name))
  const spectra = configuration.spectra || []
  const spectrumNamed = (name) => spectra.find((record) => record.name === name)
    || spectra.find((record) => simplify(record.name) && simplify(record.name) === simplify(name))
  const meshTemplates = templates.flatMap((item) => item.meshes || [])
  const recordName = (key, preferred) => {
    const records = configuration[key] || []
    return (records.find((record) => record.name === preferred) || records[0])?.name
  }
  const isVegetationSpectrum = (name) => String(spectrumNamed(name)?.model || '').toLowerCase() === 'prospect'
  const defaultSpectrum = (spectrumNamed('concrete') || spectra[0])?.name

  // 步骤 2：meshFor —— 为清单中的一个网格组生成工程里的网格设置。
  //   光谱名优先级：USD 自定义属性 > 模板同名网格 > 与材料名同名的光谱 > 模板第一个网格 > 缺省（concrete）；
  //   热/材料/冠层名：USD 属性（须存在于工程）> 模板同名网格 > 同光谱的其他网格 > 按植被或建筑取缺省名。
  const meshFor = (mesh, template) => {
    const attributes = mesh.attributes || {}
    const own = template?.meshes?.find((entry) => entry.name === mesh.group)
    const spectralName = (attributes.spectralName && spectrumNamed(attributes.spectralName)?.name)
      || own?.spectralName || spectrumNamed(mesh.material)?.name
      || template?.meshes?.[0]?.spectralName || defaultSpectrum
    const sibling = meshTemplates.find((entry) => entry.spectralName === spectralName)
    const vegetation = isVegetationSpectrum(spectralName)
    const pick = (key, recordKey, fallback) =>
      (attributes[key] && (configuration[recordKey] || []).some((record) => record.name === attributes[key]) ? attributes[key] : null)
      ?? own?.[key] ?? sibling?.[key] ?? recordName(recordKey, fallback)
    const result = {
      name: mesh.group,
      spectralName,
      thermalName: pick('thermalName', 'thermals', vegetation ? 'vegetation_temperature' : 'building_temperature'),
      materialName: pick('materialName', 'materials', vegetation ? 'leaf_c3' : 'building_surface'),
      canopyName: pick('canopyName', 'canopies', vegetation ? 'canopy_default' : 'rigid_body'),
      specularEnabled: Boolean(attributes.specularEnabled ?? own?.specularEnabled ?? false)
    }
    const energyModel = attributes.energyModel ?? own?.energyModel ?? sibling?.energyModel
    if (energyModel) result.energyModel = energyModel
    return result
  }

  // 步骤 3：按 geometry id 归并放置记录；uniqueName 保证导入后的对象名不重名（追加 _2、_3……）。
  const placementsByGeometry = new Map()
  for (const placement of manifest.placements || []) {
    if (!placementsByGeometry.has(placement.geometry)) placementsByGeometry.set(placement.geometry, [])
    placementsByGeometry.get(placement.geometry).push(placement)
  }

  const usedNames = new Set()
  const uniqueName = (name) => {
    let candidate = String(name || 'usd_object')
    for (let suffix = 2; usedNames.has(candidate); suffix += 1) candidate = `${name}_${suffix}`
    usedNames.add(candidate)
    return candidate
  }

  const items = []
  for (const geometry of manifest.geometries || []) {
    const placements = placementsByGeometry.get(geometry.id)
    if (!placements?.length) continue
    const attributes = geometry.attributes || {}
    const template = findTemplate(attributes.objectName || geometry.name)
    const primitive = template && isPrimitiveItem(template)
    const name = uniqueName(template?.name || geometry.name)
    const meshes = (geometry.meshes || []).map((mesh) => meshFor(mesh, template))
    const type = template?.type || attributes.objectType
      || (meshes.some((mesh) => isVegetationSpectrum(mesh.spectralName)) ? 'Vegetation' : 'Building')
    if (placements.length > MAX_INSTANCES) {
      stats.warnings.push(`${name}: ${placements.length} instances exceed the ${MAX_INSTANCES} limit; extra instances were dropped`)
      placements.length = MAX_INSTANCES
    }
    // 步骤 4：每个放置点写成位置文件一行：北、东、离地高度、缩放、旋转。
    //   离地高度 = USD 的 y 减去该点处地形高度（与导出时 height + ground 互逆）。
    //   位置文件的高度本来就含 scene.offsetY（runtime-scene.mjs 写运行时位置文件时才减去），导出时贴地对象写
    //   offsetY + 贴地高度、其他对象写文件高度，因此这里不必再区分类型或加减 offsetY（往返测试见
    //   tools/test-usd-roundtrip-20261125.mjs，offsetY ≠ 0 时不漂移）。
    const records = placements.map((placement) => [
      round(placement.north),
      round(placement.east),
      round(placement.y - heightAt(placement.north - offsetX, placement.east - offsetZ)),
      round(placement.scale),
      round(placement.rotation)
    ].join(' '))
    const text = records.join('\n')
    // 步骤 5：以模板为底生成对象：手动分布（mode = manual、数量 = 记录数），点坐标直接放在 distribution.points，
    //   并写出位置文件；有 OBJ 的对象再覆盖 fileName、网格与材料/冠层缺省，且清除旧的 groundFootprint。
    const item = {
      ...(template ? structuredClone(template) : {}),
      name,
      type,
      positionFile: options.writePositionFile(name, `${text}\n`),
      instanceCount: records.length,
      distribution: {
        ...(template?.distribution || {}),
        mode: 'manual', basis: 'count', count: records.length,
        points: text, offsetX: 0, offsetY: 0, offsetZ: 0
      }
    }
    if (!primitive) {
      item.fileName = geometry.objFile
      item.meshes = meshes
      item.materialName = meshes[0]?.materialName
      item.canopyName = meshes[0]?.canopyName
      delete item.groundFootprint
    }
    items.push(item)
    stats.objects += 1
    stats.instances += records.length
  }

  configuration.objects = { ...configuration.objects, items, count: items.length, names: items.map((item) => item.name) }
  return { project: normalizeProject(project), stats }
}
