// ==========================================================================
// 文件：test-server-fixes-20261125.mjs
// 功能：2026-11-25 代码审阅修复中 server.mjs 部分的回归测试（node:test，不需要引擎、不需要浏览器）。
// 主要模块：
//   1. enviMetadata：`key={...}`（`=` 后无空白且值以 { 开头）也能解析，其余写法的结果与旧正则一致
//   2. convertEnviToTiff：头文件写成 `band names={a,b}` 时波段名进入 TIFF，转换成功后 Voxel 模式删除 ENVI 原文件
//   3. 输出文件名的时间/角度标记：outputTimeToken 与旧实现逐值一致（随机抽样），典型值固定
//   4. /api/project/import-dem：超过 64MB（含 base64 预检）统一报“超过 64MB”，正常 ASC 可导入
// 运行：node --test tools/test-server-fixes-20261125.mjs
// 说明：server.mjs 以 STREAM_STUDIO_SERVER_AUTOSTART=0 导入（不自动监听），第 4 项在进程内临时监听 127.0.0.1:0。
// ==========================================================================

import assert from 'node:assert/strict'
import { after, test } from 'node:test'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

const root = mkdtempSync(join(tmpdir(), 'server-fixes-'))
process.env.STREAM_STUDIO_SERVER_AUTOSTART = '0'
process.env.STREAM_STUDIO_DATA_ROOT = join(root, 'data')
const serverModule = await import(new URL('../server.mjs', import.meta.url))
const { server, stopEngines, convertEnviToTiff, enviMetadata, outputAngleToken, outputTimeToken, outputTiffName } = serverModule
after(() => {
  stopEngines()
  server.close()
  server.closeAllConnections?.()
  rmSync(root, { recursive: true, force: true })
})

// 模块 1：ENVI 头解析。
test('enviMetadata parses key={...} as well as key = {...}', () => {
  const header = [
    'ENVI',
    'description = {',
    ' File Imported into ENVI.} ',
    'samples = 4',
    'lines   = 3',
    'bands={2}',
    'band names={alpha,beta}',
    'Data Type = 4',
    'wavelength = { 400, 500 }',
    'interleave=bsq',
    ''
  ].join('\r\n')
  const meta = enviMetadata(header)
  assert.equal(meta.samples, '4')
  assert.equal(meta.lines, '3')
  assert.equal(meta.bands, '{2}')
  assert.equal(meta['band names'], '{alpha,beta}')
  assert.equal(meta['data type'], '4')
  assert.equal(meta.wavelength, '{ 400, 500 }')
  assert.equal(meta.interleave, 'bsq')
  // 多行花括号块只取 = 所在行（旧行为），续行不含 = 因而被忽略。
  assert.equal(meta.description, '{')
})

// 模块 2：ENVI → TIFF 转换（Voxel 模式转换后删除原文件）。
test('convertEnviToTiff reads band names written without spaces and removes the ENVI pair for voxel modes', () => {
  const directory = join(root, 'envi')
  mkdirSync(directory, { recursive: true })
  const stem = 'SZA=30.00_SAA=120.00_VZA=0.00_VAA=0.00'
  const header = join(directory, `${stem}.hdr`)
  const image = join(directory, `${stem}.img`)
  writeFileSync(header, ['ENVI', 'samples = 2', 'lines = 2', 'bands = 2', 'header offset = 0', 'data type = 4',
    'interleave = bsq', 'byte order = 0', 'band names={alpha,beta}', ''].join('\n'))
  writeFileSync(image, Buffer.from(new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]).buffer))
  const tiff = convertEnviToTiff(header, 'eVoxelRT')
  assert.equal(tiff, join(directory, `${stem}_v.tif`))
  assert.ok(readFileSync(tiff).includes('HiStream image bands: {alpha, beta}'))
  assert.equal(existsSync(header), false)
  assert.equal(existsSync(image), false)
})

// 模块 3：文件名标记。reference* 是修复前 server.mjs 的实现，用来逐值比对。
function referenceTimeToken(julianTime) {
  const value = Number(julianTime)
  if (!Number.isFinite(value)) return ''
  let day = Math.floor(value)
  let minutes = Math.round((value - day) * 1440)
  if (minutes >= 1440) { day += Math.floor(minutes / 1440); minutes %= 1440 }
  if (minutes < 0) minutes = 0
  const hour = String(Math.floor(minutes / 60)).padStart(2, '0')
  const minute = String(minutes % 60).padStart(2, '0')
  return 'DOY' + day + '_' + hour + '-' + minute
}
function referenceAngleToken(value) {
  const number = Number(value)
  return Number.isFinite(number) ? number.toFixed(2) : '0.00'
}

test('output time/angle tokens keep the previous naming', () => {
  assert.equal(outputTimeToken(195.5), 'DOY195_12-00')
  assert.equal(outputTimeToken(195.9999999), 'DOY196_00-00')
  assert.equal(outputTimeToken(Number.NaN), '')
  assert.equal(outputAngleToken('x'), '0.00')
  assert.equal(outputAngleToken(12.345), (12.345).toFixed(2))
  let seed = 12345
  const random = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648
  for (let index = 0; index < 5000; index += 1) {
    const julian = Math.floor(random() * 366) + random()
    assert.equal(outputTimeToken(julian), referenceTimeToken(julian), String(julian))
    const angle = (random() - 0.5) * 720
    assert.equal(outputAngleToken(angle), referenceAngleToken(angle), String(angle))
  }
  assert.equal(outputTiffName('eVoxelEB', { julianTime: 195.5, vza: '0.00', vaa: '0.00' }), 'T=DOY195_12-00_VZA=0.00_VAA=0.00_v.tif')
  assert.equal(outputTiffName('eFacetRT', { sza: 30, saa: 120, vza: 10, vaa: 20 }), 'SZA=30.00_SAA=120.00_VZA=10.00_VAA=20.00_f.tif')
})

// 模块 4：DEM 导入大小限制。
test('import-dem rejects oversize payloads with one consistent 64MB message', async () => {
  await new Promise((done) => server.listen(0, '127.0.0.1', done))
  const base = `http://127.0.0.1:${server.address().port}`
  const post = async (path, payload) => {
    const response = await fetch(base + path, { method: 'POST', headers: { 'content-type': 'application/json', origin: base }, body: JSON.stringify(payload) })
    return { status: response.status, data: await response.json() }
  }
  const created = await post('/api/project/create', { directory: join(root, 'projects'), name: 'dem', mode: 'eVoxelRT' })
  assert.equal(created.status, 201, JSON.stringify(created.data))
  const projectPath = created.data.projectPath
  // 64 MiB 解码后恰为上限；多一个字符的 base64（> 上限推出的字符数）在解码前就被拒绝。
  const limitChars = Math.ceil(64 * 1024 * 1024 / 3) * 4
  const tooLong = await post('/api/project/import-dem', { projectPath, name: 'big.asc', contentBase64: 'A'.repeat(limitChars + 1) })
  assert.equal(tooLong.status, 400)
  assert.match(tooLong.data.error, /超过 64MB/)
  const tooBig = await post('/api/project/import-dem', { projectPath, name: 'big.asc', contentBase64: 'A'.repeat(limitChars) })
  assert.equal(tooBig.status, 400)
  assert.match(tooBig.data.error, /超过 64MB/)
  const asc = ['ncols 3', 'nrows 3', 'xllcorner 0', 'yllcorner 0', 'cellsize 10', 'NODATA_value -9999',
    '1 2 3', '4 5 6', '7 8 9', ''].join('\n')
  const ok = await post('/api/project/import-dem', { projectPath, name: 'small.asc', contentBase64: Buffer.from(asc).toString('base64') })
  assert.equal(ok.status, 201, JSON.stringify(ok.data))
  assert.equal(ok.data.relativePath, 'terrain/small.asc')
})
