// Surface class and openness textures, applied while staging a run so every
// engine mode sees ordinary OBJ groups bound to ordinary spectra.
//
// classTexture:    { enabled, fileName (PNG), repeatSize, classes: [{ color: '#rrggbb', spectralName }] }
//                  Each face takes the spectrum of the class whose colour is closest
//                  to the texel at its centroid.
// opennessTexture: { enabled, fileName (PNG), repeatSize, minOpenness, maxOpenness, levels }
//                  Texel brightness (0 black .. 1 white) maps linearly to openness and
//                  is quantised to `levels` derived spectra.
// Lookup uses OBJ-local coordinates (the pattern moves with the object) and the
// dominant axis of the face normal, matching the physical-texture projection.
import { decodePng } from './png.mjs'

export function hasSurfaceTexture(spectrum) {
  return Boolean(spectrum?.classTexture?.enabled || spectrum?.opennessTexture?.enabled)
}

const parseColor = (value) => {
  const match = /^#?([0-9a-f]{6})$/i.exec(String(value || '').trim())
  if (!match) throw new Error(`分类纹理颜色无效：${value}`)
  const number = parseInt(match[1], 16)
  return [(number >> 16) & 255, (number >> 8) & 255, number & 255]
}

function textureSampler(image, repeatSize) {
  const inverseRepeat = 1 / Math.max(0.01, Number(repeatSize) || 1)
  return (center, normal) => {
    const [ax, ay, az] = normal.map(Math.abs)
    const [u, v] = ax >= ay && ax >= az ? [center[2], center[1]] : ay >= az ? [center[0], center[2]] : [center[0], center[1]]
    const fu = u * inverseRepeat - Math.floor(u * inverseRepeat)
    const fv = v * inverseRepeat - Math.floor(v * inverseRepeat)
    const column = Math.min(image.width - 1, Math.floor(fu * image.width))
    const row = Math.min(image.height - 1, Math.floor(fv * image.height))
    return row * image.width + column
  }
}

function parseObj(content, defaultGroup) {
  const vertices = []
  const others = []
  const faces = []
  // Running totals of v / vt / vn records, for resolving relative references.
  const counts = [0, 0, 0]
  let group = defaultGroup
  for (const line of String(content).split(/\r?\n/)) {
    const trimmed = line.trim()
    if (!trimmed) continue
    if (trimmed.startsWith('v ')) {
      const values = trimmed.split(/\s+/)
      vertices.push([Number(values[1]), Number(values[2]), Number(values[3])])
      counts[0] += 1
      others.push(trimmed)
    } else if (trimmed.startsWith('g ') || trimmed.startsWith('o ')) {
      group = trimmed.slice(2).trim() || defaultGroup
    } else if (trimmed.startsWith('f ')) {
      // Faces are re-emitted after all vertex data, so relative (negative)
      // references must become absolute 1-based indices.
      const refs = trimmed.slice(2).trim().split(/\s+/).map((ref) => ref.split('/').map((part, slot) => {
        if (part === '') return ''
        const index = parseInt(part, 10)
        return String(index < 0 ? counts[slot] + index + 1 : index)
      }).join('/'))
      const ids = refs.map((ref) => parseInt(ref.split('/')[0], 10) - 1)
      faces.push({ line: `f ${refs.join(' ')}`, group, ids })
    } else if (!/^(usemtl|mtllib|s)\b/.test(trimmed)) {
      if (trimmed.startsWith('vt ')) counts[1] += 1
      else if (trimmed.startsWith('vn ')) counts[2] += 1
      others.push(trimmed)
    }
  }
  return { vertices, others, faces }
}

function faceGeometry(vertices, ids) {
  const center = [0, 0, 0]
  const normal = [0, 0, 0]
  for (let k = 0; k < ids.length; k += 1) {
    const a = vertices[ids[k]], b = vertices[ids[(k + 1) % ids.length]]
    if (!a || !b) throw new Error('OBJ 面引用了不存在的顶点')
    for (let axis = 0; axis < 3; axis += 1) center[axis] += a[axis] / ids.length
    // Newell's method handles quads and polygons.
    normal[0] += (a[1] - b[1]) * (a[2] + b[2])
    normal[1] += (a[2] - b[2]) * (a[0] + b[0])
    normal[2] += (a[0] - b[0]) * (a[1] + b[1])
  }
  return { center, normal }
}

// items: runtime object items (fileName already resolved); spectra: runtime
// spectra list (appended with derived openness spectra). Returns summary lines.
export function applySurfaceTextures({ items, spectra, readAsset, objectPath, readObject, writeObject }) {
  const byName = new Map(spectra.map((spectrum) => [spectrum.name, spectrum]))
  const images = new Map()
  const image = (fileName) => {
    if (!images.has(fileName)) images.set(fileName, decodePng(readAsset(fileName), fileName))
    return images.get(fileName)
  }
  const classMaps = new Map()
  const classMap = (spectrum) => {
    if (classMaps.has(spectrum.name)) return classMaps.get(spectrum.name)
    const texture = spectrum.classTexture
    const classes = (texture.classes || []).filter((entry) => entry && entry.spectralName)
    if (!classes.length) throw new Error(`光谱 ${spectrum.name} 的分类纹理没有设置类别`)
    for (const entry of classes) {
      const target = byName.get(entry.spectralName)
      if (!target) throw new Error(`分类纹理引用了不存在的光谱：${entry.spectralName}`)
      if (target.classTexture?.enabled) throw new Error(`分类纹理的类别光谱不能再启用分类纹理：${entry.spectralName}`)
    }
    const colors = classes.map((entry) => parseColor(entry.color))
    const picture = image(texture.fileName)
    const map = new Uint8Array(picture.width * picture.height)
    for (let pixel = 0; pixel < map.length; pixel += 1) {
      let best = 0, bestDistance = Infinity
      for (let k = 0; k < colors.length; k += 1) {
        const dr = picture.rgb[pixel * 3] - colors[k][0], dg = picture.rgb[pixel * 3 + 1] - colors[k][1], db = picture.rgb[pixel * 3 + 2] - colors[k][2]
        const distance = dr * dr + dg * dg + db * db
        if (distance < bestDistance) { bestDistance = distance; best = k }
      }
      map[pixel] = best
    }
    const result = { map, classes, sample: textureSampler(picture, texture.repeatSize) }
    classMaps.set(spectrum.name, result)
    return result
  }
  const derived = new Map()
  const opennessSpectrum = (spectrum, level, value) => {
    const name = `${spectrum.name}__open${level}`
    if (!derived.has(name)) {
      const { opennessTexture: _texture, classTexture: _classes, ...base } = spectrum
      const copy = { ...base, name, label: `${spectrum.label || spectrum.name}（开孔率 ${Number(value.toFixed(3))}）`, openness: Number(value.toFixed(4)) }
      derived.set(name, copy)
      spectra.push(copy)
      byName.set(name, copy)
    }
    return name
  }
  const opennessSamplers = new Map()
  const resolveOpenness = (spectrum, center, normal) => {
    const texture = spectrum.opennessTexture
    if (!texture?.enabled) return spectrum.name
    if (spectrum.model === 'Prospect' || spectrum.model === 'BSM') throw new Error(`开孔率纹理仅支持自定义或导入 TXT 光谱：${spectrum.name}`)
    if (!opennessSamplers.has(spectrum.name)) {
      const picture = image(texture.fileName)
      opennessSamplers.set(spectrum.name, { picture, sample: textureSampler(picture, texture.repeatSize) })
    }
    const { picture, sample } = opennessSamplers.get(spectrum.name)
    const pixel = sample(center, normal)
    const brightness = (picture.rgb[pixel * 3] + picture.rgb[pixel * 3 + 1] + picture.rgb[pixel * 3 + 2]) / (3 * 255)
    const levels = Math.max(2, Math.min(32, Math.round(Number(texture.levels) || 8)))
    const level = Math.round(brightness * (levels - 1))
    const low = Math.max(0, Math.min(0.95, Number(texture.minOpenness) || 0))
    const high = Math.max(low, Math.min(0.95, Number(texture.maxOpenness ?? 0.5)))
    return opennessSpectrum(spectrum, level, low + (high - low) * level / (levels - 1))
  }

  const summary = []
  for (const [index, item] of items.entries()) {
    const meshes = item.meshes || []
    if (!item.fileName || !meshes.some((mesh) => hasSurfaceTexture(byName.get(mesh.spectralName)))) continue
    const defaultGroup = meshes[0]?.name || item.name
    const parsed = parseObj(readObject(objectPath(item)), defaultGroup)
    const bindingByGroup = new Map(meshes.map((mesh) => [mesh.name, mesh]))
    const outputGroups = new Map()
    const counts = new Map()
    for (const face of parsed.faces) {
      const binding = bindingByGroup.get(face.group)
      const base = binding && byName.get(binding.spectralName)
      let groupName = face.group, spectralName = binding?.spectralName
      if (hasSurfaceTexture(base)) {
        const { center, normal } = faceGeometry(parsed.vertices, face.ids)
        let spectrum = base
        if (base.classTexture?.enabled) {
          const classes = classMap(base)
          spectrum = byName.get(classes.classes[classes.map[classes.sample(center, normal)]].spectralName)
        }
        spectralName = resolveOpenness(spectrum, center, normal)
        groupName = `${face.group}__${spectralName}`
        counts.set(spectralName, (counts.get(spectralName) || 0) + 1)
      }
      if (!outputGroups.has(groupName)) outputGroups.set(groupName, { source: face.group, spectralName, lines: [] })
      outputGroups.get(groupName).lines.push(face.line)
    }
    const lines = ['# Regrouped by Stream Studio surface class/openness textures', ...parsed.others]
    const newMeshes = []
    const emitted = new Set()
    for (const [groupName, group] of outputGroups) {
      lines.push(`g ${groupName}`, ...group.lines)
      const binding = bindingByGroup.get(group.source)
      if (!binding) continue
      if (groupName === group.source) { newMeshes.push(binding); emitted.add(binding.name); continue }
      newMeshes.push({ ...binding, name: groupName, spectralName: group.spectralName })
      emitted.add(binding.name)
    }
    for (const mesh of meshes) if (!emitted.has(mesh.name)) newMeshes.push(mesh)
    item.fileName = writeObject(index, lines.join('\n') + '\n')
    item.meshes = newMeshes
    const total = [...counts.values()].reduce((sum, value) => sum + value, 0)
    summary.push(`${item.name}: ${[...counts].map(([name, count]) => `${name} ${(100 * count / total).toFixed(1)}%`).join('，')}`)
  }
  return summary
}
