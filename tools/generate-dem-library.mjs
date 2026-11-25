// ==========================================================================
// 文件：generate-dem-library.mjs
// 功能：生成 assets/dem-library 下 4 个 65×65 的合成 DEM（平地、中心山丘、盆地、起伏丘陵），
//       格式为 ESRI ASCII Grid（.asc）。
// 主要步骤：
//   1. presets 定义每种地形：高程函数 elevation(x, y)（x、y 为 0-1 归一化坐标）和起伏幅度 relief（m）
//   2. 逐格采样 → 线性归一化到 0..relief（全平时取 0）→ 保留 3 位小数
//   3. 写 NCOLS/NROWS/XLLCORNER=0/YLLCORNER=0/CELLSIZE=1/NODATA_VALUE=-9999 头及数据行
// 约定：高程单位 m；格距 1（单位随工程场景，待核对）；数据行从第 0 行起顺序写出
//   （行顺序与 ASC 标准的“自北向南”是否一致，待核对）。
// ==========================================================================

import { mkdirSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const projectRoot = dirname(dirname(fileURLToPath(import.meta.url)))
const libraryRoot = join(projectRoot, 'assets', 'dem-library')
const width = 65
const height = 65

// 地形预设：高斯山丘/盆地，以及正弦 + 两个高斯峰叠加的丘陵；relief 为归一化后的最大起伏（m）。
const presets = [
  { folder: 'flat', name: 'flat_65x65.asc', relief: 0, elevation: () => 0 },
  {
    folder: 'hill', name: 'central_hill_65x65.asc', relief: 20,
    elevation: (x, y) => Math.exp(-((x - 0.5) ** 2 + (y - 0.5) ** 2) / 0.075)
  },
  {
    folder: 'basin', name: 'basin_65x65.asc', relief: 15,
    elevation: (x, y) => 1 - Math.exp(-((x - 0.5) ** 2 + (y - 0.5) ** 2) / 0.09)
  },
  {
    folder: 'rolling', name: 'rolling_hills_65x65.asc', relief: 24,
    elevation: (x, y) => 0.28 * Math.sin(x * Math.PI * 3.2)
      + 0.22 * Math.cos(y * Math.PI * 4.1)
      + 0.8 * Math.exp(-((x - 0.28) ** 2 + (y - 0.34) ** 2) / 0.035)
      + 0.6 * Math.exp(-((x - 0.72) ** 2 + (y - 0.67) ** 2) / 0.055)
  }
]

// 逐预设生成并写出 .asc 文件。
for (const preset of presets) {
  const raw = []
  for (let row = 0; row < height; row += 1) {
    for (let column = 0; column < width; column += 1) {
      raw.push(preset.elevation(column / (width - 1), row / (height - 1)))
    }
  }
  const minimum = Math.min(...raw)
  const maximum = Math.max(...raw)
  const values = raw.map((value) => maximum === minimum ? 0 : (value - minimum) / (maximum - minimum) * preset.relief)
  const rows = Array.from({ length: height }, (_, row) => values
    .slice(row * width, (row + 1) * width)
    .map((value) => value.toFixed(3))
    .join(' '))
  const content = [
    `NCOLS ${width}`,
    `NROWS ${height}`,
    'XLLCORNER 0',
    'YLLCORNER 0',
    'CELLSIZE 1',
    'NODATA_VALUE -9999',
    ...rows
  ].join('\n') + '\n'
  const outputDir = join(libraryRoot, preset.folder)
  mkdirSync(outputDir, { recursive: true })
  writeFileSync(join(outputDir, preset.name), content, 'utf8')
}

console.log(`Generated ${presets.length} DEM files in ${libraryRoot}`)
