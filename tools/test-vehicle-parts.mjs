// ==========================================================================
// 文件：test-vehicle-parts.mjs
// 功能：FacetEB 车辆部件模型在工程（项目）侧的检查（docs/vehicle-thermal.md 第 2-4、10 节）：
//   网格名到材料/光谱的自动绑定、车辆 OBJ 的分组拆分、工程校验与 XML 输出标签。
//   无测试框架：自带 check()，失败累计到 failures，结尾以退出码 1 表示失败。
// 主要模块：
//   1. 绑定表：vehicleMeshBinding(组名, 选项) 返回 rule、spectralName、materialName
//   2. OBJ 拆组：splitVehicleObj 把玻璃组拆成前挡/侧窗/后窗、把引擎盖从车漆组拆出，
//      推断车头方向 forward，且面片（f 行）数量保持不变
//   3. 校验与 XML：车辆车身材料 createVehicleBodyMaterial、validateProject、projectToXml 的部件标签
// 关系：被测代码在 src/renderer/src/project-schema.js；样例 OBJ 为 assets/obj-library/vehicle/typical/
//   modern_sedan.obj；引擎侧对应 FacetEB 的部件模型（见 test-vehicle-cabin.py 的数值检查）。
// 运行：node tools/test-vehicle-parts.mjs
// ==========================================================================

// Checks of the FacetEB vehicle parts model on the project side (docs/vehicle-thermal.md section 2-4):
// binding table, OBJ group split of the library sedan, validation, XML tags. Run: node tools/test-vehicle-parts.mjs
import { readFileSync } from 'node:fs'
import { createDefaultProject, createVehicleBodyMaterial, projectToXml, splitVehicleObj, validateProject, vehicleMeshBinding } from '../src/renderer/src/project-schema.js'

let failures = 0
const check = (ok, text) => { if (!ok) failures += 1; console.log(`${ok ? 'ok  ' : 'FAIL'} ${text}`) }

// 检查 1：expectRule(组名, 规则, 光谱名, 材料名) —— 默认车漆为 red；传 undefined 的项不比较。
//   规则按组名关键词匹配，如 Windows_front → 前挡 windshield、Black → 轮胎、
//   Headlights/TailLights → 灯罩、part_N__covered_car → 车罩（tarp，其轮胎部分为 tarpWheel）。
// 1. binding table
const expectRule = (name, rule, spectral, material) => {
  const b = vehicleMeshBinding(name, { paint: 'red' })
  check(b.rule === rule && (spectral === undefined || b.spectralName === spectral) && (material === undefined || b.materialName === material), `${name} -> ${b.rule} ${b.spectralName} ${b.materialName}`)
}
expectRule('Windows_front', 'windshield', 'vehicle_glass_front', 'vehicle_glass_laminated')
expectRule('Windows_side', 'sideWindow', 'vehicle_glass_side', 'vehicle_glass')
expectRule('Windows_rear', 'rearWindow', 'vehicle_glass_rear', 'vehicle_glass')
expectRule('Windows', 'glass', 'vehicle_glass_side', 'vehicle_glass')
expectRule('Black', 'tyre', 'vehicle_tyre_rubber', 'vehicle_tyre')
expectRule('Grey', 'trim', 'vehicle_trim_grey', 'vehicle_trim')
expectRule('Headlights', 'lamp', 'vehicle_lamp_glass', 'vehicle_lamp')
expectRule('TailLights', 'lamp', 'vehicle_lamp_glass', 'vehicle_lamp')
expectRule('White', 'paint', 'vehicle_paint_red', 'vehicle_material')
expectRule('Blue_hood', 'hood', 'vehicle_paint_red', 'vehicle_hood')
expectRule('part_1__covered_car', 'tarp', null, 'vehicle_material')
expectRule('part_2__covered_car', 'tarpWheel', 'vehicle_tyre_rubber', 'vehicle_tyre')
expectRule('chassis', 'paint', 'vehicle_paint_red', 'vehicle_material')
check(vehicleMeshBinding('Windows_front', { film: true }).spectralName === 'vehicle_glass_front_film', 'film swaps the glass spectra')

// 检查 2：对库内轿车 OBJ 拆组。期望：Windows 组拆成前/侧/后三段；Blue 组拆出 Blue_hood（引擎盖）；
//   forward 轴指向 +Z（z 分量约为 1）；拆分前后 f 行数相同；没有灯组时无法定车头，不拆分。
// 2. OBJ group split: library sedan (forward = tail light -> headlight)
const split = splitVehicleObj(readFileSync(new URL('../assets/obj-library/vehicle/typical/modern_sedan.obj', import.meta.url), 'utf8'))
check(JSON.stringify(split.split.Windows) === '["Windows_front","Windows_side","Windows_rear"]', `glass split ${JSON.stringify(split.counts)}`)
check(split.split.Blue?.includes('Blue_hood'), 'bonnet split from the paint group')
check(Math.abs(split.forward[2] - 1) < 1e-3, `forward axis ${split.forward.map((v) => v.toFixed(3))}`)
const facets = (text) => text.split(/\r?\n/).filter((line) => line.startsWith('f ')).length
check(facets(split.content) === facets(readFileSync(new URL('../assets/obj-library/vehicle/typical/modern_sedan.obj', import.meta.url), 'utf8')), 'facet count preserved')
check(JSON.stringify(splitVehicleObj('g Blue\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n').split) === '{}', 'no light groups = no split')

// 检查 3：构造含车辆车身材料的 eFacetEB 工程，校验不应有车辆相关错误；
//   XML 中该材料块应带内饰/发动机部件标签，皮革座椅 vcSeatEffusivity = 450；
//   关闭 interior/engine 的旧式材料不应输出部件标签（保持旧行为）；
//   内饰份额之和不为 1（仪表台 share 改为 0.6）时校验应报"内饰节点"错误。
// 3. validation and XML
const project = createDefaultProject({ mode: 'eFacetEB' })
const body = createVehicleBodyMaterial('t_body', { occupants: 2, speed: 15, heading: 90, windDirection: 270, engineOn: true, acOn: true, seat: 'leather' })
project.configuration.materials.push(body)
check(validateProject(project).errors.filter((e) => /车辆|载具/.test(e)).length === 0, 'valid vehicle body material')
const xml = projectToXml(project)
const at = xml.indexOf('name="t_body"')
const block = xml.slice(at, xml.indexOf('</soilSet>', at))
for (const tag of ['vcIntModel', 'vcIntShares', 'vcIntCapacities', 'vcEngModel', 'vcEngSplit', 'vcFirewallG', 'vcSeatEffusivity']) check(block.includes(`<${tag}>`), `XML tag ${tag}`)
check(/<vcSeatEffusivity>450</.test(block), 'leather seat effusivity')
const old = createVehicleBodyMaterial('t_old', { interior: false, engine: false })
project.configuration.materials.push(old)
const oldBlock = projectToXml(project)
const oldAt = oldBlock.indexOf('name="t_old"')
check(!oldBlock.slice(oldAt, oldBlock.indexOf('</soilSet>', oldAt)).includes('vcIntModel'), 'no interior/engine fields = no parts tags (old behaviour)')
const bad = structuredClone(body); bad.name = 'bad'; bad.params.cabin.interior.dashboard.share = 0.6
project.configuration.materials.push(bad)
check(validateProject(project).errors.some((e) => e.includes('内饰节点')), 'shares not summing to 1 rejected')
process.exit(failures ? 1 : 0)
