// ==========================================================================
// 文件：compare-summary.mjs
// 功能：命令行脚本，递归比较两份 JSON（通常是 scene_summary 一类的结果摘要），列出数值差异
// 用法：node review/compare-summary.mjs <新结果.json> <参考.json>
// 主要步骤：
//   1. 读入两份 JSON（第 1 个参数为新结果 A，第 2 个为参考 B）
//   2. walk 递归比较：数值求绝对/相对差，非数值比较文本，类型不同与键缺失单独记录
//   3. 按相对差异从大到小输出前 12 处，并输出文本差异、键缺失、类型差异各前 10 处
// 约定：相对差异以参考值为分母（|参考| ≤ 1e-12 时直接取绝对差）；结果只打印，不设退出码
//   （无论有无差异进程都正常退出）
// ==========================================================================

// 递归比较两份 JSON，输出数值差异（用于新跑结果与随包参考结果对比）
import { readFileSync } from 'node:fs'

const [a, b] = process.argv.slice(2)
const A = JSON.parse(readFileSync(a, 'utf8'))
const B = JSON.parse(readFileSync(b, 'utf8'))

// diffs：数值/文本/类型差异的记录；missing：只在一侧出现的键路径（带说明文字）。
const diffs = []
const missing = []

// 作用：同时遍历新结果 x 与参考 y，路径用 a.b.c 形式（数组下标也按键处理）。
// 说明：数字对数字记为 numeric（abs 为 0 的不记录）；abs 或 rel 非有限（如 NaN）时
// 归为 text。其余标量或 null 不相等时记为 text 或 type（两侧类型是否一致决定）；
// 数组与对象混用记为 type。数组之间按下标键逐项比较，长度不同的部分计入键缺失。
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

// 汇总：只取有限的数值差异并按相对差异降序排序；文本差异和类型差异各自成组。
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
// 最后给出最大相对差异（百分数），无数值差异时为 0。
const maxRel = numeric.length ? Math.max(...numeric.map((d) => d.rel)) : 0
console.log(`\n最大相对差异: ${(maxRel * 100).toFixed(4)}%`)
