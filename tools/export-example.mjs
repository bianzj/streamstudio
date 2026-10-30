// Copies a repo example to a standalone folder, adding every shared assets/ file it
// references so it runs without Stream Studio's bundled resources.
//   node tools/export-example.mjs <example directory> <target directory>
import { cpSync, existsSync, mkdirSync, readFileSync, statSync } from 'node:fs'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const projectRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const [sourceArg, targetArg] = process.argv.slice(2)
if (!sourceArg || !targetArg) throw new Error('Usage: node tools/export-example.mjs <example directory> <target directory>')
const source = resolve(sourceArg), target = resolve(targetArg)
if (existsSync(target)) throw new Error(`Target already exists: ${target}`)

cpSync(source, target, { recursive: true, filter: (path) => relative(source, path).split(/[\\/]/)[0] !== 'output' })
mkdirSync(join(target, 'output'), { recursive: true })

const c = JSON.parse(readFileSync(join(target, 'project.json'), 'utf8')).configuration
const referenced = new Set([
  c.scene?.demFile, c.meteo?.path, c.atmosphere?.enabled ? c.atmosphere.lutFile : '',
  ...(c.objects?.items || []).flatMap((item) => [item.fileName, item.positionFile]),
  ...(c.spectra || []).flatMap((s) => [s.fileName, s.physicalTexture?.enabled && s.physicalTexture.fileName, s.classTexture?.enabled && s.classTexture.fileName, s.opennessTexture?.enabled && s.opennessTexture.fileName])
].filter(Boolean))
let added = 0, bytes = 0
for (const path of referenced) {
  if (path === 'defined/meteo.txt' || existsSync(join(target, path))) continue
  if (!/^assets[\\/]/.test(path)) throw new Error(`Missing project file: ${path}`)
  mkdirSync(dirname(join(target, path)), { recursive: true })
  cpSync(join(projectRoot, path), join(target, path))
  added += 1
  bytes += statSync(join(target, path)).size
}
const missing = [...referenced].filter((path) => path !== 'defined/meteo.txt' && !existsSync(join(target, path)))
if (missing.length) throw new Error(`Still missing: ${missing.join(', ')}`)
console.log(`Exported ${source} -> ${target}: ${referenced.size} referenced files all local, ${added} shared assets added (${(bytes / 1048576).toFixed(1)} MB)`)
