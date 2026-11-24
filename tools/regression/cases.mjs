// Engine regression cases built from the bundled examples (assets/examples).
// Each case freezes one example + mode + meteo window + control overrides so
// two engine builds can be run on identical inputs and compared.

// n x n identical objects centred in the cells of a square lattice over a
// size x size domain (position rows: x y z scale rotation).
const lattice = (n, size, scale = 1) => Array.from({ length: n * n }, (_, k) =>
  [(k % n + 0.5) * size / n, (Math.floor(k / n) + 0.5) * size / n, 0, scale, 0])
// The forest trees on a 25 m lattice: an exactly periodic 100 m scene whose
// crowns stay inside the domain (jacaranda at 0.95 scale spans < 25 m), so
// with periodic boundaries all 16 lattice cells must agree up to noise.
const FOREST_LATTICE = {
  'positions/jacaranda_position.txt': lattice(4, 100, 0.95),
  'positions/searsia_position.txt': lattice(8, 100),
}
const PERIODIC_8 = { 'configuration.control.periodicTraversalCount': 8 }
const MWIR_BANDS = { 'configuration.sensor.bands': '3800,10500', 'configuration.sensor.continuousBands': false,
  'configuration.sensor.temperature': true }
// Every soil set on soil method 1 (the control value overrides the material).
const SOIL_METHOD_1 = { 'configuration.control.soilTemperatureMethod': 1 }
const SOIL_METHOD_2 = { 'configuration.control.soilTemperatureMethod': 2 }
// No per-node process products (energy-process diagnostics are tens of MB per node).
const NO_PROCESS = { 'configuration.sensor.energyProcess': false, 'configuration.sensor.radiationProcess': false }
// Method 1 with the complete heat history (8 nodes + exponential modes).
const SOIL_HISTORY_FULL = { 'configuration.control.soilInertiaHistory': 'full' }
const soilThermal = (cs, rhos, lambdas) => (index) => ({ [`configuration.materials.${index}.params.cs`]: cs,
  [`configuration.materials.${index}.params.rhos`]: rhos, [`configuration.materials.${index}.params.lambdas`]: lambdas })
const SOIL_SCOPE = soilThermal(1180, 1800, 1.55)
const SOIL_LOAM = soilThermal(1300, 1600, 1.0)
// Heterogeneous (Hapke) background at full angular strength.
const HAPKE_FULL = { 'configuration.scene.background.heterogeneityEnabled': true,
  'configuration.scene.background.angularEffectStrength': 1 }
// A flat size x size water sheet (n x n cells, upward normal) as the bundled
// water generator writes it (main.js waterSurfaceObj), 1 cm above the ground.
const waterSheetObj = (name, size, n) => {
  const lines = [`o ${name}`, `g ${name}`, 's off']
  const h = size / 2, d = size / n
  for (let j = 0; j <= n; j++) for (let i = 0; i <= n; i++) lines.push(`v ${-h + i * d} 0 ${-h + j * d}`)
  const v = (i, j) => j * (n + 1) + i + 1
  for (let j = 0; j < n; j++) for (let i = 0; i < n; i++)
    lines.push(`f ${v(i, j)} ${v(i, j + 1)} ${v(i + 1, j)}`, `f ${v(i + 1, j)} ${v(i, j + 1)} ${v(i + 1, j + 1)}`)
  return lines.join('\n') + '\n'
}
// 30 m x 30 m pond in the open south-east corner of urban_block (no
// buildings or street trees within 5 m), centre (280, 65).
const URBAN_WATER = {
  files: {
    'models/regression_water.obj': waterSheetObj('regression_water', 30, 6),
    'positions/regression_water_position.txt': '280 65 0.01 1 0\n'
  },
  addObjects: [{
    name: 'regression_water', type: 'Water', materialName: 'water_set', canopyName: 'rigid_body',
    fileName: 'models/regression_water.obj', positionFile: 'positions/regression_water_position.txt',
    shape: 'cube', dimensions: [30, 0.01, 30], instanceCount: 1,
    meshes: [{ name: 'regression_water', spectralName: 'water_surface', thermalName: 'water_temperature',
      materialName: 'water_set', canopyName: 'rigid_body' }]
  }]
}

// The urban_block buildings (one OBJ placement at the domain centre) rotated
// by `angle` degrees: the voxel proxy geometry of a rotated instance must lie
// on the world cells it fills, so ground cells enclosed by walls and roofs
// receive no direct sun (sunlit_fraction 0, as without rotation).
const URBAN_ROTATED = (angle) => ({ 'positions/urban_block_buildings_position.txt': [[150, 150, 0, 1, angle]] })
// Default layered constructions of new projects (outer to inner; see
// project-schema.js building_surface / building_roof).
const WALL_LAYERS = [{ thickness: 0.1, lambda: 1.74, rho: 2500, cp: 920 }, { thickness: 0.08, lambda: 0.04, rho: 20, cp: 1380 },
  { thickness: 0.02, lambda: 0.87, rho: 1700, cp: 1050 }]
const ROOF_LAYERS = [{ thickness: 0.05, lambda: 1.51, rho: 2300, cp: 920 }, { thickness: 0.1, lambda: 0.03, rho: 35, cp: 1380 },
  { thickness: 0.15, lambda: 1.74, rho: 2500, cp: 920 }]
// The library bulk carrier at 0.15 scale (28.5 m x 4.2 m, hull 0-1.5 m, superstructure
// to 5.1 m) floating 2 cm above the URBAN_WATER pond, material ship_material (index 7).
// SHIP_NEW is the new-project ship_material (one 12 mm steel plate, 22 C interior).
const SHIP_GROUPS = ['hull', 'weather_deck', 'cargo_hatches', 'deck_cranes', 'crane_booms', 'superstructure', 'bridge', 'funnel']
// urbanShip(funnelMaterial, position): the funnel group may take its own material
// (internal heat source of the funnel only); position rows x y z scale rotation.
const urbanShip = (funnelMaterial = 'ship_material', position = '280 65 0.02 0.15 0', waterRow = '280 65 0.01 1 0') => ({
  files: { ...URBAN_WATER.files, 'positions/regression_water_position.txt': waterRow + '\n',
    'positions/regression_ship_position.txt': position + '\n' },
  addObjects: [...URBAN_WATER.addObjects, {
    name: 'regression_ship', type: 'Ship', materialName: 'ship_material', canopyName: 'rigid_body',
    fileName: 'assets/obj-library/ship/typical/modern_bulk_carrier.obj', positionFile: 'positions/regression_ship_position.txt',
    shape: 'cube', dimensions: [4.2, 5.1, 28.5], instanceCount: 1,
    meshes: SHIP_GROUPS.map((name) => ({ name, spectralName: 'ship_surface', thermalName: 'ship_temperature',
      materialName: name === 'funnel' ? funnelMaterial : 'ship_material', canopyName: 'rigid_body' }))
  }]
})
const URBAN_SHIP = urbanShip()
// The same ship floating in a pond whose surface is 0.65 m above the ground (the keel stays
// 0.02 m above y = 0, where the scene domain is clipped): draft 0.63 m of the 1.5 m hull.
const URBAN_SHIP_DRAFT = urbanShip('ship_material', '280 65 0.02 0.15 0', '280 65 0.65 1 0')
const VEB_SHIP = (params) => ({ example: 'urban_block', mode: 'eVoxelEB', meteo: [16, 32], ...URBAN_SHIP, set: {
  'configuration.scene.voxel': 1, 'configuration.materials.7.params': params } })
const SHIP_NEW = { method: 1, rss: 4000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 30, SMC: 0, Satwater: 0, wallMode: 'wall',
  thickness: 0.012, indoorTemperature: 22, indoorHeatTransferCoefficient: 8, layers: [{ thickness: 0.012, lambda: 50, rho: 7850, cp: 460 }], speed: 0 }

// Vehicle cabin (FacetEB, docs/vehicle-thermal.md): the library sedan (groups Blue, Windows, Black, Grey,
// Headlights, TailLights) placed in the urban block. Body = 1 mm steel + 6 mm trim, glass = 4.8 mm pane
// (tau_sol 0.65), the rest a thin shell. One body material per vehicle state (cabin parameters are per
// body material); the paint (spectrum) is per object.
const CABIN = { enabled: true, occupants: 0, speed: 0, ventilationACH: 0.5, windowCracked: false, acOn: false, engineOn: false }
const VEH_COMMON = { method: 1, rss: 1000000000, cs: 900, rhos: 2700, lambdas: 10, Tsoil: 25, SMC: 0, Satwater: 0 }
const VEH_BODY = (cabin = {}, extra = {}) => ({ name: 'vehicle_body', label: '载具车身物化', type: 'Vehicle', params: { ...VEH_COMMON, wallMode: 'wall',
  thickness: 0.007, indoorTemperature: 22, indoorHeatTransferCoefficient: 7, vehiclePart: 'body',
  layers: [{ thickness: 0.001, lambda: 50, rho: 7850, cp: 460 }, { thickness: 0.006, lambda: 0.04, rho: 60, cp: 1400 }],
  cabin: { ...CABIN, ...cabin }, ...extra } })
const VEH_GLASS = { name: 'vehicle_glass', label: '载具玻璃', type: 'Vehicle', params: { ...VEH_COMMON, wallMode: 'wall', thickness: 0.0048,
  indoorTemperature: 22, indoorHeatTransferCoefficient: 8, vehiclePart: 'glass', layers: [{ thickness: 0.0048, lambda: 1.0, rho: 2500, cp: 840 }] } }
const VEH_UNDER = { name: 'vehicle_underbody', label: '载具内饰/底盘薄壳', type: 'Vehicle', params: { ...VEH_COMMON, vehiclePart: 'underbody', heatCapacityPerArea: 6000 } }
const VEH_SPECTRA = [
  { name: 'vehicle_paint_light', label: '浅色车漆', model: 'custom', reflectance: '0.65', transmittance: '0.0', refTir: 0.1, tauTir: 0, params: {} },
  { name: 'vehicle_paint_dark', label: '深色车漆', model: 'custom', reflectance: '0.12', transmittance: '0.0', refTir: 0.1, tauTir: 0, params: {} },
  { name: 'vehicle_glass_spectrum', label: '车窗玻璃', model: 'custom', reflectance: '0.08', transmittance: '0.65', refTir: 0.1, tauTir: 0, params: {} }]
const VEH_GROUPS = (paint, body) => ['Blue', 'Windows', 'Black', 'Grey', 'Headlights', 'TailLights'].map((name) => ({ name,
  spectralName: name === 'Blue' ? paint : name === 'Windows' ? 'vehicle_glass_spectrum' : 'vehicle_surface', thermalName: 'vehicle_temperature',
  materialName: name === 'Blue' ? body : name === 'Windows' ? 'vehicle_glass' : 'vehicle_underbody', canopyName: 'rigid_body' }))
// vehicles: [{ name, paint, body (material name), rows: ['x y z scale rotation', ...] }], bodies: material objects
const urbanVehicles = (vehicles, bodies, meteo = [12, 40]) => ({ example: 'urban_block', mode: 'eFacetEB', meteo,
  addSpectra: VEH_SPECTRA, addMaterials: [...bodies, VEH_GLASS, VEH_UNDER],
  files: Object.fromEntries(vehicles.map((v) => [`positions/${v.name}_position.txt`, v.rows.join('\n') + '\n'])),
  addObjects: vehicles.map((v) => ({ name: v.name, type: 'Vehicle', materialName: v.body, canopyName: 'rigid_body', spectralName: v.paint,
    fileName: 'assets/obj-library/vehicle/typical/modern_sedan.obj', positionFile: `positions/${v.name}_position.txt`,
    shape: 'cube', dimensions: [1.82, 1.46, 4.58], instanceCount: v.rows.length, meshes: VEH_GROUPS(v.paint, v.body) })) })
const VEH_PARK = urbanVehicles([
  { name: 'park_light', paint: 'vehicle_paint_light', body: 'vehicle_body', rows: ['150 40 0 1 0', '177.3 12.2 0 1 0', '98 175 0 1 0', '98 236 0 1 0'] },
  { name: 'park_dark', paint: 'vehicle_paint_dark', body: 'vehicle_body', rows: ['150 60 0 1 0'] }], [VEH_BODY()])
const VEH_DRIVE = (acOn) => urbanVehicles([
  { name: 'drive_car', paint: 'vehicle_paint_light', body: 'vehicle_body', rows: ['150 40 0 1 0'] }],
  [VEH_BODY({ occupants: 1, speed: 15, engineOn: true, acOn, heading: 90, windDirection: 270 })])

export const CASES = {
  // FacetEB vehicle cabin (06:00-20:00): 5 parked cars (light paint in sun / tree / two building-side spots, dark paint
  // in sun), one driving car at 15 m/s without and with air conditioning.
  feb_urban_vehicle_park: VEH_PARK,
  feb_urban_vehicle_drive: VEH_DRIVE(false),
  feb_urban_vehicle_ac: VEH_DRIVE(true),
  vrt_urban: { example: 'urban_block', mode: 'eVoxelRT' },
  vrt_urban_multiview: { example: 'urban_block', mode: 'eVoxelRT', set: { 'configuration.sensor.principalPlane': true } },
  veb_urban: { example: 'urban_block', mode: 'eVoxelEB', meteo: [23, 25] },
  veb_forest: { example: 'forest', mode: 'eVoxelEB', meteo: [23, 25] },
  feb_urban: { example: 'urban_block', mode: 'eFacetEB', meteo: [23, 24] },
  feb_crop: { example: 'crop_gpp_npp', mode: 'eFacetEB', meteo: [23, 25] },
  frt_crop: { example: 'crop_gpp_npp', mode: 'eFacetRT' },
  frt_crop_periodic: { example: 'crop_gpp_npp', mode: 'eFacetRT', set: { 'configuration.control.periodicTraversalCount': 8 } },
  // FacetEB without boundary effects: the reference for the outer plants of
  // feb_crop (lower boundary / downward escapes, edge-versus-centre leaves).
  feb_crop_periodic: { example: 'crop_gpp_npp', mode: 'eFacetEB', meteo: [23, 25], set: PERIODIC_8 },
  rt_urban: { example: 'urban_block', mode: 'eRaytracing' },
  rt_forest: { example: 'forest', mode: 'eRaytracing' },
  rt_crop: { example: 'crop_gpp_npp', mode: 'eRaytracing' },
  // Street-level perspective view with sky in frame (sky-edge handling).
  rt_urban_street: { example: 'urban_block', mode: 'eRaytracing', set: {
    'configuration.sensor.projection': 'perspective', 'configuration.sensor.cruiseEnabled': false,
    'configuration.sensor.positionX': 150, 'configuration.sensor.positionY': 5, 'configuration.sensor.height': 25,
    'configuration.sensor.vza': 78, 'configuration.sensor.vaa': 0, 'configuration.sensor.fov': 60,
    'configuration.control.periodicTraversalCount': 0 } },
  vrt_urban_street: { example: 'urban_block', mode: 'eVoxelRT', set: {
    'configuration.sensor.projection': 'perspective', 'configuration.sensor.cruiseEnabled': false,
    'configuration.sensor.positionX': 150, 'configuration.sensor.positionY': 5, 'configuration.sensor.height': 25,
    'configuration.sensor.vza': 78, 'configuration.sensor.vaa': 0, 'configuration.sensor.fov': 60,
    'configuration.control.periodicTraversalCount': 0 } },
  // Leaf angle / projection consistency (canopies[1] = tree_leaf_canopy).
  // vrt_forest is the reference; _planophile uses the SAIL planophile LIDF
  // (a=1, b=0); _g03 forces the constant-G fallback with G=0.3 through an
  // out-of-domain LIDF (|a|+|b|>1), where the old engine amplified leaf
  // single scattering by 0.5/G.
  vrt_forest: { example: 'forest', mode: 'eVoxelRT' },
  vrt_forest_planophile: { example: 'forest', mode: 'eVoxelRT', set: {
    'configuration.canopies.1.LIDFa': 1, 'configuration.canopies.1.LIDFb': 0 } },
  vrt_forest_g03: { example: 'forest', mode: 'eVoxelRT', set: {
    'configuration.canopies.1.G': 0.3, 'configuration.canopies.1.LIDFa': 0.8, 'configuration.canopies.1.LIDFb': 0.8 } },
  // Periodic boundaries in every transport ray (shadow, sky, bounce; voxel
  // solar gap, diffuse, scattering, TIR). Compare the outer 10 % border strip
  // with the interior: with periodic boundaries the border must lose its
  // missing-neighbour bias. The urban examples already use 20 traversals.
  rt_forest_periodic: { example: 'forest', mode: 'eRaytracing', set: { 'configuration.control.periodicTraversalCount': 8 } },
  vrt_forest_periodic: { example: 'forest', mode: 'eVoxelRT', set: { 'configuration.control.periodicTraversalCount': 8 } },
  veb_forest_periodic: { example: 'forest', mode: 'eVoxelEB', meteo: [23, 25], set: { 'configuration.control.periodicTraversalCount': 8 } },
  rt_lattice_periodic: { example: 'forest', mode: 'eRaytracing', positions: FOREST_LATTICE, set: PERIODIC_8 },
  vrt_lattice_periodic: { example: 'forest', mode: 'eVoxelRT', positions: FOREST_LATTICE, set: PERIODIC_8 },
  veb_lattice_periodic: { example: 'forest', mode: 'eVoxelEB', meteo: [23, 25], positions: FOREST_LATTICE, set: PERIODIC_8 },
  // Mid-wave infrared: reflected sunlight below 6 um (mwir_solar.glsl). The
  // 3.8 um band must warm by about rho*E*cos/(pi*dB/dT) by day; the 10.5 um
  // band in the same run must stay unchanged.
  rt_urban_mwir: { example: 'urban_block', mode: 'eRaytracing', set: MWIR_BANDS },
  vrt_urban_mwir: { example: 'urban_block', mode: 'eVoxelRT', set: MWIR_BANDS },
  vrt_forest_mwir: { example: 'forest', mode: 'eVoxelRT', set: MWIR_BANDS },
  veb_forest_mwir: { example: 'forest', mode: 'eVoxelEB', meteo: [23, 25], set: MWIR_BANDS },
  veb_urban_mwir: { example: 'urban_block', mode: 'eVoxelEB', meteo: [23, 25], set: MWIR_BANDS },
  // Night nodes (Rin = 0): the 3.8 um band must stay unchanged.
  veb_urban_mwir_night: { example: 'urban_block', mode: 'eVoxelEB', meteo: [1, 3], set: MWIR_BANDS },
  // Night nodes 01:50 and 02:20 (stable stratification): Monin-Obukhov
  // stability correction (control.stabilityCorrection) must reduce exchange.
  // A run that starts at night has no radiatively cooled history, so the
  // initial soil/structure temperature is set below the air (~14 C).
  veb_forest_night: { example: 'forest', mode: 'eVoxelEB', meteo: [3, 5], set: { 'configuration.meteo.Tsold': 283.15 } },
  veb_urban_night: { example: 'urban_block', mode: 'eVoxelEB', meteo: [3, 5], set: { 'configuration.meteo.Tsold': 283.15 } },
  // Moving shadows, 08:50-13:20 (10 nodes of 30 min): building and tree
  // shadows sweep streets, walls and roofs, so sunlit parts change between
  // nodes. Sunlit/shaded thermal states must exchange area together with
  // their stored heat (FacetEB thermalHistory diagnostics, heat closure).
  // _m1 switches the soil sets to the thermal-inertia history (method 1).
  feb_urban_shadow: { example: 'urban_block', mode: 'eFacetEB', meteo: [17, 27] },
  feb_urban_shadow_m1: { example: 'urban_block', mode: 'eFacetEB', meteo: [17, 27], set: SOIL_METHOD_1 },
  veb_urban_shadow_m1: { example: 'urban_block', mode: 'eVoxelEB', meteo: [17, 27], set: SOIL_METHOD_1 },
  // FacetEB open water (mixed layer + evaporation) over the same 10 nodes:
  // noon LE of a few hundred W m-2, small water-temperature range.
  feb_urban_water: { example: 'urban_block', mode: 'eFacetEB', meteo: [17, 27], ...URBAN_WATER },
  // The urban buildings (material soil_dry, index 5) as 0.2 m wall/roof slabs
  // with a 22 C interior (wallMode "wall"); feb_urban_shadow is the thin shell.
  feb_urban_shadow_wall: { example: 'urban_block', mode: 'eFacetEB', meteo: [17, 27], set: {
    'configuration.materials.5.params.wallMode': 'wall', 'configuration.materials.5.params.thickness': 0.2 } },
  // New-project defaults (project-schema createMaterialPresets): water_set with
  // a 2 m mixed layer starting at the mean air temperature of the first 24 h,
  // and the buildings as the layered insulated wall (wall mesh, building_surface)
  // and roof (roof mesh, building_roof). For diurnal ranges run them over two
  // days: REGRESSION_SET='configuration.meteo.start=0;configuration.meteo.end=96'.
  feb_urban_water_newdefault: { example: 'urban_block', mode: 'eFacetEB', meteo: [17, 27], ...URBAN_WATER, set: {
    'configuration.materials.8.params.mixingDepth': 2, 'configuration.materials.8.params.initialTemperatureMode': 'dailyMeanAir' } },
  feb_urban_wall_layered: { example: 'urban_block', mode: 'eFacetEB', meteo: [17, 27], set: {
    'configuration.materials.5.params.wallMode': 'wall', 'configuration.materials.5.params.layers': WALL_LAYERS,
    'configuration.materials.9': { name: 'building_roof', label: '寤虹瓚灞嬮《', type: 'Building', params: {
      method: 2, rss: 1000000000, cs: 880, rhos: 2300, lambdas: 1.4, Tsoil: 25, SMC: 0, Satwater: 0, wallMode: 'wall',
      thickness: 0.3, indoorTemperature: 22, indoorHeatTransferCoefficient: 8, layers: ROOF_LAYERS } },
    'configuration.objects.items.0.meshes.1.materialName': 'building_roof' } },
  // Mid-wave infrared in FacetRT/FacetEB (facetRtThermalSolar and the FacetEB
  // state spectra): 3.8 um warms by about rho*E*cos/pi by day, 10.5 um and
  // night nodes stay unchanged.
  frt_crop_mwir: { example: 'crop_gpp_npp', mode: 'eFacetRT', set: MWIR_BANDS },
  frt_urban_mwir: { example: 'urban_block', mode: 'eFacetRT', set: MWIR_BANDS },
  feb_urban_mwir: { example: 'urban_block', mode: 'eFacetEB', meteo: [23, 24], set: MWIR_BANDS },
  feb_urban_mwir_night: { example: 'urban_block', mode: 'eFacetEB', meteo: [1, 2], set: MWIR_BANDS },
  // light.shortwaveIrradiance (RayTracing/VoxelRT absolute 2.5-6 um solar
  // term) with an overcast direct fraction of 0.2: _d02 keeps the clear-sky
  // default (must equal the old engine); _sw200/_sw400 set 200/400 W m-2, so
  // the 3.8 um solar part scales linearly with the total
  // ((L_d02 - L_sw400) / (L_sw400 - L_sw200) = (E_clear - 400) / 200).
  rt_urban_mwir_d02: { example: 'urban_block', mode: 'eRaytracing', set: { ...MWIR_BANDS, 'configuration.light.direct': 0.2 } },
  rt_urban_mwir_sw200: { example: 'urban_block', mode: 'eRaytracing', set: { ...MWIR_BANDS, 'configuration.light.direct': 0.2, 'configuration.light.shortwaveIrradiance': 200 } },
  rt_urban_mwir_sw400: { example: 'urban_block', mode: 'eRaytracing', set: { ...MWIR_BANDS, 'configuration.light.direct': 0.2, 'configuration.light.shortwaveIrradiance': 400 } },
  vrt_urban_mwir_d02: { example: 'urban_block', mode: 'eVoxelRT', set: { ...MWIR_BANDS, 'configuration.light.direct': 0.2 } },
  vrt_urban_mwir_sw200: { example: 'urban_block', mode: 'eVoxelRT', set: { ...MWIR_BANDS, 'configuration.light.direct': 0.2, 'configuration.light.shortwaveIrradiance': 200 } },
  vrt_urban_mwir_sw400: { example: 'urban_block', mode: 'eVoxelRT', set: { ...MWIR_BANDS, 'configuration.light.direct': 0.2, 'configuration.light.shortwaveIrradiance': 400 } },
  // Rotated instances (URBAN_ROTATED): VoxelRT at 30 and 45 degrees, VoxelEB
  // at 30 degrees, and RayTracing (exact rotated triangles) for reference.
  vrt_urban_rot30: { example: 'urban_block', mode: 'eVoxelRT', positions: URBAN_ROTATED(30) },
  vrt_urban_rot45: { example: 'urban_block', mode: 'eVoxelRT', positions: URBAN_ROTATED(45) },
  veb_urban_rot30: { example: 'urban_block', mode: 'eVoxelEB', meteo: [23, 25], positions: URBAN_ROTATED(30) },
  rt_urban_rot30: { example: 'urban_block', mode: 'eRaytracing', positions: URBAN_ROTATED(30) },
  // FacetEB ship hull on the URBAN_WATER pond over one day (nodes 0-48): _shell is the
  // example's old ship_material (thin shell), the others the new 12 mm steel-plate default;
  // _speed8 sails at 8 m/s (relative wind), _waterline puts the hull below 0.9 m (world y)
  // in contact with the pond water (500 W m-2 K-1).
  feb_urban_ship_shell: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP },
  feb_urban_ship: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP, set: {
    'configuration.materials.7.params': SHIP_NEW } },
  feb_urban_ship_speed8: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP, set: {
    'configuration.materials.7.params': { ...SHIP_NEW, speed: 8 } } },
  feb_urban_ship_waterline: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP, set: {
    'configuration.materials.7.params': { ...SHIP_NEW, waterlineHeight: 0.9, waterHeatTransferCoefficient: 500 } } },
  // Ship relative wind with the course (0 = north, clockwise) and the wind-from direction:
  // _head sails into the wind (|u - v| = u + 8), _tail runs before it (|u - 8|); _speed8 above is
  // the course-less quadrature sum.
  feb_urban_ship_head: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP, set: {
    'configuration.materials.7.params': { ...SHIP_NEW, speed: 8, heading: 0, windDirection: 0 } } },
  feb_urban_ship_tail: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP, set: {
    'configuration.materials.7.params': { ...SHIP_NEW, speed: 8, heading: 0, windDirection: 180 } } },
  // Internal heat: 60 W m-2 on every hull facet (steel wall slab / the old thin shell), and
  // 20 kW spread over the funnel group only (own material ship_funnel, index 8).
  feb_urban_ship_internal: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP, set: {
    'configuration.materials.7.params': { ...SHIP_NEW, internalHeatFlux: 60 } } },
  feb_urban_ship_internal_shell: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP, set: {
    'configuration.materials.7.params.internalHeatFlux': 60 } },
  feb_urban_ship_funnel: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...urbanShip('ship_funnel'), set: {
    'configuration.materials.7.params': SHIP_NEW,
    'configuration.materials.8': { name: 'ship_funnel', label: '烟囱', type: 'Ship', params: { ...SHIP_NEW, internalHeatPower: 20000 } } } },
  // Real draft (pond surface 0.65 m, draft 0.63 m): the waterline is the water surface and every
  // facet is wetted by its exact part below it; _shield also keeps the wetted hull out of the sun
  // and sky and feeds the hull-water heat back to the pond temperature.
  feb_urban_ship_draft: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP_DRAFT, set: {
    'configuration.materials.7.params': { ...SHIP_NEW, waterlineSource: 'water', waterHeatTransferCoefficient: 500 } } },
  feb_urban_ship_draft_shield: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], ...URBAN_SHIP_DRAFT, set: {
    'configuration.materials.7.params': { ...SHIP_NEW, waterlineSource: 'water', waterHeatTransferCoefficient: 500,
      waterShielding: true, waterFeedback: true } } },
  // VoxelEB ship (minimal version, 2026-11-24): the same ship and pond, 1 m voxels (the example's 2 m
  // voxels keep only 2 hull voxels), nodes 16-32 (morning to afternoon). The hull is BUILDING voxels of
  // ship_material (semi-infinite solid SHIP_NEW cs/rhos/lambdas); speed is the forced convection of the
  // relative wind, waterlineHeight (2.0 m: the hull/deck voxels are layer 1, centre 1.5 m, below it; the
  // superstructure layers, centre 3.5 m and up, are not) couples the hull voxels to the scene water temperature at 500 W m-2 K-1.
  veb_urban_ship: VEB_SHIP(SHIP_NEW),
  veb_urban_ship_speed8: VEB_SHIP({ ...SHIP_NEW, speed: 8 }),
  veb_urban_ship_waterline: VEB_SHIP({ ...SHIP_NEW, waterlineHeight: 2.0, waterHeatTransferCoefficient: 500 }),
  veb_urban_ship_head: VEB_SHIP({ ...SHIP_NEW, speed: 8, heading: 0, windDirection: 0 }),
  // VoxelEB 12 mm steel plate and internal heat source (2026-11-24): voxelWall switches the plate on
  // (new-project ship_material; projects without it keep the semi-infinite hull); internalHeatFlux in
  // W m-2 of hull surface heats the plate's inner layer (or the surface balance without the plate).
  veb_urban_ship_plate: VEB_SHIP({ ...SHIP_NEW, voxelWall: true }),
  veb_urban_ship_plate_internal: VEB_SHIP({ ...SHIP_NEW, voxelWall: true, internalHeatFlux: 50 }),
  veb_urban_ship_plate_wl: VEB_SHIP({ ...SHIP_NEW, voxelWall: true, waterlineHeight: 2.0, waterHeatTransferCoefficient: 500 }),
  veb_urban_ship_internal_solid: VEB_SHIP({ ...SHIP_NEW, internalHeatFlux: 50 }),
  // Soil thermal properties (2026-11-23) on the background loam (soilset: urban index 4,
  // forest index 11). SOIL_SCOPE = the former built-in loam (SCOPE cs 1180, rhos 1800,
  // lambdas 1.55; still the engine fallback), SOIL_LOAM = the new built-in loam (cs 1300,
  // rhos 1600, lambdas 1.0, sqrt(lambda C) = 1440). Both are written explicitly so the
  // cases do not depend on the example. _day runs one day (nodes 0-48) for diurnal ranges.
  veb_urban_day: { example: 'urban_block', mode: 'eVoxelEB', meteo: [0, 48], set: SOIL_SCOPE(4) },
  veb_urban_soil_day: { example: 'urban_block', mode: 'eVoxelEB', meteo: [0, 48], set: SOIL_LOAM(4) },
  veb_forest_soil: { example: 'forest', mode: 'eVoxelEB', meteo: [23, 25], set: SOIL_LOAM(11) },
  feb_urban_day: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], set: SOIL_SCOPE(4) },
  feb_urban_soil_day: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], set: SOIL_LOAM(4) },
  // Ground free convection (2026-11-23) with the Monin-Obukhov correction on: the
  // larger of the MOST and free-convection conductances (feb_urban_soil_day: neutral,
  // free convection in parallel).
  feb_urban_soil_day_most: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 48], set: {
    ...SOIL_LOAM(4), 'configuration.control.stabilityCorrection': true } },
  // Hapke background (2026-11-23) at full strength on the maize field (flat ground + one
  // simple canopy). The radiosity solve must re-emit the conserving hemispherical albedo
  // (beam base*A(mu0)/A_bh, diffuse base); the zenith-view BRF only shapes the image.
  // _hot puts the sun at the zenith (image view in the hotspot); HISTREAM_FACET_HAPKE_AUDIT=1
  // prints the ground energy closure (HAPKE_ENERGY lines). feb_crop_hapke: FacetEB state
  // spectra (energy balance and temperatures must not change).
  frt_crop_hapke: { example: 'crop_gpp_npp', mode: 'eFacetRT', set: HAPKE_FULL },
  frt_crop_hapke_hot: { example: 'crop_gpp_npp', mode: 'eFacetRT', set: { ...HAPKE_FULL, 'configuration.light.zenith': 0 } },
  feb_crop_hapke: { example: 'crop_gpp_npp', mode: 'eFacetEB', meteo: [23, 25], set: HAPKE_FULL },
  // Soil method 1 heat history (2026-11-23) over two days (nodes 0-96) on the new loam:
  // _m1_2d keeps the legacy 8-node Duhamel sum (control.soilInertiaHistory absent =
  // "truncated"), _full adds the older history as exponential modes (new-project default).
  // Day 2 noon G, Ts and G/Rn and the diurnal ranges of G and Ts are the comparison.
  veb_urban_soil_m1_2d: { example: 'urban_block', mode: 'eVoxelEB', meteo: [0, 96], set: { ...SOIL_LOAM(4), ...SOIL_METHOD_1 } },
  veb_urban_soil_m1_2d_full: { example: 'urban_block', mode: 'eVoxelEB', meteo: [0, 96], set: {
    ...SOIL_LOAM(4), ...SOIL_METHOD_1, ...SOIL_HISTORY_FULL } },
  feb_urban_soil_m1_2d: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 96], set: { ...SOIL_LOAM(4), ...SOIL_METHOD_1 } },
  feb_urban_soil_m1_2d_full: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 96], set: {
    ...SOIL_LOAM(4), ...SOIL_METHOD_1, ...SOIL_HISTORY_FULL } },
  // Soil method 2 column over three days (nodes 0-144, 30 min) on the new loam (2026-11-23):
  // the multi-day G, Ts, column heat and bottom flux are compared with method 1 + full
  // history (the infinite half-space) on the same material and meteorology. Process output
  // is off (the energy-process diagnostics are tens of MB per node).
  veb_urban_soil_m2_3d: { example: 'urban_block', mode: 'eVoxelEB', meteo: [0, 144], set: { ...SOIL_LOAM(4), ...SOIL_METHOD_2 } },
  veb_urban_soil_m1_3d_full: { example: 'urban_block', mode: 'eVoxelEB', meteo: [0, 144], set: {
    ...SOIL_LOAM(4), ...SOIL_METHOD_1, ...SOIL_HISTORY_FULL } },
  feb_urban_soil_m2_3d: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 144], set: { ...SOIL_LOAM(4), ...SOIL_METHOD_2, ...NO_PROCESS } },
  feb_urban_soil_m1_3d_full: { example: 'urban_block', mode: 'eFacetEB', meteo: [0, 144], set: {
    ...SOIL_LOAM(4), ...SOIL_METHOD_1, ...SOIL_HISTORY_FULL, ...NO_PROCESS } },
}

// Facet modes take an explicit result JSON path as the third CLI argument.
export const FACET_OUTPUT = { eFacetEB: 'faceteb.json', eFacetRT: 'facetrt.json' }
