// ==========================================================================
// 文件：vehicle-lineup.mjs
// 功能：荒漠与森林两个示例共用的车辆阵列生成器：1 辆裸车（对照）、5 辆遮阳棚下的车、5 辆篷布覆盖的车；
//   每个棚/篷布都有自己的光谱与纹理。把模型文件与位置文件写入示例目录，并把用到的光谱、热状态
//   记录追加到工程 configuration（原有同名记录不重复添加）。
// 主要模块：
//   1. shadeCanopyObj：程序化生成双坡遮阳棚 OBJ（棚面 canopy_fabric + 6 根立柱 canopy_frame）
//   2. 光谱构造：texture / fileSpectrum（引用 assets/spectral-library 文件）/ customSpectrum
//   3. 数据表：SHARED_SPECTRA（车漆、饰件、玻璃、轮胎、钢架）、DESERT_CLASSES / WOODLAND_CLASSES
//      （多光谱迷彩的类别光谱）、THEMES（每个主题 5 种棚布与 5 种篷布）、MODELS（车辆 OBJ 来源）
//   4. objBounds：由 OBJ 顶点算三轴尺寸，用作对象 dimensions
//   5. addVehicleLineup：复制模型、写棚 OBJ、登记光谱/热记录、按 11 个位置生成对象条目
// 数据流：调用方（示例生成脚本，待核对）传入 theme、11 个 spots、configuration、示例/项目根目录
//   → 写 models/*.obj 与 positions/*_position.txt → 返回对象 items 数组由调用方放入工程。
// 约定：OBJ 为 Y 向上；棚的 X 方向为宽度，Z 为车长方向；位置文件一行 = 北、东、高度 0、缩放 1、旋转；
//   spots 顺序 = [裸车, 5 个棚车位, 5 个篷布车位]，棚与其下的车共用同一位置；
//   refTir / tauTir 为热红外波段反射率/透射率。
// 关系：车辆网格组名（White/Blue、Windows、Black、Grey、Headlights、TailLights、part_N__covered_car）
//   与 src/renderer/src/project-schema.js 的车辆绑定规则对应；材料名 vehicle_material、
//   tarp_fabric 等见 docs/vehicle-thermal.md。
// ==========================================================================

// Shared vehicle lineup for the desert and forest examples: one bare car, five
// cars under shade canopies and five tarp-covered cars, each canopy and tarp with
// its own spectrum and texture. Writes models/ and positions/ into exampleRoot
// and appends spectra, thermals and materials to the project configuration.
import { copyFileSync, readFileSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'

// 作用：生成双坡（人字形）遮阳棚的 OBJ 文本。参数均为米：width 宽（X）、length 长（Z）、
//   eave 檐口高、ridge 脊高、cell 棚面网格边长、post 立柱截面边长。
//   棚面高度 height(x) 由脊线（x = 0）线性降到檐口；每个网格单元拆成 2 个三角形；
//   立柱为 4 根角柱（高到 eave-0.02）加 2 根脊线端柱（高到 ridge-0.02），各为 8 顶点的方柱。
//   输出含 canopy_fabric 与 canopy_frame 两个组，供工程按组分配材料。
// Gable canopy in OBJ Y-up: X across, Z along the car, ridge along Z.
export function shadeCanopyObj({ width = 3.6, length = 6.4, eave = 2.4, ridge = 2.85, cell = 0.1, post = 0.06 } = {}) {
  const lines = ['# Stream Studio shade canopy (tools/vehicle-lineup.mjs)', 'o canopy_fabric', 'g canopy_fabric']
  const nx = Math.round(width / cell), nz = Math.round(length / cell)
  const height = (x) => ridge - (ridge - eave) * Math.abs(x) / (width / 2)
  for (let j = 0; j <= nz; j += 1) for (let i = 0; i <= nx; i += 1) {
    const x = -width / 2 + i * width / nx, z = -length / 2 + j * length / nz
    lines.push(`v ${x.toFixed(4)} ${height(x).toFixed(4)} ${z.toFixed(4)}`)
  }
  const id = (i, j) => j * (nx + 1) + i + 1
  for (let j = 0; j < nz; j += 1) for (let i = 0; i < nx; i += 1) {
    lines.push(`f ${id(i, j)} ${id(i, j + 1)} ${id(i + 1, j + 1)}`, `f ${id(i, j)} ${id(i + 1, j + 1)} ${id(i + 1, j)}`)
  }
  let base = (nx + 1) * (nz + 1)
  lines.push('o canopy_frame', 'g canopy_frame')
  const box = (cx, cz, size, y1) => {
    for (const y of [0, y1]) for (const [dx, dz] of [[-1, -1], [1, -1], [1, 1], [-1, 1]]) lines.push(`v ${(cx + dx * size / 2).toFixed(4)} ${y.toFixed(4)} ${(cz + dz * size / 2).toFixed(4)}`)
    const q = (a, b, c, d) => lines.push(`f ${base + a} ${base + b} ${base + c}`, `f ${base + a} ${base + c} ${base + d}`)
    q(1, 2, 3, 4); q(5, 8, 7, 6); q(1, 5, 6, 2); q(2, 6, 7, 3); q(3, 7, 8, 4); q(4, 8, 5, 1)
    base += 8
  }
  const px = width / 2 - post, pz = length / 2 - post
  for (const [x, z] of [[-px, -pz], [px, -pz], [px, pz], [-px, pz]]) box(x, z, post, eave - 0.02)
  box(0, -pz, post, ridge - 0.02)
  box(0, pz, post, ridge - 0.02)
  return `${lines.join('\n')}\n`
}

// CANOPY_FOOTPRINT：缺省棚的占地尺寸（m），供调用方排布车位时避让（与 shadeCanopyObj 缺省参数一致）。
export const CANOPY_FOOTPRINT = { length: 6.4, width: 3.6 }

// 光谱/纹理构造器：texture = 物理纹理（方差纹理，strength 为强度，repeatSize 为平铺尺寸，单位 m，待核对）；
//   fileSpectrum = 引用光谱库文本文件的光谱，refTir 为热红外反射率，preview 为仅用于显示的预览纹理，
//   physical 为参与辐射计算的纹理；customSpectrum = 给定单一反射率的自定义光谱。
const texture = (file, strength, repeatSize, extra = {}) => ({ enabled: true, fileName: `assets/texture-library/${file}`, strength, repeatSize, ...extra })
const noTexture = { enabled: false, fileName: '', strength: 0.2, repeatSize: 2 }
const fileSpectrum = (name, label, file, refTir, preview, physical = noTexture, extra = {}) => ({
  name, label, model: 'file', fileName: `assets/spectral-library/${file}.txt`, reflectance: '0.20', transmittance: '0.0', refTir, tauTir: 0,
  previewTexture: preview ? { enabled: true, ...preview } : { enabled: false, preset: 'canvas', repeatSize: 0.6 }, physicalTexture: physical, params: {}, ...extra
})
const customSpectrum = (name, label, reflectance, refTir, preview) => ({
  name, label, model: 'custom', reflectance: String(reflectance), transmittance: '0.0', refTir, tauTir: 0, params: {},
  previewTexture: preview ? { enabled: true, ...preview } : { enabled: false, preset: 'metal', repeatSize: 1.5 }, physicalTexture: noTexture
})

// 共享光谱：各主题通用的车漆（白/银/黑/沙/军绿）、饰件、玻璃、轮胎橡胶与棚的镀锌钢管架。
const SHARED_SPECTRA = [
  customSpectrum('car_paint_white', '车漆·白色', 0.70, 0.08),
  customSpectrum('car_paint_silver', '车漆·银灰', 0.45, 0.10, { preset: 'metal', repeatSize: 1.5 }),
  customSpectrum('car_paint_black', '车漆·黑色', 0.05, 0.06),
  fileSpectrum('car_paint_sand', '车漆·沙色（荒漠沙色光谱）', 'tarp_desert_tan', 0.08),
  fileSpectrum('car_paint_olive', '车漆·军绿（军绿帆布光谱）', 'tarp_olive_canvas', 0.08),
  customSpectrum('car_trim_grey', '车身灰色饰件', 0.20, 0.08),
  customSpectrum('car_glass', '车窗/车灯玻璃', 0.08, 0.16),
  customSpectrum('tyre_rubber', '轮胎橡胶', 0.05, 0.06),
  customSpectrum('steel_frame', '遮阳棚镀锌钢管支架', 0.30, 0.30, { preset: 'metal', repeatSize: 1 })
]

// 多光谱迷彩的"类别光谱"：迷彩分类纹理中每种颜色对应一个真实地物光谱（沙、篷布、土壤等），
//   荒漠主题用沙色系，林地主题用绿/褐/黑斑。
const DESERT_CLASSES = [
  fileSpectrum('camo_class_sand', '迷彩类别·沙丘沙', 'sand_desert_dune', 0.05),
  fileSpectrum('camo_class_tan', '迷彩类别·沙色篷布', 'tarp_desert_tan', 0.09),
  fileSpectrum('camo_class_dark_sand', '迷彩类别·暗色粗砂', 'sand_desert_dark', 0.06)
]
const WOODLAND_CLASSES = [
  fileSpectrum('camo_class_light_green', '迷彩类别·浅绿斑（仿植被伪装布）', 'tarp_ir_camouflage_green', 0.06),
  fileSpectrum('camo_class_dark_green', '迷彩类别·深绿斑（军绿帆布）', 'tarp_olive_canvas', 0.10),
  fileSpectrum('camo_class_brown', '迷彩类别·褐斑（仿干燥土壤）', 'soil_dry_loam', 0.07),
  fileSpectrum('camo_class_black', '迷彩类别·黑斑（炭黑涂层）', 'shade_net_black_fiber', 0.06)
]

// THEMES：按主题（desert / forest）给出 classSpectra、5 个遮阳棚（car 车型、paint 车漆光谱、棚布光谱）
//   和 5 个篷布车光谱。第 2 个篷布车使用分类纹理的多光谱迷彩（classTexture.classes 的 color → spectralName）。
//   openness / opennessTexture 为遮阳网的开孔率（0–1，待核对含义）。
// Five canopy fabrics and five tarps per theme, each a distinct spectrum + texture.
const THEMES = {
  desert: {
    classSpectra: DESERT_CLASSES,
    canopies: [
      { car: 'suv', paint: 'car_paint_sand', spectrum: fileSpectrum('canopy_desert_canvas', '遮阳棚 1：荒漠沙色帆布 + 帆布织纹', 'tarp_desert_tan', 0.09, { preset: 'canvas', repeatSize: 0.6 }, texture('canvas_weave_variance.jpg', 0.15, 0.5)) },
      { car: 'sedan', paint: 'car_paint_white', spectrum: fileSpectrum('canopy_white_stripes', '遮阳棚 2：白色 PE 编织布 + 遮阳条纹', 'tarp_white_pe', 0.08, { preset: 'stripes', repeatSize: 1.2 }, texture('stripes_variance.jpg', 0.4, 1.2), { tauTir: 0.10 }) },
      { car: 'suv', paint: 'car_paint_silver', spectrum: fileSpectrum('canopy_silver_film', '遮阳棚 3：镀铝反光膜 + 褶皱', 'tarp_silver_reflective', 0.72, { preset: 'metal', repeatSize: 1 }, texture('crumpled_film_variance.jpg', 0.2, 2.0)) },
      { car: 'sedan', paint: 'car_paint_black', spectrum: fileSpectrum('canopy_black_net', '遮阳棚 4：黑色遮阳网（开孔 50%）', 'shade_net_black_fiber', 0.06, { preset: 'shade-net', repeatSize: 0.3 }, noTexture, { openness: 0.5 }) },
      { car: 'suv', paint: 'car_paint_sand', spectrum: fileSpectrum('canopy_net_damaged', '遮阳棚 5：绿色遮阳网 + 破损（开孔率纹理）', 'shade_net_green_fiber', 0.07, { preset: 'shade-net', repeatSize: 0.3 }, noTexture, { opennessTexture: { enabled: true, fileName: 'assets/texture-library/openness_damaged_net.png', repeatSize: 3, minOpenness: 0.2, maxOpenness: 0.9, levels: 8 } }) }
    ],
    tarps: [
      fileSpectrum('cover_desert_camo', '篷布车 1：荒漠沙色 + 荒漠迷彩', 'tarp_desert_tan', 0.09, { preset: 'camouflage-desert', repeatSize: 1.5 }, texture('camouflage_desert_variance.jpg', 0.35, 1.5)),
      fileSpectrum('cover_desert_multispectral', '篷布车 2：多光谱荒漠迷彩（分类纹理，仿沙丘沙）', 'tarp_desert_tan', 0.09, { preset: 'camouflage-desert', repeatSize: 1.5 }, noTexture, { classTexture: { enabled: true, fileName: 'assets/texture-library/class_desert_3.png', repeatSize: 1.5, classes: [
        { color: '#c8b183', spectralName: 'camo_class_sand' }, { color: '#a88d5f', spectralName: 'camo_class_tan' }, { color: '#83704c', spectralName: 'camo_class_dark_sand' }] } }),
      fileSpectrum('cover_olive_canvas', '篷布车 3：军绿帆布 + 帆布织纹', 'tarp_olive_canvas', 0.10, { preset: 'canvas', repeatSize: 0.6 }, texture('canvas_weave_variance.jpg', 0.15, 0.5)),
      fileSpectrum('cover_ir_digital', '篷布车 4：仿植被红外伪装布 + 数码迷彩', 'tarp_ir_camouflage_green', 0.06, { preset: 'camouflage-digital', repeatSize: 1.6 }, texture('camouflage_digital_variance.jpg', 0.35, 1.6)),
      fileSpectrum('cover_silver_film', '篷布车 5：镀铝反光膜 + 褶皱', 'tarp_silver_reflective', 0.72, { preset: 'metal', repeatSize: 1 }, texture('crumpled_film_variance.jpg', 0.2, 2.0))
    ]
  },
  forest: {
    classSpectra: WOODLAND_CLASSES,
    canopies: [
      { car: 'suv', paint: 'car_paint_olive', spectrum: fileSpectrum('canopy_olive_canvas', '遮阳棚 1：军绿帆布 + 帆布织纹', 'tarp_olive_canvas', 0.10, { preset: 'canvas', repeatSize: 0.6 }, texture('canvas_weave_variance.jpg', 0.15, 0.5)) },
      { car: 'sedan', paint: 'car_paint_white', spectrum: fileSpectrum('canopy_green_net', '遮阳棚 2：绿色 HDPE 遮阳网（开孔 35%）', 'shade_net_green_fiber', 0.07, { preset: 'shade-net', repeatSize: 0.3 }, noTexture, { openness: 0.35 }) },
      { car: 'suv', paint: 'car_paint_silver', spectrum: fileSpectrum('canopy_net_damaged', '遮阳棚 3：绿色遮阳网 + 破损（开孔率纹理）', 'shade_net_green_fiber', 0.07, { preset: 'shade-net', repeatSize: 0.3 }, noTexture, { opennessTexture: { enabled: true, fileName: 'assets/texture-library/openness_damaged_net.png', repeatSize: 3, minOpenness: 0.2, maxOpenness: 0.9, levels: 8 } }) },
      { car: 'sedan', paint: 'car_paint_black', spectrum: fileSpectrum('canopy_white_stripes', '遮阳棚 4：白色 PE 编织布 + 遮阳条纹', 'tarp_white_pe', 0.08, { preset: 'stripes', repeatSize: 1.2 }, texture('stripes_variance.jpg', 0.4, 1.2), { tauTir: 0.10 }) },
      { car: 'suv', paint: 'car_paint_olive', spectrum: fileSpectrum('canopy_woodland_camo', '遮阳棚 5：仿植被红外伪装布 + 林地迷彩', 'tarp_ir_camouflage_green', 0.06, { preset: 'camouflage-woodland', repeatSize: 1.5 }, texture('camouflage_woodland_variance.jpg', 0.35, 1.5)) }
    ],
    tarps: [
      fileSpectrum('cover_woodland_camo', '篷布车 1：军绿帆布 + 林地迷彩', 'tarp_olive_canvas', 0.10, { preset: 'camouflage-woodland', repeatSize: 1.5 }, texture('camouflage_woodland_variance.jpg', 0.35, 1.5)),
      fileSpectrum('cover_multispectral_woodland', '篷布车 2：多光谱林地迷彩（分类纹理）', 'tarp_olive_canvas', 0.10, { preset: 'camouflage-woodland', repeatSize: 1.5 }, noTexture, { classTexture: { enabled: true, fileName: 'assets/texture-library/class_woodland_4.png', repeatSize: 1.5, classes: [
        { color: '#566334', spectralName: 'camo_class_light_green' }, { color: '#34401f', spectralName: 'camo_class_dark_green' },
        { color: '#6e5232', spectralName: 'camo_class_brown' }, { color: '#1e1f19', spectralName: 'camo_class_black' }] } }),
      fileSpectrum('cover_ir_digital', '篷布车 3：仿植被红外伪装布 + 数码迷彩', 'tarp_ir_camouflage_green', 0.06, { preset: 'camouflage-digital', repeatSize: 1.6 }, texture('camouflage_digital_variance.jpg', 0.35, 1.6)),
      fileSpectrum('cover_olive_canvas', '篷布车 4：军绿帆布 + 帆布织纹', 'tarp_olive_canvas', 0.10, { preset: 'canvas', repeatSize: 0.6 }, texture('canvas_weave_variance.jpg', 0.15, 0.5)),
      fileSpectrum('cover_white_pe', '篷布车 5：白色 PE 编织布 + 帆布织纹', 'tarp_white_pe', 0.08, { preset: 'canvas', repeatSize: 0.6 }, texture('canvas_weave_variance.jpg', 0.2, 0.5))
    ]
  }
}

// MODELS：车辆 OBJ 来源及用于车漆的组名（SUV 为 White，轿车为 Blue）；covered 为带篷布的整车模型。
const MODELS = {
  suv: { source: 'assets/obj-library/vehicle/typical/modern_suv.obj', paintGroup: 'White', label: 'SUV' },
  sedan: { source: 'assets/obj-library/vehicle/typical/modern_sedan.obj', paintGroup: 'Blue', label: '轿车' },
  covered: { source: 'assets/obj-library/vehicle/realistic/covered_car_realistic.obj' }
}

// 作用：读取 OBJ 顶点，返回 [X, Y, Z] 三轴范围（max - min，3 位小数），作为对象 dimensions。
function objBounds(path) {
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity]
  for (const line of readFileSync(path, 'utf8').split('\n')) {
    if (!line.startsWith('v ')) continue
    const v = line.split(/\s+/).slice(1, 4).map(Number)
    for (let k = 0; k < 3; k += 1) { min[k] = Math.min(min[k], v[k]); max[k] = Math.max(max[k], v[k]) }
  }
  return max.map((value, k) => Number((value - min[k]).toFixed(3)))
}

// 模块：addVehicleLineup —— 按 11 个车位生成 16 个对象条目（1 裸车 + 5×(车 + 棚) + 5 篷布车）。
//   副作用：向 exampleRoot/models 复制车辆 OBJ 并写 shade_canopy.obj，向 exampleRoot/positions
//   写每个对象的位置文件（调用前这些目录须已存在）；就地向 configuration.spectra / thermals 追加记录。
//   energyBalance 为真（FacetEB）时棚布/篷布使用 tarp_fabric 材料（膜能量平衡），
//   其他模式只作辐射绑定，用 vehicle_material。返回对象 items。
// spots: 11 placements { north, east, rotation } — [bare, 5 canopy cars, 5 tarp cars].
export function addVehicleLineup({ theme, spots, configuration, exampleRoot, projectRoot, sceneSize, energyBalance = false }) {
  const set = THEMES[theme]
  if (!set) throw new Error(`Unknown vehicle theme: ${theme}`)
  if (spots.length !== 11) throw new Error(`Vehicle lineup needs 11 spots, got ${spots.length}`)
  const model = (key) => {
    const target = `models/${MODELS[key].source.split('/').pop()}`
    copyFileSync(join(projectRoot, MODELS[key].source), join(exampleRoot, target))
    return target
  }
  const files = { suv: model('suv'), sedan: model('sedan'), covered: model('covered'), canopy: 'models/shade_canopy.obj' }
  writeFileSync(join(exampleRoot, files.canopy), shadeCanopyObj(), 'utf8')

  // 步骤：登记光谱与热记录——按名称去重；新增 canopy_temperature（日照/阴影初始温度 315/303，
  //   单位按工程热记录约定，应为 K，待核对）。
  const existing = new Set(configuration.spectra.map((item) => item.name))
  for (const spectrum of [...SHARED_SPECTRA, ...set.classSpectra, ...set.canopies.map((entry) => entry.spectrum), ...set.tarps]) {
    if (!existing.has(spectrum.name)) { configuration.spectra.push(spectrum); existing.add(spectrum.name) }
  }
  if (!configuration.thermals.some((item) => item.name === 'canopy_temperature')) {
    configuration.thermals.push({ name: 'canopy_temperature', label: '遮阳棚初始温度', sunlitTemperature: 315, shadedTemperature: 303 })
  }
  // Membrane EB exists only in FacetEB; the other modes use these as radiative bindings.
  const body = 'vehicle_material'
  const fabric = energyBalance ? 'tarp_fabric' : 'vehicle_material'

  // mesh：构造一个网格组设置（光谱、热记录、材料；冠层固定为刚体 rigid_body）。
  // carMeshes：车辆各网格组的绑定——车漆组 + 车窗 + 轮胎(Black) + 灰饰件(Grey) + 前后灯。
  const mesh = (name, spectralName, thermalName = 'vehicle_temperature', materialName = body) => ({ name, spectralName, thermalName, materialName, canopyName: 'rigid_body' })
  const carMeshes = (car, paint) => [
    mesh(MODELS[car].paintGroup, paint), mesh('Windows', 'car_glass'), mesh('Black', 'tyre_rubber'),
    mesh('Grey', 'car_trim_grey'), mesh('Headlights', 'car_glass'), mesh('TailLights', 'car_glass')
  ]
  const items = []
  // add：写位置文件一行（北、东、高度 0、缩放 1、旋转）并追加对象条目：手动分布、单实例，
  //   dimensions 取自 OBJ 范围；对象默认的光谱/热/材料取第一个网格组的设置。
  const add = (name, label, type, fileName, spot, meshes) => {
    const row = [spot.north.toFixed(2), spot.east.toFixed(2), 0, 1, spot.rotation].join(' ')
    writeFileSync(join(exampleRoot, 'positions', `${name}_position.txt`), `${row}\n`, 'utf8')
    items.push({
      name, label, type, fileName, positionFile: `positions/${name}_position.txt`, shape: 'cube', dimensions: objBounds(join(exampleRoot, fileName)),
      spectralName: meshes[0].spectralName, thermalName: meshes[0].thermalName, materialName: meshes[0].materialName, canopyName: 'rigid_body', meshes,
      distribution: { mode: 'manual', basis: 'count', count: 1, minX: 0, maxX: sceneSize, minY: 0, maxY: sceneSize, z: 0, scaleMin: 1, scaleMax: 1, seed: 1, offsetX: 0, offsetY: 0, offsetZ: 0, points: row },
      instanceCount: 1
    })
  }
  // 步骤：先加裸车对照，再按 5 个棚各加"车 + 棚"（同位置），最后加 5 辆篷布车（spots[6..10]）。
  add('bare_suv', '空车：白色 SUV（对照）', 'Vehicle', files.suv, spots[0], carMeshes('suv', 'car_paint_white'))
  set.canopies.forEach((entry, index) => {
    const n = index + 1
    add(`canopy_car_${n}`, `遮阳棚车 ${n}：${MODELS[entry.car].label}`, 'Vehicle', files[entry.car], spots[1 + index], carMeshes(entry.car, entry.paint))
    add(`shade_canopy_${n}`, entry.spectrum.label, 'Building', files.canopy, spots[1 + index], [
      mesh('canopy_fabric', entry.spectrum.name, 'canopy_temperature', fabric), mesh('canopy_frame', 'steel_frame', 'building_temperature', body)
    ])
  })
  set.tarps.forEach((spectrum, index) => {
    const n = index + 1
    add(`tarp_car_${n}`, spectrum.label, 'Vehicle', files.covered, spots[6 + index], [
      mesh('part_1__covered_car', spectrum.name, 'tarp_temperature', fabric), mesh('part_2__covered_car', 'tyre_rubber')
    ])
  })
  return items
}
