import { execFileSync } from 'node:child_process'
import {
  existsSync, mkdirSync, readFileSync, rmSync, writeFileSync
} from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const argument = (name, fallback) => {
  const index = process.argv.indexOf(`--${name}`)
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback
}

const modtranRoot = resolve(argument('root', process.env.MODTRAN_ROOT || 'D:\\work\\MODTRAN5.2.1'))
const executable = join(modtranRoot, 'Mod5.2.1cons.exe')
const rootFile = join(modtranRoot, 'mod5root.in')
const outputFile = resolve(argument('output', join(repoRoot, 'assets', 'atmosphere', 'simple_modtran_lut.csv')))
const workDirectory = join(modtranRoot, 'LUT_STREAM_STUDIO')
const atmosphereAxis = [
  { id: 'tropical', code: 1 },
  { id: 'midlatitude-summer', code: 2 },
  { id: 'midlatitude-winter', code: 3 }
]
const aerosolAxis = [
  { id: 'rural', code: 2 },
  { id: 'urban', code: 5 }
]
const waterVaporAxis = [0.5, 2, 5]
const visibilityAxis = [10, 23, 50]
const sensorAltitudeAxis = [1, 3, 10]
const viewZenithAxis = [0, 30, 60]

if (!existsSync(executable)) throw new Error(`找不到 MODTRAN 命令行程序：${executable}`)
if (!existsSync(join(modtranRoot, 'DATA'))) throw new Error(`找不到 MODTRAN DATA 目录：${modtranRoot}`)

function fixed(value, width, digits = 3) {
  return Number(value).toFixed(digits).padStart(width)
}

function inputText({ atmosphereCode, aerosolCode, waterVapor, visibility, sensorAltitude, viewZenith }) {
  // Thermal-emission path mode also outputs total path transmittance.  This
  // gives one consistent spectrum for both optical attenuation and TIR path
  // emission without exposing MODTRAN's fixed-width cards to the UI.
  const pathAngle = 180 - viewZenith
  const waterColumn = `G${Number(waterVapor).toFixed(7)}`
  return [
    `M   ${atmosphereCode}    2    1    0    0    0    0    0    0    0    0    0   -1    .000    .00`,
    `f   8    0  380.000 ${waterColumn}  1.00000 3f 4 f`,
    '01_2009',
    ' 1.00 1.00 1.05 1.00 1.00 1.00 1.00 1.00 1.00 1.00                              !CARD 1A5',
    ' 1.85 2.25 1.00 1.00 2.75 4.00 1.00 1.00 1.00 1.00 0.73 1.00 1.00               !CARD 1A6',
    `${String(aerosolCode).padStart(5)}    0    0    0    0    0${fixed(visibility, 10)}${fixed(0, 10)}${fixed(0, 10)}${fixed(0, 10)}${fixed(0, 10)}`,
    `${fixed(sensorAltitude, 10)}${fixed(0, 10)}${fixed(pathAngle, 10)}${fixed(0, 10)}${fixed(0, 10)}${fixed(0, 10)}    0        0.00000`,
    '       715     28571        15        15rn        w1aa',
    '    0',
    ''
  ].join('\r\n')
}

function parseTp7(path) {
  const rawRows = []
  let inTable = false
  for (const line of readFileSync(path, 'utf8').split(/\r?\n/)) {
    if (/\bFREQ\b.*\bTOT_TRANS\b.*\bPTH_THRML\b/.test(line)) {
      inTable = true
      continue
    }
    if (!inTable) continue
    const values = line.trim().split(/\s+/).map(Number)
    if (values.length < 4 || !values.slice(0, 4).every(Number.isFinite)) continue
    const wavenumber = values[0]
    if (wavenumber < 100 || wavenumber > 100000) continue
    const wavelengthNm = 1e7 / wavenumber
    const wavelengthUm = wavelengthNm / 1000
    // MODTRAN tp7 radiance is W cm^-2 sr^-1 (cm^-1)^-1. Convert to
    // W m^-2 sr^-1 um^-1: 1e4 for area and 1e4/lambda_um^2 for axis.
    const pathRadiance = Math.max(0, values[2] + values[3]) * 1e8 / (wavelengthUm * wavelengthUm)
    rawRows.push({
      wavelengthNm,
      transmittance: Math.max(0, Math.min(1, values[1])),
      pathRadiance
    })
  }
  rawRows.sort((left, right) => left.wavelengthNm - right.wavelengthNm)
  if (rawRows.length < 100) throw new Error(`MODTRAN 输出光谱不完整：${path}`)

  const wavelengths = []
  for (let wavelength = 350; wavelength <= 2500; wavelength += 5) wavelengths.push(wavelength)
  for (let wavelength = 2525; wavelength <= 14000; wavelength += 25) wavelengths.push(wavelength)
  let right = 1
  return wavelengths.map((wavelengthNm) => {
    while (right < rawRows.length - 1 && rawRows[right].wavelengthNm < wavelengthNm) right += 1
    const lower = rawRows[Math.max(0, right - 1)]
    const upper = rawRows[Math.min(rawRows.length - 1, right)]
    const span = upper.wavelengthNm - lower.wavelengthNm
    const weight = span > 0 ? Math.max(0, Math.min(1, (wavelengthNm - lower.wavelengthNm) / span)) : 0
    return {
      wavelengthNm,
      transmittance: lower.transmittance + (upper.transmittance - lower.transmittance) * weight,
      pathRadiance: lower.pathRadiance + (upper.pathRadiance - lower.pathRadiance) * weight
    }
  })
}

mkdirSync(workDirectory, { recursive: true })
mkdirSync(dirname(outputFile), { recursive: true })
const originalRootFile = existsSync(rootFile) ? readFileSync(rootFile) : null
const lines = [
  '# Stream Studio MODTRAN 5.2.1 sparse atmosphere LUT v2',
  '# columns: atmosphere_model,aerosol_model,water_vapor_cm,visibility_km,sensor_altitude_km,view_zenith_deg,wavelength_nm,transmittance,path_radiance_W_m-2_sr-1_um-1',
  'atmosphere_model,aerosol_model,water_vapor_cm,visibility_km,sensor_altitude_km,view_zenith_deg,wavelength_nm,transmittance,path_radiance'
]

try {
  let runIndex = 0
  const runCount = atmosphereAxis.length * aerosolAxis.length * waterVaporAxis.length
    * visibilityAxis.length * sensorAltitudeAxis.length * viewZenithAxis.length
  for (const atmosphere of atmosphereAxis) {
    for (const aerosol of aerosolAxis) {
      for (const waterVapor of waterVaporAxis) {
        for (const visibility of visibilityAxis) {
          for (const sensorAltitude of sensorAltitudeAxis) {
            for (const viewZenith of viewZenithAxis) {
              const name = `s${String(runIndex).padStart(3, '0')}`
              const relativeRoot = `LUT_STREAM_STUDIO/${name}`
              const caseRoot = join(workDirectory, name)
              writeFileSync(`${caseRoot}.tp5`, inputText({
                atmosphereCode: atmosphere.code,
                aerosolCode: aerosol.code,
                waterVapor,
                visibility,
                sensorAltitude,
                viewZenith
              }), 'ascii')
              writeFileSync(rootFile, `${relativeRoot}\r\n`, 'ascii')
              process.stdout.write(`[${runIndex + 1}/${runCount}] ATM=${atmosphere.id} AER=${aerosol.id} PWV=${waterVapor} cm VIS=${visibility} km H=${sensorAltitude} km VZA=${viewZenith}° ... `)
              execFileSync(executable, { cwd: modtranRoot, windowsHide: true, stdio: 'pipe' })
              const rows = parseTp7(`${caseRoot}.tp7`)
              for (const row of rows) {
                lines.push([
                  atmosphere.id, aerosol.id, waterVapor,
                  visibility, sensorAltitude, viewZenith,
                  row.wavelengthNm.toFixed(6),
                  row.transmittance.toFixed(8),
                  row.pathRadiance.toExponential(8)
                ].join(','))
              }
              process.stdout.write(`${rows.length} wavelengths\n`)
              runIndex += 1
            }
          }
        }
      }
    }
  }
  writeFileSync(outputFile, `${lines.join('\n')}\n`, 'utf8')
  console.log(`LUT written: ${outputFile}`)
} finally {
  if (originalRootFile == null) rmSync(rootFile, { force: true })
  else writeFileSync(rootFile, originalRootFile)
}
