# HiStream scene animation module

## Status and positioning (2026-11-12, development build under test)

**Research module, integrated into the Stream Studio development build (testing, not released; not in the 2.0.1 installer)** as the GUI job "场景动画（研究功能）" (server API,
`SCENE_ANIMATION_MODE` job document in `project-schema.js`, user-manual chapter, one built-in example). The
command line (`histream.exe eSceneAnimation input.json new-output-dir`, `run_case.py`) remains available.

**The GUI entry is hidden for now** (user decision 2026-11-12): `SCENE_ANIMATION_UI_ENABLED = false` in
`project-schema.js` removes the menu item and its "研究功能" group, keeps the panel from opening from any caller
(menu, example, shortcut) and filters the example out of the catalog listing; `openExample` also refuses
scene-animation entries. Nothing is dispatched or run from the interface. The engine CLI and the local
`/api/scene-animation/*` service stay available. Set the constant to `true`, rebuild the web bundle, and the menu
item, the example and the panel come back.

Three motion backends, all labelled at the top of every `run-manifest.json` (`physicsValidated: false`,
`impactModel.kind/label/predictive`) and in packaged video titles:

- `authored` and `rigid-v1`: **authored effect demonstration, not a physical prediction** ("作者设定，非物理预测").
  Breach boxes, launch velocities/delays, fireball and heat sources are preset; `rigid-v1` integrates contact
  dynamics of those authored initial conditions.
- `impact-v1` (new): **empirical engineering estimate, not independently validated** ("经验模型估算，未经独立验证").
  Projectile, charge and wall parameters drive public empirical formulas (Young penetration, Kingery–Bulmash blast
  load, impulse failure of preset blocks, rigid fragments, energy-budget fireball/heat/dust); see
  [impact-v1](#impact-v1-empirical-event-model-2026-11-12). No structural FEM, detonation CFD or combustion chemistry.

`warnings[]` lists conditions that weaken a run (thin-skin static colliders, a contact outside the authored
breach, a failed track-cache write; for impact-v1 also fit-range clamping, budget violations, no failed block,
Young validity range).

Performance (2026-11-12): moving geometry keeps one persistent adapter per triangle count (BLAS/TLAS rebuilt in
place; `persistentGeometry: false` restores the per-sample rebuild); strips of a sample go in one submission
(`batchedStrips: false`). New in the development build, see [performance options](#performance-options-s3s4s6-2026-11-12):
S3 `instancedGeometry` (one BLAS per rigid part, TLAS instance updates), S4 `additionalCameras` (one
transport/rigid/timeline update per sample time for several cameras), S6 `checkpoints` (partial-frame runs with
the sensor on resume from the nearest checkpoint instead of rendering every warm-up frame), and transport
statistics read through host-cached memory and scanned on an ordered worker thread. Default outputs of the
authored/rigid-v1 backends are byte-identical to the previous engine. Still open: S7 local substeps.

## Current capabilities and unified entry (2026-11-09)

See [当前功能与命令](CURRENT_FEATURES.md) and the checked
[machine-readable catalog](simulation/capabilities.json).
`python tools/scene-animation/run_case.py features` lists current code and limits;
`check JOB.json` resolves a job without running a solver; `run JOB.json` calls
the existing native-scene or coupled-town pipeline into a new output directory.
Examples are in [samples/](samples/). Existing detailed entry points below remain valid.
The latest coupled FacetEB core/surface/scene implementation is separate from
the older prescribed-temperature and independent ground-model paths.

Active source: `C:/work/streamstudio/models/histream/src/animation/`.
Original shared services and RT/EB solvers stay in their existing folders.
The CLI facade is the only dependency from `main.cpp` into this module.
Historical outputs remain under
`D:/cases/StreamStudio/supplemental/impact_gpu_20261107/visual_20261108/`.

## Build

Use the existing Visual Studio/vcpkg/Vulkan build environment:

```powershell
cmake -S C:/work/streamstudio/models/histream -B C:/work/streamstudio/build/histream-animation -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/work/vcpkg/scripts/buildsystems/vcpkg.cmake -DHISTREAM_ENABLE_SCENE_ANIMATION=ON -DOUTPUT_PATH=C:/work/streamstudio/build/histream-animation/runtime
cmake --build C:/work/streamstudio/build/histream-animation --config Release --target histream histream_runtime_resources --parallel 6
```

`HISTREAM_ENABLE_SCENE_ANIMATION` defaults to OFF. Use a separate clean build
and `OUTPUT_PATH` for core-only builds; the core deployment excludes animation
resources. Deployment never removes existing user resources from reused stages.

## Entry points

```powershell
# Procedural reference: existing JSON settings and frame output remain supported.
C:/work/streamstudio/build/histream-animation/runtime/Release/histream.exe eSceneAnimation animation.json new-run

# New static mesh reference assets (output must be a new directory).
python C:/work/streamstudio/tools/scene-animation/make_mesh_case.py D:/cases/StreamStudio/new-mesh/assets
C:/work/streamstudio/build/histream-animation/runtime/Release/histream.exe eSceneAnimation D:/cases/StreamStudio/new-mesh/assets/street-block.json D:/cases/StreamStudio/new-mesh/static

python C:/work/streamstudio/tools/scene-animation/render_video.py native-animation-run --ffmpeg C:/path/to/ffmpeg.exe
```

`compare_physics.py` compares native procedural/transport frames;
`compare_visuals.py` packages the earlier visual comparison. These tools do not
calculate thermal, damage or structural physics.

## Mesh contract, schema version 1

- Set `backend: "mesh"`; absence selects the procedural reference. Unknown
  backend names fail. `mesh` renders static views; `meshSequence` adds the timeline below.
- World coordinates are right-handed Z-up metres. Each asset declares `units`
  and `upAxis` (`Y` or `Z`). Y-up is converted by `(x,y,z) -> (x,-z,y)`.
- Assets have unique `id`, OBJ `path` relative to the scene JSON, and required
  lowercase SHA256. The loader uses the same bundled tinyobj library as HiStream;
  a fresh parser per asset retains face material IDs and vertex normals.
- Materials explicitly supply `id`, linear RGB `color`, `emissivity` and
  `temperatureK`. OBJ `usemtl` names must match the table. External MTL optical
  values and textures are not loaded. A missing file, unknown material, bad hash,
  degenerate face or invalid transform fails before output creation.
- Instance `transform` contains 16 row-major affine asset-to-world values,
  applied after axis conversion. Positive nonsingular determinant is required;
  normals use inverse transpose. Geometry is baked once into a world mesh and
  cached in one BLAS/TLAS for all views. A verified 12-byte position stride avoids
  the older helper's assumed 44-byte vertex layout. Original services are unchanged.
- Cameras declare safe `id`, `position`, `target`, `up`, `fovYDegrees`.
  IDs are unique without regard to case so Windows filenames cannot collide.
  `samples` is the antialiasing grid side length (1..3); hit diagnostics always
  use a separate pixel-centre ray. There are no hardcoded buildings in the shader.
- `thermal` declares `wavelengthMicrometers`, `displayMinK`, `displayMaxK`, `skyK`.
  Greybody spectral radiance is `epsilon B(T) + (1-epsilon) B(Tsky)`.
  The fixed white-hot display uses linear radiance bounds. Supplied material
  temperatures are not an energy-balance solution. This phase has no sensor PSF/AGC.
- Each view produces visible, thermal, normal and material PNGs plus float32
  antialiased radiance, centre-ray radiance, centre-ray temperature (K) and
  eight-channel hit data. `run-manifest.json` records units/layout,
  device, transforms, materials, hashes and scope. `provenance/replay.json` and
  snapshotted OBJ files can replay the same scene with the built runtime.

The generator creates original CC0 architectural reference geometry with slabs,
columns, room partitions, recessed opaque window panes, frames, canopy, parapets
and roof equipment. These synthetic meshes are not surveyed buildings. They
contain no blast, penetration, damage or structural mechanics. Background thermal
fields are available through the optional adapter below. Continuous native mesh
animation uses `meshSequence`.

## Native background temperature field

`thermal/background_field.*` loads an immutable `histream-background-field`
schemaVersion 1 file. A scene selects it with:

```json
{
  "backgroundField": {
    "path": "fields/node_18.json",
    "sha256": "SHA256_OF_METADATA_FILE",
    "expectedTimestamp": "2019-06-29T12:00:00+08:00"
  },
  "thermalPerturbations": [{"instance": "target", "deltaKelvin": 0}]
}
```

The field declares K, right-handed Z-up metres, timestamp/time convention,
asset/material/instance identities, 100% triangle coverage, solver revision and
hashes for native project, forcing, history, geometry, state, run index, engine,
source receipt, mapping and pre-solve input receipt. It transfers front/back
per-side mean temperatures with no interpolation. Mapping tolerance is 0.1 mm.
Each triangle retains its immutable front/back baseline plus a separate supplied
deltaK, added once. Missing fields never silently fall back to material presets.

Preparation converts the identical geometry `(x,y,z)->(x,z+0.6,-y)` and binds
every material through explicit OBJ groups. Native FacetEB adds two soil boundary
triangles 0.61 m below the mesh; those are recorded and excluded from transfer.
Native output vertex positions and winding must match the rendered geometry.

```powershell
python C:/work/streamstudio/tools/scene-animation/prepare_thermal_case.py SCENE_JSON NEW_NATIVE_CASE
python C:/work/streamstudio/tools/scene-animation/run_thermal_case.py NEW_NATIVE_CASE C:/work/streamstudio/build/histream-core/runtime/Release/histream.exe --layers 256
python C:/work/streamstudio/tools/scene-animation/export_background_field.py NEW_NATIVE_CASE NEW_FIELDS --nodes 18 22
python C:/work/streamstudio/tools/scene-animation/verify_background_render.py NATIVE_MESH_RENDER
```

The runner freezes referenced input hashes before the solve and verifies them
afterwards; the exporter checks them again. Production guards use explicit
exceptions and remain active under `python -O`. An export with invalid inputs
fails before creating output. Native convergence must pass, and the renderer
also checks the selected node/time and input receipt. Replay snapshots include
field bytes and source evidence. Sky reflection uses the equivalent blackbody
temperature of the same supplied native incoming longwave flux.

The demonstration uses synthetic weather, 48 h at 2 h intervals, 24 h of initial
warmup and idealized opaque two-sided facet heat capacities. This is not a
multilayer building heat-conduction model. Adjacent triangles are independent
thermal nodes; unresolved within-face sun/shade mixtures and visible triangular
temperature changes can remain. No spatial smoothing is applied to hide them.
The generic greybody renderer does not reproduce the native FacetRT spectral
radiosity solution. Numerical temperature/radiance checks are not field accuracy.

`*_radiance.f32` is the AA sample mean used for the image. `*_centerRadiance.f32`
corresponds exactly to `*_hits.f32` and `*_temperatureK.f32`; it avoids comparing
an AA mixture at a thin window frame against the temperature of only one ray.

## Targeted checks

Requires Python with NumPy/Pillow and the built native GPU runtime. Always choose
fresh test output directories:

```powershell
python C:/work/streamstudio/tools/scene-animation/tests/test_animation.py ENGINE NEW_OUTPUT
python C:/work/streamstudio/tools/scene-animation/tests/test_transport.py ENGINE NEW_OUTPUT
python C:/work/streamstudio/tools/scene-animation/tests/test_sensor.py ENGINE NEW_OUTPUT
python C:/work/streamstudio/tools/scene-animation/tests/test_mesh.py ENGINE NEW_OUTPUT
python C:/work/streamstudio/tools/scene-animation/tests/test_background_field.py ENGINE NEW_OUTPUT
python C:/work/streamstudio/tools/scene-animation/tests/test_thermal_export.py COMPLETED_NATIVE_CASE NEW_OUTPUT
```

The first three retain input protection, deterministic seeking, transport and
sensor checks. The mesh test uses an asymmetric 2/4/6 m box with distinct face
materials, axis conversion, rotation/nonuniform scaling and replay. Analytic hit
distance tolerance is 0.1 mm, normal tolerance 1e-5, spectral-radiance tolerance
3e-5 W m-2 sr-1 um-1. These are numerical/interface checks, not field validation.

## Continuous impact/explosion illustration

```powershell
python C:/work/streamstudio/tools/scene-animation/prepare_mesh_sequence.py BASE_SCENE_WITH_BACKGROUND NEW_ASSETS
C:/work/streamstudio/build/histream-animation/runtime/Release/histream.exe eSceneAnimation NEW_ASSETS/sequence.json NEW_SEQUENCE
python C:/work/streamstudio/tools/scene-animation/package_mesh_sequence.py NEW_SEQUENCE NEW_PACKAGE --ffmpeg FFMPEG_EXE
python C:/work/streamstudio/tools/scene-animation/tests/test_mesh_sequence.py ENGINE NEW_ASSETS/sequence.json NEW_TEST_OUTPUT
```

The canonical generator emits a closed remainder, closed fracture chunks and
explicit original-face links. It supports the original box-constructed reference
building and identity target transform, and rejects other inputs. The cut boxes
are authored geometry, not a damage-radius calculation. Surviving remeshed faces
inherit the original face triangle mean at their centroid; faces crossing an old
diagonal do not retain an exact piecewise native thermal field. New fracture and
fragment heating is supplied, separate from the native background field.

`sequence/mesh_timeline.*` keeps the intact target until impact, then continuously
moves the removed chunks through the opening. After a prescribed clearance path,
chunks follow gravity toward authored non-overlapping rotated landing footprints
on the actual ground triangles, and stop inelastically. Their lateral velocities
are chosen to reach those endpoints. This is preset scatter, with no structural
response, fragmentation, blast-energy or in-flight rigid-body collision solver.
The contact receipt checks actual transformed vertices and resting footprint
support/overlap. Impact switching uses the same quantized1/60s step as transport.

`render/mesh_sequence.*` caches the background BLAS and updates target/fragments.
The existing low-speed heat/tracer numerics are shared in
`shader/animation/transport_solver.glsl`. The mesh variant uses a64x56x56,1m
coarse obstacle mask sampled with six rays per cell from the intact/remainder
geometry, switching at impact. Moving chunks are render-only at this flow scale.
Wall perturbations interpolate solid cells only; hot air is never used directly
as wall temperature. Fracture/fragment supplied heat excludes that wall term.

Thermal volume transfer combines transported air temperature/tracer with an
authored near-field fireball, fine billow warp and settling-dust appearance.
Those appearance details are not resolved combustion/turbulence/settling physics.
The projectile trajectory is an authored visualization; no guidance, penetration,
blast or weapon performance is inferred.

`frames/` contains generic SceneSensor PSF/noise/AGC output. `fixed_frames/` keeps
the same logarithmic spectral-radiance display scale for every frame. Selected
unsaturated float32 radiance and surface-temperature samples remain in
`radiance/`; `geometry/` holds corresponding visible mesh views. No atmosphere or
camera calibration is claimed. The packager captions already rendered frames,
encodes H.264 and requires complete frame-count decoding. An old/new comparison
has different source geometry/view and is qualitative.

First/last-frame seeks warm the transport and, when enabled, the sensor from
time zero. With sensor disabled, direct geometry inspection renders only the
requested range. All output directories must be new. Input meshes, links,
fragment table, background evidence, loaded SPIR-V and exact executable are
snapshotted under `provenance/` for replay with the complete runtime.

### Separated projectile, jet and flight smoke

Set `sequence.projectileAppearance` to `"separated-v2"` to enable a distinct
334 K body, 342 K nose, 330 K fins and 600 K nozzle. The tapered supplied jet
starts behind the nozzle. A moving source feeds heat/tracer into the existing
GPU transport before impact, including an explicit 0.3 s initial wake history.
The 1 m grid resolves a broad smoke wake; the fine jet remains an appearance
source. Cooler particulate emission is prescribed separately from air heat,
and constituent emissions are mixed as radiance. This does not add a particle
energy, combustion or motor-flow solver. Omitting the option retains legacy
geometry and effects.

Optional `sequence.sensorOptions` fields are `binning` (1..4), `blurSigma`
(0..2 detector pixels), `noiseKelvin`, `fixedPatternKelvin` (0..5),
`agcMinimumSpanKelvin` (1..1000) and `logarithmicDisplay` (boolean). Log mode
includes the maximum source radiance in the AGC upper target before applying
the configured temporal response. These display settings leave float radiance
and the separate fixed-scale frames unchanged. A `trackingCameraOffset` world
vector enables a 35 degree flight closeup; omit it for the configured fixed view.

```powershell
python C:/work/streamstudio/tools/scene-animation/tests/test_projectile_plume.py ENGINE SEQUENCE NEW_TEST_OUTPUT
```

This fixture checks pre-impact tracer accumulation, body/nozzle temperatures,
deterministic camera/AGC seeking, and a volume-ablation radiance signal beyond
the analytic jet. It is a numerical and rendering check, not physical validation.

### Optional geometric contact and rigid fragments

Configure the existing animation build with `-DHISTREAM_ENABLE_ANIMATION_COLLISION=ON`.
This explicitly downloads SHA256-pinned Bullet3.25; the core/default build does
not download or link it. Build `histream`, `histream_runtime_resources` and
`histream_collision_tests`. The fixture executable is beside `histream.exe`.

Generate a separate preset with `prepare_mesh_sequence.py SCENE NEW_ASSETS --disjoint`
before setting `sequence.motionBackend` to `"rigid-v1"` and
`projectileAppearance` to `"separated-v2"`. The original visual architecture has
overlapping frame/glass volumes; disjoint mode partitions their removed-volume
union before fragment creation. Initial overlapping rigid proxies are rejected.
Omit `motionBackend` to keep the original authored animation.

First contact is a convex sweep of the rendered body/fins against actual intact
triangles. Fixed orientation and the authored linear path are retained; the
query extends the nominal endpoint by2m to find contact. Surface contact and
trajectory origin remain separate for side contacts. This contact synchronizes
the existing heat sources and coarse flow-mask switch.

Fragments use native **CPU** double-precision Bullet rigid boxes with a1/1920s
base clock and up to16 adaptive subdivisions for fast translation/rotation,
60 constraint iterations, gravity, friction and restitution. Static colliders are
partitioned per part (2026-11-12): consecutive closed convex 12-triangle boxes
(the canonical layout) are kept unchanged; otherwise every closed convex
12-triangle window and every 12-triangle welded-vertex component is still kept
as a solid convex part, and only the triangles that remain become a thin
two-sided skin (`solver.staticCollider` = `closedConvexParts` | `mixed` |
`triangleMeshSkin`, details in `solver.staticLayout`, and a line in the
top-level `run-manifest.json` `warnings[]`). One odd part no longer turns the
whole scene into a skin. `sequence.rigidRequireSolidStatic: true` refuses any
skin triangle, and it is checked before a cached track is replayed.
Built-in sphere CCD has zero permitted advance; a full-box static sweep guards
fast corner motion, delegating existing contacts to the ordinary constraint
solver. Dynamic rotational CCD is not guaranteed. Supplied release times,
velocities, spin,2300kg/m3 density, friction.65 and restitution.12 are visual
initial conditions, not measured materials or impact-energy predictions.

HiStream **GPU** IR/smoke rendering stays in place. `rigid.csv` records numerical
contact samples, ground/contact penetrations are reported separately, and
`timeline.json` identifies the solver and computed contact. The preset opening,
fracture and supplied temperatures remain unvalidated illustrations.

#### Flight speed, speed guard and breach consistency (2026-11-11)

- `sequence.flightSpeedMetersPerSecond` (1..2000) sets the constant flight speed
  directly and derives the nominal contact time from the path length; it is
  mutually exclusive with `impactTime`. Omitting it keeps the old behaviour, where
  speed is path length / `impactTime`. The speed and its source are written to
  `timeline.json` under `flight`. Existing presets (31.6 m in 3 s) are about
  10 m/s, i.e. a slow-motion visualization, not a weapon speed.
- After release, fragments gain no energy except gravity and the supplied launch
  velocity. `rigid-v1` computes a free-fall bound sqrt(v0^2 + 2 g H) from the fragment
  table and ground mesh and limits every body each substep to
  `rigidSpeedGuardFactor` (default 1.25, `0` = off, otherwise 1..5) times that bound.
  Only linear speed is limited (a trial that also limited angular velocity made thin
  bodies sink 45 mm into the ground, versus 0.7 mm without it). Raw peaks
  before limiting, the body and time, clamp count and the number of guarded
  bodies are in `timeline.json` (`solver.rawPeak*`, `speedGuard*`), and
  `rigid.csv`. Cause of the earlier 49.6 m/s artefact: a 4.5 x 7 x 1 cm, 0.07 kg
  frame sliver (fragment 75, spin about 900 rad/s) pinched between tonne-scale glass
  blocks. With the guard, 1 of 178 bodies is clamped 20 times. The guard bounds
  the symptom; revise presets with many slivers (`fragmentCensus`).
- `breach` in `timeline.json` states whether the computed contact lies inside the
  authored breach bounds (+0.25 m). The opening, fireball, dust, heat pulse and
  fragment exit directions are authored in world axes for a -Y facade and do
  not follow the contact point or normal. A contact outside the bounds prints a
  warning (also in `run-manifest.json` `warnings[]`); `requireContactInBreach: true` turns it into an error.
- `sequence.fragmentExit = {"normal":[nx,ny],"planeOffset":s}` (authored backend only) replaces the
  hard-coded -Y exit plane y=-2.8 by the plane dot(normal, xy) = s; fragments leave the wall towards that
  plane and land from it. Omitted, the default (-Y, 2.8) runs the original expressions, so old cases stay
  byte-identical. Receipt: `timeline.json` `fragmentExit`. The launch velocities in the fragment table are
  not rotated; rotate them in the preset for another facade.
- `prepare_mesh_sequence.py ... --disjoint --min-fragment-thickness 0.05` keeps removed
  pieces thinner than the given thickness as static structure instead of rigid bodies
  (default 0 = unchanged output). On the canonical building this keeps 39 of 178 pieces
  static (139 rigid fragments). Same 12 s run, guard off: peak fragment speed
  22.7 m/s (limit for a collision bump is (1+restitution) x 20.2 = 22.6 m/s), contact
  penetration 4.8 mm, ground penetration 0.7 mm, no guard action. With the default preset
  and no guard the peak was 49.6 m/s; with 2 cm it was still 36.2 m/s. Recommended for new
  cases; the 4.5 cm window-frame bars then stay as posts in the opening. Existing cases
  are not regenerated.
- `sequence.fragmentRelease = {"mode":"distance","propagationMetersPerSecond":10..2000,
  "onsetSeconds":0..0.5}` replaces the preset's pseudo-random release delays with
  onset + distance(fragment centre, contact) / speed, so fragments leave as an
  authored disturbance front from the actual contact point (delay must stay <= 1 s). Receipt:
  `fragmentRelease` in `timeline.json`. Default is the preset table.
- Fireball, dust, heat pulse and their volumes follow the horizontal part of the computed
  contact normal (`effectOrientation.outwardAzimuthRadians`; push-constant/Effects slots
  `sourcePoint.w` and `wind.z`). For a facade hit (normal -Y) the azimuth is 0 and the
  shaders take the original code path: all 288 frames and `transport.csv` of the reference
  case are byte-identical to the previous engine. Roof/ground hits keep the default.
  The breach and fragment table are not rotated, and fire-source profiles still assume
  X-horizontal/-Y-outward axes.
- **Rigid track cache** (`sequence.rigidCache = {"directory": "...", "enabled": true}`, relative paths
  resolve against the sequence JSON). The first run records every base-step pose and the solver statistics
  to `rigid_<key>.track`; any later run with an identical physical input replays them and never builds the
  Bullet world. The key hashes the solver revision, Bullet version, step/gravity/friction/restitution/speed
  limit, every static triangle and every body (centre, size, velocities, release time, density), so changing
  the fracture set, flight path, speed guard or release schedule always misses the cache, while camera,
  appearance, sensor, resolution and frame range changes hit it. A truncated, wrong-size or wrong-key file is
  ignored and re-recorded. The temporary file name includes the process id and a timestamp, so two concurrent
  recorders cannot interleave writes. The track is written before the receipts and `solver.trackCache.writeOutcome`
  says what happened (`written`, `skipped-existing` when a valid track with at least as many steps already
  exists, `failed` with `writeDetail`; a failure is also a top-level warning). A shorter recording never replaces
  a longer track, a rename blocked by a concurrent reader is retried with back-off, and `rigid_*.tmp` files older
  than a day are removed before recording. There is still no cross-process lock: two recorders of the same key
  compute twice, but their deterministic content is identical.
  `timeline.json` reports `solver.trackCache` (`mode` record/replay, key, file). Replay is byte-identical to a
  live solve: all 288 frames, `rigid.csv`, `transport.csv` and all solver statistics of the thin-preset case
  matched (30 of 30 fields). Same case, 192x108: 88 s recording, 30 s replay (rigid sampling 55.6 s -> 0 s).
  A 12 s track is about 66 MB; delete the cache directory to reclaim it. Bump `kSolverRevision` in
  `rigid_scene.cpp` whenever stepping, contact handling or the limiter changes.
- **Threaded output**: PNG encoding runs on `outputThreads` workers (default min(4, cores); 0 = synchronous;
  0..16) and the radiance copy/tone mapping on row chunks. Queued pixel copies are bounded by a 512 MiB byte
  budget (a single task may exceed it when nothing else is in flight), so `outputThreads=16` at 4K does not
  queue gigabytes. Output bytes do not depend on the thread count (checked against 288 frames at 1280x720 with
  the sensor on). `run-manifest.json` reports `profileSeconds` (transportAdvance, timelineFrame, adapterBuild,
  renderDispatch, observation*, sensorProcess, readbackTonemap, pngSubmit, logging, rigidSample) so the next
  bottleneck is measured rather than guessed.
- Ground-coverage diagnostics no longer abort a render; uncovered samples are
  counted in `outsideGroundCoverageSamples`. `solver.staticCollider` reports
  `closedConvexParts` or `triangleMeshSkin` (with a warning) for the static scene.
  `sequence.rigidRequireSolidStatic: true` turns that fallback into an error, so a new
  scene cannot silently lose wall solidity when one group is not a closed convex box.

### impact-v1 empirical event model (2026-11-12)

`sequence.motionBackend: "impact-v1"` with a `sequence.impactPhysics` block replaces the authored impact
inputs by an empirical estimate (`impactTime`, `flightSpeedMetersPerSecond`, `fragmentRelease` and
`fragmentExit` are refused; `projectileAppearance: "separated-v2"` and the preset fracture assets of
`rigid-v1` are required; natural/varied/ground/sustained fire and scenery are not combined with it).
All parameters below are generic example values, not data of any specific projectile or charge.

```json
"impactPhysics": {
  "projectile": {"massKg": 250, "diameterM": 0.34, "lengthM": 3.0, "speedMetersPerSecond": 250,
                 "trajectory": "straight", "noseCoefficientN": 0.85, "powered": false},
  "charge": {"massKg": 100, "tntEquivalence": 1.0},
  "fuze": {"mode": "delay", "delaySeconds": 0.005},
  "targetMaterial": {"wall": {"type": "concrete", "thicknessM": 0.3, "youngS": 0.9,
                              "criticalImpulsePaS": 2000, "densityKgPerM3": 2300}},
  "fragments": {"minimumMassKg": 2, "minimumThicknessM": 0.05},
  "energySplit": {"blast": 0.6, "heat": 0.3},
  "dust": {"massFraction": 0.01, "massExtinctionM2PerKg": 200},
  "fireball": {"peakTemperatureK": 2000},
  "ambientTemperatureK": 288, "burst": "free-air", "presentationTimeScale": 0.2
}
```

Chain (all in `event.json`, written before `run-manifest.json`):

1. **Flight** at the physical speed to the swept first contact; projectile surface temperatures from the
   adiabatic recovery temperature (r = 0.89 body, 1 at the nose).
2. **Penetration**: Young (SAND97-2426) concrete equation over the oblique path thickness; constant
   deceleration in the wall; the burst point is reached after `fuze.delaySeconds` (or at contact for
   `instant`), behind the wall when the wall is perforated.
3. **Blast load**: Kingery–Bulmash fits (UFC 3-340-02 free-air / hemispherical surface burst) at the
   point of each preset block nearest the burst; oblique reflection at the entered face; Z outside the fit
   range is clamped and reported.
4. **Failure**: a block fails when it lies in the penetration channel, below the fit range, or when its
   reflected impulse exceeds `criticalImpulsePaS`. Failed blocks below the minimum mass/thickness are
   pulverised (dust only); the others become rigid-v1 bodies released at their blast arrival time with
   v = (i_r − i_c)·A/m along burst → block centre. Intact blocks stay static colliders.
5. **Energy budget**: charge energy W·4.184 MJ/kg split into blast (used only through the load), heat
   injected once into the transport air as a Gaussian (σ = fireball D/4, widened if the peak would exceed
   the solver clamp) and an unmodelled remainder; fragment kinetic energy must stay within the blast share
   (warning otherwise). The dust tracer amount is failed volume × density × dust fraction.
   `run-manifest.json.impactHeatClosure` compares the measured air excess heat over the injection step with
   the injected share.
6. **Fireball** (render variant `sequence_impact`): diameter 3.86 W^0.32 m and duration 0.299 W^0.32 s,
   exponential cooling from `peakTemperatureK`, Planck emission of an optically thick volume.
7. **Camera shake** (`cameraObservation.jitterOnImpact`): starts at burst + Kingery–Bulmash arrival time at
   the camera distance, amplitude scaled by min(1, Ps / 6.9 kPa); `blastArrival: "blast"` is the default
   with impact-v1 and is refused for the other backends.
8. **Time**: physics runs on physical time; `presentationTimeScale` (0.001–1) is the number of physical
   seconds per video second, so frame n shows physical time (n−1)/fps × scale. `frames.csv` gains a
   `physical_time_seconds` column, the manifest reports `impactModel.presentation`, and packaged videos
   carry "经验模型估算，未经独立验证 · 慢放 ×N" and the physical time stamp.

Labels: `physicsValidated: false`, `impactModel.kind: "impact-v1"`, `predictive: false`, label "empirical model
estimate, not independently validated". Formula fixture: `histream_impact_tests` (157 checks against IATG
01.80, UFC/JRC worked examples, Swisdak fits, Young's report equations, Planck and recovery temperature);
engine checks: `tests/test_impact_v1.py`. Not modelled: structural FEM, detonation/compressible CFD,
combustion chemistry, casing fragments, ground shock, reinforcement, crater.

### Performance options (S3/S4/S6, 2026-11-12)

| Option | Default | Effect |
|---|---|---|
| `sequence.instancedGeometry` | `false` (`true` for impact-v1) | One BLAS per rigid part (static target, projectile, each fragment), built once per phase; each sample only rewrites instance transforms (small TLAS rebuild) and the triangle records. Uses the `*_instanced` shader variants (instance custom index = first triangle of the part). Images match the default path within float rounding (mean difference < 0.001 grey levels in the check). |
| `sequence.additionalCameras` | none | Up to 7 `{"id", "camera", "trackingCameraOffset"?, "cameraMount"?, "lastFrame"?, "cameraObservation"?}`. All cameras share one transport/rigid/timeline update per sample time and render into `cameras/<id>/` with their own sensor AGC and observation state. The primary output is byte-identical to a single-camera run, and each additional camera to its own single-camera run. |
| `sequence.checkpoints` | none | `{"directory", "everyFrames": 24}`: writes transport cells/step, the obstacle-mask switch and every camera's sensor AGC and detector histories at every N-th frame (key = input minus frame range/output options + engine and shader hashes). A later run of the same scene with `firstFrame > 1` resumes from the nearest earlier checkpoint instead of rendering every warm-up frame; resumed frames are byte-identical to the full run. Not available with `groundThermal`. |

Transport statistics: the per-frame GPU copy goes to host-cached staging memory and the CPU scan plus the
`transport.csv` row run on one ordered worker thread (same values, same row order).

### Case videos from existing frame folders (2026-11-12)

`compose_case_videos.py` turns any scene-animation frame folder into a captioned MP4, and can place two folders
side by side (or stacked) for a per-frame comparison; the caption bar carries the title, the frame number, the
physical time and the slow-motion factor. `--fps 120 --time-scale 1` re-encodes at the sampling rate for a
real-time clip because impact-v1 frames are 1/120 s of physical time apart. Example:

```
python compose_case_videos.py --frames case01/result/frames --second-frames case02/result/frames \
  --out compare.mp4 --title "10 um (case 01)" --second-title "3.8 um (case 02)" --time-scale 0.2 --ffmpeg ffmpeg.exe
```

Encoder selection goes through `video_encoder.py` (libx264, then libopenh264, then mpeg4).

### Visible RGB images and video

Set `sequence.optical` to `true` to additionally export `optical_frames/`.
The same native GPU dispatch retains the existing IR and geometry channels,
and uses a separate packed RGB channel for material/sun/shadow/sky shading,
single-scatter smoke/dust appearance, supplied jet/fire emission and local fire
light. A fixed exposure1.6, Reinhard tone curve and gamma2.2 produce the display.
Camera, physics clock, meshes, contact event and transported tracer are shared.
This is an illustrative RGB appearance model, not measured spectral BRDF,
combustion, volume shadowing, atmosphere or a calibrated visible camera.

```powershell
python C:/work/streamstudio/tools/scene-animation/tests/test_optical_sequence.py ENGINE SEQUENCE NEW_TEST_OUTPUT
python C:/work/streamstudio/tools/scene-animation/package_mesh_sequence.py NEW_RUN NEW_VIDEO --ffmpeg FFMPEG --optical --comparison-video PRIOR_INFRARED_VIDEO
```

The visible package contains `histream_mesh_impact_visible.mp4`, `stages.jpg`,
and (for rigid sequences) `optical_collision_closeup_slow.mp4`. The detail clip
is a crop/enlargement at half playback rate; it adds no spatial or temporal
samples. An optional comparison places the supplied IR video at left and the
new RGB video at right. Verify matching cameras/physics separately. Existing
default packaging remains infrared.

### Varied fire and contact-triggered observation (opt-in)

Use `sequence.appearanceVersion="varied-v3"` with physical RGB enabled. Each
supplied fire source can select `flameStyle` (`window-licking`,
`window-billowing`, `ground-pool`) and integer `seed` in1..999999. These profiles
change flame shape, rhythm, local lighting and the supplied smoke release
region. Facade profiles assume this example's X-horizontal/-Y-outward axes.
`tracerRate` remains a normalized supplied rate, not a measured mass flux.
Old legacy/natural-v2 paths and their defaults remain available.

```json
"cameraObservation": {
  "enabled": true,
  "jitterOnImpact": true,
  "impactShakeRadians": 0.004,
  "regionalBlur": {
    "enabled": true,
    "onImpact": true,
    "radiusNormalized": [0.22, 0.32],
    "irSigmaSourcePixels": 2.3,
    "rgbSigmaSourcePixels": 3.2,
    "irEdgeSigmaSourcePixels": 1.0,
    "rgbEdgeSigmaSourcePixels": 1.3
  }
}
```

`jitterOnImpact` uses computed contact time, including rigid-v1 convex contact.
Angular jitter is exactly zero at/before contact; post-contact motion has a
smooth onset and damped transient. The three causal shutter samples each use
their own timestamp, so an exposure can straddle contact. Motion diagnostics
then report the shutter-end transform; image samples are still integrated.
`"blastArrival": "acoustic"` (optional, default `"contact"` = unchanged) starts
the shake at contact + camera distance / `soundSpeedMetersPerSecond` (default
343): the camera-to-contact distance at the contact instant is recorded in
`cameraObservation.impactMotion.arrival`. It is an upper bound for the blast
arrival (a near-field shock is supersonic); no Kingery-Bulmash arrival time or
overpressure is computed. Not available for body-mounted cameras. Regional blur
still switches on at contact.

Regional blur operates on linear IR radiance/RGB before detector integration
and electronics. The central Gaussian region follows a prescribed impact/plume
point projected through the current camera; its blur turns on after contact.
Edge softness is present throughout. Widths above are source-image pixels,
not native detector pixels; detector PSF widths remain detector pixels.
Normalized separable spatial filtering preserves constant brightness. It is
an observation proxy, not a calibrated lens/atmosphere model. Clear canonical
images and quantitative fields are exported separately and are not blurred.

The separate `varied_fire_smoke_20261108` case reuses prior tree geometry,
rigid inputs and original FacetEB binding. Increased smoke changes transport
intentionally. Its generator is `make_varied_fire_smoke_case.py`; tests are
`test_varied_fire_camera.py`, `test_fire_seed_guards.py`,
`test_varied_compatibility.py`, and the native `histream_camera_tests` fixture.

For clear optical and mildly blurred infrared observations, use the separate
`make_optical_clear_ir_case.py` preset. RGB has full source-size detector pixels,
zero PSF/regional blur, zero response lag and a10us common shutter; IR retains
384x216 pixels,14bit raw ADC, sigma.65 native pixels and small IR-only regional
blur. Post-contact angular motion, scene and transport settings are retained.

Set `cameraObservation.irDisplay="linear-percent-2"` to display IR with strict
per-frame P2/P98 endpoints across all native pixels (sky included), a linear
clamped mapping and8bit rounding. A one-ADC-code minimum span handles flat
histograms. This option does not alter raw ADC, radiance or RGB. The default
`legacy-log` keeps the prior log display/temporal AGC. Percent stretch is a
display choice, not a hardware calibration or a universal thermal-camera rule.

### Supplied small-to-large fire growth

In natural-v2/varied-v3 sources, optional `growthSeconds` enables smoothstep
growth from each source's own ignition.0 (default) disables it; positive values
are0.1..25.5s, rounded to0.1s. Radius starts at20%, height15%, heat/tracer rates3%
of their supplied adult settings and reach100%. Fire light follows the rate
schedule. Retain adult ray bounds, refine integration around growing flames,
then return to the original adult sampling once mature. Flame temperature and
seed/profile are retained. This is a prescribed animation/source schedule,
not fuel/oxygen/chemistry or ignition/propagation prediction. The1m transport
grid can under-resolve early tiny sources; light and thermal fields are not an
energy-conserving combustion solve.

Eight per-source duration bytes occupy spare FireBlock header.y/z; source48byte
and block400byte sizes are unchanged. Unsigned CPU/GPU decoding retains high
bits for sources3/7. Omitted/zero schedules return the old source exactly.

`make_growing_fire_case.py` creates an independent case retaining camera and
rigid inputs; `test_fire_growth.py` verifies native appearance and heat/smoke.
Build `histream_fire_growth_tests` to check the shared GPU accessor against a
CPU curve across8sources/independent onsets, including signed header slots:

```powershell
build/histream-animation/runtime/Release/histream_fire_growth_tests.exe build/histream-animation/tests/fire_growth_probe.comp.spv NEW_OUTPUT
```

### Optional ground energy balance with canopy occlusion

Supplemental scenery originally casts RGB/IR visibility shadows but has no
effect on the pre-solved FacetEB background temperatures. Ground triangles are
too large to resolve individual tree shadows as uniform facet states.

`make_ground_thermal_case.py REFERENCE NEW_CASE` links the existing synthetic
36h weather/solar history and creates a0.5m local ground receiver grid.
`sequence.groundThermal={enabled:true,path:...,sha256:...}` selects a separate
GPU shader variant; omit/disable it for exact old behavior. The128byte render
push remains unchanged. Actual appended bark/leaf geometry casts direct shadows;
cosine hemisphere rays estimate diffuse sky and longwave from fixed current
object temperatures. Base-geometry canopy tags are rejected: canopy occluders
must be supplemental. Receivers are selected near-horizontal, top-visible ground
materials under the configured height limit.

Two local dry-surface energy models, with and without canopy occlusion, use the
same albedo/emissivity, areal heat capacity, convection and deep conductance.
GPU implicit60s steps use interpolated2h forcing; sun visibility refreshes every
600s. The with-canopy absolute temperature replaces the selected ground surface
temperature before the old impact perturbation. The paired delta diagnoses
canopy effects. Do not interpret old-vs-new differences solely as tree effects:
the ground model also changed. Original FacetEB files remain untouched and still
supply buildings/background; fixed tree temperature, omitted ET/multibounce and
absent airflow feedback mean this is not a fully coupled FacetEB re-solve or
field-validated prediction.

Native ground state (`ground_thermal/cells.f32`,20floats/cell), temperatures and
last-step energy residuals (`cells.csv`) are exported before display. Separate
replay parameters contain relative bundled source paths; original input receipt
is preserved. `plot_ground_thermal.py` produces matched-parameter temperature
maps/statistics; `verify_ground_thermal_case.py` checks field/provenance and
independent-channel preservation. Build `histream_ground_tests` for synthetic
day/night/no-canopy geometry and independent CPU implicit-root checks:

```powershell
python tools/scene-animation/tests/test_ground_physics.py build/histream-animation/runtime/Release/histream_ground_tests.exe build/histream-animation/runtime/Release/shader/animation/ground_thermal.comp.spv NEW_TEST_OUTPUT
```

### Body-mounted synchronized RGB/IR view

`sequence.cameraMount={body:"projectile",offsetMeters:[x,y,z]}` attaches the
selected camera to the rendered body's right/up/forward axes. Offset is in
meters, norm<=3; the camera looks forward with the configured FOV and body up.
Its pose is evaluated at every causal exposure sample and exported in
`camera.csv`. The existing authored trajectory has fixed orientation; no
attitude dynamics are inferred. Mount and trackingCameraOffset are exclusive.
Native lastFrame must end strictly before first contact; post-contact body
observations are rejected instead of rendering a frozen surviving camera.

`make_body_camera_case.py REFERENCE NEW_CASE` preserves scene/thermal/motion
inputs, sets55deg FOV and a nose lens offset[0,0,0.025]m, retaining all pre-contact
24fps frames. `package_body_camera.py CASE --ffmpeg FFMPEG` creates valid short
RGB/IR clips and a full-time-axis external/body comparison; its later body panels
explicitly mark signal loss. `verify_body_camera_case.py CASE REPO` checks every
shutter pose against the original body transform, shared ground field, unchanged
rigid/transport prefix, ADC/2% display, video hashes and previous artifacts.
`tests/test_body_camera.py ENGINE REFERENCE NEW_TEST_OUTPUT` exercises actual
native rays with a static-camera equivalent and input/contact-range guards.

`make_distant_approach_case.py BODY_CASE NEW_CASE` creates a separate visual
pre-roll:9s additional straight translation at the current authored rate, fixed
55deg optics, approximately124m nominal start distance and12s approach. It
creates both external504frame and body290frame inputs; run both with the native
engine, then invoke the same helper with `--finalize` before packaging. Shared
scene dependencies are resolved against their original config directories.
`verify_distant_approach_case.py CASE REPO` checks actual farther start,
monotone native movement, causal poses, matching external/body physics and all
artifacts. No digital zoom, slow-motion retiming or flight-model prediction.

`make_expanded_ground_case.py PARENT NEW_CASE` appends56coplanar asphalt triangles
outside the exact original floor hole, with its far edge beyond the10000m native
ray domain. It keeps cameras/motion/streets/trees unchanged. Far-floor temperature
is a parent sunlit-asphalt proxy, not a new full-domain energy solution. Run both
native sequences, then use `--finalize` and `compose_four_camera.py` for the ordered
quad. `verify_expanded_ground_case.py` binds coverage to actual OBJ/scenery and
both run manifests, checks hole/overlap/area, shared state and original artifacts.
Original coplanar curb/soil surfaces may change longwave material tie selection
after BLAS rebuild; the case reports the small temperature difference instead
of claiming bitwise thermal preservation. `tests/test_expanded_ground_binding.py`
checks stale OBJ/scene receipts and corrupt central-hole geometry.

## Open-airfield mid-IR day (sun on/off x payload on/off)

Aircraft, tanks, cars and the bus stand on the runway of the sealed coupled-town case; the town (office, ten
neighbour blocks, trees and its ground slab) is removed. One full day is solved natively under four matched
conditions and imaged at 3-5 um.

One command (preflight with `check`, execute with `run`; see `samples/airfield-day.job.json`):

    python run_case.py check samples/airfield-day.job.json
    python run_case.py run   samples/airfield-day.job.json

The `airfield-day` pipeline runs the seven steps below in order into `OUTPUT/result`, keeps logs and a
failure status in `OUTPUT/run.json`, and finally verifies `case_seal.json`, `render_seal.json`, the render
receipt and the video hashes. Options: `spinupDays`, `parallel`, `perRunVramGb`, `videoViews`, `videoEncoder`,
`deck`. The individual steps remain available:

    python make_runway_day_scene.py TOWN_CASE CASE/display_scene
    python prepare_runway_day.py CASE/display_scene samples/runway-day.actors.json CASE [--spinup-days 12]
    ENGINE eSceneAnimation CASE/thermal/scene.json CASE/gpu_geometry_thermal
    ENGINE eSceneAnimation CASE/scene.json CASE/gpu_geometry_display
    python run_runway_day.py CASE ENGINE            # four native FacetEB solves, concurrent (~14.6 GB GPU each; see below)
    python render_runway_day.py CASE --ffmpeg FFMPEG [--video-views overview vehicle_view] [--video-encoder auto]
    python build_runway_day_deck.py CASE airfield_day_mwir.pptx
    python steps_manifest.py CASE                # legacy cases: write per-node array manifests, then re-seal
    python case_seal.py CASE                     # legacy cases: write case_seal.json
    python case_seal.py CASE --verify            # verify the seal

The two `eSceneAnimation` calls are the static GPU geometry pass used by the renderer for hit/visibility
(`gpu_geometry_thermal/*_hits.f32`, `gpu_geometry_display/*_visible.png`). They are not native thermal solves;
`case_seal.json` covers their outputs, and `render_runway_day.py` refuses a sealed case whose hits changed.

* `make_runway_day_scene.py` keeps every surviving asset byte-for-byte, fills the exact hole of
  `scenery_expanded_ground` (the former town slab, -95..95 x -80..85 m) with a flat patch of the same material,
  moves the road vehicles onto the runway (checks: fully on the runway, clear of the road connector at x 91..105,
  at least 2.5 m between footprints) and redefines the four cameras. Aircraft keep their original poses.
* `prepare_runway_day.py` writes four native projects that share geometry, materials, thermal parameters, wind,
  air temperature, humidity and sky longwave. Switches: `sun` multiplies the shortwave column of the forcing by
  zero (the deep-ground starting temperature is also declared per sun case); `payload` sets every working prop's
  `powerW` to zero. The forcing is the synthetic half-sine day of the town pipeline (06-18 h, 850 W/m2 peak,
  air 26 +/- 6 C, 30 minute nodes). `--spinup-days` identical days precede the output day (default 12; the lumped
  deep-ground time constant is about 2.9 days). `heat_schedule` in the actors file selects continuous operation or a
  `final-day-window`. Aircraft power/mass in `samples/runway-day.actors.json` are example values, not data.
  `source_inputs.json` hashes the copies inside the case with relative keys.
* `run_runway_day.py` proves the 2 x 2 design before starting (geometry identical in all four; payload pairs
  differ only in power, sun pairs only in the shortwave column and the declared ground start), snapshots engine,
  shaders and sources, and runs the solves concurrently. Before starting it reads the free GPU memory with
  `nvidia-smi` and lowers `--parallel` (default 4) to floor(free / `--per-run-vram-gb`, default 14.6); it stops
  if not even one run fits, keeps the request with a warning when `nvidia-smi` is unavailable, and records
  `parallelRuns`/`gpuMemoryNote` in `native_runs.json`. A separate eight-node trial in this project compared
  concurrent and serial runs; the script does not re-establish that claim. Every receipt records
  `accepted/acceptanceIssues`; `--resume-completed` recomputes the convergence summary from the run's own
  `faceteb_run.json` and `diagnostics/` (refusing a receipt whose stored numbers disagree) and applies the current
  rules (all nodes converged, surface residual < 2 W/m2) instead of trusting a stale `completed` status. After each
  run it writes `output/steps_manifest.json` (SHA-256 of every `.facet_steps/*.bin` and `diagnostics/*.json`
  plus a Merkle root); the per-node arrays stay outside the case seal for size, but the manifest is inside it.
  At the end it writes `case_seal.json` (relative paths; inputs, configuration, native receipts, step manifests
  and GPU geometry).
* `render_runway_day.py` verifies `case_seal.json` before imaging; legacy cases can use `--allow-unsealed`
  (then it checks native receipts and `source_inputs` only). It also refuses a native project whose
  `configSha256` changed, and records the render code/native hashes. Every per-node array and diagnostic is
  checked against `steps_manifest.json` when first read (`--verify-steps lazy`, default; `full` checks all before
  imaging; `off` skips); a case without manifests prints a legacy warning. The H.264 encoder is probed before any
  work (`--video-encoder auto` = first working of libx264, libopenh264, mpeg4) and recorded in the receipt. One
  fixed band-radiance range (from the data,
  recorded in `render_receipt.json`) is used for every run, view and hour. Videos compress 24 h into 12 s;
  frames between 30 minute nodes are linear radiance interpolation, not new solves. `closure.json` gives the
  same-hour difference between the output day and the day before (periodicity), `energy_summary.json` the
  daily core-node energy terms, `day_history.csv` the group temperatures and band brightness temperatures.
  At the end it writes `render_seal.json` over the render products (radiance NPZ arrays excluded).
  `--probe-nodes N --out-dir DIR` is a test mode that uses early nodes of a running case; its products are not results.
* `build_runway_day_deck.py` builds the editable report. It reads the minimum vehicle gap from `display_scene/scene_build.json`,
  the deep-ground time constant from `coupling.json`, the band/IR dimensions, ADC bits, node spacing, video
  frames/fps and the still panel layout from `render_receipt.json`, target counts from the renderer's groups,
  and the solar window from `coupling.json`; chart axis ranges follow the data, the periodicity headline takes the
  maximum over every group (including individual props), and the residual margin to the 2 W/m2 line is printed.
  It needs `python-pptx`.
* `tests/test_runway_day.py` (run `python -m unittest discover -s tests -p test_runway_day.py` from this folder)
  checks scene composition, time axis, forcing switches, exact 2 x 2 design and that hidden differences are rejected.
  `tests/test_case_seal.py` covers relative seals/copying/mutation, `tests/test_energy_layout.py` covers the
  single- and multi-band energy-record widths, and `tests/test_integrity_tools.py` covers `require` under
  `python -O`, the GPU-memory plan, step manifests and the encoder probe.

Integrity checks in the production scripts use `integrity.require` (raises `IntegrityError`), not `assert`, so they
also run under `python -O` / `PYTHONOPTIMIZE`.

Scope: the sun-off case holds air temperature and sky longwave at the sunlit-day values, so it isolates solar
loading (a controlled counterfactual, not a real overcast day). Weather, materials, payload powers and the
lumped deep-ground reservoir are game assumptions; no atmosphere path, no 3D conduction, no measured signatures.
Pixels beyond the thermal ROI use a declared far-field boundary state. RGB panels are the static daylight reference.