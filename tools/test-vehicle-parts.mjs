// Checks of the FacetEB vehicle parts model on the project side (docs/vehicle-thermal.md section 2-4):
// binding table, OBJ group split of the library sedan, validation, XML tags. Run: node tools/test-vehicle-parts.mjs
import { readFileSync } from 'node:fs'
import { createDefaultProject, createVehicleBodyMaterial, projectToXml, splitVehicleObj, validateProject, vehicleMeshBinding } from '../src/renderer/src/project-schema.js'

let failures = 0
const check = (ok, text) => { if (!ok) failures += 1; console.log(`${ok ? 'ok  ' : 'FAIL'} ${text}`) }

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

// 2. OBJ group split: library sedan (forward = tail light -> headlight)
const split = splitVehicleObj(readFileSync(new URL('../assets/obj-library/vehicle/typical/modern_sedan.obj', import.meta.url), 'utf8'))
check(JSON.stringify(split.split.Windows) === '["Windows_front","Windows_side","Windows_rear"]', `glass split ${JSON.stringify(split.counts)}`)
check(split.split.Blue?.includes('Blue_hood'), 'bonnet split from the paint group')
check(Math.abs(split.forward[2] - 1) < 1e-3, `forward axis ${split.forward.map((v) => v.toFixed(3))}`)
const facets = (text) => text.split(/\r?\n/).filter((line) => line.startsWith('f ')).length
check(facets(split.content) === facets(readFileSync(new URL('../assets/obj-library/vehicle/typical/modern_sedan.obj', import.meta.url), 'utf8')), 'facet count preserved')
check(JSON.stringify(splitVehicleObj('g Blue\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n').split) === '{}', 'no light groups = no split')

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
