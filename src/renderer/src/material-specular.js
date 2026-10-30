const finite = (value, fallback) => value == null || String(value).trim() === '' || !Number.isFinite(Number(value)) ? fallback : Number(value)

export function normalizeSpecular(value = {}) {
  value = value || {}
  return {
    enabled: value.enabled === true || value.enabled === 1 || ['true', '1', 'yes', 'on'].includes(String(value.enabled).toLowerCase()),
    fraction: Math.max(0, Math.min(1, finite(value.fraction, 0.5))),
    roughness: Math.max(0.05, Math.min(1, finite(value.roughness, 0.25)))
  }
}

// Explicit Mesh settings own the decision. Shared settings only migrate old files.
export function meshSpecular(mesh, spectra = [], control = {}, fallbackSpectrum = '') {
  const legacy = normalizeSpecular(spectra.find(item => item.name === (mesh.spectralName || fallbackSpectrum))?.specular)
  if (mesh.specularEnabled !== undefined) return { ...legacy, enabled: normalizeSpecular({ enabled: mesh.specularEnabled }).enabled }
  if (mesh.specular && typeof mesh.specular === 'object') return { ...legacy, enabled: normalizeSpecular(mesh.specular).enabled }
  return { ...legacy, enabled: control.specularEnabled !== false && legacy.enabled }
}

export function meshSpecularRunError(mode, configuration = {}) {
  if (mode === 'eRaytracing') return ''
  const { spectra = [], control = {}, objects = {}, scene = {} } = configuration
  const active = []
  for (const object of objects.items || []) {
    for (const mesh of object.meshes?.length ? object.meshes : [object]) {
      const setting = meshSpecular(mesh, spectra, control, object.spectralName)
      if (setting.enabled && setting.fraction > 0) active.push(`${object.name} / ${mesh.name}`)
    }
  }
  const background = meshSpecular(scene.background || {}, spectra, control, 'soil')
  if (background.enabled && background.fraction > 0) active.push('背景')
  return active.length ? `镜面反射请选择 eRaytracing（简单光线追踪），或在对应表面属性中关闭镜面反射：${active.join('、')}` : ''
}

export function meshSpecularFields(setting) {
  const s = normalizeSpecular(setting)
  return `<div class="mesh-specular-fields"><label><span>使用镜面反射</span><select class="select mesh-specular-enabled"><option value="false" ${s.enabled ? '' : 'selected'}>关闭</option><option value="true" ${s.enabled ? 'selected' : ''}>启用</option></select></label></div>`
}

export function meshSpecularFromRow(row) {
  return row.querySelector('.mesh-specular-enabled').value === 'true'
}

export function specularParametersFromValues(values, previous = {}) {
  // FIXED[低] Bug: 输入框清空时值为 ''，?? 不会回退到 previous，finite('') 再回退到 0.5/0.25 默认值，用户原有参数被重置；已修复：空字符串时改用 previous 的对应值，仅真正无输入时才回退默认值
  const fraction = values.specularFraction == null || String(values.specularFraction).trim() === '' ? previous.fraction : values.specularFraction
  const roughness = values.specularRoughness == null || String(values.specularRoughness).trim() === '' ? previous.roughness : values.specularRoughness
  return normalizeSpecular({ ...previous, fraction, roughness })
}

export function specularParameterFields(value, editable = true) {
  const s = normalizeSpecular(value)
  const disabled = editable ? '' : 'disabled'
  return `<label class="dialog-field"><span>镜面比例（0–1）</span><input class="input" type="number" name="specularFraction" min="0" max="1" step="0.01" value="${s.fraction}" ${disabled}></label><label class="dialog-field"><span>粗糙度（0.05–1）</span><input class="input" type="number" name="specularRoughness" min="0.05" max="1" step="0.01" value="${s.roughness}" ${disabled}></label><div class="notice" style="grid-column:1 / -1">镜面比例和粗糙度由该光谱属性提供；是否使用，在“场景对象 → 编辑 OBJ 计算属性”中逐 Mesh 设置。仅简单光线追踪的短波计算生效。</div>`
}


