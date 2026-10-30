import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const library = path.join(root, 'assets', 'obj-library')
const catalog = JSON.parse(fs.readFileSync(path.join(library, 'catalog.json'), 'utf8'))
const failures = []

for (const model of catalog.models) {
  const file = path.join(library, model.file)
  if (!fs.existsSync(file)) {
    failures.push(`${model.id}: file not found: ${model.file}`)
    continue
  }
  let vertices = 0
  let triangles = 0
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    if (line.startsWith('v ')) vertices += 1
    if (line.startsWith('f ')) triangles += Math.max(0, line.trim().split(/\s+/).length - 3)
  }
  if (vertices === 0 || triangles === 0) failures.push(`${model.id}: empty geometry`)
  if (triangles !== model.triangles) failures.push(`${model.id}: catalog=${model.triangles}, OBJ=${triangles} triangles`)
}

for (const [category, defaults] of Object.entries(catalog.defaultSets || {})) {
  const ids = [...(defaults.typical || []), defaults.simple].filter(Boolean)
  for (const id of ids) {
    const model = catalog.models.find((item) => item.id === id)
    if (!model) failures.push(`${category}: unknown default model ${id}`)
    else if (!model.recommended) failures.push(`${category}: default model ${id} is not recommended`)
  }
}

if (failures.length) {
  console.error(failures.join('\n'))
  process.exit(1)
}

console.log(`OBJ asset catalog valid: ${catalog.models.length} models, ${catalog.models.filter((model) => model.recommended).length} recommended`)
