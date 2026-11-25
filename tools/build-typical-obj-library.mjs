// ==========================================================================
// 文件：build-typical-obj-library.mjs
// 功能：生成/整理 assets/obj-library 中“典型”车辆、船舶、树木 OBJ（Y 向上、单位 m、带顶点颜色）。
// 主要模块：
//   1. 常量与小工具：COLORS 调色板、fmt、normal（三角形法线）
//   2. ObjBuilder：累积三角形（box / 圆柱等基本体）并写出 .obj 与同名 .json 元数据
//   3. importColoredObj：导入 Quaternius 的 OBJ+MTL（CC0），把材质 Kd 转为顶点色并缩放到目标尺寸
//   4. 程序化模型：buildCityBus、addShipHull + buildContainerShip / buildBulkCarrier
//   5. rescaleColoredObj：把已有 OBJ 按目标高度等比缩放并更新 json 元数据
//   6. 脚本主体：导入 2 辆车和 1 艘邮轮，生成 3 个程序化模型，缩放街道树
// 输入：.tmp-asset-import/{cars,ships}/OBJ 下的原始素材（仓库外获取，不随代码提供）。
// 输出：assets/obj-library/<类别>/typical/*.obj 与 *.json；导入素材的原件另存到 _<名称>_source/。
// 约定：Y 向上、长度单位 m；车/船长度沿 Z、宽沿 X；模型以 XZ 中心、最低点 y=0 为原点；
//   .json 记录三角形数、包围盒尺寸 dimensions、coordinateSystem、unit。
// 注意：本脚本会直接覆盖库中同名文件；rescaleColoredObj 要求目标 OBJ 与 json 已存在。
// ==========================================================================

/**
 * Build the recommended, metre-scale OBJ set used by Stream Studio.
 *
 * Imported Quaternius models are CC0 and are converted to Y-up OBJ files with
 * embedded vertex colours. Large modern ships and the city bus are generated
 * at realistic dimensions so they remain lightweight enough for voxelization.
 */

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const library = path.join(root, 'assets', 'obj-library')
const importRoot = path.join(root, '.tmp-asset-import')

// 调色板：RGB，范围 0-1（线性/sRGB 未标明，待核对），写入 OBJ 顶点颜色。
const COLORS = {
  black: [0.025, 0.03, 0.035],
  blue: [0.08, 0.28, 0.55],
  glass: [0.12, 0.24, 0.31],
  grey: [0.35, 0.38, 0.4],
  lightGrey: [0.68, 0.71, 0.72],
  white: [0.84, 0.86, 0.86],
  red: [0.55, 0.055, 0.035],
  orange: [0.82, 0.28, 0.035],
  yellow: [0.93, 0.67, 0.05],
  containerBlue: [0.06, 0.22, 0.43],
  containerGreen: [0.07, 0.32, 0.23],
  deck: [0.38, 0.28, 0.19],
  hullRed: [0.36, 0.055, 0.035],
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true })
}

function fmt(value) {
  return Number(value).toFixed(6)
}

// 作用：返回三角形 abc 的单位法线 (b-a)×(c-a)；退化三角形时长度按 1 处理以避免除零。
function normal(a, b, c) {
  const ux = b[0] - a[0]
  const uy = b[1] - a[1]
  const uz = b[2] - a[2]
  const vx = c[0] - a[0]
  const vy = c[1] - a[1]
  const vz = c[2] - a[2]
  const n = [uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx]
  const length = Math.hypot(...n) || 1
  return n.map((value) => value / length)
}

// 模块：三角形累积器。triangle/quad 添加面（quad 拆成 abc、acd 两个三角形），
// box、cylinderX（轴沿 X）、cylinderY（轴沿 Y）生成基本体；write 输出 OBJ。
// group 名用于 OBJ 的 g 分组，供应用内按组绑定材质。
class ObjBuilder {
  constructor(label, source = 'Stream Studio procedural geometry') {
    this.label = label
    this.source = source
    this.triangles = []
  }

  triangle(a, b, c, color, group = 'part') {
    this.triangles.push({ points: [a, b, c], color, group })
  }

  quad(a, b, c, d, color, group = 'part') {
    this.triangle(a, b, c, color, group)
    this.triangle(a, c, d, color, group)
  }

  // 长方体：min/max 为对角点；6 个面的顶点顺序按外法线向外排列。
  box(min, max, color, group = 'box') {
    const [x0, y0, z0] = min
    const [x1, y1, z1] = max
    const p = [
      [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0],
      [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1],
    ]
    this.quad(p[0], p[3], p[2], p[1], color, group)
    this.quad(p[4], p[5], p[6], p[7], color, group)
    this.quad(p[0], p[4], p[7], p[3], color, group)
    this.quad(p[1], p[2], p[6], p[5], color, group)
    this.quad(p[0], p[1], p[5], p[4], color, group)
    this.quad(p[3], p[7], p[6], p[2], color, group)
  }

  cylinderX(center, radius, length, sides, color, group = 'cylinder') {
    const [cx, cy, cz] = center
    const x0 = cx - length / 2
    const x1 = cx + length / 2
    const left = []
    const right = []
    for (let i = 0; i < sides; i += 1) {
      const angle = (i / sides) * Math.PI * 2
      const y = cy + Math.cos(angle) * radius
      const z = cz + Math.sin(angle) * radius
      left.push([x0, y, z])
      right.push([x1, y, z])
    }
    for (let i = 0; i < sides; i += 1) {
      const next = (i + 1) % sides
      this.quad(left[i], right[i], right[next], left[next], color, group)
      this.triangle([x0, cy, cz], left[next], left[i], color, group)
      this.triangle([x1, cy, cz], right[i], right[next], color, group)
    }
  }

  cylinderY(center, radius, height, sides, color, group = 'cylinder') {
    const [cx, cy, cz] = center
    const y0 = cy - height / 2
    const y1 = cy + height / 2
    const lower = []
    const upper = []
    for (let i = 0; i < sides; i += 1) {
      const angle = (i / sides) * Math.PI * 2
      const x = cx + Math.cos(angle) * radius
      const z = cz + Math.sin(angle) * radius
      lower.push([x, y0, z])
      upper.push([x, y1, z])
    }
    for (let i = 0; i < sides; i += 1) {
      const next = (i + 1) % sides
      this.quad(lower[i], lower[next], upper[next], upper[i], color, group)
      this.triangle([cx, y0, cz], lower[i], lower[next], color, group)
      this.triangle([cx, y1, cz], upper[next], upper[i], color, group)
    }
  }

  // 作用：把累积的三角形写成 OBJ（每个三角形独立 3 顶点 + 面法线，不共享顶点；
  // 顶点行格式 v x y z r g b）和同名 .json（尺寸取包围盒，extra 合并进元数据）。
  write(relativePath, extra = {}) {
    const target = path.join(library, relativePath)
    ensureDir(path.dirname(target))
    const lines = [
      `# ${this.label}`,
      `# Source: ${this.source}`,
      '# Coordinates: Y-up; units: metres; vertex RGB embedded after XYZ',
    ]
    let vertex = 1
    let lastGroup = ''
    const bounds = [[Infinity, Infinity, Infinity], [-Infinity, -Infinity, -Infinity]]
    for (const triangle of this.triangles) {
      if (triangle.group !== lastGroup) {
        lines.push(`g ${triangle.group}`)
        lastGroup = triangle.group
      }
      const n = normal(...triangle.points)
      for (const point of triangle.points) {
        point.forEach((value, axis) => {
          bounds[0][axis] = Math.min(bounds[0][axis], value)
          bounds[1][axis] = Math.max(bounds[1][axis], value)
        })
        lines.push(`v ${point.map(fmt).join(' ')} ${triangle.color.map(fmt).join(' ')}`)
        lines.push(`vn ${n.map(fmt).join(' ')}`)
      }
      lines.push(`f ${vertex}//${vertex} ${vertex + 1}//${vertex + 1} ${vertex + 2}//${vertex + 2}`)
      vertex += 3
    }
    fs.writeFileSync(target, `${lines.join('\n')}\n`)
    const dimensions = bounds[1].map((value, axis) => Number((value - bounds[0][axis]).toFixed(4)))
    fs.writeFileSync(target.replace(/\.obj$/i, '.json'), `${JSON.stringify({
      source: this.source,
      triangles: this.triangles.length,
      dimensions,
      coordinateSystem: 'Y-up',
      unit: 'metre',
      vertexColors: true,
      ...extra,
    }, null, 2)}\n`)
    console.log(`created ${relativePath}: ${this.triangles.length} triangles, ${dimensions.join(' x ')} m`)
  }
}

// 作用：读取 MTL，返回 材质名 → Kd 漫反射 RGB 的 Map（材质名中的空白换成下划线）。
function parseMtl(file) {
  const materials = new Map()
  let current = 'default'
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const fields = line.trim().split(/\s+/)
    if (fields[0] === 'newmtl') current = fields.slice(1).join('_')
    if (fields[0] === 'Kd') materials.set(current, fields.slice(1, 4).map(Number))
  }
  return materials
}

// 作用：导入带 MTL 的 OBJ，转为“逐面顶点色”的库内 OBJ。
// 输入：源 OBJ/MTL、库内相对路径、目标包围盒尺寸 [X, Y, Z]（m）、标签、来源 URL。
// 步骤：1 备份源文件、许可证与来源说明到 _<名称>_source；2 解析顶点与面（多边形扇形三角化，
// 支持负索引）；3 按三轴独立比例缩放到目标尺寸（可能改变纵横比），XZ 居中、底面落在 y=0；
// 4 每个面取其材质 Kd 为顶点色（缺材质用灰色），材质名作为 group 写出。
// 说明：源文件不存在时回退使用已备份的副本，使脚本可在素材目录清理后重跑。
function importColoredObj({ sourceObj, sourceMtl, relativePath, targetDimensions, label, sourceUrl }) {
  const sourceDir = path.join(library, path.dirname(relativePath), `_${path.basename(relativePath, '.obj')}_source`)
  ensureDir(sourceDir)
  const archivedObj = path.join(sourceDir, path.basename(sourceObj))
  const archivedMtl = path.join(sourceDir, path.basename(sourceMtl))
  if (fs.existsSync(sourceObj)) fs.copyFileSync(sourceObj, archivedObj)
  if (fs.existsSync(sourceMtl)) fs.copyFileSync(sourceMtl, archivedMtl)
  const licenseCandidate = path.join(path.dirname(path.dirname(sourceObj)), 'License.txt')
  if (fs.existsSync(licenseCandidate)) fs.copyFileSync(licenseCandidate, path.join(sourceDir, 'LICENSE.txt'))
  fs.writeFileSync(path.join(sourceDir, 'SOURCE.txt'), `${sourceUrl}\nLicense: CC0 1.0 Universal\n`)

  const objFile = fs.existsSync(sourceObj) ? sourceObj : archivedObj
  const mtlFile = fs.existsSync(sourceMtl) ? sourceMtl : archivedMtl
  const materials = parseMtl(mtlFile)
  const vertices = []
  const faces = []
  let material = 'default'
  for (const line of fs.readFileSync(objFile, 'utf8').split(/\r?\n/)) {
    const fields = line.trim().split(/\s+/)
    if (fields[0] === 'v') vertices.push(fields.slice(1, 4).map(Number))
    if (fields[0] === 'usemtl') material = fields.slice(1).join('_')
    if (fields[0] === 'f') {
      const indices = fields.slice(1).map((field) => {
        const raw = Number(field.split('/')[0])
        return raw < 0 ? vertices.length + raw : raw - 1
      })
      for (let i = 1; i < indices.length - 1; i += 1) faces.push({ indices: [indices[0], indices[i], indices[i + 1]], material })
    }
  }
  const sourceMin = [0, 1, 2].map((axis) => Math.min(...vertices.map((item) => item[axis])))
  const sourceMax = [0, 1, 2].map((axis) => Math.max(...vertices.map((item) => item[axis])))
  const scale = targetDimensions.map((value, axis) => value / (sourceMax[axis] - sourceMin[axis]))
  const transformed = vertices.map((point) => [
    (point[0] - (sourceMin[0] + sourceMax[0]) / 2) * scale[0],
    (point[1] - sourceMin[1]) * scale[1],
    (point[2] - (sourceMin[2] + sourceMax[2]) / 2) * scale[2],
  ])
  const builder = new ObjBuilder(label, sourceUrl)
  for (const face of faces) {
    const color = materials.get(face.material) || COLORS.grey
    builder.triangle(...face.indices.map((index) => transformed[index]), color, face.material.replace(/[^A-Za-z0-9_-]/g, '_'))
  }
  builder.write(relativePath, { derivedFrom: path.basename(objFile) })
}

// 作用：程序化生成城市低地板公交（约 2.55 m 宽、11.9 m 长、3.18 m 高，尺寸按代码坐标估算）。
// 组成：车身、车顶、窗带、前后风挡、8 对侧窗、4 个车轮、前灯。
function buildCityBus() {
  const model = new ObjBuilder('Modern low-floor city bus', 'Stream Studio procedural reference geometry')
  model.box([-1.275, 0.52, -5.95], [1.275, 2.72, 5.95], COLORS.blue, 'body')
  model.box([-1.20, 2.72, -5.70], [1.20, 3.18, 5.65], COLORS.white, 'roof')
  model.box([-1.281, 1.72, -5.62], [1.281, 2.62, 5.42], COLORS.glass, 'windows')
  model.box([-1.286, 0.84, -5.96], [1.286, 1.62, -5.90], COLORS.glass, 'windshield')
  model.box([-1.286, 0.82, 5.90], [1.286, 1.55, 5.96], COLORS.glass, 'rear_window')
  for (const z of [-3.75, 3.75]) {
    model.cylinderX([-1.30, 0.52, z], 0.48, 0.24, 24, COLORS.black, 'wheels')
    model.cylinderX([1.30, 0.52, z], 0.48, 0.24, 24, COLORS.black, 'wheels')
  }
  for (let i = 0; i < 8; i += 1) {
    const z = -4.65 + i * 1.32
    model.box([-1.292, 1.78, z], [-1.284, 2.52, z + 0.98], COLORS.glass, 'side_windows')
    model.box([1.284, 1.78, z], [1.292, 2.52, z + 0.98], COLORS.glass, 'side_windows')
  }
  model.box([-1.285, 0.96, -5.98], [-0.70, 1.26, -5.96], COLORS.yellow, 'headlights')
  model.box([0.70, 0.96, -5.98], [1.285, 1.26, -5.96], COLORS.yellow, 'headlights')
  model.write('vehicle/typical/modern_city_bus.obj')
}

// 作用：生成船体壳：沿船长 Z 分 stations 个横剖面，每个剖面是 9 点的折线环
// （甲板两侧、舷侧、舭部、龙骨），相邻剖面连成四边形，首尾剖面封盖。
// 说明：宽度沿船长按 sin(π·t)^0.32 收窄，下限 0.035 以避免闭合成退化面；
// sternBias 使 +Z 端（船尾）略宽。深度（甲板 -2.2 m、-6.0 m）为固定经验值，
// 不随船型缩放。
function addShipHull(model, { length, beam, deckY, keelY, color, group = 'hull', stations = 72 }) {
  const rings = []
  for (let i = 0; i <= stations; i += 1) {
    const t = i / stations
    const z = (t - 0.5) * length
    const taper = Math.max(0.035, Math.sin(Math.PI * t) ** 0.32)
    const sternBias = 0.86 + 0.14 * t
    const half = beam * 0.5 * taper * sternBias
    rings.push([
      [-half * 0.90, deckY, z], [half * 0.90, deckY, z],
      [half, deckY - 2.2, z], [half * 0.78, deckY - 6.0, z],
      [half * 0.22, keelY + 1.0, z], [0, keelY, z],
      [-half * 0.22, keelY + 1.0, z], [-half * 0.78, deckY - 6.0, z],
      [-half, deckY - 2.2, z],
    ])
  }
  for (let station = 0; station < stations; station += 1) {
    for (let ring = 0; ring < rings[station].length; ring += 1) {
      const next = (ring + 1) % rings[station].length
      model.quad(rings[station][ring], rings[station + 1][ring], rings[station + 1][next], rings[station][next], color, group)
    }
  }
  const cap = (ring, reverse) => {
    const center = [0, (deckY + keelY) / 2, ring[0][2]]
    for (let i = 0; i < ring.length; i += 1) {
      const next = (i + 1) % ring.length
      if (reverse) model.triangle(center, ring[i], ring[next], color, group)
      else model.triangle(center, ring[next], ring[i], color, group)
    }
  }
  cap(rings[0], true)
  cap(rings.at(-1), false)
}

// 作用：程序化生成 Panamax 集装箱船（长 220 m、宽 32.2 m）。
// 组成：船体、主甲板；20 个箱位 × 10 列 × 4 层的集装箱（配色循环），
// z 在 58~88 的箱位留给上层建筑；上层建筑、驾驶台、烟囱、桅杆在船尾（+Z）。
function buildContainerShip() {
  const model = new ObjBuilder('Modern Panamax container ship', 'Stream Studio procedural reference geometry')
  addShipHull(model, { length: 220, beam: 32.2, deckY: 11, keelY: 0, color: COLORS.hullRed })
  model.box([-14.3, 10.8, -99], [14.3, 12.0, 95], COLORS.deck, 'main_deck')
  const palette = [COLORS.containerBlue, COLORS.containerGreen, COLORS.red, COLORS.orange, COLORS.lightGrey]
  const rows = 10
  const bays = 20
  const tiers = 4
  for (let bay = 0; bay < bays; bay += 1) {
    const z = -82 + bay * 8.4
    if (z > 58 && z < 88) continue
    for (let row = 0; row < rows; row += 1) {
      const x = -12.0 + row * 2.66
      for (let tier = 0; tier < tiers; tier += 1) {
        const y = 12.05 + tier * 2.62
        model.box([x - 1.22, y, z - 3.0], [x + 1.22, y + 2.44, z + 3.0], palette[(bay + row + tier) % palette.length], 'containers')
      }
    }
  }
  model.box([-12.5, 12.0, 66], [12.5, 28.5, 91], COLORS.white, 'superstructure')
  model.box([-12.6, 24.5, 64.5], [12.6, 27.8, 69], COLORS.glass, 'bridge')
  model.box([-4.0, 28.5, 75], [4.0, 36.0, 84], COLORS.lightGrey, 'funnel')
  model.cylinderY([0, 39.5, 65], 0.38, 25, 18, COLORS.grey, 'mast')
  model.write('ship/typical/modern_container_ship.obj')
}

// 作用：程序化生成散货船（长 190 m、宽 30 m）：6 个货舱口盖，前 5 个旁配吊机立柱与吊臂，
// 上层建筑、驾驶台、烟囱在船尾（+Z）。
function buildBulkCarrier() {
  const model = new ObjBuilder('Modern bulk carrier', 'Stream Studio procedural reference geometry')
  addShipHull(model, { length: 190, beam: 30, deckY: 10, keelY: 0, color: COLORS.black })
  model.box([-13.4, 9.8, -85], [13.4, 11.2, 85], COLORS.hullRed, 'weather_deck')
  for (let hatch = 0; hatch < 6; hatch += 1) {
    const z = -58 + hatch * 21
    model.box([-10.8, 11.2, z - 7.5], [10.8, 12.3, z + 7.5], COLORS.grey, 'cargo_hatches')
    if (hatch < 5) {
      model.cylinderY([0, 20, z + 10.5], 0.45, 17, 18, COLORS.yellow, 'deck_cranes')
      model.box([-0.8, 27.8, z + 9.8], [10.5, 29.0, z + 11.2], COLORS.yellow, 'crane_booms')
    }
  }
  model.box([-12.4, 11, 66], [12.4, 27, 87], COLORS.white, 'superstructure')
  model.box([-12.5, 23, 64.5], [12.5, 26.4, 69], COLORS.glass, 'bridge')
  model.box([-4, 27, 72], [4, 34, 82], COLORS.orange, 'funnel')
  model.write('ship/typical/modern_bulk_carrier.obj')
}

// 作用：原地重写库中 OBJ 的顶点坐标：以 XZ 中心、最低点为原点，按高度方向统一比例
// 缩放到 targetHeight（m）；顶点颜色字段保持不变；同时更新 json 中的 dimensions 与 normalizedHeight。
// 注意：直接覆盖原文件，不生成备份。
function rescaleColoredObj(relativePath, targetHeight) {
  const file = path.join(library, relativePath)
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  const points = lines.filter((line) => line.startsWith('v ')).map((line) => line.trim().split(/\s+/).slice(1, 4).map(Number))
  const min = [Infinity, Infinity, Infinity]
  const max = [-Infinity, -Infinity, -Infinity]
  for (const point of points) {
    for (let axis = 0; axis < 3; axis += 1) {
      min[axis] = Math.min(min[axis], point[axis])
      max[axis] = Math.max(max[axis], point[axis])
    }
  }
  const scale = targetHeight / (max[1] - min[1])
  const center = [(min[0] + max[0]) / 2, min[1], (min[2] + max[2]) / 2]
  const output = lines.map((line) => {
    if (!line.startsWith('v ')) return line
    const fields = line.trim().split(/\s+/)
    const xyz = fields.slice(1, 4).map(Number).map((value, axis) => (value - center[axis]) * scale)
    return `v ${xyz.map(fmt).join(' ')} ${fields.slice(4).join(' ')}`.trimEnd()
  })
  fs.writeFileSync(file, output.join('\n'))
  const metadataFile = file.replace(/\.obj$/i, '.json')
  const metadata = JSON.parse(fs.readFileSync(metadataFile, 'utf8'))
  metadata.dimensions = max.map((value, axis) => Number(((value - min[axis]) * scale).toFixed(4)))
  metadata.normalizedHeight = targetHeight
  fs.writeFileSync(metadataFile, `${JSON.stringify(metadata, null, 2)}\n`)
  console.log(`rescaled ${relativePath} to ${targetHeight} m height`)
}

// 脚本主体：先导入 2 款轿车/SUV 与邮轮（目标尺寸为 [宽, 高, 长] m），
// 再生成程序化的公交、集装箱船、散货船，最后把街道树归一化到 10 m 高。
const carObjRoot = path.join(importRoot, 'cars', 'OBJ')
const shipObjRoot = path.join(importRoot, 'ships', 'OBJ')

importColoredObj({
  sourceObj: path.join(carObjRoot, 'NormalCar1.obj'),
  sourceMtl: path.join(carObjRoot, 'NormalCar1.mtl'),
  relativePath: 'vehicle/typical/modern_sedan.obj',
  targetDimensions: [1.82, 1.46, 4.58],
  label: 'Modern production sedan',
  sourceUrl: 'https://quaternius.com/packs/cars.html',
})
importColoredObj({
  sourceObj: path.join(carObjRoot, 'SUV.obj'),
  sourceMtl: path.join(carObjRoot, 'SUV.mtl'),
  relativePath: 'vehicle/typical/modern_suv.obj',
  targetDimensions: [1.98, 1.76, 4.72],
  label: 'Modern production SUV',
  sourceUrl: 'https://quaternius.com/packs/cars.html',
})
importColoredObj({
  sourceObj: path.join(shipObjRoot, 'CruiseShip.obj'),
  sourceMtl: path.join(shipObjRoot, 'CruiseShip.mtl'),
  relativePath: 'ship/typical/modern_cruise_liner.obj',
  targetDimensions: [42, 65, 290],
  label: 'Modern cruise liner',
  sourceUrl: 'https://quaternius.com/packs/ships.html',
})

buildCityBus()
buildContainerShip()
buildBulkCarrier()

rescaleColoredObj('vegetation/typical/street_tree_realistic.obj', 10)
