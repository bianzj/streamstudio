// Turns a histream-usd manifest (OBJ files + placements) into the object list of a Stream Studio project.
import { normalizeProject } from '../src/renderer/src/project-schema.js'

const simplify = (value) => String(value ?? '').toLowerCase().replace(/[^a-z0-9]+/g, '')
const round = (value) => {
  const rounded = Math.round(Number(value) * 1e6) / 1e6
  return Number.isFinite(rounded) && !Object.is(rounded, -0) ? rounded : 0
}
const MAX_INSTANCES = 100000

function isPrimitiveItem(item) {
  return !item.fileName || item.type === 'Fire' || item.type === 'Fog' || Boolean(item.medium)
    || (item.type === 'Water' && item.sourceKind === 'prim')
}

function parseEmbeddedProject(manifest) {
  if (!manifest.histream?.project) return null
  try { return normalizeProject(JSON.parse(manifest.histream.project)) } catch { return null }
}

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
    const records = placements.map((placement) => [
      round(placement.north),
      round(placement.east),
      round(placement.y - heightAt(placement.north - offsetX, placement.east - offsetZ)),
      round(placement.scale),
      round(placement.rotation)
    ].join(' '))
    const text = records.join('\n')
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
