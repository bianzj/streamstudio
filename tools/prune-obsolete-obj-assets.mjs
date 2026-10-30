/** Remove superseded historical and cartoon OBJ assets from the library. */

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const library = path.resolve(root, 'assets', 'obj-library')
const obsolete = [
  'vehicle/ambulance.obj',
  'vehicle/city_bus.obj',
  'vehicle/city_suv.obj',
  'vehicle/delivery_van.obj',
  'vehicle/fire_engine.obj',
  'vehicle/refuse_truck.obj',
  'vehicle/sports_hatchback.obj',
  'vehicle/sports_sedan.obj',
  'vehicle/taxi.obj',
  'vehicle/agricultural_tractor.obj',
  'vehicle/kenney_sedan.obj',
  'vehicle/kenney_suv.obj',
  'vehicle/kenney_delivery_van.obj',
  'vehicle/kenney_truck.obj',
  'ship/kenney_cargo_ship.obj',
  'ship/kenney_speed_boat.obj',
  'ship/fishing_boat.obj',
  'ship/sailboat.obj',
  'ship/towboat.obj',
  'ship/tugboat.obj',
  'ship/ocean_liner.obj',
  'ship/realistic/dutch_ship_medium_realistic.obj',
  'ship/realistic/dutch_ship_medium_realistic.json',
  'ship/realistic/ship_pinnace_realistic.obj',
  'ship/realistic/ship_pinnace_realistic.json',
  'ship/realistic/_dutch_ship_medium_realistic_source',
  'ship/realistic/_ship_pinnace_realistic_source',
]

for (const relative of obsolete) {
  const target = path.resolve(library, relative)
  if (!target.startsWith(`${library}${path.sep}`)) throw new Error(`Unsafe asset path: ${target}`)
  if (!fs.existsSync(target)) continue
  fs.rmSync(target, { recursive: true, force: true })
  console.log(`removed ${relative}`)
}
