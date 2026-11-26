// ==========================================================================
// 文件：tools/test-review-fixes-20261126.mjs
// 功能：针对 2026-11-26 审阅修复的轻量回归测试（不启动 HiStream/STREAM 引擎）。
// 覆盖：工程校验、结果路径访问限制、ByteLru 淘汰、STREAM RT 直散比/组分温度单位。
// 运行：node --test tools/test-review-fixes-20261126.mjs
// ==========================================================================

import assert from 'node:assert/strict'
import { mkdtempSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'

import { createDefaultProject, validateProject } from '../src/renderer/src/project-schema.js'
import { ByteLru } from '../server/result-cache.mjs'
import { createProjectStore } from '../server/stream/config.mjs'

test('validateProject rejects non-positive scene dimensions used by /api/run', () => {
  const project = createDefaultProject({ name: 'bad', mode: 'eFacetRT', sceneX: 60, sceneY: 60, sceneHeight: 20, voxelSize: 1 })
  project.configuration.scene.x = 0
  const result = validateProject(project)
  assert.equal(result.valid, false)
  assert.match(result.errors.join('；'), /场景尺寸无效/)
})

test('ByteLru.resize never leaves the cache over its byte limit', () => {
  const cache = new ByteLru(100)
  cache.set('a', 'A', 20, [])
  cache.set('b', 'B', 20, [])
  cache.resize('a', 95)
  assert.ok(cache.bytes <= cache.limit)
  assert.equal(cache.entries.has('a'), false)
})

test('STREAM RT forwards diffuseFraction and rejects Celsius component temperatures', () => {
  const dir = mkdtempSync(join(tmpdir(), 'ssfix-'))
  const raster = join(dir, 'type.tif')
  writeFileSync(raster, 'x')
  const store = createProjectStore({ normalizeHostPath: (value) => value, windowsPath: (value) => value, forcingDirectories: () => [] })
  const scene = {
    overpass: '2019-08-15T02:30:00Z',
    optical: { enabled: true, bands: [{ center: 600, fwhm: 10 }] },
    thermal: { enabled: true, bands: [{ center: 11, fwhm: 1 }] },
    surface: { type: { file: raster }, lai: { value: 2 }, soilMoisture: { file: raster } },
    view: { vza: { value: 0 }, vaa: { value: 0 } },
    temperature: {
      mode: 'rt',
      components: { soilSunlit: { value: 300 }, soilShaded: { value: 295 }, leafSunlit: { value: 305 }, leafShaded: { value: 298 } },
      energy: { diffuseFraction: 0.6 }
    }
  }
  const config = store.buildEngineConfig(scene, { outputDir: dir, dataDir: dir })
  assert.equal(config.temperature.energy.diffuseFraction, 0.6)
  scene.temperature.components.soilSunlit = { value: 25 }
  assert.throws(() => store.buildEngineConfig(scene, { outputDir: dir, dataDir: dir }), /150–400 K/)
})

// 服务端路径限制：任意绝对路径不能通过 /api/results/read。
test('server rejects result reads outside project output/run directories', async () => {
  process.env.STREAM_STUDIO_SERVER_AUTOSTART = '0'
  const module = await import('../server.mjs')
  await new Promise((resolve, reject) => {
    module.server.once('error', reject)
    module.server.listen(0, '127.0.0.1', resolve)
  })
  try {
    const url = `http://127.0.0.1:${module.server.address().port}/api/results/read`
    const response = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: join(tmpdir(), 'not-a-result.txt') })
    })
    assert.equal(response.status, 400)
  } finally {
    await new Promise((resolve) => module.server.close(resolve))
  }
})
