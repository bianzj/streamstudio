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
  const height = new Float64Array(nx * nz).fill(lo[1])
  const stamp = (x, y, z) => {
    const fi = (x - x0) / CELL, fj = (z - z0) / CELL
    for (const i of [Math.floor(fi), Math.ceil(fi)]) for (const j of [Math.floor(fj), Math.ceil(fj)]) {
      if (i >= 0 && i < nx && j >= 0 && j < nz) height[j * nx + i] = Math.max(height[j * nx + i], y)
    }
  }
  for (const [a, b, c] of triangles) {
    const A = vertices[a], B = vertices[b], C = vertices[c]
    const edge = Math.max(Math.hypot(B[0] - A[0], B[2] - A[2]), Math.hypot(C[0] - A[0], C[2] - A[2]), Math.hypot(C[0] - B[0], C[2] - B[2]))
    const steps = Math.max(1, Math.ceil(edge / (CELL * 0.5)))
    for (let u = 0; u <= steps; u += 1) for (let v = 0; v <= steps - u; v += 1) {
      const s = u / steps, t = v / steps, w = 1 - s - t
      stamp(A[0] * w + B[0] * s + C[0] * t, A[1] * w + B[1] * s + C[1] * t, A[2] * w + B[2] * s + C[2] * t)
    }
  }
  const top = Float64Array.from(height)

  // Hanging cloth: chamfer max-propagation with a conical falloff, then a light
  // smoothing that can only raise the surface.
  const step = drape * CELL, diag = step * Math.SQRT2
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
  for (let pass = 0; pass < 2; pass += 1) {
    const copy = Float64Array.from(height)
    for (let j = 1; j < nz - 1; j += 1) for (let i = 1; i < nx - 1; i += 1) {
      const k = j * nx + i
      height[k] = Math.max(copy[k], (4 * copy[k] + copy[k - 1] + copy[k + 1] + copy[k - nx] + copy[k + nx]) / 8)
    }
  }

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
  let faceCount = 0
  for (let j = 0; j < nz - 1; j += 1) for (let i = 0; i < nx - 1; i += 1) {
    const a = index[j * nx + i], b = index[j * nx + i + 1], c = index[(j + 1) * nx + i + 1], d = index[(j + 1) * nx + i]
    if (a < 0 || b < 0 || c < 0 || d < 0) continue
    out.push(`f ${a} ${d} ${c}`, `f ${a} ${c} ${b}`)
    faceCount += 2
  }
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
