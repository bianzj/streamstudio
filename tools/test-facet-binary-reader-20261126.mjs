// ==========================================================================
// 文件：test-facet-binary-reader-20261126.mjs
// 功能：FacetRT/FacetEB 结果读取器的 JSON / 二进制（control.facetOutputFormat）两种格式一致性测试
//       （node:test，不需要引擎、不需要浏览器、不依赖 GPU）。
// 主要模块：
//   1. 合成同一份面元结果，分别按 JSON 格式与 docs/facet-binary-output.md 规定的二进制格式写盘
//      （含全部内联、混合内联/描述符、全部描述符三种规模；稀疏抽样规模）
//   2. server/facet-result.mjs：readFacetResult / sampleFacetResult / isFacetResultFile / facetResultCount 两种格式读到的值相同
//   3. 旁路文件：facetSidecarFiles / facetResultBytes / copyFacetSidecars / removeFacetResult
//   4. 损坏输入（旁路文件被截断、dtype 不支持、文件名含路径、缺必需数组）报错而不是读出错误数据
//   5. server.mjs 的 /api/results/read：两种格式返回同一份面元结果；修改旁路文件后结果缓存失效
// 运行：node --test tools/test-facet-binary-reader-20261126.mjs
// 说明：JSON 格式的数值按 9 位有效数字写出（与引擎一致），可无损还原为 float32；
//       抽样结果是 JS 数值数组，JSON 路径得到的是十进制文本解析出的 double，因此比较时统一取 Math.fround。
// ==========================================================================

import assert from 'node:assert/strict'
import { after, test } from 'node:test'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, truncateSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

const root = mkdtempSync(join(tmpdir(), 'facet-binary-reader-'))
process.env.STREAM_STUDIO_SERVER_AUTOSTART = '0'
process.env.STREAM_STUDIO_DATA_ROOT = join(root, 'data')
const reader = await import(new URL('../server/facet-result.mjs', import.meta.url))
const serverModule = await import(new URL('../server.mjs', import.meta.url))
const { server, stopEngines } = serverModule
after(() => {
  stopEngines()
  server.close()
  server.closeAllConnections?.()
  rmSync(root, { recursive: true, force: true })
})

// ---- 合成数据与两种写盘格式 ---------------------------------------------------------------------------------------

function pseudoRandom(seed) {
  let state = seed >>> 0
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state / 4294967296 }
}

// 引擎字段：vertexPositions n×9；每侧数组 2n；bandRadiosity bands×2n；bandStates bands×n×4（FacetRT）。
function syntheticArrays(n, bands, seed = 1) {
  const random = pseudoRandom(seed)
  const make = (count, scale = 1) => Float32Array.from({ length: count }, () => (random() - 0.3) * scale)
  return {
    vertexPositions: make(n * 9, 100),
    sunlit: make(2 * n), radiosity: make(2 * n, 500), lightEnhancement: make(2 * n),
    temperature: make(2 * n, 400), netRadiation: make(2 * n, 800), sensibleHeat: make(2 * n, 200),
    latentHeat: make(2 * n, 200), storageHeat: make(2 * n, 100),
    bandRadiosity: make(2 * n * bands, 50), bandStates: make(n * 4 * bands)
  }
}
const syntheticMeta = (n, bands) => ({
  facetCount: n, backend: 'HiStream Facet RT-EB coupled (test)', quantity: 'spectral radiance', units: 'W m-2 sr-1 um-1',
  wavelengths: Array.from({ length: bands }, (_, band) => 450 + 100 * band), iterations: 7, maxDelta: 0.0125,
  graph: { directedEdges: 12345 }, timing: { total: 1.5 }
})

function writeJsonFormat(directory, name, n, bands, arrays) {
  mkdirSync(directory, { recursive: true })
  // 键顺序与引擎写出顺序一致（JSON 读取器按文件内顺序单向扫描）：标量…、vertexPositions、wavelengths、各指标、bandRadiosity、bandStates。
  const { wavelengths, ...scalars } = syntheticMeta(n, bands)
  const meta = { ...scalars }
  const inline = (values) => Array.from(values, (value) => Number(value.toPrecision(9)))
  meta.vertexPositions = inline(arrays.vertexPositions)
  meta.wavelengths = wavelengths
  for (const [key, values] of Object.entries(arrays)) if (key !== 'vertexPositions') meta[key] = inline(values)
  const path = join(directory, name + '.json')
  writeFileSync(path, JSON.stringify(meta), 'utf8')
  return path
}

// 与 docs/facet-binary-output.md 一致：长度 > 1024 的数组换成描述符；.bin 小端、16 字节对齐、无文件头。
function writeBinaryFormat(directory, name, n, bands, arrays) {
  mkdirSync(directory, { recursive: true })
  const meta = { ...syntheticMeta(n, bands) }
  const chunks = []
  let offset = 0
  const shapes = {
    vertexPositions: [n, 3, 3], bandRadiosity: [bands, 2 * n], bandStates: [bands, 2 * n, 2]
  }
  for (const [key, values] of Object.entries(arrays)) {
    if (values.length <= 1024) { meta[key] = Array.from(values, (value) => Number(value.toPrecision(9))); continue }
    const padding = (16 - (offset % 16)) % 16
    if (padding) { chunks.push(Buffer.alloc(padding)); offset += padding }
    meta[key] = { file: name + '.bin', offset, length: values.length, dtype: 'float32', shape: shapes[key] || [values.length] }
    chunks.push(Buffer.from(values.buffer, values.byteOffset, values.byteLength))
    offset += values.byteLength
  }
  Object.assign(meta, { format: 'binary', formatVersion: 1, binaryFile: name + '.bin', binaryBytes: offset })
  writeFileSync(join(directory, name + '.bin'), Buffer.concat(chunks))
  const path = join(directory, name + '.json')
  writeFileSync(path, JSON.stringify(meta), 'utf8')
  return path
}

// 一次写出两种格式（不同子目录，同一份数据）。
function writePair(label, n, bands, seed) {
  const arrays = syntheticArrays(n, bands, seed)
  return {
    n, bands, arrays,
    json: writeJsonFormat(join(root, label, 'json'), 'facetrt', n, bands, arrays),
    binary: writeBinaryFormat(join(root, label, 'binary'), 'facetrt', n, bands, arrays)
  }
}

const sameFloats = (actual, expected, label) => {
  assert.equal(actual.length, expected.length, label + ' length')
  for (let index = 0; index < expected.length; index += 1) {
    if (Math.fround(actual[index]) !== Math.fround(expected[index])) assert.fail(`${label}[${index}] ${actual[index]} != ${expected[index]}`)
  }
}

// ---- 1. 完整读取 ---------------------------------------------------------------------------------------------------

// 40：全部内联（vertexPositions 360 个）；150：vertexPositions 描述符、每侧数组内联（300）；700：全部描述符。
for (const n of [40, 150, 700]) {
  test(`readFacetResult 两种格式一致（${n} 面元）`, () => {
    const pair = writePair('read-' + n, n, 3, n)
    assert.equal(reader.facetResultFormat(pair.json), 'json')
    assert.equal(reader.facetResultFormat(pair.binary), 'binary')
    assert.equal(reader.facetResultCount(pair.json), n)
    assert.equal(reader.facetResultCount(pair.binary), n)
    assert.equal(reader.isFacetResultFile(pair.json), true)
    assert.equal(reader.isFacetResultFile(pair.binary), true)
    const fromJson = reader.readFacetResult(pair.json)
    const fromBinary = reader.readFacetResult(pair.binary)
    for (const key of ['facetCount', 'backend', 'quantity', 'units', 'iterations', 'maxDelta']) assert.equal(fromBinary[key], fromJson[key], key)
    assert.deepEqual(fromBinary.wavelengths, fromJson.wavelengths)
    assert.deepEqual(fromBinary.wavelengths, [450, 550, 650])
    assert.equal(fromBinary.facetCount, n)
    sameFloats(fromBinary.vertexPositions, pair.arrays.vertexPositions, 'vertexPositions')
    sameFloats(fromBinary.vertexPositions, fromJson.vertexPositions, 'vertexPositions vs json')
    assert.deepEqual(fromBinary.metrics.map(([id, name]) => [id, name]), fromJson.metrics.map(([id, name]) => [id, name]))
    assert.ok(fromBinary.metrics.length >= 8)
    for (let index = 0; index < fromJson.metrics.length; index += 1) {
      assert.ok(fromBinary.metrics[index][2] instanceof Float32Array)
      sameFloats(fromBinary.metrics[index][2], fromJson.metrics[index][2], 'metric ' + fromJson.metrics[index][0])
    }
    sameFloats(fromBinary.bandRadiosity, fromJson.bandRadiosity, 'bandRadiosity')
    sameFloats(fromBinary.bandStates, fromJson.bandStates, 'bandStates')
    sameFloats(fromBinary.bandStates, pair.arrays.bandStates, 'bandStates source')
    // 只读几何：不读指标，但几何与波段相同。
    const geometryJson = reader.readFacetResult(pair.json, true)
    const geometryBinary = reader.readFacetResult(pair.binary, true)
    assert.equal(geometryBinary.metrics.length, 0)
    assert.equal(geometryBinary.bandRadiosity.length, 0)
    sameFloats(geometryBinary.vertexPositions, geometryJson.vertexPositions, 'geometry')
    assert.deepEqual(geometryBinary.wavelengths, geometryJson.wavelengths)
    assert.equal(geometryBinary.backend, geometryJson.backend)
  })
}

// ---- 2. 抽样 ---------------------------------------------------------------------------------------------------------

for (const [n, maximum, label] of [[700, 700, '不抽样'], [700, 64, '抽样'], [40000, 1000, '密集窗口抽样'], [40000, 10, '稀疏逐记录抽样']]) {
  test(`sampleFacetResult 两种格式一致（${n} 面元，${label}，上限 ${maximum}）`, () => {
    const pair = writePair('sample-' + n, n, 2, n + maximum)
    const fromJson = reader.sampleFacetResult(pair.json, maximum)
    const fromBinary = reader.sampleFacetResult(pair.binary, maximum)
    assert.equal(fromBinary.kind, 'facet')
    assert.equal(fromBinary.facetCount, Math.min(n, maximum))
    for (const key of ['facetCount', 'sourceFacetCount', 'sampled', 'backend', 'iterations', 'maxDelta']) assert.equal(fromBinary[key], fromJson[key], key)
    assert.equal(fromBinary.sampled, maximum < n)
    sameFloats(fromBinary.vertexPositions, fromJson.vertexPositions, 'vertexPositions')
    assert.ok(Array.isArray(fromBinary.vertexPositions))
    assert.deepEqual(fromBinary.metrics.map(({ id, label: text }) => [id, text]), fromJson.metrics.map(({ id, label: text }) => [id, text]))
    for (let index = 0; index < fromJson.metrics.length; index += 1) {
      assert.ok(Array.isArray(fromBinary.metrics[index].values))
      sameFloats(fromBinary.metrics[index].values, fromJson.metrics[index].values, fromJson.metrics[index].id)
    }
    // 抽样序号 floor(i·N/n)：直接对照源数组。
    const facetCount = fromBinary.facetCount
    for (const sample of [0, Math.floor(facetCount / 2), facetCount - 1]) {
      const source = facetCount === n ? sample : Math.min(n - 1, Math.floor(sample * n / facetCount))
      for (let value = 0; value < 9; value += 1) assert.equal(fromBinary.vertexPositions[sample * 9 + value], pair.arrays.vertexPositions[source * 9 + value])
      assert.equal(fromBinary.metrics.find(({ id }) => id === 'radiosity').values[sample * 2 + 1], pair.arrays.radiosity[source * 2 + 1])
    }
    // 几何模式：没有指标。
    const geometry = reader.sampleFacetResult(pair.binary, maximum, true)
    assert.equal(geometry.metrics.length, 0)
    sameFloats(geometry.vertexPositions, fromJson.vertexPositions, 'geometry sample')
  })
}

// ---- 3. 旁路文件管理 -----------------------------------------------------------------------------------------------

test('旁路文件：列出、计大小、复制、删除', () => {
  const pair = writePair('sidecars', 700, 3, 5)
  assert.deepEqual(reader.facetSidecarFiles(pair.json), [])
  assert.deepEqual(reader.facetSidecarFiles(pair.binary), [join(root, 'sidecars', 'binary', 'facetrt.bin')])
  const binFile = join(root, 'sidecars', 'binary', 'facetrt.bin')
  assert.equal(reader.facetResultBytes(pair.json), statSync(pair.json).size)
  assert.equal(reader.facetResultBytes(pair.binary), statSync(pair.binary).size + statSync(binFile).size)
  // 描述符对齐：每个 offset 是 16 的倍数。
  const meta = JSON.parse(readFileSync(pair.binary, 'utf8'))
  assert.equal(meta.format, 'binary')
  for (const value of Object.values(meta)) if (value && typeof value === 'object' && value.dtype) assert.equal(value.offset % 16, 0)
  // 复制到另一目录后仍可读取（JSON 按文件名引用旁路文件）。
  const copyDirectory = join(root, 'sidecars', 'copy')
  mkdirSync(copyDirectory, { recursive: true })
  writeFileSync(join(copyDirectory, 'faceteb_T=12-00.json'), readFileSync(pair.binary))
  assert.equal(reader.copyFacetSidecars(pair.binary, copyDirectory).length, 1)
  assert.equal(reader.copyFacetSidecars(pair.binary, copyDirectory).length, 0, '第二次复制应跳过')
  const copied = reader.readFacetResult(join(copyDirectory, 'faceteb_T=12-00.json'))
  sameFloats(copied.vertexPositions, pair.arrays.vertexPositions, 'copied geometry')
  // 删除：JSON 与旁路文件都消失；JSON 格式只删 JSON。
  assert.equal(reader.removeFacetResult(pair.binary), 2)
  assert.equal(existsSync(pair.binary) || existsSync(binFile), false)
  assert.equal(reader.removeFacetResult(pair.json), 1)
  assert.equal(existsSync(pair.json), false)
})

test('非面元 JSON 不被识别为面元结果', () => {
  const other = join(root, 'other.json')
  writeFileSync(other, JSON.stringify({ kind: 'facet-energy-process', facetCount: 10, fields: ['a'] }))
  assert.equal(reader.isFacetResultFile(other), false)
  const manifest = join(root, 'run-manifest.json')
  writeFileSync(manifest, JSON.stringify({ runId: 'x', format: 'other' }))
  assert.equal(reader.isFacetResultFile(manifest), false)
})

// ---- 4. 损坏输入 -----------------------------------------------------------------------------------------------------

test('二进制损坏输入报错', () => {
  const pair = writePair('corrupt', 700, 3, 9)
  const directory = join(root, 'corrupt', 'binary')
  const meta = JSON.parse(readFileSync(pair.binary, 'utf8'))
  // 旁路文件被截断：读取与抽样都要报“不完整”，而不是读出垃圾。
  truncateSync(join(directory, 'facetrt.bin'), 20000)
  assert.throws(() => reader.readFacetResult(pair.binary), /不完整/)
  assert.throws(() => reader.sampleFacetResult(pair.binary, 100), /不完整/)
  // 只有尾部数组（bandStates）被截断：完整读取报错，几何抽样仍可用。
  writeBinaryFormat(directory, 'facetrt', 700, 3, pair.arrays)
  truncateSync(join(directory, 'facetrt.bin'), statSync(join(directory, 'facetrt.bin')).size - 4096)
  assert.throws(() => reader.readFacetResult(pair.binary), /bandStates 数据不完整/)
  assert.equal(reader.sampleFacetResult(pair.binary, 100).facetCount, 100)
  // 缺失旁路文件。
  rmSync(join(directory, 'facetrt.bin'))
  assert.throws(() => reader.readFacetResult(pair.binary), /缺少面元结果旁路文件/)
  // 不支持的 dtype / 含路径的文件名 / 更高的格式版本。
  const rewrite = (patch) => writeFileSync(pair.binary, JSON.stringify(patch(structuredClone(meta))))
  writeBinaryFormat(directory, 'facetrt', 700, 3, pair.arrays)
  rewrite((value) => { value.vertexPositions.dtype = 'float64'; return value })
  assert.throws(() => reader.readFacetResult(pair.binary), /float32/)
  rewrite((value) => { value.vertexPositions.file = '../facetrt.bin'; return value })
  assert.throws(() => reader.readFacetResult(pair.binary), /旁路文件名/)
  rewrite((value) => { value.formatVersion = 2; return value })
  assert.throws(() => reader.readFacetResult(pair.binary), /版本/)
  rewrite((value) => { delete value.radiosity; return value })
  assert.throws(() => reader.readFacetResult(pair.binary), /缺少 radiosity/)
  rewrite((value) => { value.sunlit = Array.from({ length: 5 }, () => 0); return value })
  assert.throws(() => reader.readFacetResult(pair.binary), /sunlit 数据不完整/)
  assert.deepEqual(reader.facetSidecarFiles(pair.binary).map((path) => path.endsWith('facetrt.bin')), [true], '无效文件名不进入旁路文件列表')
})

// ---- 5. HTTP 读取与结果缓存 ------------------------------------------------------------------------------------------------

test('/api/results/read：两种格式返回同一份面元结果，旁路文件变化使缓存失效', async () => {
  const pair = writePair('http', 300, 2, 77)
  await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen))
  const base = `http://127.0.0.1:${server.address().port}`
  const read = async (path) => {
    const response = await fetch(base + '/api/results/read', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ path }) })
    const body = await response.json()
    assert.equal(response.status, 200, JSON.stringify(body))
    return { body, cache: response.headers.get('x-cache') }
  }
  const json = await read(pair.json)
  const binary = await read(pair.binary)
  assert.equal(json.body.kind, 'facet')
  assert.equal(binary.body.kind, 'facet')
  assert.equal(binary.body.facetCount, 300)
  sameFloats(binary.body.vertexPositions, json.body.vertexPositions, 'http vertexPositions')
  for (let index = 0; index < json.body.metrics.length; index += 1) sameFloats(binary.body.metrics[index].values, json.body.metrics[index].values, 'http ' + json.body.metrics[index].id)
  assert.equal((await read(pair.binary)).cache, 'hit')
  // 只改旁路文件内容（保持大小，改 mtime）：必须重新读取，而不是返回旧缓存。
  const binFile = join(root, 'http', 'binary', 'facetrt.bin')
  const bytes = readFileSync(binFile)
  bytes.writeFloatLE(12345.5, 0)
  await new Promise((resolveWait) => setTimeout(resolveWait, 30))
  writeFileSync(binFile, bytes)
  const changed = await read(pair.binary)
  assert.equal(changed.cache, 'miss')
  assert.equal(changed.body.vertexPositions[0], 12345.5)
})
