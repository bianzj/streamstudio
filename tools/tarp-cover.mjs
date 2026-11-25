// ==========================================================================
// 文件：tarp-cover.mjs
// 功能：为任意 OBJ 目标生成“篷布覆盖”网格（Y-up），由 server.mjs 的 /api/project/tarp-cover 调用
// 导出：generateTarpCover(objText, { cell, lift, drape, margin, maxNodes })
// 主要步骤：
//   1. 解析 OBJ 顶点与三角形，求包围盒，缺省网格尺寸 CELL 取 clamp(最大水平尺寸/80, 0.05, 0.5) m
//   2. 建立以目标足迹中心对称的规则高度场，宽度足以容纳下垂裙边
//   3. 把目标所有三角形采样“盖章”到高度场，得到目标上表面高度
//   4. 倒锥形最大值传播（模拟布料下垂）并做只升不降的平滑
//   5. 仅输出被覆盖区域及其一圈邻居的网格点，加上 lift 抬升并三角化
//   6. 返回 OBJ 文本与统计量（顶点数、三角形数、尺寸、足迹）
// 约定：OBJ 单位 m、Y 向上；高度场在 XZ 平面；lift 为篷布离目标表面的抬升（m）；
//       drape 为裙边每水平 1 m 下降的高度（m/m）；margin 为足迹外额外留出的水平余量（m）。
// ==========================================================================

// Draped tarp cover for an arbitrary OBJ, in the target's own coordinates (Y up).
// The cover is a heightfield: the target's top surface lifted by `lift`, extended
// outward like hanging cloth (height falls `drape` m per m) down to the target's base.
export function generateTarpCover(objText, { cell, lift = 0.05, drape = 6, margin = 0.15, maxNodes = 400000 } = {}) {
  const vertices = [], triangles = []
  for (const line of String(objText).split(/\r?\n/)) {
    const p = line.trim().split(/\s+/)
    if (p[0] === 'v' && p.length >= 4) vertices.push([Number(p[1]), Number(p[2]), Number(p[3])])
    else if (p[0] === 'f' && p.length >= 4) {
      const ids = p.slice(1).map((token) => { const k = Number(token.split('/')[0]); return k < 0 ? vertices.length + k : k - 1 })
      for (let k = 1; k < ids.length - 1; k += 1) triangles.push([ids[0], ids[k], ids[k + 1]])
    }
  }
  if (!triangles.length) throw new Error('目标 OBJ 没有面片')
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity]
  for (const v of vertices) for (let k = 0; k < 3; k += 1) { lo[k] = Math.min(lo[k], v[k]); hi[k] = Math.max(hi[k], v[k]) }
  const size = hi.map((value, k) => value - lo[k])
  if (!size.every(Number.isFinite)) throw new Error('目标 OBJ 顶点无效')
  // 步骤 1：未指定网格尺寸时按目标水平尺寸自适应，限幅 0.05–0.5 m。
  const CELL = Number(cell) > 0 ? Number(cell) : Math.max(0.05, Math.min(0.5, Math.max(size[0], size[2]) / 80))

  // Grid symmetric about the target's footprint centre, wide enough for the skirt.
  const reach = size[1] / drape + margin + CELL
  const cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2
  const halfX = Math.ceil((size[0] / 2 + reach) / CELL), halfZ = Math.ceil((size[2] / 2 + reach) / CELL)
  const nx = 2 * halfX + 1, nz = 2 * halfZ + 1
  if (nx * nz > maxNodes) throw new Error(`篷布网格 ${nx} × ${nz} 过大，请增大网格尺寸`)
  const x0 = cx - halfX * CELL, z0 = cz - halfZ * CELL

  // Stamp every sample into all four surrounding nodes, so any interpolation of
  // the cover over a sample is at least the sample's height (no poke-through).
  // 步骤 3 开始：高度场初值为目标最低高度 lo[1]，即“未覆盖”。
  const height = new Float64Array(nx * nz).fill(lo[1])
  const stamp = (x, y, z) => {
    const fi = (x - x0) / CELL, fj = (z - z0) / CELL
    for (const i of [Math.floor(fi), Math.ceil(fi)]) for (const j of [Math.floor(fj), Math.ceil(fj)]) {
      if (i >= 0 && i < nx && j >= 0 && j < nz) height[j * nx + i] = Math.max(height[j * nx + i], y)
    }
  }
  // 每个三角形按重心坐标加密采样（间隔约半个网格），逐点盖章；保证细长三角形也不漏。
  for (const [a, b, c] of triangles) {
    const A = vertices[a], B = vertices[b], C = vertices[c]
    const edge = Math.max(Math.hypot(B[0] - A[0], B[2] - A[2]), Math.hypot(C[0] - A[0], C[2] - A[2]), Math.hypot(C[0] - B[0], C[2] - B[2]))
    const steps = Math.max(1, Math.ceil(edge / (CELL * 0.5)))
    for (let u = 0; u <= steps; u += 1) for (let v = 0; v <= steps - u; v += 1) {
      const s = u / steps, t = v / steps, w = 1 - s - t
      stamp(A[0] * w + B[0] * s + C[0] * t, A[1] * w + B[1] * s + C[1] * t, A[2] * w + B[2] * s + C[2] * t)
    }
  }
  // 保存目标物体自身的上表面高度（尚未下垂/平滑），仅用于统计 topCells：目标上表面高于其底面的格数，
  // 即目标物体直接占据（被三角形盖章）的格数，不含布料下垂和平滑后扩展出的格。
  // 名字里的 top 指“目标上表面”（不是顶部格）；该字段只随返回值输出，仓库内没有其他使用方，故保留原名。
  const top = Float64Array.from(height)

  // Hanging cloth: chamfer max-propagation with a conical falloff, then a light
  // smoothing that can only raise the surface.
  // 步骤 4：step / diag 为沿轴向 / 对角相邻一格的下垂量（drape × 格距）。
  const step = drape * CELL, diag = step * Math.SQRT2
  // relax：用邻点高度减去 drop 抬高本格（只升不降）；正反两次扫描覆盖 8 邻域，近似倒锥形距离变换。
  const relax = (i, j, di, dj, drop) => {
    const ii = i + di, jj = j + dj
    if (ii < 0 || ii >= nx || jj < 0 || jj >= nz) return
    height[j * nx + i] = Math.max(height[j * nx + i], height[jj * nx + ii] - drop)
  }
  for (let j = 0; j < nz; j += 1) for (let i = 0; i < nx; i += 1) {
    relax(i, j, -1, 0, step); relax(i, j, 0, -1, step); relax(i, j, -1, -1, diag); relax(i, j, 1, -1, diag)
  }
  for (let j = nz - 1; j >= 0; j -= 1) for (let i = nx - 1; i >= 0; i -= 1) {
    relax(i, j, 1, 0, step); relax(i, j, 0, 1, step); relax(i, j, 1, 1, diag); relax(i, j, -1, 1, diag)
  }
  // 平滑 2 遍：取原值与（自身×4 + 4 邻点）/8 的较大者，只升不降，使布面更连续。
  for (let pass = 0; pass < 2; pass += 1) {
    const copy = Float64Array.from(height)
    for (let j = 1; j < nz - 1; j += 1) for (let i = 1; i < nx - 1; i += 1) {
      const k = j * nx + i
      height[k] = Math.max(copy[k], (4 * copy[k] + copy[k - 1] + copy[k + 1] + copy[k - nx] + copy[k + nx]) / 8)
    }
  }

  // 步骤 5：covered 表示该格高于目标底面；仅保留被覆盖格及其 8 邻域的顶点，其余不生成。
  const covered = (k) => height[k] > lo[1] + 1e-6
  const index = new Int32Array(nx * nz).fill(-1)
  const out = ['# Stream Studio draped tarp cover', 'g tarp_cover']
  let vertexCount = 0
  for (let j = 0; j < nz; j += 1) for (let i = 0; i < nx; i += 1) {
    const k = j * nx + i
    let near = false
    for (let dj = -1; dj <= 1 && !near; dj += 1) for (let di = -1; di <= 1 && !near; di += 1) {
      const ii = i + di, jj = j + dj
      near = ii >= 0 && ii < nx && jj >= 0 && jj < nz && covered(jj * nx + ii)
    }
    if (!near) continue
    index[k] = ++vertexCount
    out.push(`v ${(x0 + i * CELL).toFixed(4)} ${(height[k] + (covered(k) ? lift : 0)).toFixed(4)} ${(z0 + j * CELL).toFixed(4)}`)
  }
  // 四个角点都存在时才生成方格，拆为两个三角形；index 为顶点编号（从 1 起，OBJ 约定）。
  let faceCount = 0
  for (let j = 0; j < nz - 1; j += 1) for (let i = 0; i < nx - 1; i += 1) {
    const a = index[j * nx + i], b = index[j * nx + i + 1], c = index[(j + 1) * nx + i + 1], d = index[(j + 1) * nx + i]
    if (a < 0 || b < 0 || c < 0 || d < 0) continue
    out.push(`f ${a} ${d} ${c}`, `f ${a} ${c} ${b}`)
    faceCount += 2
  }
  // 步骤 6：统计输出网格的包围盒，用于返回 dimensions。
  let coverLo = [Infinity, Infinity, Infinity], coverHi = [-Infinity, -Infinity, -Infinity]
  for (let k = 0; k < nx * nz; k += 1) {
    if (index[k] < 0) continue
    const p = [x0 + (k % nx) * CELL, height[k] + (covered(k) ? lift : 0), z0 + Math.floor(k / nx) * CELL]
    for (let a = 0; a < 3; a += 1) { coverLo[a] = Math.min(coverLo[a], p[a]); coverHi[a] = Math.max(coverHi[a], p[a]) }
  }
  return {
    obj: `${out.join('\n')}\n`,
    vertices: vertexCount, triangles: faceCount, cell: CELL,
    targetSize: size.map((v) => Number(v.toFixed(3))),
    targetFootprint: { minX: lo[0], maxX: hi[0], minZ: lo[2], maxZ: hi[2] },
    dimensions: coverHi.map((v, k) => Number((v - coverLo[k]).toFixed(3))),
    topCells: top.reduce((n, value) => n + (value > lo[1] + 1e-6 ? 1 : 0), 0)
  }
}
