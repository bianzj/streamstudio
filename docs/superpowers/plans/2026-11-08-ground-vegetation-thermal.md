# Vegetation shade -> ground temperature

Root cause: appended scenery only casts RGB/IR visibility shadows. FacetEB
baseline was solved without these trees; ground has only324triangles,12asphalt.
Do not paint colder pixels or invent a uniform shade temperature subtraction.

Opt-in animation groundThermal module:0.5m ground receiver grid, actual branch/
leaf meshes in existing Vulkan ray-query AS. Paired local surface-energy model
with/without canopy occlusion, same heat capacity, albedo/emissivity, convection,
deep conductance and initial state. Integrate the original synthetic36h weather/
solar history at60s; direct shade refreshed at600s,32hemisphere samples for sky/
fixed-object longwave. Full implicit scalar surface step solved by Newton.
Use computed absolute with-canopy temperature on selected near-horizontal ground
materials before adding the old impact perturbation. Retain paired delta as a
causal diagnostic. Do not subtract delta from prescribed already-cool grass.
Old native field files stay exact, buildings retain their original field; RGB/fire/
rigid/camera settings unchanged. This is an absolute local ground solution, not
a rerun of fully coupled FacetEB, leaf EB/ET, atmospheric feedback or multibounce.
Fixed current object/leaf temperatures are used for longwave occluders; history
is interpolated from the existing2h synthetic forcing, not measured weather.

Keep original shader path/128byte push when disabled; newsequence_ground variant
adds two storage buffers. Ground module owns loading/guards, physics, provenance,
temperature/flux diagnostics. Native field/base-scene input immutable.

- [x] RED native missing thermal coupling; numeric GPU fixture day/night/no-canopy
  vs independent CPU implicit roots, actual geometric occlusion, residual/lag.
- [x] Module+shared GPU layout/thermal pass and absolute temperature sampler; opt-in integration.
- [x] GREEN native ground IR changes/RGB isolation, guards, old exact fixture;
  fresh scoped review, source/budget/geometry bound checks.
- [x] Newcase vegetation_ground_thermal_20261108 with120source parent frozen;
  diagnostic temperature/shade maps and matched same-material sample statistics.
- [x] Full288frame RGB/IR videos, complete decoding, original files/settings/rigid/
  transport preservation, scoped source snapshot and honest limitations.

Verified in vegetation_ground_thermal_20261108/final-verification.json:
288frames/12s;583RGB+geometry PNGs exact;rigid/transport exact;12original
background files and1728previous native images preserved.134source files.
Ground114760valid0.5m cells;2160steps/36h;last-step max residual0.03097W/m2.
Current canopy-shadow2778cells/694.5m2;paired mean-9.9227K under this synthetic
dry-surface setup. Only paired local delta attributes canopy effects.
Independent CPU/GPU day/night/no-canopy and relocation checks passed. Encoded
frame193vsnative PSNR47.81dB(IR)/42.70dB(RGB). No field/calibration validation.
