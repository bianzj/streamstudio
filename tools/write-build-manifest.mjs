// Record the exact source and deployed shader binaries of a completed build.
import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const runtime = path.resolve(root, process.argv[2] || 'models/bin_x64/Release')
const sha256 = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
function inventory(directory, predicate) {
  const files = []
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const file = path.join(directory, entry.name)
    if (entry.isDirectory()) files.push(...inventory(file, predicate))
    else if (entry.isFile() && predicate(file)) files.push(file)
  }
  return files.sort()
}
const sourceFiles = [
  path.join(root, 'models/cmake/copy-runtime-dependencies.cmake'),
  path.join(root, 'models/histream/CMakeLists.txt'),
  ...inventory(path.join(root, 'models/histream/src'), p => /\.(cpp|h|inl)$/.test(p)),
  ...inventory(path.join(root, 'models/histream/shader'), p => /\.(glsl|h|comp|rgen|rchit|rmiss|vert|frag)$/.test(p)),
  path.join(root, 'models/stream/CMakeLists.txt'),
  ...inventory(path.join(root, 'models/stream'), p => /\.(cpp|h|hpp)$/.test(p)),
  path.join(root, 'models/stream/classes/classes.json'),
  ...inventory(path.join(root, 'src/renderer'), p => /\.(js|html|css)$/.test(p)),
  path.join(root, 'server.mjs'),
  ...inventory(path.join(root, 'server'), p => /\.mjs$/.test(p)),
  ...['tools/runtime-scene.mjs', 'tools/surface-textures.mjs', 'tools/png.mjs',
    'tools/tarp-cover.mjs', 'tools/usd-export.mjs', 'tools/usd-import.mjs']
    .map(file => path.join(root, file)),
  path.join(root, 'package.json')
]
const sources = [...new Set(sourceFiles)]
  .map(file => ({ path: path.relative(root, file).replaceAll('\\', '/'), sha256: sha256(file) }))
const shaderBinaries = inventory(path.join(runtime, 'shader'), p => p.endsWith('.spv'))
  .map(file => ({ path: path.relative(runtime, file).replaceAll('\\', '/'), sha256: sha256(file) }))
let commit = '', dirty = null
const git = process.env.GIT_EXECUTABLE || 'git'
try {
  commit = execFileSync(git, ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim()
  dirty = Boolean(execFileSync(git, ['-c', 'core.safecrlf=false', 'status', '--porcelain=v1'], { cwd: root, encoding: 'utf8' }).trim())
} catch { console.warn('write-build-manifest: Git unavailable; set GIT_EXECUTABLE to record commit and dirty state.') }
const engine = path.join(runtime, process.platform === 'win32' ? 'histream.exe' : 'histream')
const runtimeBinaries = ['histream-usd.exe', 'stream.exe', 'stream-defined/classes.json', 'lavapipe/vulkan_lvp.dll', 'lavapipe/lvp_icd.x86_64.json']
  .map(file => path.join(runtime, file))
  .filter(file => fs.existsSync(file))
  .map(file => ({ path: path.relative(runtime, file).replaceAll('\\', '/'), sha256: sha256(file) }))
const manifest = {
  kind: 'stream-studio-build', schemaVersion: 1,
  applicationVersion: JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8')).version,
  createdAt: new Date().toISOString(), commit, dirty,
  sourceTreeSha256: crypto.createHash('sha256').update(JSON.stringify(sources)).digest('hex'),
  engine: { path: path.basename(engine), sha256: sha256(engine) },
  runtimeBinaries, sources, shaderBinaries
}
fs.writeFileSync(path.join(runtime, 'build-manifest.json'), JSON.stringify(manifest, null, 2) + '\n')
console.log(JSON.stringify({ version: manifest.applicationVersion, engineSHA256: manifest.engine.sha256,
  sourceTreeSHA256: manifest.sourceTreeSha256, shaderCount: shaderBinaries.length }))
