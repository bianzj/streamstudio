import { webApi } from './web-api.js'
import { appStorage } from './storage.js'
import { getLocale, startLocalization, t } from './i18n.js'
import { invalidateImageTextures, loadXmlScene, updateSceneDynamics } from './scene-loader.js'
import { createDefaultProject, createMaterialPresets, DEFAULT_CPU_THREADS, groundedPlacementHeight, MAX_SENSOR_BANDS, normalizeCpuThreads, normalizeTerrainEnvironment, normalizeProject, parseProjectJson, sensorBandValues, sensorCruisePositions, sensorViewAngles, simplifySolarSpectra, stringifyProject } from './project-schema.js'
import { createStreamWorkspace } from './stream/workspace.js'
import { createMenubar } from './menubar.js'
import * as THREE from 'three'
import { specularParametersFromValues, specularParameterFields, normalizeSpecular, meshSpecular, meshSpecularFields, meshSpecularFromRow, meshSpecularRunError } from './material-specular.js'
import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
import { TransformControls } from 'three/addons/controls/TransformControls.js'
import { OBJLoader } from 'three/addons/loaders/OBJLoader.js'
import './styles.css'

const $ = (selector) => document.querySelector(selector)
const $$ = (selector) => [...document.querySelectorAll(selector)]
const api = webApi
const histreamProxyState = { info: null, result: null }

const PHYSICAL_TEXTURE_PRESETS = [
  ['assets/texture-library/concrete_variance.jpg', '混凝土 · Concrete034'],
  ['assets/texture-library/brick_variance.jpg', '砖墙 · Bricks075A'],
  ['assets/texture-library/asphalt_variance.jpg', '沥青 · Asphalt031'],
  ['assets/texture-library/metal_variance.jpg', '金属 · Metal046B'],
  ['assets/texture-library/canvas_weave_variance.jpg', '帆布织纹 · 程序生成'],
  ['assets/texture-library/camouflage_woodland_variance.jpg', '林地迷彩 · 程序生成'],
  ['assets/texture-library/camouflage_desert_variance.jpg', '荒漠迷彩 · 程序生成'],
  ['assets/texture-library/camouflage_digital_variance.jpg', '数码迷彩 · 程序生成'],
  ['assets/texture-library/stripes_variance.jpg', '遮阳条纹 · 程序生成'],
  ['assets/texture-library/crumpled_film_variance.jpg', '褶皱反光膜 · 程序生成'],
  ['assets/texture-library/sand_mottling_variance.jpg', '沙面风纹 · 程序生成'],
  ['assets/texture-library/soil_mottling_variance.jpg', '裸土斑块 · 程序生成']
]

const CLASS_TEXTURE_PRESETS = [
  ['assets/texture-library/class_woodland_4.png', '林地迷彩 · 4 类'],
  ['assets/texture-library/class_digital_4.png', '数码迷彩 · 4 类'],
  ['assets/texture-library/class_desert_3.png', '荒漠迷彩 · 3 类']
]

const OPENNESS_TEXTURE_PRESETS = [
  ['assets/texture-library/openness_damaged_net.png', '遮阳网破损'],
  ['assets/texture-library/openness_weave_density.png', '织物疏密不均']
]

const PREVIEW_TEXTURE_PRESETS = [
  ['solar-panel', '太阳能电池板'],
  ['red-brick', '红砖墙'],
  ['gravel', '石子路 / 粗粒路面'],
  ['concrete', '混凝土墙'],
  ['plaster', '浅色粉刷墙'],
  ['stone-wall', '自然石墙'],
  ['roof-tile', '红瓦屋面'],
  ['metal', '金属表面'],
  ['wood', '木材'],
  ['grass', '草地'],
  ['canvas', '帆布 / 篷布'],
  ['camouflage-woodland', '林地迷彩'],
  ['camouflage-desert', '荒漠迷彩'],
  ['camouflage-digital', '数码迷彩'],
  ['stripes', '遮阳条纹'],
  ['shade-net', '遮阳网']
]

const state = {
  mode: 'eVoxelEB',
  executable: '',
  executableExists: true,
  defaultExecutable: '',
  defaultExecutableExists: false,
  inputPath: '',
  outputDir: '',
  xmlText: '',
  xmlDirty: false,
  editRevision: 0,
  projectGeneration: 0,
  xmlManualEdit: false,
  runPending: false,
  lastSaveError: null,
  project: null,
  projectPath: '',
  running: false,
  startedAt: 0,
  progress: 0,
  progressStage: '待命',
  logCount: 0,
  panel: 'scene',
  config: null,
  pendingObject: null,
  drawPlacement: null,
  editingObjectIndex: -1,
  selectedObjectIndex: -1,
  editingAttributeIndex: -1,
  editingPreset: null,
  presetPhysicalType: 'Vegetation',
  resultFiles: [],
  selectedResult: null,
  resultRaster: null,
  resultView: 'image',
  analysisType: 'angle',
  analysisSelection: {},
  changeAnalysisRequest: 0,
  resultDisplay: {},
  resultPreviewRequest: 0,
  resultOpenRequest: 0
}

function mergeNamedPresets(presets, values) {
  const merged = new Map(presets.map((item) => [item.name, item]))
  for (const item of values || []) {
    const preset = merged.get(item.name) || {}
    merged.set(item.name, { ...preset, ...item, params: { ...(preset.params || {}), ...(item.params || {}) }, previewTexture: { ...(preset.previewTexture || {}), ...(item.previewTexture || {}) }, physicalTexture: { ...(preset.physicalTexture || {}), ...(item.physicalTexture || {}) } })
  }
  return [...merged.values()]
}

function structureLabel(value) {
  return String(value || '')
    .replace(/Canopy\s*冠层/gi, '混沌介质')
    .replace(/Canopy/gi, '混沌介质')
    .replace(/冠层/g, '混沌介质')
}

function ensureMaterialPresets(config) {
  const presets = createMaterialPresets()
  config.spectra = simplifySolarSpectra(mergeNamedPresets(presets.spectra, config.spectra), config)
    .map((item) => {
    item = { ...item, label: String(item.label || '').replaceAll('火焰/烟羽', '火焰') }
    if (item.name === 'water_surface') item.label = '水体光谱'
    if (item.model !== 'BSM') return item
    const params = item.params || {}
    const latitude = Number(params.BSMlat)
    const longitude = Number(params.BSMlon)
    if (Number.isFinite(latitude) && Number.isFinite(longitude)
      && Math.abs(latitude) < 1e-9 && Math.abs(longitude) < 1e-9) {
      return { ...item, params: { ...params, BSMlat: 25, BSMlon: 45 } }
    }
    return item
  })
  config.canopies = mergeNamedPresets(presets.canopies, config.canopies)
    .map((item) => ({ ...item, label: structureLabel(String(item.label || '').replaceAll('火焰/烟羽', '火焰')), structureType: ['canopy', 'rigid', 'fire', 'fog'].includes(item.structureType) ? item.structureType : 'canopy' }))
  config.thermals = mergeNamedPresets(presets.thermals, config.thermals)
    .map((item) => ({ ...item, label: item.name === 'water_temperature' ? '水体初始温度' : String(item.label || '').replaceAll('火焰/烟羽', '火焰') }))
  config.objects.items = config.objects.items || []
  config.objects.count = config.objects.items.length
  config.objects.names = config.objects.items.map((item) => item.name)
  config.materials = mergeNamedPresets(presets.materials, config.materials)
    .map((item) => ({ ...item, label: item.name === 'water_set' ? '水体物化参数' : item.label }))
  const soilMethod = Math.max(0, Math.min(2, Math.round(Number(config.control?.soilTemperatureMethod ?? 2))))
  config.materials.filter((item) => item.type === 'Soil').forEach((item) => { item.params.method = soilMethod })
  return config
}

const defaultConfig = () => {
  const config = createDefaultProject().configuration
  config.meteo.path = 'HiStream 内置气象数据'
  return config
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>'"]/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[char])
}

function xmlValue(doc, selector, fallback = '') {
  return doc?.querySelector(selector)?.textContent?.trim() || fallback
}

function parsePair(value, fallback) {
  const parts = String(value).split(',').map(Number)
  return parts.length >= 2 && parts.every(Number.isFinite) ? parts : fallback
}

function parseTriple(value, fallback) {
  const parts = String(value).split(',').map(Number)
  return parts.length >= 3 && parts.slice(0, 3).every(Number.isFinite) ? parts.slice(0, 3) : fallback
}

function projectModeFromXml(content, fallback = state.mode) {
  try {
    const project = parseProjectJson(content)
    return project.mode
  } catch {}
  const doc = new DOMParser().parseFromString(String(content || ''), 'application/xml')
  const mode = doc.documentElement?.getAttribute('mode')
  return ['eVoxelEB', 'eFacetEB', 'eFacetRT', 'eVoxelRT', 'eRaytracing'].includes(mode) ? mode : fallback
}

function parseMaterialNode(node) {
  const type = node.tagName === 'leafBio' ? 'Vegetation' : node.tagName === 'waterSet' ? 'Water' : 'Soil'
  let params
  if (type === 'Vegetation') {
    params = {
      Vcmax: Number(xmlValue(node, 'Vcmax', 80)), m: Number(xmlValue(node, 'm', 9)),
      BallBerry: Number(xmlValue(node, 'BallBerry', 0.01)), Type: Number(xmlValue(node, 'Type', 3)),
      kV: Number(xmlValue(node, 'kV', 0.6396)), Rdparam: Number(xmlValue(node, 'Rdparam', 0.015)),
      Tparam: xmlValue(node, 'Tparam', '0.2,0.3,288,313,328'), Tyear: Number(xmlValue(node, 'Tyear', 25)),
      beta: Number(xmlValue(node, 'beta', 0.507)), kNPQs: Number(xmlValue(node, 'kNPQs', 0)),
      qLs: Number(xmlValue(node, 'qLs', 1)), stressfactor: Number(xmlValue(node, 'stressfactor', 1)), Tcor: Number(xmlValue(node, 'Tcor', 0))
    }
  } else if (type === 'Water') {
    params = {
      rss: Number(xmlValue(node, 'rss', 0)), heatCapacity: Number(xmlValue(node, 'heatCapacity', 4186000)),
      mixingDepth: Number(xmlValue(node, 'mixingDepth', 0.5)), evaporationCoefficient: Number(xmlValue(node, 'evaporationCoefficient', 1)),
      brdfModel: ['1', 'coxmunk', 'cox-munk'].includes(xmlValue(node, 'brdfModel', 'CoxMunk').toLowerCase()) ? 1 : 0,
      refractiveIndex: Number(xmlValue(node, 'refractiveIndex', 1.333)), slopeVariance: Number(xmlValue(node, 'slopeVariance', 0)),
      diffuseFraction: Number(xmlValue(node, 'diffuseFraction', 0.02))
    }
  } else {
    params = {
      method: Number(xmlValue(node, 'method', 2)), rss: Number(xmlValue(node, 'rss', 2000)),
      cs: Number(xmlValue(node, 'cs', 1180)), rhos: Number(xmlValue(node, 'rhos', 1800)),
      lambdas: Number(xmlValue(node, 'lambdas', 1.55)), Tsoil: Number(xmlValue(node, 'Tsoil', 25)),
      SMC: Number(xmlValue(node, 'SMC', 25)), Satwater: Number(xmlValue(node, 'Satwater', 0.45))
    }
  }
  return { name: node.getAttribute('name') || 'material', type, params }
}

function parseXml(content, mode = state.mode) {
  if (String(content || '').trimStart().startsWith('{')) return parseProjectJson(content).configuration
  const doc = new DOMParser().parseFromString(content, 'application/xml')
  if (doc.querySelector('parsererror')) throw new Error('XML 格式错误，请检查标签是否闭合')
  const base = defaultConfig()
  const energyMode = mode === 'eFacetEB' || mode === 'eVoxelEB'
  const lightPair = parsePair(xmlValue(doc, 'Geometry Light light lightAngle lightAngle', '30,135'), [30, 135])
  const samplingNode = doc.querySelector('Geometry Sensor sensor observationSampling')
  const continuousNode = doc.querySelector('Geometry Sensor sensor continuousBand')
  const generatedViewPair = parsePair(xmlValue(doc, 'Geometry Sensor sensor viewAngle viewAngle[type="custom"] viewAngles', '0,0'), [0, 0])
  const viewPair = [
    Number(samplingNode?.getAttribute('customVza') ?? generatedViewPair[0]),
    Number(samplingNode?.getAttribute('customVaa') ?? generatedViewPair[1])
  ]
  const sceneX = Number(xmlValue(doc, 'Scene sceneSizeX', base.scene.x))
  const sceneY = Number(xmlValue(doc, 'Scene sceneSizeY', base.scene.y))
  const projectionText = xmlValue(doc, 'Geometry Sensor sensor projection', 'parallel').toLowerCase()
  const sensorPosition = parseTriple(
    xmlValue(doc, 'Geometry Sensor sensor sensorPosition', ''),
    [sceneX / 2, sceneY / 2, 3000]
  )
  const cruiseNode = doc.querySelector('Geometry Sensor sensor uavPositions')
  // Older exports omitted the heading attribute and wrote customVaa as the
  // effective azimuth (first cruise heading + relative VAA + 180).
  if (cruiseNode && !cruiseNode.hasAttribute('heading') && samplingNode?.hasAttribute('customVaa')) {
    const firstHeading = Number(cruiseNode.querySelector('position')?.getAttribute('heading'))
    if (Number.isFinite(firstHeading) && Number.isFinite(viewPair[1])) viewPair[1] = (((viewPair[1] - firstHeading - 180) % 360) + 360) % 360
  }
  const directFraction = Math.max(0, Math.min(1, finiteNumber(xmlValue(
    doc, 'Geometry Light light directScatteringRatio',
    xmlValue(doc, 'Geometry Light light direct', base.light.direct)
  ), base.light.direct)))
  const demFile = xmlValue(doc, 'Scene DEM', '')
  const demSwitchNode = doc.querySelector('Control isDEM')
  const terrainEnabled = demSwitchNode
    ? String(demSwitchNode.textContent || '').trim() === '1'
    : Boolean(demFile)
  const environmentNode = doc.querySelector('Scene TerrainEnvironment')
  const terrainEnvironment = normalizeTerrainEnvironment(Object.fromEntries(
    Object.keys(base.scene.terrainEnvironment).filter((key) => environmentNode?.hasAttribute(key))
      .map((key) => [key, environmentNode.getAttribute(key)])
  ))
  const objectNodes = [...doc.querySelectorAll('Scene Object object')]
  const names = objectNodes.map((node) => node.getAttribute('objName') || node.getAttribute('name') || `对象 ${node.getAttribute('id') || ''}`)
  const items = objectNodes.map((node) => {
    const mediumKind = xmlValue(node, 'mediumKind', '')
    const type = mediumKind === 'fire' ? 'Fire' : mediumKind === 'fog' ? 'Fog' : xmlValue(node, 'types', 'Other')
    const meshNames = xmlValue(node, 'meshNames', node.getAttribute('objName') || 'object').split(',')
    const spectralNames = xmlValue(node, 'spectralNames', 'soil').split(',')
    const meshSpecularEnabled = xmlValue(node, 'meshSpecularEnabled', '').split(',')
    const thermalNames = xmlValue(node, 'thermalNames', 'soil_temperature').split(',')
    const defaultMaterial = type === 'Vegetation' ? 'leaf_c3' : type === 'Water' ? 'water_set' : 'soilset'
    const materialNames = xmlValue(node, 'bioNames', xmlValue(node, 'propNames', defaultMaterial)).split(',')
    const canopyNames = xmlValue(node, 'canopyNames', type === 'Vegetation' ? 'canopy_default' : 'rigid_body').split(',')
    return {
      name: node.getAttribute('objName') || node.getAttribute('name') || 'object', type,
      shape: xmlValue(node, 'shapeTypes', ''), dimensions: xmlValue(node, 'shapes', '1,1,1').split(',').map(Number),
      fileName: xmlValue(node, 'fileName', xmlValue(node, 'objectfile', '')), positionFile: xmlValue(node, 'objectPosition', ''),
      materialName: materialNames[0] || defaultMaterial,
      canopyName: canopyNames[0] || 'canopy_default',
      meshes: meshNames.map((name, index) => ({
        name,
        ...(meshSpecularEnabled[index] !== undefined && meshSpecularEnabled[index] !== '' ? { specularEnabled: normalizeSpecular({ enabled: meshSpecularEnabled[index] }).enabled } : {}),
        spectralName: spectralNames[index] || spectralNames[0] || 'soil',
        thermalName: thermalNames[index] || thermalNames[0] || 'soil_temperature',
        materialName: materialNames[index] || materialNames[0] || defaultMaterial,
        canopyName: canopyNames[index] || canopyNames[0] || 'canopy_default'
      })),
      ...(mediumKind ? { medium: { kind: mediumKind, radiationOnly: true, coordinateOrder: 'XYZ' }, sourceKind: 'prim', generation: { type: mediumKind === 'fog' ? '雾' : '火焰', shape: xmlValue(node, 'shapeTypes', 'cube') } } : {})
    }
  })
  const spectra = [...doc.querySelectorAll('Attribute Spectral spectral')].map((node) => ({
    name: node.getAttribute('name') || 'spectral', model: node.getAttribute('type') || 'custom',
    reflectance: xmlValue(node, 'reflectance', '0.20'), transmittance: xmlValue(node, 'transmittance', '0.0'),
    refTir: Number(xmlValue(node, 'ref_TIR', 0.05)), tauTir: Number(xmlValue(node, 'tau_TIR', 0)), fileName: xmlValue(node, 'spectral_file', ''),
    refTirWet: xmlValue(node, 'ref_TIR_wet', '') === '' ? undefined : Number(xmlValue(node, 'ref_TIR_wet')),
    specular: normalizeSpecular({ enabled: xmlValue(node, 'specularEnabled', '0'), fraction: xmlValue(node, 'specularFraction', 0.5), roughness: xmlValue(node, 'specularRoughness', 0.25) }),
    physicalTexture: {
      enabled: ['1', 'true', 'yes', 'on'].includes(xmlValue(node, 'physicalTextureEnabled', '0').toLowerCase()),
      fileName: xmlValue(node, 'physicalTextureFile', ''),
      strength: Number(xmlValue(node, 'physicalTextureStrength', 0.2)),
      repeatSize: Number(xmlValue(node, 'physicalTextureRepeatSize', 2))
    },
    params: { Cab: Number(xmlValue(node, 'Cab', 40)), Cw: Number(xmlValue(node, 'Cw', 0.01)), Cdm: Number(xmlValue(node, 'Cdm', 0.01)), Cs: Number(xmlValue(node, 'Cs', 0)), N: Number(xmlValue(node, 'N', 1.5)), SMC: Number(xmlValue(node, 'SMC', 25)), BSMBrightness: Number(xmlValue(node, 'BSMBrightness', 0.5)), BSMlat: Number(xmlValue(node, 'BSMlat', 25)), BSMlon: Number(xmlValue(node, 'BSMlon', 45)) }
  }))
  const thermals = [...doc.querySelectorAll('Attribute Thermal thermal')].map((node) => ({ name: node.getAttribute('name') || 'temperature', sunlitTemperature: Number(xmlValue(node, 'sunlitTemperature', 305)), shadedTemperature: Number(xmlValue(node, 'shadedTemperature', 295)) }))
  const materials = [...doc.querySelectorAll('Attribute Biochemistry leafBio, Attribute Biochemistry soilSet, Attribute Biochemistry waterSet')]
    .filter((node) => node.getAttribute('name') !== 'unused_leaf')
    .map(parseMaterialNode)
  const canopies = [...doc.querySelectorAll('Attribute Canopy canopy')].map((node) => ({
    name: node.getAttribute('name') || 'canopy_default',
    structureType: (() => {
      const value = xmlValue(node, 'structureType', 'canopy').toLowerCase()
      return value === '1' || value === 'rigid' ? 'rigid' : value === '2' || value === 'fire' ? 'fire' : value === '3' || value === 'fog' ? 'fog' : 'canopy'
    })(),
    lai: Number(xmlValue(node, 'lai', 3)), density: Number(xmlValue(node, 'density', 1)),
    hc: Number(xmlValue(node, 'hc', 2)), G: Number(xmlValue(node, 'G', 0.5)),
    LIDFa: Number(xmlValue(node, 'LIDFa', -0.35)), LIDFb: Number(xmlValue(node, 'LIDFb', -0.15)),
    hspot: Number(xmlValue(node, 'hspot', 0.2)), leafwidth: Number(xmlValue(node, 'leafwidth', 0.1)),
    extinction: Number(xmlValue(node, 'extinction', 0)), scatteringAlbedo: Number(xmlValue(node, 'scatteringAlbedo', 0)),
    asymmetry: Number(xmlValue(node, 'asymmetry', 0)), emissionScale: Number(xmlValue(node, 'emissionScale', 0)),
    fixedTemperature: Number(xmlValue(node, 'fixedTemperature', 0))
  }))
  return {
    outDir: xmlValue(doc, 'Control outDir', ''),
    scene: {
      x: sceneX,
      y: sceneY,
      height: Number(xmlValue(doc, 'Scene Height', base.scene.height)),
      offsetX: Number(xmlValue(doc, 'Scene offsetX', base.scene.offsetX)),
      offsetY: Number(xmlValue(doc, 'Scene offsetY', base.scene.offsetY)),
      offsetZ: Number(xmlValue(doc, 'Scene offsetZ', base.scene.offsetZ)),
      voxel: Number(xmlValue(doc, 'Scene voxelSize', base.scene.voxel)),
      voxelFillThreshold: Number(xmlValue(doc, 'Scene voxelFillThreshold', base.scene.voxelFillThreshold)),
      terrain: terrainEnabled,
      terrainEnvironment,
      demFile,
      demInfo: null,
      background: {
        spectralName: xmlValue(doc, 'Scene bgSpectral', base.scene.background.spectralName),
        ...(doc.querySelector('Scene backgroundSpecularEnabled') ? {
          specularEnabled: normalizeSpecular({ enabled: xmlValue(doc, 'Scene backgroundSpecularEnabled', '0') }).enabled
        } : {}),
        thermalName: xmlValue(doc, 'Scene bgThermal', base.scene.background.thermalName),
        materialType: xmlValue(doc, 'Scene bgBioType', base.scene.background.materialType),
        materialName: xmlValue(doc, 'Scene bgBioName', base.scene.background.materialName),
        heterogeneityEnabled: xmlValue(doc, 'Scene backgroundHeterogeneity', 'None').toLowerCase() === 'hapke',
        angularEffectStrength: Math.max(0, Math.min(1, Number(xmlValue(
          doc, 'Scene backgroundAngularStrength', base.scene.background.angularEffectStrength
        ))))
      }
    },
    light: {
      zenith: lightPair[0], azimuth: lightPair[1],
      direct: directFraction,
      diffuse: 1 - directFraction,
      skyTemperature: Number(xmlValue(doc, 'Geometry Light light skyTemperature', base.light.skyTemperature))
    },
    sensor: {
      x: Number(xmlValue(doc, 'Geometry Sensor sensor pixelResolutionX', base.sensor.x)),
      y: Number(xmlValue(doc, 'Geometry Sensor sensor pixelResolutionY', base.sensor.y)),
      bands: xmlValue(continuousNode, 'customBands', xmlValue(doc, 'Geometry Sensor sensor controlBand', base.sensor.bands)),
      continuousBands: continuousNode?.getAttribute('enabled') === '1',
      bandStart: Number(xmlValue(continuousNode, 'start', base.sensor.bandStart)),
      bandEnd: Number(xmlValue(continuousNode, 'end', base.sensor.bandEnd)),
      bandStep: Number(xmlValue(continuousNode, 'step', base.sensor.bandStep)),
      principalPlane: samplingNode?.getAttribute('principalPlane') === '1',
      hemisphere: samplingNode?.getAttribute('hemisphere') === '1',
      projection: ['perspective', 'central', 'center'].includes(projectionText) ? 'perspective' : 'parallel',
      vza: viewPair[0], vaa: viewPair[1],
      positionX: sensorPosition[0], positionY: sensorPosition[1], height: sensorPosition[2],
      fov: Number(xmlValue(doc, 'Geometry Sensor sensor FOV', base.sensor.fov)),
      cruiseEnabled: projectionText === 'perspective' && xmlValue(doc, 'Control isUAVtrave', '0') === '1',
      cruiseHeading: Number(cruiseNode?.getAttribute('heading') ?? base.sensor.cruiseHeading),
      cruiseRoute: cruiseNode?.getAttribute('route') || base.sensor.cruiseRoute,
      cruiseTargetX: Number(cruiseNode?.getAttribute('targetX') ?? base.sensor.cruiseTargetX),
      cruiseTargetY: Number(cruiseNode?.getAttribute('targetY') ?? base.sensor.cruiseTargetY),
      cruiseTargetZ: Number(cruiseNode?.getAttribute('targetZ') ?? sensorPosition[2]),
      cruiseStep: Number(cruiseNode?.getAttribute('step') ?? base.sensor.cruiseStep),
      cruiseCount: Number(cruiseNode?.getAttribute('count') ?? cruiseNode?.querySelectorAll('position').length ?? base.sensor.cruiseCount),
      image: xmlValue(doc, 'Control isImage', '1') === '1',
      temperature: xmlValue(doc, 'Control isTemperature', '1') === '1',
      process: xmlValue(doc, 'Control isProcess', '0') === '1',
      radiationProcess: xmlValue(doc, 'Control isRadiationProcess', energyMode ? '0' : xmlValue(doc, 'Control isProcess', '0')) === '1',
      energyProcess: xmlValue(doc, 'Control isEnergyProcess', energyMode ? xmlValue(doc, 'Control isProcess', '0') : '0') === '1',
      albedo: xmlValue(doc, 'Control isAlbedo', '0') === '1'
    },
    control: {
      specularEnabled: xmlValue(doc, 'Control specularReflectionEnabled', '1') === '1',
      depth: Number(xmlValue(doc, 'Control rayTracingDepth', base.control.depth)),
      samples: Number(xmlValue(doc, 'Control sampleCount', base.control.samples)),
      periodicTraversalCount: Math.max(0, Math.min(20, Math.round(Number(
        xmlValue(doc, 'Control periodicNeighborCount', base.control.periodicTraversalCount)
      )))),
      skyboxEnabled: xmlValue(doc, 'Control skyboxEnabled', '0') === '1',
      facetBackend: ['cpu', 'auto'].includes(xmlValue(doc, 'Control facetBackend', 'gpu').toLowerCase())
        ? xmlValue(doc, 'Control facetBackend', 'gpu').toLowerCase() : 'gpu',
      computeBackend: ['cpu', 'auto'].includes(xmlValue(doc, 'Control computeBackend', 'gpu').toLowerCase())
        ? xmlValue(doc, 'Control computeBackend', 'gpu').toLowerCase() : 'gpu',
      cpuThreads: normalizeCpuThreads(xmlValue(doc, 'Control cpuThreads', String(DEFAULT_CPU_THREADS))),
      voxelCollisionOrders: Math.max(2, Math.min(5, Math.round(Number(
        xmlValue(doc, 'Control voxelCollisionOrders', base.control.voxelCollisionOrders)
      )) || 2)),
      radiationSolver: xmlValue(doc, 'Control radiationSolver', base.control.radiationSolver) === 'accelerated'
        ? 'accelerated' : 'traditional',
      gpu: Number(xmlValue(doc, 'Control GPU', base.control.gpu)),
      couplingIterations: Number(xmlValue(doc, 'Control couplingIterations', base.control.couplingIterations)),
      temperatureTolerance: Number(xmlValue(doc, 'Control temperatureTolerance', base.control.temperatureTolerance)),
      temperatureRelaxation: Number(xmlValue(doc, 'Control temperatureRelaxation', base.control.temperatureRelaxation)),
      stabilityCorrection: xmlValue(doc, 'Control stabilityCorrection', '0') === '1',
      soilTemperatureMethod: Math.max(0, Math.min(2, Math.round(Number(xmlValue(
        doc, 'Control soilTemperatureMethod',
        materials.find((item) => item.type === 'Soil')?.params?.method ?? base.control.soilTemperatureMethod
      ))))),
      vegetationTemperatureMethod: Math.max(0, Math.min(1, Math.round(Number(xmlValue(
        doc, 'Control vegetationTemperatureMethod', base.control.vegetationTemperatureMethod
      )))))
    },
    objects: { count: objectNodes.length, names: names.length ? names : ['均质地表'], items },
    spectra: spectra.length ? spectra : base.spectra,
    canopies: canopies.length ? canopies : base.canopies,
    thermals: thermals.length ? thermals : base.thermals,
    materials: materials.length ? materials : base.materials,
    meteo: {
      path: xmlValue(doc, 'Meteorology filePath', '未配置'),
      start: Number(xmlValue(doc, 'Meteorology startTimeNode', 0)),
      end: Number(xmlValue(doc, 'Meteorology endTimeNode', 0)),
      z: Number(xmlValue(doc, 'Meteorology z', base.meteo.z)),
      Tsold: Number(xmlValue(doc, 'Meteorology Tsold', base.meteo.Tsold)),
      SatWater: Number(xmlValue(doc, 'Meteorology SatWater', base.meteo.SatWater)),
      dTime: Number(xmlValue(doc, 'Meteorology dTime', base.meteo.dTime)),
      latitude: Number(xmlValue(doc, 'Meteorology Latitude', base.meteo.latitude)),
      longitude: Number(xmlValue(doc, 'Meteorology Longitude', base.meteo.longitude)),
      year: Number(xmlValue(doc, 'Meteorology Year', base.meteo.year)),
      timezone: Number(xmlValue(doc, 'Meteorology TimeZone', base.meteo.timezone))
    },
    atmosphere: {
      enabled: xmlValue(doc, 'Atmosphere enabled', '0') === '1',
      model: ['tropical', 'midlatitude-summer', 'midlatitude-winter'].includes(xmlValue(doc, 'Atmosphere model', 'midlatitude-summer'))
        ? xmlValue(doc, 'Atmosphere model', 'midlatitude-summer') : 'midlatitude-summer',
      waterVapor: Math.max(0.5, Math.min(5, Number(xmlValue(doc, 'Atmosphere waterVapor', 2)))),
      aerosol: ['rural', 'urban'].includes(xmlValue(doc, 'Atmosphere aerosol', 'rural'))
        ? xmlValue(doc, 'Atmosphere aerosol', 'rural') : 'rural',
      visibility: Math.max(10, Math.min(50, Number(xmlValue(doc, 'Atmosphere visibility', 23)))),
      lutFile: xmlValue(doc, 'Atmosphere lutFile', 'assets/atmosphere/simple_modtran_lut.csv')
    }
  }
}

function markXmlDirty() {
  state.xmlDirty = true
  state.editRevision += 1
}

function syncProjectText() {
  if (projectTextSyncTimer) { clearTimeout(projectTextSyncTimer); projectTextSyncTimer = null }
  if (!state.project || !state.config) return
  state.project.configuration = state.config
  state.project.mode = state.mode
  state.project = normalizeProject(state.project)
  state.xmlText = stringifyProject(state.project)
  markXmlDirty()
  state.xmlManualEdit = false
  $('#xmlEditor').value = state.xmlText
  $('#unsavedMark').hidden = false
  $('#xmlStatus').textContent = '有未保存的修改'
}

// Re-serialising the whole project (including DEM samples) on every keystroke
// is expensive; live inputs debounce it and saveXml() flushes it first.
let projectTextSyncTimer = null
function scheduleProjectTextSync() {
  markXmlDirty()
  $('#unsavedMark').hidden = false
  if (projectTextSyncTimer) clearTimeout(projectTextSyncTimer)
  projectTextSyncTimer = setTimeout(syncProjectText, 250)
}

function flushProjectTextSync() {
  if (projectTextSyncTimer) syncProjectText()
}

const pendingFramePreviews = new Map()
function previewOnNextFrame(key, render) {
  if (!pendingFramePreviews.has(key)) requestAnimationFrame(() => {
    const latest = pendingFramePreviews.get(key)
    pendingFramePreviews.delete(key)
    latest()
  })
  pendingFramePreviews.set(key, render)
}

function updateXml() { syncProjectText() }

function fileName(path) {
  if (!path) return '未打开工程'
  return path.replaceAll('\\', '/').split('/').pop()
}

function isPrimObject(item = {}) {
  return item.sourceKind === 'prim' || Boolean(item.generation)
}

function isMediumObject(item = {}) {
  return item.type === 'Fire' || item.type === 'Fog' || Boolean(item.medium)
}

function toast(title, message, kind = '') {
  const item = document.createElement('div')
  item.className = `toast ${kind}`
  item.innerHTML = `<strong>${escapeHtml(title)}</strong><span>${escapeHtml(message)}</span>`
  $('#toastStack').append(item)
  setTimeout(() => item.remove(), 4200)
}

const MAX_LOG_ROWS = 2000

function addLog(text, kind = '') {
  const lines = String(text).replace(/\r/g, '').split('\n').filter(Boolean)
  for (const line of lines) {
    const row = document.createElement('div')
    row.className = `log-line ${kind}`
    row.innerHTML = `<time>${new Date().toLocaleTimeString('zh-CN', { hour12: false })}</time><span></span>`
    row.querySelector('span').textContent = line
    $('#consoleOutput').append(row)
    state.logCount += 1
  }
  const output = $('#consoleOutput')
  while (output.childElementCount > MAX_LOG_ROWS) output.firstElementChild.remove()
  $('#logCount').textContent = state.logCount
  output.scrollTop = output.scrollHeight
}

function setEngineState(label, detail, css = 'ready') {
  $('#engineState').textContent = label
  $('#enginePath').textContent = detail
  $('#engineDot').className = css
}

function runElapsedText(milliseconds = 0) {
  const seconds = Math.max(0, milliseconds) / 1000
  const hours = Math.floor(seconds / 3600)
  const minutes = Math.floor(seconds / 60) % 60
  const remaining = Math.floor(seconds % 60)
  const tenths = Math.floor((seconds % 1) * 10)
  return `${hours ? `${String(hours).padStart(2, '0')}:` : ''}${String(minutes).padStart(2, '0')}:${String(remaining).padStart(2, '0')}.${tenths}`
}

function renderRunProgress(finalElapsed = null) {
  const percent = Math.max(0, Math.min(100, Math.round(Number(state.progress) || 0)))
  const elapsed = finalElapsed == null ? Date.now() - state.startedAt : finalElapsed
  $('#elapsed').textContent = `${state.running ? '已运行' : '耗时'} ${runElapsedText(elapsed)}`
  $('#runProgressText').textContent = state.running
    ? '运行中' : (percent >= 100 ? '已完成' : state.progressStage)
  $('#runProgressPercent').textContent = `${percent}%`
  $$('#runProgress .run-progress-blocks i').forEach((block, index) => {
    const fill = Math.max(0, Math.min(100, (percent - index * 10) * 10))
    block.style.setProperty('--block-progress', `${fill}%`)
  })
  $('#runProgress').classList.toggle('running', state.running)
  $('#runProgress').classList.toggle('complete', !state.running && percent >= 100)
  if (state.running) {
    $('#engineState').textContent = `模拟运行中 · ${percent}%`
    $('#enginePath').textContent = `PID ${state.pid || '...'} · ${state.progressStage} · ${runElapsedText(elapsed)}`
  }
}

function setSimulationProgress(percent, stage = state.progressStage) {
  state.progress = Math.max(state.progress, Math.min(100, Math.max(0, Number(percent) || 0)))
  state.progressStage = stage || state.progressStage
  renderRunProgress()
}

function setRunning(running) {
  state.running = running
  $('#runBtn').disabled = running
  $('#stopBtn').disabled = !running
  $$('#modeGrid button').forEach((button) => { button.disabled = running })
  $('#drawRegionBtn').disabled = running
  $('#drawBoxBtn').disabled = running
  $('#editObjectBtn').disabled = running
  $('#sceneObjectToolsBtn').disabled = running
  $('#sceneObjectModifyBtn').disabled = running
  $('#sceneObjectAddBtn').disabled = running
  $('#computeDevice').disabled = running
  $('#cpuThreads').disabled = running
  $('#refreshDevicesBtn').disabled = running
  if (running) closeSceneObjectToolsMenu()
  if (running && geometryDraw.mode) cancelGeometryDraw()
  if (running && objectEdit.enabled) setObjectEditEnabled(false)
  if (running) {
    setEngineState('模拟运行中', `PID ${state.pid || '...'}`, 'running')
    renderRunProgress()
  }
  else {
    const facet = state.mode === 'eFacetRT' || state.mode === 'eFacetEB'
    const exists = state.executableExists
    const path = state.executable
    setEngineState(exists ? (facet ? 'HiStream Facet 待命' : 'HiStream 待命') : '未找到引擎', path, exists ? 'ready' : '')
  }
}

// Three.js 场景
const viewport = $('#viewport')
const scene = new THREE.Scene()
const defaultSceneBackground = new THREE.Color(0xf3f6fa)
scene.background = defaultSceneBackground
// Physical scene objects must not fade into the background as camera distance
// increases; distance fog was visually equivalent to extra transmittance.
scene.fog = null
const camera = new THREE.PerspectiveCamera(42, 1, 0.1, 1200)
camera.position.set(25, 22, 28)
const renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: 'high-performance' })
renderer.setPixelRatio(Math.min(devicePixelRatio, 2))
renderer.shadowMap.enabled = true
renderer.shadowMap.type = THREE.PCFShadowMap
renderer.outputColorSpace = THREE.SRGBColorSpace
renderer.toneMapping = THREE.ACESFilmicToneMapping
renderer.toneMappingExposure = 1.05
viewport.append(renderer.domElement)
const controls = new OrbitControls(camera, renderer.domElement)
controls.enableDamping = true
controls.dampingFactor = 0.075
controls.maxPolarAngle = Math.PI * 0.48
controls.minDistance = 4
controls.maxDistance = 320
const transformControls = new TransformControls(camera, renderer.domElement)
transformControls.setSize(.82)
transformControls.space = 'world'
scene.add(transformControls.getHelper())
scene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.65))
const sun = new THREE.DirectionalLight(0xfff7e8, 3.2)
sun.castShadow = true
sun.shadow.mapSize.set(2048, 2048)
sun.shadow.camera.left = sun.shadow.camera.bottom = -70
sun.shadow.camera.right = sun.shadow.camera.top = 70
sun.shadow.bias = -0.0005
scene.add(sun)
const world = new THREE.Group()
scene.add(world)
let grid = null
let voxelPreview = null
let sceneBoundsGuide = null
let sceneOffsetPreviewTimer = null
let cruiseVisualization = null
let sunVisualization = null
let currentStyle = 'solid'
let resultViewer = null
let typicalSkyboxTexture = null
const geometryDraw = {
  mode: null,
  phase: 'idle',
  start: null,
  current: null,
  height: 0,
  initialHeight: 0,
  heightAnchorY: 0,
  pointerId: null,
  preview: null
}
const geometryDrawRaycaster = new THREE.Raycaster()
const geometryDrawPointer = new THREE.Vector2()
const objectEditRaycaster = new THREE.Raycaster()
const objectEditPointer = new THREE.Vector2()
const objectEdit = {
  enabled: false,
  selected: null,
  selectionBox: null,
  pointerStart: null,
  transformStartScale: new THREE.Vector3(),
  saving: false,
  savePending: false
}

function geometryDrawHint(text = '') {
  const hint = $('#viewportDrawHint')
  hint.hidden = !text
  hint.textContent = text
}

function closeSceneObjectToolsMenu() {
  const menu = $('#sceneObjectToolsMenu')
  const button = $('#sceneObjectToolsBtn')
  if (menu) menu.hidden = true
  button?.setAttribute('aria-expanded', 'false')
  for (const [triggerId, menuId] of [['sceneObjectModifyBtn', 'sceneObjectModifyMenu'], ['sceneObjectAddBtn', 'sceneObjectAddMenu']]) {
    const trigger = $(`#${triggerId}`)
    const submenu = $(`#${menuId}`)
    if (submenu) submenu.hidden = true
    trigger?.classList.remove('open')
    trigger?.setAttribute('aria-expanded', 'false')
  }
}

function toggleSceneObjectSubmenu(kind) {
  const selectedTrigger = $(`#sceneObject${kind}Btn`)
  const selectedMenu = $(`#sceneObject${kind}Menu`)
  const opening = selectedMenu.hidden
  for (const [triggerId, menuId] of [['sceneObjectModifyBtn', 'sceneObjectModifyMenu'], ['sceneObjectAddBtn', 'sceneObjectAddMenu']]) {
    const trigger = $(`#${triggerId}`)
    const submenu = $(`#${menuId}`)
    const active = opening && trigger === selectedTrigger
    submenu.hidden = !active
    trigger.classList.toggle('open', active)
    trigger.setAttribute('aria-expanded', String(active))
  }
}

function updateSceneObjectToolsButton() {
  $('#sceneObjectToolsBtn')?.classList.toggle('active', Boolean(objectEdit.enabled || geometryDraw.mode))
}

function clearGeometryDrawPreview() {
  if (!geometryDraw.preview) return
  scene.remove(geometryDraw.preview)
  disposeObject(geometryDraw.preview)
  geometryDraw.preview = null
}

function geometryDrawSize() {
  if (!geometryDraw.start || !geometryDraw.current) return null
  return {
    sizeX: Math.abs(geometryDraw.current.north - geometryDraw.start.north),
    sizeZ: Math.abs(geometryDraw.current.east - geometryDraw.start.east),
    centerX: (geometryDraw.current.north + geometryDraw.start.north) / 2,
    centerZ: (geometryDraw.current.east + geometryDraw.start.east) / 2,
    worldX: (geometryDraw.current.worldX + geometryDraw.start.worldX) / 2,
    worldZ: (geometryDraw.current.worldZ + geometryDraw.start.worldZ) / 2,
    worldY: Math.max(geometryDraw.current.worldY, geometryDraw.start.worldY)
  }
}

function renderGeometryDrawPreview() {
  clearGeometryDrawPreview()
  const size = geometryDrawSize()
  if (!size || size.sizeX <= 0 || size.sizeZ <= 0) return
  const displayScale = Number(world.userData.displayScale) || 1
  const width = size.sizeX * displayScale
  const depth = size.sizeZ * displayScale
  const height = geometryDraw.mode === 'box' ? Math.max(.02, geometryDraw.height * displayScale) : Math.max(.018, displayScale * .04)
  const geometry = new THREE.BoxGeometry(width, height, depth)
  const group = new THREE.Group()
  const fill = new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({
    color: geometryDraw.mode === 'box' ? 0x2f81d4 : 0x2ba66d,
    transparent: true, opacity: .24, depthTest: false, depthWrite: false
  }))
  const edges = new THREE.LineSegments(new THREE.EdgesGeometry(geometry), new THREE.LineBasicMaterial({
    color: geometryDraw.mode === 'box' ? 0x1768c4 : 0x16845a,
    transparent: true, opacity: .96, depthTest: false, depthWrite: false
  }))
  fill.renderOrder = 80
  edges.renderOrder = 81
  group.add(fill, edges)
  group.position.set(size.worldX, size.worldY + height / 2 + .025, size.worldZ)
  geometryDraw.preview = group
  scene.add(group)
  const dimensions = geometryDraw.mode === 'box'
    ? `${size.sizeX.toFixed(2)} × ${geometryDraw.height.toFixed(2)} × ${size.sizeZ.toFixed(2)} m`
    : `${size.sizeX.toFixed(2)} × ${size.sizeZ.toFixed(2)} m`
  geometryDrawHint(geometryDraw.phase === 'height'
    ? `尺寸 ${dimensions}；移动鼠标调整高度，单击确认，Esc 或右键取消`
    : `尺寸 ${dimensions}；拖动确定范围，Esc 或右键取消`)
}

function cancelGeometryDraw(message = '') {
  clearGeometryDrawPreview()
  geometryDraw.mode = null
  geometryDraw.phase = 'idle'
  geometryDraw.start = null
  geometryDraw.current = null
  geometryDraw.pointerId = null
  controls.enabled = true
  $('.viewport-panel')?.classList.remove('drawing-geometry')
  $('#drawRegionBtn')?.classList.remove('active')
  $('#drawBoxBtn')?.classList.remove('active')
  updateSceneObjectToolsButton()
  geometryDrawHint('')
  if (message) toast('场景绘制', message)
}

function startGeometryDraw(mode) {
  if (!state.inputPath) { toast('无法绘制对象', '请先新建或打开工程', 'error'); return }
  if (state.running) { toast('无法绘制对象', '请先停止当前模拟', 'error'); return }
  if (geometryDraw.mode === mode) { cancelGeometryDraw(); return }
  if (objectEdit.enabled) setObjectEditEnabled(false)
  cancelGeometryDraw()
  closeSceneObjectToolsMenu()
  geometryDraw.mode = mode
  geometryDraw.phase = 'ready'
  controls.enabled = false
  $('.viewport-panel')?.classList.add('drawing-geometry')
  $(`#${mode === 'box' ? 'drawBoxBtn' : 'drawRegionBtn'}`)?.classList.add('active')
  updateSceneObjectToolsButton()
  geometryDrawHint(mode === 'box'
    ? '在地面拖出立方体底面；松开后移动鼠标设置高度，再单击确认'
    : '在地面按住左键拖出矩形区域；松开后直接生成 OBJ')
}

function geometryPointFromPointer(event) {
  const bounds = renderer.domElement.getBoundingClientRect()
  if (!bounds.width || !bounds.height) return null
  geometryDrawPointer.set(
    (event.clientX - bounds.left) / bounds.width * 2 - 1,
    -(event.clientY - bounds.top) / bounds.height * 2 + 1
  )
  geometryDrawRaycaster.setFromCamera(geometryDrawPointer, camera)
  const ground = world.children.find((child) => child.userData.kind === 'ground')
  const hit = ground ? geometryDrawRaycaster.intersectObject(ground, false)[0] : null
  if (!hit) return null
  const config = state.config
  const scale = Number(world.userData.displayScale) || 1
  const sceneX = Math.max(.1, Number(config.scene.x) || 60)
  const sceneZ = Math.max(.1, Number(config.scene.y) || 60)
  const offsetX = Number(config.scene.offsetX) || 0
  const offsetZ = Number(config.scene.offsetZ) || 0
  const snap = Math.max(.01, Number(config.scene.voxel) || 1)
  const snapWithin = (value, maximum) => THREE.MathUtils.clamp(Math.round(value / snap) * snap, 0, maximum)
  const localNorth = snapWithin(hit.point.x / scale + sceneX / 2, sceneX)
  const localEast = snapWithin(hit.point.z / scale + sceneZ / 2, sceneZ)
  return {
    north: localNorth + offsetX,
    east: localEast + offsetZ,
    worldX: (localNorth - sceneX / 2) * scale,
    worldZ: (localEast - sceneZ / 2) * scale,
    worldY: hit.point.y
  }
}

function finishGeometryDraw() {
  const size = geometryDrawSize()
  const mode = geometryDraw.mode
  if (!size || size.sizeX <= 0 || size.sizeZ <= 0) {
    clearGeometryDrawPreview()
    geometryDraw.phase = 'ready'
    geometryDrawHint('绘制范围太小，请重新拖动')
    return
  }
  const form = $('#geometryForm')
  const index = (state.config?.objects?.items?.length || 0) + 1
  const verticalOffset = Number(state.config?.scene?.offsetY) || 0
  form.reset()
  form.elements.geometryName.value = `${mode === 'box' ? 'draw_box' : 'draw_region'}_${index}`
  form.elements.geometryShape.value = 'cube'
  form.elements.geometryMode.value = mode === 'box' ? 'surface' : 'water'
  form.elements.geometrySizeX.value = size.sizeX
  form.elements.geometrySizeY.value = mode === 'box' ? geometryDraw.height : Math.max(.01, Number(state.config?.scene?.voxel) || 1)
  form.elements.geometrySizeZ.value = size.sizeZ
  form.elements.geometryDetail.value = 1
  if (mode === 'box') {
    state.drawPlacement = { north: size.centerX, east: size.centerZ, height: verticalOffset }
  } else {
    form.elements.waterPositionX.value = size.centerX
    form.elements.waterPositionY.value = size.centerZ
    form.elements.waterPositionZ.value = verticalOffset
    form.elements.waterOffsetX.value = 0
    form.elements.waterOffsetY.value = 0
    form.elements.waterOffsetZ.value = 0
  }
  cancelGeometryDraw()
  updateGeometryDialog()
  form.requestSubmit()
}

function onGeometryDrawPointerDown(event) {
  if (!geometryDraw.mode || event.button !== 0) return
  event.preventDefault()
  event.stopPropagation()
  if (geometryDraw.phase === 'height') { finishGeometryDraw(); return }
  const point = geometryPointFromPointer(event)
  if (!point) return
  geometryDraw.phase = 'base'
  geometryDraw.start = point
  geometryDraw.current = point
  geometryDraw.pointerId = event.pointerId
  renderer.domElement.setPointerCapture?.(event.pointerId)
  renderGeometryDrawPreview()
}

function onGeometryDrawPointerMove(event) {
  if (!geometryDraw.mode) return
  if (geometryDraw.phase === 'height') {
    const sceneHeight = Math.max(.1, Number(state.config?.scene?.height) || 20)
    const snap = Math.max(.01, Number(state.config?.scene?.voxel) || 1)
    const raw = geometryDraw.initialHeight + (geometryDraw.heightAnchorY - event.clientY) * sceneHeight / Math.max(120, viewport.clientHeight)
    geometryDraw.height = THREE.MathUtils.clamp(Math.max(snap, Math.round(raw / snap) * snap), snap, sceneHeight)
    renderGeometryDrawPreview()
    return
  }
  if (geometryDraw.phase !== 'base' || geometryDraw.pointerId !== event.pointerId) return
  const point = geometryPointFromPointer(event)
  if (!point) return
  geometryDraw.current = point
  renderGeometryDrawPreview()
}

function onGeometryDrawPointerUp(event) {
  if (!geometryDraw.mode || geometryDraw.phase !== 'base' || geometryDraw.pointerId !== event.pointerId) return
  event.preventDefault()
  event.stopPropagation()
  renderer.domElement.releasePointerCapture?.(event.pointerId)
  geometryDraw.pointerId = null
  const point = geometryPointFromPointer(event)
  if (point) geometryDraw.current = point
  const size = geometryDrawSize()
  if (!size || size.sizeX <= 0 || size.sizeZ <= 0) {
    clearGeometryDrawPreview()
    geometryDraw.phase = 'ready'
    geometryDrawHint('绘制范围太小，请重新拖动')
    return
  }
  if (geometryDraw.mode === 'region') { finishGeometryDraw(); return }
  const snap = Math.max(.01, Number(state.config?.scene?.voxel) || 1)
  const sceneHeight = Math.max(snap, Number(state.config?.scene?.height) || 20)
  geometryDraw.initialHeight = THREE.MathUtils.clamp(Math.max(snap, Math.max(size.sizeX, size.sizeZ) * .5), snap, sceneHeight)
  geometryDraw.height = Math.max(snap, Math.round(geometryDraw.initialHeight / snap) * snap)
  geometryDraw.heightAnchorY = event.clientY
  geometryDraw.phase = 'height'
  renderGeometryDrawPreview()
}

function updateObjectSelectionBox() {
  if (!objectEdit.selectionBox || !objectEdit.selected) return
  objectEdit.selectionBox.setFromObject(objectEdit.selected)
}

function clearObjectSelection(showHint = true) {
  transformControls.detach()
  if (objectEdit.selectionBox) {
    scene.remove(objectEdit.selectionBox)
    objectEdit.selectionBox.geometry?.dispose?.()
    objectEdit.selectionBox.material?.dispose?.()
  }
  objectEdit.selected = null
  objectEdit.selectionBox = null
  if (objectEdit.enabled && showHint) geometryDrawHint('单击选择 OBJ；双击修改物化属性；W/E/R 切换移动、旋转、缩放')
}

function setObjectTransformMode(mode) {
  if (!['translate', 'rotate', 'scale'].includes(mode)) return
  transformControls.setMode(mode)
  transformControls.showX = mode !== 'rotate'
  transformControls.showY = true
  transformControls.showZ = mode !== 'rotate'
  $$('#transformModeButtons button').forEach((button) => button.classList.toggle('active', button.dataset.transformMode === mode))
}

function setObjectEditEnabled(enabled) {
  if (enabled && !state.inputPath) { toast('无法编辑 OBJ', '请先新建或打开工程', 'error'); return }
  if (enabled && state.running) { toast('无法编辑 OBJ', '请先停止当前模拟', 'error'); return }
  objectEdit.enabled = Boolean(enabled)
  if (objectEdit.enabled) {
    if (geometryDraw.mode) cancelGeometryDraw()
    setObjectTransformMode(transformControls.mode || 'translate')
    geometryDrawHint('单击选择 OBJ；双击修改物化属性；W/E/R 切换移动、旋转、缩放')
  } else {
    clearObjectSelection(false)
    geometryDrawHint('')
  }
  $('#editObjectBtn')?.classList.toggle('active', objectEdit.enabled)
  $('.viewport-panel')?.classList.toggle('editing-object', objectEdit.enabled)
  closeSceneObjectToolsMenu()
  updateSceneObjectToolsButton()
}

function projectObjectFromIntersection(intersection) {
  let object = intersection?.object || null
  while (object && object !== world) {
    if (object.userData.projectObject) return object
    object = object.parent
  }
  return null
}

function objectAtPointer(event) {
  const bounds = renderer.domElement.getBoundingClientRect()
  if (!bounds.width || !bounds.height) return null
  objectEditPointer.set(
    (event.clientX - bounds.left) / bounds.width * 2 - 1,
    -(event.clientY - bounds.top) / bounds.height * 2 + 1
  )
  objectEditRaycaster.setFromCamera(objectEditPointer, camera)
  const roots = world.children.filter((child) => child.userData.projectObject)
  for (const intersection of objectEditRaycaster.intersectObjects(roots, true)) {
    const object = projectObjectFromIntersection(intersection)
    if (object) return object
  }
  return null
}

function selectObjectForEditing(object) {
  if (!object) { clearObjectSelection(); return }
  const itemIndex = Number(object.userData.projectObjectIndex)
  const item = state.config?.objects?.items?.[itemIndex]
  if (!item) return
  if (item.movement?.enabled) {
    toast('暂时不能移动该 OBJ', '请先在对象设置中关闭随机移动', 'error')
    return
  }
  clearObjectSelection(false)
  objectEdit.selected = object
  objectEdit.selectionBox = new THREE.BoxHelper(object, 0x1768c4)
  objectEdit.selectionBox.material.depthTest = false
  objectEdit.selectionBox.material.transparent = true
  objectEdit.selectionBox.material.opacity = .9
  objectEdit.selectionBox.renderOrder = 90
  scene.add(objectEdit.selectionBox)
  transformControls.attach(object)
  state.selectedObjectIndex = itemIndex
  const placementIndex = Number(object.userData.projectPlacementIndex) + 1
  geometryDrawHint(`${item.name} · 第 ${placementIndex} 个实例；拖动手柄编辑，双击修改物化属性`)
}

function sceneTerrainHeight(config, north, east) {
  if (!config?.scene?.terrain) return 0
  const dem = config.scene.demInfo
  const width = Math.max(2, Math.round(Number(dem?.terrainWidth) || 0))
  const height = Math.max(2, Math.round(Number(dem?.terrainHeight) || 0))
  const values = Array.isArray(dem?.terrainValues) ? dem.terrainValues : []
  if (values.length !== width * height) return 0
  const u = THREE.MathUtils.clamp(Number(east) / Math.max(1, Number(config.scene.y)), 0, 1) * (width - 1)
  const v = (1 - THREE.MathUtils.clamp(Number(north) / Math.max(1, Number(config.scene.x)), 0, 1)) * (height - 1)
  const x0 = Math.floor(u), y0 = Math.floor(v)
  const x1 = Math.min(width - 1, x0 + 1), y1 = Math.min(height - 1, y0 + 1)
  const fallback = Number.isFinite(Number(dem.minimum)) ? Number(dem.minimum) : 0
  const sample = (column, row) => {
    const value = Number(values[row * width + column])
    return Number.isFinite(value) ? value : fallback
  }
  const top = THREE.MathUtils.lerp(sample(x0, y0), sample(x1, y0), u - x0)
  const bottom = THREE.MathUtils.lerp(sample(x0, y1), sample(x1, y1), u - x0)
  return Math.max(0, THREE.MathUtils.lerp(top, bottom, v - y0) - fallback)
}

function editedPlacement(object) {
  const config = state.config
  const displayScale = Number(world.userData.displayScale) || 1
  const sceneX = Math.max(.1, Number(config.scene.x) || 60)
  const sceneY = Math.max(.1, Number(config.scene.y) || 60)
  const localNorth = object.position.x / displayScale + sceneX / 2
  const localEast = object.position.z / displayScale + sceneY / 2
  const initialScale = object.userData.projectInitialScale || object.scale
  const scaleFactor = Math.max(.01, object.scale.x / Math.max(1e-9, initialScale.x))
  const original = object.userData.projectPlacement || { scale: 1 }
  const verticalAnchor = (Number(object.userData.projectVerticalAnchor) || 0) * scaleFactor
  const round = (value) => Number(Number(value).toFixed(6))
  let rotation = -THREE.MathUtils.radToDeg(object.rotation.y)
  rotation = ((rotation % 360) + 360) % 360
  const scale = Math.max(.01, (Number(original.scale) || 1) * scaleFactor)
  const footprint = object.userData.projectGroundFootprint
  const height = footprint
    ? groundedPlacementHeight(footprint, { north: localNorth, east: localEast, scale, rotation }, (north, east) => sceneTerrainHeight(config, north, east))
    : (object.position.y - verticalAnchor) / displayScale - sceneTerrainHeight(config, localNorth, localEast)
  return {
    x: round(localNorth + (Number(config.scene.offsetX) || 0)),
    y: round(localEast + (Number(config.scene.offsetZ) || 0)),
    z: round(height + (Number(config.scene.offsetY) || 0)),
    scale: round(scale),
    rotation: round(rotation)
  }
}

function snapGroundedObject(object) {
  if (!object?.userData?.projectGroundFootprint) return
  const config = state.config
  const displayScale = Number(world.userData.displayScale) || 1
  const placement = editedPlacement(object)
  const north = object.position.x / displayScale + Math.max(.1, Number(config.scene.x) || 60) / 2
  const east = object.position.z / displayScale + Math.max(.1, Number(config.scene.y) || 60) / 2
  object.position.y = (sceneTerrainHeight(config, north, east) + placement.z - (Number(config.scene.offsetY) || 0)) * displayScale
}

async function saveEditedObjectTransform(object) {
  if (!object?.userData?.projectObject || objectEdit.saving) {
    if (objectEdit.saving) objectEdit.savePending = true
    return
  }
  const itemIndex = Number(object.userData.projectObjectIndex)
  const placementIndex = Number(object.userData.projectPlacementIndex)
  const item = state.config?.objects?.items?.[itemIndex]
  if (!item || !Number.isInteger(placementIndex)) return
  objectEdit.saving = true
  try {
    let points
    if (item.positionFile) points = parseDistributionPointsText((await api.readText(item.positionFile)).content)
    else points = generateDistribution(item.distribution || { ...defaultDistribution(item), mode: 'single' })
    if (!points[placementIndex]) throw new Error(`找不到第 ${placementIndex + 1} 个实例的位置记录`)
    const placement = editedPlacement(object)
    points[placementIndex] = placement
    const saved = await api.saveDistribution({ path: item.positionFile, name: item.name, instances: points })
    item.positionFile = saved.path
    item.distribution = {
      ...defaultDistribution(item), mode: 'manual', basis: 'count', count: points.length,
      points: distributionPointsText(points), offsetX: 0, offsetY: 0, offsetZ: 0
    }
    item.instanceCount = points.length
    object.userData.projectPlacement = { ...placement }
    object.userData.projectInitialScale = object.scale.clone()
    object.userData.projectVerticalAnchor = (Number(object.userData.projectVerticalAnchor) || 0) * Math.max(.01, object.scale.x / Math.max(1e-9, objectEdit.transformStartScale.x || object.scale.x))
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw state.lastSaveError || new Error('工程配置保存失败')
    addLog(`OBJ 实例位置已更新：${item.name} #${placementIndex + 1}`, 'success')
  } catch (error) {
    toast('OBJ 位置保存失败', error.message, 'error')
    addLog(`OBJ 位置保存失败：${error.message}`, 'error')
  } finally {
    objectEdit.saving = false
    if (objectEdit.savePending) {
      objectEdit.savePending = false
      saveEditedObjectTransform(objectEdit.selected)
    }
  }
}

function onObjectEditPointerDown(event) {
  if (!objectEdit.enabled || geometryDraw.mode || event.button !== 0 || transformControls.axis || transformControls.dragging) return
  objectEdit.pointerStart = { x: event.clientX, y: event.clientY }
}

function onObjectEditPointerUp(event) {
  if (!objectEdit.enabled || geometryDraw.mode || !objectEdit.pointerStart || transformControls.dragging || transformControls.axis) return
  const distance = Math.hypot(event.clientX - objectEdit.pointerStart.x, event.clientY - objectEdit.pointerStart.y)
  objectEdit.pointerStart = null
  if (distance <= 5) selectObjectForEditing(objectAtPointer(event))
}

function onObjectEditDoubleClick(event) {
  if (!objectEdit.enabled || geometryDraw.mode) return
  const object = objectAtPointer(event)
  if (!object) return
  selectObjectForEditing(object)
  showObjectAttributeDialog(Number(object.userData.projectObjectIndex))
}

function createTypicalSkyboxTexture() {
  const canvas = document.createElement('canvas')
  canvas.width = 1024
  canvas.height = 512
  const context = canvas.getContext('2d')
  const sky = context.createLinearGradient(0, 0, 0, canvas.height)
  sky.addColorStop(0, '#1769b0')
  sky.addColorStop(0.48, '#65b9ed')
  sky.addColorStop(0.7, '#d7effb')
  sky.addColorStop(1, '#eef5f6')
  context.fillStyle = sky
  context.fillRect(0, 0, canvas.width, canvas.height)

  const cloudBands = [
    [90, 185, 145, 34], [285, 145, 190, 43], [535, 205, 165, 38],
    [760, 130, 205, 46], [975, 210, 145, 34], [420, 260, 125, 27]
  ]
  for (const [x, y, width, height] of cloudBands) {
    const glow = context.createRadialGradient(x, y, 2, x, y, width * 0.58)
    glow.addColorStop(0, 'rgba(255,255,255,0.92)')
    glow.addColorStop(0.55, 'rgba(250,253,255,0.72)')
    glow.addColorStop(1, 'rgba(235,244,249,0)')
    context.fillStyle = glow
    context.beginPath()
    context.ellipse(x, y, width, height, 0, 0, Math.PI * 2)
    context.fill()
  }
  const texture = new THREE.CanvasTexture(canvas)
  texture.mapping = THREE.EquirectangularReflectionMapping
  texture.colorSpace = THREE.SRGBColorSpace
  return texture
}

function updateSceneSkybox(config) {
  if (config?.control?.skyboxEnabled) {
    typicalSkyboxTexture ||= createTypicalSkyboxTexture()
    scene.background = typicalSkyboxTexture
  } else {
    scene.background = defaultSceneBackground
  }
}

// FIXED[中|内存泄漏]: 只释放 material.map，normalMap/alphaMap/emissiveMap/roughnessMap 等其它贴图及 uniforms 中的纹理不会 dispose，反复重建场景会累积 GPU 纹理；已修复：遍历材质自身的全部属性与 uniforms，凡 isTexture 的统一 dispose（同一纹理多次 dispose 是幂等的，未做引用计数）
function disposeObject(object) {
  object.traverse((child) => {
    child.geometry?.dispose?.()
    const materials = Array.isArray(child.material) ? child.material : [child.material]
    materials.forEach((item) => {
      if (!item) return
      for (const value of Object.values(item)) if (value?.isTexture) value.dispose()
      for (const uniform of Object.values(item.uniforms || {})) if (uniform?.value?.isTexture) uniform.value.dispose()
      item.dispose?.()
    })
  })
}

function clearWorld() {
  clearObjectSelection(false)
  while (world.children.length) {
    const child = world.children.pop()
    disposeObject(child)
  }
  grid = null
  voxelPreview = null
  sceneBoundsGuide = null
  cruiseVisualization = null
}

function formatSceneDimension(value) {
  const number = Math.max(0, Number(value) || 0)
  return number >= 100 ? Math.round(number).toLocaleString('zh-CN') : Number(number.toFixed(1)).toLocaleString('zh-CN')
}

function createDimensionLabel(text, width) {
  const label = t(text)
  const font = '700 34px "Segoe UI", "Microsoft YaHei", sans-serif'
  const canvas = document.createElement('canvas')
  const measure = canvas.getContext('2d')
  measure.font = font
  canvas.width = Math.max(320, Math.ceil(measure.measureText(label).width) + 48)
  canvas.height = 82
  const context = canvas.getContext('2d')
  context.fillStyle = 'rgba(248, 251, 255, 0.94)'
  context.strokeStyle = 'rgba(36, 111, 211, 0.9)'
  context.lineWidth = 4
  context.beginPath()
  context.roundRect(4, 4, canvas.width - 8, canvas.height - 8, 18)
  context.fill()
  context.stroke()
  context.fillStyle = '#173f70'
  context.font = font
  context.textAlign = 'center'
  context.textBaseline = 'middle'
  context.fillText(label, canvas.width / 2, canvas.height / 2 + 1)

  const texture = new THREE.CanvasTexture(canvas)
  texture.colorSpace = THREE.SRGBColorSpace
  const sprite = new THREE.Sprite(new THREE.SpriteMaterial({
    map: texture,
    transparent: true,
    depthTest: false,
    depthWrite: false
  }))
  // `width` is the world width of a 320 px label; longer text widens the sprite.
  const scale = width / 320
  sprite.scale.set(canvas.width * scale, canvas.height * scale, 1)
  sprite.renderOrder = 31
  return sprite
}

function addSceneBoundsEdge(group, start, end, radius, material) {
  const direction = end.clone().sub(start)
  const geometry = new THREE.CylinderGeometry(radius, radius, direction.length(), 6, 1, false)
  const edge = new THREE.Mesh(geometry, material.clone())
  edge.position.copy(start).add(end).multiplyScalar(0.5)
  edge.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), direction.normalize())
  edge.renderOrder = 29
  group.add(edge)
}

function updateSceneBoundsGuide(config) {
  if (sceneBoundsGuide) {
    world.remove(sceneBoundsGuide)
    disposeObject(sceneBoundsGuide)
    sceneBoundsGuide = null
  }

  const sceneX = Math.max(0.1, Number(config?.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config?.scene?.y) || 60)
  const offsetX = Number(config?.scene?.offsetX) || 0
  const offsetY = Number(config?.scene?.offsetY) || 0
  const offsetZ = Number(config?.scene?.offsetZ) || 0
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  const width = sceneX * displayScale
  const depth = sceneY * displayScale
  // The guide describes the configured simulation domain, not the current
  // mesh bounds. Objects may extend outside it, but must not resize the box.
  const sceneHeight = Math.max(0, Number(config?.scene?.height) || 0)
  const displayHeight = Math.max(0.06, sceneHeight * displayScale)

  const bounds = new THREE.Box3(
    new THREE.Vector3(-width / 2, 0, -depth / 2),
    new THREE.Vector3(width / 2, displayHeight, depth / 2)
  )
  const group = new THREE.Group()
  group.name = 'scene-bounds-guide'
  group.userData.sceneBoundsGuide = true

  const helper = new THREE.Box3Helper(bounds, 0x246fd3)
  helper.material.transparent = true
  helper.material.opacity = 0.66
  helper.material.depthTest = false
  helper.material.depthWrite = false
  helper.renderOrder = 30
  group.add(helper)

  const edgeMaterial = new THREE.MeshBasicMaterial({ color: 0x247bd8, transparent: true, opacity: 0.72, depthTest: false, depthWrite: false })
  const edgeRadius = THREE.MathUtils.clamp(Math.max(width, depth) * 0.0014, 0.018, 0.065)
  const bottom = 0.035
  const corners = [
    new THREE.Vector3(-width / 2, bottom, -depth / 2),
    new THREE.Vector3(width / 2, bottom, -depth / 2),
    new THREE.Vector3(width / 2, bottom, depth / 2),
    new THREE.Vector3(-width / 2, bottom, depth / 2)
  ]
  const topCorners = corners.map((corner) => new THREE.Vector3(corner.x, displayHeight, corner.z))
  for (let index = 0; index < 4; index += 1) {
    const next = (index + 1) % 4
    addSceneBoundsEdge(group, corners[index], corners[next], edgeRadius, edgeMaterial)
    addSceneBoundsEdge(group, topCorners[index], topCorners[next], edgeRadius, edgeMaterial)
    addSceneBoundsEdge(group, corners[index], topCorners[index], edgeRadius, edgeMaterial)
  }
  edgeMaterial.dispose()

  const extent = Math.max(width, depth)
  const labelWidth = THREE.MathUtils.clamp(extent * 0.16, 2.8, 8)
  const padding = THREE.MathUtils.clamp(extent * 0.035, 0.35, 1.4)
  const xLabel = createDimensionLabel(`X · ${formatSceneDimension(offsetX)}–${formatSceneDimension(offsetX + sceneX)} m`, labelWidth)
  xLabel.position.set(0, Math.max(0.18, displayHeight * 0.025), depth / 2 - padding * 0.55)
  group.add(xLabel)
  const zLabel = createDimensionLabel(`Z · ${formatSceneDimension(offsetZ)}–${formatSceneDimension(offsetZ + sceneY)} m`, labelWidth)
  zLabel.position.set(width / 2 - padding * 0.55, Math.max(0.18, displayHeight * 0.025), 0)
  group.add(zLabel)
  const heightLabel = createDimensionLabel(`Y · ${formatSceneDimension(offsetY)}–${formatSceneDimension(offsetY + sceneHeight)} m`, labelWidth)
  heightLabel.position.set(width / 2 - padding * 0.55, displayHeight / 2, depth / 2 - padding * 0.55)
  group.add(heightLabel)

  // Geographic azimuths use north as 0 degrees. Three.js +X is scene north.
  const northLength = Math.min(
    THREE.MathUtils.clamp(extent * 0.16, 2.8, 7.2),
    Math.max(1.2, depth * 0.3)
  )
  // Keep the compass inside the front-left quadrant; depth testing is disabled
  // so dense city geometry cannot hide the reference direction.
  const northOrigin = new THREE.Vector3(
    -width * 0.28,
    Math.max(0.12, displayHeight + 0.08),
    depth * 0.2 - northLength
  )
  const northArrow = new THREE.ArrowHelper(
    new THREE.Vector3(1, 0, 0), northOrigin, northLength,
    0x15805f, northLength * 0.22, northLength * 0.12
  )
  northArrow.name = 'scene-north-arrow'
  northArrow.traverse((child) => {
    child.renderOrder = 40
    if (child.material) {
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(northArrow)
  const northLabel = createDimensionLabel('正北 N · 0°', THREE.MathUtils.clamp(labelWidth * 0.72, 2.8, 5.4))
  northLabel.name = 'scene-north-label'
  northLabel.position.copy(northOrigin).add(new THREE.Vector3(northLength * 1.08, Math.max(0.18, displayHeight * 0.025), 0))
  northLabel.renderOrder = 41
  group.add(northLabel)

  sceneBoundsGuide = group
  world.add(group)
  world.userData.boundsHeight = displayHeight
  world.userData.sceneHeight = sceneHeight
  const summary = $('#sceneDimensionSummary')
  if (summary) summary.textContent = `计算域 X ${formatSceneDimension(offsetX)}–${formatSceneDimension(offsetX + sceneX)} · Y ${formatSceneDimension(offsetY)}–${formatSceneDimension(offsetY + sceneHeight)} · Z ${formatSceneDimension(offsetZ)}–${formatSceneDimension(offsetZ + sceneY)} m`
}

function cruisePointToWorld(point, config) {
  const sceneX = Math.max(0.1, Number(config?.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config?.scene?.y) || 60)
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  return new THREE.Vector3(
    (Number(point.x) - sceneX * 0.5) * displayScale,
    Math.max(0, Number(point.z)) * displayScale,
    (Number(point.y) - sceneY * 0.5) * displayScale
  )
}

function createCruiseAircraft(size, headingDegrees) {
  const aircraft = new THREE.Group()
  aircraft.name = 'cruise-aircraft'
  aircraft.userData.kind = 'sensor'
  aircraft.rotation.y = THREE.MathUtils.degToRad(headingDegrees + 90)

  const white = new THREE.MeshStandardMaterial({ color: 0xf4f7fa, roughness: 0.42, metalness: 0.1, transparent: true, opacity: 0.86 })
  const blue = new THREE.MeshStandardMaterial({ color: 0x83acd2, roughness: 0.46, metalness: 0.08, transparent: true, opacity: 0.84 })
  const dark = new THREE.MeshStandardMaterial({ color: 0x718da6, roughness: 0.34, metalness: 0.14, transparent: true, opacity: 0.86 })
  for (const item of [white, blue, dark]) item.userData.baseColor = item.color.getHex()

  const body = new THREE.Mesh(new THREE.CylinderGeometry(size * 0.085, size * 0.12, size * 1.18, 10), white)
  body.rotation.x = Math.PI / 2
  aircraft.add(body)
  const nose = new THREE.Mesh(new THREE.ConeGeometry(size * 0.12, size * 0.34, 10), blue)
  nose.rotation.x = Math.PI / 2
  nose.position.z = size * 0.76
  aircraft.add(nose)
  const wings = new THREE.Mesh(new THREE.BoxGeometry(size * 1.45, size * 0.055, size * 0.34), blue)
  wings.position.z = size * 0.04
  aircraft.add(wings)
  const tail = new THREE.Mesh(new THREE.BoxGeometry(size * 0.68, size * 0.045, size * 0.22), white)
  tail.position.z = -size * 0.5
  aircraft.add(tail)
  const fin = new THREE.Mesh(new THREE.BoxGeometry(size * 0.055, size * 0.3, size * 0.25), blue)
  fin.position.set(0, size * 0.15, -size * 0.48)
  aircraft.add(fin)
  const cockpit = new THREE.Mesh(new THREE.SphereGeometry(size * 0.105, 10, 6), dark)
  cockpit.scale.set(0.85, 0.55, 1.35)
  cockpit.position.set(0, size * 0.1, size * 0.28)
  aircraft.add(cockpit)

  // The aircraft's physical scale can be tiny in kilometre-scale scenes, so
  // retain the 3-D model and add a camera-facing marker for legibility.
  const markerCanvas = document.createElement('canvas')
  markerCanvas.width = markerCanvas.height = 160
  const markerContext = markerCanvas.getContext('2d')
  markerContext.beginPath()
  markerContext.arc(80, 80, 66, 0, Math.PI * 2)
  markerContext.fillStyle = 'rgba(255, 255, 255, 0.76)'
  markerContext.fill()
  markerContext.lineWidth = 9
  markerContext.strokeStyle = '#8aafd1'
  markerContext.stroke()
  markerContext.fillStyle = '#789fc2'
  markerContext.font = '700 82px "Segoe UI Symbol", "Microsoft YaHei", sans-serif'
  markerContext.textAlign = 'center'
  markerContext.textBaseline = 'middle'
  markerContext.fillText('✈', 80, 78)
  const markerTexture = new THREE.CanvasTexture(markerCanvas)
  markerTexture.colorSpace = THREE.SRGBColorSpace
  const marker = new THREE.Sprite(new THREE.SpriteMaterial({ map: markerTexture, transparent: true, depthTest: false, depthWrite: false }))
  marker.name = 'cruise-aircraft-marker'
  marker.position.y = size * 0.68
  marker.scale.set(size * 1.12, size * 1.12, 1)
  marker.renderOrder = 38
  aircraft.add(marker)
  aircraft.traverse((child) => {
    if (!child.isMesh) return
    child.userData.kind = 'sensor'
    child.castShadow = true
    child.renderOrder = 35
  })
  return aircraft
}

function createObservationSatellite(size) {
  const satellite = new THREE.Group()
  satellite.name = 'parallel-observation-satellite'
  satellite.userData.kind = 'sensor'

  const bodyMaterial = new THREE.MeshStandardMaterial({ color: 0xeaf0f4, roughness: 0.42, metalness: 0.34, transparent: true, opacity: 0.86 })
  const trimMaterial = new THREE.MeshStandardMaterial({ color: 0xd9bd83, roughness: 0.48, metalness: 0.3, transparent: true, opacity: 0.84 })
  const panelMaterial = new THREE.MeshStandardMaterial({ color: 0x7197bc, roughness: 0.4, metalness: 0.2, transparent: true, opacity: 0.82 })
  const cellMaterial = new THREE.LineBasicMaterial({ color: 0xa9c3dc, transparent: true, opacity: 0.58 })
  for (const item of [bodyMaterial, trimMaterial, panelMaterial]) item.userData.baseColor = item.color.getHex()

  const body = new THREE.Mesh(new THREE.BoxGeometry(size * 0.58, size * 0.48, size * 0.72), bodyMaterial)
  satellite.add(body)
  const trim = new THREE.Mesh(new THREE.BoxGeometry(size * 0.66, size * 0.12, size * 0.78), trimMaterial)
  satellite.add(trim)

  for (const side of [-1, 1]) {
    const panel = new THREE.Mesh(new THREE.BoxGeometry(size * 1.05, size * 0.055, size * 0.58), panelMaterial)
    panel.position.x = side * size * 0.86
    satellite.add(panel)
    const panelGrid = new THREE.GridHelper(size * 0.92, 4, 0x71a7e8, 0x71a7e8)
    panelGrid.scale.z = 0.52
    panelGrid.position.set(side * size * 0.86, size * 0.035, 0)
    panelGrid.material = cellMaterial.clone()
    satellite.add(panelGrid)
  }

  // The camera points along local -Y; the whole satellite is rotated toward
  // the scene target after it has been positioned on the viewing hemisphere.
  const cameraHousing = new THREE.Mesh(new THREE.CylinderGeometry(size * 0.18, size * 0.23, size * 0.32, 14), trimMaterial)
  cameraHousing.position.y = -size * 0.38
  satellite.add(cameraHousing)
  const lens = new THREE.Mesh(new THREE.CylinderGeometry(size * 0.105, size * 0.105, size * 0.08, 16), new THREE.MeshStandardMaterial({ color: 0x71869a, roughness: 0.28, metalness: 0.4, transparent: true, opacity: 0.86 }))
  lens.position.y = -size * 0.58
  satellite.add(lens)

  const antenna = new THREE.Mesh(new THREE.SphereGeometry(size * 0.24, 16, 8, 0, Math.PI * 2, 0, Math.PI * 0.48), bodyMaterial)
  antenna.scale.y = 0.38
  antenna.position.y = size * 0.38
  satellite.add(antenna)
  satellite.traverse((child) => {
    if (!child.isMesh) return
    child.userData.kind = 'sensor'
    child.castShadow = true
    child.renderOrder = 36
  })
  return satellite
}

function createParallelObservationVisualization(config) {
  const sensor = config.sensor
  const sceneX = Math.max(0.1, Number(config.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config.scene?.y) || 60)
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  const extent = Math.max(sceneX, sceneY) * displayScale
  const sceneHeight = Math.max(0, Number(config.scene?.height) || 0) * displayScale
  const angles = sensorViewAngles(sensor, config.light?.azimuth)
  const [zenithDegrees, azimuthDegrees] = angles[0] || [Number(sensor.vza) || 0, Number(sensor.vaa) || 0]
  const zenith = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(Number(zenithDegrees) || 0, 0, 89.999))
  const azimuthValue = ((Number(azimuthDegrees) || 0) % 360 + 360) % 360
  const azimuth = THREE.MathUtils.degToRad(azimuthValue)
  const target = new THREE.Vector3(0, Math.max(0.04, sceneHeight * 0.32), 0)
  const observationOut = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  const orbitalRadius = THREE.MathUtils.clamp(extent * 0.63, 13, 31)
  const satellitePosition = target.clone().addScaledVector(observationOut, orbitalRadius)
  const satelliteSize = THREE.MathUtils.clamp(extent * 0.026, 0.55, 1.15)

  const group = new THREE.Group()
  group.name = 'sensor-parallel-visualization'
  const satellite = createObservationSatellite(satelliteSize)
  satellite.position.copy(satellitePosition)
  satellite.quaternion.setFromUnitVectors(
    new THREE.Vector3(0, -1, 0), target.clone().sub(satellitePosition).normalize()
  )
  group.add(satellite)

  const viewArrow = new THREE.ArrowHelper(
    observationOut.clone().negate(), satellitePosition,
    orbitalRadius * 0.82, 0xc28aa6,
    satelliteSize * 0.4, satelliteSize * 0.22
  )
  viewArrow.name = 'parallel-view-direction'
  viewArrow.traverse((child) => {
    child.renderOrder = 39
    if (child.material) {
      child.material.transparent = true
      child.material.opacity = 0.62
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(viewArrow)

  const referenceGeometry = new THREE.BufferGeometry().setFromPoints([
    target,
    target.clone().add(new THREE.Vector3(0, orbitalRadius, 0))
  ])
  const reference = new THREE.Line(referenceGeometry, new THREE.LineDashedMaterial({
    color: 0x82a9cd, dashSize: satelliteSize * 0.28, gapSize: satelliteSize * 0.18,
    transparent: true, opacity: 0.5, depthTest: false, depthWrite: false
  }))
  reference.computeLineDistances()
  reference.name = 'parallel-zenith-reference'
  reference.renderOrder = 38
  group.add(reference)

  if (zenith > 0.002) {
    const arcPoints = []
    const arcRadius = orbitalRadius * 0.24
    for (let index = 0; index <= 28; index += 1) {
      const angle = zenith * index / 28
      arcPoints.push(target.clone().add(new THREE.Vector3(
        Math.sin(angle) * Math.cos(azimuth) * arcRadius,
        Math.cos(angle) * arcRadius,
        Math.sin(angle) * Math.sin(azimuth) * arcRadius
      )))
    }
    const arc = new THREE.Line(
      new THREE.BufferGeometry().setFromPoints(arcPoints),
      new THREE.LineBasicMaterial({ color: 0xc28aa6, transparent: true, opacity: 0.6, depthTest: false, depthWrite: false })
    )
    arc.name = 'parallel-view-zenith-arc'
    arc.renderOrder = 40
    group.add(arc)
  }

  const groundY = 0.075
  const azimuthDirection = new THREE.Vector3(Math.cos(azimuth), 0, Math.sin(azimuth)).normalize()
  const azimuthLength = THREE.MathUtils.clamp(extent * 0.2, 3.6, 8.5)
  const azimuthArrow = new THREE.ArrowHelper(
    azimuthDirection, new THREE.Vector3(0, groundY, 0), azimuthLength,
    0x78a799, satelliteSize * 0.3, satelliteSize * 0.17
  )
  azimuthArrow.name = 'parallel-view-azimuth'
  azimuthArrow.traverse((child) => {
    child.renderOrder = 39
    if (child.material) {
      child.material.transparent = true
      child.material.opacity = 0.62
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(azimuthArrow)

  const batchSuffix = angles.length > 1 ? ` · 代表方向 1/${angles.length}` : ''
  const label = createDimensionLabel(
    `卫星观测 Z${formatSceneDimension(zenithDegrees)}° A${formatSceneDimension(azimuthValue)}°${batchSuffix}`,
    THREE.MathUtils.clamp(extent * 0.17, 4.2, 7.6)
  )
  label.name = 'parallel-observation-label'
  label.position.copy(satellitePosition)
  label.position.y += satelliteSize * 1.25
  label.renderOrder = 42
  group.add(label)

  group.userData = { satellite, viewArrow, zenithDegrees, azimuthDegrees: azimuthValue }
  return group
}

function createObservationFootprint() {
  const group = new THREE.Group()
  group.name = 'sensor-observation-footprint'

  const fillGeometry = new THREE.BufferGeometry()
  fillGeometry.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(12), 3))
  fillGeometry.setIndex([0, 1, 2, 0, 2, 3])
  const fill = new THREE.Mesh(fillGeometry, new THREE.MeshBasicMaterial({
    color: 0xc28aa6,
    transparent: true,
    opacity: 0.08,
    side: THREE.DoubleSide,
    depthWrite: false
  }))
  fill.renderOrder = 32
  group.add(fill)

  const outlineGeometry = new THREE.BufferGeometry()
  outlineGeometry.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(15), 3))
  const outline = new THREE.Line(outlineGeometry, new THREE.LineBasicMaterial({
    color: 0xc28aa6,
    transparent: true,
    opacity: 0.62,
    depthTest: false,
    depthWrite: false
  }))
  outline.renderOrder = 37
  group.add(outline)

  const coneGeometry = new THREE.BufferGeometry()
  coneGeometry.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(24), 3))
  const cone = new THREE.LineSegments(coneGeometry, new THREE.LineBasicMaterial({
    color: 0xc28aa6,
    transparent: true,
    opacity: 0.36,
    depthTest: false,
    depthWrite: false
  }))
  cone.renderOrder = 36
  group.add(cone)

  return { group, fillGeometry, outlineGeometry, coneGeometry, label: null }
}

function updateObservationFootprint(data, origin, headingDegrees) {
  const footprint = data.footprint
  if (!footprint) return
  const groundY = 0.045
  const verticalHalfFov = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(data.verticalFov, 0.1, 120) * 0.5)
  const horizontalHalfFov = Math.atan(Math.tan(verticalHalfFov) * data.aspect)
  const viewAzimuth = THREE.MathUtils.degToRad(headingDegrees + data.relativeVaa)
  const forward = new THREE.Vector3(
    Math.sin(data.zenith) * Math.cos(viewAzimuth),
    -Math.cos(data.zenith),
    Math.sin(data.zenith) * Math.sin(viewAzimuth)
  ).normalize()
  const right = new THREE.Vector3(-Math.sin(viewAzimuth), 0, Math.cos(viewAzimuth))
  const up = right.clone().cross(forward).normalize()
  const halfSceneWidth = data.sceneX * data.displayScale * 0.5
  const halfSceneDepth = data.sceneY * data.displayScale * 0.5
  const farDistance = Math.max(halfSceneWidth, halfSceneDepth) * 4
  const corners = [[-1, 1], [1, 1], [1, -1], [-1, -1]].map(([horizontal, vertical]) => {
    const ray = forward.clone()
      .addScaledVector(right, horizontal * Math.tan(horizontalHalfFov))
      .addScaledVector(up, vertical * Math.tan(verticalHalfFov))
      .normalize()
    const distance = ray.y < -0.0001
      ? Math.max(0, (origin.y - groundY) / -ray.y)
      : farDistance
    const point = origin.clone().addScaledVector(ray, distance)
    point.set(
      THREE.MathUtils.clamp(point.x, -halfSceneWidth, halfSceneWidth),
      groundY,
      THREE.MathUtils.clamp(point.z, -halfSceneDepth, halfSceneDepth)
    )
    return point
  })

  const fillPositions = footprint.fillGeometry.getAttribute('position')
  corners.forEach((point, index) => fillPositions.setXYZ(index, point.x, point.y, point.z))
  fillPositions.needsUpdate = true

  const outlinePositions = footprint.outlineGeometry.getAttribute('position')
  ;[...corners, corners[0]].forEach((point, index) => outlinePositions.setXYZ(index, point.x, point.y, point.z))
  outlinePositions.needsUpdate = true

  const conePositions = footprint.coneGeometry.getAttribute('position')
  corners.forEach((point, index) => {
    conePositions.setXYZ(index * 2, origin.x, origin.y, origin.z)
    conePositions.setXYZ(index * 2 + 1, point.x, point.y, point.z)
  })
  conePositions.needsUpdate = true

  const center = corners.reduce((result, point) => result.add(point), new THREE.Vector3()).multiplyScalar(0.25)
  if (!footprint.label) {
    const width = (corners[0].distanceTo(corners[1]) + corners[3].distanceTo(corners[2])) * 0.5 / data.displayScale
    const length = (corners[0].distanceTo(corners[3]) + corners[1].distanceTo(corners[2])) * 0.5 / data.displayScale
    const labelWidth = THREE.MathUtils.clamp(Math.max(data.sceneX, data.sceneY) * data.displayScale * 0.18, 3.2, 8)
    footprint.label = createDimensionLabel(`视场约 ${formatSceneDimension(width)} × ${formatSceneDimension(length)} m`, labelWidth)
    footprint.label.name = 'sensor-observation-footprint-label'
    footprint.label.renderOrder = 39
    footprint.group.add(footprint.label)
  }
  footprint.label.position.copy(center)
  footprint.label.position.y = groundY + Math.max(0.12, data.aircraftSize * 0.14)
}

function updateCruiseVisualization(config) {
  if (cruiseVisualization) {
    world.remove(cruiseVisualization)
    disposeObject(cruiseVisualization)
    cruiseVisualization = null
  }
  const sensor = config?.sensor
  if (!sensor) return
  if (sensor.projection !== 'perspective') {
    cruiseVisualization = createParallelObservationVisualization(config)
    world.add(cruiseVisualization)
    return
  }

  const cruiseEnabled = Boolean(sensor.cruiseEnabled)
  const fixedHeading = ((Number(sensor.vaa) || 0) % 360 + 360) % 360
  const positions = cruiseEnabled
    ? sensorCruisePositions(sensor, config.scene)
    : [{ x: Number(sensor.positionX), y: Number(sensor.positionY), z: Number(sensor.height), heading: fixedHeading }]
  if (!positions.length) return
  const sceneX = Math.max(0.1, Number(config.scene?.x) || 60)
  const sceneY = Math.max(0.1, Number(config.scene?.y) || 60)
  const displayScale = world.userData.displayScale || Math.min(1, 45 / Math.max(sceneX, sceneY))
  const worldPoints = positions.map((point) => cruisePointToWorld(point, config))
  const group = new THREE.Group()
  group.name = 'sensor-perspective-visualization'

  const segmentVertices = []
  for (let index = 1; cruiseEnabled && index < positions.length; index += 1) {
    const previous = positions[index - 1]
    const current = positions[index]
    if (Math.abs(current.x - previous.x) > sceneX * 0.5 || Math.abs(current.y - previous.y) > sceneY * 0.5) continue
    segmentVertices.push(...worldPoints[index - 1].toArray(), ...worldPoints[index].toArray())
  }
  if (segmentVertices.length) {
    const geometry = new THREE.BufferGeometry()
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(segmentVertices, 3))
    const line = new THREE.LineSegments(geometry, new THREE.LineBasicMaterial({ color: 0x82a9cd, transparent: true, opacity: 0.58, depthTest: false, depthWrite: false }))
    line.renderOrder = 33
    group.add(line)
  }

  if (cruiseEnabled) {
    const maximumMarkers = 512
    const markerStep = Math.max(1, Math.ceil(worldPoints.length / maximumMarkers))
    const markerPoints = worldPoints.filter((_, index) => index % markerStep === 0 || index === worldPoints.length - 1)
    const markerGeometry = new THREE.BufferGeometry().setFromPoints(markerPoints)
    const markers = new THREE.Points(markerGeometry, new THREE.PointsMaterial({ color: 0xd9ad79, transparent: true, opacity: 0.68, size: 4.5, sizeAttenuation: false, depthTest: false, depthWrite: false }))
    markers.renderOrder = 34
    group.add(markers)
  }

  const aircraftSize = THREE.MathUtils.clamp(Math.max(sceneX, sceneY) * displayScale * 0.024, 0.52, 1.25)
  const aircraft = createCruiseAircraft(aircraftSize, Number(positions[0].heading) || 0)
  aircraft.position.copy(worldPoints[0])
  group.add(aircraft)

  const zenith = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(Number(sensor.vza) || 0, 0, 89.999))
  const relativeVaa = cruiseEnabled ? Number(sensor.vaa) || 0 : 0
  const relativeAzimuth = Number(positions[0].heading) + relativeVaa
  const azimuth = THREE.MathUtils.degToRad(relativeAzimuth || 0)
  const viewDirection = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    -Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  const heightDisplay = Math.max(0.01, Number(sensor.height) * displayScale)
  const arrowLength = THREE.MathUtils.clamp(heightDisplay / Math.max(0.12, Math.cos(zenith)) * 0.7, aircraftSize * 1.1, aircraftSize * 4)
  const viewArrow = new THREE.ArrowHelper(viewDirection, worldPoints[0], arrowLength, 0xc28aa6, aircraftSize * 0.28, aircraftSize * 0.16)
  viewArrow.traverse((child) => { child.renderOrder = 36; if (child.material) { child.material.transparent = true; child.material.opacity = 0.62; child.material.depthTest = false; child.material.depthWrite = false } })
  group.add(viewArrow)

  const footprint = createObservationFootprint()
  group.add(footprint.group)
  group.userData = {
    positions,
    worldPoints,
    aircraft,
    viewArrow,
    sceneX,
    sceneY,
    displayScale,
    aircraftSize,
    zenith,
    relativeVaa,
    verticalFov: Number(sensor.fov) || 60,
    aspect: Math.max(0.01, Number(sensor.x) || 1) / Math.max(0.01, Number(sensor.y) || 1),
    footprint,
    startedAt: performance.now(),
    duration: THREE.MathUtils.clamp(Math.max(1, positions.length - 1) * 0.5, 2.5, 12)
  }
  updateObservationFootprint(group.userData, worldPoints[0], Number(positions[0].heading) || 0)
  cruiseVisualization = group
  world.add(group)
}

function updateCruiseAnimation(now) {
  const data = cruiseVisualization?.userData
  if (!data?.aircraft || !data.positions?.length) return
  const count = data.positions.length
  let point = data.positions[0]
  let currentHeading = Number(point.heading) || 0
  if (count > 1) {
    const startedAt = Number.isFinite(Number(data.startedAt)) ? Number(data.startedAt) : now
    const duration = Math.max(0.001, Number(data.duration) || 1)
    const elapsedSeconds = Math.max(0, now - startedAt) / 1000
    const progress = (elapsedSeconds % duration) / duration * (count - 1)
    const index = Math.max(0, Math.min(count - 2, Math.floor(progress)))
    const fraction = progress - index
    const start = data.positions[index] || data.positions[0]
    const end = data.positions[index + 1] || start
    currentHeading = Number(start.heading) || 0
    const wrappedLerp = (a, b, extent) => {
      let delta = b - a
      if (delta > extent * 0.5) delta -= extent
      if (delta < -extent * 0.5) delta += extent
      return ((a + delta * fraction) % extent + extent) % extent
    }
    point = {
      x: wrappedLerp(start.x, end.x, data.sceneX),
      y: wrappedLerp(start.y, end.y, data.sceneY),
      z: THREE.MathUtils.lerp(start.z, end.z, fraction)
    }
  }
  const worldPoint = cruisePointToWorld(point, state.config)
  if (count > 1) worldPoint.y += Math.sin(now * 0.004) * data.aircraftSize * 0.035
  data.aircraft.position.copy(worldPoint)
  data.aircraft.rotation.y = THREE.MathUtils.degToRad(currentHeading + 90)
  data.viewArrow.position.copy(worldPoint)
  const viewAzimuth = THREE.MathUtils.degToRad(currentHeading + data.relativeVaa)
  data.viewArrow.setDirection(new THREE.Vector3(
    Math.sin(data.zenith) * Math.cos(viewAzimuth),
    -Math.cos(data.zenith),
    Math.sin(data.zenith) * Math.sin(viewAzimuth)
  ).normalize())
  updateObservationFootprint(data, worldPoint, currentHeading)
}

function material(color, roughness = .82) {
  const result = new THREE.MeshStandardMaterial({
    color,
    roughness,
    metalness: .02,
    transparent: false,
    opacity: 1,
    depthTest: true,
    depthWrite: true,
    alphaTest: 0,
    alphaToCoverage: false
  })
  result.userData.baseColor = result.color.getHex()
  return result
}

function generateWorld(config = defaultConfig()) {
  clearWorld()
  const sx = Math.max(0.1, Number(config.scene.x) || 60)
  const sz = Math.max(0.1, Number(config.scene.y) || 60)
  const displayScale = Math.min(1, 45 / Math.max(sx, sz))
  const width = sx * displayScale
  const depth = sz * displayScale
  world.userData.extent = Math.max(width, depth)
  world.userData.displayScale = displayScale
  const dem = config.scene.terrain ? config.scene.demInfo : null
  const demWidth = Math.max(2, Math.round(Number(dem?.terrainWidth) || 0))
  const demHeight = Math.max(2, Math.round(Number(dem?.terrainHeight) || 0))
  const demValues = Array.isArray(dem?.terrainValues) ? dem.terrainValues : []
  const hasDemSurface = demValues.length === demWidth * demHeight
  const sceneVoxel = Math.max(.1, Number(config.scene.voxel) || 1)
  const requestedColumns = Math.max(2, Math.ceil(sx / sceneVoxel) + 1)
  const requestedRows = Math.max(2, Math.ceil(sz / sceneVoxel) + 1)
  const previewScale = Math.min(1, 128 / Math.max(requestedColumns, requestedRows))
  const terrainColumns = Math.max(2, Math.round(requestedColumns * previewScale))
  const terrainRows = Math.max(2, Math.round(requestedRows * previewScale))
  const groundGeo = hasDemSurface
    ? new THREE.PlaneGeometry(width, depth, terrainColumns - 1, terrainRows - 1)
    : new THREE.PlaneGeometry(width, depth, 36, 36)
  const positions = groundGeo.attributes.position
  const baseElevation = Number.isFinite(Number(dem?.minimum)) ? Number(dem.minimum) : 0
  const demSample = (u, v) => {
    const imageX = THREE.MathUtils.clamp(u, 0, 1) * (demWidth - 1)
    const imageY = THREE.MathUtils.clamp(v, 0, 1) * (demHeight - 1)
    const x0 = Math.floor(imageX), y0 = Math.floor(imageY)
    const x1 = Math.min(demWidth - 1, x0 + 1), y1 = Math.min(demHeight - 1, y0 + 1)
    const at = (x, y) => {
      const source = demValues[y * demWidth + x]
      const value = source == null ? Number.NaN : Number(source)
      return Number.isFinite(value) ? value : baseElevation
    }
    const top = THREE.MathUtils.lerp(at(x0, y0), at(x1, y0), imageX - x0)
    const bottom = THREE.MathUtils.lerp(at(x0, y1), at(x1, y1), imageX - x0)
    return THREE.MathUtils.lerp(top, bottom, imageY - y0)
  }
  for (let i = 0; i < positions.count; i++) {
    // After the -90° X rotation, world +X is north and world +Z is east.
    // GeoTIFF columns grow eastward and row zero is the northern edge.
    const u = depth > 0 ? .5 - positions.getY(i) / depth : .5
    const v = width > 0 ? .5 - positions.getX(i) / width : .5
    const sourceElevation = hasDemSurface ? demSample(u, v) : baseElevation
    const elevation = Number.isFinite(sourceElevation) ? sourceElevation : baseElevation
    positions.setZ(i, hasDemSurface ? Math.max(0, elevation - baseElevation) * displayScale : 0)
  }
  groundGeo.computeVertexNormals()
  const ground = new THREE.Mesh(groundGeo, material(0xcbd8e6, .96))
  ground.rotation.x = -Math.PI / 2
  ground.receiveShadow = true
  ground.userData.kind = 'ground'
  world.add(ground)
  grid = new THREE.GridHelper(Math.max(width, depth), Math.max(8, Math.round(Math.max(width, depth) / 1.5)), 0x7895b5, 0xb8c6d6)
  grid.position.y = .012
  grid.material.opacity = .23
  grid.material.transparent = true
  world.add(grid)

  updateSceneBoundsGuide(config)
  updateCruiseVisualization(config)
  applyViewStyle(currentStyle)
  updateStats()
}

function updateSun(config) {
  const zenithDegrees = THREE.MathUtils.clamp(Number(config.light.zenith) || 0, 0, 90)
  const azimuthDegrees = ((Number(config.light.azimuth) || 0) % 360 + 360) % 360
  const zenith = THREE.MathUtils.degToRad(zenithDegrees)
  const azimuth = THREE.MathUtils.degToRad(azimuthDegrees)
  const lightDirection = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  sun.position.copy(lightDirection).multiplyScalar(45)
  sun.intensity = 1.2 + config.light.direct * 2.5

  if (sunVisualization) {
    scene.remove(sunVisualization)
    disposeObject(sunVisualization)
  }
  const extent = Math.max(12, Number(world.userData.extent) || 45)
  const sceneHeight = Math.max(0, Number(world.userData.boundsHeight) || 0)
  const radius = THREE.MathUtils.clamp(extent * 0.54, 11, 28)
  const center = new THREE.Vector3(0, sceneHeight * 0.35, 0)
  const sunPosition = center.clone().addScaledVector(lightDirection, radius)
  const group = new THREE.Group()
  group.name = 'sun-direction-visualization'

  const sunSize = THREE.MathUtils.clamp(extent * 0.024, 0.45, 1.05)
  const sphere = new THREE.Mesh(
    new THREE.SphereGeometry(sunSize, 24, 14),
    new THREE.MeshBasicMaterial({ color: 0xf1d57c, transparent: true, opacity: 0.84, depthTest: false, depthWrite: false })
  )
  sphere.position.copy(sunPosition)
  sphere.renderOrder = 44
  group.add(sphere)

  const glowCanvas = document.createElement('canvas')
  glowCanvas.width = glowCanvas.height = 192
  const glowContext = glowCanvas.getContext('2d')
  const glow = glowContext.createRadialGradient(96, 96, 18, 96, 96, 92)
  glow.addColorStop(0, 'rgba(255, 248, 205, 0.74)')
  glow.addColorStop(0.34, 'rgba(244, 215, 132, 0.44)')
  glow.addColorStop(1, 'rgba(237, 196, 101, 0)')
  glowContext.fillStyle = glow
  glowContext.fillRect(0, 0, 192, 192)
  const glowTexture = new THREE.CanvasTexture(glowCanvas)
  glowTexture.colorSpace = THREE.SRGBColorSpace
  const glowSprite = new THREE.Sprite(new THREE.SpriteMaterial({ map: glowTexture, transparent: true, depthTest: false, depthWrite: false }))
  glowSprite.position.copy(sunPosition)
  glowSprite.scale.setScalar(sunSize * 3.5)
  glowSprite.renderOrder = 43
  group.add(glowSprite)

  const incomingDirection = center.clone().sub(sunPosition).normalize()
  const arrow = new THREE.ArrowHelper(incomingDirection, sunPosition, radius * 0.72, 0xd8ba73, sunSize * 0.76, sunSize * 0.4)
  arrow.name = 'sun-incoming-direction'
  arrow.traverse((child) => {
    child.renderOrder = 42
    if (child.material) {
      child.material.transparent = true
      child.material.opacity = 0.58
      child.material.depthTest = false
      child.material.depthWrite = false
    }
  })
  group.add(arrow)

  const label = createDimensionLabel(`太阳 Z${formatSceneDimension(zenithDegrees)}° A${formatSceneDimension(azimuthDegrees)}°`, THREE.MathUtils.clamp(extent * 0.11, 3.2, 5.4))
  label.position.copy(sunPosition)
  label.position.y += sunSize * 1.75
  label.renderOrder = 45
  group.add(label)

  sunVisualization = group
  scene.add(group)
}

const thermalBaseColors = new WeakMap()

function applyViewStyle(style) {
  currentStyle = style
  world.traverse((child) => {
    if (!child.isMesh || child === voxelPreview) return
    const materials = Array.isArray(child.material) ? child.material : [child.material]
    for (const item of materials) {
      if (!item?.color) continue
      item.wireframe = style === 'wireframe'
      if (style === 'thermal') {
        if (!thermalBaseColors.has(item)) thermalBaseColors.set(item, item.color.clone())
        const colors = { vegetation: 0xe7b84a, building: 0xd3544c, ground: 0x4c72bd, sensor: 0xe8ebe9 }
        item.color.setHex(colors[child.userData.kind] || 0x61b68e)
      } else if (thermalBaseColors.has(item)) {
        item.color.copy(thermalBaseColors.get(item))
        thermalBaseColors.delete(item)
      } else if (item.userData.baseColor != null) item.color.setHex(item.userData.baseColor)
    }
  })
}

function fitCamera() {
  const extent = world.userData.extent || 20
  const height = world.userData.boundsHeight || 2
  camera.position.set(extent * 1.04, Math.max(extent * .86, height * 1.35), extent * 1.14)
  controls.target.set(0, Math.min(height * .36, extent * .25), 0)
  controls.update()
}

function updateStats() {
  let triangles = 0
  world.traverse((object) => {
    if (!object.isMesh || !object.geometry) return
    const count = object.geometry.index ? object.geometry.index.count / 3 : object.geometry.attributes.position.count / 3
    triangles += count * (object.isInstancedMesh ? object.count : 1)
  })
  $('#triangles').textContent = `${Math.round(triangles / 100) / 10}K TRI`
}

function resize() {
  renderer.setSize(viewport.clientWidth, viewport.clientHeight, false)
  camera.aspect = viewport.clientWidth / Math.max(1, viewport.clientHeight)
  camera.updateProjectionMatrix()
}
new ResizeObserver(resize).observe(viewport)
let frameCount = 0, lastFps = performance.now(), lastAnimationTime = performance.now()
function animate(now) {
  requestAnimationFrame(animate)
  const deltaSeconds = Math.min(.1, Math.max(0, (now - lastAnimationTime) / 1000))
  lastAnimationTime = now
  controls.update(); updateSceneDynamics(world, deltaSeconds, state.config); updateCruiseAnimation(now); updateObjectSelectionBox(); renderer.render(scene, camera); renderResultViewer(); frameCount++
  if (now - lastFps > 750) {
    $('#fps').textContent = `${Math.round(frameCount * 1000 / (now - lastFps))} FPS`
    frameCount = 0; lastFps = now
  }
}
requestAnimationFrame(animate)

// 属性面板
const panelTitles = { scene: '场景', light: '太阳与天空', sensor: '传感器', objects: '场景对象', materials: '材质属性', atmosphere: '大气影响', meteorology: '气象驱动', fluid: '流体力学', simulation: '仿真模拟', 'shared-geo': '地理与时间', 'shared-dem': 'DEM 地形' }
function inputUnit(id, value, unit, attrs = '') { return `<div class="input-with-unit"><input class="input" id="${id}" value="${escapeHtml(value)}" ${attrs}><span>${unit}</span></div>` }
function field(label, content) { return `<div class="field"><label>${t(label)}</label>${content}</div>` }
function switchRow(label, id, on, disabled = false) { return `<div class="switch-row"><span>${t(label)}</span><button type="button" class="switch ${on ? 'on' : ''}" id="${id}" role="switch" aria-checked="${on}" ${disabled ? 'disabled' : ''}></button></div>` }
function builtinNotice() { return state.inputPath ? '' : '<div class="property-group"><div class="notice">尚未打开工程。请点击“打开工程”，选择已有工程的 project.json。</div></div>' }

function libraryCard(title, subtitle, values, kind, index) {
  const detail = values.map(([key, value]) => `${key}: ${value}`).join('；')
  const search = `${title} ${subtitle} ${detail}`.toLowerCase()
  return `<button class="library-card" type="button" data-library-search="${escapeHtml(search)}" data-preset-kind="${kind}" data-preset-index="${index}" title="点击修改 · ${escapeHtml(`${title} · ${subtitle}\n${detail}`)}"><strong>${escapeHtml(title)}</strong><small>${escapeHtml(subtitle)}</small><div class="library-values">${values.map(([key, value]) => `<span><i>${escapeHtml(key)}</i>${escapeHtml(value)}</span>`).join('')}</div></button>`
}

function physicalValues(item) {
  const p = item.params || {}
  if (item.energyModel === 'photovoltaic') return [['模型','光伏热电耦合'],['参考效率',p.eta25],['温度系数',p.gamma],['背面系数',p.bifaciality],['面热容',p.heatCapacityPerArea],['对流倍率',p.convectiveScale]]
  if (item.energyModel === 'wood') return [['模型', '木质固体'], ['比热容', p.cs], ['密度', p.rhos], ['导热率', p.lambdas], ['对流倍率', p.convectiveScale], ['初温', `${p.Tsoil} ℃`], ['蒸腾', '关闭']]
  if (item.energyModel === 'membrane') return [['模型', '双面薄膜'], ['面热容', p.heatCapacityPerArea], ['对流倍率', p.convectiveScale], ['蒸腾', '关闭']]
  if (item.type === 'Vegetation') return [['Vcmax', p.Vcmax], ['BallBerry', p.BallBerry], ['kV', p.kV], ['Rd', p.Rdparam], ['Tyear', p.Tyear], ['胁迫', p.stressfactor]]
  if (item.type === 'Water') return [['辐射模型', Number(p.brdfModel) === 1 ? 'Cox–Munk' : 'Lambert'], ['折射率', p.refractiveIndex], ['坡度方差', Number(p.slopeVariance) > 0 ? p.slopeVariance : '随风速'], ['漫反射比例', p.diffuseFraction], ['rss', p.rss], ['热容', p.heatCapacity], ['混合深度', p.mixingDepth], ['蒸发系数', p.evaporationCoefficient]]
  return [['rss', p.rss], ['热容', p.cs], ['密度', p.rhos], ['导热率', p.lambdas], ['SMC', p.SMC], ['饱和量', p.Satwater]]
}

function canopyValues(item) {
  const labels = { canopy: '混沌介质', rigid: '刚体', fire: '火焰', fog: '雾' }
  if (item.structureType === 'fire' || item.structureType === 'fog') return [
    ['结构', labels[item.structureType]], ['消光', `${item.extinction} m⁻¹`], ['散射率', item.scatteringAlbedo],
    ['非对称', item.asymmetry], ['温度', `${item.fixedTemperature} K`], ['发射倍率', item.emissionScale]
  ]
  return [
    ['结构', labels[item.structureType] || '混沌介质'],
    ['LAI', item.lai], ['LAD', item.density], ['高度', `${item.hc} m`],
    ['G', item.G], ['热点', item.hspot]
  ]
}

function libraryHeading(title, count, kind) {
  return `<div class="library-heading"><h3>${escapeHtml(title)} · ${count}</h3><div class="library-actions"><input class="input library-filter" data-library-filter="${kind}" placeholder="筛选 ${escapeHtml(title)}"><button class="button ghost library-add" type="button" data-preset-kind="${kind}"><svg><use href="#i-plus"/></svg>新增</button></div></div>`
}

function materialLibraryHtml(c) {
  const previewLabels = Object.fromEntries(PREVIEW_TEXTURE_PRESETS)
  const spectra = c.spectra.map((item, index) => libraryCard(item.label || item.name, `${item.name} · ${item.model}`, [['反射率', item.reflectance], ['透射率', item.transmittance], ['TIR反射', item.refTir], ...(item.physicalTexture?.enabled ? [['物理纹理', Number(item.physicalTexture.relativeStd) > 0 ? `CV ${item.physicalTexture.relativeStd}` : `强度 ${item.physicalTexture.strength}`]] : []), ...(Number(item.openness) > 0 ? [['开孔率', item.openness]] : []), ...(item.classTexture?.enabled ? [['分类纹理', `${item.classTexture.classes.filter((entry) => entry.spectralName).length} 类`]] : []), ...(item.opennessTexture?.enabled ? [['开孔纹理', `${item.opennessTexture.minOpenness}–${item.opennessTexture.maxOpenness}`]] : []), ['预览', item.previewTexture?.enabled ? (previewLabels[item.previewTexture.preset] || item.previewTexture.preset) : '关闭']], 'spectrum', index)).join('')
  const thermals = c.thermals.map((item, index) => libraryCard(item.label || item.name, item.name, [['阳面', `${item.sunlitTemperature} K`], ['阴面', `${item.shadedTemperature} K`]], 'thermal', index)).join('')
  const canopies = c.canopies.map((item, index) => libraryCard(item.label || item.name, item.name, canopyValues(item), 'canopy', index)).join('')
  const types = { Vegetation: '植被生理生化', Soil: '土壤表面物化', Water: '水体物性', Ship: '船舶表面物化' }
  const materials = c.materials.map((item, index) => libraryCard(item.label || item.name, `${item.name} · ${item.energyModel === 'wood' ? '木质固体热平衡' : item.energyModel === 'membrane' ? '篷布/织物薄膜' : (types[item.type] || item.type)}`, physicalValues(item), 'physical', index)).join('')
  return `<div class="property-group">${libraryHeading('光谱特征', c.spectra.length, 'spectrum')}<div class="library-list" data-library-list="spectrum">${spectra}</div></div><div class="property-group">${libraryHeading('温度特征', c.thermals.length, 'thermal')}<div class="library-list" data-library-list="thermal">${thermals}</div></div><div class="property-group">${libraryHeading('结构参数', c.canopies.length, 'canopy')}<div class="library-list" data-library-list="canopy">${canopies}</div></div><div class="property-group">${libraryHeading('物化属性', c.materials.length, 'physical')}<div class="library-list" data-library-list="physical">${materials}</div></div><div class="property-group"><div class="notice">点击任一属性可直接修改；保存后同步更新当前工程的 project.json。</div></div>`
}

function isFacetMode(mode = state.mode) { return mode === 'eFacetRT' || mode === 'eFacetEB' }

function selectedBackend(control) {
  const value = isFacetMode() ? control.facetBackend : control.computeBackend
  return ['cpu', 'auto'].includes(value) ? value : 'gpu'
}

function renderComputeDevices() {
  const select = $('#computeDevice')
  const control = state.config?.control
  if (!select || !control) return
  const facet = isFacetMode()
  const backend = selectedBackend(control)
  const gpuIndex = Math.max(0, Math.round(Number(control.gpu) || 0))
  const report = state.devices
  const indexKey = facet ? 'facetIndex' : 'voxelIndex'
  const options = []
  for (const gpu of (report?.gpus || []).filter((item) => item[indexKey] >= 0).sort((a, b) => a[indexKey] - b[indexKey])) {
    options.push({ value: `gpu:${gpu[indexKey]}`, label: `GPU ${gpu[indexKey]} · ${gpu.name}` })
  }
  if (!options.some((item) => item.value === `gpu:${gpuIndex}`)) {
    options.push({ value: `gpu:${gpuIndex}`, label: `GPU ${gpuIndex}${report ? `（${t('未检测到')}）` : ''}` })
  }
  for (const gpu of (report?.gpus || []).filter((item) => item[indexKey] < 0)) {
    options.push({ value: '', label: `${gpu.name}（${facet ? t('不满足面元 GPU 要求') : t('不支持硬件光线追踪')}）`, disabled: true })
  }
  options.push({ value: 'cpu', label: facet ? `CPU · ${t('原生多线程栅格')}` : `CPU · Mesa lavapipe${report && !report.cpu?.lavapipe ? `（${t('驱动未就绪')}）` : ''}` })
  options.push({ value: 'auto', label: `${t('自动')} · GPU ${gpuIndex}，${t('不可用时回退 CPU')}` })
  select.innerHTML = options.map((item) => `<option value="${item.value}"${item.disabled ? ' disabled' : ''}>${escapeHtml(item.label)}</option>`).join('')
  select.value = backend === 'gpu' ? `gpu:${gpuIndex}` : backend
  $('#cpuThreadsRow').hidden = backend === 'gpu'
  const processors = report?.logicalProcessors
  const threads = processors ? Math.min(normalizeCpuThreads(control.cpuThreads), processors) : normalizeCpuThreads(control.cpuThreads)
  $('#cpuThreads').max = String(processors || 1024)
  $('#cpuThreads').value = String(threads)
  const lavapipeLimit = report?.lavapipeThreadLimit || 32
  const used = facet ? threads : Math.min(threads, lavapipeLimit)
  let hint = ''
  if (state.devicesLoading) hint = t('正在检测计算设备…')
  else if (state.devicesError) hint = `${t('设备检测失败')}：${state.devicesError}`
  else if (backend !== 'gpu') {
    hint = `${t('多核并行')}：${used} ${t('线程')}（${t('范围')} 1–${processors || 1024}${processors ? `，${t('本机')} ${processors} ${t('逻辑处理器')}` : ''}）`
    if (used < threads) hint += `。${t('当前 lavapipe 驱动最多使用')} ${lavapipeLimit} ${t('线程')}`
  }
  $('#computeDeviceHint').textContent = hint
}

let deviceRequest = 0
async function loadComputeDevices(refresh = false) {
  const request = ++deviceRequest
  state.devicesLoading = true; state.devicesError = ''; renderComputeDevices()
  let devices = null
  let devicesError = ''
  try { devices = await api.devices(state.executable, refresh) }
  catch (error) { devicesError = error.message }
  if (request !== deviceRequest) return
  state.devices = devices; state.devicesError = devicesError; state.devicesLoading = false
  renderComputeDevices()
}

function remapGpuIndex(previousMode, nextMode) {
  const control = state.config?.control
  const facetModes = ['eFacetRT', 'eFacetEB']
  const fromKey = facetModes.includes(previousMode) ? 'facetIndex' : 'voxelIndex'
  const toKey = facetModes.includes(nextMode) ? 'facetIndex' : 'voxelIndex'
  if (!control || fromKey === toKey || !state.devices?.gpus) return
  const current = Math.max(0, Math.round(Number(control.gpu) || 0))
  const gpu = state.devices.gpus.find((item) => item[fromKey] === current)
  if (gpu && gpu[toKey] >= 0 && gpu[toKey] !== current) {
    control.gpu = gpu[toKey]
    updateXml()
  }
}

function renderInspector() {
  renderComputeDevices()
  const c = state.config || defaultConfig()
  $('#inspectorTitle').textContent = t(panelTitles[state.panel])
  const background = c.scene.background || {}
  const dem = c.scene.demInfo
  const demNumber = (value, digits = 3) => Number.isFinite(Number(value)) ? Number(value).toLocaleString(getLocale(), { maximumFractionDigits: digits }) : '—'
  const demPixelSize = dem ? `${demNumber(dem.pixelSizeX)} × ${demNumber(dem.pixelSizeY)} ${dem.unit || ''}`.trim() : '—'
  const sceneOffsetX = Number(c.scene.offsetX) || 0
  const sceneOffsetY = Number(c.scene.offsetY) || 0
  const sceneOffsetZ = Number(c.scene.offsetZ) || 0
  const demReliefWarning = dem && Number(dem.relief) > Number(c.scene.height)
    ? `<div class="notice warning">高差 ${demNumber(dem.relief)} m 超过场景最大高度 ${demNumber(c.scene.height)} m，请增大最大高度。</div>` : ''
  const demMetadata = dem ? `<div class="metric-grid"><div class="metric"><strong>${escapeHtml(dem.width)} × ${escapeHtml(dem.height)}</strong><small>栅格列 × 行</small></div><div class="metric"><strong>${escapeHtml(demPixelSize)}</strong><small>像元大小</small></div><div class="metric"><strong>${demNumber(dem.minimum)}–${demNumber(dem.maximum)} m</strong><small>最低–最高高程</small></div><div class="metric"><strong>${dem.noData == null ? '未设置' : demNumber(dem.noData)}</strong><small>NoData</small></div></div><div class="notice">整幅 DEM 自动双线性拉伸至 ${demNumber(c.scene.x)} × ${demNumber(c.scene.y)} m 场景范围。</div>${demReliefWarning}` : ''
  const angularStrength = Math.max(0, Math.min(1, Number(background.angularEffectStrength ?? 0.5)))
  const backgroundPanel = `<div class="property-group"><h3>背景属性</h3>${field('光谱属性', `<select class="select" id="backgroundSpectral">${bindingOptionList(c.spectra, background.spectralName, 'soil')}</select>`)}${field('温度属性', `<select class="select" id="backgroundThermal">${bindingOptionList(c.thermals, background.thermalName, 'soil_temperature')}</select>`)}${field('物化属性', `<select class="select" id="backgroundMaterial">${bindingOptionList(c.materials, background.materialName, 'soilset')}</select>`)}${switchRow('背景使用镜面反射', 'backgroundSpecularSwitch', Boolean(background.specularEnabled))}</div><div class="property-group"><h3>背景异质性</h3>${switchRow('启用 Hapke 角度效应', 'backgroundHeterogeneitySwitch', Boolean(background.heterogeneityEnabled))}${field('结构强度', `<div class="range-wrap"><input class="range" id="backgroundAngularStrength" type="range" min="0" max="1" step="0.01" value="${angularStrength}" ${background.heterogeneityEnabled ? '' : 'disabled'}><span class="range-value" id="backgroundAngularStrengthValue">${angularStrength.toFixed(2)}</span></div>`)}<div class="notice">光学波段使用 Hapke，热红外使用红外 Hapke；0 为各向同性，1 为完整角度效应。</div></div>`
  let html = ''
  // 三维工程输入：影像模拟（STREAM）使用独立工作台与独立工程，不共享这些值。
  const sharedNotice = `<div class="notice">这些输入只属于当前三维工程；影像模拟（STREAM）使用自己的工程，不读取这里的值。</div>`
  if (state.panel === 'shared-geo') {
    html = `<div class="property-group"><h3>站点位置</h3>${field('纬度', inputUnit('meteoLatitude', c.meteo.latitude, '°', 'type="number" min="-90" max="90" step="0.0001"'))}${field('经度', inputUnit('meteoLongitude', c.meteo.longitude, '°', 'type="number" min="-180" max="180" step="0.0001"'))}${field('时区', inputUnit('meteoTimezone', c.meteo.timezone, 'UTC±h', 'type="number" min="-12" max="14" step="0.25"'))}${field('年份', inputUnit('meteoYear', c.meteo.year, '年', 'type="number" min="-2000" max="6000" step="1"'))}<div class="notice">气象文件中的年积日按此年份和当地时区解释，用于计算太阳位置。</div></div>${sharedNotice}${builtinNotice()}`
  } else if (state.panel === 'shared-dem') {
    const environment = c.scene.terrainEnvironment || (c.scene.terrainEnvironment = normalizeTerrainEnvironment())
    const environmentFields = (rows) => rows.map(([key, label, unit, attributes]) =>
      field(label, inputUnit(`terrainEnv_${key}`, environment[key], unit, `type="number" ${attributes || 'step="any"'}`))).join('')
    const environmentPanel = `<div class="property-group"><h3>地形水热经验场 · VoxelEB</h3>
      ${switchRow('启用气温与水汽高度变化', 'terrainAirSwitch', environment.airEnabled)}
      ${environmentFields([
        ['referenceHeight', '气象参考高度', 'm'],
        ['temperatureLapseRate', '气温递减率', 'K/m'],
        ['vaporScaleHeight', '水汽压尺度高度', 'm', 'min="0.001" step="any"']
      ])}
      ${switchRow('启用土壤水高度变化', 'terrainSoilSwitch', environment.soilEnabled)}
      ${environmentFields([
        ['drainageHeight', '沟谷基准高度', 'm'],
        ['soilDry', '高坡位干端含水量', 'm³/m³', 'min="0" max="1" step="0.01"'],
        ['soilValley', '沟谷含水量', 'm³/m³', 'min="0" max="1" step="0.01"'],
        ['soilScaleHeight', '土壤水衰减尺度', 'm', 'min="0.001" step="any"']
      ])}
      <div class="notice">高度以 DEM 最低点为零；将气象站海拔减去 DEM 最低海拔后填入参考高度。正递减率表示随高度降温，负值表示逆温。水汽压按指数变化并限制在局地饱和值内，相对湿度由局地温度计算。土壤水采用指定的统一沟谷基准面，未自动提取河网；默认参数未经当地标定，不包含水量收支。</div>
      <div class="notice">T = Tref − Γ(h − href)；e = eref exp[−(h − href)/Hv]；θ = θdry + (θvalley − θdry) exp[−max(0, hground − hdrain)/L]。气温与水汽会进入局地蒸散和叶片生理；土壤水会影响蒸发阻力和土壤热传导。</div>
    </div>`
    html = `<div class="property-group"><h3>DEM 地形</h3>${switchRow('启用 DEM 地形', 'terrainSwitch', c.scene.terrain)}${c.scene.demFile ? `<div class="path-input"><span title="${escapeHtml(c.scene.demFile)}">${escapeHtml(c.scene.demFile)}</span></div>` : ''}${demMetadata}<label class="button ghost" for="demFile" style="width:100%;justify-content:center;margin-top:9px"><svg><use href="#i-import"/></svg>导入 DEM 文件<input id="demFile" type="file" accept=".tif,.tiff,.asc,image/tiff,text/plain" hidden></label></div><div class="property-group"><div class="notice">DEM 用于三维地形网格。</div></div>${environmentPanel}${sharedNotice}${builtinNotice()}`
  } else if (state.panel === 'scene') {
    const domainNotice = `Three.js 显示全部实例；运行时跳过完整 OBJ 包围盒与计算域 X=[${formatSceneDimension(sceneOffsetX)}, ${formatSceneDimension(sceneOffsetX + Number(c.scene.x))}]、Y=[${formatSceneDimension(sceneOffsetY)}, ${formatSceneDimension(sceneOffsetY + Number(c.scene.height))}]、Z=[${formatSceneDimension(sceneOffsetZ)}, ${formatSceneDimension(sceneOffsetZ + Number(c.scene.y))}] m 无交集的实例；跨界对象仅计算域内部分。`
    html = `<div class="property-group"><h3>空间范围</h3>${field('X 尺寸（南北）', inputUnit('sceneX', c.scene.x, 'm', 'type="number" min="1"'))}${field('Z 尺寸（东西）', inputUnit('sceneY', c.scene.y, 'm', 'type="number" min="1"'))}${field('Y 最大高度', inputUnit('sceneHeight', c.scene.height, 'm', 'type="number" min="0.1"'))}${field('体元大小', inputUnit('voxelSize', c.scene.voxel, 'm', 'type="number" min="0.1" step="0.1"'))}${field('OBJ填充阈值', inputUnit('voxelFillThreshold', c.scene.voxelFillThreshold, '0–1', 'type="number" min="0" max="1" step="0.01"'))}<div class="notice">统一方向：+X 向北、−X 向南；+Y 向上；+Z 向东、−Z 向西。OBJ、Three.js 与计算引擎采用同一约定；方位角 0° 向北、90° 向东；平行投影固定上北、右东，中心投影保持天空向上和地平线水平。</div></div><div class="property-group"><h3>计算域偏移</h3>${field('X 偏移（北向）', inputUnit('sceneOffsetX', sceneOffsetX, 'm', 'type="number" step="any"'))}${field('Y 偏移（高度）', inputUnit('sceneOffsetY', sceneOffsetY, 'm', 'type="number" step="any"'))}${field('Z 偏移（东向）', inputUnit('sceneOffsetZ', sceneOffsetZ, 'm', 'type="number" step="any"'))}<div class="notice">${domainNotice}</div></div>${backgroundPanel}${builtinNotice()}`
  } else if (state.panel === 'light') {
    html = `<div class="property-group"><h3>太阳位置</h3>${field('太阳天顶角', inputUnit('sunZenith', c.light.zenith, '°', 'type="number" min="0" max="90"'))}${field('太阳方位角', inputUnit('sunAzimuth', c.light.azimuth, '°', 'type="number" min="0" max="360"'))}</div>
      <div class="property-group"><h3>辐照条件</h3>${field('直射比例', `<div class="range-wrap"><input class="range" id="directLight" type="range" min="0" max="1" step="0.01" value="${c.light.direct}"><span class="range-value" id="directValue">${c.light.direct.toFixed(2)}</span></div>`)}${field('漫射比例', `<div class="range-wrap"><input class="range" id="diffuseLight" type="range" min="0" max="1" step="0.01" value="${c.light.diffuse}"><span class="range-value" id="diffuseValue">${c.light.diffuse.toFixed(2)}</span></div>`)}${field('天空温度', inputUnit('skyTemperature', c.light.skyTemperature, 'K', 'type="number"'))}<div class="notice">HiStream 自动令漫射比例 = 1 − 直射比例。</div></div>${builtinNotice()}`
  } else if (state.panel === 'sensor') {
    const perspective = c.sensor.projection === 'perspective'
    updateCruiseVisualization(c)
    const cruisePositions = perspective && c.sensor.cruiseEnabled ? sensorCruisePositions(c.sensor, c.scene) : []
    const previewIndices = cruisePositions.length <= 8
      ? cruisePositions.map((_, index) => index)
      : [0, 1, 2, 3, 4, cruisePositions.length - 2, cruisePositions.length - 1]
    const cruisePreview = cruisePositions.length
      ? `<div class="notice"><strong>预计巡航位置 · 共 ${cruisePositions.length} 个</strong><br>${previewIndices.map((index, previewIndex) => {
          const point = cruisePositions[index]
          const separator = previewIndex === 5 && cruisePositions.length > 8 ? '…　' : ''
          return `${separator}P${index + 1} · X/Y/Z (${demNumber(point.x)}, ${demNumber(point.z)}, ${demNumber(point.y)}) m`
        }).join('　')}</div>`
      : ''
    const cruiseRoute = ['line', 'rectangle', 'random'].includes(c.sensor.cruiseRoute) ? c.sensor.cruiseRoute : 'line'
    const cruiseRouteOptions = `<select class="input" id="sensorCruiseRoute"><option value="line" ${cruiseRoute === 'line' ? 'selected' : ''}>直线往返</option><option value="rectangle" ${cruiseRoute === 'rectangle' ? 'selected' : ''}>中心矩形</option><option value="random" ${cruiseRoute === 'random' ? 'selected' : ''}>随机飞行</option></select>`
    const cruiseRouteFields = cruiseRoute === 'line'
      ? `${field('目标位置 X（北向）', inputUnit('sensorCruiseTargetX', c.sensor.cruiseTargetX, 'm', 'type="number"'))}${field('目标位置 Z（东向）', inputUnit('sensorCruiseTargetY', c.sensor.cruiseTargetY, 'm', 'type="number"'))}${field('目标位置 Y（高度）', inputUnit('sensorCruiseTargetZ', c.sensor.cruiseTargetZ, 'm', 'type="number" min="0.01"'))}`
      : cruiseRoute === 'rectangle'
        ? `${field('矩形长度 X（南北）', inputUnit('sensorCruiseRectangleWidth', c.sensor.cruiseRectangleWidth, 'm', 'type="number" min="0.01"'))}${field('矩形宽度 Z（东西）', inputUnit('sensorCruiseRectangleHeight', c.sensor.cruiseRectangleHeight, 'm', 'type="number" min="0.01"'))}`
        : '<button class="button ghost" id="randomCruiseBtn" type="button" style="width:100%;justify-content:center;margin:8px 0"><svg><use href="#i-camera"/></svg>换一条随机路线</button>'
    const cruiseFields = c.sensor.cruiseEnabled
      ? `${field('巡航轨迹', cruiseRouteOptions)}${cruiseRouteFields}${field('航点间距', inputUnit('sensorCruiseStep', c.sensor.cruiseStep, 'm', 'type="number" min="0.01" step="any"'))}${field('航点数量', inputUnit('sensorCruiseCount', c.sensor.cruiseCount, '个', 'type="number" min="1" max="10000" step="1"'))}<div class="notice">直线轨迹在起点与目标点之间三维往返，高度随航程线性变化；矩形和随机轨迹保持当前高度。越界后从对侧继续。</div>${cruisePreview}`
      : ''
    const sensorHeightLabel = c.sensor.cruiseEnabled ? (cruiseRoute === 'line' ? '起始位置 Y（高度）' : '巡航高度 Y') : '观测位置 Y（高度）'
    const projectionFields = perspective
      ? `${field(cruiseRoute === 'rectangle' && c.sensor.cruiseEnabled ? '矩形中心 X（北向）' : '起始位置 X（北向）', inputUnit('sensorPositionX', c.sensor.positionX, 'm', 'type="number"'))}${field(cruiseRoute === 'rectangle' && c.sensor.cruiseEnabled ? '矩形中心 Z（东向）' : '起始位置 Z（东向）', inputUnit('sensorPositionY', c.sensor.positionY, 'm', 'type="number"'))}${field(sensorHeightLabel, inputUnit('sensorHeight', c.sensor.height, 'm', 'type="number" min="0.01"'))}${field('垂直视场角', inputUnit('sensorFov', c.sensor.fov, '°', 'type="number" min="0.1" max="120" step="0.1"'))}${switchRow('启用航点巡航', 'sensorCruiseSwitch', c.sensor.cruiseEnabled)}${cruiseFields}<div class="notice">视场方向见上方“观测方向”。</div>`
      : '<div class="notice">平行投影的观测方向与采样方式见上方“观测方向”。</div>'
    const bandCount = sensorBandValues(c.sensor).length
    const angleCount = perspective ? 1 : sensorViewAngles(c.sensor, c.light.azimuth).length
    const continuousFields = c.sensor.continuousBands
      ? `${field('起始波段', inputUnit('bandStart', c.sensor.bandStart, 'nm', 'type="number" min="0.000001" step="any"'))}${field('结束波段', inputUnit('bandEnd', c.sensor.bandEnd, 'nm', 'type="number" min="0.000001" step="any"'))}${field('波段间隔', inputUnit('bandStep', c.sensor.bandStep, 'nm', 'type="number" min="0.000001" step="any"'))}`
      : ''
    const projectionNotice = perspective
      ? (c.sensor.cruiseEnabled
          ? '相机方向随飞机航向旋转。天顶角 0° 表示垂直向下；相对飞机方位角 0° 向前、90° 向右、180° 向后、270° 向左。倾斜观测保持天空向上、地平线水平；正下视保持上北、右东。'
          : '中心投影使用观测位置、巡航高度、视场角和视场方向；天顶角 0° 表示垂直向下。倾斜观测保持天空向上、地平线水平；正下视保持上北、右东。')
      : `平行投影共 ${angleCount} 个观测方向。主平面批量范围为 −75°～75°、间隔 5°，同时包含太阳主平面和垂直太阳主平面；半球采用 128 个等面积方向并限制在 VZA 0°～75°。输出统一为上北、右东。`
    html = `<div class="property-group"><h3>观测方向</h3>${field('观测天顶角', inputUnit('viewZenith', c.sensor.vza, '°', 'type="number" min="0" max="89.999" step="any"'))}${field('观测方位角', inputUnit('viewAzimuth', c.sensor.vaa, '°', 'type="number" min="0" max="360" step="any"'))}<div class="notice">方位角从正北顺时针。平行投影按该方向成像；中心投影下它是视场指向（巡航时表示相对飞机方位角）。</div></div><div class="property-group"><h3>采样方式</h3>${switchRow('太阳主平面与垂直主平面', 'principalPlaneSwitch', c.sensor.principalPlane)}${switchRow('128 方向半球观测', 'hemisphereSwitch', c.sensor.hemisphere)}</div><div class="property-group"><h3>成像参数</h3>${field('图像分辨率', `<div class="input-row"><input class="input" id="sensorX" type="number" value="${escapeHtml(c.sensor.x)}"><input class="input" id="sensorY" type="number" value="${escapeHtml(c.sensor.y)}"></div>`)}${field('自定义波段 [nm]', `<input class="input" id="sensorBands" value="${escapeHtml(c.sensor.bands)}">`)}${switchRow('连续波段模拟', 'continuousBandsSwitch', c.sensor.continuousBands)}${continuousFields}<div class="notice">最终输出 ${bandCount} 个波段：连续与自定义波段合并、去重并按波长升序排列；最多 ${MAX_SENSOR_BANDS} 个波段。</div>${field('投影方式', `<select class="input" id="sensorProjection"><option value="parallel" ${perspective ? '' : 'selected'}>平行投影</option><option value="perspective" ${perspective ? 'selected' : ''}>中心投影</option></select>`)}${projectionFields}</div><div class="property-group"><div class="notice">${projectionNotice}</div></div>${builtinNotice()}`
  } else if (state.panel === 'objects') {
    const objectCard = (item, index) => {
      const prim = isPrimObject(item)
      const meshBindings = (item.meshes || []).map((mesh) => `${mesh.name}: ${mesh.spectralName} / ${mesh.thermalName} / ${mesh.materialName || item.materialName} / ${mesh.canopyName || item.canopyName}`).join('；') || '无 Mesh 映射'
      const generatedShape = item.type === 'Water'
        ? (item.generation?.shape || item.shape) === 'ellipsoid' ? '圆形/椭圆水面' : '矩形水面'
        : (item.generation?.shape || item.shape) === 'ellipsoid' ? '椭球' : '立方体'
      const generatedGeometry = prim
        ? `<small>${escapeHtml(item.generation?.type || 'PRIM 原型')} · ${generatedShape} · ${(item.dimensions || []).map((value) => Number(value).toLocaleString('zh-CN')).join(' × ')} m</small>`
        : ''
      return `<div class="object-card ${state.selectedObjectIndex === index ? 'active' : ''}" data-index="${index}" role="button" tabindex="0">
        <div class="object-card-head"><strong>${escapeHtml(item.name)}</strong><div class="object-card-actions"><button class="button ghost object-material" type="button">属性</button><button class="button ghost object-distribution" type="button">分布</button>${!prim && !isMediumObject(item) && item.fileName && !item.tarpCoverOf ? '<button class="button ghost object-tarp" type="button" title="按该对象外形生成贴合的篷布罩">篷布罩</button>' : ''}<button class="button ghost danger object-delete" type="button">删除</button></div></div>
        ${generatedGeometry}
        <small title="${escapeHtml(meshBindings)}">属性 · ${escapeHtml(item.materialName || '未定义物化属性')} · ${escapeHtml(item.canopyName || '未定义结构参数')} · ${escapeHtml(meshBindings)}</small>
        <small>分布 · ${escapeHtml(distributionSummary(item))}</small>
        <small title="${escapeHtml(item.fileName)}">${prim ? 'PRIM 网格' : 'OBJ 文件'} · ${escapeHtml(item.fileName || '无网格文件')}</small>
      </div>`
    }
    const indexedObjects = (c.objects.items || []).map((item, index) => ({ item, index }))
    const importedCards = indexedObjects.filter(({ item }) => !isPrimObject(item) && !isMediumObject(item)).map(({ item, index }) => objectCard(item, index)).join('')
    const primitiveCards = indexedObjects.filter(({ item }) => isPrimObject(item) && !isMediumObject(item)).map(({ item, index }) => objectCard(item, index)).join('')
    const mediumCards = indexedObjects.filter(({ item }) => isMediumObject(item)).map(({ item, index }) => objectCard(item, index)).join('')
    const mediumDisabled = state.mode === 'eVoxelRT' || state.mode === 'eVoxelEB' ? '' : 'disabled'
    html = `<div class="property-group"><h3>场景统计</h3><div class="metric-grid"><div class="metric"><strong>${escapeHtml(c.objects.count || '—')}</strong><small>计算对象</small></div><div class="metric"><strong>${c.spectra.length}</strong><small>光谱定义</small></div></div></div>
      <div class="property-group"><h3>OBJ 导入实体</h3>${importedCards || '<div class="notice">尚未导入 OBJ 实体。</div>'}<button class="button ghost" id="objectImportAction" style="width:100%;justify-content:center;margin-top:8px"><svg><use href="#i-import"/></svg>导入 OBJ 实体</button></div>
      <div class="property-group"><h3>PRIM 原型几何</h3>${primitiveCards || '<div class="notice">尚未生成 PRIM 原型。</div>'}<button class="button ghost" id="objectGenerateAction" style="width:100%;justify-content:center;margin-top:8px"><svg><use href="#i-cube"/></svg>新增 PRIM 原型</button></div>
      <div class="property-group"><h3>参与介质场景对象</h3>${mediumCards || '<div class="notice">尚未添加火焰或雾。</div>'}<div class="library-actions"><button class="button ghost" id="objectMediumAction" ${mediumDisabled} style="flex:1;justify-content:center;margin-top:8px"><svg><use href="#i-cloud"/></svg>新增火焰</button><button class="button ghost" id="objectFogAction" ${mediumDisabled} style="flex:1;justify-content:center;margin-top:8px"><svg><use href="#i-cloud"/></svg>新增雾</button></div><div class="notice">火焰和雾参与 VoxelRT / VoxelEB 的体元辐射传输；雾以局部参与介质体积表示，不替代 MODTRAN 大气。</div></div>
      <div class="property-group"><div class="notice">OBJ 用于管理外部导入实体；PRIM 用于管理椭球、立方体、混沌介质和水平水面。对象均可独立设置属性和实例分布。</div></div>`
  } else if (state.panel === 'materials') {
    html = materialLibraryHtml(c)
  } else if (state.panel === 'atmosphere') {
    const atmosphere = c.atmosphere || (c.atmosphere = { enabled: false, model: 'midlatitude-summer', waterVapor: 2, aerosol: 'rural', visibility: 23, lutFile: 'assets/atmosphere/simple_modtran_lut.csv' })
    const disabled = atmosphere.enabled ? '' : 'disabled'
    html = `<div class="property-group"><h3>大气辐射传输</h3>${switchRow('启用 MODTRAN 查找表', 'atmosphereSwitch', atmosphere.enabled)}${field('大气类型', `<select class="select" id="atmosphereModel" ${disabled}><option value="tropical" ${atmosphere.model === 'tropical' ? 'selected' : ''}>热带大气</option><option value="midlatitude-summer" ${atmosphere.model === 'midlatitude-summer' ? 'selected' : ''}>中纬度夏季</option><option value="midlatitude-winter" ${atmosphere.model === 'midlatitude-winter' ? 'selected' : ''}>中纬度冬季</option></select>`)}${field('柱状水汽量 PWV', inputUnit('atmosphereWaterVapor', atmosphere.waterVapor, 'cm', `type="number" min="0.5" max="5" step="0.1" ${disabled}`))}${field('气溶胶类型', `<select class="select" id="atmosphereAerosol" ${disabled}><option value="rural" ${atmosphere.aerosol === 'rural' ? 'selected' : ''}>乡村型</option><option value="urban" ${atmosphere.aerosol === 'urban' ? 'selected' : ''}>城市型</option></select>`)}${field('能见度', inputUnit('atmosphereVisibility', atmosphere.visibility, 'km', `type="number" min="10" max="50" step="1" ${disabled}`))}<div class="notice">传感器高度和观测角度自动读取，参数在 LUT 节点间插值，覆盖 350–14000 nm。地表短波仅计算路径透过率衰减，不包含大气散射程辐射；热红外包含透过率和程辐射。视场中的天空像元按各自半球天顶角计算；启用时使用 LUT，关闭时使用简化经验天空模型。</div></div>${builtinNotice()}`
  } else if (state.panel === 'meteorology') {
    html = `<div class="property-group"><h3>时间设置</h3>${field('开始节点', `<input class="input" id="meteoStart" type="number" min="0" step="1" value="${escapeHtml(c.meteo.start)}">`)}${field('结束节点', `<input class="input" id="meteoEnd" type="number" min="${Number(c.meteo.start) + 1}" step="1" value="${escapeHtml(c.meteo.end)}">`)}${field('时间步长 dTime', inputUnit('meteoDTime', c.meteo.dTime, 's', 'type="number" min="1" step="1"'))}<div class="notice">气象文件中的年积日按“场景输入 → 地理与时间”的年份与时区解释；节点索引以文件首行为 0。</div></div>
      <div class="property-group"><h3>站点参数</h3>${field('观测高度 z', inputUnit('meteoHeight', c.meteo.z, 'm', 'type="number" min="0" step="0.1"'))}<div class="notice">纬度和经度在“场景输入 → 地理与时间”中设置。</div></div>
      <div class="property-group"><h3>初始状态</h3>${field('初始土壤温度 Tsold', inputUnit('meteoSoilTemperature', c.meteo.Tsold, 'K', 'type="number" min="150" max="400" step="0.1"'))}${field('饱和含水量 SatWater', inputUnit('meteoSatWater', c.meteo.SatWater, '0–1', 'type="number" min="0" max="1" step="0.01"'))}<div class="notice">当前能量平衡初温取首个气象节点，饱和含水量以“材质属性 → 物化属性”的土壤参数为准。</div></div>
      <div class="property-group"><h3>驱动文件</h3><div class="path-input"><span title="${escapeHtml(c.meteo.path)}">${escapeHtml(c.meteo.path)}</span></div><label class="button ghost" for="meteoFile" style="width:100%;justify-content:center;margin-top:9px"><svg><use href="#i-import"/></svg>导入本地 meteo 文件<input id="meteoFile" type="file" accept=".txt,.meteo,.dat,.csv,text/plain" hidden></label><div class="notice">文件将复制到当前工程的 meteorology 目录，并写入 project.json。</div></div>${builtinNotice()}`
  } else if (state.panel === 'fluid') {
    const fluid = c.fluid || (c.fluid = { enabled: false, voxelSize: Number(c.scene.voxel) || 1, timeStep: 5, pressureIterations: 20, windDirection: 0, buoyancy: 0.033, dragCoefficient: 0.3, thermalCoupling: 1, diffusivity: 0.1, smokeEmission: 0.02, maxVelocity: 30, outputWind: false, outputAirTemperature: false, outputHeight: 2 })
    if (state.mode !== 'eVoxelEB') {
      html = `<div class="property-group"><h3>流体力学</h3><div class="notice">流体力学当前只接入体元能量平衡（VoxelEB）。切换到“体元能量平衡”后，可设置风场、热浮力、树冠阻力以及地表换热。</div></div>${builtinNotice()}`
    } else {
      const disabled = fluid.enabled ? '' : 'disabled'
      const spacing = Math.max(0.1, Number(fluid.voxelSize) || Number(c.scene.voxel) || 1)
      const fluidHeight = Math.max(
        spacing,
        Number(c.scene.height) || 0,
        (Number(fluid.outputHeight) || 2) + spacing * 0.5
      )
      const gridX = Math.max(1, Math.ceil(Number(c.scene.x) / spacing))
      const gridY = Math.max(1, Math.ceil(fluidHeight / spacing))
      const gridZ = Math.max(1, Math.ceil(Number(c.scene.y) / spacing))
      const cellCount = gridX * gridY * gridZ
      const memoryMb = cellCount * 236 / 1048576
      const outputLayer = Math.max(0, Math.min(gridY - 1, Math.floor((Number(fluid.outputHeight) || 2) / spacing)))
      const resolvedOutputHeight = (outputLayer + 0.5) * spacing
      html = `<div class="property-group"><h3>求解器 · D3Q19 LBM</h3>${switchRow('启用流体力学', 'fluidSwitch', fluid.enabled)}${field('流体体元大小', inputUnit('fluidVoxelSize', spacing, 'm', `type="number" min="0.1" step="0.1" ${disabled}`))}<div class="metric-grid"><div class="metric"><strong>${gridX} × ${gridY} × ${gridZ}</strong><small>空气网格 X × 高 × Y</small></div><div class="metric"><strong>${cellCount.toLocaleString('zh-CN')}</strong><small>空气单元</small></div><div class="metric"><strong>${memoryMb < 1 ? memoryMb.toFixed(2) : memoryMb.toFixed(1)} MB</strong><small>流体状态估算显存</small></div></div><div class="notice">流体采用 D3Q19 BGK 格子玻尔兹曼方法；分布函数、风速、气温和烟雾使用独立稠密网格，辐射传输与能量平衡仍使用场景体元。调大流体体元可显著降低显存占用。</div></div>
        <div class="property-group"><h3>来流边界</h3>${field('来流风向', inputUnit('fluidWindDirection', fluid.windDirection, '°', `type="number" min="0" max="359.9" step="1" ${disabled}`))}${field('数值限速上限', inputUnit('fluidMaxVelocity', fluid.maxVelocity, 'm/s', `type="number" min="0.1" max="200" step="0.5" ${disabled}`))}<div class="notice">来流风速逐时间读取气象驱动；限速上限只用于防止数值发散，不是入射风速或色标上限。0° 沿场景正 Y 方向，90° 沿正 X 方向。</div></div>
        <div class="property-group"><h3>数值求解</h3>${field('最大标量时间步', inputUnit('fluidTimeStep', fluid.timeStep, 's', `type="number" min="0.1" max="60" step="0.1" ${disabled}`))}${field('运动黏度/标量扩散', inputUnit('fluidDiffusivity', fluid.diffusivity, 'm²/s', `type="number" min="0" step="0.01" ${disabled}`))}<div class="notice">每个气象节点至少推进一次来流穿越计算域的 LBM 迭代；物理风速采用固定比例映射到稳定的格子速度，温度与烟雾按这里的最大时间步输运。LBM 直接由密度分布恢复压力，不再执行压力 Jacobi 迭代。</div></div>
        <div class="property-group"><h3>热力与介质耦合</h3>${field('热浮力系数', `<input class="input" id="fluidBuoyancy" type="number" min="0" step="0.001" value="${fluid.buoyancy}" ${disabled}>`)}${field('树冠阻力系数', `<input class="input" id="fluidDrag" type="number" min="0" step="0.05" value="${fluid.dragCoefficient}" ${disabled}>`)}${field('地表换热强度', `<input class="input" id="fluidThermalCoupling" type="number" min="0" step="0.1" value="${fluid.thermalCoupling}" ${disabled}>`)}${field('火焰烟雾源强', `<input class="input" id="fluidSmokeEmission" type="number" min="0" step="0.01" value="${fluid.smokeEmission}" ${disabled}>`)}<div class="notice">建筑和刚体为固体边界，树冠为多孔阻力区；火焰提供温度和烟雾浓度源项。</div></div>
        <div class="property-group"><h3>高度切片输出</h3>${field('输出高度', inputUnit('fluidOutputHeight', fluid.outputHeight, 'm', `type="number" min="0.1" step="0.1" ${disabled}`))}${switchRow('输出风速 GeoTIFF', 'fluidWindOutputSwitch', Boolean(fluid.outputWind), !fluid.enabled)}${switchRow('输出气温 GeoTIFF', 'fluidTemperatureOutputSwitch', Boolean(fluid.outputAirTemperature), !fluid.enabled)}<div class="notice">填写高度会定位到对应流体体元；当前实际输出体元中心约为 ${resolvedOutputHeight.toFixed(2)} m。两个文件均按气象时间节点输出到 output 目录。风速文件包含水平风速大小、X 分量、垂直分量和 Y 分量；气温文件单位为 ℃。建筑内部按固体处理，在风速和气温图中写为 NoData。默认均不输出。</div></div>${builtinNotice()}`
    }
  } else {
    const energyBalanceOutput = state.mode === 'eFacetEB' || state.mode === 'eVoxelEB'
    const imageOutputLabel = energyBalanceOutput ? '逐时间输出图像' : '输出图像'
    const processOutputNotice = energyBalanceOutput
      ? state.mode === 'eVoxelEB'
        ? '<div class="notice">辐射过程：短波、长波和净辐射；能量过程：潜热、显热、表面热通量、GPP 和 NPP。GPP 为叶片总光合，NPP 为叶片净同化近似值，暂未扣除根和茎呼吸。</div>'
        : '<div class="notice">辐射过程：短波、长波和净辐射；能量过程：潜热、显热和表面热通量。两类结果可独立输出并用于三维分析。</div>'
      : '<div class="notice">辐射过程保存面元或体元辐射度，用于三维分析。</div>'
    const processOutputSwitches = switchRow('输出辐射过程', 'radiationProcessSwitch', c.sensor.radiationProcess) +
      (energyBalanceOutput ? switchRow('输出能量过程', 'energyProcessSwitch', c.sensor.energyProcess) : '')
    const samplingControl = field('采样数目', `<input class="input" id="sampleCount" type="number" min="1" max="256" step="1" value="${escapeHtml(c.control.samples)}">`)
    const configuredPeriodicCount = Math.max(0, Math.min(20, Math.round(
      Number.isFinite(Number(c.control.periodicTraversalCount))
        ? Number(c.control.periodicTraversalCount) : 0
    )))
    const effectivePeriodicCount = configuredPeriodicCount
    const periodicBoundaryControl = `${field('周期穿透边界次数', `<input class="input" id="periodicTraversalCount" type="number" min="0" max="20" step="1" value="${effectivePeriodicCount}">`, '0=关闭')}${switchRow('典型天空盒', 'skyboxSwitch', Boolean(c.control.skyboxEnabled))}<div class="notice">不会复制邻域场景。0 为关闭；1–20 表示主视线离开一侧边界后，从对侧继续查询同一场景的最大次数。天空盒勾选后显示典型蓝天与云层；不勾选时保持默认 COS 天空响应。两项互相独立。</div>`
    const heterogeneousVoxelControl = (state.mode === 'eVoxelRT' || state.mode === 'eVoxelEB')
      ? `${switchRow('使用异质性体元', 'heterogeneousVoxelSwitch', Boolean(c.control.heterogeneousVoxel))}<div class="notice">开启后从 OBJ 面元计算每个体元的三轴聚集指数和体密度，并在 VoxelRT/VoxelEB 消光、散射及热辐射中使用；关闭时保持均匀体元算法。默认关闭。</div>`
      : ''
    const radiationSolverControl = state.mode === 'eFacetRT' || state.mode === 'eVoxelRT'
      ? `${field('辐射求解方法', `<select class="select" id="radiationSolver"><option value="traditional" ${c.control.radiationSolver !== 'accelerated' ? 'selected' : ''}>传统稳健方法</option><option value="accelerated" ${c.control.radiationSolver === 'accelerated' ? 'selected' : ''}>${state.mode === 'eVoxelRT' ? '光谱批处理加速' : '光谱复用加速'}</option></select>`)}<div class="notice">${state.mode === 'eVoxelRT' ? '传统方法逐波段提交并等待 GPU；加速方法每次同时求解最多 4 个相邻波段，共享同一轮几何射线遍历，保留全部射线、光谱参数和物理公式，不进行波段插值。' : '传统方法每个波段从零开始固定迭代；加速方法复用相邻同类型波段的辐射度，并按残差提前收敛。两者使用相同几何、采样和输出定义。'}</div>`
      : ''
    const collisionOrdersControl = state.mode === 'eVoxelRT'
      ? `${field('真实碰撞次数', `<select class="select" id="voxelCollisionOrders">${[[2, '2 次（默认）'], [3, '3 次'], [4, '4 次'], [5, '5 次']].map(([value, label]) => `<option value="${value}" ${Number(c.control.voxelCollisionOrders ?? 2) === value ? 'selected' : ''}>${label}</option>`).join('')}</select>`)}<div class="notice">体元之间的散射逐次显式追踪，其余高阶散射用光谱不变（回碰）理论补偿。与完全收敛的多次散射相比：2 次约 5% 以内，3 次约 2%，4 次约 1%，5 次基本收敛；误差主要出现在近红外等叶片反照率高的密冠层，可见光几乎不受影响。每增加一次多一轮漫射遍历。</div>`
      : ''
    const backendNotice = `<div class="notice">${isFacetMode()
      ? 'GPU / CPU / 自动及 CPU 线程数在左侧“计算设备”中选择。面元 CPU 版为原生多线程软件栅格可见性、CSR 邻接图和 Jacobi 辐射度迭代；FacetEB 在该辐射解上继续多线程计算温度与能量通量，通常比 GPU 慢。'
      : 'GPU / CPU / 自动及 CPU 线程数在左侧“计算设备”中选择。CPU 版通过 Mesa lavapipe 软件 Vulkan 驱动多线程执行与 GPU 相同的着色器和光线追踪算法，速度明显慢于 GPU，建议先用较少采样数试算。'}</div>`
    const couplingGeometry = state.mode === 'eFacetEB' ? '面元' : state.mode === 'eVoxelEB' ? '体元' : ''
    const spectralAcceleration = field('光谱加速宽度', inputUnit('spectralAccelerationWidth', c.control.spectralAccelerationWidth ?? 100, 'nm', 'type="number" min="1" max="1000" step="1"'))
    const couplingNotice = state.mode === 'eVoxelEB'
      ? '默认100 nm。直射和漫射均在窗口内积分，并使用窗口中心光学属性计算一次。'
      : '默认100 nm。短波净辐射按窗口分别积分直射和漫射，并使用窗口中心光学属性求解后累加；最终影像对传感器选定波段逐个独立求解，不进行输出波段插值。'
    const coupling = couplingGeometry ? `<div class="property-group"><h3>${couplingGeometry} RT–EB 耦合</h3>${spectralAcceleration}${state.mode === 'eFacetEB' ? field('大气稳定度', `<select class="select" id="stabilityCorrection"><option value="1" ${c.control.stabilityCorrection !== false ? 'selected' : ''}>Monin–Obukhov 修正</option><option value="0" ${c.control.stabilityCorrection === false ? 'selected' : ''}>中性大气（兼容默认）</option></select>`) : ''}${field('最大耦合迭代', `<input class="input" id="couplingIterations" type="number" min="1" max="100" value="${escapeHtml(c.control.couplingIterations)}">`)}${field('温度收敛阈值', inputUnit('temperatureTolerance', c.control.temperatureTolerance, 'K', 'type="number" min="0.001" step="0.01"'))}${field('温度松弛系数', `<input class="input" id="temperatureRelaxation" type="number" min="0.05" max="1" step="0.05" value="${escapeHtml(c.control.temperatureRelaxation)}">`)}<div class="notice">${couplingNotice}场景几何和${couplingGeometry}辐射结构只初始化一次。</div></div>` : ''
    const temperatureMethods = energyBalanceOutput ? `<div class="property-group"><h3>温度模拟方法</h3>${field('土壤温度', `<select class="select" id="soilTemperatureMethod"><option value="0" ${c.control.soilTemperatureMethod === 0 ? 'selected' : ''}>0 · 瞬时能量平衡</option><option value="1" ${c.control.soilTemperatureMethod === 1 ? 'selected' : ''}>1 · 热惯性动态模型</option><option value="2" ${c.control.soilTemperatureMethod === 2 ? 'selected' : ''}>2 · 温度廓线传导模型（默认）</option></select>`)}${field('植被温度', `<select class="select" id="vegetationTemperatureMethod"><option value="0" ${c.control.vegetationTemperatureMethod === 0 ? 'selected' : ''}>经验法 · Ball–Berry</option><option value="1" ${c.control.vegetationTemperatureMethod === 1 ? 'selected' : ''}>机制法 · Farquhar</option></select>`)}<div class="notice">初始温度只作为求解初值；能量平衡各时刻均由所选方法动态更新。土壤方法会同步应用到全部土壤物化属性。</div></div>` : ''
    html = `<div class="property-group"><h3>模拟产出</h3>${switchRow(imageOutputLabel, 'imageSwitch', c.sensor.image)}${processOutputSwitches}${processOutputNotice}${switchRow('输出温度', 'temperatureSwitch', c.sensor.temperature)}${switchRow('输出反照率', 'albedoSwitch', c.sensor.albedo)}</div><div class="property-group"><h3>计算控制</h3>${field('追踪深度', `<input class="input" id="rayDepth" type="number" min="1" max="64" value="${escapeHtml(c.control.depth)}">`)}${samplingControl}${periodicBoundaryControl}${heterogeneousVoxelControl}${radiationSolverControl}${collisionOrdersControl}${backendNotice}</div>${temperatureMethods}${coupling}${builtinNotice()}`
  }
  $('#inspectorContent').innerHTML = html
  bindInspector()
}

function restoreActualScene() {
  loadActualScene(state.config).catch((error) => addLog(`恢复场景对象失败：${error.message}`, 'error'))
}

// Blank or non-numeric entries in a number field restore the previous value
// instead of silently becoming 0 (Number('') === 0).
function finiteInputNumber(input) {
  const raw = String(input?.value ?? '').trim()
  const value = Number(raw)
  return raw !== '' && Number.isFinite(value) ? value : null
}

function bindValue(id, target, selector, converter = Number, rebuildScene = false) {
  const input = $(`#${id}`)
  if (!input) return
  let previous = input.value
  input.addEventListener('change', () => {
    const numeric = input.type === 'number' || converter === Number
    let value = numeric && finiteInputNumber(input) === null ? undefined : converter(input.value)
    // Restoring re-applies the previous value: live 'input' previews may
    // already have written an intermediate value into the configuration.
    if (value === undefined || (typeof value === 'number' && !Number.isFinite(value))) {
      if (numeric && String(previous).trim() === '') { input.value = previous; return }
      value = converter(previous)
    }
    if (typeof value === 'number' && !Number.isFinite(value)) { input.value = previous; return }
    target(value); input.value = String(value); previous = input.value; updateXml(selector, value)
    refreshFromState(rebuildScene)
    if (rebuildScene) restoreActualScene()
  })
}

function bindSwitch(id, getter, setter, selector, rebuildScene = false) {
  const button = $(`#${id}`)
  button?.addEventListener('click', () => {
    const next = !getter(); setter(next); button.classList.toggle('on', next); button.setAttribute('aria-checked', String(next)); updateXml(selector, next ? 1 : 0); refreshFromState(rebuildScene)
    if (rebuildScene) restoreActualScene()
  })
}

function syncSensorSamplingXml() {
  syncProjectText()
}

function bindInspector() {
  const c = state.config
  if (!c) return
  // DEFERRED[中|Bug]: sceneX/sceneY/sceneHeight/voxelSize/rayDepth/couplingIterations/sensorX/sensorY 等只校验“有限数”，不校验 min（HTML min 属性不阻止 change 提交），0 或负值会写进 project.json 交给引擎（体元 0 m、分辨率 0、尺寸负数）；预览侧 generateWorld 用 Math.max 兜底掩盖了问题；建议：为这些字段传入带下限/取整的 converter，与 project-schema 的 normalize 规则保持一致 —— bindValue 已能拦住空串/非有限值（回退原值），但“下限”需要为每个字段新增数值约束并与服务端 normalize 对齐，本轮禁止新增/修改数值常量
  bindValue('sceneX', (v) => c.scene.x = v, 'Scene sceneSizeX', Number, true); bindValue('sceneY', (v) => c.scene.y = v, 'Scene sceneSizeY', Number, true)
  bindValue('sceneHeight', (v) => c.scene.height = v, 'Scene Height', Number, true); bindValue('voxelSize', (v) => c.scene.voxel = v, 'Scene voxelSize'); bindValue('voxelFillThreshold', (v) => c.scene.voxelFillThreshold = Math.min(1, Math.max(0, v)), 'Scene voxelFillThreshold')
  for (const [id, key] of [['sceneOffsetX', 'offsetX'], ['sceneOffsetY', 'offsetY'], ['sceneOffsetZ', 'offsetZ']]) {
    bindValue(id, (value) => c.scene[key] = value, `Scene ${key}`, Number, true)
    $(`#${id}`)?.addEventListener('input', (event) => {
      const value = Number(event.target.value)
      if (!Number.isFinite(value)) return
      c.scene[key] = value
      previewOnNextFrame('sceneBounds', () => updateSceneBoundsGuide(c))
      scheduleProjectTextSync()
      if (sceneOffsetPreviewTimer) clearTimeout(sceneOffsetPreviewTimer)
      sceneOffsetPreviewTimer = setTimeout(() => {
        sceneOffsetPreviewTimer = null
        restoreActualScene()
      }, 280)
    })
    $(`#${id}`)?.addEventListener('change', () => {
      if (sceneOffsetPreviewTimer) clearTimeout(sceneOffsetPreviewTimer)
      sceneOffsetPreviewTimer = null
    })
  }
  // Height changes need immediate visual feedback while the number field is
  // being edited; the normal change handler still performs the full rebuild.
  $('#sceneHeight')?.addEventListener('input', (event) => {
    const value = Number(event.target.value)
    if (!Number.isFinite(value) || value < 0.1) return
    c.scene.height = value
    previewOnNextFrame('sceneBounds', () => { updateSceneBoundsGuide(c); updateSun(c) })
    scheduleProjectTextSync()
  })
  bindValue('rayDepth', (v) => c.control.depth = v, 'Control rayTracingDepth')
  bindValue('sampleCount', (v) => c.control.samples = Math.min(256, Math.max(1, Math.round(v))), 'Control sampleCount', (value) => Math.min(256, Math.max(1, Math.round(Number(value) || 32))))
  bindValue('periodicTraversalCount', (count) => {
    c.control.periodicTraversalCount = count
  }, 'Control periodicNeighborCount', (value) => {
    const parsed = Number(value)
    return Math.max(0, Math.min(20, Math.round(Number.isFinite(parsed) ? parsed : 0)))
  })
  bindSwitch('skyboxSwitch', () => Boolean(c.control.skyboxEnabled), (value) => {
    c.control.skyboxEnabled = value
    updateSceneSkybox(c)
  }, 'Control skyboxEnabled')
  bindSwitch('heterogeneousVoxelSwitch', () => Boolean(c.control.heterogeneousVoxel), (v) => c.control.heterogeneousVoxel = v, 'Control heterogeneousVoxel')
  bindValue('voxelCollisionOrders', (v) => c.control.voxelCollisionOrders = v, 'Control voxelCollisionOrders', (value) => Math.max(2, Math.min(5, Math.round(Number(value)) || 2)))
  bindValue('radiationSolver', (v) => c.control.radiationSolver = v, 'Control radiationSolver', (value) => value === 'accelerated' ? 'accelerated' : 'traditional')
  bindValue('couplingIterations', (v) => c.control.couplingIterations = v, 'Control couplingIterations')
  bindValue('spectralAccelerationWidth', (v) => c.control.spectralAccelerationWidth = Math.max(1, Math.min(1000, v)), 'Control spectralAccelerationWidth', (value) => Math.max(1, Math.min(1000, Number(value) || 100)))
  bindValue('temperatureTolerance', (v) => c.control.temperatureTolerance = v, 'Control temperatureTolerance')
  bindValue('temperatureRelaxation', (v) => c.control.temperatureRelaxation = v, 'Control temperatureRelaxation')
  bindValue('stabilityCorrection', (v) => c.control.stabilityCorrection = v === 1, 'Control stabilityCorrection')
  bindValue('soilTemperatureMethod', (v) => {
    c.control.soilTemperatureMethod = Math.max(0, Math.min(2, Math.round(v)))
    c.materials.filter((item) => item.type === 'Soil').forEach((item) => { item.params.method = c.control.soilTemperatureMethod })
  }, 'Control soilTemperatureMethod', (value) => Math.max(0, Math.min(2, Math.round(Number(value) || 0))))
  bindValue('vegetationTemperatureMethod', (v) => c.control.vegetationTemperatureMethod = Math.max(0, Math.min(1, Math.round(v))), 'Control vegetationTemperatureMethod', (value) => Math.max(0, Math.min(1, Math.round(Number(value) || 0))))
  const fluid = c.fluid || (c.fluid = {})
  $('#fluidSwitch')?.addEventListener('click', () => {
    fluid.enabled = !fluid.enabled
    syncProjectText()
    refreshFromState(false)
    renderInspector()
  })
  bindValue('fluidWindDirection', (v) => fluid.windDirection = ((v % 360) + 360) % 360, 'Fluid windDirection', (value) => ((Number(value) % 360) + 360) % 360)
  bindValue('fluidVoxelSize', (v) => fluid.voxelSize = Math.max(0.1, v), 'Fluid voxelSize', (value) => Math.max(0.1, Number(value) || Number(c.scene.voxel) || 1), true)
  bindValue('fluidTimeStep', (v) => fluid.timeStep = Math.max(0.1, Math.min(60, v)), 'Fluid timeStep', (value) => Math.max(0.1, Math.min(60, Number(value) || 5)))
  bindValue('fluidPressureIterations', (v) => fluid.pressureIterations = Math.max(2, Math.min(100, 2 * Math.round(v / 2))), 'Fluid pressureIterations', (value) => Math.max(2, Math.min(100, 2 * Math.round((Number(value) || 20) / 2))))
  bindValue('fluidBuoyancy', (v) => fluid.buoyancy = Math.max(0, v), 'Fluid buoyancy', (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidDrag', (v) => fluid.dragCoefficient = Math.max(0, v), 'Fluid dragCoefficient', (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidThermalCoupling', (v) => fluid.thermalCoupling = Math.max(0, v), 'Fluid thermalCoupling', (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidDiffusivity', (v) => fluid.diffusivity = Math.max(0, v), 'Fluid diffusivity', (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidSmokeEmission', (v) => fluid.smokeEmission = Math.max(0, v), 'Fluid smokeEmission', (value) => Math.max(0, Number(value) || 0))
  bindValue('fluidMaxVelocity', (v) => fluid.maxVelocity = Math.max(0.1, Math.min(200, v)), 'Fluid maxVelocity', (value) => Math.max(0.1, Math.min(200, Number(value) || 30)))
  bindValue('fluidOutputHeight', (v) => fluid.outputHeight = Math.max(0.1, v), 'Fluid outputHeight', (value) => Math.max(0.1, Number(value) || 2))
  bindSwitch('fluidWindOutputSwitch', () => Boolean(fluid.outputWind), (v) => fluid.outputWind = v, 'Fluid outputWind')
  bindSwitch('fluidTemperatureOutputSwitch', () => Boolean(fluid.outputAirTemperature), (v) => fluid.outputAirTemperature = v, 'Fluid outputAirTemperature')
  bindSwitch('terrainSwitch', () => c.scene.terrain, (v) => c.scene.terrain = v, 'Control isDEM', true)
  const terrainEnvironment = c.scene.terrainEnvironment || (c.scene.terrainEnvironment = normalizeTerrainEnvironment())
  bindSwitch('terrainAirSwitch', () => terrainEnvironment.airEnabled, (v) => terrainEnvironment.airEnabled = v, 'Scene TerrainEnvironment')
  bindSwitch('terrainSoilSwitch', () => terrainEnvironment.soilEnabled, (v) => terrainEnvironment.soilEnabled = v, 'Scene TerrainEnvironment')
  for (const key of ['referenceHeight', 'temperatureLapseRate', 'vaporScaleHeight', 'drainageHeight', 'soilDry', 'soilValley', 'soilScaleHeight']) {
    bindValue(`terrainEnv_${key}`, (v) => terrainEnvironment[key] = v, 'Scene TerrainEnvironment')
  }

  const atmosphere = c.atmosphere || (c.atmosphere = { enabled: false, model: 'midlatitude-summer', waterVapor: 2, aerosol: 'rural', visibility: 23, lutFile: 'assets/atmosphere/simple_modtran_lut.csv' })
  const atmosphereSwitch = $('#atmosphereSwitch')
  atmosphereSwitch?.addEventListener('click', () => {
    atmosphere.enabled = !atmosphere.enabled
    atmosphereSwitch.classList.toggle('on', atmosphere.enabled)
    atmosphereSwitch.setAttribute('aria-checked', String(atmosphere.enabled))
    for (const id of ['atmosphereModel', 'atmosphereWaterVapor', 'atmosphereAerosol', 'atmosphereVisibility']) {
      const input = $(`#${id}`)
      if (input) input.disabled = !atmosphere.enabled
    }
    updateXml('Atmosphere enabled', atmosphere.enabled ? 1 : 0)
    refreshFromState(false)
  })
  bindValue('atmosphereModel', (value) => atmosphere.model = value, 'Atmosphere model', String)
  bindValue('atmosphereWaterVapor', (value) => atmosphere.waterVapor = Math.max(0.5, Math.min(5, value)), 'Atmosphere waterVapor', (value) => Math.max(0.5, Math.min(5, Number(value) || 2)))
  bindValue('atmosphereAerosol', (value) => atmosphere.aerosol = value, 'Atmosphere aerosol', String)
  bindValue('atmosphereVisibility', (value) => atmosphere.visibility = Math.max(10, Math.min(50, value)), 'Atmosphere visibility', (value) => Math.max(10, Math.min(50, Number(value) || 23)))
  const background = c.scene.background || (c.scene.background = {})
  const bindBackground = (id, key, selector) => $(`#${id}`)?.addEventListener('change', (event) => {
    background[key] = event.target.value
    updateXml(selector, event.target.value)
    if (key === 'materialName') {
      const material = c.materials.find((item) => item.name === event.target.value)
      background.materialType = material?.type || background.materialType || 'Soil'
      updateXml('Scene bgBioType', background.materialType)
    }
    refreshFromState(false)
  })
  bindBackground('backgroundSpectral', 'spectralName', 'Scene bgSpectral')
  bindBackground('backgroundThermal', 'thermalName', 'Scene bgThermal')
  bindBackground('backgroundMaterial', 'materialName', 'Scene bgBioName')
  bindSwitch('backgroundSpecularSwitch', () => Boolean(background.specularEnabled), (value) => {
    background.specularEnabled = value
  }, 'Scene backgroundSpecularEnabled')
  const heterogeneitySwitch = $('#backgroundHeterogeneitySwitch')
  heterogeneitySwitch?.addEventListener('click', () => {
    const enabled = !Boolean(background.heterogeneityEnabled)
    background.heterogeneityEnabled = enabled
    heterogeneitySwitch.classList.toggle('on', enabled)
    heterogeneitySwitch.setAttribute('aria-checked', String(enabled))
    const strengthInput = $('#backgroundAngularStrength')
    if (strengthInput) strengthInput.disabled = !enabled
    updateXml('Scene backgroundHeterogeneity', enabled ? 'Hapke' : 'None')
    updateXml('Scene backgroundAngularStrength', enabled
      ? Math.max(0, Math.min(1, Number(background.angularEffectStrength ?? 0.5))) : 0)
    refreshFromState(false)
  })
  const angularStrengthInput = $('#backgroundAngularStrength')
  angularStrengthInput?.addEventListener('input', () => {
    const value = Math.max(0, Math.min(1, Number(angularStrengthInput.value)))
    background.angularEffectStrength = value
    $('#backgroundAngularStrengthValue').textContent = value.toFixed(2)
    updateXml('Scene backgroundAngularStrength', background.heterogeneityEnabled ? value : 0)
  })
  angularStrengthInput?.addEventListener('change', () => refreshFromState(false))
  const bindAngle = (id, key) => $(`#${id}`)?.addEventListener('change', (event) => { const value = finiteInputNumber(event.target); if (value === null) { event.target.value = String(c.light[key] ?? ''); return } c.light[key] = value; syncProjectText(); if (key === 'azimuth' && c.sensor.principalPlane) syncSensorSamplingXml(); refreshFromState(false) })
  bindAngle('sunZenith', 'zenith'); bindAngle('sunAzimuth', 'azimuth')
  const bindLightFraction = (id, sourceKey) => {
    const input = $(`#${id}`)
    input?.addEventListener('input', () => {
      const value = Math.max(0, Math.min(1, Number(input.value)))
      c.light[sourceKey] = value
      c.light[sourceKey === 'direct' ? 'diffuse' : 'direct'] = 1 - value
      $('#directLight').value = String(c.light.direct); $('#diffuseLight').value = String(c.light.diffuse)
      $('#directValue').textContent = c.light.direct.toFixed(2); $('#diffuseValue').textContent = c.light.diffuse.toFixed(2)
      previewOnNextFrame('sun', () => updateSun(c))
    })
    input?.addEventListener('change', () => {
      updateXml('Geometry Light light directScatteringRatio', c.light.direct)
      refreshFromState(false)
    })
  }
  bindLightFraction('directLight', 'direct'); bindLightFraction('diffuseLight', 'diffuse')
  bindValue('skyTemperature', (v) => c.light.skyTemperature = v, 'Geometry Light light skyTemperature')
  bindValue('sensorX', (v) => c.sensor.x = v, 'Geometry Sensor sensor pixelResolutionX'); bindValue('sensorY', (v) => c.sensor.y = v, 'Geometry Sensor sensor pixelResolutionY')
  $('#sensorBands')?.addEventListener('change', (event) => { c.sensor.bands = event.target.value; syncSensorSamplingXml(); renderInspector() })
  const bindSensorNumber = (id, key, minimum = -Infinity) => $(`#${id}`)?.addEventListener('change', (event) => {
    const value = finiteInputNumber(event.target)
    if (value === null) { event.target.value = String(c.sensor[key] ?? ''); return }
    c.sensor[key] = Math.max(minimum, value); syncSensorSamplingXml(); renderInspector()
  })
  bindSensorNumber('bandStart', 'bandStart', 0.000001); bindSensorNumber('bandEnd', 'bandEnd', 0.000001); bindSensorNumber('bandStep', 'bandStep', 0.000001)
  const bindSensorSwitch = (id, key) => $(`#${id}`)?.addEventListener('click', () => {
    c.sensor[key] = !c.sensor[key]; syncSensorSamplingXml(); renderInspector()
  })
  bindSensorSwitch('continuousBandsSwitch', 'continuousBands')
  bindSensorSwitch('principalPlaneSwitch', 'principalPlane')
  bindSensorSwitch('hemisphereSwitch', 'hemisphere')
  bindSensorSwitch('sensorCruiseSwitch', 'cruiseEnabled')
  $('#sensorProjection')?.addEventListener('change', (event) => {
    c.sensor.projection = event.target.value === 'perspective' ? 'perspective' : 'parallel'
    if (!Number.isFinite(Number(c.sensor.positionX))) c.sensor.positionX = Number(c.scene.x) / 2
    if (!Number.isFinite(Number(c.sensor.positionY))) c.sensor.positionY = Number(c.scene.y) / 2
    if (!(Number(c.sensor.height) > 0)) c.sensor.height = 3000
    updateXml('Geometry Sensor sensor projection', c.sensor.projection)
    updateXml('Geometry Sensor sensor sensorPosition', `${c.sensor.positionX},${c.sensor.positionY},${c.sensor.height}`)
    syncSensorSamplingXml()
    refreshFromState(false)
    renderInspector()
  })
  const bindSensorPosition = (id, key) => $(`#${id}`)?.addEventListener('change', (event) => {
    const value = finiteInputNumber(event.target)
    if (value === null) { event.target.value = String(c.sensor[key] ?? ''); return }
    c.sensor[key] = value
    updateXml('Geometry Sensor sensor sensorPosition', `${c.sensor.positionX},${c.sensor.positionY},${c.sensor.height}`)
    refreshFromState(false)
    renderInspector()
  })
  bindSensorPosition('sensorPositionX', 'positionX'); bindSensorPosition('sensorPositionY', 'positionY'); bindSensorPosition('sensorHeight', 'height')
  bindSensorNumber('sensorFov', 'fov', 0.1)
  $('#sensorCruiseRoute')?.addEventListener('change', (event) => {
    c.sensor.cruiseRoute = event.target.value
    syncSensorSamplingXml()
    renderInspector()
  })
  bindSensorNumber('sensorCruiseTargetX', 'cruiseTargetX')
  bindSensorNumber('sensorCruiseTargetY', 'cruiseTargetY')
  bindSensorNumber('sensorCruiseTargetZ', 'cruiseTargetZ', 0.01)
  bindSensorNumber('sensorCruiseRectangleWidth', 'cruiseRectangleWidth', 0.01)
  bindSensorNumber('sensorCruiseRectangleHeight', 'cruiseRectangleHeight', 0.01)
  bindSensorNumber('sensorCruiseStep', 'cruiseStep', 0.01)
  bindSensorNumber('sensorCruiseCount', 'cruiseCount', 1)
  $('#randomCruiseBtn')?.addEventListener('click', () => {
    c.sensor.cruiseRandomSeed = Math.max(1, Math.floor(Math.random() * 2147483646))
    syncSensorSamplingXml()
    renderInspector()
  })
  const bindView = (id, key) => $(`#${id}`)?.addEventListener('change', (event) => { const value = finiteInputNumber(event.target); if (value === null) { event.target.value = String(c.sensor[key] ?? ''); return } c.sensor[key] = value; syncSensorSamplingXml(); renderInspector() })
  bindView('viewZenith', 'vza'); bindView('viewAzimuth', 'vaa')
  const energyOutputMode = state.mode === 'eFacetEB' || state.mode === 'eVoxelEB'
  const syncActiveProcess = () => updateXml('Control isProcess',
    (energyOutputMode ? (c.sensor.radiationProcess || c.sensor.energyProcess) : c.sensor.radiationProcess) ? 1 : 0)
  bindSwitch('imageSwitch', () => c.sensor.image, (v) => c.sensor.image = v, 'Control isImage')
  bindSwitch('radiationProcessSwitch', () => c.sensor.radiationProcess, (v) => { c.sensor.radiationProcess = v; syncActiveProcess() }, 'Control isRadiationProcess')
  bindSwitch('energyProcessSwitch', () => c.sensor.energyProcess, (v) => { c.sensor.energyProcess = v; syncActiveProcess() }, 'Control isEnergyProcess')
  bindSwitch('temperatureSwitch', () => c.sensor.temperature, (v) => c.sensor.temperature = v, 'Control isTemperature'); bindSwitch('albedoSwitch', () => c.sensor.albedo, (v) => c.sensor.albedo = v, 'Control isAlbedo')
  bindValue('meteoStart', (v) => {
    c.meteo.start = v
    if (!(Number(c.meteo.end) > v)) { c.meteo.end = v + 1; updateXml('Meteorology endTimeNode', c.meteo.end); $('#meteoEnd').value = String(c.meteo.end) }
  }, 'Meteorology startTimeNode', (value) => Math.max(0, Math.round(Number(value) || 0)))
  bindValue('meteoEnd', (v) => c.meteo.end = v, 'Meteorology endTimeNode', (value) => Math.max(Number(c.meteo.start) + 1, Math.round(Number(value) || 0)))
  bindValue('meteoDTime', (v) => c.meteo.dTime = v, 'Meteorology dTime', (value) => Math.max(1, Number(value) || 1800))
  bindValue('meteoLatitude', (v) => c.meteo.latitude = v, 'Meteorology Latitude', (value) => Math.max(-90, Math.min(90, Number(value) || 0)))
  bindValue('meteoLongitude', (v) => c.meteo.longitude = v, 'Meteorology Longitude', (value) => Math.max(-180, Math.min(180, Number(value) || 0)))
  bindValue('meteoYear', (v) => c.meteo.year = v, 'Meteorology Year', (value) => Math.max(-2000, Math.min(6000, Math.round(Number(value)) || 2019)))
  bindValue('meteoTimezone', (v) => c.meteo.timezone = v, 'Meteorology TimeZone', (value) => { const hours = Number(value); return Number.isFinite(hours) ? Math.max(-12, Math.min(14, hours)) : 8 })
  bindValue('meteoHeight', (v) => c.meteo.z = v, 'Meteorology z', (value) => Math.max(0, Number(value) || 0))
  bindValue('meteoSoilTemperature', (v) => c.meteo.Tsold = v, 'Meteorology Tsold', (value) => Math.max(150, Math.min(400, Number(value) || 300)))
  bindValue('meteoSatWater', (v) => c.meteo.SatWater = v, 'Meteorology SatWater', (value) => Math.max(0, Math.min(1, Number(value) || 0)))
  $('#demFile')?.addEventListener('change', (event) => importDem(event.target.files?.[0]))
  $('#meteoFile')?.addEventListener('change', (event) => importMeteo(event.target.files?.[0]))
  $('#objectImportAction')?.addEventListener('click', importObj)
  $('#objectGenerateAction')?.addEventListener('click', showGeometryDialog)
  $('#objectMediumAction:not([disabled])')?.addEventListener('click', () => showMediumDialog('fire'))
  $('#objectFogAction:not([disabled])')?.addEventListener('click', () => showMediumDialog('fog'))
  $$('.object-card[data-index]').forEach((card) => {
    const showAttributes = () => showObjectAttributeDialog(Number(card.dataset.index))
    card.addEventListener('click', (event) => { if (!event.target.closest('button')) showAttributes() })
    card.addEventListener('keydown', (event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); showAttributes() } })
  })
  $$('.object-material').forEach((button) => button.addEventListener('click', (event) => { event.stopPropagation(); showObjectAttributeDialog(Number(button.closest('.object-card').dataset.index)) }))
  $$('.object-distribution').forEach((button) => button.addEventListener('click', (event) => { event.stopPropagation(); showObjectDialog(Number(button.closest('.object-card').dataset.index)) }))
  $$('.object-delete').forEach((button) => button.addEventListener('click', (event) => { event.stopPropagation(); deleteSceneObject(Number(button.closest('.object-card').dataset.index)) }))
  $$('.object-tarp').forEach((button) => button.addEventListener('click', async (event) => {
    event.stopPropagation()
    button.disabled = true
    try { await addTarpCover(Number(button.closest('.object-card').dataset.index)) } finally { button.disabled = false }
  }))
  $$('.library-add').forEach((button) => button.addEventListener('click', () => showPresetDialog(button.dataset.presetKind)))
  $$('.library-card[data-preset-index]').forEach((card) => card.addEventListener('click', () => showPresetDialog(card.dataset.presetKind, Number(card.dataset.presetIndex))))
  $$('.library-filter').forEach((input) => input.addEventListener('input', () => {
    const keyword = input.value.trim().toLowerCase()
    $$(`[data-library-list="${input.dataset.libraryFilter}"] .library-card`).forEach((card) => card.classList.toggle('filtered-out', !card.dataset.librarySearch.includes(keyword)))
  }))
}

function renderNavBadges() {
  const c = state.config
  if (!c) return
  const simulationOutputs = []
  if (c.sensor?.image) simulationOutputs.push(t('图像'))
  if (c.sensor?.radiationProcess || c.sensor?.energyProcess || c.sensor?.process) simulationOutputs.push(t('过程'))
  simulationOutputs.push(t('统计'))
  $('#sceneSizeBadge').textContent = `${c.scene.x} × ${c.scene.y} m`; $('#lightBadge').textContent = `${c.light.zenith}° / ${c.light.azimuth}°`; $('#sensorBadge').textContent = `${c.sensor.x} × ${c.sensor.y}`
  $('#objectBadge').textContent = c.objects.count || '0'; $('#materialBadge').textContent = `${c.spectra.length + c.thermals.length + c.canopies.length + c.materials.length}`; $('#atmosphereBadge').textContent = t(c.atmosphere?.enabled ? '已启用' : '关闭'); $('#meteoBadge').textContent = state.inputPath ? `${c.meteo.start}–${c.meteo.end}` : t('未加载'); $('#fluidBadge').textContent = t(state.mode === 'eVoxelEB' ? (c.fluid?.enabled ? '已启用' : '关闭') : '仅 VoxelEB'); $('#simulationBadge').textContent = simulationOutputs.join(' / ')
  // 场景输入：地理/DEM 的分组徽标
  $('#geoBadge').textContent = `${Number(c.meteo.latitude).toFixed(1)}° / UTC${Number(c.meteo.timezone) >= 0 ? '+' : ''}${Number(c.meteo.timezone)}`
  $('#demBadge').textContent = t(c.scene.demFile ? (c.scene.terrain ? '已启用' : '已载入') : '关闭')
}

function refreshFromState(rebuild = true) {
  const c = state.config
  renderNavBadges()
  if (rebuild) { generateWorld(c); fitCamera() }
  updateSceneSkybox(c)
  updateSun(c)
}

function hasTerrainValues(config) {
  const dem = config?.scene?.demInfo
  const width = Math.max(2, Math.round(Number(dem?.terrainWidth) || 0))
  const height = Math.max(2, Math.round(Number(dem?.terrainHeight) || 0))
  return Array.isArray(dem?.terrainValues) && dem.terrainValues.length === width * height
}

async function ensureDemInfo(config, projectPath = state.inputPath) {
  if (!config?.scene?.terrain || !config.scene.demFile || hasTerrainValues(config)) return false
  const inspected = await api.inspectDem({ path: config.scene.demFile, projectPath })
  config.scene.demInfo = inspected.metadata || null
  // Save snapshots are detached; only the active scene may update its project.
  if (state.config === config && state.project?.configuration) state.project.configuration.scene.demInfo = config.scene.demInfo
  return hasTerrainValues(config)
}

let sceneLoadGeneration = 0

async function loadActualScene(config) {
  const generation = ++sceneLoadGeneration
  const isCurrent = () => generation === sceneLoadGeneration
  clearObjectSelection(false)
  try {
    if (await ensureDemInfo(config) && isCurrent()) generateWorld(config)
  } catch (error) {
    if (isCurrent()) addLog(`DEM 高程恢复失败：${error.message}`, 'error')
  }
  if (!isCurrent()) return
  if (await loadXmlScene({ api, world, config, log: addLog, isCurrent }) === null) return
  updateSceneBoundsGuide(config); updateStats(); applyViewStyle(currentStyle); fitCamera()
  if (objectEdit.enabled) geometryDrawHint('单击选择 OBJ；双击修改物化属性；W/E/R 切换移动、旋转、缩放')
}

function loadXml(result) {
  if (result?.streamProject) { streamWorkspace.openProject(result.streamProject); return }
  // FIXED[中|错误处理]: 本函数和 chooseXml 都不检查 state.running，运行中切换工程会改写 outputDir/inputPath，运行结束事件落到新工程上；已修复：运行中拒绝切换工程（沿用“打开示例”的同一提示）
  if (state.running) { toast('无法打开工程', '模拟运行中，不能更换工程', 'error'); return }
  streamWorkspace.close()
  if (projectTextSyncTimer) { clearTimeout(projectTextSyncTimer); projectTextSyncTimer = null }
  try {
    const projectMode = result.project?.mode || projectModeFromXml(result.content, state.mode)
    const project = normalizeProject(result.project || parseProjectJson(result.content))
    const config = ensureMaterialPresets(project.configuration)
    if (project?.configuration?.scene?.demInfo) config.scene.demInfo = project.configuration.scene.demInfo
    const storedItems = project?.configuration?.objects?.items || []
    config.objects.items = config.objects.items.map((item, index) => {
      const stored = storedItems.find((entry) => (entry.fileName && entry.fileName === item.fileName) || entry.name === item.name) || storedItems[index]
      return stored ? { ...stored, ...item, distribution: stored.distribution, instanceCount: stored.instanceCount } : item
    })
    if (project) project.configuration = config
    invalidateTextureCaches()
    state.projectGeneration += 1
    Object.assign(state, { inputPath: result.path, xmlText: result.content, xmlDirty: false, xmlManualEdit: false, project, projectPath: result.projectPath || '', config, outputDir: config.outDir || result.path.replace(/[\\/][^\\/]+$/, '') , selectedObjectIndex: -1, editingObjectIndex: -1, editingAttributeIndex: -1 })
    selectMode(projectMode)
    $('#xmlEditor').value = result.content; $('#xmlEditor').hidden = false; $('#emptyEditor').hidden = true; $('#applyXmlBtn').disabled = false; $('#xmlStatus').textContent = result.path
    $('#projectName').textContent = project?.name || fileName(result.path); $('#unsavedMark').hidden = true
    // FIXED[中|错误处理]: loadActualScene(config) 未 await 也无 catch（saveXml 的 replaced 分支、applyXmlBtn 处理器同样），OBJ 读取/解析失败成为未处理 rejection，场景对象静默缺失；已修复：改用自带 catch 的 restoreActualScene()（saveXml 的 replaced 分支与 applyXmlBtn 处理器同样替换）；运行中拒绝切换工程已在上方 loadXml 入口加入
    refreshFromState(); restoreActualScene(); renderInspector(); addLog(`已加载配置：${result.path}`, 'success'); toast('配置已加载', fileName(result.path))
    void reportUsdOpen(result)
  } catch (error) { toast('无法读取配置', error.message, 'error'); addLog(error.message, 'error') }
}

async function chooseXml() {
  try {
    const result = await api.chooseXml(state.mode)
    if (result) loadXml(result)
  } catch (error) {
    toast('无法打开工程', error.message, 'error')
    addLog(`无法打开工程：${error.message}`, 'error')
  }
}

function reportUsdSave(usd) {
  if (!usd) return
  if (usd.error) { addLog(`project.usd 同步失败：${usd.error}`, 'warning'); return }
  addLog(`已同步 project.usd（${usd.prototypes} 个模型、${usd.instances} 个实例，${(usd.elapsedMs / 1000).toFixed(1)} s）`)
  if (usd.externalBackup) addLog(`project.usd 曾在其他软件中被修改，覆盖前已备份为 ${usd.externalBackup}`, 'warning')
  for (const warning of usd.skippedObjects || []) addLog(`USD 导出跳过：${warning}`, 'warning')
}

// A USD file opened as a project, or project.usd edited elsewhere since Stream Studio last wrote it.
async function reportUsdOpen(result) {
  const summary = result?.usdImport
  if (summary) {
    addLog(`已从 OpenUSD 载入场景：${result.usdSource}（${summary.objects} 个对象、${summary.instances} 个实例${summary.createdProject ? '，已新建 project.json' : '，原 project.json 已备份'}${summary.restoredFromMetadata ? '，参数按 USD 内嵌的 Stream Studio 工程恢复' : ''}）`, 'success')
    for (const warning of summary.warnings || []) addLog(`USD 提示：${warning}`, 'warning')
  }
  if (!result?.usdChangedExternally) return
  if (!window.confirm(t('project.usd 已在其他软件中被修改，是否载入这些修改？选择“取消”则保留当前工程，下次保存时会先备份再覆盖 project.usd。'))) return
  try {
    const reopened = await api.openProject(result.path.replace(/project\.json$/i, 'project.usd'), state.mode)
    loadXml(reopened)
  } catch (error) {
    toast('无法载入 project.usd', error.message, 'error')
    addLog(`无法载入 project.usd：${error.message}`, 'error')
  }
}

function hideExampleDialog() { $('#exampleDialog').hidden = true }

function renderExampleSummary() {
  const example = state.examples?.find((item) => item.id === $('#exampleSelect').value)
  $('#exampleSummary').textContent = example ? `${t('推荐模式')}：${example.recommendedMode} · ${example.focus.map((item) => t(item)).join('、')}${example.note ? ` · ${t(example.note)}` : ''}` : ''
}

async function showExampleDialog() {
  if (state.running) { toast('无法打开示例', '模拟运行中，不能更换工程', 'error'); return }
  try {
    const { examples, directory } = await api.listExamples()
    if (!examples.length) throw new Error('没有可用的示例工程')
    state.examples = examples
    const groups = new Map()
    for (const item of [...examples].sort((a, b) => Number(a.tier === 'technical') - Number(b.tier === 'technical'))) {
      const group = `${t(item.tier === 'technical' ? '技术演示' : '内置精选')} · ${t(item.categoryLabel || '其他案例')}`
      if (!groups.has(group)) groups.set(group, [])
      groups.get(group).push(item)
    }
    $('#exampleSelect').innerHTML = [...groups].map(([label, items]) =>
      `<optgroup label="${escapeHtml(label)}">${items.map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(t(item.label))}</option>`).join('')}</optgroup>`).join('')
    $('#exampleDirectory').value = appStorage.getItem('histreamExampleDirectory') || directory
    renderExampleSummary()
    $('#exampleDialog').hidden = false
  } catch (error) { toast('无法读取示例', error.message, 'error') }
}

async function openExampleFromDialog(event) {
  event.preventDefault()
  const submit = $('#submitExampleBtn')
  try {
    submit.disabled = true
    const directory = $('#exampleDirectory').value.trim().replace(/^"(.*)"$/, '$1')
    const result = await api.openExample($('#exampleSelect').value, directory)
    appStorage.setItem('histreamExampleDirectory', directory)
    loadXml(result)
    hideExampleDialog()
    addLog(`示例已复制并打开：${result.path}`, 'success')
  } catch (error) {
    toast('无法打开示例', error.message, 'error')
    addLog(error.message, 'error')
  } finally { submit.disabled = false }
}

async function importMeteo(file) {
  if (!file) return
  try {
    if (state.running) throw new Error('模拟运行中，不能更换气象驱动文件')
    // FIXED[中|并发]: 与 importDem 同类问题——上传期间若切换了工程，下面会把气象文件写进新工程的 state.config 并立即 saveXml，而文件实际导入在旧工程目录；已修复：await 前缓存 state.inputPath，返回后不一致则中止并提示
    const projectPath = state.inputPath
    const imported = await api.importMeteo({ name: file.name, content: await file.text() })
    if (state.inputPath !== projectPath) throw new Error('导入期间已切换工程，本次气象文件未写入')
    const project = ensureProjectState()
    state.config.meteo.path = imported.relativePath || imported.path
    project.configuration = state.config
    updateXml('Meteorology filePath', imported.path)
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    refreshFromState(false)
    await loadActualScene(state.config)
    renderInspector()
    toast('导入成功', imported.relativePath || file.name)
    addLog(`已导入气象驱动：${imported.path}`, 'success')
  } catch (error) {
    toast('气象文件导入失败', error.message, 'error')
    addLog(error.message, 'error')
  }
}

function readFileBase64(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.onerror = () => reject(reader.error || new Error('无法读取 DEM 文件'))
    reader.onload = () => resolve(String(reader.result || '').split(',', 2)[1] || '')
    reader.readAsDataURL(file)
  })
}

async function importDem(file) {
  if (!file) return
  try {
    if (state.running) throw new Error('模拟运行中，不能更换 DEM 文件')
    if (file.size > 64 * 1024 * 1024) throw new Error('DEM 文件超过 64MB，请先裁剪或降采样')
    const projectPath = state.inputPath
    const imported = await api.importDem({ name: file.name, contentBase64: await readFileBase64(file), projectPath: state.inputPath })
    // FIXED[中|并发]: 读取 64MB 文件并上传期间若切换了工程，下面会把 DEM 写进新工程的 state.config 并立即 saveXml，而文件实际导入在旧工程目录（importMeteo 同样问题）；已修复：await 前缓存 state.inputPath，返回后不一致则中止并提示
    if (state.inputPath !== projectPath) throw new Error('导入期间已切换工程，本次 DEM 未写入')
    const project = ensureProjectState()
    state.config.scene.demFile = imported.relativePath || imported.path
    state.config.scene.demInfo = imported.metadata || null
    state.config.scene.terrain = true
    project.configuration = state.config
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    refreshFromState()
    await loadActualScene(state.config)
    renderInspector()
    const warning = (imported.warnings || []).join('；')
    toast(warning ? 'DEM 已导入（有提示）' : 'DEM 导入成功', warning || imported.relativePath || file.name, warning ? 'warning' : '')
    addLog(`已导入 DEM：${imported.path}`, 'success')
    if (warning) addLog(`DEM 提示：${warning}`, 'warning')
  } catch (error) {
    toast('DEM 导入失败', error.message, 'error')
    addLog(error.message, 'error')
  }
}

async function saveXml(fromProject = false) {
  fromProject = fromProject === true
  // Commit a number field still being edited (its change event fires on blur).
  const active = document.activeElement
  if (active && active !== document.body && active.closest?.('#inspectorContent')) active.blur()
  flushProjectTextSync()
  const generation = state.projectGeneration
  const revision = state.editRevision
  const savedInputPath = state.inputPath
  const savedText = state.xmlText
  const savedMode = state.mode
  const isCurrent = () => state.projectGeneration === generation && state.inputPath === savedInputPath
  if (!savedText) { await chooseXml(); return isCurrent() ? false : null }
  state.lastSaveError = null
  try {
    // Inspector/dialog handlers hold a reference to state.config, so only text
    // typed into the JSON editor may replace it with a newly parsed object.
    const manualEdit = !fromProject && (state.xmlManualEdit || !state.config)
    const sourceConfig = manualEdit ? ensureMaterialPresets(parseXml(savedText, savedMode)) : state.config
    const replaced = sourceConfig !== state.config
    const config = structuredClone(sourceConfig)
    const project = structuredClone(state.project)
    if (fromProject) await ensureDemInfo(config, savedInputPath)
    // null means superseded: callers must skip both refresh and failure rollback.
    if (!isCurrent() || state.editRevision !== revision) return null
    if (project) {
      project.configuration = config
      project.mode = savedMode
    }
    const result = await api.saveXml({ path: savedInputPath, content: savedText, project })
    if (!result?.path) return isCurrent() ? false : null
    // The submitted snapshot is on disk; newer edits must remain dirty and intact.
    if (!isCurrent()) { addLog('已保存提交时的配置；当前工程保持不变', 'warning'); reportUsdSave(result.usd); return null }
    if (state.editRevision !== revision) {
      // An older save may finish after a newer one cleared the dirty indicator.
      state.xmlDirty = true
      $('#unsavedMark').hidden = false
      $('#xmlStatus').textContent = '有未保存的修改'
      addLog('已保存提交时的配置；后续修改仍需保存', 'warning')
      reportUsdSave(result.usd)
      return true
    }
    state.config = sourceConfig
    if (fromProject && sourceConfig?.scene) sourceConfig.scene.demInfo = config.scene.demInfo
    state.inputPath = result.path
    state.project = result.project || project
    state.projectPath = result.projectPath || state.projectPath
    state.xmlText = result.content || state.xmlText
    state.xmlDirty = false
    state.xmlManualEdit = false
    $('#xmlEditor').value = state.xmlText
    $('#unsavedMark').hidden = true; $('#xmlStatus').textContent = result.path; $('#projectName').textContent = state.project?.name || fileName(result.path)
    if (replaced) { refreshFromState(); restoreActualScene(); renderInspector() }
    addLog(`已保存配置：${result.path}`, 'success'); toast('保存成功', fileName(result.path)); reportUsdSave(result.usd); return true
  } catch (error) {
    if (isCurrent()) state.lastSaveError = error
    toast('保存失败', error.message, 'error')
    addLog(`工程配置保存失败（${savedInputPath}）：${error.message}`, 'error')
    return isCurrent() ? false : null
  }
}

function ensureProjectState() {
  if (state.project) return state.project
  const parts = state.inputPath.replaceAll('\\', '/').split('/')
  state.project = createDefaultProject({ name: parts.at(-2) || '导入工程', mode: state.mode })
  state.project.configuration = state.config
  return state.project
}

function uniqueName(value, fallback) {
  const cleaned = String(value || '').replace(/\.[^.]+$/, '').replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '')
  return cleaned || fallback
}

function sanitizeObjContent(content) {
  const sanitized = String(content)
    .split(/\r?\n/)
    .filter((line) => !/^\s*(mtllib|usemtl)\b/i.test(line))
    .join('\n')
    .trimEnd()
  return sanitized ? sanitized + '\n' : ''
}

function finiteNumber(value, fallback) {
  const number = Number(value)
  return Number.isFinite(number) ? number : fallback
}

const MIN_DISTRIBUTION_DENSITY = .01
const DISTRIBUTION_DENSITY_DECIMALS = 2
const MAX_DISTRIBUTION_INSTANCES = 100000
const SQUARE_METERS_PER_HECTARE = 10000

function densityPerHectare(value, unit = 'hectare') {
  const density = normalizeDistributionDensity(value)
  return unit === 'squareKilometer'
    ? normalizeDistributionDensity(density / 100)
    : density
}

function normalizeDistributionDensity(value, fallback = 200) {
  const number = Number(value)
  const safe = Number.isFinite(number) && number > 0 ? number : fallback
  return Math.max(MIN_DISTRIBUTION_DENSITY, Number(safe.toFixed(DISTRIBUTION_DENSITY_DECIMALS)))
}

function distributionDensityInputValue(value) {
  return normalizeDistributionDensity(value).toFixed(DISTRIBUTION_DENSITY_DECIMALS)
}

function distributionArea(distribution) {
  const width = Math.max(0, Math.abs(distribution.maxX - distribution.minX))
  const height = Math.max(0, Math.abs(distribution.maxY - distribution.minY))
  return Math.max(.000001, width * height)
}

function populationFromValues(countValue, densityValue, area, basis) {
  let count = Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, Math.round(finiteNumber(countValue, 1))))
  let density = normalizeDistributionDensity(densityValue)
  if (basis === 'density') {
    count = Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, Math.round(area * density / SQUARE_METERS_PER_HECTARE)))
  } else {
    density = normalizeDistributionDensity(count / area * SQUARE_METERS_PER_HECTARE)
  }
  return { count, density }
}

function defaultDistribution(item = {}) {
  const sceneX = Math.max(1, finiteNumber(state.config?.scene?.x, 60))
  const sceneY = Math.max(1, finiteNumber(state.config?.scene?.y, 60))
  const vegetation = item.type === 'Vegetation' || !item.type
  const density = 200
  const count = Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, Math.round(sceneX * sceneY * density / SQUARE_METERS_PER_HECTARE)))
  return {
    mode: 'random', basis: 'density', count, density, densityUnit: 'hectare',
    rows: 4, columns: 6,
    minX: 0, maxX: sceneX,
    minY: 0, maxY: sceneY,
    seed: 1, scaleMin: vegetation ? .85 : 1, scaleMax: vegetation ? 1.15 : 1,
    z: 0, offsetX: 0, offsetY: 0, offsetZ: 0, points: ''
  }
}

function distributionFromForm(form, options = {}) {
  const values = Object.fromEntries(new FormData(form).entries())
  const defaults = defaultDistribution({ type: values.objectType })
  const mode = ['random', 'grid', 'single', 'manual'].includes(values.distributionMode) ? values.distributionMode : defaults.mode
  const basis = values.distributionBasis === 'density' ? 'density' : 'count'
  const densityInput = String(values.distributionDensity ?? '').trim()
  const densityValue = Number(densityInput)
  const requiresDensity = options.requireValidDensity &&
    (mode === 'random' || mode === 'grid') && basis === 'density'
  if (requiresDensity && (!densityInput || !Number.isFinite(densityValue) || densityValue < MIN_DISTRIBUTION_DENSITY)) {
    throw new Error('分布密度必须填写大于 0 的有效数值，且保留两位小数')
  }
  const minX = finiteNumber(values.distributionMinX, defaults.minX)
  const maxX = finiteNumber(values.distributionMaxX, defaults.maxX)
  const minY = finiteNumber(values.distributionMinY, defaults.minY)
  const maxY = finiteNumber(values.distributionMaxY, defaults.maxY)
  const population = populationFromValues(
    values.distributionCount,
    values.distributionDensity,
    distributionArea({ minX, maxX, minY, maxY }),
    basis
  )
  return {
    mode,
    basis,
    count: population.count,
    density: population.density,
    densityUnit: 'hectare',
    rows: Math.max(1, Math.round(finiteNumber(values.distributionRows, defaults.rows))),
    columns: Math.max(1, Math.round(finiteNumber(values.distributionColumns, defaults.columns))),
    minX, maxX, minY, maxY,
    seed: Math.round(finiteNumber(values.distributionSeed, defaults.seed)),
    scaleMin: Math.max(.01, finiteNumber(values.distributionScaleMin, defaults.scaleMin)),
    scaleMax: Math.max(.01, finiteNumber(values.distributionScaleMax, defaults.scaleMax)),
    z: finiteNumber(values.distributionZ, defaults.z),
    offsetX: finiteNumber(values.distributionOffsetX, defaults.offsetX),
    offsetY: finiteNumber(values.distributionOffsetY, defaults.offsetY),
    offsetZ: finiteNumber(values.distributionOffsetZ, defaults.offsetZ),
    points: String(values.distributionPoints || '').trim()
  }
}

function setDistributionForm(form, value) {
  const source = value || {}
  const distribution = {
    ...defaultDistribution(),
    ...source,
    density: densityPerHectare(source.density, source.densityUnit),
    densityUnit: 'hectare'
  }
  const fields = {
    distributionMode: distribution.mode, distributionBasis: distribution.basis,
    distributionCount: distribution.count, distributionDensity: distributionDensityInputValue(distribution.density),
    distributionDensityUnit: 'hectare',
    distributionRows: distribution.rows, distributionColumns: distribution.columns,
    distributionMinX: distribution.minX, distributionMaxX: distribution.maxX,
    distributionMinY: distribution.minY, distributionMaxY: distribution.maxY,
    distributionSeed: distribution.seed, distributionScaleMin: distribution.scaleMin,
    distributionScaleMax: distribution.scaleMax, distributionZ: distribution.z,
    distributionOffsetX: distribution.offsetX, distributionOffsetY: distribution.offsetY,
    distributionOffsetZ: distribution.offsetZ,
    distributionPoints: distribution.points || ''
  }
  for (const [name, fieldValue] of Object.entries(fields)) if (form.elements[name]) form.elements[name].value = fieldValue
}

function seededRandom(seed) {
  let value = seed >>> 0
  return () => {
    value += 0x6D2B79F5
    let result = value
    result = Math.imul(result ^ result >>> 15, result | 1)
    result ^= result + Math.imul(result ^ result >>> 7, result | 61)
    return ((result ^ result >>> 14) >>> 0) / 4294967296
  }
}

function distributionTargetCount(distribution) {
  const width = Math.max(0, Math.abs(distribution.maxX - distribution.minX))
  const height = Math.max(0, Math.abs(distribution.maxY - distribution.minY))
  const requested = distribution.basis === 'density'
    ? Math.round(width * height * distribution.density / SQUARE_METERS_PER_HECTARE)
    : Math.round(distribution.count)
  return Math.max(1, Math.min(MAX_DISTRIBUTION_INSTANCES, requested || 1))
}

function isMobileAgentType(type) {
  return type === 'Human' || type === 'Vehicle' || type === 'Ship'
}

function defaultMovement(type) {
  const vehicle = type === 'Vehicle' || type === 'Ship'
  return {
    enabled: false,
    mode: 'random',
    speed: vehicle ? 8.3 : 1.3,
    speedVariation: vehicle ? 1 : .2,
    range: vehicle ? 50 : 20,
    turnInterval: vehicle ? 8 : 5,
    seed: 1
  }
}

function movementFromForm(form) {
  const type = form.elements.objectType.value
  if (!isMobileAgentType(type)) return null
  const defaults = defaultMovement(type)
  return {
    enabled: form.elements.movementEnabled.checked,
    mode: 'random',
    speed: Math.max(0, finiteNumber(form.elements.movementSpeed.value, defaults.speed)),
    speedVariation: Math.max(0, finiteNumber(form.elements.movementSpeedVariation.value, defaults.speedVariation)),
    range: Math.max(.1, finiteNumber(form.elements.movementRange.value, defaults.range)),
    turnInterval: Math.max(.1, finiteNumber(form.elements.movementTurnInterval.value, defaults.turnInterval)),
    seed: Math.round(finiteNumber(form.elements.movementSeed.value, defaults.seed))
  }
}

function setMovementForm(form, movement, type) {
  const value = { ...defaultMovement(type), ...(movement || {}), mode: 'random' }
  form.elements.movementEnabled.checked = Boolean(value.enabled)
  form.elements.movementSpeed.value = value.speed
  form.elements.movementSpeedVariation.value = value.speedVariation
  form.elements.movementRange.value = value.range
  form.elements.movementTurnInterval.value = value.turnInterval
  form.elements.movementSeed.value = value.seed
}

function updateMovementFormState(form) {
  const section = $('#objectMovementFields')
  const visible = isMobileAgentType(form.elements.objectType.value)
  section.hidden = !visible
  const enabled = visible && form.elements.movementEnabled.checked
  section.querySelectorAll('input:not([name="movementEnabled"])').forEach((input) => { input.disabled = !enabled })
}

function parseDistributionPointsText(text, defaultZ = 0) {
  const points = []
  const lines = String(text || '').replace(/^\uFEFF/, '').split(/\r?\n/)
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index].trim()
    if (!line || line.startsWith('#') || line.startsWith('//')) continue
    const values = line.split(/[\s,]+/).filter(Boolean).map(Number)
    if (values.length < 2 || values.length > 5 || !values.every(Number.isFinite)) {
      throw new Error(`自定义坐标第 ${index + 1} 行无效，应为 2–5 个数值`)
    }
    const point = {
      x: values[0], y: values[1], z: values[2] ?? defaultZ,
      scale: values[3] ?? 1, rotation: values[4] ?? 0
    }
    if (point.scale <= 0) throw new Error(`自定义坐标第 ${index + 1} 行的缩放必须大于 0`)
    points.push(point)
  }
  if (!points.length) throw new Error('TXT 中没有有效的自定义坐标')
  if (points.length > MAX_DISTRIBUTION_INSTANCES) throw new Error('单个 OBJ 最多支持 100000 个实例')
  return points
}

function distributionPointsText(points) {
  return points.map((point) => [point.x, point.y, point.z, point.scale, point.rotation].join(' ')).join('\n')
}

function applyDistributionOffset(points, distribution) {
  const offsetX = finiteNumber(distribution.offsetX, 0)
  const offsetY = finiteNumber(distribution.offsetY, 0)
  const offsetZ = finiteNumber(distribution.offsetZ, 0)
  return points.map((point) => ({
    ...point,
    x: point.x + offsetX,
    y: point.y + offsetY,
    z: point.z + offsetZ
  }))
}

function generateDistribution(distribution) {
  const d = { ...defaultDistribution(), ...distribution }
  const minX = Math.min(d.minX, d.maxX), maxX = Math.max(d.minX, d.maxX)
  const minY = Math.min(d.minY, d.maxY), maxY = Math.max(d.minY, d.maxY)
  const scaleMin = Math.min(d.scaleMin, d.scaleMax), scaleMax = Math.max(d.scaleMin, d.scaleMax)
  if (d.mode === 'manual') {
    return applyDistributionOffset(parseDistributionPointsText(d.points, d.z), d)
  }
  if (d.mode === 'single') return applyDistributionOffset([{ x: (minX + maxX) / 2, y: (minY + maxY) / 2, z: d.z, scale: scaleMin, rotation: 0 }], d)
  const targetCount = distributionTargetCount(d)
  if (d.mode === 'grid') {
    const width = Math.max(.001, maxX - minX), height = Math.max(.001, maxY - minY)
    const columns = Math.max(1, Math.ceil(Math.sqrt(targetCount * width / height)))
    const rows = Math.max(1, Math.ceil(targetCount / columns))
    d.rows = rows; d.columns = columns
    return applyDistributionOffset(Array.from({ length: targetCount }, (_, index) => {
      const row = Math.floor(index / columns), column = index % columns
      return {
        x: columns === 1 ? (minX + maxX) / 2 : minX + (maxX - minX) * column / (columns - 1),
        y: rows === 1 ? (minY + maxY) / 2 : minY + (maxY - minY) * row / (rows - 1),
        z: d.z, scale: scaleMin, rotation: 0
      }
    }), d)
  }
  const random = seededRandom(d.seed)
  return applyDistributionOffset(Array.from({ length: targetCount }, () => ({
    x: minX + random() * (maxX - minX), y: minY + random() * (maxY - minY), z: d.z,
    scale: scaleMin + random() * (scaleMax - scaleMin), rotation: random() * 360
  })), d)
}

function distributionSummary(item) {
  const labels = { random: '随机', grid: '网格', single: '单实例', manual: '自定义' }
  const count = item.instanceCount || (item.distribution ? distributionTargetCount({ ...defaultDistribution(item), ...item.distribution }) : 1)
  const control = item.distribution?.basis === 'density' ? `${item.distribution.density} 个/公顷` : `${count} 个实例`
  const offsets = [item.distribution?.offsetX, item.distribution?.offsetY, item.distribution?.offsetZ].map((value) => finiteNumber(value, 0))
  const offset = offsets.some((value) => value !== 0) ? ` · 偏移 (${offsets.join(', ')}) m` : ''
  const movement = isMobileAgentType(item.type) && item.movement?.enabled
    ? ` · 随机移动 ${finiteNumber(item.movement.speed, defaultMovement(item.type).speed)} m/s`
    : ''
  return `${labels[item.distribution?.mode] || '已有位置'} · ${control}${offset}${movement}`
}

function optionList(items, newLabel, selected = '__new__') {
  const create = `<option value="__new__" ${selected === '__new__' ? 'selected' : ''}>新建：${escapeHtml(newLabel)}</option>`
  return create + items.map((item) => `<option value="${escapeHtml(item.name)}" ${item.name === selected ? 'selected' : ''}>${escapeHtml(item.label || item.name)} · ${escapeHtml(item.name)}</option>`).join('')
}

function formField(name) { return document.querySelector("#materialForm [name=\"" + name + "\"]") }

function setFormField(name, value) {
  const input = formField(name)
  if (input && value !== undefined && value !== null) input.value = value
}

function physicalTypeForObject(type) {
  if (type === 'SolarPanel') return 'Photovoltaic'
  if (type === 'Vegetation' || type === 'Fire' || type === 'Fog') return 'Vegetation'
  if (type === 'Water' || type === 'Ship' || type === 'Building') return type
  return 'Soil'
}

function objectClassification(item) {
  return item.classification === 'SolarPanel' ? 'SolarPanel' : (item.type || 'Other')
}

function objectTypeFields(classification) {
  // Keep the engine's supported solid type; the UI classification is persistent metadata.
  return { type: classification === 'SolarPanel' ? 'Building' : classification,
    classification: classification === 'SolarPanel' ? 'SolarPanel' : undefined }
}

function materialMatchesObjectType(material, objectType) {
  if (!material) return false
  // A surface's geometry and semantic label do not restrict photovoltaic EB.
  if (material.energyModel === 'photovoltaic') return objectType !== 'Fire' && objectType !== 'Fog'
  // A thin sheet (tarp, shade net) can cover any solid object.
  if (material.energyModel === 'membrane') return !['Fire', 'Fog', 'Water'].includes(objectType)
  // Keep legacy soil bindings available for existing buildings.
  return material.type === physicalTypeForObject(objectType)
    || (objectType === 'Building' && material.type === 'Soil')
    // The engine exports vehicles and people as Building solids.
    || ((objectType === 'Vehicle' || objectType === 'Human') && material.type === 'Building' && !material.energyModel)
}

function physicalInput(label, name, value, step = 'any') {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><input class="input" name="${name}" type="number" step="${step}" value="${escapeHtml(value)}"></label>`
}

function spectrumFileImportFields(prefix, fileName = '') {
  return `<label class="dialog-field"><span>波谱文件</span><input class="input" id="${prefix}SpectrumFile" type="file" accept=".txt,.dat,.csv,text/plain"></label><input type="hidden" name="fileName" value="${escapeHtml(fileName)}"><div class="notice" id="${prefix}SpectrumFileStatus">${fileName ? `已导入：${escapeHtml(fileName)}` : 'TXT 第 1 行为波长，第 2 行为反射率，第 3 行可选为透射率。'}</div>`
}

function physicalTextureFields(texture = {}) {
  const enabled = Boolean(texture.enabled)
  const fileName = String(texture.fileName || PHYSICAL_TEXTURE_PRESETS[0][0])
  const known = PHYSICAL_TEXTURE_PRESETS.some(([value]) => value === fileName)
  const options = PHYSICAL_TEXTURE_PRESETS.map(([value, label]) =>
    `<option value="${escapeHtml(value)}" ${value === fileName ? 'selected' : ''}>${escapeHtml(label)}</option>`
  ).join('')
  const custom = !known && fileName
    ? `<option value="${escapeHtml(fileName)}" selected>${escapeHtml(fileName)}</option>` : ''
  return `<label class="dialog-field"><span>物理纹理</span><select class="select" name="physicalTextureEnabled"><option value="false" ${enabled ? '' : 'selected'}>关闭（默认）</option><option value="true" ${enabled ? 'selected' : ''}>启用</option></select></label><label class="dialog-field"><span>方差模板</span><select class="select" name="physicalTextureFileName">${options}${custom}</select></label><label class="dialog-field"><span>导入纹理图像</span><input class="input physical-texture-file" type="file" accept=".png,.jpg,.jpeg,.tif,.tiff,image/png,image/jpeg,image/tiff"></label>${physicalInput('方差强度（0–1）', 'physicalTextureStrength', texture.strength ?? 0.2, 0.01)}${physicalInput('反射率变异系数 CV（0–0.5，0 = 按方差强度）', 'physicalTextureRelativeStd', texture.relativeStd ?? 0, 0.01)}${physicalInput('重复尺寸（m）', 'physicalTextureRepeatSize', texture.repeatSize ?? 2, 0.01)}<div class="notice" style="grid-column:1 / -1">仅改变短波反射率的空间方差（按图像亮度，所有波段同一倍率）；按材质面积归一化，平均反射率保持不变。变异系数 CV &gt; 0 时直接指定反射率的相对标准差（例如 0.08 表示 ±8%），图像只决定斑块形状和尺度；为 0 时按“方差强度”缩放，实际 CV 通常只有强度的 1/4 左右。为保证反射率为正且不超过 1−透射率，实际 CV 可能被限低，运行日志会给出实际 CV。热红外发射率和透射率不随此纹理变化。每个三角面取面心处一个值，网格需细于纹理斑块。FacetRT / FacetEB / RayTracing 生效，体元模式不支持。</div>`
}

function bindPhysicalTextureImport(rootSelector) {
  const root = $(rootSelector)
  const input = root?.querySelector('.physical-texture-file')
  const select = root?.querySelector('[name="physicalTextureFileName"]')
  if (!input || !select) return
  input.addEventListener('change', async () => {
    const file = input.files?.[0]
    if (!file) return
    try {
      input.disabled = true
      const imported = await api.importTexture({ name: file.name, contentBase64: await readFileBase64(file) })
      if (![...select.options].some((option) => option.value === imported.relativePath)) {
        select.insertAdjacentHTML('beforeend', `<option value="${escapeHtml(imported.relativePath)}">${escapeHtml(imported.relativePath)}</option>`)
      }
      select.value = imported.relativePath
      invalidateTextureCaches(imported.relativePath)
      const enabled = root.querySelector('[name="physicalTextureEnabled"]')
      if (enabled) enabled.value = 'true'
      toast('纹理导入成功', imported.relativePath, 'success')
    } catch (error) {
      input.value = ''
      toast('纹理导入失败', error.message, 'error')
    } finally { input.disabled = false }
  })
}

function opennessField(value = 0) {
  return `${physicalInput('开孔率（0–0.95）', 'openness', value ?? 0, 0.01)}<div class="notice" style="grid-column:1 / -1">用于遮阳网、穿孔篷布：上方反射率/透射率填纤维本身的值，运行时按开孔率 O 换算为等效值 R′=(1−O)R、T′=O+(1−O)T（热红外同理）。孔中透过的直射光按漫射处理，棚下不形成光斑。仅自定义和导入 TXT 光谱可用。</div>`
}

function textureFileOptions(presets, fileName) {
  const known = presets.some(([value]) => value === fileName)
  return presets.map(([value, label]) => `<option value="${escapeHtml(value)}" ${value === fileName ? 'selected' : ''}>${escapeHtml(label)}</option>`).join('')
    + (!known && fileName ? `<option value="${escapeHtml(fileName)}" selected>${escapeHtml(fileName)}</option>` : '')
}

function enabledSelect(label, name, enabled) {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><select class="select" name="${name}"><option value="false" ${enabled ? '' : 'selected'}>关闭（默认）</option><option value="true" ${enabled ? 'selected' : ''}>启用</option></select></label>`
}

function classTextureFields(texture = {}) {
  const fileName = String(texture.fileName || CLASS_TEXTURE_PRESETS[0][0])
  return `${enabledSelect('光谱分类纹理', 'classTextureEnabled', Boolean(texture.enabled))}<label class="dialog-field"><span>分类图像（PNG）</span><select class="select" name="classTextureFileName">${textureFileOptions(CLASS_TEXTURE_PRESETS, fileName)}</select></label><label class="dialog-field"><span>导入分类图像</span><input class="input class-texture-file" type="file" accept=".png,image/png"></label>${physicalInput('分类纹理重复尺寸（m）', 'classTextureRepeatSize', texture.repeatSize ?? 1.5, 0.01)}<div class="class-texture-rows" style="grid-column:1 / -1" data-classes-file="${escapeHtml(fileName)}" data-classes="${escapeHtml(JSON.stringify(texture.classes || []))}"></div><div class="notice" style="grid-column:1 / -1">图像中每种颜色绑定一条光谱：运行时每个三角面按面心取色，归入颜色最接近的类别并使用该类别光谱（含其透射率、热红外、物理纹理和开孔率设置）。未指定光谱的颜色归入最接近的已指定颜色。按对象自身坐标投影，图案随对象移动；需使用无损 PNG。启用后本光谱自身的反射率不再使用。</div>`
}

function opennessTextureFields(texture = {}) {
  const fileName = String(texture.fileName || OPENNESS_TEXTURE_PRESETS[0][0])
  return `${enabledSelect('开孔率纹理', 'opennessTextureEnabled', Boolean(texture.enabled))}<label class="dialog-field"><span>开孔率图像（灰度 PNG）</span><select class="select" name="opennessTextureFileName">${textureFileOptions(OPENNESS_TEXTURE_PRESETS, fileName)}</select></label><label class="dialog-field"><span>导入开孔率图像</span><input class="input openness-texture-file" type="file" accept=".png,image/png"></label>${physicalInput('开孔率纹理重复尺寸（m）', 'opennessTextureRepeatSize', texture.repeatSize ?? 1, 0.01)}${physicalInput('最小开孔率（黑）', 'opennessTextureMin', texture.minOpenness ?? 0, 0.01)}${physicalInput('最大开孔率（白）', 'opennessTextureMax', texture.maxOpenness ?? 0.5, 0.01)}${physicalInput('开孔率档位数（2–32）', 'opennessTextureLevels', texture.levels ?? 8, 1)}<div class="notice" style="grid-column:1 / -1">图像亮度在最小、最大开孔率之间线性映射（黑 = 最小，白 = 最大），量化为若干档；每个三角面按面心取值，运行时按 R′=(1−O)R、T′=O+(1−O)T 换算透射率和反射率（热红外同理）。启用后覆盖上方固定开孔率。仅自定义和导入 TXT 光谱可用。</div>`
}

const imagePaletteCache = new Map()

// Both caches are keyed by project-relative path; omit fileName on project switch.
function invalidateTextureCaches(fileName) {
  if (fileName === undefined) imagePaletteCache.clear()
  else imagePaletteCache.delete(fileName)
  invalidateImageTextures(fileName)
}

function imagePalette(fileName) {
  if (imagePaletteCache.has(fileName)) return imagePaletteCache.get(fileName)
  const promise = new Promise((resolve, reject) => {
    const image = new Image()
    // DEFERRED[低|性能]: 原尺寸绘制并在主线程逐像素统计颜色，大分类图（如 8K×8K）会长时间阻塞 UI 并占用数百 MB 内存；建议：限制最大边长后缩放采样，或放到 Worker/OffscreenCanvas 中统计 —— 需要引入新的最大边长常量并改变调色板统计口径（缩放采样会影响颜色占比），本轮不改
    image.onload = () => {
      const canvas = document.createElement('canvas')
      canvas.width = image.naturalWidth
      canvas.height = image.naturalHeight
      const context = canvas.getContext('2d', { willReadFrequently: true })
      context.drawImage(image, 0, 0)
      const data = context.getImageData(0, 0, canvas.width, canvas.height).data
      const counts = new Map()
      for (let index = 0; index < data.length; index += 4) {
        const key = (data[index] << 16) | (data[index + 1] << 8) | data[index + 2]
        counts.set(key, (counts.get(key) || 0) + 1)
      }
      const total = data.length / 4
      const colors = [...counts].sort((a, b) => b[1] - a[1]).filter(([, count]) => count / total >= 0.005).slice(0, 12)
        .map(([key, count]) => ({ color: `#${key.toString(16).padStart(6, '0')}`, share: count / total }))
      resolve(colors)
    }
    image.onerror = () => reject(new Error('图像加载失败'))
    image.src = `/api/project/image?path=${encodeURIComponent(fileName)}`
  })
  promise.catch(() => imagePaletteCache.delete(fileName))
  imagePaletteCache.set(fileName, promise)
  return promise
}

const classTextureRequests = new WeakMap()

async function renderClassTextureRows(root, currentSpectrumName) {
  const container = root?.querySelector('.class-texture-rows')
  const select = root?.querySelector('[name="classTextureFileName"]')
  if (!container || !select) return
  const requestedFile = select.value
  const request = (classTextureRequests.get(container) || 0) + 1
  classTextureRequests.set(container, request)
  const existing = [...root.querySelectorAll('[name="classColor"]')].map((input, index) => ({
    color: input.value, spectralName: root.querySelectorAll('[name="classSpectral"]')[index]?.value || ''
  }))
  // FIXED[低|错误处理]: JSON.parse 位于 try 之外，data-classes 损坏时整个函数 reject，容器停留在旧内容且调用方多为未 await 的 fire-and-forget，异常无人处理；已修复：本地 try/catch 解析，损坏时回退为 []
  let saved = existing
  if (!saved.length) {
    try { saved = JSON.parse(container.dataset.classes || '[]') } catch { saved = [] }
  }
  // While the palette loads, keep the bindings submittable, but only when they
  // belong to the image being loaded (colours of another image would misclassify).
  const pending = container.dataset.classesFile === requestedFile
    ? saved.map((entry) => `<input type="hidden" name="classColor" value="${escapeHtml(entry.color)}"><input type="hidden" name="classSpectral" value="${escapeHtml(entry.spectralName)}">`).join('')
    : ''
  container.innerHTML = `<div class="notice">正在读取图像颜色…</div>${pending}`
  const isCurrent = () => classTextureRequests.get(container) === request && select.value === requestedFile
  try {
    const palette = await imagePalette(requestedFile)
    if (!isCurrent()) return
    container.dataset.classesFile = requestedFile
    const choices = (state.config.spectra || []).filter((item) => item.name !== currentSpectrumName && !item.classTexture?.enabled)
    container.innerHTML = palette.length ? palette.map(({ color, share }) => {
      const chosen = saved.find((entry) => entry.color === color)?.spectralName || ''
      const options = choices.map((item) => `<option value="${escapeHtml(item.name)}" ${item.name === chosen ? 'selected' : ''}>${escapeHtml(item.label || item.name)} · ${escapeHtml(item.name)}</option>`).join('')
      return `<div style="display:flex;gap:8px;align-items:center;margin:4px 0"><span style="flex:none;width:22px;height:22px;border-radius:4px;border:1px solid rgba(0,0,0,.25);background:${color}"></span><code style="flex:none">${color}</code><small style="flex:none;width:48px">${(share * 100).toFixed(1)}%</small><input type="hidden" name="classColor" value="${color}"><select class="select" name="classSpectral" style="flex:1"><option value="">（未指定：归入最接近的已指定颜色）</option>${options}</select></div>`
    }).join('') : '<div class="notice">图像中没有占比 ≥ 0.5% 的颜色。</div>'
  } catch (error) {
    if (!isCurrent()) return
    container.innerHTML = `<div class="notice">无法读取图像颜色：${escapeHtml(error.message)}</div>${pending}`
  }
}

function bindTextureImport(root, inputSelector, selectName, enabledName, afterImport) {
  const input = root?.querySelector(inputSelector)
  const select = root?.querySelector(`[name="${selectName}"]`)
  if (!input || !select) return
  input.addEventListener('change', async () => {
    const file = input.files?.[0]
    if (!file) return
    try {
      input.disabled = true
      const imported = await api.importTexture({ name: file.name, contentBase64: await readFileBase64(file) })
      if (![...select.options].some((option) => option.value === imported.relativePath)) {
        select.insertAdjacentHTML('beforeend', `<option value="${escapeHtml(imported.relativePath)}">${escapeHtml(imported.relativePath)}</option>`)
      }
      select.value = imported.relativePath
      invalidateTextureCaches(imported.relativePath)
      const enabled = root.querySelector(`[name="${enabledName}"]`)
      if (enabled) enabled.value = 'true'
      toast('纹理导入成功', imported.relativePath, 'success')
      afterImport?.()
    } catch (error) {
      input.value = ''
      toast('纹理导入失败', error.message, 'error')
    } finally { input.disabled = false }
  })
}

function bindSurfaceTextureFields(rootSelector, currentSpectrumName) {
  const root = $(rootSelector)
  if (!root) return
  const refresh = () => renderClassTextureRows(root, currentSpectrumName)
  root.querySelector('[name="classTextureFileName"]')?.addEventListener('change', refresh)
  bindTextureImport(root, '.class-texture-file', 'classTextureFileName', 'classTextureEnabled', refresh)
  bindTextureImport(root, '.openness-texture-file', 'opennessTextureFileName', 'opennessTextureEnabled')
  refresh()
}

function classTextureFromForm(formData) {
  const colors = formData.getAll('classColor').map(String)
  const spectra = formData.getAll('classSpectral').map(String)
  return {
    enabled: formData.get('classTextureEnabled') === 'true',
    fileName: String(formData.get('classTextureFileName') || ''),
    repeatSize: Math.max(0.01, Number(formData.get('classTextureRepeatSize')) || 1.5),
    classes: colors.map((color, index) => ({ color, spectralName: spectra[index] || '' }))
  }
}

function opennessTextureFromValues(values) {
  const minimum = Math.max(0, Math.min(0.95, Number(values.opennessTextureMin) || 0))
  return {
    enabled: values.opennessTextureEnabled === 'true',
    fileName: String(values.opennessTextureFileName || ''),
    repeatSize: Math.max(0.01, Number(values.opennessTextureRepeatSize) || 1),
    minOpenness: minimum,
    maxOpenness: Math.max(minimum, Math.min(0.95, Number(values.opennessTextureMax) || 0)),
    levels: Math.max(2, Math.min(32, Math.round(Number(values.opennessTextureLevels) || 8)))
  }
}

function previewTextureFields(texture = {}) {
  const enabled = Boolean(texture.enabled)
  const preset = String(texture.preset || PREVIEW_TEXTURE_PRESETS[0][0])
  const options = PREVIEW_TEXTURE_PRESETS.map(([value, label]) =>
    `<option value="${escapeHtml(value)}" ${value === preset ? 'selected' : ''}>${escapeHtml(label)}</option>`
  ).join('')
  return `<label class="dialog-field"><span>Three.js 预览纹理</span><select class="select" name="previewTextureEnabled"><option value="false" ${enabled ? '' : 'selected'}>关闭</option><option value="true" ${enabled ? 'selected' : ''}>启用</option></select></label><label class="dialog-field"><span>常见外观</span><select class="select" name="previewTexturePreset">${options}</select></label>${physicalInput('纹理重复尺寸（m）', 'previewTextureRepeatSize', texture.repeatSize ?? 2, 0.05)}<div class="notice" style="grid-column:1 / -1">只用于 Three.js 场景预览，不参与 HiStream 求解，也不改变反射率、发射率或模拟结果。</div>`
}

function previewTextureFromValues(values) {
  return {
    enabled: values.previewTextureEnabled === 'true',
    preset: String(values.previewTexturePreset || PREVIEW_TEXTURE_PRESETS[0][0]),
    repeatSize: Math.max(0.05, Number(values.previewTextureRepeatSize) || 2)
  }
}

function physicalTextureFromValues(values) {
  return {
    enabled: values.physicalTextureEnabled === 'true',
    fileName: String(values.physicalTextureFileName || ''),
    strength: Math.max(0, Math.min(1, Number(values.physicalTextureStrength) || 0)),
    relativeStd: Math.max(0, Math.min(0.5, Number(values.physicalTextureRelativeStd) || 0)),
    repeatSize: Math.max(0.01, Number(values.physicalTextureRepeatSize) || 2)
  }
}

function bindSpectrumFileInput(prefix, rootSelector) {
  const root = $(rootSelector)
  const input = root?.querySelector(`#${prefix}SpectrumFile`)
  const hidden = root?.querySelector('[name="fileName"]')
  const status = root?.querySelector(`#${prefix}SpectrumFileStatus`)
  if (!input || !hidden || !status) return
  input.addEventListener('change', async () => {
    const file = input.files?.[0]
    if (!file) return
    try {
      input.disabled = true
      const imported = await api.importSpectrum({ name: file.name, content: await file.text() })
      hidden.value = imported.relativePath
      status.textContent = `已导入：${imported.relativePath}`
      toast('波谱导入成功', imported.relativePath, 'success')
    } catch (error) {
      input.value = ''
      toast('波谱导入失败', error.message, 'error')
    } finally { input.disabled = false }
  })
}

function renderModelParameters(fileName = '') {
  const model = $('#spectralModel').value
  if (model === 'Prospect') {
    $('#modelParameterFields').innerHTML = `${field('Cab', '<input class="input" name="paramCab" type="number" step="0.1" value="40">')}${field('Cw', '<input class="input" name="paramCw" type="number" step="0.001" value="0.01">')}${field('Cdm', '<input class="input" name="paramCdm" type="number" step="0.001" value="0.01">')}${field('Cs', '<input class="input" name="paramCs" type="number" step="0.001" value="0">')}${field('N', '<input class="input" name="paramN" type="number" step="0.1" value="1.5">')}`
  } else if (model === 'BSM') {
    $('#modelParameterFields').innerHTML = `${field('SMC', '<input class="input" name="paramSMC" type="number" step="0.1" value="25">')}${field('亮度', '<input class="input" name="paramBSMBrightness" type="number" step="0.1" value="0.5">')}${field('纬度参数', '<input class="input" name="paramBSMlat" type="number" step="0.1" value="25">')}${field('经度参数', '<input class="input" name="paramBSMlon" type="number" step="0.1" value="45">')}${field('湿端 TIR 反射率', '<input class="input" name="refTirWet" type="number" step="0.005" min="0" max="1" placeholder="留空自动">')}`
  } else if (model === 'file') {
    $('#modelParameterFields').innerHTML = spectrumFileImportFields('material', fileName)
    bindSpectrumFileInput('material', '#materialForm')
  } else {
    const count = configuredSpectralBands().length
    $('#modelParameterFields').innerHTML = `<div class="notice">自定义模型按传感器波段填写反射率和透射率；当前为 ${count} 个波段。</div>`
  }
}

function materialSpectrumValues(name) {
  return $$(`#materialForm [name="${name}Value"]`).map((input) => input.value.trim()).filter(Boolean).join(',')
}

function renderMaterialSpectrumValueFields(reflectance = '0.20', transmittance = '0.0') {
  const multiband = $('#spectralModel').value === 'custom'
  $('#materialReflectanceFields').innerHTML = presetSpectrumFields('反射率', 'reflectance', reflectance, '0.20', multiband)
  $('#materialTransmittanceFields').innerHTML = presetSpectrumFields('透射率', 'transmittance', transmittance, '0.0', multiband)
}

function applySpectrumPreset() {
  const selected = state.config.spectra.find((item) => item.name === document.querySelector('#spectralPreset').value)
  const nameInput = document.querySelector('#spectralMaterialName')
  if (selected) {
    nameInput.value = selected.name
    nameInput.readOnly = true
    document.querySelector('#spectralModel').value = selected.model || 'custom'
    setFormField('refTir', selected.refTir ?? 0.05)
    setFormField('tauTir', selected.tauTir ?? 0)
    renderModelParameters(selected.fileName || '')
    // The BSM branch of renderModelParameters rebuilds #modelParameterFields, so the wet TIR value has to be
    // applied after it, next to the model parameters.
    setFormField('refTirWet', selected.refTirWet ?? '')
    renderMaterialSpectrumValueFields(selected.reflectance || '0.20', selected.transmittance || '0.0')
    $('#materialPhysicalTextureFields').innerHTML = opennessField(selected.openness) + previewTextureFields(selected.previewTexture) + physicalTextureFields(selected.physicalTexture) + specularParameterFields(selected.specular, false)
    bindPhysicalTextureImport('#materialForm')
    const params = selected.params || {}
    for (const [key, value] of Object.entries({ Cab: params.Cab, Cw: params.Cw, Cdm: params.Cdm, Cs: params.Cs, N: params.N, SMC: params.SMC, BSMBrightness: params.BSMBrightness, BSMlat: params.BSMlat, BSMlon: params.BSMlon })) setFormField('param' + key, value)
  } else {
    nameInput.readOnly = false
    renderModelParameters()
    renderMaterialSpectrumValueFields()
    $('#materialPhysicalTextureFields').innerHTML = opennessField() + previewTextureFields() + physicalTextureFields() + specularParameterFields()
    bindPhysicalTextureImport('#materialForm')
  }
}

function renderSpectrumPresetOptions() {
  const type = document.querySelector("#materialObjectType").value
  const preferred = { SolarPanel: "pv_opaque", Vegetation: "green_leaf", Fire: "fire_medium", Fog: "fog_medium", Soil: "soil", Building: "concrete", Human: "human_surface", Vehicle: "vehicle_surface", Ship: "ship_surface", Other: "concrete", Water: "water_surface" }[type]
  const current = document.querySelector("#spectralPreset").value
  const selected = state.config.spectra.some((item) => item.name === current) ? current : preferred
  document.querySelector("#spectralPreset").innerHTML = optionList(state.config.spectra, document.querySelector("#spectralMaterialName").value, selected)
  applySpectrumPreset()
}

function applyThermalPreset() {
  const selected = state.config.thermals.find((item) => item.name === document.querySelector("#thermalPreset").value)
  const nameInput = document.querySelector("#thermalMaterialName")
  if (selected) {
    nameInput.value = selected.name
    nameInput.readOnly = true
    setFormField("sunlitTemperature", selected.sunlitTemperature)
    setFormField("shadedTemperature", selected.shadedTemperature)
  } else nameInput.readOnly = false
}

function renderThermalPresetOptions() {
  const type = document.querySelector("#materialObjectType").value
  const preferred = { SolarPanel: "pv_temperature", Vegetation: "vegetation_temperature", Fire: "fire_temperature", Fog: "fog_temperature", Soil: "soil_temperature", Building: "building_temperature", Human: "human_temperature", Vehicle: "vehicle_temperature", Ship: "ship_temperature", Other: "building_temperature", Water: "water_temperature" }[type]
  const current = document.querySelector("#thermalPreset").value
  const selected = state.config.thermals.some((item) => item.name === current) ? current : preferred
  document.querySelector("#thermalPreset").innerHTML = optionList(state.config.thermals, document.querySelector("#thermalMaterialName").value, selected)
  applyThermalPreset()
}

function renderPhysicalParameters() {
  const type = physicalTypeForObject($('#materialObjectType').value)
  const selected = state.config.materials.find((item) => item.name === $('#physicalMaterialPreset').value)
  const nameInput = $('#physicalMaterialName')
  if (selected) {
    nameInput.value = selected.name
    nameInput.readOnly = true
    $('#physicalParameterFields').innerHTML = `<div class="notice">使用“${escapeHtml(selected.label || selected.name)}”预设：${physicalValues(selected).map(([key, value]) => `${escapeHtml(key)}=${escapeHtml(value)}`).join('，')}</div>`
    return
  }
  nameInput.readOnly = false
  if (nameInput.dataset.newName) nameInput.value = nameInput.dataset.newName
  const defaults = createMaterialPresets().materials.find((item) => type === 'Photovoltaic' ? item.energyModel === 'photovoltaic' : item.type === type && !item.energyModel)?.params || {}
  if (type === 'Photovoltaic') {
    $('#physicalParameterFields').innerHTML = [['参考效率（25℃）','eta25',0.001],['温度系数（K⁻¹）','gamma',0.0001],['背面发电系数','bifaciality',0.01],['面热容（J·m⁻²·K⁻¹）','heatCapacityPerArea',100],['对流换热倍率','convectiveScale',0.1]].map(([label, key, step]) => physicalInput(label, 'pv_' + key, defaults[key], step)).join('')
  } else if (type === 'Vegetation') {
    $('#physicalParameterFields').innerHTML = [
      physicalInput('Vcmax', 'bioVcmax', defaults.Vcmax, 0.1), physicalInput('气孔斜率 m', 'bioM', defaults.m, 0.1),
      physicalInput('Ball-Berry 截距', 'bioBallBerry', defaults.BallBerry, 0.001), physicalInput('光合类型', 'bioType', defaults.Type, 1),
      physicalInput('kV', 'bioKV', defaults.kV, 0.0001), physicalInput('暗呼吸系数', 'bioRdparam', defaults.Rdparam, 0.001),
      `<label class="dialog-field"><span>温度响应参数</span><input class="input" name="bioTparam" value="${escapeHtml(defaults.Tparam)}"></label>`,
      physicalInput('年均温', 'bioTyear', defaults.Tyear, 0.1), physicalInput('beta', 'bioBeta', defaults.beta, 0.001),
      physicalInput('NPQ 系数', 'bioKNPQs', defaults.kNPQs, 0.01), physicalInput('光限制', 'bioQLs', defaults.qLs, 0.01),
      physicalInput('胁迫因子', 'bioStressfactor', defaults.stressfactor, 0.01), physicalInput('温度校正', 'bioTcor', defaults.Tcor, 1)
    ].join('')
  } else if (type === 'Water') {
    $('#physicalParameterFields').innerHTML = [
      physicalInput('表面阻力 rss', 'waterRss', defaults.rss, 1), physicalInput('体积热容', 'waterHeatCapacity', defaults.heatCapacity, 1),
      physicalInput('混合深度', 'waterMixingDepth', defaults.mixingDepth, 0.1), physicalInput('蒸发系数', 'waterEvaporationCoefficient', defaults.evaporationCoefficient, 0.1),
      physicalInput('辐射模型（0 Lambert / 1 Cox–Munk）', 'waterBrdfModel', defaults.brdfModel, 1), physicalInput('水体折射率', 'waterRefractiveIndex', defaults.refractiveIndex, 0.001),
      physicalInput('水面坡度方差（0 随风速）', 'waterSlopeVariance', defaults.slopeVariance, 0.001), physicalInput('漫反射比例', 'waterDiffuseFraction', defaults.diffuseFraction, 0.01)
    ].join('')
  } else {
    $('#physicalParameterFields').innerHTML = [
      physicalInput('方法', 'soilMethod', defaults.method, 1), physicalInput('表面阻力 rss', 'soilRss', defaults.rss, 1),
      physicalInput('比热容 cs', 'soilCs', defaults.cs, 1), physicalInput('密度 rhos', 'soilRhos', defaults.rhos, 1),
      physicalInput('导热率 λ', 'soilLambdas', defaults.lambdas, 0.01), physicalInput('初始温度', 'soilTsoil', defaults.Tsoil, 0.1),
      physicalInput('土壤含水量 SMC', 'soilSMC', defaults.SMC, 0.1), physicalInput('饱和含水量', 'soilSatwater', defaults.Satwater, 0.01)
    ].join('')
  }
}

function renderPhysicalPresetOptions() {
  const objectType = $('#materialObjectType').value
  const type = physicalTypeForObject(objectType)
  const materials = state.config.materials.filter((item) => materialMatchesObjectType(item, objectType))
  const preferred = { SolarPanel: 'pv_panel', Vegetation: 'leaf_c3', Fire: 'leaf_c3', Fog: 'leaf_c3', Soil: 'soilset', Building: 'building_surface', Human: 'soil_dry', Vehicle: 'vehicle_material', Ship: 'ship_material', Other: 'soilset', Water: 'water_set' }[objectType]
  $('#physicalMaterialPreset').innerHTML = optionList(materials, $('#physicalMaterialName').value, materials.some((item) => item.name === preferred) ? preferred : materials[0]?.name || '__new__')
  renderPhysicalParameters()
}

function recommendedVegetationMeshBinding(meshName, fallback) {
  const name = String(meshName || '').toLowerCase()
  if (/(leaf|leaves|foliage|needle|叶片|树叶|针叶)/.test(name)) {
    return { ...fallback, spectralName: 'green_leaf', thermalName: 'vegetation_temperature', materialName: 'leaf_c3', canopyName: 'tree_leaf_canopy' }
  }
  if (/(branch|branches|twig|limb|树枝|枝条)/.test(name)) {
    return { ...fallback, spectralName: 'tree_wood', thermalName: 'tree_wood_temperature', materialName: 'tree_branch_wood', canopyName: 'tree_branch_rigid' }
  }
  if (/(trunk|stem|bark|wood|树干|树皮|木质)/.test(name)) {
    return { ...fallback, spectralName: 'tree_wood', thermalName: 'tree_wood_temperature', materialName: 'tree_trunk_wood', canopyName: 'tree_trunk_rigid' }
  }
  return fallback
}

function renderMeshMaterialRows({ recommendVegetation = false } = {}) {
  const pending = state.pendingObject
  if (!pending) return
  const type = $('#materialObjectType').value
  const preferredSpectrum = { SolarPanel: 'pv_opaque', Vegetation: 'green_leaf', Fire: 'fire_medium', Fog: 'fog_medium', Soil: 'soil', Building: 'concrete', Human: 'human_surface', Vehicle: 'vehicle_surface', Ship: 'ship_surface', Other: 'concrete', Water: 'water_surface' }[type]
  const preferredThermal = { SolarPanel: 'pv_temperature', Vegetation: 'vegetation_temperature', Fire: 'fire_temperature', Fog: 'fog_temperature', Soil: 'soil_temperature', Building: 'building_temperature', Human: 'human_temperature', Vehicle: 'vehicle_temperature', Ship: 'ship_temperature', Other: 'building_temperature', Water: 'water_temperature' }[type]
  const selectedSpectrum = document.querySelector("#spectralPreset").value || preferredSpectrum
  const selectedThermal = document.querySelector("#thermalPreset").value || preferredThermal
  const physicalItems = state.config.materials.filter((item) => materialMatchesObjectType(item, type))
  const preferredPhysical = { SolarPanel: 'pv_panel', Vegetation: 'leaf_c3', Fire: 'leaf_c3', Fog: 'leaf_c3', Soil: 'soilset', Building: 'building_surface', Human: 'soil_dry', Vehicle: 'vehicle_material', Ship: 'ship_material', Other: 'soilset', Water: 'water_set' }[type]
  const selectedPhysical = $('#physicalMaterialPreset').value || preferredPhysical
  const photovoltaic = physicalItems.find((item) => item.name === selectedPhysical)?.energyModel === 'photovoltaic'
  const preferredCanopy = photovoltaic ? 'rigid_body' : type === 'Fire' ? 'fire_medium' : type === 'Fog' ? 'fog_medium' : type === 'Vegetation' ? 'tree_leaf_canopy' : 'rigid_body'
  const savedSpecular = new Map($$('#meshMaterialRows tr').map(row => [row.dataset.meshName, meshSpecularFromRow(row)]))
  $('#meshMaterialRows').innerHTML = pending.meshNames.map((name, index) => {
    const defaults = {
      spectralName: selectedSpectrum,
      thermalName: selectedThermal,
      materialName: selectedPhysical,
      canopyName: preferredCanopy
    }
    const binding = recommendVegetation && type === 'Vegetation' && !photovoltaic
      ? recommendedVegetationMeshBinding(name, defaults) : defaults
    const spectrumOptions = optionList(state.config.spectra || [], $('#spectralMaterialName').value, binding.spectralName)
    const thermalOptions = optionList(state.config.thermals || [], $('#thermalMaterialName').value, binding.thermalName)
    const physicalOptions = optionList(physicalItems, $('#physicalMaterialName').value, binding.materialName)
    const canopyOptions = bindingOptionList(state.config.canopies || [], binding.canopyName, binding.canopyName)
    return `<tr data-mesh-name="${escapeHtml(name)}"><td title="${escapeHtml(name)}">${escapeHtml(name)}</td><td><select class="select mesh-spectrum" data-index="${index}">${spectrumOptions}</select></td><td><select class="select mesh-thermal" data-index="${index}">${thermalOptions}</select></td><td><select class="select mesh-physical" data-index="${index}">${physicalOptions}</select></td><td><select class="select mesh-canopy" data-index="${index}">${canopyOptions}</select></td><td>${meshSpecularFields({ enabled: savedSpecular.get(name) })}</td></tr>`
  }).join('')
}

function showMaterialDialog() {
  const pending = state.pendingObject
  if (!pending) return
  $('#materialForm').reset()
  const base = uniqueName(pending.source.path, 'object')
  const suggestedType = pending.suggestedType || 'Vegetation'
  $('#materialObjectName').value = base
  $('#materialObjectType').value = suggestedType
  $('#physicalMaterialName').value = `${base}_material`
  $('#spectralMaterialName').value = `${base}_spectral`
  $('#thermalMaterialName').value = `${base}_temperature`
  $('#physicalMaterialName').dataset.newName = `${base}_material`
  $('#spectralPreset').value = '__new__'
  $('#thermalPreset').value = '__new__'
  $('#physicalMaterialPreset').value = '__new__'
  $('#spectralModel').value = suggestedType === 'Vegetation' ? 'Prospect' : suggestedType === 'Soil' ? 'BSM' : 'custom'
  renderModelParameters(); renderSpectrumPresetOptions(); renderThermalPresetOptions(); renderPhysicalPresetOptions()
  renderMeshMaterialRows({ recommendVegetation: true })
  $('#materialDialog').hidden = false
}

function hideMaterialDialog(discard = true) {
  $('#materialDialog').hidden = true
  if (discard && state.pendingObject) disposeObject(state.pendingObject.object)
  if (discard) state.pendingObject = null
}

function geometryValues() {
  const values = Object.fromEntries(new FormData($('#geometryForm')).entries())
  const result = {
    name: uniqueName(values.geometryName, 'geometry'),
    shape: values.geometryShape === 'cube' ? 'cube' : 'ellipsoid',
    mode: ['surface', 'chaos', 'water', 'fire', 'fog'].includes(values.geometryMode) ? values.geometryMode : 'surface',
    sizeX: Number(values.geometrySizeX), sizeY: Number(values.geometrySizeY), sizeZ: Number(values.geometrySizeZ),
    detail: Math.max(1, Math.min(32, Math.round(Number(values.geometryDetail) || 2))),
    basis: ['lai', 'lad', 'count'].includes(values.chaosBasis) ? values.chaosBasis : 'lai',
    amount: Number(values.chaosAmount), facetArea: Number(values.chaosFacetArea),
    orientation: ['spherical', 'horizontal', 'vertical'].includes(values.chaosOrientation) ? values.chaosOrientation : 'spherical',
    seed: Math.round(Number(values.chaosSeed) || 0),
    positionX: Number(values.mediumPositionX), positionY: Number(values.mediumPositionY), positionZ: Number(values.mediumPositionZ),
    extinction: Number(values.mediumExtinction), scatteringAlbedo: Number(values.mediumScatteringAlbedo),
    asymmetry: Number(values.mediumAsymmetry), temperature: Number(values.mediumTemperature), emissionScale: Number(values.mediumEmissionScale),
    waterPositionX: Number(values.waterPositionX), waterPositionY: Number(values.waterPositionY), waterPositionZ: Number(values.waterPositionZ),
    waterOffsetX: Number(values.waterOffsetX), waterOffsetY: Number(values.waterOffsetY), waterOffsetZ: Number(values.waterOffsetZ)
  }
  if (![result.sizeX, result.sizeY, result.sizeZ].every((value) => Number.isFinite(value) && value > 0)) throw new Error('几何体三个方向的尺寸必须大于 0')
  if (result.mode === 'chaos' && (!Number.isFinite(result.amount) || result.amount <= 0 || !Number.isFinite(result.facetArea) || result.facetArea <= 0)) throw new Error('填充强度和单个面元面积必须大于 0')
  if ((result.mode === 'fire' || result.mode === 'fog') && ![result.positionX, result.positionY, result.positionZ, result.extinction, result.scatteringAlbedo, result.asymmetry, result.temperature, result.emissionScale].every(Number.isFinite)) throw new Error('参与介质参数必须为有效数值')
  if ((result.mode === 'fire' || result.mode === 'fog') && (result.extinction < 0 || result.scatteringAlbedo < 0 || result.scatteringAlbedo > 1 || Math.abs(result.asymmetry) >= 1 || result.temperature < 0 || result.emissionScale < 0)) throw new Error('参与介质参数超出有效范围')
  if (result.mode === 'water' && ![result.waterPositionX, result.waterPositionY, result.waterPositionZ, result.waterOffsetX, result.waterOffsetY, result.waterOffsetZ].every(Number.isFinite)) throw new Error('水体位置与偏移必须为有效数值')
  return result
}

function chaosTriangleCount(values) {
  if (values.basis === 'count') return Math.round(values.amount)
  const footprint = values.shape === 'cube'
    ? values.sizeX * values.sizeZ
    : Math.PI * values.sizeX * values.sizeZ / 4
  const volume = values.shape === 'cube'
    ? values.sizeX * values.sizeY * values.sizeZ
    : Math.PI * values.sizeX * values.sizeY * values.sizeZ / 6
  return Math.ceil((values.basis === 'lad' ? values.amount * volume : values.amount * footprint) / values.facetArea)
}

function surfaceTriangleCount(values) {
  if (values.mode === 'water') return values.shape === 'cube'
    ? 2 * values.detail * values.detail
    : Math.max(16, values.detail * 16)
  return values.shape === 'cube' ? 12 * values.detail * values.detail : 16 * values.detail * (4 * values.detail - 1)
}

function updateGeometryDialog() {
  const form = $('#geometryForm')
  if (!form) return
  const chaos = form.elements.geometryMode.value === 'chaos'
  const medium = form.elements.geometryMode.value === 'fire' || form.elements.geometryMode.value === 'fog'
  const fog = form.elements.geometryMode.value === 'fog'
  const water = form.elements.geometryMode.value === 'water'
  $('#geometryShapeField').hidden = false
  $('#geometrySizeYField').hidden = water
  $('#geometryShapeLabel').textContent = water ? '水面形状' : '包络形状'
  form.elements.geometryShape.options[0].textContent = water ? '圆形 / 椭圆' : '椭球'
  form.elements.geometryShape.options[1].textContent = water ? '矩形' : '立方体'
  $('#geometrySizeYLabel').textContent = medium ? 'Y 尺寸（m）' : 'Y 高度（m）'
  $('#geometrySizeZLabel').textContent = medium ? 'Z 高度（m）' : water ? 'Y 尺寸（m）' : 'Z 尺寸（m）'
  $('#geometrySurfaceFields').hidden = chaos || medium
  $('#geometryChaosFields').hidden = !chaos
  $('#geometryWaterFields').hidden = !water
  $('#geometryMediumFields').hidden = !medium
  $('#geometryMediumNotice').textContent = fog
    ? '坐标统一为 X 南北、Y 向上、Z 东西；雾会按消光系数衰减，并按反照率和非对称因子散射辐射。'
    : '坐标统一为 X 南北、Y 向上、Z 东西；火焰会衰减、散射并按固定温度发射。'
  $('#geometrySurfaceNotice').textContent = water
    ? '生成朝上的水平三角网格水面；水面高程可在对象“分布”中通过 Z 设置。'
    : '生成封闭外壳，适用于建筑等连续表面。立方体按六个面细分，椭球生成连续三角网格。'
  const labels = { lai: 'LAI（m²/m²）', lad: 'LAD（m²/m³）', count: '面元数量' }
  $('#geometryAmountLabel').textContent = labels[form.elements.chaosBasis.value] || labels.lai
  try {
    const values = geometryValues()
    const triangles = values.mode === 'chaos'
      ? chaosTriangleCount(values)
      : surfaceTriangleCount(values)
    $('#geometryEstimate').textContent = medium
      ? `将生成 ${(values.shape === 'ellipsoid' ? '椭球' : '立方体')}${fog ? '雾' : '火焰'}体积；中心平面位置 X/Z=(${values.positionX}, ${values.positionY}) m，底部 Y=${values.positionZ} m。`
      : water
        ? `将生成 ${values.shape === 'ellipsoid' ? '圆形/椭圆' : '矩形'}水面 ${values.sizeX} × ${values.sizeZ} m；最终中心 X/Y/Z=(${values.waterPositionX + values.waterOffsetX}, ${values.waterPositionY + values.waterOffsetY}, ${values.waterPositionZ + values.waterOffsetZ}) m；共 ${Math.max(1, triangles).toLocaleString('zh-CN')} 个三角面元。`
      : `预计生成 ${Math.max(1, triangles).toLocaleString('zh-CN')} 个三角面元；Y 方向为高度，底面位于 Y=0。`
  } catch (error) {
    $('#geometryEstimate').textContent = error.message
  }
}

function showGeometryDialog() {
  if (!state.inputPath) { toast('无法生成几何体', '请先新建或打开工程', 'error'); return }
  const form = $('#geometryForm')
  form.reset()
  form.elements.geometryName.value = `geometry_${(state.config?.objects?.items?.length || 0) + 1}`
  updateGeometryDialog()
  $('#geometryDialog').hidden = false
  setTimeout(() => form.elements.geometryName.focus(), 0)
}

function setMediumDefaults(kind) {
  const form = $('#geometryForm')
  const fog = kind === 'fog'
  form.elements.geometryMode.value = fog ? 'fog' : 'fire'
  form.elements.geometryName.value = `${fog ? 'fog' : 'fire'}_${(state.config?.objects?.items?.length || 0) + 1}`
  form.elements.geometryShape.value = 'ellipsoid'
  form.elements.geometrySizeX.value = fog ? state.config.scene.x : 25
  form.elements.geometrySizeY.value = fog ? state.config.scene.y : 25
  form.elements.geometrySizeZ.value = fog ? Math.max(5, state.config.scene.height) : 10
  form.elements.mediumPositionX.value = state.config.scene.x / 2
  form.elements.mediumPositionY.value = state.config.scene.y / 2
  form.elements.mediumPositionZ.value = 0
  form.elements.mediumExtinction.value = fog ? 0.08 : 1.2
  form.elements.mediumScatteringAlbedo.value = fog ? 0.95 : 0.72
  form.elements.mediumAsymmetry.value = fog ? 0.85 : 0.55
  form.elements.mediumTemperature.value = fog ? 288 : 1100
  form.elements.mediumEmissionScale.value = 1
}

function setWaterDefaults() {
  const form = $('#geometryForm')
  form.elements.geometryMode.value = 'water'
  form.elements.geometryName.value = `water_${(state.config?.objects?.items?.length || 0) + 1}`
  form.elements.geometryShape.value = 'cube'
  form.elements.geometrySizeX.value = Math.max(1, Number(state.config?.scene?.x) || 60)
  form.elements.geometrySizeY.value = 0.01
  form.elements.geometrySizeZ.value = Math.max(1, Number(state.config?.scene?.y) || 60)
  form.elements.geometryDetail.value = 2
  form.elements.waterPositionX.value = Math.max(1, Number(state.config?.scene?.x) || 60) / 2
  form.elements.waterPositionY.value = Math.max(1, Number(state.config?.scene?.y) || 60) / 2
  form.elements.waterPositionZ.value = 0.01
  form.elements.waterOffsetX.value = 0
  form.elements.waterOffsetY.value = 0
  form.elements.waterOffsetZ.value = 0
}

function showMediumDialog(kind = 'fire') {
  if (!state.inputPath) { toast('无法添加参与介质', '请先新建或打开工程', 'error'); return }
  if (state.mode !== 'eVoxelRT' && state.mode !== 'eVoxelEB') { toast('参与介质不可用', '请先切换到 VoxelRT 或 VoxelEB', 'error'); return }
  const form = $('#geometryForm')
  form.reset()
  setMediumDefaults(kind)
  updateGeometryDialog()
  $('#geometryDialog').hidden = false
  setTimeout(() => form.elements.geometryName.focus(), 0)
}

function hideGeometryDialog() {
  $('#geometryDialog').hidden = true
}

function objNumber(value) {
  const rounded = Math.abs(value) < 5e-9 ? 0 : value
  return Number(rounded.toFixed(7)).toString()
}

function surfaceGeometryObj(values) {
  const medium = values.mode === 'fire' || values.mode === 'fog'
  const sizeYUp = medium ? values.sizeZ : values.sizeY
  const sizeZDepth = medium ? values.sizeY : values.sizeZ
  const geometry = values.shape === 'cube'
    ? new THREE.BoxGeometry(values.sizeX, sizeYUp, sizeZDepth, values.detail, values.detail, values.detail)
    : new THREE.SphereGeometry(1, Math.max(8, values.detail * 8), Math.max(4, values.detail * 4))
  if (values.shape === 'cube') geometry.translate(0, sizeYUp / 2, 0)
  else {
    geometry.scale(values.sizeX / 2, sizeYUp / 2, sizeZDepth / 2)
    geometry.translate(0, sizeYUp / 2, 0)
  }
  const triangles = geometry.index ? geometry.toNonIndexed() : geometry
  const positions = triangles.getAttribute('position')
  const lines = [
    '# Stream Studio generated OBJ',
    '# generation: geometry',
    `# envelope: ${values.shape} ${values.sizeX} ${values.sizeY} ${values.sizeZ}`,
    `o ${values.name}`, `g ${values.name}`, 's off'
  ]
  for (let index = 0; index < positions.count; index += 1) {
    lines.push(`v ${objNumber(positions.getX(index))} ${objNumber(positions.getY(index))} ${objNumber(positions.getZ(index))}`)
  }
  for (let index = 0; index < positions.count; index += 3) lines.push(`f ${index + 1} ${index + 2} ${index + 3}`)
  if (triangles !== geometry) triangles.dispose()
  geometry.dispose()
  return `${lines.join('\n')}\n`
}

function waterSurfaceObj(values) {
  const detail = Math.max(1, values.detail)
  const geometry = values.shape === 'cube'
    ? new THREE.PlaneGeometry(values.sizeX, values.sizeZ, detail, detail)
    : new THREE.CircleGeometry(1, Math.max(16, detail * 16))
  geometry.rotateX(-Math.PI / 2)
  if (values.shape === 'ellipsoid') geometry.scale(values.sizeX / 2, 1, values.sizeZ / 2)
  const triangles = geometry.index ? geometry.toNonIndexed() : geometry
  const positions = triangles.getAttribute('position')
  const lines = [
    '# Stream Studio generated water surface OBJ',
    '# generation: water-surface',
    `# extent: ${values.sizeX} ${values.sizeZ}`,
    `o ${values.name}`, `g ${values.name}`, 's off'
  ]
  for (let index = 0; index < positions.count; index += 1) {
    lines.push(`v ${objNumber(positions.getX(index))} ${objNumber(positions.getY(index))} ${objNumber(positions.getZ(index))}`)
  }
  for (let index = 0; index < positions.count; index += 3) lines.push(`f ${index + 1} ${index + 2} ${index + 3}`)
  if (triangles !== geometry) triangles.dispose()
  geometry.dispose()
  return `${lines.join('\n')}\n`
}

function normalizeVector([x, y, z]) {
  const length = Math.hypot(x, y, z) || 1
  return [x / length, y / length, z / length]
}

function crossVector(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

function chaosNormal(random, orientation) {
  const azimuth = random() * Math.PI * 2
  if (orientation === 'horizontal') {
    const tilt = (random() * 2 - 1) * Math.PI / 12
    return [Math.sin(tilt) * Math.cos(azimuth), Math.cos(tilt), Math.sin(tilt) * Math.sin(azimuth)]
  }
  if (orientation === 'vertical') {
    const elevation = (random() * 2 - 1) * Math.PI / 12
    return [Math.cos(elevation) * Math.cos(azimuth), Math.sin(elevation), Math.cos(elevation) * Math.sin(azimuth)]
  }
  const y = random() * 2 - 1
  const horizontal = Math.sqrt(Math.max(0, 1 - y * y))
  return [horizontal * Math.cos(azimuth), y, horizontal * Math.sin(azimuth)]
}

function chaosCenter(random, values) {
  if (values.shape === 'cube') return [(random() - 0.5) * values.sizeX, random() * values.sizeY, (random() - 0.5) * values.sizeZ]
  const direction = chaosNormal(random, 'spherical')
  const radius = Math.cbrt(random())
  return [direction[0] * radius * values.sizeX / 2, values.sizeY / 2 + direction[1] * radius * values.sizeY / 2, direction[2] * radius * values.sizeZ / 2]
}

function insideEnvelope(point, values) {
  if (values.shape === 'cube') return Math.abs(point[0]) <= values.sizeX / 2 && point[1] >= 0 && point[1] <= values.sizeY && Math.abs(point[2]) <= values.sizeZ / 2
  const x = point[0] / (values.sizeX / 2)
  const y = (point[1] - values.sizeY / 2) / (values.sizeY / 2)
  const z = point[2] / (values.sizeZ / 2)
  return x * x + y * y + z * z <= 1 + 1e-7
}

function chaosGeometryObj(values) {
  const count = chaosTriangleCount(values)
  if (!Number.isFinite(count) || count < 1) throw new Error('计算得到的面元数量必须大于 0')
  if (count > 200000) throw new Error(`预计生成 ${count.toLocaleString('zh-CN')} 个面元，超过 200,000 上限；请降低 LAI/LAD 或增大单个面元面积`)
  const random = seededRandom(values.seed)
  const radius = Math.sqrt(4 * values.facetArea / (3 * Math.sqrt(3)))
  const angles = [0, Math.PI * 2 / 3, Math.PI * 4 / 3]
  const lines = [
    '# Stream Studio generated OBJ', '# generation: geometry-chaos',
    `# envelope: ${values.shape} ${values.sizeX} ${values.sizeY} ${values.sizeZ}`,
    `# facet-area: ${values.facetArea}`, `# triangle-count: ${count}`,
    `o ${values.name}`, `g ${values.name}_chaos`, 's off'
  ]
  let generated = 0
  for (let triangle = 0; triangle < count; triangle += 1) {
    let points = null
    for (let attempt = 0; attempt < 80 && !points; attempt += 1) {
      const center = chaosCenter(random, values)
      const normal = chaosNormal(random, values.orientation)
      const reference = Math.abs(normal[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]
      const axisU = normalizeVector(crossVector(reference, normal))
      const axisV = normalizeVector(crossVector(normal, axisU))
      const candidate = angles.map((angle) => [
        center[0] + radius * (Math.cos(angle) * axisU[0] + Math.sin(angle) * axisV[0]),
        center[1] + radius * (Math.cos(angle) * axisU[1] + Math.sin(angle) * axisV[1]),
        center[2] + radius * (Math.cos(angle) * axisU[2] + Math.sin(angle) * axisV[2])
      ])
      if (candidate.every((point) => insideEnvelope(point, values))) points = candidate
    }
    if (!points) throw new Error(`第 ${triangle + 1} 个面元无法放入包络；请减小单个面元面积或增大几何尺寸`)
    for (const point of points) lines.push(`v ${objNumber(point[0])} ${objNumber(point[1])} ${objNumber(point[2])}`)
    const first = generated * 3 + 1
    lines.push(`f ${first} ${first + 1} ${first + 2}`)
    generated += 1
  }
  return `${lines.join('\n')}\n`
}

function preparePendingObject(result, content, metadata = {}) {
  const object = new OBJLoader().parse(content)
  const meshNames = []
  object.traverse((child) => { if (child.isMesh) { const name = child.name || child.material?.name || `mesh_${meshNames.length + 1}`; if (!meshNames.includes(name)) meshNames.push(name) } })
  if (!meshNames.length) throw new Error('OBJ 中没有可用 mesh')
  const box = new THREE.Box3().setFromObject(object)
  const size = box.getSize(new THREE.Vector3())
  const center = box.getCenter(new THREE.Vector3())
  const scale = 8 / Math.max(size.x, size.y, size.z, .001)
  object.position.sub(center).multiplyScalar(scale)
  object.scale.setScalar(scale)
  object.position.y += size.y * scale / 2
  state.pendingObject = { source: { ...result, content }, object, meshNames, dimensions: metadata.dimensions || [size.x, size.y, size.z], ...metadata }
}

async function generateGeometryObject(event) {
  event.preventDefault()
  const submit = $('#geometryForm button[type="submit"]')
  const drawPlacement = state.drawPlacement
  state.drawPlacement = null
  try {
    submit.disabled = true
    ensureProjectState()
    const values = geometryValues()
    if (state.config.objects.items.some((item) => item.name === values.name)) throw new Error(`对象名称已存在：${values.name}`)
    const medium = values.mode === 'fire' || values.mode === 'fog'
    if (medium && state.mode !== 'eVoxelRT' && state.mode !== 'eVoxelEB') throw new Error('参与介质当前只支持 VoxelRT 或 VoxelEB')
    const content = values.mode === 'chaos'
      ? chaosGeometryObj(values)
      : values.mode === 'water' ? waterSurfaceObj(values) : surfaceGeometryObj(values)
    const source = new OBJLoader().parse(content)
    const meshNames = []
    source.traverse((child) => { if (child.isMesh) { const name = child.name || child.material?.name || `mesh_${meshNames.length + 1}`; if (!meshNames.includes(name)) meshNames.push(name) } })
    disposeObject(source)
    if (!meshNames.length) throw new Error('生成结果中没有可用三角面元')
    const type = values.mode === 'fire' ? 'Fire' : values.mode === 'fog' ? 'Fog' : values.mode === 'chaos' ? 'Vegetation' : values.mode === 'water' ? 'Water' : 'Building'
    const imported = await api.importObj({ name: `PRIM_${values.name}.obj`, content })
    const distribution = {
      ...defaultDistribution({ type }), mode: 'single', basis: 'count', count: 1, scaleMin: 1, scaleMax: 1,
      // Position files expose X north, Z east and Y height. Internal x/y/z keys
      // retain their legacy order: x = north, y = east, z = height.
      ...(medium ? { minX: values.positionX, maxX: values.positionX, minY: values.positionY, maxY: values.positionY, z: values.positionZ } : {}),
      ...(drawPlacement && !medium && type !== 'Water' ? {
        minX: drawPlacement.north, maxX: drawPlacement.north,
        minY: drawPlacement.east, maxY: drawPlacement.east,
        z: drawPlacement.height
      } : {}),
      ...(type === 'Water' ? {
        minX: values.waterPositionX, maxX: values.waterPositionX,
        minY: values.waterPositionY, maxY: values.waterPositionY,
        z: values.waterPositionZ,
        offsetX: values.waterOffsetX, offsetY: values.waterOffsetY, offsetZ: values.waterOffsetZ
      } : {})
    }
    const instances = generateDistribution(distribution)
    const savedDistribution = await api.saveDistribution({ path: imported.positionPath, name: values.name, instances })
    const mediumLabel = type === 'Fog' ? '雾' : '火焰'
    const mediumKey = type === 'Fog' ? 'fog' : 'fire'
    const spectralName = medium ? `${values.name}_${mediumKey}_spectrum` : type === 'Vegetation' ? 'green_leaf' : type === 'Water' ? 'water_surface' : 'concrete'
    const thermalName = medium ? `${values.name}_${mediumKey}_temperature` : type === 'Vegetation' ? 'vegetation_temperature' : type === 'Water' ? 'water_temperature' : 'building_temperature'
    const canopyName = medium ? `${values.name}_${mediumKey}_medium` : type === 'Vegetation' ? 'canopy_default' : 'rigid_body'
    if (medium) {
      upsertByName(state.config.spectra, {
        name: spectralName, label: `${values.name} · ${mediumLabel}介质`,
        model: 'custom', reflectance: '0', transmittance: '0', refTir: 0, tauTir: 0, params: {}
      })
      upsertByName(state.config.thermals, {
        name: thermalName, label: `${values.name} · 固定温度`,
        sunlitTemperature: values.temperature, shadedTemperature: values.temperature
      })
      upsertByName(state.config.canopies, {
        name: canopyName, label: `${values.name} · ${mediumLabel}`,
        structureType: values.mode, lai: 0, density: 0, hc: values.sizeZ, G: 0,
        LIDFa: 0, LIDFb: 0, hspot: 0, leafwidth: 0,
        extinction: values.extinction, scatteringAlbedo: values.scatteringAlbedo,
        asymmetry: values.asymmetry, emissionScale: values.emissionScale,
        fixedTemperature: values.temperature
      })
    }
    const item = {
      name: values.name, type,
      materialName: type === 'Vegetation' || medium ? 'leaf_c3' : type === 'Water' ? 'water_set' : 'soil_dry', canopyName,
      fileName: imported.path, positionFile: savedDistribution.path,
      shape: values.shape, dimensions: [values.sizeX, values.sizeY, values.sizeZ],
      meshes: meshNames.map((name) => ({
        name, spectralName, thermalName,
        materialName: medium ? 'leaf_c3' : type === 'Water' ? 'water_set' : type === 'Vegetation' ? 'leaf_c3' : 'soil_dry',
        canopyName
      })),
      distribution, instanceCount: 1, sourceKind: 'prim',
      ...(medium ? { medium: { kind: values.mode, radiationOnly: true, coordinateOrder: 'XYZ' } } : {}),
      generation: { type: values.mode === 'fire' ? '火焰' : values.mode === 'fog' ? '雾' : values.mode === 'chaos' ? '几何体混沌' : values.mode === 'water' ? '水体' : '几何体', shape: values.shape, parameters: values }
    }
    state.config.objects.items.push(item)
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    state.project.configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('PRIM 原型保存失败')
    hideGeometryDialog()
    state.panel = 'objects'
    state.selectedObjectIndex = state.config.objects.items.length - 1
    $$('.tree-item').forEach((button) => button.classList.toggle('active', button.dataset.panel === 'objects'))
    refreshFromState(false)
    await loadActualScene(state.config)
    renderInspector()
    showObjectAttributeDialog(state.selectedObjectIndex)
    const triangleCount = values.mode === 'chaos' ? chaosTriangleCount(values) : surfaceTriangleCount(values)
    addLog(`${medium ? '参与介质' : 'PRIM 原型'}已生成并加入场景：${values.name}${medium ? ' · VoxelRT / VoxelEB' : ` · ${triangleCount} 个三角面元`}`, 'success')
    toast(medium ? '参与介质对象已创建' : 'PRIM 原型生成完成', `${values.name} · 已显示在场景中`)
  } catch (error) { toast('几何体生成失败', error.message, 'error') }
  finally { submit.disabled = false }
}

async function importObj() {
  if (!state.inputPath) { toast('无法导入 OBJ', '请先新建或打开工程', 'error'); return }
  // FIXED[中|错误处理]: api.chooseObj() 在 try 之外，读取/选择失败会变成未处理的 Promise rejection，用户无任何提示；另外若已有 pendingObject 未释放就被 preparePendingObject 覆盖，旧 Object3D 的几何/材质泄漏；已修复：把 await 放进 try/catch 并 toast 提示，覆盖前先 disposeObject(state.pendingObject.object)
  let result
  try { result = await api.chooseObj() }
  catch (error) { toast('无法导入 OBJ', error.message, 'error'); addLog(`无法导入 OBJ：${error.message}`, 'error'); return }
  if (!result) return
  try {
    const content = sanitizeObjContent(result.content)
    if (state.pendingObject) disposeObject(state.pendingObject.object)
    preparePendingObject(result, content)
    showMaterialDialog()
  } catch (error) { toast('OBJ 解析失败', error.message, 'error') }
}

function upsertByName(list, value) {
  const index = list.findIndex((item) => item.name === value.name)
  if (index >= 0) list[index] = value
  else list.push(value)
}

// FIXED[中|Bug]: 各物化参数直接 Number(values.xxx)：空输入框得到 0（如 Vcmax、cs、rhos、lambdas 变 0），非数字得到 NaN；Wood 分支 Math.max(1, NaN) 仍为 NaN，会原样写进 project.json 并交给引擎；已修复：返回前过滤掉 params 中非有限的数字项（缺失/非法即跳过，不引入新的默认值），NaN 不再写进工程；空串被当成 0 的字段（未包 Math.max 的 Vcmax/SMC 等）行为未改，见下方说明
function physicalFromForm(values, type) {
  let params
  if (type === 'Photovoltaic') {
    params = Object.fromEntries(['eta25','gamma','bifaciality','heatCapacityPerArea','convectiveScale'].map(k=>[k,Number(values['pv_'+k])]))
  } else if (type === 'Membrane') {
    params = {
      heatCapacityPerArea: Math.max(1, Number(values.membraneHeatCapacityPerArea) || 800),
      convectiveScale: Math.max(0.1, Number(values.membraneConvectiveScale) || 1)
    }
  } else if (type === 'Wood') {
    params = {
      method: 1, rss: 1000000000, cs: Math.max(1, Number(values.soilCs)), rhos: Math.max(1, Number(values.soilRhos)),
      lambdas: Math.max(0.001, Number(values.soilLambdas)), Tsoil: Number(values.soilTsoil), SMC: 0, Satwater: 0,
      convectiveScale: Math.max(0.1, Number(values.woodConvectiveScale))
    }
  } else if (type === 'Vegetation') {
    params = {
      Vcmax: Number(values.bioVcmax), m: Number(values.bioM), BallBerry: Number(values.bioBallBerry), Type: Number(values.bioType),
      kV: Number(values.bioKV), Rdparam: Number(values.bioRdparam), Tparam: values.bioTparam, Tyear: Number(values.bioTyear),
      beta: Number(values.bioBeta), kNPQs: Number(values.bioKNPQs), qLs: Number(values.bioQLs),
      stressfactor: Number(values.bioStressfactor), Tcor: Number(values.bioTcor)
    }
  } else if (type === 'Water') {
    params = {
      rss: Math.max(0, Number(values.waterRss)), heatCapacity: Math.max(1, Number(values.waterHeatCapacity)),
      mixingDepth: Math.max(0.01, Number(values.waterMixingDepth)), evaporationCoefficient: Math.max(0, Math.min(1, Number(values.waterEvaporationCoefficient))),
      brdfModel: Number(values.waterBrdfModel) === 1 ? 1 : 0, refractiveIndex: Math.max(1, Number(values.waterRefractiveIndex)),
      slopeVariance: Math.max(0, Number(values.waterSlopeVariance)), diffuseFraction: Math.max(0, Math.min(1, Number(values.waterDiffuseFraction)))
    }
  } else {
    params = {
      method: Number(values.soilMethod), rss: Number(values.soilRss), cs: Number(values.soilCs), rhos: Number(values.soilRhos),
      lambdas: Number(values.soilLambdas), Tsoil: Number(values.soilTsoil), SMC: Number(values.soilSMC), Satwater: Number(values.soilSatwater)
    }
  }
  // FIXED 配套：过滤掉 NaN/Infinity，避免“缺失”被写成 NaN（JSON 里是 null）
  params = Object.fromEntries(Object.entries(params).filter(([, value]) => typeof value !== 'number' || Number.isFinite(value)))
  if (type === 'Photovoltaic') return {name:values.materialName,type:'Building',energyModel:'photovoltaic',params}
  if (type === 'Membrane') return { name: values.materialName, type: 'Building', energyModel: 'membrane', params }
  return type === 'Wood'
    ? { name: values.materialName, type: 'Vegetation', energyModel: 'wood', params }
    : { name: values.materialName, type, params }
}

function presetSelect(label, id, name, options) {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><select class="select" id="${id}" name="${name}">${options}</select></label>`
}

function presetText(label, name, value, attrs = '') {
  return `<label class="dialog-field"><span>${escapeHtml(label)}</span><input class="input" name="${name}" value="${escapeHtml(value)}" ${attrs}></label>`
}

function splitSpectrumValues(value, fallback) {
  const values = String(value ?? '').split(',').map((entry) => entry.trim()).filter(Boolean)
  return values.length ? values : [String(fallback)]
}

function configuredSpectralBands() {
  const bands = String(state.config?.sensor?.bands || '').split(/[\s,;]+/).map((entry) => entry.trim()).filter(Boolean)
  return bands.length ? bands : ['默认波段']
}

function presetSpectrumFields(label, name, value, fallback, useConfiguredBands = false) {
  const sourceValues = splitSpectrumValues(value, fallback)
  const bands = useConfiguredBands ? configuredSpectralBands() : []
  const fieldCount = bands.length > 1 ? bands.length : 1
  return Array.from({ length: fieldCount }, (_, index) => {
    const entry = sourceValues[index] ?? sourceValues[0] ?? String(fallback)
    const fieldLabel = fieldCount > 1 ? `${label}（${bands[index]} nm）` : label
    return presetText(fieldLabel, `${name}Value`, entry, 'type="number" min="0" max="1" step="any" required')
  }).join('')
}

function currentSpectrumValues(name) {
  return $$(`#presetParameterFields [name="${name}Value"]`).map((input) => input.value.trim()).filter(Boolean).join(',')
}

function presetList(kind) {
  if (kind === 'spectrum') return state.config.spectra
  if (kind === 'thermal') return state.config.thermals
  if (kind === 'canopy') return state.config.canopies
  return state.config.materials
}

function renderPresetParameterFields(spectralModel = 'custom', item = null) {
  const kind = $('#presetKind').value
  const target = $('#presetParameterFields')
  if (kind === 'spectrum') {
    const current = item || {}
    const models = [['custom', '自定义（面板数据）'], ['Prospect', 'PROSPECT 模型'], ['BSM', 'BSM 模型'], ['file', '导入波谱 TXT']]
    const modelOptions = models.map(([value, label]) => `<option value="${value}" ${value === spectralModel ? 'selected' : ''}>${label}</option>`).join('')
    const modelDefault = createMaterialPresets().spectra.find((entry) => entry.model === spectralModel) || createMaterialPresets().spectra[0]
    const defaults = { ...(modelDefault.params || {}), ...finiteParams(current.params) }
    let parameters = ''
    if (spectralModel === 'Prospect') {
      parameters = `${physicalInput('Cab', 'paramCab', defaults.Cab, 0.1)}${physicalInput('Cw', 'paramCw', defaults.Cw, 0.001)}${physicalInput('Cdm', 'paramCdm', defaults.Cdm, 0.001)}${physicalInput('Cs', 'paramCs', defaults.Cs, 0.001)}${physicalInput('N', 'paramN', defaults.N, 0.1)}`
    } else if (spectralModel === 'BSM') {
      parameters = `${physicalInput('土壤含水量 SMC', 'paramSMC', defaults.SMC, 0.1)}${physicalInput('亮度', 'paramBSMBrightness', defaults.BSMBrightness, 0.1)}${physicalInput('纬度参数', 'paramBSMlat', defaults.BSMlat, 0.1)}${physicalInput('经度参数', 'paramBSMlon', defaults.BSMlon, 0.1)}${physicalInput('湿端 TIR 反射率（饱和土壤，留空自动取下限）', 'refTirWet', current.refTirWet ?? '', 0.005)}<div class="notice" style="grid-column:1 / -1">土壤发射率随含水量升高：SMC ≤ 0.05 为上面的 TIR 反射率，SMC 越高越接近这里的湿端值（留空即干端发射率 +0.03，上限 0.995）。发射率 = 1 − TIR 反射率 − TIR 透射率。</div>`
    } else if (spectralModel === 'file') {
      parameters = spectrumFileImportFields('preset', current.fileName || '')
    }
    target.innerHTML = `${presetSelect('光谱模型', 'presetSpectralModel', 'spectralModel', modelOptions)}${presetSpectrumFields('反射率', 'reflectance', current.reflectance, spectralModel === 'Prospect' ? '0.08' : '0.20', spectralModel === 'custom')}${presetSpectrumFields('透射率', 'transmittance', current.transmittance, spectralModel === 'Prospect' ? '0.04' : '0.0', spectralModel === 'custom')}${physicalInput('TIR 反射率', 'refTir', current.refTir ?? 0.05, 0.01)}${physicalInput('TIR 透射率', 'tauTir', current.tauTir ?? 0, 0.01)}${opennessField(current.openness)}${parameters}${previewTextureFields(current.previewTexture)}${physicalTextureFields(current.physicalTexture)}${specularParameterFields(current.specular)}${classTextureFields(current.classTexture)}${opennessTextureFields(current.opennessTexture)}`
    bindPhysicalTextureImport('#presetForm')
    bindSurfaceTextureFields('#presetForm', current.name || $('#presetName').value)
    $('#presetSpectralModel').addEventListener('change', (event) => {
      const preservedParams = finiteParams(current.params)
      $$(`#presetParameterFields [name^="param"]`).forEach((input) => {
        const value = finiteInputNumber(input)
        if (value !== null) preservedParams[input.name.slice(5)] = value
      })
      const formData = new FormData($('#presetForm'))
      renderPresetParameterFields(event.target.value, {
        ...current,
        model: event.target.value,
        reflectance: currentSpectrumValues('reflectance'),
        transmittance: currentSpectrumValues('transmittance'),
        refTir: Number($('#presetParameterFields [name="refTir"]').value),
        refTirWet: optionalTirReflectance($('#presetParameterFields [name="refTirWet"]')?.value),
        tauTir: Number($('#presetParameterFields [name="tauTir"]').value),
        openness: Number($('#presetParameterFields [name="openness"]')?.value) || 0,
        fileName: $('#presetParameterFields [name="fileName"]')?.value || '',
        previewTexture: previewTextureFromValues(Object.fromEntries(formData.entries())),
        physicalTexture: physicalTextureFromValues(Object.fromEntries(formData.entries())),
        specular: specularParametersFromValues(Object.fromEntries(formData.entries()), current.specular),
        classTexture: classTextureFromForm(formData),
        opennessTexture: opennessTextureFromValues(Object.fromEntries(formData.entries())),
        params: preservedParams
      })
    })
    if (spectralModel === 'file') bindSpectrumFileInput('preset', '#presetForm')
    return
  }
  if (kind === 'thermal') {
    target.innerHTML = `${physicalInput('阳面温度（K）', 'sunlitTemperature', item?.sunlitTemperature ?? 305, 0.1)}${physicalInput('阴面温度（K）', 'shadedTemperature', item?.shadedTemperature ?? 295, 0.1)}`
    return
  }
  if (kind === 'canopy') {
    const defaults = { ...createMaterialPresets().canopies[0], ...(item || {}) }
    const structureOptions = [['canopy', '混沌介质（多孔透射）'], ['rigid', '刚体（不透光轮廓）'], ['fire', '火焰（参与介质）'], ['fog', '雾（参与介质）']]
      .map(([value, label]) => `<option value="${value}" ${value === defaults.structureType ? 'selected' : ''}>${label}</option>`).join('')
    target.innerHTML = `${presetSelect('结构类型', 'presetStructureType', 'structureType', structureOptions)}${physicalInput('叶面积指数 LAI', 'lai', defaults.lai, 0.01)}${physicalInput('叶面积密度 LAD', 'density', defaults.density, 0.01)}${physicalInput('介质高度 hc（m）', 'hc', defaults.hc, 0.01)}${physicalInput('投影系数 G', 'G', defaults.G, 0.01)}${physicalInput('叶倾角参数 LIDFa', 'LIDFa', defaults.LIDFa, 0.01)}${physicalInput('叶倾角参数 LIDFb', 'LIDFb', defaults.LIDFb, 0.01)}${physicalInput('热点参数 hspot', 'hspot', defaults.hspot, 0.01)}${physicalInput('叶宽 leafwidth（m）', 'leafwidth', defaults.leafwidth, 0.01)}${physicalInput('消光系数（m⁻¹）', 'extinction', defaults.extinction ?? 0, 0.01)}${physicalInput('单次散射反照率', 'scatteringAlbedo', defaults.scatteringAlbedo ?? 0, 0.01)}${physicalInput('散射非对称因子', 'asymmetry', defaults.asymmetry ?? 0, 0.01)}${physicalInput('发射倍率', 'emissionScale', defaults.emissionScale ?? 0, 0.01)}${physicalInput('固定温度（K）', 'fixedTemperature', defaults.fixedTemperature ?? 0, 1)}`
    return
  }
  const typeOptions = [['Building', '建筑表面'], ['Photovoltaic', '光伏板热电耦合'], ['Membrane', '篷布/织物薄膜（FacetEB）'], ['Vegetation', '植被生理生化'], ['Wood', '木质固体热平衡'], ['Soil', '土壤表面物化'], ['Water', '水体物性'], ['Ship', '船舶表面物化']].map(([value, label]) => `<option value="${value}" ${value === state.presetPhysicalType ? 'selected' : ''}>${label}</option>`).join('')
  const baseDefaults = createMaterialPresets().materials.find((entry) => state.presetPhysicalType === 'Photovoltaic' ? entry.energyModel === 'photovoltaic' : state.presetPhysicalType === 'Membrane' ? entry.energyModel === 'membrane' : state.presetPhysicalType === 'Wood' ? entry.energyModel === 'wood' : entry.type === state.presetPhysicalType && !entry.energyModel)?.params || {}
  const defaults = { ...baseDefaults, ...(item?.params || {}) }
  let parameters
  if (state.presetPhysicalType === 'Membrane') {
    parameters = `${physicalInput('面热容（J·m⁻²·K⁻¹）', 'membraneHeatCapacityPerArea', defaults.heatCapacityPerArea, 10)}${physicalInput('对流换热倍率', 'membraneConvectiveScale', defaults.convectiveScale, 0.1)}<div class="notice" style="grid-column:1 / -1">单层薄板：两面共用一个温度，两面同时吸收/发射辐射并与空气对流换热，不计蒸腾。面热容 = 面密度 × 比热容，例如 PVC 涂层帆布 0.6 kg·m⁻² × 1300 ≈ 800；PE 篷布 0.15 × 1900 ≈ 300；HDPE 遮阳网（含孔）约 100–300。仅 FacetEB 使用，VoxelEB 不支持。</div>`
  } else if (state.presetPhysicalType === 'Photovoltaic') {
    parameters = [['参考效率（25℃）','eta25',0.001],['温度系数（K⁻¹）','gamma',0.0001],['背面发电系数','bifaciality',0.01],['面热容（J·m⁻²·K⁻¹）','heatCapacityPerArea',100],['对流换热倍率','convectiveScale',0.1]].map(([label,k,step])=>physicalInput(label,'pv_'+k,defaults[k],step)).join('')+'<div class="notice">使用 FacetEB 与不透明光谱。温度由辐射、对流、储热与发电共同求解；OBJ 正面为电池正面。</div>'
  } else if (state.presetPhysicalType === 'Vegetation') {
    parameters = [
      physicalInput('Vcmax', 'bioVcmax', defaults.Vcmax, 0.1), physicalInput('气孔斜率 m', 'bioM', defaults.m, 0.1),
      physicalInput('Ball-Berry 截距', 'bioBallBerry', defaults.BallBerry, 0.001), physicalInput('光合类型', 'bioType', defaults.Type, 1),
      physicalInput('kV', 'bioKV', defaults.kV, 0.0001), physicalInput('暗呼吸系数', 'bioRdparam', defaults.Rdparam, 0.001),
      presetText('温度响应参数', 'bioTparam', defaults.Tparam), physicalInput('年均温', 'bioTyear', defaults.Tyear, 0.1),
      physicalInput('beta', 'bioBeta', defaults.beta, 0.001), physicalInput('NPQ 系数', 'bioKNPQs', defaults.kNPQs, 0.01),
      physicalInput('光限制', 'bioQLs', defaults.qLs, 0.01), physicalInput('胁迫因子', 'bioStressfactor', defaults.stressfactor, 0.01),
      physicalInput('温度校正', 'bioTcor', defaults.Tcor, 1)
    ].join('')
  } else if (state.presetPhysicalType === 'Water') {
    parameters = `${physicalInput('表面阻力 rss', 'waterRss', defaults.rss, 1)}${physicalInput('体积热容', 'waterHeatCapacity', defaults.heatCapacity, 1)}${physicalInput('混合深度', 'waterMixingDepth', defaults.mixingDepth, 0.1)}${physicalInput('蒸发系数', 'waterEvaporationCoefficient', defaults.evaporationCoefficient, 0.1)}${physicalInput('辐射模型（0 Lambert / 1 Cox–Munk）', 'waterBrdfModel', defaults.brdfModel, 1)}${physicalInput('水体折射率', 'waterRefractiveIndex', defaults.refractiveIndex, 0.001)}${physicalInput('水面坡度方差（0 随风速）', 'waterSlopeVariance', defaults.slopeVariance, 0.001)}${physicalInput('漫反射比例', 'waterDiffuseFraction', defaults.diffuseFraction, 0.01)}`
  } else if (state.presetPhysicalType === 'Wood') {
    parameters = `${physicalInput('比热容 cs（J·kg⁻¹·K⁻¹）', 'soilCs', defaults.cs, 1)}${physicalInput('密度 rhos（kg·m⁻³）', 'soilRhos', defaults.rhos, 1)}${physicalInput('导热率 λ（W·m⁻¹·K⁻¹）', 'soilLambdas', defaults.lambdas, 0.01)}${physicalInput('对流换热倍率', 'woodConvectiveScale', defaults.convectiveScale ?? 1, 0.1)}${physicalInput('初始温度（℃）', 'soilTsoil', defaults.Tsoil, 0.1)}<div class="notice">木质固体不计算光合作用和蒸腾，温度由辐射、对流、导热与热储存共同求解。</div>`
  } else {
    parameters = `${physicalInput('方法', 'soilMethod', defaults.method, 1)}${physicalInput('表面阻力 rss', 'soilRss', defaults.rss, 1)}${physicalInput('比热容 cs', 'soilCs', defaults.cs, 1)}${physicalInput('密度 rhos', 'soilRhos', defaults.rhos, 1)}${physicalInput('导热率 λ', 'soilLambdas', defaults.lambdas, 0.01)}${physicalInput('初始温度', 'soilTsoil', defaults.Tsoil, 0.1)}${physicalInput('土壤含水量 SMC', 'soilSMC', defaults.SMC, 0.1)}${physicalInput('饱和含水量', 'soilSatwater', defaults.Satwater, 0.01)}`
  }
  target.innerHTML = `${presetSelect('物化类型', 'presetPhysicalType', 'physicalType', typeOptions)}${parameters}`
  $('#presetPhysicalType').addEventListener('change', (event) => {
    state.presetPhysicalType = event.target.value
    renderPresetParameterFields('custom')
  })
}

function showPresetDialog(kind = 'spectrum', index = -1) {
  if (!state.inputPath) { toast('无法编辑材质属性', '请先新建或打开工程', 'error'); return }
  $('#presetForm').reset()
  const list = presetList(kind)
  const item = Number.isInteger(index) && index >= 0 ? list[index] : null
  state.editingPreset = item ? { kind, index } : null
  $('#presetKind').value = kind
  $('#presetKind').disabled = Boolean(item)
  $('#presetName').readOnly = Boolean(item)
  $('#presetName').value = item?.name || ''
  $('#presetLabel').value = item?.label || ''
  state.presetPhysicalType = item?.energyModel === 'photovoltaic' ? 'Photovoltaic' : item?.energyModel === 'membrane' ? 'Membrane' : item?.energyModel === 'wood' ? 'Wood' : (item?.type || 'Vegetation')
  $('#presetDialogTitle').textContent = item ? `修改材质属性 · ${item.label || item.name}` : '新增材质属性'
  $('#presetSubmitBtn').innerHTML = item ? '<svg><use href="#i-save"/></svg>保存修改' : '<svg><use href="#i-plus"/></svg>加入预设库'
  renderPresetParameterFields(item?.model || 'custom', item)
  $('#presetDialog').hidden = false
  setTimeout(() => (item ? $('#presetParameterFields .input, #presetParameterFields .select') : $('#presetName'))?.focus(), 0)
}

function hidePresetDialog() {
  $('#presetDialog').hidden = true
  $('#presetKind').disabled = false
  $('#presetName').readOnly = false
  state.editingPreset = null
}

const SPECTRUM_MODEL_PARAMS = ['Cab', 'Cw', 'Cdm', 'Cs', 'N', 'SMC', 'BSMBrightness', 'BSMlat', 'BSMlon']

// Parameters missing from the form (other spectral models) or stored as null
// must not override model defaults with NaN/0.
function finiteParams(params) {
  return Object.fromEntries(Object.entries(params || {}).filter(([, value]) => value !== null && value !== '' && Number.isFinite(Number(value))))
}

function spectrumParamsFromValues(values, previous) {
  const params = finiteParams(previous)
  for (const key of SPECTRUM_MODEL_PARAMS) {
    const raw = String(values[`param${key}`] ?? '').trim()
    if (raw !== '' && Number.isFinite(Number(raw))) params[key] = Number(raw)
  }
  return params
}
// Blank keeps the engine default: the wet (saturated) TIR reflectance is derived
// from the dry one, so soil emissivity still rises with water content.
function optionalTirReflectance(value) {
  if (value === '' || value === undefined || value === null) return undefined
  const numeric = Number(value)
  if (!Number.isFinite(numeric)) return undefined
  return Math.max(0, Math.min(1, numeric))
}

function presetFromForm(values) {
  const editing = state.editingPreset
  const existing = editing ? presetList(editing.kind)[editing.index] : null
  const name = existing?.name || uniqueName(values.presetName, 'material')
  const label = String(values.presetLabel || '').trim() || name
  const kind = editing?.kind || values.presetKind
  if (kind === 'spectrum') {
    const classTexture = values.classTexture
    const opennessTexture = opennessTextureFromValues(values)
    const surfaceTextures = {
      ...(classTexture?.enabled || existing?.classTexture ? { classTexture } : {}),
      ...(opennessTexture.enabled || existing?.opennessTexture ? { opennessTexture } : {})
    }
    return {
      list: state.config.spectra,
      item: { name, label, model: values.spectralModel, fileName: values.fileName || '', reflectance: values.reflectance, transmittance: values.transmittance, refTir: Number(values.refTir), refTirWet: optionalTirReflectance(values.refTirWet), tauTir: Number(values.tauTir), openness: Number(values.openness) || 0, previewTexture: previewTextureFromValues(values), physicalTexture: physicalTextureFromValues(values), specular: specularParametersFromValues(values, existing?.specular), ...surfaceTextures, params: spectrumParamsFromValues(values, existing?.params) }
    }
  }
  if (kind === 'thermal') {
    return { list: state.config.thermals, item: { name, label, sunlitTemperature: Number(values.sunlitTemperature), shadedTemperature: Number(values.shadedTemperature) } }
  }
  if (kind === 'canopy') {
    return {
      list: state.config.canopies,
      item: {
        name, label,
        structureType: ['canopy', 'rigid', 'fire', 'fog'].includes(values.structureType) ? values.structureType : 'canopy',
        lai: Number(values.lai), density: Number(values.density), hc: Number(values.hc), G: Number(values.G),
        LIDFa: Number(values.LIDFa), LIDFb: Number(values.LIDFb), hspot: Number(values.hspot), leafwidth: Number(values.leafwidth),
        extinction: Math.max(0, Number(values.extinction) || 0),
        scatteringAlbedo: Math.max(0, Math.min(1, Number(values.scatteringAlbedo) || 0)),
        asymmetry: Math.max(-0.99, Math.min(0.99, Number(values.asymmetry) || 0)),
        emissionScale: Math.max(0, Number(values.emissionScale) || 0),
        fixedTemperature: Math.max(0, Number(values.fixedTemperature) || 0)
      }
    }
  }
  return { list: state.config.materials, item: { ...physicalFromForm({ ...values, materialName: name }, values.physicalType), label } }
}

async function savePreset(event) {
  event.preventDefault()
  const submit = $('#presetForm button[type="submit"]')
  const formData = new FormData($('#presetForm'))
  const values = Object.fromEntries(formData.entries())
  values.presetKind = state.editingPreset?.kind || values.presetKind
  if (values.presetKind === 'spectrum') {
    values.reflectance = formData.getAll('reflectanceValue').map(String).join(',')
    values.transmittance = formData.getAll('transmittanceValue').map(String).join(',')
    values.classTexture = classTextureFromForm(formData)
    if (values.spectralModel === 'file' && !values.fileName) { toast('请先导入波谱文件', '需要包含波长行和反射率行', 'error'); return }
  }
  const { list, item } = presetFromForm(values)
  const editingIndex = state.editingPreset?.index ?? -1
  if (list.some((entry, index) => index !== editingIndex && entry.name === item.name)) { toast('名称已存在', `请更换内部名称：${item.name}`, 'error'); return }
  const previous = editingIndex >= 0 ? list[editingIndex] : null
  let applied = false
  try {
    submit.disabled = true
    ensureProjectState()
    if (editingIndex >= 0) list[editingIndex] = item
    else list.push(item)
    applied = true
    state.project.configuration = state.config
    markXmlDirty()
    $('#unsavedMark').hidden = false
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('材质属性保存失败')
    hidePresetDialog(); refreshFromState(false); await loadActualScene(state.config); renderInspector()
    addLog(`${editingIndex >= 0 ? '材质属性已修改' : '材质预设已加入工程'}：${item.name}`, 'success')
  } catch (error) {
    if (applied) {
      if (editingIndex >= 0) list[editingIndex] = previous
      else list.pop()
    }
    toast(editingIndex >= 0 ? '修改材质属性失败' : '新增材质属性失败', error.message, 'error')
  } finally { submit.disabled = false }
}

async function saveImportedObject(event) {
  event.preventDefault()
  const pending = state.pendingObject
  if (!pending) return
  const submit = $('#materialForm button[type="submit"]')
  let stage = '读取 OBJ 属性'
  let previousObject = null
  let presetSnapshot = null
  let objectIndex = -1
  let insertedObject = false
  try {
    submit.disabled = true
    ensureProjectState()
    const formData = new FormData($('#materialForm'))
    const values = Object.fromEntries(formData.entries())
    values.reflectance = formData.getAll('reflectanceValue').map(String).join(',')
    values.transmittance = formData.getAll('transmittanceValue').map(String).join(',')
    if (values.spectralModel === 'file' && !values.fileName) throw new Error('请先导入波谱文件')
    stage = '复制 OBJ 到工程'
    const imported = await api.importObj({ name: `${uniqueName(values.objectName, 'object')}.obj`, content: pending.source.content })
    stage = '生成实例分布'
    const distribution = defaultDistribution({ type: values.objectType, dimensions: pending.dimensions })
    const instances = generateDistribution(distribution)
    stage = '保存实例位置文件'
    const savedDistribution = await api.saveDistribution({ path: imported.positionPath, name: values.objectName, instances })
    stage = '校验 Mesh 属性映射'
    const spectrum = { name: values.spectralName, model: values.spectralModel, fileName: values.fileName || '', reflectance: values.reflectance, transmittance: values.transmittance, refTir: Number(values.refTir), refTirWet: optionalTirReflectance(values.refTirWet), tauTir: Number(values.tauTir), openness: Number(values.openness) || 0, previewTexture: previewTextureFromValues(values), physicalTexture: physicalTextureFromValues(values), specular: specularParametersFromValues(values, state.config.spectra.find(entry => entry.name === values.spectralName)?.specular), params: { Cab: values.paramCab, Cw: values.paramCw, Cdm: values.paramCdm, Cs: values.paramCs, N: values.paramN, SMC: values.paramSMC, BSMBrightness: values.paramBSMBrightness, BSMlat: values.paramBSMlat, BSMlon: values.paramBSMlon } }
    const thermal = { name: values.thermalName, sunlitTemperature: Number(values.sunlitTemperature), shadedTemperature: Number(values.shadedTemperature) }
    const spectralSelections = $$('.mesh-spectrum'), thermalSelections = $$('.mesh-thermal')
    const physicalSelections = $$('.mesh-physical'), canopySelections = $$('.mesh-canopy')
    const physicalType = physicalTypeForObject(values.objectType)
    const physical = state.config.materials.find((item) => item.name === values.materialPreset) || physicalFromForm(values, physicalType)
    // FIXED[低|错误处理]: 新建的光谱/温度/物化预设在此直接 upsert 进 state.config，后续校验或保存失败时 catch 只回滚 objects.items，预设库残留半成品条目（同名时还会覆盖已有预设）；已修复：upsert 前对三个预设数组做浅快照（upsert 是替换/追加元素，不原地改元素），catch 中一并恢复
    presetSnapshot = { spectra: [...state.config.spectra], thermals: [...state.config.thermals], materials: [...state.config.materials] }
    if (spectralSelections.some((select) => select.value === '__new__')) upsertByName(state.config.spectra, spectrum)
    if (thermalSelections.some((select) => select.value === '__new__')) upsertByName(state.config.thermals, thermal)
    if (values.materialPreset === '__new__' || physicalSelections.some((select) => select.value === '__new__')) upsertByName(state.config.materials, physical)
    const defaultCanopy = values.objectType === 'Fire' ? 'fire_medium' : values.objectType === 'Fog' ? 'fog_medium' : values.objectType === 'Vegetation' ? 'tree_leaf_canopy' : 'rigid_body'
    const meshes = pending.meshNames.map((name, index) => ({
      name,
      spectralName: spectralSelections[index].value === '__new__' ? spectrum.name : spectralSelections[index].value,
      thermalName: thermalSelections[index].value === '__new__' ? thermal.name : thermalSelections[index].value,
      materialName: physicalSelections[index].value === '__new__' ? physical.name : physicalSelections[index].value,
      canopyName: canopySelections[index]?.value || defaultCanopy,
      specularEnabled: meshSpecularFromRow($('#meshMaterialRows').rows[index])
    }))
    if (!physical?.name || !state.config.materials.some((entry) => entry.name === physical.name)) throw new Error('物化属性未定义')
    for (const mesh of meshes) {
      if (!state.config.spectra.some((entry) => entry.name === mesh.spectralName)) throw new Error('光谱属性不存在：' + mesh.spectralName)
      if (!state.config.thermals.some((entry) => entry.name === mesh.thermalName)) throw new Error('温度属性不存在：' + mesh.thermalName)
      const meshPhysical = state.config.materials.find((entry) => entry.name === mesh.materialName)
      if (!materialMatchesObjectType(meshPhysical, values.objectType)) throw new Error('Mesh 物化属性无效：' + mesh.materialName)
      if (!state.config.canopies.some((entry) => entry.name === mesh.canopyName)) throw new Error('Mesh 结构属性不存在：' + mesh.canopyName)
    }
    const item = { name: values.objectName, ...objectTypeFields(values.objectType), materialName: meshes[0]?.materialName || physical.name, canopyName: meshes[0]?.canopyName || defaultCanopy, fileName: imported.path, positionFile: savedDistribution.path, shape: pending.shape || 'cube', dimensions: pending.dimensions, meshes, distribution, instanceCount: instances.length, ...(isMobileAgentType(values.objectType) ? { movement: defaultMovement(values.objectType) } : {}), ...(pending.generation ? { generation: pending.generation } : {}) }
    const existing = state.config.objects.items.findIndex((entry) => entry.name === item.name)
    objectIndex = existing >= 0 ? existing : state.config.objects.items.length
    if (existing >= 0) {
      previousObject = state.config.objects.items[existing]
      state.config.objects.items[existing] = item
    } else {
      state.config.objects.items.push(item)
      insertedObject = true
    }
    const itemIndex = objectIndex
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    state.project.configuration = state.config
    markXmlDirty()
    stage = '保存 project.json'
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw state.lastSaveError || new Error('工程配置保存失败')

    stage = '刷新三维场景'
    disposeObject(pending.object); state.pendingObject = null
    hideMaterialDialog(false); refreshFromState(false); await loadActualScene(state.config)
    state.panel = 'objects'; state.selectedObjectIndex = itemIndex
    $$('.tree-item').forEach((button) => button.classList.toggle('active', button.dataset.panel === 'objects'))
    renderInspector(); await showObjectDialog(itemIndex)
    addLog(`OBJ 属性已加入工程：${imported.path}`, 'success'); toast('OBJ 属性配置完成', `${item.name} · 请继续设置实例分布`)
  } catch (error) {
    if (objectIndex >= 0) {
      if (insertedObject) state.config.objects.items.splice(objectIndex, 1)
      else if (previousObject) state.config.objects.items[objectIndex] = previousObject
      state.config.objects.count = state.config.objects.items.length
      state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
      if (state.project) state.project.configuration = state.config
    }
    if (presetSnapshot) {
      state.config.spectra = presetSnapshot.spectra
      state.config.thermals = presetSnapshot.thermals
      state.config.materials = presetSnapshot.materials
    }
    const message = `${stage}失败：${error.message}`
    toast('OBJ 导入失败', message, 'error')
    addLog(message, 'error')
  }
  finally { submit.disabled = false }
}


function bindingOptionList(items, selected, fallback) {
  if (!items.length) return '<option value="">无可用属性</option>'
  const selectedName = items.some((item) => item.name === selected) ? selected : (items.some((item) => item.name === fallback) ? fallback : items[0].name)
  return items.map((item) => `<option value="${escapeHtml(item.name)}" ${item.name === selectedName ? 'selected' : ''}>${escapeHtml(item.label || item.name)} · ${escapeHtml(item.name)}</option>`).join('')
}

function renderObjectAttributeSummary() {
  const form = $('#objectAttributeForm')
  const material = state.config?.materials?.find((item) => item.name === form.elements.materialName.value)
  const summary = material ? physicalValues(material).map(([key, value]) => `${key}=${value}`).join('，') : '未找到物化属性定义'
  $('#objectAttributeSummary').textContent = material ? `${material.label || material.name} · ${summary}` : summary
}

function renderObjectAttributeOptions(useCurrent = false, resetBindings = false) {
  const item = state.config?.objects?.items?.[state.editingAttributeIndex]
  if (!item) return
  const form = $('#objectAttributeForm')
  const type = form.elements.objectType.value
  const materials = state.config.materials.filter((entry) => materialMatchesObjectType(entry, type))
  const preferredMaterial = { SolarPanel: 'pv_panel', Vegetation: 'leaf_c3', Fire: 'leaf_c3', Fog: 'leaf_c3', Soil: 'soilset', Building: 'building_surface', Human: 'soil_dry', Vehicle: 'vehicle_material', Ship: 'ship_material', Other: 'soilset', Water: 'water_set' }[type]
  const currentMaterial = resetBindings ? preferredMaterial : useCurrent ? form.elements.materialName.value : item.materialName
  form.elements.materialName.innerHTML = bindingOptionList(materials, currentMaterial, preferredMaterial)
  const preferredCanopy = type === 'Fire' ? 'fire_medium' : type === 'Fog' ? 'fog_medium' : type === 'Vegetation' ? 'canopy_default' : 'rigid_body'
  const currentCanopy = resetBindings ? preferredCanopy : item.canopyName
  form.elements.canopyName.innerHTML = bindingOptionList(state.config.canopies || [], currentCanopy, preferredCanopy)
  const preferredSpectrum = { SolarPanel: 'pv_opaque', Vegetation: 'green_leaf', Fire: 'fire_medium', Fog: 'fog_medium', Soil: 'soil', Building: 'concrete', Human: 'human_surface', Vehicle: 'vehicle_surface', Ship: 'ship_surface', Other: 'concrete', Water: 'water_surface' }[type]
  const preferredThermal = { SolarPanel: 'pv_temperature', Vegetation: 'vegetation_temperature', Fire: 'fire_temperature', Fog: 'fog_temperature', Soil: 'soil_temperature', Building: 'building_temperature', Human: 'human_temperature', Vehicle: 'vehicle_temperature', Ship: 'ship_temperature', Other: 'building_temperature', Water: 'water_temperature' }[type]

  const currentMeshes = useCurrent
    ? $$('#objectAttributeMeshRows tr').map((row) => ({
      name: row.dataset.meshName,
      spectralName: resetBindings ? preferredSpectrum : row.querySelector('.attribute-spectrum').value,
      thermalName: resetBindings ? preferredThermal : row.querySelector('.attribute-thermal').value,
      materialName: resetBindings ? preferredMaterial : row.querySelector('.attribute-physical').value,
      canopyName: resetBindings ? preferredCanopy : row.querySelector('.attribute-canopy').value,
      specularEnabled: meshSpecularFromRow(row)
    }))
    : (item.meshes?.length ? item.meshes : [{ ...item, name: item.name }])
  $('#objectAttributeMeshRows').innerHTML = currentMeshes.map((mesh) => `<tr data-mesh-name="${escapeHtml(mesh.name)}"><td title="${escapeHtml(mesh.name)}">${escapeHtml(mesh.name)}</td><td><select class="select attribute-spectrum">${bindingOptionList(state.config.spectra, mesh.spectralName, preferredSpectrum)}</select></td><td><select class="select attribute-thermal">${bindingOptionList(state.config.thermals, mesh.thermalName, preferredThermal)}</select></td><td><select class="select attribute-physical">${bindingOptionList(materials, mesh.materialName || item.materialName, preferredMaterial)}</select></td><td><select class="select attribute-canopy">${bindingOptionList(state.config.canopies || [], mesh.canopyName || item.canopyName, preferredCanopy)}</select></td><td>${meshSpecularFields(meshSpecular(mesh, state.config.spectra, state.config.control, item.spectralName))}</td></tr>`).join('')
  renderObjectAttributeSummary()
}

function showObjectAttributeDialog(index) {
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  state.editingAttributeIndex = index
  state.selectedObjectIndex = index
  const form = $('#objectAttributeForm')
  form.reset()
  form.elements.objectName.value = item.name
  form.elements.objectType.value = objectClassification(item)
  form.elements.objectPath.value = item.fileName || ''
  form.elements.leafDensitySource.value = item.leafDensitySource === 'geometry' ? 'geometry' : 'parameter'
  renderObjectAttributeOptions(false)
  $$('.object-card').forEach((card) => card.classList.toggle('active', Number(card.dataset.index) === index))
  $('#objectAttributeDialog').hidden = false
}

function hideObjectAttributeDialog() {
  $('#objectAttributeDialog').hidden = true
  state.editingAttributeIndex = -1
}

async function saveObjectAttributes(event) {
  event.preventDefault()
  const index = state.editingAttributeIndex
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  const form = $('#objectAttributeForm')
  const submit = form.querySelector('button[type="submit"]')
  const type = form.elements.objectType.value
  const materialName = form.elements.materialName.value
  const canopyName = form.elements.canopyName.value
  const material = state.config.materials.find((entry) => entry.name === materialName)
  const canopy = state.config.canopies.find((entry) => entry.name === canopyName)
  const meshes = $$('#objectAttributeMeshRows tr').map((row) => ({
    name: row.dataset.meshName,
    spectralName: row.querySelector('.attribute-spectrum').value,
    thermalName: row.querySelector('.attribute-thermal').value,
    materialName: row.querySelector('.attribute-physical').value,
    canopyName: row.querySelector('.attribute-canopy').value,
    specularEnabled: meshSpecularFromRow(row)
  }))
  try {
    submit.disabled = true
    if ((type === 'Fire' || type === 'Fog') && state.mode !== 'eVoxelRT' && state.mode !== 'eVoxelEB') throw new Error('参与介质当前只支持 VoxelRT 或 VoxelEB')
    if (!materialMatchesObjectType(material, type)) throw new Error('所选物化属性与对象类型不匹配')
    if (!canopy) throw new Error('所选结构参数不存在')
    if (type === 'Fire' && canopy.structureType !== 'fire') throw new Error('火焰对象必须绑定火焰结构参数')
    if (type === 'Fog' && canopy.structureType !== 'fog') throw new Error('雾对象必须绑定雾结构参数')
    if (!meshes.length) throw new Error('OBJ 没有可配置的 Mesh')
    for (const mesh of meshes) {
      if (!state.config.spectra.some((entry) => entry.name === mesh.spectralName)) throw new Error('光谱属性不存在：' + mesh.spectralName)
      if (!state.config.thermals.some((entry) => entry.name === mesh.thermalName)) throw new Error('温度属性不存在：' + mesh.thermalName)
      const meshMaterial = state.config.materials.find((entry) => entry.name === mesh.materialName)
      if (!materialMatchesObjectType(meshMaterial, type)) throw new Error(`Mesh“${mesh.name}”物化属性无效`)
      const meshCanopy = state.config.canopies.find((entry) => entry.name === mesh.canopyName)
      if (!meshCanopy) throw new Error(`Mesh“${mesh.name}”结构属性不存在`)
      if (type === 'Fire' && meshCanopy.structureType !== 'fire') throw new Error(`Mesh“${mesh.name}”必须绑定火焰结构属性`)
      if (type === 'Fog' && meshCanopy.structureType !== 'fog') throw new Error(`Mesh“${mesh.name}”必须绑定雾结构属性`)
    }
    const previous = { type: item.type, classification: item.classification, materialName: item.materialName, canopyName: item.canopyName, leafDensitySource: item.leafDensitySource, meshes: item.meshes, medium: item.medium, movement: item.movement }
    Object.assign(item, objectTypeFields(type), { materialName, canopyName, meshes })
    if (form.elements.leafDensitySource.value === 'geometry') item.leafDensitySource = 'geometry'
    else delete item.leafDensitySource
    if (type === 'Fire' || type === 'Fog') item.medium = { kind: type === 'Fog' ? 'fog' : 'fire', radiationOnly: true, coordinateOrder: 'XYZ' }
    else delete item.medium
    if (isMobileAgentType(type)) item.movement = { ...defaultMovement(type), ...(item.movement || {}), enabled: Boolean(item.movement?.enabled), mode: 'random' }
    else delete item.movement
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) { Object.assign(item, previous); throw new Error('工程配置保存失败') }
    hideObjectAttributeDialog()
    await loadActualScene(state.config)
    refreshFromState(false); renderInspector()
    addLog(`OBJ 属性已更新：${item.name}`, 'success')
    toast('OBJ 属性已更新', item.name)
  } catch (error) { toast('OBJ 属性保存失败', error.message, 'error') }
  finally { submit.disabled = false }
}

function syncDistributionPopulation(form, source = 'count') {
  const mode = form.elements.distributionMode.value
  if (mode !== 'random' && mode !== 'grid') return
  const minX = finiteNumber(form.elements.distributionMinX.value, 0)
  const maxX = finiteNumber(form.elements.distributionMaxX.value, 1)
  const minY = finiteNumber(form.elements.distributionMinY.value, 0)
  const maxY = finiteNumber(form.elements.distributionMaxY.value, 1)
  const area = distributionArea({ minX, maxX, minY, maxY })
  const population = source === 'density'
    ? populationFromValues(form.elements.distributionCount.value, form.elements.distributionDensity.value, area, 'density')
    : populationFromValues(form.elements.distributionCount.value, form.elements.distributionDensity.value, area, 'count')
  form.elements.distributionBasis.value = source === 'density' ? 'density' : 'count'
  form.elements.distributionCount.value = population.count
  form.elements.distributionDensity.value = distributionDensityInputValue(population.density)
}

function updateDistributionFormState(form) {
  const mode = form.elements.distributionMode.value
  const basis = form.elements.distributionBasis.value
  const usesPopulation = mode === 'random' || mode === 'grid'
  form.elements.distributionBasis.disabled = !usesPopulation
  form.elements.distributionCount.disabled = !usesPopulation
  form.elements.distributionDensity.disabled = !usesPopulation
  form.elements.distributionCount.readOnly = usesPopulation && basis === 'density'
  form.elements.distributionDensity.readOnly = usesPopulation && basis === 'count'
  form.elements.distributionPoints.closest('.distribution-points').hidden = mode !== 'manual'

  let detail
  if (mode === 'manual') {
    const count = String(form.elements.distributionPoints.value || '').split(/\r?\n/).map((line) => line.trim()).filter(Boolean).length
    detail = `自定义坐标 · ${count} 个实例`
  } else if (mode === 'single') detail = '单实例 · 位于当前分布范围中心'
  else {
    const distribution = distributionFromForm(form)
    distribution.basis = basis
    distribution.count = finiteNumber(form.elements.distributionCount.value, distribution.count)
    distribution.density = finiteNumber(form.elements.distributionDensity.value, distribution.density)
    distribution.densityUnit = 'hectare'
    const count = distributionTargetCount(distribution)
    const areaHectares = distributionArea(distribution) / SQUARE_METERS_PER_HECTARE
    detail = `${mode === 'grid' ? '规则网格' : '纯随机'} · ${areaHectares.toFixed(2)} 公顷 × ${distribution.density} 个/公顷 · 将生成 ${count} 个实例`
  }
  $('#objectDistributionStatus').textContent = `${detail}；保存后覆盖该 OBJ 自己的位置文件。`
}

async function showObjectDialog(index) {
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  state.editingObjectIndex = index
  const form = $('#objectForm')
  form.reset()
  form.elements.objectName.value = item.name
  form.elements.objectType.value = objectClassification(item)
  form.elements.objectPath.value = item.fileName || ''
  let distribution = item.distribution
  if (!distribution && item.positionFile) {
    try {
      const result = await api.readText(item.positionFile)
      distribution = { ...defaultDistribution(item), mode: 'manual', points: result.content.trim() }
    } catch (error) { toast('位置文件读取失败', error.message, 'error') }
  }
  state.selectedObjectIndex = index
  setDistributionForm(form, distribution || defaultDistribution(item))
  setMovementForm(form, item.movement, item.type)
  updateDistributionFormState(form)
  updateMovementFormState(form)
  renderInspector()
  $('#objectDialog').hidden = false
}

async function importDistributionTxt(file) {
  if (!file) return
  try {
    if (file.size > 5 * 1024 * 1024) throw new Error('TXT 文件不能超过 5 MB')
    const form = $('#objectForm')
    const defaultZ = finiteNumber(form.elements.distributionZ.value, 0)
    const points = parseDistributionPointsText(await file.text(), defaultZ)
    form.elements.distributionMode.value = 'manual'
    form.elements.distributionPoints.value = distributionPointsText(points)
    updateDistributionFormState(form)
    $('#objectDistributionStatus').textContent = `已导入 ${file.name} · ${points.length} 个实例；保存后写入 positions 文件。`
    toast('自定义分布已导入', `${file.name} · ${points.length} 个实例`)
  } catch (error) {
    toast('TXT 导入失败', error.message, 'error')
  } finally {
    $('#distributionTxtFile').value = ''
  }
}

function hideObjectDialog() {
  $('#objectDialog').hidden = true
  state.editingObjectIndex = -1
}

async function saveSceneObject(event) {
  event.preventDefault()
  const index = state.editingObjectIndex
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  const itemSnapshot = { ...item }
  const submit = $('#objectForm button[type="submit"]')
  try {
    submit.disabled = true
    const form = $('#objectForm')
    const nextName = String(form.elements.objectName.value).trim()
    if (!nextName) throw new Error('对象名称不能为空')
    if (state.config.objects.items.some((entry, itemIndex) => itemIndex !== index && entry.name === nextName)) throw new Error(`对象名称已存在：${nextName}`)
    const distribution = distributionFromForm(form, { requireValidDensity: true })
    const instances = generateDistribution(distribution)
    const saved = await api.saveDistribution({ path: item.positionFile, name: nextName, instances })
    // FIXED[低|错误处理]: 先原地修改 item（name/distribution/movement/instanceCount）再 saveXml，保存失败时 catch 不回滚，界面与磁盘上的 project.json 不一致；另外传给 saveDistribution 的是未 trim 的名称，与 item.name=nextName 不一致；已修复：进函数时留存 item 快照，catch 中删除新增键并还原字段；saveDistribution 统一用 nextName
    item.name = nextName
    item.positionFile = saved.path
    item.distribution = distribution
    const movement = movementFromForm(form)
    if (movement) item.movement = movement
    else delete item.movement
    item.instanceCount = instances.length
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    hideObjectDialog()
    await loadActualScene(state.config)
    refreshFromState(false); renderInspector()
    addLog(`对象分布已更新：${item.name} · ${instances.length} 个实例`, 'success')
    toast('对象已更新', `${item.name} · ${instances.length} 个实例`)
  } catch (error) {
    for (const key of Object.keys(item)) if (!(key in itemSnapshot)) delete item[key]
    Object.assign(item, itemSnapshot)
    // 回滚 item 后需要同步重算对象名列表，否则 objects.names 仍保留失败前的 nextName，与 item.name 不一致
    if (state.config?.objects?.items) {
      state.config.objects.count = state.config.objects.items.length
      state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    }
    toast('对象修改失败', error.message, 'error')
  }
  finally { submit.disabled = false }
}

async function addTarpCover(index) {
  const target = state.config?.objects?.items?.[index]
  if (!target?.fileName) { toast('无法生成篷布罩', '该对象没有 OBJ 网格', 'error'); return }
  const c = state.config
  const presets = createMaterialPresets()
  const snapshot = structuredClone({ spectra: c.spectra, thermals: c.thermals, materials: c.materials, canopies: c.canopies })
  for (const [kind, presetName] of [['spectra', 'tarp_olive_canvas'], ['thermals', 'tarp_temperature'], ['materials', 'tarp_fabric'], ['canopies', 'rigid_body']]) {
    if (!c[kind].some((entry) => entry.name === presetName)) c[kind].push(structuredClone(presets[kind].find((entry) => entry.name === presetName)))
  }
  const names = new Set(c.objects.items.map((entry) => entry.name))
  const base = `${uniqueName(target.name, 'object')}_tarp`
  let name = base
  for (let suffix = 2; names.has(name); suffix += 1) name = `${base}_${suffix}`
  // FacetEB solves membranes; VoxelEB rejects them, so keep the target's solid material there.
  const materialName = state.mode === 'eVoxelEB' ? (target.materialName || 'building_surface') : 'tarp_fabric'
  try {
    const cover = await api.generateTarpCover({ fileName: target.fileName, name })
    const mesh = { name: 'tarp_cover', spectralName: 'tarp_olive_canvas', thermalName: 'tarp_temperature', materialName, canopyName: 'rigid_body' }
    c.objects.items.splice(index + 1, 0, {
      name, label: `${target.label || target.name} 篷布罩`, type: ['Vehicle', 'Human', 'Ship', 'Building'].includes(target.type) ? target.type : 'Building',
      ...(target.movement ? { movement: structuredClone(target.movement) } : {}),
      fileName: cover.relativePath, positionFile: target.positionFile, shape: 'cube', dimensions: cover.dimensions,
      spectralName: mesh.spectralName, thermalName: mesh.thermalName, materialName, canopyName: mesh.canopyName, meshes: [mesh],
      distribution: structuredClone(target.distribution || {}), instanceCount: target.instanceCount,
      groundFootprint: target.groundFootprint || cover.targetFootprint,
      tarpCoverOf: target.name
    })
    c.objects.count = c.objects.items.length
    c.objects.names = c.objects.items.map((entry) => entry.name)
    ensureProjectState().configuration = c
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    await loadActualScene(c)
    refreshFromState(false); renderInspector()
    addLog(`已为 ${target.name} 生成篷布罩 ${name}：${cover.triangles} 个三角面，网格 ${cover.cell.toFixed(3)} m`, 'success')
    if (state.mode === 'eVoxelEB') toast('篷布罩已生成', 'VoxelEB 不支持薄膜热平衡，已沿用目标物化属性；如需篷布热平衡请切换到 FacetEB 并改绑 tarp_fabric')
    else toast('篷布罩已生成', name)
  } catch (error) {
    const at = c.objects.items.findIndex((entry) => entry.name === name)
    if (at >= 0) c.objects.items.splice(at, 1)
    c.objects.count = c.objects.items.length
    c.objects.names = c.objects.items.map((entry) => entry.name)
    Object.assign(c, snapshot)
    toast('生成篷布罩失败', error.message, 'error')
  }
}

async function deleteSceneObject(index) {
  const item = state.config?.objects?.items?.[index]
  if (!item) return
  const previousSelectedIndex = state.selectedObjectIndex
  state.config.objects.items.splice(index, 1)
  if (state.selectedObjectIndex === index) state.selectedObjectIndex = -1
  else if (state.selectedObjectIndex > index) state.selectedObjectIndex -= 1
  state.config.objects.count = state.config.objects.items.length
  state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
  try {
    ensureProjectState().configuration = state.config
    markXmlDirty()
    const projectSaved = await saveXml(true)
    if (projectSaved === null) return
    if (!projectSaved) throw new Error('工程配置保存失败')
    await loadActualScene(state.config)
    refreshFromState(false); renderInspector()
    addLog(`已从场景删除对象：${item.name}`, 'success')
    toast('对象已删除', item.name)
  } catch (error) {
    state.config.objects.items.splice(index, 0, item)
    state.selectedObjectIndex = previousSelectedIndex
    state.config.objects.count = state.config.objects.items.length
    state.config.objects.names = state.config.objects.items.map((entry) => entry.name)
    toast('删除对象失败', error.message, 'error')
  }
}

async function runSimulation() {
  if (!state.inputPath) { toast("无法运行", "请先打开场景工程的 project.json", "error"); return }
  if (state.running || state.runPending) return
  const specularError = meshSpecularRunError(state.mode, state.config)
  if (specularError) { toast('无法运行', specularError, 'error'); return }
  state.runPending = true
  try {
    if (state.xmlDirty && !(await saveXml())) return
    await startSimulation()
  } finally { state.runPending = false }
}

async function startSimulation() {
  try {
    state.startedAt = Date.now(); state.progress = 1; state.progressStage = '正在启动计算引擎'; state.outputDir = state.config?.outDir || state.outputDir; state.runId = ''
    setRunning(true)
    const response = await api.run({ mode: state.mode, executable: state.executable, inputPath: state.inputPath })
    if (response.outputDir) state.outputDir = response.outputDir
    state.runId = response.runId || ''
    if (state.running) state.pid = response.pid
  } catch (error) {
    addLog(error.message, 'error')
    if (isEngineAllowlistError(error)) { addLog(ENGINE_REJECTED_HINT, 'warning'); toast('引擎路径未获允许', ENGINE_REJECTED_HINT, 'error') }
    else toast('无法启动模拟', error.message, 'error')
    state.progress = 0; state.progressStage = '启动失败'; setRunning(false); renderRunProgress(Date.now() - state.startedAt)
  }
}

async function resetSimulation() {
  const button = $('#resetBtn')
  try {
    button.disabled = true
    const result = await api.reset()
    state.pid = null
    state.startedAt = 0
    state.progress = 0
    state.progressStage = '已重置'
    setRunning(false)
    renderRunProgress(0)
    const detail = result.terminated?.length
      ? `已终止：${result.terminated.join('、')}`
      : '未发现残留的模拟进程'
    addLog(`模拟环境已重置；${detail}`, 'success')
    toast('重置完成', '现在可以重新运行模拟')
  } catch (error) {
    addLog(`重置失败：${error.message}`, 'error')
    toast('重置失败', error.message, 'error')
  } finally { button.disabled = false }
}

const simulationModeLabels = {
  eFacetRT: '面元辐射传输', eFacetEB: '面元能量平衡',
  eVoxelRT: '体元辐射传输', eVoxelEB: '体元辐射传输与能量平衡',
  eRaytracing: '简单光线追踪'
}

function simulationMethodSummary() {
  if (!['eFacetEB', 'eVoxelEB'].includes(state.mode)) return ''
  const soil = t(['瞬时能量平衡', '热惯性动态模型', '温度廓线传导模型'][state.config?.control?.soilTemperatureMethod ?? 2])
  const vegetation = (state.config?.control?.vegetationTemperatureMethod ?? 0) === 0
    ? 'Ball–Berry 经验法' : 'Farquhar 机制法'
  return t(`温度方法：土壤=${soil}；植被=${vegetation}`)
}

function normalizedEngineLog(text) {
  const translations = [
    [/Preparing CPU raster buffers/i, '准备面元计算缓冲区'],
    [/Creating Vulkan device and GPU buffers/i, '初始化 GPU 与计算缓冲区'],
    [/(GPU )?visibility graph/i, '构建面元可见性图'],
    [/(CPU |GPU )?sunlight (visibility )?raster/i, '计算太阳光照比例'],
    [/(CPU |GPU )?(Jacobi )?radiosity solve/i, '计算辐射传输'],
    [/diffuse baseline and direct-light enhancement/i, '计算漫射基线与直射增强'],
    [/(Vulkan GPU|CPU) complete/i, '面元辐射传输计算完成']
  ]
  return String(text || '').replace(/\r/g, '').split('\n').filter(Boolean).flatMap((source) => {
    const line = source.trim()
    if (!line || /^(cube|OBJ placement)/i.test(line)) return []
    const progress = line.match(/^PROGRESS\s+([0-9]+)\s+(.+)$/i)
    if (progress) {
      let stage = progress[2]
      for (const [pattern, replacement] of translations) if (pattern.test(stage)) { stage = replacement; break }
      setSimulationProgress(progress[1], stage)
      return [`进度 ${progress[1]}% · ${stage}`]
    }
    const processResult = line.match(/^PROCESS\s+(.+)$/i)
    if (processResult) return [`过程结果 · ${processResult[1]}`]
    const primaryResult = line.match(/^RESULT\s+(.+)$/i)
    if (primaryResult) return [`主结果 · ${primaryResult[1]}`]
    const observation = line.match(/^(?:OBSERVATION|Angle Info:)\s*(.*)$/i)
    if (observation) return [`观测角度 · ${observation[1].replaceAll('_', ' ').trim()}`]
    const time = line.match(/^Time Info:\s*t_?(.*)$/i)
    if (time) return time[1] ? [`时间节点 · ${time[1]}`] : []
    return [line]
  }).join('\n')
}

function handleSimulationEvent(event) {
  if (event.type === 'status') {
    if (event.running && !state.running) {
      if (event.outputDir) state.outputDir = event.outputDir
      state.runId = event.runId || ''
      state.startedAt = event.startedAt || Date.now()
      state.progressStage = '正在运行（已重新连接）'
      state.pid = event.pid; setRunning(true)
      addLog(`已连接到正在运行的模拟 · PID ${event.pid}`, 'success')
    } else if (!event.running && state.running && state.runId) {
      // A run that had started finished while the event stream was down.
      state.progressStage = '模拟已结束（连接中断期间）'
      addLog('事件流重新连接时模拟已结束，请刷新结果查看最终状态', 'error')
      setRunning(false); renderRunProgress(Date.now() - state.startedAt)
    }
    return
  }
  if (event.type === 'started') {
    if (event.outputDir) state.outputDir = event.outputDir
    state.runId = event.runId || state.runId || ''
    if (!state.startedAt) state.startedAt = Date.now()
    state.progress = 1
    state.progressStage = '正在启动计算引擎'
    state.pid = event.pid; setRunning(true)
    addLog(t(`开始模拟 · ${t(simulationModeLabels[state.mode] || state.mode)} · PID ${event.pid}`), 'success')
    const methods = simulationMethodSummary()
    if (methods) addLog(methods)
    addLog(`输出：图像=${state.config?.sensor?.image ? '是' : '否'}；辐射过程=${state.config?.sensor?.radiationProcess ? '是' : '否'}；能量过程=${state.config?.sensor?.energyProcess ? '是' : '否'}`)
  }
  else if (event.type === 'stdout') {
    const message = normalizedEngineLog(event.text)
    if (message) addLog(message)
  }
  else if (event.type === 'stderr' || event.type === 'error') addLog(event.text, 'error')
  else if (event.type === 'closed') {
    if (event.outputDir) state.outputDir = event.outputDir
    const success = event.code === 0
    state.progress = success ? 100 : state.progress
    state.progressStage = success ? '模拟完成' : event.status === 'non_converged' ? '模拟未收敛，结果已保留' : '模拟异常结束'
    addLog(success ? `模拟完成，耗时 ${(event.elapsed / 1000).toFixed(2)} 秒` : `模拟已结束，退出码 ${event.code}${event.signal ? `，信号 ${event.signal}` : ''}`, success ? 'success' : 'error')
    // DEFERRED[低|可维护性]: 下一行 if (!success) 只作用于 toast，setRunning(false)/renderRunProgress 实际无条件执行，但写在同一行极易被误读或在后续修改时误改为条件执行；建议：拆成多行并为 if 加花括号 —— 行为本身正确，只是换行/加花括号的格式调整，本轮明确不做格式化
    if (!success) toast('模拟结束', event.status === 'non_converged' ? '部分节点未达到收敛容差，结果已保留' : '退出码：' + event.code, 'error'); setRunning(false); renderRunProgress(event.elapsed)
  }
}

function formatBytes(value) {
  if (value < 1024) return `${value} B`
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`
  return `${(value / 1024 / 1024).toFixed(1)} MB`
}


function emptyResultPreview(title, message) {
  $('#resultPreview').innerHTML = `<div class="empty-editor"><svg><use href="#i-layers"/></svg><h3>${escapeHtml(title)}</h3><p>${escapeHtml(message)}</p></div>`
}

function decodeResultPixels(result) {
  const source = atob(result.pixels || '')
  return Uint8Array.from(source, (character) => character.charCodeAt(0))
}

function grayscaleResultPixels(bytes, width, height) {
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d')
  const image = context.createImageData(width, height)
  for (let index = 0; index < bytes.length; index += 1) {
    const value = bytes[index]
    const offset = index * 4
    image.data[offset] = value; image.data[offset + 1] = value; image.data[offset + 2] = value; image.data[offset + 3] = 255
  }
  context.putImageData(image, 0, 0)
  return canvas
}

function thermalHotspotPixels(bytes, width, height) {
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d')
  const image = context.createImageData(width, height)
  for (let index = 0; index < bytes.length; index += 1) {
    const strength = bytes[index] / 255
    if (strength <= 0) continue
    const offset = index * 4
    image.data[offset] = 255
    image.data[offset + 1] = Math.round(214 * (1 - strength))
    image.data[offset + 2] = Math.round(40 * (1 - strength))
    image.data[offset + 3] = Math.round(160 + strength * 95)
  }
  context.putImageData(image, 0, 0)
  return canvas
}

function resultRasterFromEnvi(result) {
  const width = result.previewWidth, height = result.previewHeight
  const bytes = decodeResultPixels(result)
  const textureCanvas = grayscaleResultPixels(bytes, width, height)
  if (result.hotspotPixels) {
    const hotspotBytes = Uint8Array.from(atob(result.hotspotPixels), (character) => character.charCodeAt(0))
    textureCanvas.getContext('2d').drawImage(thermalHotspotPixels(hotspotBytes, width, height), 0, 0)
  }
  return { width, height, bytes, textureCanvas }
}

function resultViewActions(allow3D = true) {
  if (!allow3D) return ''
  return '<div class="result-view-actions"><button class="button ghost result-2d-btn" id="result2dBtn" type="button">二维图像</button><button class="button ghost result-3d-btn" id="result3dBtn" type="button">三维分析</button></div>'
}

function formatColorbarValue(value) {
  if (!Number.isFinite(Number(value))) return '—'
  const numeric = Number(value)
  if (numeric === 0) return '0'
  const absolute = Math.abs(numeric)
  return absolute >= 10000 || absolute < .001 ? numeric.toExponential(3) : numeric.toPrecision(5)
}

function resultColorbar(label, minimum, maximum, palette = 'spectral') {
  const text = String(label || '数值')
  const match = text.match(/^(.*?)\s*\[([^\]]+)\]\s*$/)
  const title = match ? `${match[1]} · ${match[2]}` : text
  const paletteClass = ['gray', 'hotspot', 'red', 'green', 'blue', 'diverging'].includes(palette) ? palette : ''
  return `<div class="result-colorbar ${paletteClass}" role="img" aria-label="${escapeHtml(title)} 色标"><div class="result-colorbar-title" title="${escapeHtml(title)}">${escapeHtml(title)}</div><div class="result-colorbar-scale"><div class="result-colorbar-gradient"></div><div class="result-colorbar-labels"><span>${escapeHtml(formatColorbarValue(minimum))}</span><span>${escapeHtml(formatColorbarValue(maximum))}</span></div></div></div>`
}

function resultBandWavelength(result, index) {
  const name = String(result.bandNames?.[index] || '')
  const labelled = Number(name.match(/@\s*([0-9.]+)\s*nm/i)?.[1])
  if (Number.isFinite(labelled)) return labelled
  const configured = sensorBandValues(state.config?.sensor || {})
  return configured.length === result.bands && Number.isFinite(configured[index]) ? configured[index] : null
}

function resultBandLabel(result, index) {
  return result.bandNames?.[index] || `波段 ${index + 1}`
}

function resultOpticalBands(result) {
  if (!result) return []
  const all = Array.from({ length: result.bands }, (_, index) => ({ index, wavelength: resultBandWavelength(result, index) }))
  const knownOptical = all.filter((item) => Number.isFinite(item.wavelength) && item.wavelength <= 2500)
  return knownOptical.length ? knownOptical : all.slice(0, Math.min(5, all.length))
}

function defaultColorBands(result) {
  if (!result || result.bands < 3) return null
  const optical = resultOpticalBands(result)
  if (optical.length < 3) return null
  if (!optical.every((item) => Number.isFinite(item.wavelength))) return [optical[2].index, optical[1].index, optical[0].index]
  const closest = (target) => optical.reduce((best, item) => Math.abs(item.wavelength - target) < Math.abs(best.wavelength - target) ? item : best)
  return [closest(668).index, closest(560).index, closest(475).index]
}

function normalizedResultDisplay(result, file) {
  const defaults = defaultColorBands(result)
  const opticalIndices = new Set(resultOpticalBands(result).map((item) => item.index))
  const previous = state.resultDisplay[file.path] || {}
  const validBand = (value, fallback) => Number.isInteger(Number(value)) && Number(value) >= 0 && Number(value) < result.bands ? Number(value) : fallback
  const channels = [0, 1, 2].map((channel) => {
    const selected = validBand(previous.channels?.[channel], defaults?.[channel] ?? 0)
    return opticalIndices.has(selected) ? selected : defaults?.[channel] ?? 0
  })
  const gray = validBand(previous.gray, defaults?.[0] ?? result.band ?? 0)
  const settings = { mode: defaults && previous.mode !== 'gray' ? 'color' : 'gray', channels, gray }
  state.resultDisplay[file.path] = settings
  return settings
}

function resultBandOptions(result, selected, indices = Array.from({ length: result.bands }, (_, index) => index)) {
  return indices.map((index) => `<option value="${index}" ${index === selected ? 'selected' : ''}>${escapeHtml(resultBandLabel(result, index))}</option>`).join('')
}

function resultDisplayControls(result, settings) {
  const colorAvailable = Boolean(defaultColorBands(result))
  const opticalIndices = resultOpticalBands(result).map((item) => item.index)
  const colorSelectors = ['红色 R', '绿色 G', '蓝色 B'].map((label, channel) => `<label><span>${label}</span><select class="input result-channel" aria-label="${label}" data-channel="${channel}">${resultBandOptions(result, settings.channels[channel], opticalIndices)}</select></label>`).join('')
  const selectors = settings.mode === 'color'
    ? colorSelectors
    : `<label><span>灰度波段</span><select class="input" id="resultGrayBand" aria-label="灰度波段">${resultBandOptions(result, settings.gray)}</select></label>`
  return `<div class="result-display-controls"><div class="result-display-modes"><button class="button ghost ${settings.mode === 'color' ? 'active' : ''}" id="resultColorMode" type="button" ${colorAvailable ? '' : 'disabled'}>彩色图像</button><button class="button ghost ${settings.mode === 'gray' ? 'active' : ''}" id="resultGrayMode" type="button">灰度图像</button></div><div class="result-channel-controls">${selectors}</div>${settings.mode === 'color' ? '<small>默认真彩色：红 668 nm、绿 560 nm、蓝 475 nm；也可选择红边或近红外组成假彩色。</small>' : '<small>选择一个波段，以 P2–P98 灰度拉伸显示。</small>'}</div>`
}

function colorCompositeRaster(rasters) {
  const width = rasters[0].width, height = rasters[0].height
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d')
  const image = context.createImageData(width, height)
  for (let index = 0; index < width * height; index += 1) {
    const offset = index * 4
    image.data[offset] = rasters[0].bytes[index]
    image.data[offset + 1] = rasters[1].bytes[index]
    image.data[offset + 2] = rasters[2].bytes[index]
    image.data[offset + 3] = 255
  }
  context.putImageData(image, 0, 0)
  return { width, height, bytes: rasters[0].bytes, textureCanvas: canvas }
}

function simulationTimeParts(value) {
  const text = String(value ?? '').trim()
  if (!text) return null
  const julianTime = Number(text)
  if (Number.isFinite(julianTime)) {
    let day = Math.floor(julianTime)
    let minuteOfDay = Math.round((julianTime - day) * 1440)
    if (minuteOfDay >= 1440) { day += 1; minuteOfDay -= 1440 }
    if (minuteOfDay < 0) { day -= 1; minuteOfDay += 1440 }
    return { day, hour: Math.floor(minuteOfDay / 60), minute: minuteOfDay % 60 }
  }
  const match = text.match(/DOY\s*(\d+).*?(\d{1,2})[-_:](\d{1,2})/i)
  return match ? { day: Number(match[1]), hour: Number(match[2]), minute: Number(match[3]) } : null
}

function displaySimulationTime(value) {
  const parts = simulationTimeParts(value)
  if (parts) return `DOY${parts.day} ${String(parts.hour).padStart(2, '0')}:${String(parts.minute).padStart(2, '0')}`
  return String(value || '').replaceAll('_to_', ' → ').replaceAll('_', ' ')
}

function bindResultViewActions(result, file, raster, image = false) {
  $('#result3dBtn')?.addEventListener('click', () => showResult3D(raster, file, result))
  $('#result2dBtn')?.addEventListener('click', () => image ? drawImagePreview(result, file) : result.kind === 'tiff' ? drawTiffPreview(result, file) : drawEnviPreview(result, file))
}

function fluidSliceDisplaySize(width, height) {
  const previewMaximum = Math.max(Number(width) || 0, Number(height) || 0, 1)
  const scale = Math.min(720 / previewMaximum, Math.max(1, 480 / previewMaximum))
  return {
    width: Math.max(1, Math.round(width * scale)),
    height: Math.max(1, Math.round(height * scale))
  }
}

async function drawEnviPreview(result, file, format = 'ENVI') {
  const previewRequest = ++state.resultPreviewRequest
  disposeResultViewer()
  const settings = normalizedResultDisplay(result, file)
  const selectedBands = settings.mode === 'color' ? settings.channels : [settings.gray]
  // FIXED[中|错误处理]: 切换波段/彩色模式时由事件监听直接调用本函数且不 await、不 catch；任一 api.readResult 失败都会成为未处理 rejection，此时 3D 视图已被 dispose，预览区停留在旧画面而无错误提示；已修复：Promise.all 包 try/catch，失败时（且 previewRequest 仍为最新）先 disposeResultViewer，再用 emptyResultPreview + toast + addLog 提示
  let bandResults
  try {
    bandResults = await Promise.all(selectedBands.map((band) => band === result.band ? result : api.readResult(file.path, band)))
  } catch (error) {
    if (previewRequest === state.resultPreviewRequest) {
      disposeResultViewer()
      emptyResultPreview('结果读取失败', error.message)
      toast('结果读取失败', error.message, 'error')
      addLog(`结果读取失败：${error.message}`, 'error')
    }
    return
  }
  if (previewRequest !== state.resultPreviewRequest || state.selectedResult?.path !== file.path) return
  const rasters = bandResults.map(resultRasterFromEnvi)
  const raster = settings.mode === 'color' ? colorCompositeRaster(rasters) : rasters[0]
  state.resultRaster = raster
  const allow3D = format !== 'TIFF' && settings.mode === 'gray'
  const observationMeta = format === 'TIFF' && file.observationLabel ? '<span>' + escapeHtml(file.observationLabel.replaceAll('_to_', ' → ').replaceAll('_', ' ')) + '</span>' : ''
  const activeResult = bandResults[0]
  const layerName = resultBandLabel(result, settings.gray)
  const displayMinimum = Number.isFinite(Number(activeResult.stretchMinimum)) ? Number(activeResult.stretchMinimum) : Number(activeResult.minimum)
  const displayMaximum = Number.isFinite(Number(activeResult.stretchMaximum)) ? Number(activeResult.stretchMaximum) : Number(activeResult.maximum)
  const hotspotCount = settings.mode === 'gray' ? Number(activeResult.hotspotCount) || 0 : 0
  const hotspotLegend = hotspotCount > 0
    ? resultColorbar(`高温异常 · ${hotspotCount.toLocaleString('zh-CN')} 像元`, Number(activeResult.hotspotThreshold), Number(activeResult.maximum), 'hotspot') : ''
  const rawRange = settings.mode === 'gray' && activeResult.thermalBand
    ? `<span>原始范围 ${formatColorbarValue(activeResult.minimum)} ～ ${formatColorbarValue(activeResult.maximum)}</span>` : ''
  const grayStretchLabel = activeResult.stretchMode === 'thermal-surface'
    ? '地表细节 P2–P98（天空与无效零值不参与拉伸）'
    : 'P2–P98'
  const colorbars = settings.mode === 'color'
    ? bandResults.map((bandResult, channel) => resultColorbar(`${['R', 'G', 'B'][channel]} · ${resultBandLabel(result, settings.channels[channel])} · P2–P98`, bandResult.stretchMinimum, bandResult.stretchMaximum, ['red', 'green', 'blue'][channel])).join('')
    : resultColorbar(`${layerName} · ${grayStretchLabel}`, displayMinimum, displayMaximum, 'gray')
  const displaySummary = settings.mode === 'color'
    ? `彩色合成：R ${escapeHtml(resultBandLabel(result, settings.channels[0]))} · G ${escapeHtml(resultBandLabel(result, settings.channels[1]))} · B ${escapeHtml(resultBandLabel(result, settings.channels[2]))}`
    : `灰度波段：${escapeHtml(layerName)}`
  const isAirTemperatureSlice = format === 'TIFF' && /^fluid_air_temperature_/i.test(String(file.name || ''))
  const displaySize = isAirTemperatureSlice
    ? fluidSliceDisplaySize(result.previewWidth, result.previewHeight)
    : { width: result.previewWidth, height: result.previewHeight }
  const fluidSliceClass = isAirTemperatureSlice ? ' fluid-slice-result-card' : ''
  const fluidCanvasClass = isAirTemperatureSlice ? ' fluid-slice-canvas' : ''
  const perspectiveForwardView = state.project?.configuration?.sensor?.projection === 'perspective'
    && Math.abs(Number(state.project.configuration.sensor.vza) || 0) > 0.001
    && !isAirTemperatureSlice
  const orientationIndicator = perspectiveForwardView
    ? '<div class="result-north-indicator" aria-label="影像上方朝向天空"><strong>UP</strong><span>↑</span></div>'
    : '<div class="result-north-indicator" aria-label="影像上方为正北"><strong>N</strong><span>↑</span></div>'
  const orientationLabel = perspectiveForwardView ? '天空向上 · 地平线水平' : '上北 · 右东'
  $('#resultPreview').innerHTML = `<div class="result-preview-card${fluidSliceClass}"><div class="result-image-stage"><canvas class="result-canvas${fluidCanvasClass}" id="resultCanvas" width="${displaySize.width}" height="${displaySize.height}"></canvas>${orientationIndicator}</div>${resultDisplayControls(result, settings)}${colorbars}${hotspotLegend}${resultViewActions(allow3D)}<div class="result-meta"><span>${result.width} × ${result.height}</span><span>${format} · ${result.bands} 个独立波段</span><span>${orientationLabel}</span><span>${displaySummary}</span>${observationMeta}<span>${settings.mode === 'color' ? '各通道显示 P2–P98' : `灰度显示 ${grayStretchLabel}`}</span>${settings.mode === 'gray' ? `<span>MIN ${displayMinimum.toPrecision(6)}</span><span>MAX ${displayMaximum.toPrecision(6)}</span>` : ''}${rawRange}</div></div>`
  const canvas = $('#resultCanvas')
  const context = canvas.getContext('2d')
  context.imageSmoothingEnabled = isAirTemperatureSlice
  context.drawImage(raster.textureCanvas, 0, 0, displaySize.width, displaySize.height)
  $('#resultColorMode')?.addEventListener('click', () => { settings.mode = 'color'; drawEnviPreview(result, file, format) })
  $('#resultGrayMode')?.addEventListener('click', () => { settings.mode = 'gray'; drawEnviPreview(result, file, format) })
  $$('.result-channel').forEach((select) => select.addEventListener('change', () => {
    settings.channels[Number(select.dataset.channel)] = Number(select.value)
    drawEnviPreview(result, file, format)
  }))
  $('#resultGrayBand')?.addEventListener('change', (event) => {
    settings.gray = Number(event.target.value)
    drawEnviPreview(result, file, format)
  })
  bindResultViewActions(activeResult, file, raster)
}

async function drawTiffPreview(result, file) {
  await drawEnviPreview(result, file, 'TIFF')
}

function windArrowColor(value, minimum, maximum) {
  const range = maximum - minimum
  const normalized = range > 0 ? Math.max(0, Math.min(1, (value - minimum) / range)) : 0.5
  return `hsl(${(1 - normalized) * 225},82%,42%)`
}

function drawWindPreview(result, file) {
  disposeResultViewer()
  const raster = resultRasterFromEnvi(result)
  state.resultRaster = raster
  const displaySize = fluidSliceDisplaySize(result.previewWidth, result.previewHeight)
  const canvasWidth = displaySize.width
  const canvasHeight = displaySize.height
  const observationMeta = file.observationLabel
    ? `<span>${escapeHtml(file.observationLabel.replaceAll('_to_', ' → ').replaceAll('_', ' '))}</span>` : ''
  $('#resultPreview').innerHTML = `<div class="result-preview-card fluid-slice-result-card"><div class="result-image-stage"><canvas class="result-canvas fluid-slice-canvas" id="windFieldCanvas" width="${canvasWidth}" height="${canvasHeight}" role="img" aria-label="自适应风速箭头图"></canvas><div class="result-north-indicator" aria-label="影像上方为正北"><strong>N</strong><span>↑</span></div></div>${resultColorbar('水平风速 P2–P98 [m/s]', result.stretchMinimum, result.stretchMaximum)}<div class="result-meta"><span>${result.width} × ${result.height}</span><span>上北 · 右东</span><span>自适应箭头 ${Number(result.arrows?.length || 0).toLocaleString('zh-CN')} 个</span><span>每隔 ${result.arrowStep} 个网格采样</span>${observationMeta}<span>色标范围：水平风速 P2–P98</span><span>箭头方向：水平风向</span><span>箭头长度与颜色：水平风速</span></div></div>`
  const canvas = $('#windFieldCanvas')
  const context = canvas.getContext('2d')
  context.imageSmoothingEnabled = true
  context.globalAlpha = 0.34
  context.drawImage(raster.textureCanvas, 0, 0, canvasWidth, canvasHeight)
  context.globalAlpha = 1
  const spacingX = result.arrowStep * canvasWidth / Math.max(1, result.width)
  const spacingY = result.arrowStep * canvasHeight / Math.max(1, result.height)
  const availableLength = Math.max(5, Math.min(spacingX, spacingY) * 0.78)
  const minimum = Number(result.stretchMinimum)
  const maximum = Number(result.stretchMaximum)
  for (const arrow of result.arrows || []) {
    const [gridX, gridY, u, v, magnitude] = arrow.map(Number)
    const horizontalSpeed = Math.hypot(u, v)
    if (!(horizontalSpeed > 1e-8) || !Number.isFinite(magnitude)) continue
    const normalized = maximum > minimum
      ? Math.max(0, Math.min(1, (magnitude - minimum) / (maximum - minimum))) : 0.5
    const length = availableLength * (0.38 + 0.62 * normalized)
    const dx = u / horizontalSpeed * length
    const dy = -v / horizontalSpeed * length
    const centerX = (gridX + 0.5) * canvasWidth / result.width
    const centerY = (gridY + 0.5) * canvasHeight / result.height
    const startX = centerX - dx * 0.5, startY = centerY - dy * 0.5
    const endX = centerX + dx * 0.5, endY = centerY + dy * 0.5
    const angle = Math.atan2(dy, dx)
    const headLength = Math.max(2.5, Math.min(7, length * 0.32))
    context.beginPath()
    context.moveTo(startX, startY)
    context.lineTo(endX, endY)
    context.moveTo(endX, endY)
    context.lineTo(endX - headLength * Math.cos(angle - Math.PI / 6), endY - headLength * Math.sin(angle - Math.PI / 6))
    context.moveTo(endX, endY)
    context.lineTo(endX - headLength * Math.cos(angle + Math.PI / 6), endY - headLength * Math.sin(angle + Math.PI / 6))
    context.strokeStyle = windArrowColor(magnitude, minimum, maximum)
    context.lineWidth = Math.max(1.1, Math.min(2.1, availableLength * 0.09))
    context.lineCap = 'round'
    context.lineJoin = 'round'
    context.stroke()
  }
}

function rasterFromImage(image) {
  const scale = Math.min(1, 768 / Math.max(image.naturalWidth, image.naturalHeight, 1))
  const width = Math.max(1, Math.round(image.naturalWidth * scale))
  const height = Math.max(1, Math.round(image.naturalHeight * scale))
  const canvas = document.createElement('canvas')
  canvas.width = width; canvas.height = height
  const context = canvas.getContext('2d', { willReadFrequently: true })
  context.drawImage(image, 0, 0, width, height)
  const source = context.getImageData(0, 0, width, height).data
  const bytes = new Uint8Array(width * height)
  for (let index = 0; index < bytes.length; index += 1) {
    const offset = index * 4
    bytes[index] = Math.round(source[offset] * .2126 + source[offset + 1] * .7152 + source[offset + 2] * .0722)
  }
  return { width, height, bytes, textureCanvas: canvas }
}

async function drawImagePreview(result, file) {
  // FIXED[低|并发]: 本函数不递增 state.resultPreviewRequest，只比较文件路径；同一文件的 drawEnviPreview 尚在等待 readResult 时切到图像预览，旧请求返回后会覆盖当前画面；已修复：开头同样 ++state.resultPreviewRequest，await 后比对 request 与路径
  const previewRequest = ++state.resultPreviewRequest
  disposeResultViewer()
  state.resultRaster = null
  const imageUrl = `data:${result.mimeType};base64,${result.data}`
  $('#resultPreview').innerHTML = `<div class="result-preview-card"><div class="result-image-stage"><img class="result-image" id="resultImage" alt="${escapeHtml(file.name)}"></div><div class="result-meta"><span>${escapeHtml(file.name)}</span><span>${escapeHtml(result.mimeType)}</span></div></div>`
  const image = $('#resultImage')
  await new Promise((resolve, reject) => {
    image.onload = () => resolve()
    image.onerror = () => reject(new Error('图像结果无法解码'))
    image.src = imageUrl
  })
  if (previewRequest !== state.resultPreviewRequest || state.selectedResult?.path !== file.path) return
  const raster = rasterFromImage(image)
  state.resultRaster = raster
}

function disposeResultViewer() {
  $('#resultPreview')?.classList.remove('analysis-preview')
  if (!resultViewer) return
  resultViewer.resizeObserver?.disconnect()
  resultViewer.controls.dispose()
  resultViewer.scene.traverse((object) => {
    object.geometry?.dispose?.()
    if (Array.isArray(object.material)) object.material.forEach((material) => material.dispose?.())
    else object.material?.dispose?.()
  })
  resultViewer.texture?.dispose?.()
  resultViewer.renderer.dispose()
  // dispose() keeps the GL context alive; Chromium caps live contexts (~16)
  // and evicts the oldest one, which is the main scene viewport.
  resultViewer.renderer.forceContextLoss()
  resultViewer.renderer.domElement.remove()
  resultViewer = null
}

function resizeResultViewer() {
  if (!resultViewer) return
  const host = resultViewer.host
  const width = Math.max(1, host.clientWidth), height = Math.max(1, host.clientHeight)
  resultViewer.renderer.setSize(width, height, false)
  resultViewer.camera.aspect = width / height
  resultViewer.camera.updateProjectionMatrix()
}

function renderResultViewer() {
  if (!resultViewer || $('#resultDialog').hidden) return
  resultViewer.controls.update()
  resultViewer.renderer.render(resultViewer.scene, resultViewer.camera)
}

function showResult3D(raster, file, sourceResult = null) {
  if (!raster) return
  disposeResultViewer()
  const rasterResult = ['envi', 'tiff'].includes(sourceResult?.kind) ? sourceResult : null
  const rasterLabel = rasterResult?.bandNames?.[rasterResult.band] || '像素亮度'
  const rasterMinimum = rasterResult?.minimum ?? 0
  const rasterMaximum = rasterResult?.maximum ?? 255
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${resultColorbar(rasterLabel, rasterMinimum, rasterMaximum, 'gray')}${resultViewActions()}<div class="result-meta"><span>${raster.width} × ${raster.height}</span><span>整幅图像三维分析</span><span>高度按像素亮度映射</span></div></div>`
  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const resultSun = new THREE.DirectionalLight(0xfff7e8, 3)
  resultSun.position.set(8, 14, 10)
  resultScene.add(resultSun)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .1, 120)
  camera3d.position.set(15, 13, 16)
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true
  controls3d.dampingFactor = .08
  controls3d.maxPolarAngle = Math.PI * .48
  controls3d.minDistance = 5
  controls3d.maxDistance = 55
  controls3d.target.set(0, 1.2, 0)

  const columns = Math.max(2, Math.min(180, raster.width))
  const rows = Math.max(2, Math.min(180, raster.height))
  const width = 16
  const depth = width * raster.height / Math.max(1, raster.width)
  const positions = [], uvs = [], indices = []
  for (let row = 0; row < rows; row += 1) {
    const sy = Math.round(row * (raster.height - 1) / Math.max(1, rows - 1))
    for (let column = 0; column < columns; column += 1) {
      const sx = Math.round(column * (raster.width - 1) / Math.max(1, columns - 1))
      const brightness = raster.bytes[sy * raster.width + sx] / 255
      positions.push((column / (columns - 1) - .5) * width, .08 + brightness * 4.12, (.5 - row / (rows - 1)) * depth)
      uvs.push(column / (columns - 1), 1 - row / (rows - 1))
    }
  }
  for (let row = 0; row < rows - 1; row += 1) for (let column = 0; column < columns - 1; column += 1) {
    const a = row * columns + column, b = a + 1, c = a + columns, d = c + 1
    indices.push(a, c, b, b, c, d)
  }
  const geometry = new THREE.BufferGeometry()
  geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3))
  geometry.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2))
  geometry.setIndex(indices)
  geometry.computeVertexNormals()
  const texture = new THREE.CanvasTexture(raster.textureCanvas)
  texture.colorSpace = THREE.SRGBColorSpace
  texture.minFilter = THREE.NearestFilter
  texture.magFilter = THREE.NearestFilter
  texture.generateMipmaps = false
  const surface = new THREE.Mesh(geometry, new THREE.MeshStandardMaterial({ map: texture, roughness: .88, metalness: .02, side: THREE.DoubleSide, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false }))
  surface.castShadow = surface.receiveShadow = true
  resultScene.add(surface)
  const base = new THREE.Mesh(new THREE.PlaneGeometry(width, depth), new THREE.MeshBasicMaterial({ color: 0xe6edf5, transparent: true, opacity: .8, side: THREE.DoubleSide }))
  base.rotation.x = -Math.PI / 2; base.position.y = -.06
  resultScene.add(base)
  const resultGrid = new THREE.GridHelper(Math.max(width, depth), 16, 0x7895b5, 0xb8c6d6)
  resultGrid.position.y = -.04; resultGrid.material.opacity = .25; resultGrid.material.transparent = true
  resultScene.add(resultGrid)
  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }), texture }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2))
  resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  resultViewer.renderer.toneMapping = THREE.ACESFilmicToneMapping
  resultViewer.renderer.toneMappingExposure = 1.05
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer)
  resultViewer.resizeObserver.observe(host)
  resizeResultViewer()
  controls3d.update()
  $('#result3dBtn').classList.add('active')
  // FIXED[低|错误处理]: 返回 2D 时 drawImagePreview/drawTiffPreview 返回的 Promise 没有 catch，解码失败成为未处理 rejection 且预览区停留在空 <img>；sourceResult 为 null 时 drawImagePreview(null) 会因 result.mimeType 抛 TypeError；已修复：sourceResult 与 rasterResult 都为空时直接返回，绘制用 async 处理器 await 并 try/catch，失败时先 disposeResultViewer 再用 emptyResultPreview + addLog 提示
  $('#result2dBtn').addEventListener('click', async () => {
    if (!rasterResult && !sourceResult) return
    try {
      if (!rasterResult) await drawImagePreview(sourceResult, file)
      else if (rasterResult.kind === 'tiff') await drawTiffPreview(rasterResult, file)
      else await drawEnviPreview(rasterResult, file)
    } catch (error) {
      disposeResultViewer()
      emptyResultPreview('结果读取失败', error.message)
      addLog(`结果预览失败：${error.message}`, 'error')
    }
  })

}

function facetControls(result, metric, side) {
  const metrics = result.metrics || []
  const metricButtons = metrics.map((item) => `<button class="button ghost facet-metric ${item.id === metric ? 'active' : ''}" type="button" data-metric="${escapeHtml(item.id)}">${escapeHtml(item.label)}</button>`).join('')
  return `<div class="facet-controls"><div><span>显示指标</span>${metricButtons}</div><div><span>面方向</span><button class="button ghost facet-side ${side === 'both' ? 'active' : ''}" type="button" data-side="both">正反两面</button><button class="button ghost facet-side ${side === 'front' ? 'active' : ''}" type="button" data-side="front">正面</button><button class="button ghost facet-side ${side === 'back' ? 'active' : ''}" type="button" data-side="back">背面</button></div></div>`
}

function facetIndices(result, side) {
  const total = result.facetCount * 2
  if (side === 'front') return Array.from({ length: result.facetCount }, (_, index) => index * 2)
  if (side === 'back') return Array.from({ length: result.facetCount }, (_, index) => index * 2 + 1)
  return Array.from({ length: total }, (_, index) => index)
}

function facetStatsForSide(values, facetCount, side) {
  let minimum = Infinity, maximum = -Infinity
  const update = (value) => {
    if (!Number.isFinite(value)) return
    minimum = Math.min(minimum, value); maximum = Math.max(maximum, value)
  }
  for (let facet = 0; facet < facetCount; facet += 1) {
    const front = Number(values[facet * 2])
    const back = Number(values[facet * 2 + 1])
    if (side !== 'back') update(front)
    if (side !== 'front') update(back)
  }
  return Number.isFinite(minimum) ? { minimum, maximum } : { minimum: 0, maximum: 0 }
}

function facetMetric(result, metric) {
  return result.metrics.find((item) => item.id === metric) || result.metrics[0]
}

function bindFacetControls(result, file, metric, side) {
  $$('.facet-metric').forEach((button) => button.addEventListener('click', () => showFacet3D(result, file, button.dataset.metric, side)))
  $$('.facet-side').forEach((button) => button.addEventListener('click', () => showFacet3D(result, file, metric, button.dataset.side)))
  $('#facet3dBtn')?.addEventListener('click', () => showFacet3D(result, file, metric, side))
}

function facetViewActions() {
  return '<div class="result-view-actions"><button class="button ghost facet-3d-btn active" id="facet3dBtn" type="button">面元三维</button></div>'
}

function showFacet3D(result, file, metric = 'radiosity', side = 'both') {
  disposeResultViewer()
  const selected = facetMetric(result, metric)
  const stats = facetStatsForSide(selected.values, result.facetCount, side)
  const valueCount = side === 'both' ? result.facetCount * 2 : result.facetCount
  const sourceFacetCount = Math.max(result.facetCount, Number(result.sourceFacetCount) || result.facetCount)
  const samplingLabel = sourceFacetCount > result.facetCount
    ? '面元过多：原始 ' + sourceFacetCount.toLocaleString('zh-CN') + ' 个，当前显示 ' + result.facetCount.toLocaleString('zh-CN') + ' 个'
    : '完整显示 ' + result.facetCount.toLocaleString('zh-CN') + ' 个面'
  const rawPositions = Array.isArray(result.vertexPositions) ? result.vertexPositions : []
  const hasGeometry = rawPositions.length >= result.facetCount * 9
  const sideDescription = side === 'both' ? '三维表面取正反面较大值' : side === 'front' ? '显示正面值' : '显示背面值'
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${resultColorbar(selected.label, stats.minimum, stats.maximum)}${facetControls(result, selected.id, side)}${facetViewActions()}<div class="result-meta"><span>${escapeHtml(samplingLabel)} · ${valueCount.toLocaleString('zh-CN')} 个面值</span><span>颜色：${escapeHtml(selected.label)}</span><span>${sideDescription}</span><span>MIN ${Number(stats.minimum).toPrecision(6)} · MAX ${Number(stats.maximum).toPrecision(6)}</span>${hasGeometry ? '<span>真实 OBJ 面元空间位置</span>' : '<span>旧结果无几何坐标，使用面元序号排列</span>'}</div></div>`
  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const resultSun = new THREE.DirectionalLight(0xfff7e8, 3)
  resultSun.position.set(8, 14, 10); resultScene.add(resultSun)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .01, 300)
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true; controls3d.dampingFactor = .08; controls3d.maxPolarAngle = Math.PI * .49
  controls3d.minDistance = .5; controls3d.maxDistance = 300

  const range = stats.maximum - stats.minimum
  const colorForValue = (value, color) => {
    const normalized = Number.isFinite(value) ? (range ? (value - stats.minimum) / range : .5) : 0
    color.setHSL((1 - normalized) * .67, Number.isFinite(value) ? .82 : 0, Number.isFinite(value) ? .5 : .16)
    return normalized
  }

  if (hasGeometry) {
    let minimum = [Infinity, Infinity, Infinity], maximum = [-Infinity, -Infinity, -Infinity]
    for (let index = 0; index < result.facetCount * 9; index += 3) {
      for (let axis = 0; axis < 3; axis += 1) {
        const value = Number(rawPositions[index + axis])
        if (!Number.isFinite(value)) continue
        minimum[axis] = Math.min(minimum[axis], value); maximum[axis] = Math.max(maximum[axis], value)
      }
    }
    const spanX = Math.max(.001, maximum[0] - minimum[0]), spanY = Math.max(.001, maximum[1] - minimum[1]), spanZ = Math.max(.001, maximum[2] - minimum[2])
    const modelScale = 20 / Math.max(spanX, spanY, spanZ)
    const centerX = (minimum[0] + maximum[0]) / 2, centerZ = (minimum[2] + maximum[2]) / 2
    const positions = new Float32Array(result.facetCount * 9)
    const colors = new Float32Array(result.facetCount * 9)
    const facetColor = new THREE.Color()
    for (let facet = 0; facet < result.facetCount; facet += 1) {
      const front = Number(selected.values[facet * 2])
      const back = Number(selected.values[facet * 2 + 1])
      const value = side === 'front' ? front : side === 'back' ? back : Math.max(front, back)
      colorForValue(value, facetColor)
      for (let corner = 0; corner < 3; corner += 1) {
        const offset = facet * 9 + corner * 3
        positions[offset] = (Number(rawPositions[offset]) - centerX) * modelScale
        positions[offset + 1] = (Number(rawPositions[offset + 1]) - minimum[1]) * modelScale
        positions[offset + 2] = (Number(rawPositions[offset + 2]) - centerZ) * modelScale
        colors[offset] = facetColor.r; colors[offset + 1] = facetColor.g; colors[offset + 2] = facetColor.b
      }
    }
    const geometry = new THREE.BufferGeometry()
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3))
    geometry.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3))
    const surface = new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.DoubleSide, toneMapped: false, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false }))
    surface.castShadow = surface.receiveShadow = true
    resultScene.add(surface)
    const groundWidth = Math.max(.5, spanX * modelScale), groundDepth = Math.max(.5, spanZ * modelScale)
    const base = new THREE.Mesh(new THREE.PlaneGeometry(groundWidth * 1.08, groundDepth * 1.08), new THREE.MeshBasicMaterial({ color: 0xe6edf5, transparent: true, opacity: .8, side: THREE.DoubleSide }))
    base.rotation.x = -Math.PI / 2; base.position.y = -.04; resultScene.add(base)
    const resultGrid = new THREE.GridHelper(Math.max(groundWidth, groundDepth) * 1.08, 22, 0x7895b5, 0xb8c6d6)
    resultGrid.position.y = -.02; resultGrid.scale.z = groundDepth / Math.max(groundWidth, groundDepth); resultGrid.scale.x = groundWidth / Math.max(groundWidth, groundDepth); resultGrid.material.opacity = .25; resultGrid.material.transparent = true; resultScene.add(resultGrid)
    camera3d.position.set(Math.max(16, groundWidth * 1.25), Math.max(12, spanY * modelScale * .85 + 8), Math.max(16, groundDepth * 1.25))
    controls3d.target.set(0, Math.max(.5, spanY * modelScale * .35), 0)
  } else {
    const indices = facetIndices(result, side)
    const columns = Math.max(1, Math.ceil(Math.sqrt(indices.length)))
    const rows = Math.max(1, Math.ceil(indices.length / columns))
    const sideLength = 22, cell = sideLength / Math.max(columns, rows)
    const geometry = new THREE.BoxGeometry(Math.max(.04, cell * .72), 1, Math.max(.04, cell * .72))
    const bars = new THREE.InstancedMesh(geometry, new THREE.MeshStandardMaterial({ vertexColors: true, roughness: .82, metalness: .02, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false }), indices.length)
    const matrix = new THREE.Matrix4(), color = new THREE.Color()
    indices.forEach((index, position) => {
      const value = Number(selected.values[index]), normalized = colorForValue(value, color)
      const height = Number.isFinite(value) ? .18 + normalized * 6 : .04
      const column = position % columns, row = Math.floor(position / columns)
      matrix.compose(new THREE.Vector3((column - (columns - 1) / 2) * cell, height / 2, ((rows - 1) / 2 - row) * cell), new THREE.Quaternion(), new THREE.Vector3(1, height, 1))
      bars.setMatrixAt(position, matrix); bars.setColorAt(position, color)
    })
    bars.instanceMatrix.needsUpdate = true; if (bars.instanceColor) bars.instanceColor.needsUpdate = true
    resultScene.add(bars)
    const base = new THREE.Mesh(new THREE.PlaneGeometry(sideLength, sideLength), new THREE.MeshBasicMaterial({ color: 0xe6edf5, transparent: true, opacity: .8, side: THREE.DoubleSide }))
    base.rotation.x = -Math.PI / 2; base.position.y = -.04; resultScene.add(base)
    const resultGrid = new THREE.GridHelper(sideLength, 22, 0x7895b5, 0xb8c6d6)
    resultGrid.position.y = -.02; resultGrid.material.opacity = .25; resultGrid.material.transparent = true; resultScene.add(resultGrid)
    camera3d.position.set(sideLength * .8, sideLength * .75, sideLength * .9); controls3d.target.set(0, 1.8, 0)
  }

  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }) }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  resultViewer.renderer.toneMapping = THREE.ACESFilmicToneMapping; resultViewer.renderer.toneMappingExposure = 1.05
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer); resultViewer.resizeObserver.observe(host)
  resizeResultViewer(); controls3d.update(); $('#facet3dBtn').classList.add('active'); bindFacetControls(result, file, selected.id, side)
}

function showVoxel3D(result, file, metric = result.metrics?.[0]?.id) {
  disposeResultViewer()
  const selected = result.metrics.find((item) => item.id === metric) || result.metrics[0]
  if (!selected || !Array.isArray(result.voxelPositions) || !result.voxelCount) {
    emptyResultPreview('体元过程结果为空', '没有可显示的体元位置或过程变量。')
    return
  }
  let valueMinimum = Infinity, valueMaximum = -Infinity
  let minimum = [Infinity, Infinity, Infinity], maximum = [-Infinity, -Infinity, -Infinity]
  for (let voxel = 0; voxel < result.voxelCount; voxel += 1) {
    // FIXED[中|Bug]: 与 showSoilProfile3D 的做法对齐——JSON 中的 NaN/NoData 序列化为 null，Number(null) === 0 会被当成有效值参与 min/max；已修复：非数字（含 null）不再被 Number() 强转为 0，按无效值处理（下方着色循环同样处理；showFacet3D 未改）
    const raw = selected.values[voxel]
    const value = typeof raw === 'number' ? raw : NaN
    if (Number.isFinite(value)) { valueMinimum = Math.min(valueMinimum, value); valueMaximum = Math.max(valueMaximum, value) }
    for (let axis = 0; axis < 3; axis += 1) {
      const position = Number(result.voxelPositions[voxel * 3 + axis])
      if (Number.isFinite(position)) { minimum[axis] = Math.min(minimum[axis], position); maximum[axis] = Math.max(maximum[axis], position) }
    }
  }
  if (!Number.isFinite(valueMinimum)) valueMinimum = valueMaximum = 0
  const sourceCount = Math.max(result.voxelCount, Number(result.sourceVoxelCount) || result.voxelCount)
  const sampled = sourceCount > result.voxelCount
  const sampleLabel = sampled
    ? `抽样显示 ${result.voxelCount.toLocaleString('zh-CN')} / ${sourceCount.toLocaleString('zh-CN')} 个体元`
    : `完整显示 ${result.voxelCount.toLocaleString('zh-CN')} 个体元`
  const samplingNotice = sampled
    ? `<div class="result-sampling-notice"><strong>当前为抽样显示</strong><span>结果文件已保存全部 ${sourceCount.toLocaleString('zh-CN')} 个体元；三维窗口抽样显示 ${result.voxelCount.toLocaleString('zh-CN')} 个。</span></div>`
    : ''
  const metricButtons = result.metrics.map((item) => `<button class="button ghost facet-metric ${item.id === selected.id ? 'active' : ''}" type="button" data-metric="${escapeHtml(item.id)}">${escapeHtml(item.label)}</button>`).join('')
  const soilProfileAction = result.soilProfile
    ? '<button class="button ghost" id="soilProfile3dBtn" type="button">多层土壤温度</button>' : ''
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${samplingNotice}${resultColorbar(selected.label, valueMinimum, valueMaximum)}<div class="facet-controls"><div><span>显示指标</span>${metricButtons}</div></div><div class="result-view-actions"><button class="button ghost facet-3d-btn active" id="facet3dBtn" type="button">体元过程</button>${soilProfileAction}</div><div class="result-meta"><span>${escapeHtml(sampleLabel)}</span><span>颜色：${escapeHtml(selected.label)}</span><span>MIN ${Number(valueMinimum).toPrecision(6)} · MAX ${Number(valueMaximum).toPrecision(6)}</span><span>${escapeHtml(result.time || '静态辐射场')}</span></div></div>`

  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const span = maximum.map((value, axis) => Math.max(.001, value - minimum[axis]))
  const modelScale = 20 / Math.max(...span)
  const center = minimum.map((value, axis) => (value + maximum[axis]) / 2)
  // 略微覆盖相邻体元，消除斜视角和抗锯齿造成的地形接缝。
  const boxSize = Math.max(.025, Number(result.voxelSize || 1) * modelScale * 1.002)
  const geometry = new THREE.BoxGeometry(boxSize, boxSize, boxSize)
  // BoxGeometry 为六个面保留独立法向；顶点颜色只表达面朝向，实例颜色仍表达计算指标。
  const zenith = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(Number(state.config?.light?.zenith) || 0, 0, 90))
  const azimuth = THREE.MathUtils.degToRad(((Number(state.config?.light?.azimuth) || 0) % 360 + 360) % 360)
  const solarDirection = new THREE.Vector3(
    Math.sin(zenith) * Math.cos(azimuth),
    Math.cos(zenith),
    Math.sin(zenith) * Math.sin(azimuth)
  ).normalize()
  const normals = geometry.getAttribute('normal')
  const vertexCount = geometry.getAttribute('position').count
  const faceColors = new Float32Array(vertexCount * 3)
  for (let vertex = 0; vertex < vertexCount; vertex += 1) {
    const incidence = Math.max(0,
      normals.getX(vertex) * solarDirection.x +
      normals.getY(vertex) * solarDirection.y +
      normals.getZ(vertex) * solarDirection.z)
    const brightness = .32 + .68 * incidence
    faceColors[vertex * 3] = brightness
    faceColors[vertex * 3 + 1] = brightness
    faceColors[vertex * 3 + 2] = brightness
  }
  geometry.setAttribute('color', new THREE.Float32BufferAttribute(faceColors, 3))
  const material = new THREE.MeshBasicMaterial({ color: 0xffffff, vertexColors: true, transparent: false, opacity: 1, depthTest: true, depthWrite: true, alphaTest: 0, alphaToCoverage: false, toneMapped: false })
  const voxels = new THREE.InstancedMesh(geometry, material, result.voxelCount)
  const matrix = new THREE.Matrix4(), color = new THREE.Color()
  const valueRange = valueMaximum - valueMinimum
  for (let voxel = 0; voxel < result.voxelCount; voxel += 1) {
    const x = (Number(result.voxelPositions[voxel * 3]) - center[0]) * modelScale
    const y = (Number(result.voxelPositions[voxel * 3 + 1]) - minimum[1]) * modelScale
    const z = (Number(result.voxelPositions[voxel * 3 + 2]) - center[2]) * modelScale
    matrix.makeTranslation(x, y, z); voxels.setMatrixAt(voxel, matrix)
    const rawValue = selected.values[voxel]
    const value = typeof rawValue === 'number' ? rawValue : NaN
    const normalized = Number.isFinite(value) ? (valueRange ? (value - valueMinimum) / valueRange : .5) : 0
    color.setHSL((1 - normalized) * .67, Number.isFinite(value) ? .82 : 0, Number.isFinite(value) ? .5 : .16)
    voxels.setColorAt(voxel, color)
  }
  voxels.instanceMatrix.needsUpdate = true
  if (voxels.instanceColor) voxels.instanceColor.needsUpdate = true
  material.needsUpdate = true
  resultScene.add(voxels)
  const groundWidth = Math.max(.5, span[0] * modelScale), groundDepth = Math.max(.5, span[2] * modelScale)
  const grid = new THREE.GridHelper(Math.max(groundWidth, groundDepth) * 1.08, 22, 0x7895b5, 0xb8c6d6)
  grid.position.y = -.02; grid.scale.x = groundWidth / Math.max(groundWidth, groundDepth); grid.scale.z = groundDepth / Math.max(groundWidth, groundDepth); grid.material.opacity = .25; grid.material.transparent = true
  resultScene.add(grid)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .01, 300)
  camera3d.position.set(Math.max(16, groundWidth * 1.2), Math.max(12, span[1] * modelScale + 7), Math.max(16, groundDepth * 1.2))
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true; controls3d.dampingFactor = .08; controls3d.maxPolarAngle = Math.PI * .49; controls3d.minDistance = .5; controls3d.maxDistance = 300
  controls3d.target.set(0, Math.max(.5, span[1] * modelScale * .35), 0)
  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }) }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer); resultViewer.resizeObserver.observe(host)
  resizeResultViewer(); controls3d.update()
  $$('.facet-metric').forEach((button) => button.addEventListener('click', () => showVoxel3D(result, file, button.dataset.metric)))
  $('#facet3dBtn')?.addEventListener('click', () => showVoxel3D(result, file, selected.id))
  $('#soilProfile3dBtn')?.addEventListener('click', () => showSoilProfile3D(result, file))
}

function soilProfileDepthLabel(depth, unit) {
  const numeric = Number(depth)
  if (!Number.isFinite(numeric)) return '未知深度'
  if (unit === 'm') {
    if (numeric === 0) return '表层 0 m'
    if (numeric < .1) return `${formatColorbarValue(numeric * 100)} cm`
  }
  return `${formatColorbarValue(numeric)} ${unit || ''}`.trim()
}

function showSoilProfile3D(result, file, selectedLayers = null) {
  const profile = result.soilProfile
  // FIXED[中|内存泄漏]: 此处在 disposeResultViewer() 之前就用 emptyResultPreview 覆盖 innerHTML，旧 resultViewer 的 WebGL 上下文/几何/ResizeObserver 未释放，且 animate 中 renderResultViewer 仍对已脱离 DOM 的画布持续渲染；已修复：把 disposeResultViewer() 提到校验之前，空结果直接释放旧视图
  disposeResultViewer()
  if (!profile || !Array.isArray(profile.values) || !profile.layerCount ||
      !Array.isArray(result.voxelPositions) || !result.voxelCount) {
    emptyResultPreview('土壤温度剖面为空', '当前结果没有多层土壤温度数据。')
    return
  }
  const allLayers = Array.from({ length: profile.layerCount }, (_, layer) => layer)
  const visibleLayers = selectedLayers instanceof Set
    ? new Set([...selectedLayers].filter((layer) => allLayers.includes(layer)))
    : new Set(allLayers)
  if (!visibleLayers.size) visibleLayers.add(0)

  let valueMinimum = Infinity, valueMaximum = -Infinity
  let minimumX = Infinity, maximumX = -Infinity, minimumZ = Infinity, maximumZ = -Infinity
  const validIndices = Array.from({ length: profile.layerCount }, () => [])
  for (let layer = 0; layer < profile.layerCount; layer += 1) {
    const values = Array.isArray(profile.values[layer]) ? profile.values[layer] : []
    for (let voxel = 0; voxel < Math.min(result.voxelCount, values.length); voxel += 1) {
      // JSON serializes binary NaN/NoData as null.  Number(null) is 0, which
      // previously drew every vegetation/building voxel as a false cold spot
      // through all soil layers.
      const rawValue = values[voxel]
      if (typeof rawValue !== 'number' || !Number.isFinite(rawValue)) continue
      const value = rawValue
      validIndices[layer].push(voxel)
      valueMinimum = Math.min(valueMinimum, value)
      valueMaximum = Math.max(valueMaximum, value)
      const x = Number(result.voxelPositions[voxel * 3])
      const z = Number(result.voxelPositions[voxel * 3 + 2])
      if (Number.isFinite(x)) { minimumX = Math.min(minimumX, x); maximumX = Math.max(maximumX, x) }
      if (Number.isFinite(z)) { minimumZ = Math.min(minimumZ, z); maximumZ = Math.max(maximumZ, z) }
    }
  }
  if (!Number.isFinite(valueMinimum) || !Number.isFinite(minimumX) || !Number.isFinite(minimumZ)) {
    emptyResultPreview('土壤温度剖面为空', '该过程结果中没有有效的土壤体元温度。')
    return
  }

  const depthButtons = allLayers.map((layer) => {
    const label = soilProfileDepthLabel(profile.depths?.[layer] ?? layer, profile.depthUnit)
    return `<button class="button ghost soil-profile-layer ${visibleLayers.has(layer) ? 'active' : ''}" type="button" data-layer="${layer}">L${layer + 1} · ${escapeHtml(label)}</button>`
  }).join('')
  const metricButtons = result.metrics.map((item) => `<button class="button ghost facet-metric" type="button" data-metric="${escapeHtml(item.id)}">${escapeHtml(item.label)}</button>`).join('')
  const sourceCount = Math.max(result.voxelCount, Number(result.sourceVoxelCount) || result.voxelCount)
  const soilCount = validIndices.reduce((maximum, indices) => Math.max(maximum, indices.length), 0)
  const unitLabel = profile.temperatureUnit === 'degC' ? '°C' : profile.temperatureUnit
  const activeLayerList = allLayers.filter((layer) => visibleLayers.has(layer))
  const maximumInstances = 320000
  const maximumPerLayer = Math.max(1, Math.floor(maximumInstances / activeLayerList.length))
  const displayIndices = new Map(activeLayerList.map((layer) => {
    const source = validIndices[layer]
    if (source.length <= maximumPerLayer) return [layer, source]
    return [layer, Array.from({ length: maximumPerLayer }, (_, index) =>
      source[Math.min(source.length - 1, Math.floor(index * source.length / maximumPerLayer))])]
  }))
  const instanceCount = [...displayIndices.values()].reduce((sum, indices) => sum + indices.length, 0)
  const sourceLayerInstances = activeLayerList.reduce((sum, layer) => sum + validIndices[layer].length, 0)
  const layerSampled = instanceCount < sourceLayerInstances
  const displaySamplingLabel = layerSampled
    ? `当前显示 ${instanceCount.toLocaleString('zh-CN')} 个分层单元（均匀抽样）` : ''
  const samplingNotice = sourceCount > result.voxelCount || layerSampled
    ? `<div class="result-sampling-notice"><strong>当前为抽样显示</strong><span>结果文件已保存全部 ${sourceCount.toLocaleString('zh-CN')} 个体元；三维窗口当前显示 ${instanceCount.toLocaleString('zh-CN')} 个分层单元。</span></div>`
    : ''
  $('#resultPreview').innerHTML = `<div class="result-preview-card result-3d-card"><div class="result-3d-viewport" id="result3dViewport"></div>${samplingNotice}${resultColorbar(`土壤温度 [${unitLabel}]`, valueMinimum, valueMaximum)}<div class="facet-controls"><div><span>土层开关</span>${depthButtons}</div></div><div class="result-view-actions"><button class="button ghost" id="facet3dBtn" type="button">体元过程</button><button class="button ghost active" id="soilProfile3dBtn" type="button">多层土壤温度</button></div><div class="result-meta"><span>${profile.layerCount} 层温度剖面 · ${soilCount.toLocaleString('zh-CN')} 个有效土壤体元</span><span>深度：${escapeHtml((profile.depths || []).map((depth) => soilProfileDepthLabel(depth, profile.depthUnit)).join('、'))}</span><span>统一色标：${formatColorbarValue(valueMinimum)} ～ ${formatColorbarValue(valueMaximum)} ${escapeHtml(unitLabel)}</span><span>垂向等距展开，标签为真实土层深度</span>${displaySamplingLabel ? `<span>${escapeHtml(displaySamplingLabel)}</span>` : ''}<span>${sourceCount > result.voxelCount ? `抽样载入 ${result.voxelCount.toLocaleString('zh-CN')} / ${sourceCount.toLocaleString('zh-CN')} 个体元` : escapeHtml(result.time || '静态温度场')}</span></div></div>`

  const host = $('#result3dViewport')
  const resultScene = new THREE.Scene()
  resultScene.background = new THREE.Color(0xf7f9fc)
  resultScene.add(new THREE.HemisphereLight(0xe8f1fb, 0xb8c8d8, 1.8))
  const spanX = Math.max(.001, maximumX - minimumX)
  const spanZ = Math.max(.001, maximumZ - minimumZ)
  const modelScale = 20 / Math.max(spanX, spanZ)
  const centerX = (minimumX + maximumX) / 2
  const centerZ = (minimumZ + maximumZ) / 2
  const layerGap = Math.max(.8, Math.min(1.5, 10 / Math.max(1, profile.layerCount - 1)))
  const cellSize = Math.max(.025, Number(result.voxelSize || 1) * modelScale * 1.002)
  const cellHeight = Math.max(.025, cellSize * .07)
  const geometry = new THREE.BoxGeometry(cellSize, cellHeight, cellSize)
  const vertexCount = geometry.getAttribute('position').count
  geometry.setAttribute('color', new THREE.Float32BufferAttribute(new Float32Array(vertexCount * 3).fill(1), 3))
  const material = new THREE.MeshBasicMaterial({ color: 0xffffff, vertexColors: true, toneMapped: false })
  const layers = new THREE.InstancedMesh(geometry, material, instanceCount)
  const matrix = new THREE.Matrix4(), color = new THREE.Color()
  const valueRange = valueMaximum - valueMinimum
  let instance = 0
  for (const layer of activeLayerList) {
    const y = (profile.layerCount - 1 - layer) * layerGap
    for (const voxel of displayIndices.get(layer)) {
      const x = (Number(result.voxelPositions[voxel * 3]) - centerX) * modelScale
      const z = (Number(result.voxelPositions[voxel * 3 + 2]) - centerZ) * modelScale
      matrix.makeTranslation(x, y, z)
      layers.setMatrixAt(instance, matrix)
      const value = Number(profile.values[layer][voxel])
      const normalized = valueRange ? (value - valueMinimum) / valueRange : .5
      color.setHSL((1 - normalized) * .67, .82, .5)
      layers.setColorAt(instance, color)
      instance += 1
    }
    const gridSize = Math.max(spanX, spanZ) * modelScale * 1.04
    const grid = new THREE.GridHelper(gridSize, 20, 0x7895b5, 0xb8c6d6)
    grid.position.y = y - cellHeight
    grid.scale.x = spanX / Math.max(spanX, spanZ)
    grid.scale.z = spanZ / Math.max(spanX, spanZ)
    grid.material.opacity = .16
    grid.material.transparent = true
    resultScene.add(grid)
  }
  layers.instanceMatrix.needsUpdate = true
  if (layers.instanceColor) layers.instanceColor.needsUpdate = true
  resultScene.add(layers)

  const groundWidth = Math.max(.5, spanX * modelScale)
  const groundDepth = Math.max(.5, spanZ * modelScale)
  const stackHeight = Math.max(layerGap, (profile.layerCount - 1) * layerGap)
  const camera3d = new THREE.PerspectiveCamera(42, 1, .01, 300)
  camera3d.position.set(Math.max(16, groundWidth * 1.2), Math.max(14, stackHeight * 1.2 + 6), Math.max(16, groundDepth * 1.2))
  const controls3d = new OrbitControls(camera3d, host)
  controls3d.enableDamping = true; controls3d.dampingFactor = .08; controls3d.maxPolarAngle = Math.PI * .52; controls3d.minDistance = .5; controls3d.maxDistance = 300
  controls3d.target.set(0, stackHeight * .45, 0)
  resultViewer = { host, scene: resultScene, camera: camera3d, controls: controls3d, renderer: new THREE.WebGLRenderer({ antialias: true, alpha: true }) }
  resultViewer.renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); resultViewer.renderer.outputColorSpace = THREE.SRGBColorSpace
  host.append(resultViewer.renderer.domElement)
  resultViewer.resizeObserver = new ResizeObserver(resizeResultViewer); resultViewer.resizeObserver.observe(host)
  resizeResultViewer(); controls3d.update()

  $$('.soil-profile-layer').forEach((button) => button.addEventListener('click', () => {
    const next = new Set(visibleLayers)
    const layer = Number(button.dataset.layer)
    if (next.has(layer) && next.size > 1) next.delete(layer)
    else next.add(layer)
    showSoilProfile3D(result, file, next)
  }))
  $$('.facet-metric').forEach((button) => button.addEventListener('click', () => showVoxel3D(result, file, button.dataset.metric)))
  $('#facet3dBtn')?.addEventListener('click', () => showVoxel3D(result, file))
  $('#soilProfile3dBtn')?.addEventListener('click', () => showSoilProfile3D(result, file, visibleLayers))
}

function parseCsvRows(content) {
  const rows = []
  let row = [], cell = '', quoted = false
  const source = String(content || '').replace(/^\uFEFF/, '')
  for (let index = 0; index <= source.length; index += 1) {
    const character = source[index] ?? '\n'
    if (quoted) {
      if (character === '"' && source[index + 1] === '"') { cell += '"'; index += 1 }
      else if (character === '"') quoted = false
      else cell += character
    } else if (character === '"') quoted = true
    else if (character === ',') { row.push(cell); cell = '' }
    else if (character === '\n') {
      row.push(cell.replace(/\r$/, '')); cell = ''
      if (row.some((value) => value !== '')) rows.push(row)
      row = []
    } else cell += character
  }
  if (rows.length < 2) return []
  const headers = rows[0].map((value) => value.trim())
  return rows.slice(1).map((values) => Object.fromEntries(headers.map((header, index) => [header, values[index] ?? ''])))
}

function fileAngleToken(file, key) {
  const match = String(file || '').match(new RegExp(`(?:^|_)${key}=([^_.]+(?:\\.[^_.]+)?)(?=_|\\.|$)`, 'i'))
  return match ? Number(match[1]) : NaN
}

function csvNumeric(value) {
  return String(value ?? '').trim() === '' ? NaN : Number(value)
}

function statisticsRows(content, observation = {}) {
  const latest = new Map()
  for (const source of parseCsvRows(content)) {
    const row = {
      file: source.file || '', mode: source.mode || 'Unknown', time: source.time || '',
      vza: csvNumeric(source.vza), vaa: csvNumeric(source.vaa), bandIndex: csvNumeric(source.band_index),
      bandName: source.band_name || `Band ${Number(source.band_index) + 1}`,
      count: csvNumeric(source.count), min: csvNumeric(source.min), max: csvNumeric(source.max),
      mean: csvNumeric(source.mean), stddev: csvNumeric(source.stddev)
    }
    row.sza = csvNumeric(source.sza)
    row.saa = csvNumeric(source.saa)
    row.planeAzimuth = csvNumeric(observation.planeAzimuth)
    row.principalPlaneRequested = observation.principalPlane
    row.hemisphereRequested = observation.hemisphere
    if (!Number.isFinite(row.sza)) row.sza = fileAngleToken(row.file, 'SZA')
    if (!Number.isFinite(row.saa)) row.saa = fileAngleToken(row.file, 'SAA')
    if (!Number.isFinite(row.vza)) row.vza = fileAngleToken(row.file, 'VZA')
    if (!Number.isFinite(row.vaa)) row.vaa = fileAngleToken(row.file, 'VAA')
    if (!Number.isFinite(row.bandIndex)) continue
    // VZA=0 is a single physical direction: VAA has no meaning at nadir.
    // Keeping the latest equivalent nadir row also hides stale records from
    // statistics files produced by older append-only engine builds.
    const directionKey = Math.abs(row.vza) < 1e-6
      ? `nadir\u0000${row.mode}\u0000${row.time}\u0000${row.sza}\u0000${row.saa}`
      : row.file
    latest.set(`${directionKey}\u0000${row.bandIndex}`, row)
  }
  return [...latest.values()]
}

function uniqueValues(rows, getter, sorter = (a, b) => String(a).localeCompare(String(b), 'zh-CN', { numeric: true })) {
  return [...new Map(rows.map((row) => { const value = getter(row); return [String(value), value] })).values()].sort(sorter)
}

function analysisOption(value, label, selected) {
  return `<option value="${escapeHtml(value)}" ${String(value) === String(selected) ? 'selected' : ''}>${escapeHtml(label)}</option>`
}

function analysisSelect(label, id, values, selected, labeler = (value) => value) {
  return `<label><span>${escapeHtml(label)}</span><select class="input analysis-control" id="${id}">${values.map((value) => analysisOption(value, labeler(value), selected)).join('')}</select></label>`
}

function statisticBandKey(row) { return `${row.bandIndex}|${row.bandName}` }
function statisticAngleKey(row) { return `${row.vza}|${row.vaa}` }
function statisticBandLabel(key) { const [index, ...name] = String(key).split('|'); return `${name.join('|')}（图层 ${Number(index) + 1}）` }
function statisticAngleLabel(key) { const [vza, vaa] = String(key).split('|'); return `VZA ${formatColorbarValue(vza)}° / VAA ${formatColorbarValue(vaa)}°` }

function statisticQuantity(row) {
  const name = String(row?.bandName || '').toLowerCase()
  if (name.includes('reflectance') || name.includes('反射率')) return 'reflectance'
  if (name.includes('brightness temperature') || name.includes('亮温')) return 'brightnessTemperature'
  if (name.includes('radiance') || name.includes('辐亮度')) return 'radiance'
  if (name.includes('temperature') || name.includes('温度')) return 'temperature'
  return 'value'
}

const statisticQuantityLabels = {
  reflectance: '反射率 [-]', brightnessTemperature: '亮温 [K]',
  radiance: '光谱辐亮度', temperature: '温度 [K]', value: '结果值'
}

function statisticValueLabel(row) {
  return statisticQuantityLabels[statisticQuantity(row)] || '结果值'
}

function statisticTimeOrder(value) {
  const parts = simulationTimeParts(value)
  if (parts) return parts.day * 1440 + parts.hour * 60 + parts.minute
  const text = String(value || '')
  const timestamp = Date.parse(text.replaceAll('_to_', ' '))
  return Number.isFinite(timestamp) ? timestamp : Number.MAX_SAFE_INTEGER
}

function numericBandPosition(row) {
  const numbers = String(row.bandName).match(/\d+(?:\.\d+)?/g)
  return numbers?.length ? Number(numbers[numbers.length - 1]) : row.bandIndex + 1
}

function niceTicks(minimum, maximum, target) {
  const raw = (maximum - minimum) / target
  const magnitude = 10 ** Math.floor(Math.log10(raw))
  const multiple = [1, 2, 2.5, 5, 10].find((value) => value * magnitude >= raw * (1 - 1e-9))
  const step = multiple * magnitude
  const decimals = Math.max(0, -Math.floor(Math.log10(step) + 1e-9) + (multiple === 2.5 ? 1 : 0))
  const ticks = []
  for (let index = Math.ceil(minimum / step - 1e-9); index * step <= maximum + step * 1e-9; index += 1) ticks.push(index * step)
  if (ticks.length < 3 && target < 40) return niceTicks(minimum, maximum, target * 2)
  const large = Math.max(Math.abs(minimum), Math.abs(maximum)) >= 1e5
  return { ticks, format: (value) => large ? value.toExponential(2) : value.toFixed(decimals) }
}

function lineAnalysisSvg(series, xLabel, yLabel, xFormatter = formatColorbarValue) {
  const points = series.flatMap((item) => item.points).filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y))
  if (!points.length) return '<div class="analysis-empty">当前筛选条件没有可绘制的数据。</div>'
  const width = 820, height = 440, left = 76, right = 28, top = 28, bottom = 62
  let xMin = Math.min(...points.map((point) => point.x)), xMax = Math.max(...points.map((point) => point.x))
  let yMin = Math.min(...points.map((point) => point.y)), yMax = Math.max(...points.map((point) => point.y))
  const singleX = xMin === xMax ? xMin : null
  if (singleX !== null) { xMin -= .5; xMax += .5 }
  if (yMin === yMax) { const pad = Math.max(.001, Math.abs(yMin) * .05); yMin -= pad; yMax += pad }
  else { const pad = (yMax - yMin) * .08; yMin -= pad; yMax += pad }
  const x = (value) => left + (value - xMin) / (xMax - xMin) * (width - left - right)
  const y = (value) => top + (yMax - value) / (yMax - yMin) * (height - top - bottom)
  const colors = ['#246fd3', '#ef8c28', '#20a36a', '#9b5de5']
  // Ordinal axes (time) keep their own labels at evenly spaced positions.
  const xAxis = xFormatter !== formatColorbarValue ? null
    : singleX !== null ? { ticks: [singleX], format: (value) => String(Number(value.toPrecision(6))) } : niceTicks(xMin, xMax, 6)
  const xTicks = xAxis ? xAxis.ticks : Array.from({ length: 6 }, (_, index) => xMin + index / 5 * (xMax - xMin))
  const yAxis = niceTicks(yMin, yMax, 5)
  const grid = xTicks.map((value, index) => {
    const gx = x(value)
    return `<line x1="${gx}" y1="${top}" x2="${gx}" y2="${height - bottom}"/><text x="${gx}" y="${height - bottom + 24}" text-anchor="middle">${escapeHtml(xAxis ? xAxis.format(value) : xFormatter(value, index))}</text>`
  }).concat(yAxis.ticks.map((value) => {
    const gy = y(value)
    return `<line x1="${left}" y1="${gy}" x2="${width - right}" y2="${gy}"/><text x="${left - 12}" y="${gy + 4}" text-anchor="end">${escapeHtml(yAxis.format(value))}</text>`
  })).join('')
  const paths = series.map((item, seriesIndex) => {
    const sorted = [...item.points].filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y)).sort((a, b) => a.x - b.x)
    const color = colors[seriesIndex % colors.length]
    const path = sorted.map((point, index) => `${index ? 'L' : 'M'}${x(point.x).toFixed(2)},${y(point.y).toFixed(2)}`).join(' ')
    const dots = sorted.map((point) => `<circle cx="${x(point.x)}" cy="${y(point.y)}" r="4"><title>${escapeHtml(point.label || `${xFormatter(point.x)} · ${formatColorbarValue(point.y)}`)}</title></circle>`).join('')
    return `<g style="--series-color:${color}"><path class="analysis-series-line" d="${path}"/>${dots}</g>`
  }).join('')
  const legend = series.length > 1 ? `<div class="analysis-legend">${series.map((item, index) => `<span><i style="background:${colors[index % colors.length]}"></i>${escapeHtml(item.name)}</span>`).join('')}</div>` : ''
  return `<div class="analysis-chart-wrap"><svg class="analysis-chart" viewBox="0 0 ${width} ${height}" role="img"><g class="analysis-grid">${grid}</g><line class="analysis-axis" x1="${left}" y1="${height - bottom}" x2="${width - right}" y2="${height - bottom}"/><line class="analysis-axis" x1="${left}" y1="${top}" x2="${left}" y2="${height - bottom}"/><text class="analysis-axis-title" x="${(left + width - right) / 2}" y="${height - 12}" text-anchor="middle">${escapeHtml(xLabel)}</text><text class="analysis-axis-title" transform="translate(18 ${(top + height - bottom) / 2}) rotate(-90)" text-anchor="middle">${escapeHtml(yLabel)}</text>${paths}</svg>${legend}</div>`
}

function angularDistance(a, b) { return Math.abs(((Number(a) - Number(b) + 540) % 360) - 180) }

function signedPlaneZenith(row, perpendicular = false) {
  if (!Number.isFinite(row.vza) || !Number.isFinite(row.vaa)) return null
  // Time-series sun azimuths vary per node; the view planes stay at the
  // azimuth stored with that run. Without a snapshot, do not invent a plane.
  const solarAzimuth = Number.isFinite(row.planeAzimuth) ? row.planeAzimuth
    : !row.time && Number.isFinite(row.saa) ? row.saa : NaN
  if (!Number.isFinite(solarAzimuth)) return null
  if (Math.abs(row.vza) < 1e-6) return 0
  if (Math.abs(row.vza / 5 - Math.round(row.vza / 5)) > 1e-5) return null
  const positive = solarAzimuth + (perpendicular ? 90 : 0)
  if (angularDistance(row.vaa, positive) < 1e-3) return row.vza
  if (angularDistance(row.vaa, positive + 180) < 1e-3) return -row.vza
  return null
}

function polarAnalysisSvg(rows, valueLabel) {
  const normalizeAzimuth = (angle) => ((Number(angle) % 360) + 360) % 360
  const points = rows
    .filter((row) => Number.isFinite(row.vza) && row.vza >= 0 && Number.isFinite(row.vaa) && Number.isFinite(row.mean))
    .map((row) => ({ ...row, vza: Number(row.vza), vaa: normalizeAzimuth(row.vaa), mean: Number(row.mean) }))
  if (!points.length) return '<div class="analysis-empty">当前筛选条件没有半球观测数据。</div>'

  // Match TiRT-EB's angle-completion mode: average duplicate directions,
  // interpolate periodically around VAA, then interpolate radially along VZA.
  const directionKey = (vza, vaa) => Math.abs(vza) < 1e-6 ? 'nadir'
    : `${Number(vza).toFixed(8)}|${normalizeAzimuth(vaa).toFixed(8)}`
  const buckets = new Map()
  for (const point of points) {
    const key = directionKey(point.vza, point.vaa)
    const bucket = buckets.get(key) || { vza: point.vza, vaa: point.vaa, sum: 0, count: 0 }
    bucket.sum += point.mean
    bucket.count += 1
    buckets.set(key, bucket)
  }
  const averaged = [...buckets.values()].map((bucket) => ({ ...bucket, mean: bucket.sum / bucket.count }))
  const zeniths = [...new Set(averaged.map((point) => point.vza))].sort((a, b) => a - b)
  if (zeniths.length < 2) return '<div class="analysis-empty">角度补全至少需要两个观测天顶角。</div>'

  const width = 820, height = 460, cx = 410, cy = 220, radius = 178
  const minimum = Math.min(...averaged.map((row) => row.mean)), maximum = Math.max(...averaged.map((row) => row.mean))
  const range = maximum - minimum
  const radialMin = zeniths[0], radialMax = zeniths[zeniths.length - 1]
  if (!(radialMax > 0)) return '<div class="analysis-empty">角度补全需要非零的观测天顶角。</div>'

  const samplesByZenith = zeniths.map((vza) => averaged.filter((point) => Math.abs(point.vza - vza) < 1e-8).sort((a, b) => a.vaa - b.vaa))
  // A combined hemisphere + principal-plane run is still scattered data.
  // Repeated zeniths along its two cross-sections must not turn it into a grid.
  const nonNadirRings = samplesByZenith.filter((samples) => samples[0].vza > 1e-6)
  const firstRing = nonNadirRings[0] || []
  const isPolarGrid = firstRing.length > 1 && nonNadirRings.every((samples) =>
    samples.length === firstRing.length && samples.every((sample, index) =>
      angularDistance(sample.vaa, firstRing[index].vaa) < 1e-6))
  const plottedRadialMin = isPolarGrid ? radialMin : 0
  const scatteredSamples = averaged.map((point) => {
    const zenithRadians = point.vza * Math.PI / 180
    const azimuthRadians = point.vaa * Math.PI / 180
    return {
      ...point,
      direction: [
        Math.sin(zenithRadians) * Math.sin(azimuthRadians),
        Math.sin(zenithRadians) * Math.cos(azimuthRadians),
        Math.cos(zenithRadians)
      ]
    }
  })
  const interpolateAzimuth = (samples, target) => {
    if (!samples.length) return NaN
    if (samples.length === 1) return samples[0].mean
    let angle = normalizeAzimuth(target)
    if (angle < samples[0].vaa) angle += 360
    let lower = samples[samples.length - 1]
    let upper = { ...samples[0], vaa: samples[0].vaa + 360 }
    for (let index = 0; index < samples.length - 1; index += 1) {
      if (angle <= samples[index + 1].vaa) {
        lower = samples[index]
        upper = samples[index + 1]
        break
      }
    }
    const fraction = (angle - lower.vaa) / Math.max(upper.vaa - lower.vaa, 1e-9)
    return lower.mean + (upper.mean - lower.mean) * fraction
  }
  const interpolatePolarGrid = (vza, vaa) => {
    if (vza < radialMin - 1e-9 || vza > radialMax + 1e-9) return NaN
    const radialSamples = zeniths.map((zenith, index) => ({ zenith, value: interpolateAzimuth(samplesByZenith[index], vaa) })).filter((sample) => Number.isFinite(sample.value))
    if (!radialSamples.length) return NaN
    if (radialSamples.length === 1 || vza <= radialSamples[0].zenith) return radialSamples[0].value
    for (let index = 0; index < radialSamples.length - 1; index += 1) {
      if (vza <= radialSamples[index + 1].zenith) {
        const lower = radialSamples[index], upper = radialSamples[index + 1]
        const fraction = (vza - lower.zenith) / Math.max(upper.zenith - lower.zenith, 1e-9)
        return lower.value + (upper.value - lower.value) * fraction
      }
    }
    return radialSamples[radialSamples.length - 1].value
  }
  const interpolateScattered = (vza, vaa) => {
    const zenithRadians = vza * Math.PI / 180
    const azimuthRadians = normalizeAzimuth(vaa) * Math.PI / 180
    const target = [
      Math.sin(zenithRadians) * Math.sin(azimuthRadians),
      Math.sin(zenithRadians) * Math.cos(azimuthRadians),
      Math.cos(zenithRadians)
    ]
    const nearest = []
    for (const sample of scatteredSamples) {
      const cosine = Math.max(-1, Math.min(1, sample.direction[0] * target[0] + sample.direction[1] * target[1] + sample.direction[2] * target[2]))
      const distance = 1 - cosine
      if (nearest.length === 8 && distance >= nearest[nearest.length - 1].distance) continue
      let index = nearest.length
      while (index > 0 && distance < nearest[index - 1].distance) index -= 1
      nearest.splice(index, 0, { distance, value: sample.mean })
      if (nearest.length > 8) nearest.pop()
    }
    if (!nearest.length) return NaN
    if (nearest[0].distance < 1e-12) return nearest[0].value
    let weightedValue = 0, totalWeight = 0
    for (const sample of nearest) {
      const weight = 1 / Math.max(sample.distance, 1e-12)
      weightedValue += sample.value * weight
      totalWeight += weight
    }
    return totalWeight ? weightedValue / totalWeight : NaN
  }
  const interpolateField = isPolarGrid ? interpolatePolarGrid : interpolateScattered
  const colorFor = (value) => {
    const normalized = Math.max(0, Math.min(1, range ? (value - minimum) / range : .5))
    const stops = [[30, 74, 161], [245, 245, 245], [190, 28, 28]]
    const segment = normalized < .5 ? 0 : 1
    const local = segment ? (normalized - .5) * 2 : normalized * 2
    const left = stops[segment], right = stops[segment + 1]
    return `rgb(${left.map((channel, index) => Math.round(channel + (right[index] - channel) * local)).join(',')})`
  }
  const pointOnCircle = (distance, azimuth) => {
    const radians = (azimuth - 90) * Math.PI / 180
    return [cx + Math.cos(radians) * distance, cy + Math.sin(radians) * distance]
  }
  const sectorPath = (innerRadius, outerRadius, startAzimuth, endAzimuth) => {
    const [outerStartX, outerStartY] = pointOnCircle(outerRadius, startAzimuth)
    const [outerEndX, outerEndY] = pointOnCircle(outerRadius, endAzimuth)
    if (innerRadius < 1e-6) return `M${cx},${cy}L${outerStartX.toFixed(2)},${outerStartY.toFixed(2)}A${outerRadius.toFixed(2)},${outerRadius.toFixed(2)} 0 0 1 ${outerEndX.toFixed(2)},${outerEndY.toFixed(2)}Z`
    const [innerEndX, innerEndY] = pointOnCircle(innerRadius, endAzimuth)
    const [innerStartX, innerStartY] = pointOnCircle(innerRadius, startAzimuth)
    return `M${outerStartX.toFixed(2)},${outerStartY.toFixed(2)}A${outerRadius.toFixed(2)},${outerRadius.toFixed(2)} 0 0 1 ${outerEndX.toFixed(2)},${outerEndY.toFixed(2)}L${innerEndX.toFixed(2)},${innerEndY.toFixed(2)}A${innerRadius.toFixed(2)},${innerRadius.toFixed(2)} 0 0 0 ${innerStartX.toFixed(2)},${innerStartY.toFixed(2)}Z`
  }

  const radialSteps = Math.max(1, Math.ceil(radialMax - plottedRadialMin))
  const azimuthSteps = 180
  const cells = []
  for (let radialIndex = 0; radialIndex < radialSteps; radialIndex += 1) {
    const innerVza = plottedRadialMin + (radialMax - plottedRadialMin) * radialIndex / radialSteps
    const outerVza = plottedRadialMin + (radialMax - plottedRadialMin) * (radialIndex + 1) / radialSteps
    for (let azimuthIndex = 0; azimuthIndex < azimuthSteps; azimuthIndex += 1) {
      const startAzimuth = azimuthIndex * 360 / azimuthSteps
      const endAzimuth = (azimuthIndex + 1) * 360 / azimuthSteps
      const value = interpolateField((innerVza + outerVza) / 2, (startAzimuth + endAzimuth) / 2)
      if (!Number.isFinite(value)) continue
      const path = sectorPath(radius * innerVza / radialMax, radius * outerVza / radialMax, startAzimuth, endAzimuth)
      const color = colorFor(value)
      cells.push(`<path d="${path}" fill="${color}" stroke="${color}" stroke-width="0.35"/>`)
    }
  }

  const guideAngles = [radialMax / 3, radialMax * 2 / 3, radialMax]
  const guides = guideAngles.map((angle) => `<circle cx="${cx}" cy="${cy}" r="${radius * angle / radialMax}"/><text x="${cx + 5}" y="${cy - radius * angle / radialMax + 14}">${formatColorbarValue(angle)}°</text>`).join('')
  const axes = [0, 90, 180, 270].map((azimuth) => {
    const radians = (azimuth - 90) * Math.PI / 180
    const x = cx + Math.cos(radians) * radius, y = cy + Math.sin(radians) * radius
    const labels = { 0: 'N / 0°', 90: 'E / 90°', 180: 'S / 180°', 270: 'W / 270°' }
    return `<line x1="${cx}" y1="${cy}" x2="${x}" y2="${y}"/><text x="${cx + Math.cos(radians) * (radius + 28)}" y="${cy + Math.sin(radians) * (radius + 28) + 4}" text-anchor="middle">${labels[azimuth]}</text>`
  }).join('')
  return `<div class="analysis-chart-wrap"><svg class="analysis-chart analysis-polar" viewBox="0 0 ${width} ${height}" role="img" aria-label="方位角周期补全极坐标图"><g class="analysis-polar-field">${cells.join('')}</g><g class="analysis-polar-grid">${guides}${axes}</g></svg>${resultColorbar(`${valueLabel} · 整幅图像均值`, minimum, maximum, 'diverging')}</div>`
}

function scatterAnalysisSvg(sourcePoints) {
  const points = sourcePoints
    .map((point) => ({ x: Number(point[0]), y: Number(point[1]) }))
    .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y))
  if (!points.length) return '<div class="analysis-empty">没有可绘制的有效像元。</div>'
  const width = 680, height = 520, left = 78, right = 28, top = 28, bottom = 62
  const sortedValues = points.flatMap((point) => [point.x, point.y]).sort((a, b) => a - b)
  const percentile = (fraction) => {
    const position = (sortedValues.length - 1) * fraction
    const lower = Math.floor(position), upper = Math.ceil(position)
    if (lower === upper) return sortedValues[lower]
    const weight = position - lower
    return sortedValues[lower] * (1 - weight) + sortedValues[upper] * weight
  }
  let minimum = percentile(.02)
  let maximum = percentile(.98)
  if (minimum === maximum) { const pad = Math.max(.001, Math.abs(minimum) * .05); minimum -= pad; maximum += pad }
  else { const pad = (maximum - minimum) * .05; minimum -= pad; maximum += pad }
  const x = (value) => left + (value - minimum) / (maximum - minimum) * (width - left - right)
  const y = (value) => top + (maximum - value) / (maximum - minimum) * (height - top - bottom)
  const grid = Array.from({ length: 6 }, (_, index) => {
    const ratio = index / 5
    const gx = left + ratio * (width - left - right), gy = top + ratio * (height - top - bottom)
    const value = minimum + ratio * (maximum - minimum)
    const reverseValue = maximum - ratio * (maximum - minimum)
    return `<line x1="${gx}" y1="${top}" x2="${gx}" y2="${height - bottom}"/><line x1="${left}" y1="${gy}" x2="${width - right}" y2="${gy}"/><text x="${gx}" y="${height - bottom + 24}" text-anchor="middle">${escapeHtml(formatColorbarValue(value))}</text><text x="${left - 12}" y="${gy + 4}" text-anchor="end">${escapeHtml(formatColorbarValue(reverseValue))}</text>`
  }).join('')
  const dots = points.map((point) => `<circle cx="${x(point.x).toFixed(2)}" cy="${y(point.y).toFixed(2)}" r="2"><title>A ${escapeHtml(formatColorbarValue(point.x))} · B ${escapeHtml(formatColorbarValue(point.y))}</title></circle>`).join('')
  return `<div class="analysis-chart-wrap"><svg class="analysis-chart change-scatter-chart" viewBox="0 0 ${width} ${height}" role="img" aria-label="图像 A 与图像 B 像元散点图"><defs><clipPath id="changeScatterClip"><rect x="${left}" y="${top}" width="${width - left - right}" height="${height - top - bottom}"/></clipPath></defs><g class="analysis-grid">${grid}</g><line class="analysis-reference-line" x1="${x(minimum)}" y1="${y(minimum)}" x2="${x(maximum)}" y2="${y(maximum)}"/><line class="analysis-axis" x1="${left}" y1="${height - bottom}" x2="${width - right}" y2="${height - bottom}"/><line class="analysis-axis" x1="${left}" y1="${top}" x2="${left}" y2="${height - bottom}"/><text class="analysis-axis-title" x="${(left + width - right) / 2}" y="${height - 12}" text-anchor="middle">图像 A 像元值</text><text class="analysis-axis-title" transform="translate(18 ${(top + height - bottom) / 2}) rotate(-90)" text-anchor="middle">图像 B 像元值</text><g class="change-scatter-points" clip-path="url(#changeScatterClip)">${dots}</g></svg></div>`
}

function changeResultFiles() {
  return state.resultFiles.filter((file) => file.kind === 'tiff')
}

function changeFileLabel(file) {
  return file?.observationLabel ? `${file.name} · ${file.observationLabel}` : file?.name || ''
}

function changeDifferenceColorbar(limit) {
  return `<div class="result-colorbar change-colorbar" role="img" aria-label="B 减 A 差值色标"><div class="result-colorbar-title">B − A · P2–P98</div><div class="result-colorbar-scale"><div class="result-colorbar-gradient"></div><div class="result-colorbar-labels"><span>${escapeHtml(formatColorbarValue(-limit))}</span><span>0</span><span>${escapeHtml(formatColorbarValue(limit))}</span></div></div></div>`
}

function bindChangeAnalysisControls() {
  const update = (key, value) => {
    state.analysisSelection.change = {
      ...(state.analysisSelection.change || {}),
      [key]: value,
      ...(['firstPath', 'secondPath'].includes(key) ? { band: '0' } : {})
    }
    renderChangeAnalysis()
  }
  $('#changeFirst')?.addEventListener('change', (event) => update('firstPath', event.target.value))
  $('#changeSecond')?.addEventListener('change', (event) => update('secondPath', event.target.value))
  $('#changeBand')?.addEventListener('change', (event) => update('band', event.target.value))
  $('#changeSwap')?.addEventListener('click', () => {
    const selection = state.analysisSelection.change || {}
    state.analysisSelection.change = { ...selection, firstPath: selection.secondPath, secondPath: selection.firstPath }
    renderChangeAnalysis()
  })
}

async function renderChangeAnalysis() {
  disposeResultViewer()
  const requestId = ++state.changeAnalysisRequest
  const files = changeResultFiles()
  const previous = state.analysisSelection.change || {}
  const first = files.find((file) => file.path === previous.firstPath) || files[0]
  const second = files.find((file) => file.path === previous.secondPath && file.path !== first?.path)
    || files.find((file) => file.path !== first?.path)
  const pairChanged = first?.path !== previous.firstPath || second?.path !== previous.secondPath
  const band = pairChanged ? 0 : Math.max(0, Math.floor(Number(previous.band) || 0))
  state.analysisSelection.change = { firstPath: first?.path || '', secondPath: second?.path || '', band: String(band) }
  renderResultFiles()
  const fileValues = files.map((file) => file.path)
  const fileLabel = (path) => changeFileLabel(files.find((file) => file.path === path))
  const controls = `${analysisSelect('图像 A（基准）', 'changeFirst', fileValues, first?.path || '', fileLabel)}${analysisSelect('图像 B（对比）', 'changeSecond', fileValues, second?.path || '', fileLabel)}${analysisSelect('波段', 'changeBand', [band], band, (value) => `图层 ${Number(value) + 1}`)}<button class="button ghost change-swap" id="changeSwap" type="button">交换 A / B</button>`
  const preview = $('#resultPreview')
  preview.classList.add('analysis-preview')
  preview.innerHTML = `<div class="result-preview-card analysis-card change-analysis-card"><div class="analysis-controls change-controls">${controls}</div><div id="changeAnalysisOutput" class="change-analysis-output"><div class="analysis-empty">${files.length < 2 ? '至少需要两个 TIFF 图像结果。' : '正在计算逐像元差异…'}</div></div></div>`
  preview.scrollTop = 0
  bindChangeAnalysisControls()
  if (!first || !second) {
    $('#resultStatus').textContent = '变化分析至少需要两个 TIFF 图像结果'
    return
  }

  $('#resultStatus').textContent = `正在比较 ${first.name} 与 ${second.name}...`
  try {
    const result = await api.compareResults(first.path, second.path, band, 4000)
    if (requestId !== state.changeAnalysisRequest || state.analysisType !== 'change') return
    const bandSelect = $('#changeBand')
    bandSelect.innerHTML = Array.from({ length: result.bands }, (_, index) => analysisOption(index, `${result.bandNames[index] || `波段 ${index + 1}`}（图层 ${index + 1}）`, result.band)).join('')
    const correlation = Number.isFinite(result.correlation) ? formatColorbarValue(result.correlation) : '—'
    $('#changeAnalysisOutput').innerHTML = `<div class="change-metrics"><div><span>有效像元</span><strong>${Number(result.validPixels).toLocaleString('zh-CN')}</strong></div><div><span>平均差值</span><strong>${escapeHtml(formatColorbarValue(result.meanDifference))}</strong></div><div><span>MAE</span><strong>${escapeHtml(formatColorbarValue(result.mae))}</strong></div><div><span>RMSE</span><strong>${escapeHtml(formatColorbarValue(result.rmse))}</strong></div><div><span>相关系数 R</span><strong>${escapeHtml(correlation)}</strong></div><div><span>差值范围</span><strong>${escapeHtml(formatColorbarValue(result.differenceMinimum))} ～ ${escapeHtml(formatColorbarValue(result.differenceMaximum))}</strong></div></div><div class="change-figures"><section><h4>差异图（B − A）</h4><div class="change-difference-stage"><canvas class="result-canvas change-difference-canvas" id="changeDifferenceCanvas" width="${result.previewWidth}" height="${result.previewHeight}"></canvas></div>${changeDifferenceColorbar(result.differenceLimit)}</section><section><h4>像元散点图</h4>${scatterAnalysisSvg(result.scatter)}</section></div><div class="result-meta"><span>${result.width} × ${result.height}</span><span>${escapeHtml(result.bandNames[result.band] || `波段 ${result.band + 1}`)}</span><span>显示范围 P2–P98，统计使用全部有效像元</span><span>散点抽样 ${Number(result.scatter.length).toLocaleString('zh-CN')} 个</span></div>`
    const source = atob(result.differencePixels || '')
    const bytes = Uint8ClampedArray.from(source, (character) => character.charCodeAt(0))
    const canvas = $('#changeDifferenceCanvas')
    const context = canvas.getContext('2d')
    const image = context.createImageData(result.previewWidth, result.previewHeight)
    image.data.set(bytes)
    context.putImageData(image, 0, 0)
    $('#resultStatus').textContent = `变化分析 · ${result.bandNames[result.band] || `波段 ${result.band + 1}`} · B − A`
  } catch (error) {
    if (requestId !== state.changeAnalysisRequest) return
    $('#changeAnalysisOutput').innerHTML = `<div class="analysis-empty">${escapeHtml(error.message)}</div>`
    $('#resultStatus').textContent = error.message
  }
}

function bindAnalysisControls(rows) {
  $$('.analysis-control').forEach((control) => control.addEventListener('change', () => {
    state.analysisSelection[state.analysisType] = Object.fromEntries($$('.analysis-control').map((item) => [item.id, item.value]))
    renderStatisticsAnalysis(rows, state.analysisType)
  }))
}

function renderStatisticsAnalysis(rows, type = state.analysisType) {
  disposeResultViewer()
  const selection = state.analysisSelection[type] || {}
  const modes = uniqueValues(rows, (row) => row.mode)
  const expectedMode = String(state.mode || '').replace(/^e/, '')
  const mode = modes.find((value) => value.toLowerCase() === expectedMode.toLowerCase()) || modes[0]
  const modeRows = rows.filter((row) => row.mode === mode)
  const bandKeys = uniqueValues(modeRows, statisticBandKey, (a, b) => Number(a.split('|')[0]) - Number(b.split('|')[0]))
  const times = uniqueValues(modeRows, (row) => row.time, (a, b) => statisticTimeOrder(a) - statisticTimeOrder(b))
  const angleKeys = uniqueValues(modeRows, statisticAngleKey, (a, b) => Number(a.split('|')[0]) - Number(b.split('|')[0]) || Number(a.split('|')[1]) - Number(b.split('|')[1]))
  const band = bandKeys.includes(selection.analysisBand) ? selection.analysisBand : bandKeys[0]
  const time = times.includes(selection.analysisTime) ? selection.analysisTime : times[0]
  const angle = angleKeys.includes(selection.analysisAngle) ? selection.analysisAngle : angleKeys[0]
  let controls = '', chart = '', description = '', interactionHint = '鼠标停留在数据点上可查看精确值'

  if (type === 'angle') {
    const candidates = modeRows.filter((row) => statisticBandKey(row) === band && row.time === time)
    const mainRows = candidates.map((row) => ({ row, signed: signedPlaneZenith(row, false) })).filter((item) => item.signed != null)
    const perpendicularRows = candidates.map((row) => ({ row, signed: signedPlaneZenith(row, true) })).filter((item) => item.signed != null)
    const hasHemisphere = candidates[0]?.hemisphereRequested ?? (candidates.length >= 100)
    // Cross-section and nadir observations are valid hemisphere samples too.
    const hemisphereRows = hasHemisphere ? candidates : []
    const hasPlanes = candidates[0]?.principalPlaneRequested !== false
      && (mainRows.length > 1 || perpendicularRows.length > 1 || (!hasHemisphere && mainRows.length > 0))
    const available = [...(hasPlanes ? ['main'] : []), ...(hemisphereRows.length ? ['hemisphere'] : [])]
    if (!available.length) available.push('directions')
    const anglePlot = available.includes(selection.analysisAnglePlot) ? selection.analysisAnglePlot : available[0]
    const selectedBandRow = candidates[0] || modeRows.find((row) => statisticBandKey(row) === band)
    const valueLabel = statisticValueLabel(selectedBandRow)
    const plotControl = available.length > 1 ? analysisSelect('显示方式', 'analysisAnglePlot', available, anglePlot, (value) => ({ main: '主平面与垂直主平面', hemisphere: '半球极坐标 · 角度补全' }[value])) : ''
    controls = `${analysisSelect('波段', 'analysisBand', bandKeys, band, statisticBandLabel)}${analysisSelect('时刻', 'analysisTime', times, time, (value) => value ? displaySimulationTime(value) : '静态')}${plotControl}`
    if (anglePlot === 'hemisphere') {
      chart = polarAnalysisSvg(hemisphereRows, valueLabel)
      interactionHint = '方位角按 0–360° 周期补全，颜色为观测方向插值结果'
    }
    else if (anglePlot === 'directions') {
      const azimuths = uniqueValues(candidates, (row) => row.vaa, (a, b) => a - b)
      chart = lineAnalysisSvg(azimuths.map((vaa) => ({
        name: `VAA ${vaa}°`,
        points: candidates.filter((row) => row.vaa === vaa).map((row) => ({
          x: row.vza, y: row.mean,
          label: `VZA ${row.vza}° / VAA ${row.vaa}° · ${valueLabel}均值 ${formatColorbarValue(row.mean)}`
        }))
      })), '观测天顶角 [°]', `${valueLabel} · 整幅图像均值`)
      interactionHint = '按结果中的实际观测方位角分组；未将这些方向标为太阳主平面'
    }
    else {
      const planes = [{ name: '太阳主平面', rows: mainRows }, { name: '垂直主平面', rows: perpendicularRows }]
      chart = lineAnalysisSvg(planes.filter((plane) => plane.rows.length).map((plane) => ({
        name: plane.name,
        points: plane.rows.map(({ row, signed }) => ({ x: signed, y: row.mean, label: `${plane.name} · 有符号天顶角 ${signed}° · VZA ${row.vza}° / VAA ${row.vaa}° · ${valueLabel}均值 ${formatColorbarValue(row.mean)}` }))
      })), '有符号观测天顶角 [°]', `${valueLabel} · 整幅图像均值`)
      interactionHint = '两条曲线按该次运行设置的太阳方位角及其 +90° 划分；时序运行保持观测平面固定，悬停查看实际 VZA / VAA'
    }
    description = `角度分析 · ${statisticBandLabel(band)} · ${time ? displaySimulationTime(time) : '静态'} · 整幅图像均值`
  } else if (type === 'band') {
    const anglesAtTime = uniqueValues(modeRows.filter((row) => row.time === time), statisticAngleKey, (a, b) => Number(a.split('|')[0]) - Number(b.split('|')[0]) || Number(a.split('|')[1]) - Number(b.split('|')[1]))
    const selectedAngle = anglesAtTime.includes(angle) ? angle : anglesAtTime[0]
    const candidates = modeRows.filter((row) => row.time === time && statisticAngleKey(row) === selectedAngle)
    controls = `${analysisSelect('观测角', 'analysisAngle', anglesAtTime, selectedAngle, statisticAngleLabel)}${analysisSelect('时刻', 'analysisTime', times, time, (value) => value ? displaySimulationTime(value) : '静态')}`
    const wavelengthRanges = [
      { label: '光学波段（≤ 2500 nm）', rows: candidates.filter((row) => numericBandPosition(row) <= 2500) },
      { label: '热红外波段（> 2500 nm）', rows: candidates.filter((row) => numericBandPosition(row) > 2500) }
    ].filter((range) => range.rows.length)
    chart = wavelengthRanges.map((range) => {
      const quantities = uniqueValues(range.rows, statisticQuantity)
      const valueLabel = quantities.length === 1 ? (statisticQuantityLabels[quantities[0]] || '结果值') : '结果值'
      const figure = lineAnalysisSvg([{ name: range.label, points: range.rows.map((row) => ({ x: numericBandPosition(row), y: row.mean, label: `${row.bandName} · 整幅图像均值 ${formatColorbarValue(row.mean)}` })) }], '波长 [nm]', `${valueLabel} · 整幅图像均值`)
      return `<section class="analysis-chart-section"><h4>${range.label}</h4>${figure}</section>`
    }).join('')
    description = `波段分析 · ${statisticAngleLabel(selectedAngle)} · ${time ? displaySimulationTime(time) : '静态'} · 整幅图像均值`
  } else {
    const selectedRows = modeRows.filter((row) => statisticBandKey(row) === band && statisticAngleKey(row) === angle).sort((a, b) => statisticTimeOrder(a.time) - statisticTimeOrder(b.time))
    const selectedBandRow = selectedRows[0] || modeRows.find((row) => statisticBandKey(row) === band)
    const valueLabel = statisticValueLabel(selectedBandRow)
    controls = `${analysisSelect('波段', 'analysisBand', bandKeys, band, statisticBandLabel)}${analysisSelect('观测角', 'analysisAngle', angleKeys, angle, statisticAngleLabel)}`
    chart = lineAnalysisSvg([{ name: '时间变化', points: selectedRows.map((row) => ({ x: statisticTimeOrder(row.time), y: row.mean, label: `${displaySimulationTime(row.time)} · ${valueLabel}均值 ${formatColorbarValue(row.mean)}` })) }], '模拟时刻', `${valueLabel} · 整幅图像均值`, (value) => {
      const row = selectedRows.reduce((closest, item) => !closest || Math.abs(statisticTimeOrder(item.time) - value) < Math.abs(statisticTimeOrder(closest.time) - value) ? item : closest, null)
      return row ? displaySimulationTime(row.time) : ''
    })
    description = `时间分析 · ${statisticBandLabel(band)} · ${statisticAngleLabel(angle)} · 整幅图像均值`
  }
  const preview = $('#resultPreview')
  preview.classList.add('analysis-preview')
  preview.innerHTML = `<div class="result-preview-card analysis-card"><div class="analysis-controls">${controls}</div>${chart}<div class="result-meta"><span>${escapeHtml(description)}</span><span>${escapeHtml(interactionHint)}</span></div></div>`
  preview.scrollTop = 0
  requestAnimationFrame(() => { preview.scrollTop = 0 })
  bindAnalysisControls(rows)
}

function drawStatisticsAnalysis(result) {
  const rows = statisticsRows(result.content, result.observation)
  if (!rows.length) {
    emptyResultPreview('统计结果为空', '请先运行图像模拟，系统会自动生成带模型后缀的统计 CSV。')
    return
  }
  renderStatisticsAnalysis(rows, state.analysisType)
}


async function openResultFile(file, band = 0) {
  if (!file) return
  const openRequest = ++state.resultOpenRequest
  const stale = () => openRequest !== state.resultOpenRequest || $('#resultDialog').hidden
  try {
    const displayName = resultFileDisplayName(file)
    state.selectedResult = file
    $$('.result-file').forEach((button) => button.classList.toggle('active', Number(button.dataset.index) === state.resultFiles.indexOf(file)))
    $('#resultStatus').textContent = `正在读取 ${displayName}...`
    let result = await api.readResult(file.path, band)
    if (stale()) return
    if (!result.kind && Number.isInteger(Number(result.facetCount)) &&
        Array.isArray(result.radiosity) && Array.isArray(result.lightEnhancement) &&
        Array.isArray(result.sunlit)) {
      const metrics = [
        { id: 'radiosity', label: 'Radiosity', values: result.radiosity },
        { id: 'lightEnhancement', label: 'Light enhancement', values: result.lightEnhancement },
        { id: 'sunlit', label: 'Sunlit fraction', values: result.sunlit }
      ]
      delete result.radiosity; delete result.lightEnhancement; delete result.sunlit
      result = { ...result, kind: 'facet', path: file.path, metrics }
    }
    if (result.kind === 'envi') await drawEnviPreview(result, file)
    else if (result.kind === 'wind') drawWindPreview(result, file)
    else if (result.kind === 'tiff') await drawTiffPreview(result, file)
    else if (result.kind === 'facet') showFacet3D(result, file)
    else if (result.kind === 'voxel') showVoxel3D(result, file)
    else if (result.kind === 'image') await drawImagePreview(result, file)
    else if (result.kind === 'text' && state.resultView === 'analysis' && state.analysisType !== '3d') drawStatisticsAnalysis(result)
    else if (result.kind === 'text') { disposeResultViewer(); state.resultRaster = null; $('#resultPreview').innerHTML = `<div class="result-preview-card"><pre class="result-text">${escapeHtml(result.content)}</pre><button class="button ghost result-open-external" type="button">用系统程序打开</button></div>` }
    else { disposeResultViewer(); state.resultRaster = null; $('#resultPreview').innerHTML = `<div class="empty-editor"><svg><use href="#i-layers"/></svg><h3>暂不支持内置预览</h3><p>可在资源管理器中定位并使用外部程序打开该文件。</p><button class="button ghost result-open-external" type="button">用系统程序打开</button></div>` }
    $('#resultPreview').querySelector('.result-open-external')?.addEventListener('click', () => api.openPath(file.path))
    $('#resultStatus').textContent = result.kind === 'envi' ? `ENVI 浮点影像 · ${displayName}` : result.kind === 'wind' ? `自适应风速箭头图 · ${displayName}` : result.kind === 'tiff' ? `TIFF 栅格影像 · ${displayName}` : result.kind === 'facet' ? `面元结果 · ${displayName}` : result.kind === 'text' && state.resultView === 'analysis' ? `${({ angle: '角度', band: '波段', time: '时间' })[state.analysisType] || ''}统计分析 · ${displayName}` : result.kind === 'image' ? `图像结果 · ${displayName}` : displayName
  } catch (error) {
    if (stale()) return
    emptyResultPreview('结果读取失败', error.message)
    $('#resultStatus').textContent = error.message
  }
}

function threeDimensionalOutputEnabled() {
  const energyMode = state.mode === 'eFacetEB' || state.mode === 'eVoxelEB'
  return energyMode
    ? Boolean(state.config?.sensor?.radiationProcess || state.config?.sensor?.energyProcess)
    : Boolean(state.config?.sensor?.radiationProcess)
}

function resultFilesForView() {
  if (state.resultView === 'image') return state.resultFiles.filter((file) => ['tiff', 'envi', 'image'].includes(file.kind))
  if (state.analysisType === 'change') return changeResultFiles()
  if (state.analysisType === '3d') return threeDimensionalOutputEnabled() ? state.resultFiles.filter((file) => ['facet', 'process'].includes(file.kind)) : []
  return state.resultFiles.filter((file) => file.kind === 'text' && /(?:^|[\\/])result_statistics(?:_[a-z0-9-]+)?\.csv$/i.test(file.path))
}

function resultFileDisplayName(file) {
  if (file?.kind === 'process' && file.processType === 'fluid') {
    return `体元流体力学 · ${file.processTime || '静态'}`
  }
  return file?.name || ''
}

function updateResultViewHeader() {
  const analysis = state.resultView === 'analysis'
  $('#resultDialogTitle').textContent = analysis ? '分析' : '图像结果'
  $('#resultImageTab').classList.toggle('active', !analysis)
  $('#resultAnalysisTab').classList.toggle('active', analysis)
  $('#resultAnalysisTabs').hidden = !analysis
  const tabs = { angle: '#angleAnalysisTab', band: '#bandAnalysisTab', time: '#timeAnalysisTab', change: '#changeAnalysisTab', '3d': '#threeAnalysisTab' }
  for (const [type, selector] of Object.entries(tabs)) $(selector).classList.toggle('active', analysis && state.analysisType === type)
}

async function setResultView(view, openFirst = true) {
  disposeResultViewer()
  state.resultRaster = null
  state.resultView = view === 'analysis' ? 'analysis' : 'image'
  state.selectedResult = null
  updateResultViewHeader()
  renderResultFiles()
  if (state.resultView === 'analysis' && state.analysisType === 'change') {
    if (openFirst) await renderChangeAnalysis()
    return
  }
  const first = resultFilesForView()[0]
  if (openFirst && first) await openResultFile(first)
}

async function setAnalysisType(type, openFirst = true) {
  disposeResultViewer()
  state.resultRaster = null
  state.resultView = 'analysis'
  state.analysisType = ['angle', 'band', 'time', 'change', '3d'].includes(type) ? type : 'angle'
  state.selectedResult = null
  updateResultViewHeader()
  renderResultFiles()
  if (state.analysisType === 'change') {
    if (openFirst) await renderChangeAnalysis()
    return
  }
  const first = resultFilesForView()[0]
  if (openFirst && first) await openResultFile(first)
}

function renderResultFiles() {
  const visibleFiles = resultFilesForView()
  if (!visibleFiles.length) {
    const analysis = state.resultView === 'analysis'
    const is3d = analysis && state.analysisType === '3d'
    const processDisabled = is3d && !threeDimensionalOutputEnabled()
    const analysisLabel = ({ angle: '角度', band: '波段', time: '时间', change: '变化', '3d': '三维' })[state.analysisType]
    const requirement = processDisabled
      ? '请在仿真模拟中启用对应的过程输出。'
      : analysis
        ? ({ angle: '需要“图像输出”和统计 CSV，且至少两个观测角度。', band: '需要“图像输出”和统计 CSV，且至少两个波段。', time: '需要“图像输出”和统计 CSV，且至少两个时间节点。', change: '需要输出目录中至少两幅 TIFF 图像结果。', '3d': '需要过程输出（辐射过程或能量过程）。' })[state.analysisType]
        : '请先运行模拟，然后点击刷新。'
    document.querySelector('#resultFiles').innerHTML = `<div class="result-empty-list">${processDisabled ? '当前模式未启用过程输出。' : analysis ? `尚无${analysisLabel}分析结果。` : '尚无图像结果。'}<br>${requirement}</div>`
    emptyResultPreview(processDisabled ? '尚无三维结果' : analysis ? `尚无${analysisLabel}分析数据` : '尚无图像结果', requirement)
    return
  }
  $('#resultFiles').innerHTML = visibleFiles.map((file) => {
    const index = state.resultFiles.indexOf(file)
    const displayName = t(resultFileDisplayName(file))
    const changeSelection = state.analysisSelection.change || {}
    const changeRole = state.resultView === 'analysis' && state.analysisType === 'change'
      ? (file.path === changeSelection.firstPath ? 'A' : file.path === changeSelection.secondPath ? 'B' : '') : ''
    const extension = changeRole || (file.resultType === 'wind' ? 'WIND' : file.kind === 'envi' ? 'ENVI' : file.kind === 'tiff' ? 'TIFF' : file.kind === 'facet' ? 'FACET' : file.kind === 'text' ? 'CSV' : file.name.split('.').pop())
    const time = new Date(file.modifiedAt).toLocaleString('zh-CN', { hour12: false })
    return `<button class="result-file ${changeRole ? 'active' : ''}" data-index="${index}"><span class="result-file-icon">${escapeHtml(extension)}</span><span><strong title="${escapeHtml(displayName)}">${escapeHtml(displayName)}</strong><small>${file.observationLabel ? escapeHtml(file.observationLabel.replaceAll('_to_', ' → ').replaceAll('_', ' ')) + ' · ' : ''}${formatBytes(file.size)} · ${time}</small></span></button>`
  }).join('')
  $$('.result-file').forEach((button) => button.addEventListener('click', () => {
    const file = state.resultFiles[Number(button.dataset.index)]
    if (state.resultView === 'analysis' && state.analysisType === 'change') {
      const selection = state.analysisSelection.change || {}
      if (file.path !== selection.firstPath) state.analysisSelection.change = { ...selection, secondPath: file.path, band: '0' }
      renderResultFiles()
      renderChangeAnalysis()
    } else openResultFile(file)
  }))
}

async function refreshResults(openFirst = false) {
  if (!state.outputDir) throw new Error('当前工程未配置输出目录')
  const result = await api.listResults(state.outputDir)
  state.outputDir = result.directory
  state.resultFiles = result.files
  $('#resultDirectory').textContent = result.directory
  renderResultFiles()
  if (state.resultView === 'analysis' && state.analysisType === 'change') {
    await renderChangeAnalysis()
    return
  }
  const visibleFiles = resultFilesForView()
  const selected = state.selectedResult && visibleFiles.find((file) => file.path === state.selectedResult.path)
  if (selected) await openResultFile(selected)
  else if (openFirst && visibleFiles.length) await openResultFile(visibleFiles[0])
}

async function deleteAllResults() {
  try {
    const kind = state.resultView === 'analysis' && state.analysisType === '3d' ? '3d' : 'image'
    const result = await api.deleteResults(state.outputDir, kind)
    disposeResultViewer()
    state.selectedResult = null
    state.resultRaster = null
    await refreshResults(false)
    $('#resultStatus').textContent = '已删除 ' + result.deleted + (kind === '3d' ? ' 个三维输出' : ' 个 TIFF 图像结果')
    addLog('已删除 ' + result.deleted + (kind === '3d' ? ' 个三维输出' : ' 个 TIFF 图像结果'), 'success')
    toast(kind === '3d' ? '三维结果已删除' : '图像结果已删除', result.deleted + ' 个文件')
  } catch (error) {
    toast('删除结果失败', error.message, 'error')
  }
}

async function showResults(view = 'image', analysisType = null) {
  if (!state.inputPath) { toast('无法查看结果', '请先新建或打开工程', 'error'); return }
  $('#resultDialog').hidden = false
  if (view === '3d') { state.resultView = 'analysis'; state.analysisType = '3d' }
  else if (view === 'analysis') { state.resultView = 'analysis'; if (analysisType) state.analysisType = analysisType }
  else state.resultView = 'image'
  state.selectedResult = null
  updateResultViewHeader()
  try {
    await refreshResults(true)
  } catch (error) {
    state.resultFiles = []
    $('#resultFiles').innerHTML = '<div class="result-empty-list">无法读取输出目录。</div>'
    emptyResultPreview('无法打开模拟结果', error.message)
    $('#resultStatus').textContent = error.message
  }
}

function hideResults() {
  state.resultOpenRequest += 1
  disposeResultViewer()
  state.resultRaster = null
  $('#resultDialog').hidden = true
}

function selectMode(mode, announce = false) {
  const button = $(`.mode-card[data-mode="${mode}"]`)
  if (!button) return
  const previousMode = state.mode
  const facetMode = previousMode === 'eFacetRT' || previousMode === 'eFacetEB' || previousMode === 'eRaytracing'
  const voxelMode = mode === 'eVoxelRT' || mode === 'eVoxelEB'
  const converted = facetMode && voxelMode
  state.mode = mode
  $$('.mode-card').forEach((item) => item.classList.toggle('active', item === button))
  renderNavBadges()
  if (state.project) {
    state.project.mode = mode
    if (converted) state.project.voxelization = { sourceMode: previousMode, targetMode: mode, voxelSize: Number(state.config?.scene?.voxel) || 1, updatedAt: new Date().toISOString() }
  }
  if (converted) {
    $('#voxelBtn').classList.add('active')
    if (voxelPreview) voxelPreview.visible = true
  }
  if (announce) {
    remapGpuIndex(previousMode, mode)
    if (!state.running) setRunning(false)
    if (state.project) { markXmlDirty(); $('#unsavedMark').hidden = false }
    addLog(converted ? `面元场景已接入${button.querySelector('span').textContent}，体元大小 ${Number(state.config?.scene?.voxel) || 1} m` : `计算模式切换为 ${button.querySelector('span').textContent}`)
    if (converted) toast('面元 → 体元通道已启用', 'OBJ、材质和位置保持不变，运行时生成稀疏体元')
    renderInspector()
  }
}

function showProjectDialog() {
  $('#newProjectMode').value = state.mode
  $('#projectDirectory').value = appStorage.getItem('histreamProjectDirectory') || ''
  $('#projectDialog').hidden = false
  setTimeout(() => ($('#projectDirectory').value ? $('#newProjectName') : $('#projectDirectory')).focus(), 0)
}

function hideProjectDialog() { $('#projectDialog').hidden = true }

function showSaveAsDialog() {
  if (!state.project || !state.inputPath) {
    toast('无法另存为', '请先新建或打开工程', 'error')
    return
  }
  if (state.running) {
    toast('无法另存为', '模拟运行中，请停止后再试', 'error')
    return
  }
  const sourceDirectory = state.inputPath.replace(/[\\/][^\\/]+$/, '')
  const parentDirectory = sourceDirectory.replace(/[\\/][^\\/]+$/, '') || sourceDirectory
  $('#saveAsDirectory').value = parentDirectory
  $('#saveAsName').value = `${state.project.name || fileName(sourceDirectory)}_copy`
  $('#saveAsSourcePath').textContent = sourceDirectory
  $('#saveAsDialog').hidden = false
  setTimeout(() => { $('#saveAsName').focus(); $('#saveAsName').select() }, 0)
}

function hideSaveAsDialog() { $('#saveAsDialog').hidden = true }

function showReferenceDialog() {
  $('#referenceDialog').hidden = false
  $('#closeReferenceBtn').focus()
}

function hideReferenceDialog() {
  $('#referenceImageDialog').hidden = true
  $('#referenceImage').removeAttribute('src')
  $('#referenceDialog').hidden = true
}

function hideHistreamProxyDialog() {
  $('#histreamProxyDialog').hidden = true
}

function selectedHistreamProxyFrame() {
  const value = $('#histreamProxyFrameSelect').value
  if (!value) throw new Error('请选择训练捕获帧')
  const [episodeId, frameIndex] = JSON.parse(value)
  if (!episodeId || !Number.isInteger(frameIndex)) throw new Error('捕获帧选项无效')
  return { episodeId, frameIndex }
}

function showHistreamProxyBand() {
  const result = histreamProxyState.result
  if (!result) return
  const band = $('#histreamProxyBandSelect').value
  const preview = result.previews?.[band]
  if (!preview) return
  const index = result.band_names.indexOf(band)
  const units = result.band_units[index] || ''
  const image = $('#histreamProxyPreview')
  image.src = preview
  image.alt = `${band} ${units} preview for ${result.episode_id} frame ${result.frame_index}`
  image.hidden = false
  $('#histreamProxyPreviewTitle').textContent = `${band} · ${units} · ${result.shape[1]} × ${result.shape[0]}`
}

async function openHistreamProxyDialog() {
  const dialog = $('#histreamProxyDialog')
  dialog.hidden = false
  $('#closeHistreamProxyBtn').focus()
  $('#histreamProxyScope').textContent = '正在检查 histream-proxy 部署清单…'
  $('#histreamProxyStatus').textContent = '正在读取固定场景和模型状态。'
  $('#histreamProxyFrameSelect').disabled = true
  $('#runHistreamProxyBtn').disabled = true
  $('#downloadHistreamProxyBtn').hidden = true
  $('#histreamProxyPreview').hidden = true
  $('#histreamProxyPreview').removeAttribute('src')
  $('#histreamProxyRuntime').textContent = ''
  histreamProxyState.info = null
  histreamProxyState.result = null
  try {
    const info = await api.histreamProxyInfo()
    histreamProxyState.info = info
    if (!info.ready) {
      $('#histreamProxyScope').textContent = `代理尚未就绪：${info.reason || '部署清单或运行文件缺失。'}`
      $('#histreamProxyStatus').textContent = '未启动任何模拟引擎。请先完成训练导出和代理部署。'
      return
    }
    const frames = Array.isArray(info.supportedFrames) ? info.supportedFrames : []
    const select = $('#histreamProxyFrameSelect')
    select.replaceChildren()
    for (const frame of frames) {
      const option = document.createElement('option')
      option.value = JSON.stringify([frame.episode_id, Number(frame.frame_index)])
      option.textContent = `${frame.episode_id} · 帧 ${frame.frame_index} · ${frame.hour_bjt}:00 · ${frame.platform} · ${frame.split}`
      select.append(option)
    }
    select.disabled = frames.length === 0
    $('#runHistreamProxyBtn').disabled = frames.length === 0
    $('#histreamProxyScope').textContent = `${info.captureScope}。模型输入为训练时生成的完整三维体及观测几何；当前普通 project.json 不参与推理。共 ${frames.length} 个已登记帧，输出 ${info.shape?.join(' × ')}，波段：${info.bandNames?.join(' / ')}。`
    $('#histreamProxyStatus').textContent = frames.length ? '就绪。选择登记帧后生成数值影像。' : '部署清单没有可用帧。'
    $('#histreamProxyRuntime').textContent = `backend=${info.backend} · Python=${info.pythonExecutable} · runner=${info.runnerPath} · nativeEngineInvoked=${info.nativeEngineInvoked}`
  } catch (error) {
    $('#histreamProxyScope').textContent = `无法读取代理部署：${error.message}`
    $('#histreamProxyStatus').textContent = '代理检查失败；标准引擎状态未改变。'
  }
}

async function runHistreamProxyFrame() {
  const button = $('#runHistreamProxyBtn')
  const select = $('#histreamProxyFrameSelect')
  button.disabled = true
  $('#histreamProxyStatus').textContent = '正在遍历完整三维体并合成波段…'
  $('#downloadHistreamProxyBtn').hidden = true
  try {
    const frame = selectedHistreamProxyFrame()
    const result = await api.runHistreamProxy({
      captureId: histreamProxyState.info?.captureId,
      episodeId: frame.episodeId,
      frameIndex: frame.frameIndex
    })
    if (result.status !== 'ok' || result.backend !== 'histream-proxy' || result.native_engine_invoked !== false) {
      throw new Error('代理返回的状态或后端标识无效')
    }
    histreamProxyState.result = result
    showHistreamProxyBand()
    const download = $('#downloadHistreamProxyBtn')
    download.href = result.downloadUrl
    download.download = `${result.download_id}.npz`
    download.hidden = false
    $('#histreamProxyStatus').textContent = `完成：${result.episode_id} 帧 ${result.frame_index} · ${result.shape.join(' × ')} · 命中 ${result.hit_pixels.toLocaleString()} 像素 · 未调用 histream.exe。`
    $('#histreamProxyRuntime').textContent = `split=${result.split} · route=${result.route_id} · thermal=${result.thermal_state} · solar=${result.solar_source || 'engine SPA'} · nativeEngineInvoked=${result.native_engine_invoked}`
  } catch (error) {
    $('#histreamProxyStatus').textContent = `预测失败：${error.message}`
    $('#histreamProxyPreview').hidden = true
    toast('histream-proxy 失败', error.message, 'error')
  } finally {
    button.disabled = !histreamProxyState.info?.ready || !select.value
  }
}

function showReferenceImage(button) {
  const image = button.querySelector('img')
  if (!image) return
  $('#referenceImage').src = image.src
  $('#referenceImage').alt = image.alt
  $('#referenceImageTitle').textContent = button.dataset.referenceTitle || image.alt || '图例预览'
  $('#referenceImageDialog').hidden = false
  $('#closeReferenceImageBtn').focus()
}

function hideReferenceImage() {
  $('#referenceImageDialog').hidden = true
  $('#referenceImage').removeAttribute('src')
}

async function saveProjectAs(event) {
  event.preventDefault()
  const submit = $('#saveAsForm button[type="submit"]')
  const values = Object.fromEntries(new FormData($('#saveAsForm')).entries())
  flushProjectTextSync()
  const generation = state.projectGeneration
  const revision = state.editRevision
  const sourcePath = state.inputPath
  const isCurrent = () => state.projectGeneration === generation && state.inputPath === sourcePath
  try {
    submit.disabled = true
    const config = structuredClone(state.xmlManualEdit || !state.config ? ensureMaterialPresets(parseXml(state.xmlText, state.mode)) : state.config)
    const project = structuredClone(state.project)
    project.configuration = config
    project.mode = state.mode
    await ensureDemInfo(config, sourcePath)
    if (!isCurrent()) return
    const result = await api.saveProjectAs({ ...values, sourcePath, project })
    if (!isCurrent() || state.editRevision !== revision) { addLog(`提交时的工程已另存为：${result.projectDir}；当前工程或后续修改保持不变`, 'warning'); reportUsdSave(result.usd); return }
    appStorage.setItem('histreamProjectDirectory', values.directory)
    hideSaveAsDialog()
    loadXml(result)
    addLog(`工程已另存为：${result.projectDir}`, 'success')
    reportUsdSave(result.usd)
    toast('另存为成功', result.project.name)
  } catch (error) {
    toast('另存为失败', error.message, 'error')
    addLog(`工程另存为失败：${error.message}`, 'error')
  } finally { submit.disabled = false }
}

async function createProject(event) {
  event.preventDefault()
  const submit = $('#projectForm button[type="submit"]')
  const values = Object.fromEntries(new FormData($('#projectForm')).entries())
  try {
    submit.disabled = true
    const result = await api.createProject(values)
    appStorage.setItem('histreamProjectDirectory', values.directory)
    loadXml(result)
    hideProjectDialog()
    addLog(`已创建工程：${result.projectDir}`, 'success')
    toast('工程创建成功', result.project.name)
  } catch (error) { toast('创建工程失败', error.message, 'error') }
  finally { submit.disabled = false }
}

$$('.mode-card').forEach((button) => button.addEventListener('click', () => selectMode(button.dataset.mode, true)))
$('#computeDevice').addEventListener('change', (event) => {
  const control = state.config?.control
  if (!control || !event.target.value) return
  const value = event.target.value
  const backend = value.startsWith('gpu:') ? 'gpu' : value
  if (backend === 'gpu') control.gpu = Number(value.slice(4))
  control.computeBackend = backend
  control.facetBackend = backend
  updateXml()
  renderComputeDevices()
  addLog(`计算设备：${event.target.selectedOptions[0]?.textContent || value}`)
})
$('#cpuThreads').addEventListener('change', (event) => {
  const control = state.config?.control
  if (!control) return
  control.cpuThreads = normalizeCpuThreads(event.target.value)
  updateXml()
  renderComputeDevices()
})
$('#refreshDevicesBtn').addEventListener('click', () => loadComputeDevices(true))
$$('.tree-item').forEach((button) => button.addEventListener('click', () => { state.panel = button.dataset.panel; $$('.tree-item').forEach((item) => item.classList.toggle('active', item === button)); renderInspector() }))
$$('#viewStyle button').forEach((button) => button.addEventListener('click', () => { $$('#viewStyle button').forEach((item) => item.classList.toggle('active', item === button)); applyViewStyle(button.dataset.style) }))
$('#gridBtn').addEventListener('click', () => { if (!grid) return; grid.visible = !grid.visible; $('#gridBtn').classList.toggle('active', grid.visible) })
$('#voxelBtn').addEventListener('click', () => { if (!voxelPreview) return; voxelPreview.visible = !voxelPreview.visible; $('#voxelBtn').classList.toggle('active', voxelPreview.visible) })
$('#sceneObjectToolsBtn').addEventListener('click', (event) => {
  event.stopPropagation()
  const menu = $('#sceneObjectToolsMenu')
  if (menu.hidden) {
    menu.hidden = false
    $('#sceneObjectToolsBtn').setAttribute('aria-expanded', 'true')
  } else closeSceneObjectToolsMenu()
})
$('#sceneObjectToolsMenu').addEventListener('click', (event) => event.stopPropagation())
$('#sceneObjectModifyBtn').addEventListener('click', () => toggleSceneObjectSubmenu('Modify'))
$('#sceneObjectAddBtn').addEventListener('click', () => toggleSceneObjectSubmenu('Add'))
$('#drawRegionBtn').addEventListener('click', () => startGeometryDraw('region'))
$('#drawBoxBtn').addEventListener('click', () => startGeometryDraw('box'))
$('#editObjectBtn').addEventListener('click', () => {
  if (!objectEdit.enabled) setObjectEditEnabled(true)
  else { clearObjectSelection(); closeSceneObjectToolsMenu() }
})
document.addEventListener('pointerdown', (event) => {
  if (!event.target.closest?.('.scene-object-tools')) closeSceneObjectToolsMenu()
})
$$('#transformModeButtons button').forEach((button) => button.addEventListener('click', () => {
  if (!objectEdit.enabled) setObjectEditEnabled(true)
  setObjectTransformMode(button.dataset.transformMode)
}))
transformControls.addEventListener('dragging-changed', (event) => {
  controls.enabled = !event.value
  if (event.value) objectEdit.pointerStart = null
})
transformControls.addEventListener('mouseDown', () => {
  if (objectEdit.selected) objectEdit.transformStartScale.copy(objectEdit.selected.scale)
})
transformControls.addEventListener('objectChange', () => {
  const object = objectEdit.selected
  if (!object) return
  if (transformControls.mode === 'scale') {
    const start = objectEdit.transformStartScale
    const ratios = {
      X: object.scale.x / Math.max(1e-9, start.x),
      Y: object.scale.y / Math.max(1e-9, start.y),
      Z: object.scale.z / Math.max(1e-9, start.z)
    }
    const axis = String(transformControls.axis || '')
    const factor = Math.max(.01, Number.isFinite(ratios[axis]) ? ratios[axis] : Math.max(ratios.X, ratios.Y, ratios.Z))
    object.scale.copy(start).multiplyScalar(factor)
  }
  snapGroundedObject(object)
  updateObjectSelectionBox()
})
transformControls.addEventListener('mouseUp', () => saveEditedObjectTransform(objectEdit.selected))
renderer.domElement.addEventListener('pointerdown', onGeometryDrawPointerDown, true)
renderer.domElement.addEventListener('pointermove', onGeometryDrawPointerMove, true)
renderer.domElement.addEventListener('pointerup', onGeometryDrawPointerUp, true)
renderer.domElement.addEventListener('pointerdown', onObjectEditPointerDown)
renderer.domElement.addEventListener('pointerup', onObjectEditPointerUp)
renderer.domElement.addEventListener('dblclick', onObjectEditDoubleClick)
renderer.domElement.addEventListener('pointercancel', () => { if (geometryDraw.mode) cancelGeometryDraw('绘制已取消') }, true)
renderer.domElement.addEventListener('contextmenu', (event) => {
  if (!geometryDraw.mode) return
  event.preventDefault()
  cancelGeometryDraw('绘制已取消')
}, true)
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && geometryDraw.mode) cancelGeometryDraw('绘制已取消')
})
window.addEventListener('keydown', (event) => {
  if (!objectEdit.enabled) return
  const target = event.target
  if (target instanceof HTMLElement && (target.matches('input, textarea, select') || target.isContentEditable)) return
  const key = event.key.toLowerCase()
  if (key === 'w') setObjectTransformMode('translate')
  else if (key === 'e') setObjectTransformMode('rotate')
  else if (key === 'r') setObjectTransformMode('scale')
  else if (event.key === 'Escape' && $('#objectAttributeDialog').hidden && $('#objectDialog').hidden) {
    event.preventDefault()
    event.stopImmediatePropagation()
    if (objectEdit.selected) clearObjectSelection()
    else setObjectEditEnabled(false)
  }
})
// FIXED[低|错误处理]: 本行中停止按钮 () => api.stop() 的 Promise 没有 catch，停止失败（网络/进程已退出）时无提示且为未处理 rejection；已修复：补 .catch 并 toast + addLog 提示
$('#fitBtn').addEventListener('click', fitCamera); $('#openXmlBtn').addEventListener('click', chooseXml); $('#drawerOpenBtn').addEventListener('click', chooseXml); $('#saveXmlBtn').addEventListener('click', saveXml); $('#saveAsBtn').addEventListener('click', showSaveAsDialog); $('#runBtn').addEventListener('click', runSimulation); $('#stopBtn').addEventListener('click', () => { api.stop().catch((error) => { toast('无法停止模拟', error.message, 'error'); addLog(`停止失败：${error.message}`, 'error') }) }); $('#resetBtn').addEventListener('click', resetSimulation)
$('#openExampleBtn').addEventListener('click', showExampleDialog)
$('#exampleForm').addEventListener('submit', openExampleFromDialog)
$('#exampleSelect').addEventListener('change', renderExampleSummary)
$('#cancelExampleBtn').addEventListener('click', hideExampleDialog)
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && !$('#exampleDialog').hidden) hideExampleDialog()
})
$('#newProjectBtn').addEventListener('click', showProjectDialog)
$('#closeProjectBtn').addEventListener('click', hideProjectDialog)
$('#projectDialog').addEventListener('click', (event) => { if (event.target === $('#projectDialog')) hideProjectDialog() })
$('#projectForm').addEventListener('submit', createProject)
$('#closeSaveAsBtn').addEventListener('click', hideSaveAsDialog)
// 保留编辑内容：点击窗外不关闭，只能取消或保存成功后关闭。
$('#saveAsForm').addEventListener('submit', saveProjectAs)
$('#referenceBtn').addEventListener('click', showReferenceDialog)
$('#closeReferenceBtn').addEventListener('click', hideReferenceDialog)
$('#referenceDialog').addEventListener('click', (event) => { if (event.target === $('#referenceDialog')) hideReferenceDialog() })
$('#openHistreamProxyBtn').addEventListener('click', openHistreamProxyDialog)
createMenubar({
  root: $('#menubar'),
  route: (item) => {
    if (!item.dataset.sceneCommand || !streamWorkspace.isOpen()) return false
    streamWorkspace.command(item.dataset.sceneCommand)
    return true
  }
})
const streamWorkspace = createStreamWorkspace({ toast })
for (const item of document.querySelectorAll('[data-stream-command]')) {
  item.addEventListener('click', () => {
    if (item.dataset.streamCommand === 'show') streamWorkspace.open()
    else streamWorkspace.command(item.dataset.streamCommand)
  })
}
for (const item of document.querySelectorAll('[data-solution-mode]')) {
  item.addEventListener('click', () => {
    streamWorkspace.close()
    selectMode(item.dataset.solutionMode, true)
  })
}

const SHORTCUTS = [
  ['项目', [['Ctrl+N', '新建工程'], ['Ctrl+O', '打开工程'], ['Ctrl+S', '保存'], ['Ctrl+Shift+S', '另存为']]],
  ['工作台', [['F7', '打开解析模型（STREAM 像元辐射传输 / 能量平衡）']]],
  ['模拟', [['F5', '运行模拟（三维场景或遥感影像，取决于当前工作台）']]],
  ['遥感影像', [['滚轮', '缩放'], ['拖动', '平移'], ['双击', '适应窗口']]],
  ['菜单与对话框', [['方向键', '在菜单之间和菜单项之间移动'], ['Esc', '关闭菜单或对话框']]]
]

async function showHelp(topic) {
  if (topic === 'reference') { showReferenceDialog(); return }
  if (topic === 'user' || topic === 'theory') {
    try {
      const result = await api.openHelpDocument(topic)
      addLog(`已打开 ${result.path}`)
    } catch (error) { toast('无法打开手册', error.message, 'error') }
    return
  }
  const body = $('#helpBody')
  if (topic === 'shortcuts') {
    $('#helpDialogTitle').textContent = t('快捷键')
    body.innerHTML = SHORTCUTS.map(([group, rows]) => `<section><h3>${escapeHtml(t(group))}</h3><dl>${rows.map(([key, action]) => `<div><dt><kbd>${escapeHtml(key)}</kbd></dt><dd>${escapeHtml(t(action))}</dd></div>`).join('')}</dl></section>`).join('')
  } else {
    let defaults = {}
    try { defaults = await api.defaults() } catch {}
    $('#helpDialogTitle').textContent = t('关于 Stream Studio')
    body.innerHTML = `<div class="about-head"><div class="brandLogo" aria-hidden="true"><span class="logoLab">RTLab</span><span class="logoSlash"></span><span class="logoStream">STREAM</span></div>
      <div><strong>Stream Studio</strong><span>${escapeHtml(t('版本'))} ${escapeHtml(defaults.appVersion || '—')}</span></div></div>
      <p>${escapeHtml(t('三维场景与遥感影像的光学—热红外辐射传输和能量平衡模拟平台。'))}</p>
      <dl class="about-list">
        <div><dt>${escapeHtml(t('三维引擎'))}</dt><dd>HiStream　${escapeHtml(defaults.executable || '—')}</dd></div>
        <div><dt>${escapeHtml(t('遥感影像引擎'))}</dt><dd>STREAM　${escapeHtml(defaults.streamExecutable || '—')}</dd></div>
        <div><dt>${escapeHtml(t('运行环境'))}</dt><dd>Node ${escapeHtml(defaults.versions?.node || '—')}，Three.js ${escapeHtml(defaults.versions?.three || '—')}</dd></div>
        <div><dt>${escapeHtml(t('作者'))}</dt><dd>卞尊健、曹彪、历华、杜永明、肖青、柳钦火</dd></div>
        <div><dt>${escapeHtml(t('许可'))}</dt><dd>PolyForm Noncommercial License 1.0.0${escapeHtml(t('；商业使用须另行取得书面授权'))}</dd></div>
      </dl>`
  }
  $('#helpDialog').hidden = false
  $('#closeHelpBtn').focus()
}
const hideHelp = () => { $('#helpDialog').hidden = true }
for (const item of document.querySelectorAll('[data-help]')) item.addEventListener('click', () => showHelp(item.dataset.help))
$('#closeHelpBtn').addEventListener('click', hideHelp)
$('#helpDialog').addEventListener('click', (event) => { if (event.target === $('#helpDialog')) hideHelp() })
window.addEventListener('keydown', (event) => { if (event.key === 'Escape' && !$('#helpDialog').hidden) hideHelp() })
$('#closeHistreamProxyBtn').addEventListener('click', hideHistreamProxyDialog)
$('#histreamProxyDialog').addEventListener('click', (event) => { if (event.target === $('#histreamProxyDialog')) hideHistreamProxyDialog() })
$('#runHistreamProxyBtn').addEventListener('click', runHistreamProxyFrame)
$('#histreamProxyBandSelect').addEventListener('change', showHistreamProxyBand)
$$('.reference-shot').forEach((button) => button.addEventListener('click', () => showReferenceImage(button)))
$('#closeReferenceImageBtn').addEventListener('click', hideReferenceImage)
$('#referenceImageDialog').addEventListener('click', (event) => { if (event.target === $('#referenceImageDialog')) hideReferenceImage() })
$('#closeGeometryBtn').addEventListener('click', hideGeometryDialog)
$('#geometryDialog').addEventListener('click', (event) => { if (event.target === $('#geometryDialog')) hideGeometryDialog() })
$('#geometryForm').addEventListener('input', updateGeometryDialog)
$('#geometryForm').addEventListener('change', (event) => {
  if (event.target.name === 'geometryMode' && (event.target.value === 'fire' || event.target.value === 'fog')) setMediumDefaults(event.target.value)
  if (event.target.name === 'geometryMode' && event.target.value === 'water') setWaterDefaults()
  updateGeometryDialog()
})
$('#geometryForm').addEventListener('submit', generateGeometryObject)
$('#closeMaterialBtn').addEventListener('click', () => hideMaterialDialog(true))
$('#materialDialog').addEventListener('click', (event) => { if (event.target === $('#materialDialog')) hideMaterialDialog(true) })
$('#materialForm').addEventListener('submit', saveImportedObject)
$('#closeObjectAttributeBtn').addEventListener('click', hideObjectAttributeDialog)
$('#objectAttributeDialog').addEventListener('click', (event) => { if (event.target === $('#objectAttributeDialog')) hideObjectAttributeDialog() })
$('#objectAttributeForm').addEventListener('submit', saveObjectAttributes)
$('#objectAttributeType').addEventListener('change', () => renderObjectAttributeOptions(true, true))
$('#objectAttributeMaterial').addEventListener('change', () => {
  const materialName = $('#objectAttributeMaterial').value
  // Apply the object-wide choice to all meshes; individual rows remain editable.
  $$('#objectAttributeMeshRows .attribute-physical').forEach((select) => { select.value = materialName })
  if (state.config.materials.find((item) => item.name === materialName)?.energyModel === 'photovoltaic') {
    $('#objectAttributeForm').elements.canopyName.value = 'rigid_body'
    $$('#objectAttributeMeshRows .attribute-canopy').forEach((select) => { select.value = 'rigid_body' })
  }
  renderObjectAttributeSummary()
})
$('#closeObjectBtn').addEventListener('click', hideObjectDialog)
$('#objectDialog').addEventListener('click', (event) => { if (event.target === $('#objectDialog')) hideObjectDialog() })
$('#objectForm').addEventListener('submit', saveSceneObject)
$('#objectForm').addEventListener('input', (event) => {
  const form = $('#objectForm')
  if (event.target.name === 'distributionCount') syncDistributionPopulation(form, 'count')
  else if (event.target.name === 'distributionDensity') syncDistributionPopulation(form, 'density')
  else if (['distributionMinX', 'distributionMaxX', 'distributionMinY', 'distributionMaxY'].includes(event.target.name)) syncDistributionPopulation(form, form.elements.distributionBasis.value)
  updateDistributionFormState(form)
  updateMovementFormState(form)
})
$('#objectForm').addEventListener('change', (event) => {
  const form = $('#objectForm')
  if (event.target.name === 'distributionBasis') syncDistributionPopulation(form, event.target.value)
  updateDistributionFormState(form)
  updateMovementFormState(form)
})
$('#distributionTxtFile').addEventListener('change', (event) => importDistributionTxt(event.target.files?.[0]))
$('#spectralModel').addEventListener('change', () => {
  const reflectance = materialSpectrumValues('reflectance') || '0.20'
  const transmittance = materialSpectrumValues('transmittance') || '0.0'
  const fileName = $('#materialForm [name="fileName"]')?.value || ''
  renderModelParameters(fileName)
  renderMaterialSpectrumValueFields(reflectance, transmittance)
})
document.querySelector("#spectralPreset").addEventListener("change", () => {
  applySpectrumPreset()
  $$('.mesh-spectrum').forEach((select) => { select.value = $('#spectralPreset').value })
})
document.querySelector("#thermalPreset").addEventListener("change", () => {
  applyThermalPreset()
  $$('.mesh-thermal').forEach((select) => { select.value = $('#thermalPreset').value })
})
$('#materialObjectType').addEventListener('change', () => {
  const type = $('#materialObjectType').value
  if (type === 'SolarPanel') {
    $('#spectralPreset').value = '__new__'
    $('#thermalPreset').value = '__new__'
  }
  $('#spectralModel').value = type === 'Vegetation' ? 'Prospect' : type === 'Soil' ? 'BSM' : 'custom'
  renderModelParameters(); renderSpectrumPresetOptions(); renderThermalPresetOptions(); renderPhysicalPresetOptions(); renderMeshMaterialRows()
})
$('#physicalMaterialPreset').addEventListener('change', () => {
  renderPhysicalParameters()
  const materialName = $('#physicalMaterialPreset').value
  $$('.mesh-physical').forEach((select) => { select.value = materialName })
  if (state.config.materials.find((item) => item.name === materialName)?.energyModel === 'photovoltaic') {
    $$('.mesh-canopy').forEach((select) => { select.value = 'rigid_body' })
  }
})
$('#closePresetBtn').addEventListener('click', hidePresetDialog)
$('#presetDialog').addEventListener('click', (event) => { if (event.target === $('#presetDialog')) hidePresetDialog() })
$('#presetKind').addEventListener('change', () => { state.presetPhysicalType = 'Vegetation'; renderPresetParameterFields() })
$('#presetForm').addEventListener('submit', savePreset)


$('#engineBtn').addEventListener('click', chooseEngine)
$('#openOutputBtn').addEventListener('click', () => showResults('image'))
$('#openImageResultsTopBtn').addEventListener('click', () => showResults('image'))
$('#open3dResultsTopBtn').addEventListener('click', () => showResults('3d'))
$('#openTimeAnalysisBtn').addEventListener('click', () => showResults('analysis', 'time'))
$('#openAngleAnalysisBtn').addEventListener('click', () => showResults('analysis', 'angle'))
$('#openBandAnalysisBtn').addEventListener('click', () => showResults('analysis', 'band'))
$('#openChangeAnalysisBtn').addEventListener('click', () => showResults('analysis', 'change'))
$('#resultImageTab').addEventListener('click', () => setResultView('image'))
$('#resultAnalysisTab').addEventListener('click', () => setResultView('analysis'))
$('#angleAnalysisTab').addEventListener('click', () => setAnalysisType('angle'))
$('#bandAnalysisTab').addEventListener('click', () => setAnalysisType('band'))
$('#timeAnalysisTab').addEventListener('click', () => setAnalysisType('time'))
$('#changeAnalysisTab').addEventListener('click', () => setAnalysisType('change'))
$('#threeAnalysisTab').addEventListener('click', () => setAnalysisType('3d'))
$('#closeResultBtn').addEventListener('click', hideResults)
$('#resultDialog').addEventListener('click', (event) => { if (event.target === $('#resultDialog')) hideResults() })
$('#resultDeleteAllBtn').addEventListener('click', deleteAllResults)
$('#resultRefreshBtn').addEventListener('click', async () => { try { await refreshResults(true) } catch (error) { toast('刷新结果失败', error.message, 'error') } })
$('#resultFolderBtn').addEventListener('click', async () => { try { await api.openPath(state.outputDir) } catch (error) { toast('无法打开输出目录', error.message, 'error') } })
$('#clearLogBtn').addEventListener('click', () => { $('#consoleOutput').innerHTML = ''; state.logCount = 0; $('#logCount').textContent = '0' })
$('#xmlBtn').addEventListener('click', () => { $('#xmlDrawer').hidden = false }); $('#closeXmlBtn').addEventListener('click', () => { $('#xmlDrawer').hidden = true })
$('#xmlDrawer').addEventListener('click', (event) => { if (event.target === $('#xmlDrawer')) $('#xmlDrawer').hidden = true })
$('#xmlEditor').addEventListener('input', () => { state.xmlText = $('#xmlEditor').value; markXmlDirty(); state.xmlManualEdit = true; $('#unsavedMark').hidden = false; $('#xmlStatus').textContent = '有未保存的修改' })
$('#applyXmlBtn').addEventListener('click', async () => { try { const project = parseProjectJson(state.xmlText); const mode = project.mode; state.project = project; state.config = ensureMaterialPresets(project.configuration); state.xmlManualEdit = false; selectMode(mode); if (await saveXml()) { refreshFromState(); restoreActualScene(); renderInspector(); $('#xmlDrawer').hidden = true } } catch (error) { toast('JSON 格式错误', error.message, 'error') } })
window.addEventListener('keydown', (event) => { if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') { event.preventDefault(); if (event.shiftKey) showSaveAsDialog(); else saveXml() } if (event.key === 'F5') { event.preventDefault(); runSimulation() } if (event.key === 'F7') { event.preventDefault(); streamWorkspace.open() } if (event.key === 'Escape') { if (!$('#histreamProxyDialog').hidden) hideHistreamProxyDialog(); else if (!$('#referenceImageDialog').hidden) hideReferenceImage(); else if (!$('#referenceDialog').hidden) hideReferenceDialog(); else if (!$('#resultDialog').hidden) hideResults(); else if (!$('#objectAttributeDialog').hidden) hideObjectAttributeDialog(); else if (!$('#objectDialog').hidden) hideObjectDialog(); else if (!$('#materialDialog').hidden) hideMaterialDialog(true); else if (!$('#geometryDialog').hidden) hideGeometryDialog(); else if (!$('#presetDialog').hidden) hidePresetDialog(); else if (!$('#projectDialog').hidden) hideProjectDialog(); else if (!$('#xmlDrawer').hidden) $('#xmlDrawer').hidden = true } })
setInterval(() => { if (state.running) renderRunProgress() }, 250)

const ENGINE_PROMPT = 'HiStream 可执行文件路径（留空恢复内置引擎）。自定义路径须先由管理员加入服务端环境变量 STREAM_STUDIO_ENGINE_ALLOWLIST（多个路径用系统路径分隔符分隔，Windows 为分号 ;），并重启 Stream Studio，否则运行会被拒绝。'
const ENGINE_REJECTED_HINT = '自定义引擎不在允许列表中：请将该路径加入环境变量 STREAM_STUDIO_ENGINE_ALLOWLIST后重启 Stream Studio，或在“引擎”中留空以恢复内置引擎。'

async function chooseEngine() {
  if (state.mode === 'eFacetRT' || state.mode === 'eFacetEB') { toast('HiStream Facet 引擎', state.executable); return }
  const path = await api.chooseExecutable(t(ENGINE_PROMPT))
  if (path === null) return
  if (!path || path === state.defaultExecutable) {
    state.executable = state.defaultExecutable; state.executableExists = state.defaultExecutableExists
    setRunning(false); addLog(`已恢复内置 HiStream 引擎：${state.executable}`, 'success')
    loadComputeDevices()
    return
  }
  state.executable = path; state.executableExists = true; setRunning(false)
  loadComputeDevices()
  addLog(`HiStream 引擎：${path}`, 'success')
  addLog('自定义引擎须已加入服务端 STREAM_STUDIO_ENGINE_ALLOWLIST，否则运行时会被拒绝', 'warning')
}

function isEngineAllowlistError(error) {
  return String(error?.message || '').includes('ENGINE_ALLOWLIST')
}

async function init() {
  state.config = defaultConfig(); generateWorld(state.config); refreshFromState(false); fitCamera(); renderInspector(); resize()
  api.onSimulationEvent(handleSimulationEvent)
  let defaults
  try {
    defaults = await api.defaults()
    state.executable = defaults.executable; state.executableExists = defaults.executableExists
    state.defaultExecutable = defaults.executable; state.defaultExecutableExists = defaults.executableExists
    $('#openHistreamProxyBtn').hidden = !defaults.histreamProxyConfigured
    setRunning(false)
    addLog(`GPU Radiosity：${defaults.radiosityExecutable}`, defaults.radiosityExecutableExists ? 'success' : 'error')
    addLog(`默认 HiStream：${defaults.executable}`, 'success'); addLog(`Three.js ${defaults.versions.three} · Node ${defaults.versions.node}`)
    loadComputeDevices()
  } catch (error) { state.executableExists = false; setEngineState('桌面桥接不可用', error.message, ''); addLog(error.message, 'error'); return }
  if (!defaults.projectFile) return
  try { loadXml(await api.openProject(defaults.projectFile)) }
  catch (error) { addLog(`无法重新打开上次的工程：${error.message}`, 'error') }
}

startLocalization({ onChange: () => {
  renderInspector()
  if (!state.config) return
  renderNavBadges(); updateSceneBoundsGuide(state.config); updateCruiseVisualization(state.config); updateSun(state.config)
} })
init()
