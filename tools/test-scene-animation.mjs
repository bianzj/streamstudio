// Scene-animation (eSceneAnimation) job schema and server confinement checks. No GPU or engine is used:
// every run request below must be rejected before anything is spawned.
// Run: node --test tools/test-scene-animation.mjs
import assert from 'node:assert/strict'
import { after, before, test } from 'node:test'
import { createServer } from 'node:http'
import { existsSync, mkdirSync, mkdtempSync, realpathSync, rmSync, symlinkSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, isAbsolute, join, relative, resolve } from 'node:path'
import {
  createSceneAnimationJob, ENGINE_MODES, normalizeSceneAnimationJob, PROJECT_MODES, SCENE_ANIMATION_JOB_FORMAT,
  SCENE_ANIMATION_MODE, sceneAnimationFileReferences, validateSceneAnimationInput, validateSceneAnimationJob
} from '../src/renderer/src/project-schema.js'
import {
  createSceneAnimationService, isSceneAnimationManifest, SCENE_ANIMATION_ENGINE_VARIABLE,
  sceneAnimationManifestBackend, sceneAnimationManifestSummary
} from '../server/scene-animation.mjs'

const meshSequenceInput = (sequence = {}) => ({
  backend: 'meshSequence', width: 192, height: 108,
  assets: [{ id: 'a', path: 'asset.obj', sha256: '00' }],
  instances: [], cameras: [{ id: 'view' }],
  sequence: { fps: 8, frameCount: 24, firstFrame: 1, lastFrame: 24, impactTime: 0.5, ...sequence }
})

test('eSceneAnimation is an engine mode but not a project-editor mode', () => {
  assert.equal(SCENE_ANIMATION_MODE, 'eSceneAnimation')
  assert.ok(!PROJECT_MODES.includes(SCENE_ANIMATION_MODE))
  assert.deepEqual(ENGINE_MODES, [...PROJECT_MODES, SCENE_ANIMATION_MODE])
})

test('job documents are created, normalized and validated', () => {
  const job = createSceneAnimationJob({ input: ' "D:\\cases\\scene.json" ', outputRoot: 'D:\\out' })
  assert.equal(job.format, SCENE_ANIMATION_JOB_FORMAT)
  assert.equal(job.mode, SCENE_ANIMATION_MODE)
  assert.equal(job.schemaVersion, 1)
  assert.equal(job.input, 'D:\\cases\\scene.json')
  assert.ok(!Number.isNaN(Date.parse(job.createdAt)))
  assert.equal(normalizeSceneAnimationJob({ input: 42, outputRoot: null, label: { x: 1 } }).outputRoot, '')
  assert.equal(validateSceneAnimationJob(job).valid, true)
  for (const [value, message] of [
    [{}, '请选择场景动画输入 JSON'],
    [{ input: 'D:\\scene.txt' }, '.json'],
    [{ input: 'D:\\a.json', format: 'histream-project' }, 'format'],
    [{ input: 'D:\\a.json', mode: 'eVoxelEB' }, '运行模式'],
    [{ input: 'D:\\a.json', schemaVersion: 2 }, 'schemaVersion'],
    [{ input: ['D:\\a.json'] }, 'input 必须是路径字符串'],
    [{ input: 'D:\\a\u0001.json' }, '控制字符']
  ]) {
    const result = validateSceneAnimationJob(value)
    assert.equal(result.valid, false, JSON.stringify(value))
    assert.ok(result.errors.some((error) => error.includes(message)), `${JSON.stringify(value)} -> ${result.errors}`)
  }
  assert.equal(validateSceneAnimationJob(null).valid, false)
})

test('inputs: mesh sequences are summarized, impact-v1 presentation scale is surfaced', () => {
  const checked = validateSceneAnimationInput(meshSequenceInput({ optical: true, cameraObservation: { enabled: true } }))
  assert.equal(checked.valid, true, checked.errors.join())
  assert.equal(checked.backend, 'meshSequence')
  assert.equal(checked.summary.fps, 8)
  assert.deepEqual([checked.summary.firstFrame, checked.summary.lastFrame], [1, 24])
  assert.equal(checked.summary.motionBackend, 'authored')
  assert.equal(checked.summary.optical, true)
  assert.equal(checked.summary.cameraObservation, true)
  const impact = validateSceneAnimationInput(meshSequenceInput({ motionBackend: 'impact-v1', impactPhysics: { presentationTimeScale: 0.04 } }))
  assert.equal(impact.summary.motionBackend, 'impact-v1')
  assert.equal(impact.summary.presentationTimeScale, 0.04)
  const procedural = validateSceneAnimationInput({ width: 64, height: 48, frameCount: 10, renderMode: 'thermal' })
  assert.equal(procedural.valid, true)
  assert.equal(procedural.backend, 'procedural')
  assert.equal(procedural.summary.lastFrame, 10)
  assert.equal(validateSceneAnimationInput({ backend: 'mesh', assets: [], cameras: [] }).valid, true)
})

test('inputs the engine would silently misread are rejected', () => {
  for (const [value, message] of [
    [{ format: 'histream-project', mode: 'eVoxelEB', configuration: {} }, 'project.json'],
    [{ mode: 'eFacetRT', configuration: {} }, 'project.json'],
    [{ schemaVersion: 1, pipeline: 'native-scene', scene: 'x.json' }, 'run_case.py'],
    [{ format: SCENE_ANIMATION_JOB_FORMAT, input: 'x.json' }, '任务记录'],
    [{ assets: [], cameras: [] }, '没有 backend'],
    [{ sequence: { fps: 8 } }, 'sequence 但没有 backend'],
    [{ backend: 'voxel' }, '未知的场景动画 backend'],
    [{ backend: 'meshSequence' }, '必须包含 sequence'],
    [{ backend: 'meshSequence', sequence: [] }, 'sequence 必须是对象'],
    [{}, '无法识别'],
    [[], '根节点']
  ]) {
    const result = validateSceneAnimationInput(value)
    assert.equal(result.valid, false, JSON.stringify(value))
    assert.ok(result.errors.some((error) => error.includes(message)), `${JSON.stringify(value)} -> ${result.errors}`)
  }
  assert.ok(validateSceneAnimationInput({ backend: 'mesh', sequence: {} }).warnings.length)
})

test('hashed file references are collected from the whole input', () => {
  const value = meshSequenceInput({ fragments: { path: 'chunks.json', sha256: '11' } })
  value.backgroundField = { path: 'background/field.json', sha256: '22' }
  const paths = sceneAnimationFileReferences(value).map((item) => item.path).sort()
  assert.deepEqual(paths, ['asset.obj', 'background/field.json', 'chunks.json'])
})

test('manifest summaries are generic over impact model kinds', () => {
  const procedural = { kind: 'histream-gpu-scene-animation', status: 'complete', backend: 'gpu' }
  assert.equal(sceneAnimationManifestBackend(procedural), 'procedural')
  assert.equal(isSceneAnimationManifest(procedural), true)
  assert.equal(isSceneAnimationManifest({ kind: 'histream-run', status: 'completed' }), false)
  assert.equal(isSceneAnimationManifest({ backend: 'meshSequence' }), false)
  const summary = sceneAnimationManifestSummary({
    backend: 'meshSequence', status: 'complete', width: 192, height: 108, fps: 8, firstFrame: 1, lastFrame: 24,
    timelineFrameCount: 24, wallSeconds: 1.5, physicsValidated: false, warnings: ['thin skin', 7],
    impactModel: { kind: 'impact-v1', predictive: false, label: '经验模型估算，未经独立实验验证', presentation: { presentationTimeScale: 0.04 } },
    timeline: {}, optical: { enabled: true }, cameraObservation: { enabled: true }
  })
  assert.equal(summary.backend, 'meshSequence')
  assert.equal(summary.motionBackend, 'authored')
  assert.deepEqual(summary.warnings, ['thin skin'])
  assert.equal(summary.impactModel.kind, 'impact-v1')
  assert.equal(summary.impactModel.presentation.presentationTimeScale, 0.04)
  assert.equal(summary.optical, true)
  assert.equal(summary.cameraObservation.enabled, true)
  assert.equal(summary.physicsValidated, false)
})

// ---------- service over HTTP ----------
const PNG = Buffer.from('89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d4944415478da63f8cfc0f01f0005000201a0d3a5e60000000049454e44ae426082', 'hex')
const root = realpathSync(mkdtempSync(join(tmpdir(), 'scene-animation-test-')))
const runDir = join(root, 'run_1')
const outsideDir = join(root, 'outside')
const pathWithin = (base, candidate) => {
  const rel = relative(resolve(base), resolve(candidate))
  return rel === '' || (rel !== '..' && !rel.startsWith(`..${process.platform === 'win32' ? '\\' : '/'}`) && !isAbsolute(rel))
}
const realPathWithin = (base, candidate) => { try { return pathWithin(realpathSync(base), realpathSync(candidate)) } catch { return false } }
let slotReason = ''
let server
let baseUrl
const events = []

before(async () => {
  mkdirSync(join(runDir, 'frames'), { recursive: true })
  mkdirSync(outsideDir, { recursive: true })
  writeFileSync(join(runDir, 'run-manifest.json'), JSON.stringify({ backend: 'meshSequence', status: 'complete', firstFrame: 1, lastFrame: 2, fps: 8, warnings: ['w'] }))
  writeFileSync(join(runDir, 'frames', 'frame_0001.png'), PNG)
  writeFileSync(join(runDir, 'frames', 'frame_0002.png'), PNG)
  writeFileSync(join(runDir, 'frames', 'notes.txt'), 'x')
  mkdirSync(join(runDir, 'cameras', 'street', 'frames'), { recursive: true })
  writeFileSync(join(runDir, 'cameras', 'street', 'frames', 'frame_0001.png'), PNG)
  mkdirSync(join(runDir, 'cameras', 'bad id', 'frames'), { recursive: true })
  writeFileSync(join(runDir, 'cameras', 'bad id', 'frames', 'frame_0001.png'), PNG)
  writeFileSync(join(outsideDir, 'frame_0001.png'), PNG)
  writeFileSync(join(outsideDir, 'run-manifest.json'), JSON.stringify({ kind: 'histream-run', status: 'completed' }))
  try { symlinkSync(outsideDir, join(runDir, 'optical_frames'), 'junction') } catch {}
  writeFileSync(join(root, 'scene.json'), JSON.stringify({ ...meshSequenceInput(), assets: [] }))
  writeFileSync(join(root, 'missing-asset.json'), JSON.stringify(meshSequenceInput()))
  writeFileSync(join(root, 'not-histream.exe'), 'MZ')
  const service = createSceneAnimationService({
    json: (response, status, value) => { response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' }); response.end(JSON.stringify(value)) },
    body: async (request) => { const chunks = []; for await (const chunk of request) chunks.push(chunk); return JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}') },
    emit: (event) => events.push(event), normalizeHostPath: (value) => String(value || '').trim(), windowsPath: (value) => String(value || ''),
    pathWithin, realPathWithin, defaultEngine: () => join(root, 'engine', 'histream.exe'),
    engineSlot: { busyReason: () => slotReason, claim: () => { throw new Error('test must never spawn an engine') }, release: () => {} }
  })
  server = createServer(async (request, response) => {
    const url = new URL(request.url, 'http://127.0.0.1')
    if (await service.handle(request, response, url) === false) { response.writeHead(404); response.end('{}') }
  })
  await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen))
  baseUrl = `http://127.0.0.1:${server.address().port}`
})
after(() => { server?.close(); rmSync(root, { recursive: true, force: true }) })

const post = async (path, value) => {
  const response = await fetch(`${baseUrl}${path}`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(value) })
  return { status: response.status, body: await response.json() }
}
const frame = (params, headers = {}) => fetch(`${baseUrl}/api/scene-animation/frame?${new URLSearchParams(params)}`, { headers })

test('results list only engine frame sequences inside the run folder', async () => {
  const { status, body } = await post('/api/scene-animation/result', { outputDir: runDir })
  assert.equal(status, 200, body.error)
  assert.deepEqual(body.frames.map((item) => [item.kind, item.frames]), [['frames', [1, 2]], ['cameras/street/frames', [1]]])
  // additional-camera frames are served; ids outside [A-Za-z0-9_-] and other sub-paths are not
  assert.equal((await frame({ dir: runDir, kind: 'cameras/street/frames', n: '1' })).status, 200)
  for (const kind of ['cameras/bad id/frames', 'cameras/street/../frames', 'cameras/street/notes', 'cameras/../frames'])
    assert.equal((await frame({ dir: runDir, kind, n: '1' })).status, 400, kind)
  assert.equal(body.summary.backend, 'meshSequence')
  assert.deepEqual(body.summary.warnings, ['w'])
  assert.equal((await post('/api/scene-animation/result', { outputDir: join(runDir, 'run-manifest.json') })).status, 200)
  assert.equal((await post('/api/scene-animation/result', { outputDir: outsideDir })).status, 400)
  assert.equal((await post('/api/scene-animation/result', { outputDir: 'relative\\run' })).status, 400)
  assert.equal((await post('/api/scene-animation/result', { outputDir: '\\\\server\\share\\run' })).status, 400)
})

test('frames are served as PNG and traversal or junction escapes are rejected', async () => {
  const ok = await frame({ dir: runDir, kind: 'frames', n: '1' })
  assert.equal(ok.status, 200)
  assert.equal(ok.headers.get('content-type'), 'image/png')
  assert.deepEqual(Buffer.from(await ok.arrayBuffer()), PNG)
  for (const params of [
    { dir: runDir, kind: '..', n: '1' },
    { dir: runDir, kind: 'frames/../..', n: '1' },
    { dir: runDir, kind: 'frames', n: '1/../../x' },
    { dir: runDir, kind: 'frames', n: '-1' },
    { dir: join(runDir, 'frames', '..', '..'), kind: 'frames', n: '1' },
    { dir: outsideDir, kind: 'frames', n: '1' },
    { dir: runDir, kind: 'detector_ir', n: '1' }
  ]) assert.equal((await frame(params)).status, 400, JSON.stringify(params))
  assert.equal((await frame({ dir: runDir, kind: 'frames', n: '3' })).status, 404)
  if (existsSync(join(runDir, 'optical_frames'))) assert.equal((await frame({ dir: runDir, kind: 'optical_frames', n: '1' })).status, 404)
  assert.equal((await frame({ dir: runDir, kind: 'frames', n: '1' }, { 'Sec-Fetch-Site': 'cross-site' })).status, 403)
})

test('runs are refused before spawning without a usable engine or with forbidden fields', async () => {
  const input = join(root, 'scene.json')
  const forbidden = await post('/api/scene-animation/run', { input, executable: 'C:\\Windows\\System32\\cmd.exe' })
  assert.equal(forbidden.status, 400)
  assert.match(forbidden.body.error, /不接受自定义程序/)
  const missingEngine = await post('/api/scene-animation/run', { input })
  assert.equal(missingEngine.status, 400)
  assert.match(missingEngine.body.error, /找不到 HiStream 引擎/)
  const missingAsset = await post('/api/scene-animation/run', { input: join(root, 'missing-asset.json') })
  assert.match(missingAsset.body.error, /文件不存在/)
  assert.match((await post('/api/scene-animation/run', { input: join(root, 'absent.json') })).body.error, /找不到场景动画输入/)
  slotReason = '已有 HiStream 模拟任务正在运行'
  assert.equal((await post('/api/scene-animation/run', { input })).status, 409)
  slotReason = ''
  process.env[SCENE_ANIMATION_ENGINE_VARIABLE] = join(root, 'not-histream.exe')
  try {
    const override = await post('/api/scene-animation/run', { input })
    assert.match(override.body.error, new RegExp(SCENE_ANIMATION_ENGINE_VARIABLE))
    const inspected = await post('/api/scene-animation/inspect', { input })
    assert.equal(inspected.status, 200)
    assert.equal(inspected.body.valid, true)
    assert.equal(inspected.body.engine.ready, false)
  } finally { delete process.env[SCENE_ANIMATION_ENGINE_VARIABLE] }
  assert.equal(events.filter((event) => event.type === 'scene-animation-started').length, 0)
  assert.ok(dirname(input))
})
