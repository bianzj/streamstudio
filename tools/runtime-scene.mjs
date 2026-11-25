// ==========================================================================
// 文件：runtime-scene.mjs
// 功能：运行前把工程整理成“运行时场景”：只保留与计算域相交的对象实例，必要时把单个越界 OBJ
//       裁剪到计算域内，路径转绝对，偏移归零，并把中间文件写入临时目录 .histream-runtime-<token>
//       （server.mjs 的 runtime-scene 任务调用 prepareRuntimeSceneProject）
// 主要模块/步骤：
//   1. 位置文件解析（positionRecords）与包围盒工具（primitiveBounds / objBounds / transformedBounds）
//   2. 域相交/包含判定（intersectsDomain / containedByDomain）
//   3. OBJ 几何裁剪：parseObjGeometry → 逐三角形做 Sutherland–Hodgman 平面裁剪
//      （clipPolygonToPlane）→ 局部坐标写回（transformObjPosition / inverseObjPosition）
//   4. prepareRuntimeSceneProject：路径绝对化 → 逐对象筛选实例 / 贴地 / 裁剪 → 表面纹理展开
//      （surface-textures.mjs）→ 多孔光谱展开 → 偏移归零 → 写运行时 project.json
// 数据流：原工程 JSON + 位置文件 + OBJ → 运行时目录中的 project.json / position-N.txt /
//         object-N.obj / spectrum-N.txt；返回统计量与 cleanup() 供调用方清理。
// 约定：OBJ 为 Y 向上；计算域 X∈[offsetX, offsetX+scene.x]，Y(高)∈[offsetY, offsetY+scene.height]，
//       Z∈[offsetZ, offsetZ+scene.y]（注意工程的 scene.y 对应运行时 Z）；单位 m；
//       位置文件每行前三列依次赋给运行时的 x、z、y（y 为高度，即“北、东、高”），其后为缩放、旋转角（度）；
//       旋转取 -rotation 绕 Y 轴；裁剪后实例的位置相对计算域原点（offset 已减去）。
// ==========================================================================

import { existsSync, mkdirSync, readFileSync, rmdirSync, statSync, unlinkSync, writeFileSync } from 'node:fs'
import { basename, dirname, isAbsolute, resolve } from 'node:path'
import { effectivePorousSpectrum, groundedPlacementHeight, isGroundedObject, normalizeProject, stringifyProject } from '../src/renderer/src/project-schema.js'
import { applySurfaceTextures } from './surface-textures.mjs'

// 作用：解析位置文件文本，返回 { records, invalid }。支持 # 和 // 注释、逗号/空白分隔、UTF-8 BOM。
// 每行至少 3 个数：x、z、y，可选缩放（缺省 1）和旋转角（缺省 0，度）；格式非法的行只计入 invalid。
function positionRecords(content) {
  const records = []
  let invalid = 0
  for (const sourceLine of String(content || '').replace(/^\uFEFF/, '').split(/\r?\n/)) {
    const line = sourceLine.replace(/#.*/, '').trim()
    if (!line || line.startsWith('//')) continue
    const values = line.split(/[\s,]+/).map(Number)
    if (values.length < 3 || !values.slice(0, 3).every(Number.isFinite)) {
      invalid += 1
      continue
    }
    records.push({
      x: values[0],
      z: values[1],
      y: values[2],
      scale: Number.isFinite(values[3]) ? values[3] : 1,
      rotation: Number.isFinite(values[4]) ? values[4] : 0
    })
  }
  return { records, invalid }
}

// 数值转字符串，并把 -0 规范成 "0"，避免写出 "-0"。
function runtimeNumber(value) {
  return Object.is(value, -0) ? '0' : String(value)
}

// 缺省资源路径解析：绝对路径原样规范化，相对路径相对工程目录；server 可通过 options.resolveAssetPath 覆盖。
function defaultAssetPath(value, baseDir) {
  return isAbsolute(value) ? resolve(value) : resolve(baseDir, value)
}

// 作用：无 OBJ 的基元对象（长方体等）的局部包围盒：X/Z 以中心对称，Y 从 0 到高度；尺寸取 item.dimensions。
function primitiveBounds(item) {
  const dimensions = Array.isArray(item.dimensions) ? item.dimensions.map(Number) : []
  const sizeX = Math.abs(Number.isFinite(dimensions[0]) ? dimensions[0] : 0)
  const sizeY = Math.abs(Number.isFinite(dimensions[1]) ? dimensions[1] : 0)
  const sizeZ = Math.abs(Number.isFinite(dimensions[2]) ? dimensions[2] : 0)
  return { minX: -sizeX / 2, maxX: sizeX / 2, minY: 0, maxY: sizeY, minZ: -sizeZ / 2, maxZ: sizeZ / 2 }
}

// 作用：由 OBJ 顶点求局部包围盒；没有有效顶点时返回 fallback。
// 说明：面元求解器按最小 Y 贴地，体元求解器保留 OBJ 原始高程，所以 Y 范围取两者的并集，
//       以免跨域对象在选定求解器裁剪之前被提前丢弃。
function objBounds(content, fallback) {
  const bounds = {
    minX: Infinity, maxX: -Infinity,
    minY: Infinity, maxY: -Infinity,
    minZ: Infinity, maxZ: -Infinity
  }
  for (const line of String(content || '').split(/\r?\n/)) {
    const match = line.match(/^\s*v\s+([^\s]+)\s+([^\s]+)\s+([^\s]+)/)
    if (!match) continue
    const [x, y, z] = match.slice(1).map(Number)
    if (![x, y, z].every(Number.isFinite)) continue
    bounds.minX = Math.min(bounds.minX, x)
    bounds.maxX = Math.max(bounds.maxX, x)
    bounds.minY = Math.min(bounds.minY, y)
    bounds.maxY = Math.max(bounds.maxY, y)
    bounds.minZ = Math.min(bounds.minZ, z)
    bounds.maxZ = Math.max(bounds.maxZ, z)
  }
  if (!Number.isFinite(bounds.minX)) return fallback
  // Three.js and facet solvers ground OBJ geometry at its minimum Y, while
  // voxel solvers retain native OBJ elevation. Use their union so a crossing
  // object is never discarded before the selected solver clips it.
  const rawMinY = bounds.minY
  const rawMaxY = bounds.maxY
  bounds.minY = Math.min(rawMinY, 0)
  bounds.maxY = Math.max(rawMaxY, rawMaxY - rawMinY)
  return bounds
}

// 作用：把局部包围盒的 8 个角点按“缩放 → 绕 Y 轴旋转（-rotation） → 平移”变换到世界系，
// 返回其轴对齐包围盒（保守估计，旋转后可能偏大）。
function transformedBounds(local, position) {
  const scale = Number.isFinite(position.scale) ? position.scale : 1
  const radians = -(Number.isFinite(position.rotation) ? position.rotation : 0) * Math.PI / 180
  const cosine = Math.cos(radians)
  const sine = Math.sin(radians)
  const result = {
    minX: Infinity, maxX: -Infinity,
    minY: Infinity, maxY: -Infinity,
    minZ: Infinity, maxZ: -Infinity
  }
  for (const x of [local.minX, local.maxX]) {
    for (const y of [local.minY, local.maxY]) {
      for (const z of [local.minZ, local.maxZ]) {
        const scaledX = x * scale
        const scaledY = y * scale
        const scaledZ = z * scale
        const worldX = position.x + cosine * scaledX + sine * scaledZ
        const worldY = position.y + scaledY
        const worldZ = position.z - sine * scaledX + cosine * scaledZ
        result.minX = Math.min(result.minX, worldX)
        result.maxX = Math.max(result.maxX, worldX)
        result.minY = Math.min(result.minY, worldY)
        result.maxY = Math.max(result.maxY, worldY)
        result.minZ = Math.min(result.minZ, worldZ)
        result.maxZ = Math.max(result.maxZ, worldZ)
      }
    }
  }
  return result
}

// 包围盒是否与计算域相交（含边界）；用于筛掉完全在域外的实例。
function intersectsDomain(bounds, domain) {
  return bounds.maxX >= domain.minX && bounds.minX <= domain.maxX
    && bounds.maxY >= domain.minY && bounds.minY <= domain.maxY
    && bounds.maxZ >= domain.minZ && bounds.minZ <= domain.maxZ
}

// 包围盒是否完全位于计算域内；checkHeight=false 时忽略高度（有地形时高程由地形决定）。
function containedByDomain(bounds, domain, checkHeight = true) {
  return bounds.minX >= domain.minX && bounds.maxX <= domain.maxX
    && (!checkHeight || (bounds.minY >= domain.minY && bounds.maxY <= domain.maxY))
    && bounds.minZ >= domain.minZ && bounds.maxZ <= domain.maxZ
}

// 作用：解析 OBJ 的顶点位置与三角形（多边形按扇形剖分），支持负的相对索引，忽略越界索引。
// 三角形记录所属分组：取最近的 o/g 名；若尚无分组则暂用 usemtl 名。
function parseObjGeometry(content) {
  const positions = []
  const triangles = []
  let activeGroup = ''
  for (const sourceLine of String(content || '').split(/\r?\n/)) {
    const cells = sourceLine.trim().split(/\s+/)
    const record = cells[0]
    if ((record === 'o' || record === 'g') && cells[1]) {
      activeGroup = cells[1]
    } else if (record === 'usemtl' && !activeGroup && cells[1]) {
      activeGroup = cells[1]
    } else if (record === 'v' && cells.length >= 4) {
      const position = cells.slice(1, 4).map(Number)
      if (position.every(Number.isFinite)) positions.push(position)
    } else if (record === 'f' && cells.length >= 4) {
      const face = cells.slice(1).map((cell) => {
        const raw = Number(cell.split('/')[0])
        return raw > 0 ? raw - 1 : positions.length + raw
      }).filter((index) => Number.isInteger(index) && index >= 0 && index < positions.length)
      for (let index = 1; index + 1 < face.length; index += 1) {
        triangles.push({ indices: [face[0], face[index], face[index + 1]], group: activeGroup })
      }
    }
  }
  return { positions, triangles }
}

// 作用：Sutherland–Hodgman 单平面裁剪。保留 point[axis] 大于等于（或小于等于）plane 的一侧，
// 1e-8 的容差让恰在平面上的点算作内部；穿越平面处线性插值生成新顶点（比例限幅 0–1）。
function clipPolygonToPlane(source, axis, plane, keepGreater) {
  const result = []
  if (!source.length) return result
  const inside = (point) => keepGreater ? point[axis] >= plane - 1e-8 : point[axis] <= plane + 1e-8
  let previous = source[source.length - 1]
  let previousInside = inside(previous)
  for (const current of source) {
    const currentInside = inside(current)
    if (currentInside !== previousInside) {
      const denominator = current[axis] - previous[axis]
      if (Math.abs(denominator) > 1e-12) {
        const amount = Math.max(0, Math.min(1, (plane - previous[axis]) / denominator))
        result.push(previous.map((value, index) => value + amount * (current[index] - value)))
      }
    }
    if (currentInside) result.push(current)
    previous = current
    previousInside = currentInside
  }
  return result
}

// 局部 OBJ 坐标 → 世界坐标：先缩放，再绕 Y 轴旋转 -rotation（度），最后平移到 (x, y, z)。
function transformObjPosition(local, placement) {
  const radians = -placement.rotation * Math.PI / 180
  const cosine = Math.cos(radians)
  const sine = Math.sin(radians)
  const x = local[0] * placement.scale
  const y = local[1] * placement.scale
  const z = local[2] * placement.scale
  return [
    placement.x + cosine * x + sine * z,
    placement.y + y,
    placement.z - sine * x + cosine * z
  ]
}

// transformObjPosition 的逆变换，用于把裁剪后的世界顶点写回对象局部坐标（位置文件仍负责摆放）。
function inverseObjPosition(world, placement) {
  const radians = -placement.rotation * Math.PI / 180
  const cosine = Math.cos(radians)
  const sine = Math.sin(radians)
  const dx = world[0] - placement.x
  const dz = world[2] - placement.z
  return [
    (cosine * dx - sine * dz) / placement.scale,
    (world[1] - placement.y) / placement.scale,
    (sine * dx + cosine * dz) / placement.scale
  ]
}

// 作用：把单个实例的 OBJ 裁剪到计算域内，返回 { content, sourceTriangles, outputTriangles }；
// 无可用几何或缩放非正时返回 null。clipHeight 为 true 时同时裁剪高度（无地形时）。
// 步骤：1) 变换每个三角形到世界系；2) 依次对 X、(Y)、Z 的上下界裁剪；3) 逆变换回局部坐标，
//       每个子三角形独立输出 3 个顶点（不共享顶点），按分组写 g 行。
function clippedObjContent(content, placement, domain, fallbackGroup, clipHeight) {
  if (!(placement.scale > 0)) return null
  const geometry = parseObjGeometry(content)
  if (!geometry.positions.length || !geometry.triangles.length) return null
  let originalMinY = Infinity
  for (const position of geometry.positions) originalMinY = Math.min(originalMinY, position[1])
  const output = [
    '# HiStream runtime OBJ clipped to the calculation domain',
    // Facet solvers ground OBJ files at their minimum Y. Preserve the source
    // minimum with an unreferenced vertex so clipping cannot shift elevation.
    `v 0 ${runtimeNumber(originalMinY)} 0`
  ]
  // 顶点编号从 2 起：第 1 个顶点是上面写入的 minY 占位顶点（OBJ 索引从 1 起）。
  let vertexIndex = 2
  let outputTriangles = 0
  let previousGroup = null
  for (const triangle of geometry.triangles) {
    let polygon = triangle.indices.map((index) => transformObjPosition(geometry.positions[index], placement))
    for (const [axis, minimum, maximum] of [
      [0, domain.minX, domain.maxX],
      ...(clipHeight ? [[1, domain.minY, domain.maxY]] : []),
      [2, domain.minZ, domain.maxZ]
    ]) {
      polygon = clipPolygonToPlane(polygon, axis, minimum, true)
      polygon = clipPolygonToPlane(polygon, axis, maximum, false)
      if (polygon.length < 3) break
    }
    if (polygon.length < 3) continue
    const group = triangle.group || fallbackGroup || 'object'
    if (group !== previousGroup) {
      output.push(`g ${String(group).replace(/\s+/g, '_')}`)
      previousGroup = group
    }
    for (let index = 1; index + 1 < polygon.length; index += 1) {
      const vertices = [polygon[0], polygon[index], polygon[index + 1]]
        .map((position) => inverseObjPosition(position, placement))
      for (const vertex of vertices) output.push(`v ${vertex.map(runtimeNumber).join(' ')}`)
      output.push(`f ${vertexIndex} ${vertexIndex + 1} ${vertexIndex + 2}`)
      vertexIndex += 3
      outputTriangles += 1
    }
  }
  return { content: output.join('\n') + '\n', sourceTriangles: geometry.triangles.length, outputTriangles }
}

// 作用：生成运行时场景。输入：工程文件路径、工程 JSON、options（resolveAssetPath、terrainHeight）。
// 输出：{ inputPath（运行时 project.json）, project, textureSummary, total/kept/excluded/invalid,
//       clippedObjects, sourceTriangles, retainedTriangles, grounded, cleanup }。
// 副作用：在工程目录下创建 .histream-runtime-<token> 临时目录；调用方运行后需执行 cleanup()，
//       本函数出错时会自行清理。
export function prepareRuntimeSceneProject(inputPath, sourceProject, options = {}) {
  const baseDir = dirname(inputPath)
  const resolveAssetPath = options.resolveAssetPath || ((value) => defaultAssetPath(value, baseDir))
  const project = normalizeProject(sourceProject)
  const scene = project.configuration.scene
  const offsetX = Number(scene.offsetX) || 0
  const offsetY = Number(scene.offsetY) || 0
  const offsetZ = Number(scene.offsetZ) || 0
  const maxX = offsetX + Number(scene.x)
  const maxY = offsetY + Number(scene.height)
  const maxZ = offsetZ + Number(scene.y)
  // 计算域（含偏移）。注意高度上界为 offsetY + scene.height，水平第二轴使用 scene.y。
  const domain = { minX: offsetX, maxX, minY: offsetY, maxY, minZ: offsetZ, maxZ }
  const token = `${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2)}`
  const runtimeDirectory = resolve(baseDir, `.histream-runtime-${token}`)
  const temporaryPaths = []
  const retainedItems = []
  const boundsCache = new Map()
  let total = 0
  let kept = 0
  let excluded = 0
  let invalid = 0
  let clippedObjects = 0
  let sourceTriangles = 0
  let retainedTriangles = 0
  let grounded = 0
  // 贴地所需的地形高度函数；无地形或未提供时按平地 0 m 处理。
  const heightAt = scene.terrain && options.terrainHeight ? options.terrainHeight : () => 0

  // 清理：删除本次生成的所有临时文件，再尝试删除（必须已为空的）运行时目录；失败静默忽略。
  const cleanup = () => {
    for (const path of temporaryPaths.splice(0)) {
      try {
        if (existsSync(path)) unlinkSync(path)
      } catch {}
    }
    try {
      if (existsSync(runtimeDirectory)) rmdirSync(runtimeDirectory)
    } catch {}
  }

  try {
    // 步骤 1：路径绝对化。输出目录、DEM、气象（除内置 defined/meteo.txt）、大气 LUT、光谱及其纹理、
    // 对象 OBJ 与位置文件都改成绝对路径，因为运行时 project.json 位于临时目录，相对路径会失效。
    mkdirSync(runtimeDirectory)
    const absoluteAsset = (value) => String(value || '').trim()
      ? resolveAssetPath(value, baseDir) : ''
    const configuration = project.configuration
    configuration.outDir = absoluteAsset(configuration.outDir || 'output')
    if (configuration.scene.demFile) configuration.scene.demFile = absoluteAsset(configuration.scene.demFile)
    if (configuration.meteo?.path && configuration.meteo.path !== 'defined/meteo.txt') {
      configuration.meteo.path = absoluteAsset(configuration.meteo.path)
    }
    if (configuration.atmosphere?.lutFile) {
      configuration.atmosphere.lutFile = absoluteAsset(configuration.atmosphere.lutFile)
    }
    for (const spectrum of configuration.spectra || []) {
      if (spectrum.fileName) spectrum.fileName = absoluteAsset(spectrum.fileName)
      for (const texture of [spectrum.physicalTexture, spectrum.classTexture, spectrum.opennessTexture]) {
        if (texture?.fileName) texture.fileName = absoluteAsset(texture.fileName)
      }
    }
    for (const item of configuration.objects?.items || []) {
      if (item.fileName) item.fileName = absoluteAsset(item.fileName)
      if (item.positionFile) item.positionFile = absoluteAsset(item.positionFile)
    }

    // 步骤 2：逐对象处理。没有位置文件的对象（如单实例）原样保留；有位置文件的按实例筛选。
    for (const [index, item] of (project.configuration.objects?.items || []).entries()) {
      if (!item.positionFile) {
        total += 1
        kept += 1
        retainedItems.push(item)
        continue
      }

      // 步骤 2.1：读取位置文件，统计总实例数与无效行；并取得该对象的局部包围盒
      // （有 OBJ 时读文件求包围盒并按路径缓存，Fire/Fog/介质对象使用基元包围盒）。
      const sourcePath = resolveAssetPath(item.positionFile, baseDir)
      if (!sourcePath || !existsSync(sourcePath) || !statSync(sourcePath).isFile()) {
        throw new Error(`找不到对象位置文件：${sourcePath || item.positionFile}`)
      }
      const parsed = positionRecords(readFileSync(sourcePath, 'utf8'))
      total += parsed.records.length
      invalid += parsed.invalid
      let localBounds = primitiveBounds(item)
      let objectContent = ''
      if (item.fileName && item.type !== 'Fire' && item.type !== 'Fog' && !item.medium) {
        const objectPath = resolveAssetPath(item.fileName, baseDir)
        if (!objectPath || !existsSync(objectPath) || !statSync(objectPath).isFile()) {
          throw new Error(`找不到 OBJ 文件：${objectPath || item.fileName}`)
        }
        if (!boundsCache.has(objectPath)) {
          objectContent = readFileSync(objectPath, 'utf8')
          boundsCache.set(objectPath, { bounds: objBounds(objectContent, localBounds), content: objectContent })
        }
        const cached = boundsCache.get(objectPath)
        localBounds = cached.bounds
        objectContent = cached.content
      }
      // 步骤 2.2：贴地对象（车辆等）按足迹计算放置高度（含地形），覆盖位置文件中的高度。
      // Vehicles never float: drop each placement until its footprint rests on the terrain.
      if (isGroundedObject(item)) {
        const footprint = item.groundFootprint || localBounds
        for (const record of parsed.records) {
          record.y = offsetY + groundedPlacementHeight(footprint,
            { north: record.x - offsetX, east: record.z - offsetZ, scale: record.scale, rotation: record.rotation }, heightAt)
          grounded += 1
        }
      }
      // 步骤 2.3：保留包围盒与计算域相交的实例，其余计入 excluded。
      const retainedRecords = parsed.records.filter((position) => {
        const inside = intersectsDomain(transformedBounds(localBounds, position), domain)
        if (!inside) excluded += 1
        return inside
      })
      // 步骤 2.4：仅当对象只有一个实例且越出计算域时才裁剪 OBJ（多实例时整体保留，不拆分）；
      // 裁剪后若没有任何三角形落在域内则整个实例被排除。
      if (retainedRecords.length === 1 && parsed.records.length === 1 && objectContent) {
        const placementBounds = transformedBounds(localBounds, retainedRecords[0])
        if (!containedByDomain(placementBounds, domain, !scene.terrain)) {
          const clipped = clippedObjContent(
            objectContent,
            retainedRecords[0],
            domain,
            item.meshes?.[0]?.name || item.name,
            !scene.terrain
          )
          if (clipped && clipped.outputTriangles > 0) {
            const objectPath = resolve(runtimeDirectory, `object-${index}.obj`)
            writeFileSync(objectPath, clipped.content, 'utf8')
            temporaryPaths.push(objectPath)
            item.fileName = basename(objectPath)
            clippedObjects += 1
            sourceTriangles += clipped.sourceTriangles
            retainedTriangles += clipped.outputTriangles
          } else if (clipped) {
            excluded += retainedRecords.length
            continue
          }
        }
      }
      // 步骤 2.5：把实例坐标换成相对计算域原点的局部坐标（减去 offset），写运行时位置文件 position-N.txt。
      const localRecords = retainedRecords.map((position) => ({
        ...position,
        x: position.x - offsetX,
        y: position.y - offsetY,
        z: position.z - offsetZ
      }))
      kept += localRecords.length
      if (!localRecords.length) continue

      const positionPath = resolve(runtimeDirectory, `position-${index}.txt`)
      const content = localRecords.map((position) => [
        runtimeNumber(position.x),
        runtimeNumber(position.z),
        runtimeNumber(position.y),
        runtimeNumber(position.scale),
        runtimeNumber(position.rotation)
      ].join(' ')).join('\n') + '\n'
      writeFileSync(positionPath, content, 'utf8')
      temporaryPaths.push(positionPath)
      retainedItems.push({ ...item, positionFile: basename(positionPath) })
    }

    // 步骤 3：展开表面分类/开孔率纹理（可能改写对象 OBJ 与光谱列表）。
    const textureSummary = applySurfaceTextures({
      items: retainedItems,
      spectra: configuration.spectra,
      readAsset: (path) => readFileSync(path),
      objectPath: (item) => isAbsolute(item.fileName) ? item.fileName : resolve(runtimeDirectory, item.fileName),
      readObject: (path) => readFileSync(path, 'utf8'),
      writeObject: (index, content) => {
        const path = resolve(runtimeDirectory, `object-${index}-textured.obj`)
        writeFileSync(path, content, 'utf8')
        temporaryPaths.push(path)
        return basename(path)
      }
    })
    // 步骤 4：把多孔（开孔率）光谱展开为有效光谱，必要时写出 spectrum-N.txt 临时文件。
    configuration.spectra = configuration.spectra.map((spectrum, index) => effectivePorousSpectrum(spectrum, {
      readSpectrumFile: (path) => readFileSync(path, 'utf8'),
      writeSpectrumFile: (content) => {
        const path = resolve(runtimeDirectory, `spectrum-${index}.txt`)
        writeFileSync(path, content, 'utf8')
        temporaryPaths.push(path)
        return basename(path)
      }
    }))

    // 步骤 5：用筛选后的对象替换原列表，并把场景偏移归零（上面的坐标已减去 offset），写出运行时 project.json。
    project.configuration.objects.items = retainedItems
    project.configuration.objects.count = retainedItems.length
    project.configuration.objects.names = retainedItems.map((item) => item.name)
    scene.offsetX = 0
    scene.offsetY = 0
    scene.offsetZ = 0

    const runtimeProjectPath = resolve(runtimeDirectory, 'project.json')
    writeFileSync(runtimeProjectPath, stringifyProject(project), 'utf8')
    temporaryPaths.push(runtimeProjectPath)
    return {
      inputPath: runtimeProjectPath,
      project,
      textureSummary,
      total,
      kept,
      excluded,
      invalid,
      clippedObjects,
      sourceTriangles,
      retainedTriangles,
      grounded,
      cleanup
    }
  } catch (error) {
    cleanup()
    throw error
  }
}
