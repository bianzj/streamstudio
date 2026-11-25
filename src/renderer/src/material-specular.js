// ==========================================================================
// 文件：material-specular.js
// 功能：镜面反射（specular）设置的规范化、新旧工程迁移、运行前校验与表单片段生成
// 主要模块：
//   1. normalizeSpecular：把任意输入规整为 { enabled, fraction, roughness }
//   2. meshSpecular：按 Mesh 取“是否启用”（Mesh 显式设置优先，旧的共享设置仅用于迁移）
//   3. meshSpecularRunError：运行前检查，镜面反射只支持 eRaytracing
//   4. meshSpecularFields / meshSpecularFromRow：逐 Mesh 的“使用镜面反射”下拉框及其读取
//   5. specularParametersFromValues / specularParameterFields：光谱属性里的镜面比例与粗糙度
// 数据流：光谱属性（spectra[].specular）提供 fraction/roughness；Mesh 或背景提供 enabled；
//   main.js 在材质/对象对话框中调用本文件生成表单并读回，运行前调用 meshSpecularRunError
// 约定：fraction 为 0–1 的无量纲比例，默认 0.5；roughness 范围 0.05–1，默认 0.25
// 注意：UI 文案提示镜面反射仅对简单光线追踪的短波计算生效
// ==========================================================================

// 工具：把输入转为有限数值；空值、空串、非数值（NaN/Infinity）一律返回 fallback。
const finite = (value, fallback) => value == null || String(value).trim() === '' || !Number.isFinite(Number(value)) ? fallback : Number(value)

// 作用：规整镜面反射设置，保证字段齐全且在合法范围内。
// 输入：任意对象（可为空）；输出：{ enabled: 布尔, fraction: 0–1, roughness: 0.05–1 }。
// 说明：enabled 兼容布尔、1 以及字符串 true/1/yes/on（旧工程/表单值）；
//   roughness 下限取 0.05，避免出现零粗糙度（理想镜面）。
export function normalizeSpecular(value = {}) {
  value = value || {}
  return {
    enabled: value.enabled === true || value.enabled === 1 || ['true', '1', 'yes', 'on'].includes(String(value.enabled).toLowerCase()),
    fraction: Math.max(0, Math.min(1, finite(value.fraction, 0.5))),
    roughness: Math.max(0.05, Math.min(1, finite(value.roughness, 0.25)))
  }
}

// 作用：求某个 Mesh 的镜面反射设置。fraction/roughness 取自其光谱属性（legacy），
//   enabled 的优先级：mesh.specularEnabled > mesh.specular > 旧共享开关 control.specularEnabled。
// 输入：mesh、光谱列表 spectra、control、mesh 未指定光谱时的备用光谱名 fallbackSpectrum。
// Explicit Mesh settings own the decision. Shared settings only migrate old files.
export function meshSpecular(mesh, spectra = [], control = {}, fallbackSpectrum = '') {
  const legacy = normalizeSpecular(spectra.find(item => item.name === (mesh.spectralName || fallbackSpectrum))?.specular)
  if (mesh.specularEnabled !== undefined) return { ...legacy, enabled: normalizeSpecular({ enabled: mesh.specularEnabled }).enabled }
  if (mesh.specular && typeof mesh.specular === 'object') return { ...legacy, enabled: normalizeSpecular(mesh.specular).enabled }
  return { ...legacy, enabled: control.specularEnabled !== false && legacy.enabled }
}

// 作用：运行前校验。有任何表面（Mesh 或背景）启用了镜面反射且 fraction > 0，
//   而当前模式不是 eRaytracing 时，返回提示文案；无问题返回空串。
// 说明：对象没有 meshes 时把对象自身当作一个 Mesh；背景缺省按 'soil' 光谱处理。
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

// 作用：生成逐 Mesh 表格单元里的“使用镜面反射”下拉框 HTML（只含启用开关，
//   比例与粗糙度在光谱属性对话框里编辑）。
export function meshSpecularFields(setting) {
  const s = normalizeSpecular(setting)
  return `<div class="mesh-specular-fields"><label><span>使用镜面反射</span><select class="select mesh-specular-enabled"><option value="false" ${s.enabled ? '' : 'selected'}>关闭</option><option value="true" ${s.enabled ? 'selected' : ''}>启用</option></select></label></div>`
}

// 作用：从表格行读回“使用镜面反射”下拉框的取值（布尔）。
export function meshSpecularFromRow(row) {
  return row.querySelector('.mesh-specular-enabled').value === 'true'
}

// 作用：由对话框表单值得到镜面参数；字段为空时沿用 previous 中原有取值。
//   enabled 不在此处理（由 Mesh 级设置决定），仅 fraction 与 roughness 来自表单。
export function specularParametersFromValues(values, previous = {}) {
  const fraction = values.specularFraction == null || String(values.specularFraction).trim() === '' ? previous.fraction : values.specularFraction
  const roughness = values.specularRoughness == null || String(values.specularRoughness).trim() === '' ? previous.roughness : values.specularRoughness
  return normalizeSpecular({ ...previous, fraction, roughness })
}

// 作用：生成光谱属性对话框中的“镜面比例/粗糙度”输入框 HTML；
//   editable=false 时输入框禁用（只读查看）。
export function specularParameterFields(value, editable = true) {
  const s = normalizeSpecular(value)
  const disabled = editable ? '' : 'disabled'
  return `<label class="dialog-field"><span>镜面比例（0–1）</span><input class="input" type="number" name="specularFraction" min="0" max="1" step="0.01" value="${s.fraction}" ${disabled}></label><label class="dialog-field"><span>粗糙度（0.05–1）</span><input class="input" type="number" name="specularRoughness" min="0.05" max="1" step="0.01" value="${s.roughness}" ${disabled}></label><div class="notice" style="grid-column:1 / -1">镜面比例和粗糙度由该光谱属性提供；是否使用，在“场景对象 → 编辑 OBJ 计算属性”中逐 Mesh 设置。仅简单光线追踪的短波计算生效。</div>`
}


