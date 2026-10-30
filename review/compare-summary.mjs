// 递归比较两份 JSON，输出数值差异（用于新跑结果与随包参考结果对比）
import { readFileSync } from 'node:fs'

const [a, b] = process.argv.slice(2)
const A = JSON.parse(readFileSync(a, 'utf8'))
const B = JSON.parse(readFileSync(b, 'utf8'))

const diffs = []
const missing = []

function walk(x, y, path) {
  if (typeof x === 'number' && typeof y === 'number') {
    const abs = Math.abs(x - y)
    const rel = Math.abs(y) > 1e-12 ? abs / Math.abs(y) : abs
    if (abs > 0) diffs.push({ path, old: y, now: x, abs, rel, kind: Number.isFinite(abs) && Number.isFinite(rel) ? 'numeric' : 'text' })
    return
  }
  if (x === null || y === null || typeof x !== 'object' || typeof y !== 'object') {
    if (x !== y) diffs.push({ path, old: y, now: x, kind: typeof x === typeof y && (x === null) === (y === null) ? 'text' : 'type' })
    return
  }
  if (Array.isArray(x) !== Array.isArray(y)) { diffs.push({ path, old: y, now: x, kind: 'type' }); return }
  for (const key of new Set([...Object.keys(x), ...Object.keys(y)])) {
    const sub = path ? `${path}.${key}` : key
    if (!(key in y)) { missing.push(`${sub} (仅新结果有)`); continue }
    if (!(key in x)) { missing.push(`${sub} (仅参考有)`); continue }
    walk(x[key], y[key], sub)
  }
}
walk(A, B, '')

const numeric = diffs.filter((d) => d.kind === 'numeric' && Number.isFinite(d.abs) && Number.isFinite(d.rel))
const text = diffs.filter((d) => d.kind === 'text')
const types = diffs.filter((d) => d.kind === 'type')
numeric.sort((p, q) => q.rel - p.rel)

console.log(`新结果: ${a}`)
console.log(`参  考: ${b}`)
console.log(`数值差异 ${numeric.length} 处，文本差异 ${text.length} 处，类型差异 ${types.length} 处，键缺失 ${missing.length} 处`)
console.log('\n--- 相对差异最大的 12 处 ---')
for (const d of numeric.slice(0, 12)) {
  console.log(`${d.path}\n    参考=${d.old}  新=${d.now}  绝对差=${d.abs.toFixed(6)}  相对差=${(d.rel * 100).toFixed(4)}%`)
}
if (text.length) {
  console.log('\n--- 文本差异（前 10 处）---')
  for (const d of text.slice(0, 10)) console.log(`${d.path}: 参考=${JSON.stringify(d.old)} 新=${JSON.stringify(d.now)}`)
}
if (missing.length) {
  console.log('\n--- 键缺失（前 10 处）---')
  for (const m of missing.slice(0, 10)) console.log(m)
}
if (types.length) {
  console.log('\n--- 类型差异（前 10 处）---')
  for (const d of types.slice(0, 10)) console.log(`${d.path}: 参考=${JSON.stringify(d.old)} 新=${JSON.stringify(d.now)}`)
}
const maxRel = numeric.length ? Math.max(...numeric.map((d) => d.rel)) : 0
console.log(`\n最大相对差异: ${(maxRel * 100).toFixed(4)}%`)
