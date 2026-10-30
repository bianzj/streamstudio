import { existsSync, mkdirSync, readFileSync, rmdirSync, statSync, unlinkSync, writeFileSync } from 'node:fs'
import { basename, dirname, isAbsolute, resolve } from 'node:path'
import { effectivePorousSpectrum, groundedPlacementHeight, isGroundedObject, normalizeProject, stringifyProject } from '../src/renderer/src/project-schema.js'
import { applySurfaceTextures } from './surface-textures.mjs'

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

function runtimeNumber(value) {
  return Object.is(value, -0) ? '0' : String(value)
}

function defaultAssetPath(value, baseDir) {
  return isAbsolute(value) ? resolve(value) : resolve(baseDir, value)
}

function primitiveBounds(item) {
  const dimensions = Array.isArray(item.dimensions) ? item.dimensions.map(Number) : []
  const sizeX = Math.abs(Number.isFinite(dimensions[0]) ? dimensions[0] : 0)
  const sizeY = Math.abs(Number.isFinite(dimensions[1]) ? dimensions[1] : 0)
  const sizeZ = Math.abs(Number.isFinite(dimensions[2]) ? dimensions[2] : 0)
  return { minX: -sizeX / 2, maxX: sizeX / 2, minY: 0, maxY: sizeY, minZ: -sizeZ / 2, maxZ: sizeZ / 2 }
}

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

function intersectsDomain(bounds, domain) {
  return bounds.maxX >= domain.minX && bounds.minX <= domain.maxX
    && bounds.maxY >= domain.minY && bounds.minY <= domain.maxY
    && bounds.maxZ >= domain.minZ && bounds.minZ <= domain.maxZ
}

function containedByDomain(bounds, domain, checkHeight = true) {
  return bounds.minX >= domain.minX && bounds.maxX <= domain.maxX
    && (!checkHeight || (bounds.minY >= domain.minY && bounds.maxY <= domain.maxY))
    && bounds.minZ >= domain.minZ && bounds.maxZ <= domain.maxZ
}

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
  const heightAt = scene.terrain && options.terrainHeight ? options.terrainHeight : () => 0

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

    for (const [index, item] of (project.configuration.objects?.items || []).entries()) {
      if (!item.positionFile) {
        total += 1
        kept += 1
        retainedItems.push(item)
        continue
      }

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
      // Vehicles never float: drop each placement until its footprint rests on the terrain.
      if (isGroundedObject(item)) {
        const footprint = item.groundFootprint || localBounds
        for (const record of parsed.records) {
          record.y = offsetY + groundedPlacementHeight(footprint,
            { north: record.x - offsetX, east: record.z - offsetZ, scale: record.scale, rotation: record.rotation }, heightAt)
          grounded += 1
        }
      }
      const retainedRecords = parsed.records.filter((position) => {
        const inside = intersectsDomain(transformedBounds(localBounds, position), domain)
        if (!inside) excluded += 1
        return inside
      })
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
    configuration.spectra = configuration.spectra.map((spectrum, index) => effectivePorousSpectrum(spectrum, {
      readSpectrumFile: (path) => readFileSync(path, 'utf8'),
      writeSpectrumFile: (content) => {
        const path = resolve(runtimeDirectory, `spectrum-${index}.txt`)
        writeFileSync(path, content, 'utf8')
        temporaryPaths.push(path)
        return basename(path)
      }
    }))

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
