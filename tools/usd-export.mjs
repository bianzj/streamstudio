// Exports a HiStream project as ASCII OpenUSD in editor-preview axes (Y-up, X = north, Z = east).
import { createHash } from 'node:crypto'
import { closeSync, existsSync, mkdirSync, openSync, readdirSync, readFileSync, renameSync, statSync, unlinkSync, writeFileSync, writeSync } from 'node:fs'
import { basename, dirname, extname, join, relative, resolve } from 'node:path'
import { groundedPlacementHeight, isGroundedObject, stringifyProject } from '../src/renderer/src/project-schema.js'

export const USD_EXPORT_FORMAT = 'histream-usd-v1'
const ARRAY_CHUNK = 8192
// Bump when the prototype asset layout changes so cached assets are rebuilt.
const ASSET_VERSION = 2
const ASSET_FILE = /^[A-Za-z0-9_]+-[0-9a-f]{12}\.(usda|json)$/

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

function number(value) {
  const rounded = Math.round(Number(value) * 1e6) / 1e6
  return Number.isFinite(rounded) && !Object.is(rounded, -0) ? String(rounded) : '0'
}

function tuple(values) {
  return `(${Array.from(values, number).join(', ')})`
}

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

// heightAt(north, east) returns terrain height above the DEM minimum in local scene coordinates.
export function exportProjectUsd(project, options) {
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

    const light = configuration.light || {}
    writer.write('    def DistantLight "Sun"\n    {\n')
    writer.write('        float inputs:angle = 0.53\n        float inputs:intensity = 3\n')
    writer.write(`        matrix4d xformOp:transform = ${sunTransform(light.zenith ?? 30, light.azimuth ?? 135)}\n`)
    writer.write('        uniform token[] xformOpOrder = ["xformOp:transform"]\n')
    writer.write(attributeLines('        ', { zenith: Number(light.zenith), azimuth: Number(light.azimuth) }))
    writer.write('    }\n\n')

    writer.write('    def Scope "Objects"\n    {\n')
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
  if (existsSync(options.assetsDirectory)) {
    for (const name of readdirSync(options.assetsDirectory)) {
      if (ASSET_FILE.test(name) && !usedAssets.has(name)) {
        try { unlinkSync(join(options.assetsDirectory, name)) } catch {}
      }
    }
  }
  return { path: options.outputPath, ...stats, materials: materialPaths.size }
}
