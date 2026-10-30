# 300 m urban block example

Open it from **打开示例** in Stream Studio and run the VoxelRT configuration.

- Source: a 300 m x 300 m window (north 2800–3100 m, east 3500–3800 m) of the Beijing north 4th Ring city scene; only buildings lying entirely inside the window are kept
- Buildings: 55 footprint-extruded buildings up to 75 m, `wall` (red brick) and `roof` (concrete) meshes
- Vegetation: 345 street trees (ellipsoid canopy primitive, green leaf spectrum)
- Domain: 300 m x 300 m x 100 m, voxel 2 m, 512 x 512 sensor, bands 660 / 860 / 10500 nm
- Meteorology: `meteorology/meteo_default.txt`, kept for switching to VoxelEB
