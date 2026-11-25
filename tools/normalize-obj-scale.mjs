// ==========================================================================
// 文件：normalize-obj-scale.mjs
// 功能：就地缩放 OBJ 模型并把模型底部对齐到 Y=0，用法：node tools/normalize-obj-scale.mjs <scale> <obj...>
// 主要步骤：
//   1. 解析命令行：scale 必须大于 0，至少一个 OBJ 路径
//   2. 读取每个 OBJ，求所有顶点的最小 Y
//   3. 对 v 行执行：x、z 乘 scale，y 先减最小 Y 再乘 scale；其他行（含 vn）原样保留
// 约定：OBJ 为 Y-up；法线不重算（仅均匀缩放时无需处理）；写回会把换行统一为 \n。
// ==========================================================================

import { readFile, writeFile } from 'node:fs/promises'

const scale = Number(process.argv[2])
const paths = process.argv.slice(3)
if (!(scale > 0) || paths.length === 0) {
  throw new Error('Usage: node tools/normalize-obj-scale.mjs <scale> <obj...>')
}

for (const path of paths) {
  const text = await readFile(path, 'utf8')
  const lines = text.split(/\r?\n/)
  const vertices = lines.map((line) => line.match(/^\s*v\s+([-+\d.eE]+)\s+([-+\d.eE]+)\s+([-+\d.eE]+)/)).filter(Boolean)
  // 用最小 Y 把模型落到地面（底部 Y = 0）。用循环求极值：顶点数极大时 Math.min(...数组) 的参数展开会超出调用栈上限。
  let minimumY = Infinity
  for (const match of vertices) minimumY = Math.min(minimumY, Number(match[2]))
  const output = lines.map((line) => {
    const match = line.match(/^(\s*v\s+)([-+\d.eE]+)\s+([-+\d.eE]+)\s+([-+\d.eE]+)(.*)$/)
    if (!match) return line
    const [, prefix, x, y, z, rest] = match
    return `${prefix}${Number(x) * scale} ${(Number(y) - minimumY) * scale} ${Number(z) * scale}${rest}`
  }).join('\n')
  await writeFile(path, output, 'utf8')
  console.log(path)
}
