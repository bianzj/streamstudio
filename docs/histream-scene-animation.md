# HiStream GPU scene module

Current feature inventory and executable job entry:
[当前模拟功能](../tools/scene-animation/CURRENT_FEATURES.md),
[run_case.py](../tools/scene-animation/run_case.py), and
[capabilities.json](../tools/scene-animation/simulation/capabilities.json).

Status (2026-11-12, development build under test; not in the 2.0.1 installer): research module. The panel, server
API, job document and manual chapter are implemented, but the GUI entry is hidden for now (`SCENE_ANIMATION_UI_ENABLED
= false` in `project-schema.js`: no menu item, no catalog entry, nothing dispatched from the interface), because there
is no parameter form yet; run it from the CLI.
The airfield-day chain runs through `run_case.py` (pipeline `airfield-day`).
The `authored` and `rigid-v1` motion backends are authored visual demonstrations, not predictive impact models;
`impact-v1` is an empirical engineering estimate (Young penetration, Kingery–Bulmash load, impulse failure,
energy budget) that is not independently validated. Every run manifest states the kind at top level
(`physicsValidated: false`, `impactModel.kind/label/predictive`), and `warnings[]` lists weakening conditions.
impact-v1 and the S3/S4/S6 performance options are documented in the
[module README](../tools/scene-animation/README.md#impact-v1-empirical-event-model-2026-11-12).
The native animation and coupled FacetEB pipelines remain distinct implementations;
CPU/GPU placement and approximations are listed per feature.

## Module layout and mesh scene backend (2026-11-08)

The optional animation module is now in `models/histream/src/animation/`,
with its own `cmake/scene-animation.cmake`. Enable
`HISTREAM_ENABLE_SCENE_ANIMATION=ON` explicitly for animation builds.
Ordinary builds default to OFF. Current tools, scene contracts and targeted
checks are documented in [tools/scene-animation/README.md](../tools/scene-animation/README.md).

The new `backend: "mesh"` supports static visible/IR geometry inspection.
Material temperatures can be supplied or loaded through the native FacetEB
background-field adapter. `backend: "meshSequence"` provides the continuous
impact/fireball/debris/plume illustration; its authored damage and kinematics
are documented in the tools README and are not a structural/blast prediction.
The existing procedural backend
and its continuous demonstration retain the settings documented below.


## Procedural reference backend

`eSceneAnimation` is a native HiStream Vulkan compute mode. It uses the existing
GPU selection/context and generates an RGB sequence in a dedicated compute
shader. It does not load the GUI's RT/EB `project.json` or OBJ scenes: the first
example is a deterministic, fictional empty urban block defined in the shader.

```powershell
& C:\work\streamstudio\models\bin_x64\Release\histream.exe eSceneAnimation animation.json new-run
```

Input JSON (all fields optional):

```json
{
  "width": 1280,
  "height": 720,
  "fps": 24,
  "frameCount": 288,
  "impactTime": 3,
  "gpuIndex": 0,
  "samples": 4
}
```

For white-hot thermal images, add `"renderMode": "thermal"`. Visible RGB remains
the default (`"visible"`). Thermal options are `thermalWavelengthMicrometers`
(default 10, range 3–14), `thermalDisplayMinK` (260) and
`thermalDisplayMaxK` (650), with finite increasing display bounds in 150–2000 K.
The thermal path runs in the native GPU shader. It evaluates prescribed
temperatures with single-wavelength Planck radiance, greybody surfaces and
simplified absorbing/emitting smoke, flame and dust. White-hot uses a fixed
logarithmic radiance stretch; bounds are not inferred target temperatures.
Temperature, emissivity and extinction are illustrative inputs. No explosion
heat calculation, instrument-calibrated response or atmosphere is included.
Optional band integration and configured sensor blur/noise are described below.
Raw thermal PNGs are grayscale RGB display images,
not radiance or brightness-temperature products. The original RGB outputs are
retained in their separate run directory.

Dimensions: 32–4096 pixels. FPS: 1–120. Frame count: 1–10000.
`firstFrame`/`lastFrame` optionally select a 1-based inclusive frame range;
presentation time is `(frame-1)/fps`, so seeking does not change a frame.
`impactTime` is a finite presentation time from 0 to 3600 s. It is not a flight
prediction. Samples: 1–4; the delivered example uses four spatial samples.
Hardware GPU is required. The existing CPU Vulkan fallback is not used here.

`physicsMode: "transport"` enables the independent dense-grid heat/smoke module
(thermal rendering only; maximum timeline 60 seconds). `transportWindX` and
`transportWindY` specify the background wind in m/s (default 0.8, 0.15; range
-5 to 5). `transportScenario` is `heatedPlume` by default; `ambient` and
`passivePulse` are numerical fixtures. This module does not call VoxelEB/LBM.

The MAC face-flux projection uses compatible divergence, pressure-gradient
and nearest-neighbor Poisson operators. Ground/solid faces are closed;
exterior pressure is zero with corrected boundary flux. It uses 28 Jacobi
iterations, so nonzero residual divergence is exported rather than suppressed.
`transport.csv` records numerical diagnostics, including the pressure residual
and measured post-projection divergence. The 64×56×56 grid has 1 m cells and
fixed 1/60 s updates; seeking warms from t=0. Conservative obstacle voxelization
preserves thin members, with some thickening at this coarse resolution.

Air temperature and normalized smoke tracer use semi-Lagrangian advection and
eddy diffusion. Buoyancy drives the plume; grey radiative cooling removes air
heat. A 3 mm effective surface layer uses concrete conductivity, heat capacity,
convection and radiation to evolve a temperature perturbation above a 300 K
reference. Material background temperatures, tracer extinction coefficient,
heat sources and initial hot patch remain supplied inputs. The model has no
combustion chemistry, impact-energy calculation or independent field validation.
Semi-Lagrangian tracer transport is not strictly conservative; test drift is
reported in the case records. No validated blast, damage or temperature claim
follows from these numerical checks.

`thermalBandMode: "lwir8_14"` integrates a uniform 8–14 µm response using
8-point trapezoidal quadrature. The default `single` retains the requested
single wavelength. This is not a specific instrument SRF. The default path has
no sensor response; an optional generic camera-display chain is available below.
Neither path includes atmosphere or instrument calibration.

The example now has 13 surrounding buildings and a hollow target with slabs
and columns. Shared geometry drives both rendering and transport obstacles.
The breach removes prescribed local facade, slab and column sections; stresses,
load paths, fracture and collapse are not computed. For an unobscured geometry
inspection image, `showParticipatingMedia: false` hides volume rendering while
retaining the damaged geometry. Label such an image as a geometry view.

## Optional detailed scene and generic IR camera

`sceneStyle: "detailed"` adds varied roof setbacks, parapets, a service annex,
material/facade patterns and prescribed room-temperature variability. A staged
irregular planar breach replaces the ellipsoid cut. Major geometry is shared
with transport. Parked vehicles, curbs, bollards and thin exposed reinforcement
are visual accessories and are not obstacles in the 1 m transport grid.
This remains procedural illustrative geometry, not scanned city assets or a
fracture solution. The default `simple` style preserves the earlier example.

`thermalSensor: true` enables a separate native GPU display pipeline. Camera
subpixel samples are first averaged in float linear radiance. Detector binning
and a normalized, truncated 5×5 Gaussian PSF act on this radiance. Configured
noise and fixed pattern are then added, followed by normalization, 12-bit
display quantization, gamma 0.92 and interpolation to the output resolution.
The PNG is 8-bit display RGB; it is not a temperature/radiance product.

Options (defaults): `sensorBinning: 2` (integer 1–4),
`sensorBlurSigmaPixels: 0.7` (0–1.5 detector pixels),
`sensorNoiseKelvin: 0.09`, `sensorFixedPatternKelvin: 0.05` (both 0–2 K,
converted through the configured Planck derivative at 300 K),
`sensorResponseSeconds: 0.35` (0–5), and `sensorAgc: true`.
These are generic configured effects, not measured instrument NEDT/MTF.

AGC uses a deterministic sample of the radiance image and the 2nd/99.3rd
percentiles. To prevent the cold sky dominating this scene's display, samples
below the 275 K blackbody reference are excluded when at least 32 samples
remain. Cold scenes fall back to all samples. A minimum span equivalent to
20 K at 300 K is enforced, and bounds follow a first-order temporal response.
With `sensorAgc: false`, configured thermalDisplayMinK/MaxK radiance references
are used. Sensor mode bypasses the earlier logarithmic display transform;
metadata identifies the selected mapping. Display clipping is not a claim of
physical detector full-well saturation.

`sensor.csv` records target and actual exposure bounds. Requested partial runs
warm sensor history from frame 1, so a requested frame matches full playback.
`exportRadianceFrames: [1, 85, 253]` saves selected pre-display linear images as
little-endian row-major float32 `.f32` files. Indices must be in 1..lastFrame;
the manifest records dimensions, band/radiance units and exported indices.
The loaded sensor SPIR-V is snapshotted alongside the engine/transport shaders.

The output directory must not exist, including for concurrent launches.
It contains raw RGBA PNGs, `frames.csv`, an input copy, device/run receipt and
snapshots of the actual executable and loaded SPIR-V. Existing RT/EB modes and
their calibrated outputs are separate. A shader/engine build is required:

```powershell
cmake --build build/histream --config Release --target histream histream_runtime_resources --parallel 12
```

`tools/scene-animation/render_video.py` only adds captions and packages existing
GPU PNGs. It needs Pillow and an explicitly supplied ffmpeg executable. MP4 is
H.264/yuv420p; odd image sizes are padded to even sizes. Full video decoding is
checked, and raw PNG, captioned PNG, executable and shader SHA-256 values are
recorded. Original GPU frames are retained.

```powershell
python tools/scene-animation/render_video.py new-run --ffmpeg C:/path/to/ffmpeg.exe
python tools/scene-animation/tests/test_animation.py models/bin_x64/Release/histream.exe D:/cases/StreamStudio/new-test-run
```

Scope: the GPU evaluates prescribed projectile motion, a facade opening,
illustrative fragment arcs, and evolving procedural flame/smoke/dust volumes.
It integrates visible opacity and RGB emission, or single-wavelength thermal
radiance from prescribed temperatures, along the camera ray. No guidance,
collision prediction, detonation, compressible shock
wave, fuel chemistry, structural response or calibrated spectral radiance is
computed. RGB colors must not be interpreted as temperature, brightness
temperature or validated damage. The native tests verify input boundaries,
GPU execution, changing frames, output protection and deterministic seeking;
they do not establish scientific accuracy.

## Mesh sequence: physical appearance and camera observation

Both additions are opt-in. `sequence.physicalAppearance: true` selects generic
GGX/Smith/Schlick RGB materials, roughness/metallic/IOR, filtered procedural
albedo, tracer attenuation of direct sunlight and approximate aerial haze.
Material properties `roughness` (.02–1), `metallic` (0–1), and `ior` (1.01–3)
are optional; the default appearance branch retains previous behavior.
Glass is an opaque appearance proxy; this is not measured spectral BRDF or
full multiple-bounce/combustion/structural simulation.

`sequence.cameraObservation.enabled: true` requires both `sequence.optical`
and `sequence.sensor`. It saves separate `observed_optical`, `observed_ir`
display images and native IR ADC codes in `detector_ir` (16-bit PNG container).
`optical_frames`, `frames`, `fixed_frames`, selected radiance/geometry and the
nominal physics diagnostics remain separate reference outputs.

The GPU chain operates in linear signal: three actual scene samples over a
finite shutter (Simpson weights, ending at the nominal timestamp), normalized
Gaussian PSF, 4×4 rectangular pixel-aperture integration and continuous angular
jitter/boresight projection, weighted exposure, detector response, fixed pattern
gain/offset plus read/row noise, ADC, AGC/tone curve and 8-bit display. PSF sigma
is specified in native detector pixels, accounting for `fieldScale`. The first
exposure is clamped to scene time zero; temporal response starts from that
sample. Partial output requests warm from frame 1 for deterministic seeking.

Generic example within a 1280×720 mesh render:

```json
"cameraObservation": {
  "enabled": true,
  "exposureSeconds": 0.016666666666666666,
  "jitterRmsRadians": 0.003,
  "jitterFastRadians": 0.0012,
  "rollRmsDegrees": 0.06,
  "fieldScale": 0.985,
  "seed": 20261108,
  "ir": {"width": 384, "height": 216, "psfSigmaPixels": 0.85,
    "apertureFraction": 0.9, "adcBits": 14, "noiseKelvin": 0.06,
    "gainFpn": 0.003, "offsetKelvin": 0.07, "rowNoiseKelvin": 0.025,
    "responseSeconds": 0.012, "boresightPixels": [0.37, -0.21],
    "rollDegrees": 0.08, "adcMinK": 250, "adcMaxK": 650},
  "rgb": {"width": 960, "height": 540, "psfSigmaPixels": 0.55,
    "apertureFraction": 0.95, "adcBits": 12, "noiseLinear": 0.002,
    "gainFpn": 0.001, "offsetLinear": 0.0004, "rowNoiseLinear": 0.0005,
    "responseSeconds": 0, "boresightPixels": [0, 0], "rollDegrees": 0,
    "adcMaxLinear": 4}
}
```

Detector dimensions must fit the source with the same aspect ratio. Exposure
cannot exceed the frame interval. JSON and direct native options reject
nonfinite/out-of-range values before dispatch. IR values use monochromatic
Planck radiance at the scene wavelength (3–14 µm), with noise in equivalent K
at 300 K. RGB units are uncalibrated scene units. Display enlargement adds no
detector resolution. No spectral bandpass, photon flux/shot noise, rolling
shutter or dropped frames is modeled. Three temporal samples approximate fast
motion; large rotations use source-edge clamping and cannot reveal unseen
geometry. These chosen parameters are not a measured device or EMVA validation.

`observation.csv` records per-frame jitter, IFOV, ADC extrema, exposure weight
and display bounds. Actual camera shaders are copied into provenance. Numerical
fixtures cover flat fields, fractional registration, PSF step/energy/crop,
ADC conversion, temporal response, deterministic noise and nonfinite guards;
integration checks canonical-channel isolation and deterministic seeking.

```powershell
cmake --build build/histream-animation --config Release --target histream histream_runtime_resources histream_camera_tests --parallel 6
build/histream-animation/runtime/Release/histream_camera_tests.exe build/histream-animation/runtime/Release/shader/animation/camera D:/cases/StreamStudio/new-camera-fixtures
python tools/scene-animation/tests/test_camera_observation.py build/histream-animation/runtime/Release/histream.exe CAMERA_SEQUENCE_JSON NEW_TEST_OUTPUT
python tools/scene-animation/package_camera_sequence.py NATIVE_FULL_RUN NEW_VIDEO_OUTPUT --ffmpeg C:/path/to/ffmpeg.exe
```

Method references: [PBRT microfacets](https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory)
and [EMVA 1288 Linear 4.0](https://www.emva.org/wp-content/uploads/EMVA1288Linear_4.0Release.pdf)
for general sensor noise/quantization terminology. The effective IR chain is
not a calibrated photoelectron model or a claim of standard compliance.

## Mesh sequence: sustained fire and supplemental scenery

`sequence.sustainedFire` supplies 1–8 fixed source regions relative to the
existing contact point. It requires `media: true` and `projectileAppearance:
"separated-v2"`. Each region has a prescribed ignition delay, duration and
fade, volumetric flame/ember appearance and continuous heat/tracer input into
the existing low-speed transport. This is not ignition prediction, fuel or
oxygen chemistry, fire spread, tree combustion or structural fire response.
IR constituent emissions are mixed in linear radiance. The supplied RGB
emission colors are illustrative. The root option is disabled by default;
the separate `sequence_fire.comp` variant preserves default shader arithmetic.

```json
"sustainedFire": {
  "enabled": true,
  "sources": [{
    "offsetMeters": [-1.1, -1.1, -1.1],
    "radiusMeters": 1.05, "heightMeters": 3.1,
    "onsetSeconds": 0.45, "durationSeconds": 30, "fadeSeconds": 3,
    "temperatureK": 1120, "heatKelvinPerSecond": 140, "tracerRate": 1.8
  }]
}
```

Offsets use world axes and are added to the existing computed contact.
Delay/duration are seconds after contact; temperatures are supplied kelvin,
heating is supplied K/s, and tracer is a normalized source rate. Radius is
.25–4 m, height .5–8 m, onset 0–30 s, duration .5–120 s, fade .1–duration
(default min(2,duration)), temperature 500–1400 K, heating 0–300 K/s and tracer
0–10. Source regions must fit the transport domain (ground source centres may
lie near z=0). The GPU packet is a 16-byte count plus up to8×48-byte records;
it uses storage descriptors and leaves the128-byte mesh/80-byte legacy push
interfaces unchanged. Bounds extend at ignition and remain for residual smoke.

`sequence.scenery` references a separate hashed MeshScene file:

```json
"scenery": {"enabled": true, "path": "scenery.json", "sha256": "..."}
```

It uses the existing strict OBJ/material/transform parser, matching world
coordinates and IR wavelength. Scenery has independent prescribed material
temperatures and cannot claim the original FacetEB background field. Its
triangles/materials join only the cached static render background for RGB/IR
visibility and shadows, with explicit material-index remapping. They remain
outside Bullet contacts and the coarse fluid obstacle mask. The original
FacetEB scene binding and record coverage stay immutable; the manifest names
their original-scene scope and separately records supplemental coverage.
Scenery recipes/assets are snapshotted with relative paths for replay.

`tools/scene-animation/make_fire_trees_case.py REFERENCE_CASE NEW_CASE`
generates original procedural trunks, branches, irregular clustered crowns
and planters, using explicit prescribed tree/soil temperatures. It records
generator radius and a separate conservative canopy bound and checks these
against rotated building rectangles, the street and the debris region. This
does not establish scanned-tree accuracy or vegetation energy balance.

`tests/test_fire_scenery.py` checks late RGB/IR fire, transported smoke,
pre-ignition behavior including inactive remote sources, prescribed scenery
temperatures, snapshot replay/seeking and duration/media/domain/hash guards.
`verify_fire_trees_case.py CASE REFERENCE_CASE REPO` verifies a full delivery,
frozen reference protection, exact rigid/background provenance and complete
video decoding; it does not certify fire or device accuracy.

## Natural appearance variant

`sequence.appearanceVersion: "natural-v2"` requires optical output and
`physicalAppearance: true`. It selects `sequence_natural.comp`, leaving the
original sequence/fire shader arithmetic available for unchanged cases.
The variant adds advected multi-scale flame detail, intermittent emitting
sheets/tips, mottled embers and smoke sunlight attenuation. It reads existing
low-speed velocity/tracer fields; supplied heat rates, source schedules and
rigid dynamics are unchanged. It is an appearance model, not reactive fuel
chemistry or calibrated flame radiation.

Natural volume integration uses approximately .35 m steps away from flames,
.065×source-radius steps near active fire, and a .16 m pre-contact cap with
.025–.05 m sampling inside the active nozzle-jet envelope. A1536-step budget
uses remaining-distance/remaining-budget as a lower step bound so the whole
integration span is covered unless opacity terminates it. This resolves more
detail than the original .4 m fire appearance quadrature, without changing
the existing fluid grid or claiming flame-scale CFD.

Supplemental materials can set `thinLeaf: true` (boolean). Static leaf
triangles retain the prescribed-temperature flag and receive a separate
thin-leaf tag. Only the natural RGB shader uses it for an approximate diffuse
backlighting/unresolved-scattering term; IR remains an opaque emissive surface
at the prescribed temperature. `thinLeafTaggedTriangles` records tag coverage.

`natural_vegetation.py` generates tapered smooth-normal branches and curved
four-triangle leaves with explicit OBJ vertex normals and real canopy holes.
`make_natural_fire_trees_case.py REFERENCE_FIRE_TREE_CASE NEW_CASE` retains
the40tree locations, clearance bounds and source schedules, generates132000
canopy leaves/589280scenery triangles, and enables4scene samples/pixel. Tree
motion, vegetation energy balance and automatic fire spread remain outside
this variant. The geometry and material colors are original synthetic assets,
not measured trees or spectral leaf calibration.

Targeted checks: `tests/test_natural_vegetation.py` tests finite/unit normals,
winding/area, counts/bounds and projected openings; `tests/test_natural_appearance.py`
uses the retained reference-case fixtures to test finite IR, unsteady appearance,
exact transport isolation, seeking/guards and a backlit-leaf RGB/IR isolation
case. Separate legacy regressions verify unchanged default/fire output.
`compare_natural_appearance.py` creates a matching old/new view comparison;
`verify_natural_case.py` checks full native/runtime/source/background/rigid
provenance and complete video decoding.
