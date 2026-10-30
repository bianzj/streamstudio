# Minimal examples

Run from this repository's root (`C:\work\streamstudio`). The standalone
`C:\work\histream` checkout was retired on 2026-10-18; this vendored copy under
`models/histream` is the maintained one.

```bat
C:\work\bin_x64\Debug\histream.exe eVoxelEB
C:\work\bin_x64\Debug\histream.exe eWaterEB
C:\work\bin_x64\Debug\histream.exe eVoxelRT
C:\work\bin_x64\Debug\histream.exe eRaytracing
```

VoxelEB surface BRDF selection is optional. Omitted values retain Lambertian
reflection.

```xml
<soilSet name="soil">
  <!-- existing thermal/hydraulic fields -->
  <brdfModel>Hapke</brdfModel>
  <hapkeB0>1.0</hapkeB0>
  <hapkeH>0.1</hapkeH>
  <hapkeG>0.0</hapkeG>
</soilSet>

<waterSet name="water">
  <!-- existing energy-balance fields -->
  <brdfModel>CoxMunk</brdfModel>
  <refractiveIndex>1.333</refractiveIndex>
  <!-- <= 0 derives slope variance from meteorological wind speed -->
  <slopeVariance>0.0</slopeVariance>
  <diffuseFraction>0.02</diffuseFraction>
</waterSet>
```

The in-memory configurations are defined in `src/base/xmlexamples.h`.
