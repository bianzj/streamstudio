// ==========================================================================
// 文件：test-usd-roundtrip-20261125.mjs
// 功能：usd-export → usd-import 往返测试（node:test，不需要引擎）：scene.offsetY ≠ 0 且有地形时，
//   贴地对象（车辆）与普通对象的放置高度往返后不应漂移（审阅怀疑车辆会多出 offsetY，经此测试确认不会）。
// 约定（均有代码依据）：位置文件的 北/东/高 是含 offset 的绝对坐标（runtime-scene.mjs 写运行时位置文件时
//   统一减去 offsetX/Y/Z；scene-loader.js 预览同样减去）；usd-export 写出的 USD y = 位置文件高度（或贴地
//   高度 offsetY + groundedPlacementHeight）+ 放置点地形高度；usd-import 取 y − 地形高度，两者互逆。
// 做法：用盒形基本体（无 OBJ）建车辆与建筑，导出 USDA，解析实例平移，按 manifest 约定交给 buildProjectFromUsd。
// 运行：node --test tools/test-usd-roundtrip-20261125.mjs
// ==========================================================================

import assert from 'node:assert/strict'
import { after, test } from 'node:test'
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { createDefaultProject, groundedPlacementHeight, normalizeProject } from '../src/renderer/src/project-schema.js'
import { exportProjectUsd } from './usd-export.mjs'
import { buildProjectFromUsd } from './usd-import.mjs'

const root = mkdtempSync(join(tmpdir(), 'usd-roundtrip-'))
after(() => rmSync(root, { recursive: true, force: true }))

// 线性地形：高度随北、东坐标增加（本地场景坐标，不含 offset）。
const heightAt = (north, east) => 0.1 * north + 0.05 * east

function makeProject(offsetY) {
  const project = createDefaultProject({ name: 'roundtrip', mode: 'eVoxelRT', sceneX: 100, sceneY: 100, sceneHeight: 50 })
  const configuration = project.configuration
  configuration.scene.terrain = true
  configuration.scene.offsetX = 3
  configuration.scene.offsetY = offsetY
  configuration.scene.offsetZ = 2
  const base = { distribution: {}, meshes: [], dimensions: [4, 1.5, 2] }
  configuration.objects.items = [
    { ...base, name: 'car', type: 'Vehicle', positionFile: 'car.txt' },
    { ...base, name: 'box', type: 'Building', dimensions: [10, 5, 10], positionFile: 'box.txt' }
  ]
  // 位置文件为含 offset 的绝对坐标：车辆 (13, 12) 旋转 30°；建筑 (23, 22) 高度 offsetY + 2。
  writeFileSync(join(root, 'car.txt'), '13 12 0 1 30\n')
  writeFileSync(join(root, 'box.txt'), `23 22 ${offsetY + 2} 1 0\n`)
  return normalizeProject(project)
}

// 解析 usda 中每个实例的平移与 histream:placement* 属性，组装成 usd-import 期望的 manifest。
function manifestFromUsd(text, project) {
  const placements = []
  const pattern = /def Xform "i\d+" \([\s\S]*?\)\s*\{\s*double3 xformOp:translate = \(([^)]*)\)[\s\S]*?histream:placementRotation = ([-\d.e+]+)\s*custom double histream:placementScale = ([-\d.e+]+)/g
  let match
  const instances = []
  while ((match = pattern.exec(text))) {
    const [north, y, east] = match[1].split(',').map(Number)
    instances.push({ north, y, east, rotation: Number(match[2]), scale: Number(match[3]) })
  }
  assert.equal(instances.length, project.configuration.objects.items.length)
  const geometries = project.configuration.objects.items.map((item, index) => ({ id: index, name: item.name, attributes: { objectName: item.name }, meshes: [] }))
  instances.forEach((instance, index) => placements.push({ geometry: index, ...instance }))
  return { geometries, placements, skipped: [] }
}

function runRoundTrip(offsetY) {
  const project = makeProject(offsetY)
  const outputPath = join(root, `scene-${offsetY}.usda`)
  const options = {
    outputPath, assetsDirectory: join(root, 'assets'), resolveAssetPath: (value) => join(root, value), heightAt
  }
  const stats = exportProjectUsd(project, options)
  assert.equal(stats.instances, 2)
  const manifest = manifestFromUsd(readFileSync(outputPath, 'utf8'), project)
  const written = {}
  const imported = buildProjectFromUsd(project, manifest, {
    heightAt, writePositionFile: (name, text) => { written[name] = text; return `${name}.txt` }
  })
  return { project, manifest, written, imported }
}

test('usd round trip keeps vehicle and ordinary object heights when offsetY is not zero', () => {
  for (const offsetY of [0, 7]) {
    const { project, written } = runRoundTrip(offsetY)
    // 普通对象：高度原样回读（位置文件里已含 offsetY）。
    const [north, east, height] = written.box.trim().split(/\s+/).map(Number)
    assert.deepEqual([north, east, height], [23, 22, offsetY + 2], `offsetY=${offsetY} 建筑`)
    // 车辆：高度 = offsetY + 足迹落在地形上的贴地高度（与 runtime-scene.mjs 对贴地对象的覆盖公式相同）。
    const car = written.car.trim().split(/\s+/).map(Number)
    const scene = project.configuration.scene
    const expected = offsetY + groundedPlacementHeight({ minX: -2, maxX: 2, minZ: -1, maxZ: 1 },
      { north: 13 - scene.offsetX, east: 12 - scene.offsetZ, scale: 1, rotation: 30 }, heightAt)
    assert.ok(Math.abs(car[2] - expected) < 1e-5, `offsetY=${offsetY} 车辆高度 ${car[2]} 应为 ${expected}`)
  }
})

test('re-exporting an imported project reproduces the same USD instance heights', () => {
  const first = runRoundTrip(7)
  const outputPath = join(root, 'again.usda')
  // 把回读生成的位置文件落盘后再导出，实例平移应与第一次完全相同。
  writeFileSync(join(root, 'car.txt'), first.written.car)
  writeFileSync(join(root, 'box.txt'), first.written.box)
  exportProjectUsd(first.project, { outputPath, assetsDirectory: join(root, 'assets'), resolveAssetPath: (value) => join(root, value), heightAt })
  const translates = (file) => [...readFileSync(file, 'utf8').matchAll(/xformOp:translate = (\([^)]*\))\n\s+float xformOp:rotateY/g)].map((m) => m[1])
  assert.deepEqual(translates(outputPath), translates(join(root, 'scene-7.usda')))
})
