// Engine regression cases built from the bundled examples (assets/examples).
// Each case freezes one example + mode + meteo window + control overrides so
// two engine builds can be run on identical inputs and compared.
export const CASES = {
  vrt_urban: { example: 'urban_block', mode: 'eVoxelRT' },
  vrt_urban_multiview: { example: 'urban_block', mode: 'eVoxelRT', set: { 'configuration.sensor.principalPlane': true } },
  veb_urban: { example: 'urban_block', mode: 'eVoxelEB', meteo: [23, 25] },
  veb_forest: { example: 'forest', mode: 'eVoxelEB', meteo: [23, 25] },
  feb_urban: { example: 'urban_block', mode: 'eFacetEB', meteo: [23, 24] },
  feb_crop: { example: 'crop_gpp_npp', mode: 'eFacetEB', meteo: [23, 25] },
  frt_crop: { example: 'crop_gpp_npp', mode: 'eFacetRT' },
  frt_crop_periodic: { example: 'crop_gpp_npp', mode: 'eFacetRT', set: { 'configuration.control.periodicTraversalCount': 8 } },
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
}

// Facet modes take an explicit result JSON path as the third CLI argument.
export const FACET_OUTPUT = { eFacetEB: 'faceteb.json', eFacetRT: 'facetrt.json' }
