#ifndef HISTREAM_SOLAR_STEPS_GLSL
#define HISTREAM_SOLAR_STEPS_GLSL

// Step budget of a solar-visibility ray (VoxelRT and VoxelEB voxelrad_solar).
//
// A solar ray is one straight line, so it can be followed until it leaves the
// scene instead of stopping after radiationMaxSteps (a budget meant for the
// branching diffuse paths). Each step is one closest-hit query; a straight
// line crosses at most nx + ny + nz cell boundaries of the grid. With periodic
// boundaries it crosses copies until it climbs above the grid top, which takes
// a ray parameter of height / d.y. The budget doubles that count (coincident
// faces of neighbouring voxels) and adds a margin; it never falls below the
// configured step count, so rays that ended within radiationMaxSteps are traced
// exactly as before. Reaching this budget is a geometric anomaly and is counted
// in the VoxelEB solar diagnostics.
//
// Requires traceray_comp.glsl (PeriodicSceneTop, periodicCrossingLimit).

// A solar ray stops once its transmittance is at or below this value and keeps
// it: the visibility is then too high by at most the cutoff, so the direct
// irradiance error of the voxel is <= SOLAR_TRANSMITTANCE_CUTOFF * E_beam.
const float SOLAR_TRANSMITTANCE_CUTOFF = 1.0e-4;
int SolarStepLimit(vec3 direction, int configuredSteps)
{
  vec3 cells = max(vec3(setting.voxelSize), vec3(1.0));
  vec3 d = abs(normalize(direction));
  float crossings = cells.x + cells.y + cells.z;
  if(periodicCrossingLimit(setting.periodicNeighborCount) > 0)
  {
    float top = PeriodicSceneTop();
    float height = (top < 1.0e20 ? max(top, cells.y) : cells.y) + 2.0;
    crossings = height + height / max(d.y, 0.01) * (d.x + d.z);
  }
  float limit = min(2.0 * crossings + 16.0, 1.0e6);
  return max(configuredSteps, int(limit));
}

#endif
