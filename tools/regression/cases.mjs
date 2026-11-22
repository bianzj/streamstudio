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

export const CASES = {
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
}

// Facet modes take an explicit result JSON path as the third CLI argument.
export const FACET_OUTPUT = { eFacetEB: 'faceteb.json', eFacetRT: 'facetrt.json' }
