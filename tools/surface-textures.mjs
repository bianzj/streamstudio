// ==========================================================================
// 文件：surface-textures.mjs
// 功能：运行准备阶段把“分类纹理 / 开孔率纹理”展开为普通的 OBJ 分组 + 普通光谱绑定，
//       使各引擎模式无需理解纹理（下方原有英文注释给出两种纹理的字段定义）
// 主要模块：
//   1. hasSurfaceTexture / parseColor：纹理判定与颜色解析（#rrggbb → [r,g,b]）
//   2. textureSampler：按 OBJ 局部坐标与法线主轴，把面中心映射为纹理像素下标（周期平铺）
//   3. parseObj / faceGeometry：解析 OBJ（相对索引转绝对）、求面中心与 Newell 法线
//   4. applySurfaceTextures：逐面查纹理 → 选定光谱（类别最近色 / 量化开孔率派生光谱）→
//      按“原分组__光谱名”重新分组，输出新 OBJ 并改写对象的 meshes 绑定
// 数据流：runtime-scene.mjs 提供 items / spectra / 读写回调 → 本文件 → 新 OBJ 文件 + 派生光谱
// 与其他文件：依赖 png.mjs 解码 PNG；被 runtime-scene.mjs 调用。
// 约定：纹理 v 方向直接用 OBJ 的 z/y 坐标，不做翻转（待核对）；开孔率限制在 [0, 0.95]。
// ==========================================================================

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

// 判定光谱是否启用了分类纹理或开孔率纹理。
export function hasSurfaceTexture(spectrum) {
  return Boolean(spectrum?.classTexture?.enabled || spectrum?.opennessTexture?.enabled)
}

// 解析 #rrggbb（# 可省略），返回 [r, g, b]（0–255）；非法值抛错。
const parseColor = (value) => {
  const match = /^#?([0-9a-f]{6})$/i.exec(String(value || '').trim())
  if (!match) throw new Error(`分类纹理颜色无效：${value}`)
  const number = parseInt(match[1], 16)
  return [(number >> 16) & 255, (number >> 8) & 255, number & 255]
}

// 作用：返回采样函数 (面中心, 法线) → 像素下标（行优先）。repeatSize 为纹理平铺一周的物体尺寸（m）。
// 按法线绝对值最大的轴选投影平面（X 主导取 (z,y)，Y 主导取 (x,z)，否则取 (x,y)），坐标取小数部分后映射到像素。
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

// 作用：解析 OBJ 为 { vertices, others, faces }。others 保留 v/vt/vn 等非面行（去掉 usemtl/mtllib/s），
// faces 记录原行（索引已转绝对）、所属分组与顶点下标。后续按面重新分组，所以面要晚于顶点输出。
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

// 作用：求多边形面的中心（顶点平均）与未归一化法线（Newell 法）；顶点缺失时抛错。
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
// 步骤 1：准备缓存与辅助函数：image（PNG 解码缓存）、classMap（分类纹理像素→最近类别）、
// opennessSpectrum / resolveOpenness（开孔率量化并派生光谱）。
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
    // 对每个像素在各类别颜色中取 RGB 欧氏距离最小者（不做色彩空间变换），预先算好类别下标图。
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
  // 按量化等级生成/复用派生光谱 <名>__open<level>：复制原光谱并去掉纹理字段，写入 openness。
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
    // 亮度取 RGB 平均（0 黑 … 1 白），量化为 levels 档（限制 2–32），线性映射到 [minOpenness, maxOpenness]。
    const brightness = (picture.rgb[pixel * 3] + picture.rgb[pixel * 3 + 1] + picture.rgb[pixel * 3 + 2]) / (3 * 255)
    const levels = Math.max(2, Math.min(32, Math.round(Number(texture.levels) || 8)))
    const level = Math.round(brightness * (levels - 1))
    const low = Math.max(0, Math.min(0.95, Number(texture.minOpenness) || 0))
    const high = Math.max(low, Math.min(0.95, Number(texture.maxOpenness ?? 0.5)))
    return opennessSpectrum(spectrum, level, low + (high - low) * level / (levels - 1))
  }

  // 步骤 2：逐对象处理。只处理绑定了含纹理光谱的对象；先解析 OBJ，再逐面选光谱并按“原分组__光谱名”归组。
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
    // 步骤 3：输出新 OBJ（先写顶点等非面行，再按组写面）并生成新的 meshes 绑定；未被引用的原绑定保留。
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
    // 写出重组后的 OBJ，改用新文件；summary 记录各光谱占面数的百分比，供日志显示。
    item.fileName = writeObject(index, lines.join('\n') + '\n')
    item.meshes = newMeshes
    const total = [...counts.values()].reduce((sum, value) => sum + value, 0)
    summary.push(`${item.name}: ${[...counts].map(([name, count]) => `${name} ${(100 * count / total).toFixed(1)}%`).join('，')}`)
  }
  return summary
}
