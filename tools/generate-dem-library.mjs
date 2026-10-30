import { mkdirSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const projectRoot = dirname(dirname(fileURLToPath(import.meta.url)))
const libraryRoot = join(projectRoot, 'assets', 'dem-library')
const width = 65
const height = 65

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
