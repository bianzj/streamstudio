# 300 m urban block example

Open it from **打开示例** in Stream Studio and run the VoxelRT configuration.

- Source: a 300 m x 300 m window (north 2800–3100 m, east 3500–3800 m) of the Beijing north 4th Ring city scene; only buildings lying entirely inside the window are kept
- Buildings: 55 footprint-extruded buildings up to 75 m, `wall` (red brick) and `roof` (concrete) meshes
- Vegetation: 345 street trees (ellipsoid canopy primitive, green leaf spectrum)
- Domain: 300 m x 300 m x 100 m, voxel 2 m, 512 x 512 sensor, bands 660 / 860 / 10500 nm
- Meteorology: `meteorology/meteo_default.txt`, kept for switching to VoxelEB

The VoxelEB and FacetEB runs use the sensible heat exchange of the
[Town Energy Budget scheme](https://link.springer.com/article/10.1023/A:1002463829265)
when `configuration.control.urbanAerodynamics` supplies positive `buildingHeight`
and `streetWidth` in metres. This example uses 31 m (roof-area weighted height)
and 31.5 m (effective width from the wall-to-open-plan-area ratio). Its idealized
wind forcing uses a 45 m reference height, above the representative roof level.
Use the actual above-roof wind measurement height for a calibrated project.

Roofs exchange with the reference air (neutral coefficient, which vanishes in
calm air). Outdoor roads and walls share one diagnostic canyon air temperature
per scene and time step, with a coefficient that includes the canyon convective
velocity. Roads use one aerodynamic resistance for sensible and latent heat.
Street-tree leaves and the ground under them exchange sensible heat with the
canyon air; vapour pressure stays at the reference value. FacetEB weights the
canyon balance by triangle area, and VoxelEB by voxel face area. The rest of
the model supplies radiation, conduction, and latent heat exchange. Roads use the
background soil material, so they evaporate like moist soil; raise its surface
resistance for an impervious road.
