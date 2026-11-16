# HiStream animation isolation and mesh-scene implementation plan

> For implementation workers: execute in dependency order with executing-plans,
> or use subagent-driven-development only when delegation is authorized.
> Approved plan. Tasks 0-4 completed on 2026-11-08.
> The original planning turn was read-only. Execution evidence is linked below.

**Goal:** Separate original HiStream RT/EB code from the new animation work,
then improve scene realism with mesh assets and traceable background heat fields.

**Architecture:** A gated animation module exposes one CLI facade and uses
explicit adapters to existing HiStream services. Existing procedural rendering
remains a reference backend inside that module; a new mesh backend consumes an
explicit scene description. Original model solvers continue to own their physics.

**Tech stack:** Existing C++17, CMake, Vulkan/GPU and GLSL; existing Python video
tools. The plan introduces no replacement rendering engine or external service.

**Spec:** User requests continued modification with old/new code separated,
and requests the plan before implementation. Design and boundaries are below.

## Current verified layout

- Formal repository: `C:/work/streamstudio`.
- New files currently in `models/histream/src/base/`:
  `scene_animation.{h,cpp}`, `scene_transport.{h,cpp}`, `scene_sensor.{h,cpp}`.
- Animation shaders are already grouped in `models/histream/shader/animation/`.
- `src/main.cpp` currently includes the animation facade and dispatches
  `eSceneAnimation`; root CMake globs the new C++ through `src/base/*.cpp`.
- Root CMake contains animation shader compilation and unconditional deployment.
  Its recursive shared-shader dependency list also includes animation headers,
  so animation-only edits can trigger unrelated shader rebuilds.
- Animation test scripts are in `tools/`; presentation scripts are in `statistic/`.
- Existing visible/thermal outputs, shader/executable snapshots and source copies
  are retained under `D:/cases/StreamStudio/supplemental/impact_gpu_20261107/`.
  The current reference is `visual_20261108`; preserve its inputs and hashes.

## Design and boundaries

### Three separate responsibilities

1. Original model code: `src/base`, `raytracing`, `voxelrt`, `voxeleb`,
   `facetrt`, `faceteb`. Do not add animation algorithms here.
2. New animation module: scene loading, timeline, rendering adapters, transport,
   sensor display and native frame outputs. Depend on original services through
   explicit interfaces; original solvers must not depend on animation classes.
3. Frozen reference: keep the existing case source/executable/SPIR-V/config
   snapshots as the previous implementation. They are not active development
   locations. Record source-to-destination mapping for the directory migration.

Use one formal repository. Directory/module separation does not require a
second repository copy or a new product executable in the first implementation.

### Proposed active layout

```text
C:/work/streamstudio/
  models/histream/
    src/
      base/                       original shared services
      raytracing/ voxelrt/ ...     original RT/EB solvers
      animation/
        scene_animation.h/.cpp    existing public facade and CLI coordination
        transport/                existing SceneTransport implementation
        sensor/                   existing SceneSensor implementation
        scene/                    new mesh scene, materials and timeline state
        render/                   procedural reference + new mesh backend
        thermal/                  background-temperature file adapter
        bridge/                   explicit HiStream mesh/field adapters
    shader/animation/
      scene.comp geometry.glsl    current procedural reference backend
      transport.comp sensor.comp existing shaders, deployment names preserved
      mesh/                       new mesh rendering shaders
    cmake/scene-animation.cmake   module targets, shader build and resources
  tools/scene-animation/
    tests/                        moved current animation/transport/sensor checks
    render_video.py               moved presentation tool
    compare_physics.py            moved transport comparison tool
    compare_visuals.py            moved visual comparison tool
    README.md                     entry points, dependencies and pass criteria
  docs/histream-scene-animation.md
```

Do not create a duplicate `Tests` directory just for casing. Keep the animation
checks together under the existing tools convention and give them one documented
entry. Fix old script/document references as part of the move.

### Build boundary

- Add a CMake target `histream_scene_animation` and an explicit
  `HISTREAM_ENABLE_SCENE_ANIMATION` option, default OFF for ordinary model builds.
  Animation development builds explicitly enable it.
- Keep the `eSceneAnimation` command when enabled. A disabled build reports that
  the module is not enabled and exits clearly before looking for shaders.
- Main contains only the guarded include/dispatch. Module-specific configuration,
  validation, resources and algorithms live in the module.
- Remove animation files from the original base source glob by moving them;
  explicitly enumerate module sources rather than extending unrelated globs.
- Exclude `shader/animation/` from core shared-header dependencies. Animation
  shader dependencies belong to the module build file.
- Check enabled/disabled builds in separate build/staging directories so stale
  shader files do not masquerade as a clean core-only deployment. Final output
  directory mechanics must be checked against nvpro CMake before implementation.

### Scene/field contracts

- The facade retains the current `runSceneAnimation(input, output, shader,
  executable)` behavior during the mechanical migration.
- The later mesh backend receives a versioned scene description: mesh assets and
  hashes, instance transforms, mesh/material IDs, camera, presentation time,
  temperature-source references and output configuration.
- Materials map geometry to radiative/thermal properties; object shape does not
  silently decide material identity. Every adapter records coordinates, units,
  time convention and transform matrices.
- Start background-heat integration through immutable output files from an
  existing HiStream solver using the same scene/forcing. Do not duplicate the
  energy-balance solver inside the animation shader.
- Keep deterministic supplied hot sources and preset damaged asset variants.
  No prediction of weapon penetration, blast energy, structural failure or
  collapse is part of this implementation.

## Implementation tasks

### Task 0 — Freeze and inventory the current reference

- [x] Verify the current `visual_20261108` source, binary, shader, config and video
  hash receipts. Reuse existing snapshots; add only missing evidence.
- [x] Inventory only animation-related staged/unstaged/untracked changes and
  preserve all unrelated user edits.
- [x] Write a move manifest for every source/test/script and its new location.

**Acceptance:** the current reference remains reproducible and no old case is
used as a new run's writable output directory.

### Task 1 — Mechanical module isolation, with unchanged behavior

- [x] Capture selected reference frames, float-radiance samples and diagnostics
  using the current build/config before moving active files.
- [x] Move the six animation C++ files into the proposed module folders; update
  includes, CMake target sources, guarded CLI integration and resource rules.
- [x] Move the existing three test scripts and video/comparison tools; update
  their documented paths. Do not alter numerical/rendering algorithms here.
- [x] Check enabled and disabled build paths. A source-only animation shader edit
  must not rebuild core RT/EB shaders; core-only staging must omit the module.
- [x] Run moved animation, transport and sensor checks, including deterministic
  warm seeking, radiance exports and no-overwrite behavior.
- [x] Compare selected image/radiance bytes and numerical diagnostics to the
  reference. Investigate differences before proceeding.

**Acceptance:** module ON preserves the command and reference results; module
OFF preserves core model availability. This task has no visual changes.

### Task 2 — Add detailed static mesh scenes through an adapter

- [x] Add `scene/mesh_scene.{h,cpp}`, `scene/material_table.{h,cpp}`,
  `bridge/histream_mesh_adapter.{h,cpp}` and `render/mesh_renderer.{h,cpp}`.
- [x] Test a non-symmetric mesh with distinct axis lengths and material patches
  to catch vertex-stride/index/normal/coordinate errors before city assets.
- [x] Verify OBJ loading and GPU acceleration-structure integration rather than
  assuming every existing upload interface is directly reusable.
- [x] Add one detailed building, then a small street block with interiors and
  traceable asset/license/material records. Static geometry is cached; dynamic
  variants are explicit assets. No street layout hardcoded in the new shader.
- [x] Keep a selectable procedural reference backend for regression/comparison.

**Acceptance:** three no-smoke views show correct scale, occlusion, wall thickness,
normal orientation and material IDs. A missing asset/material fails explicitly.

### Task 3 — Load a physically sourced background thermal field

- [x] Add `thermal/background_field.{h,cpp}` with checks for units, axes, timestamp,
  coverage, material/mesh mapping, source version and input hashes.
- [x] Use the same scene and documented meteorological forcing with the existing
  energy-balance engine; retain forcing and necessary thermal-history inputs.
- [x] Map background temperatures to mesh rendering and superpose the existing
  explicitly supplied transient temperature perturbation without double heating.
- [x] Produce daylight, shaded and evening static IR views under a fixed display
  scale; also retain floating radiance/temperature fields for quantitative review.

**Acceptance:** mapped fields align with geometry and reproduce their source
values within a documented interpolation tolerance. Without observations,
label results as model-to-model/numerical checks, not field accuracy validation.

### Task 4 — Restore the continuous demonstration on the new backend

- [x] Add staged damaged mesh assets with fracture-surface materials, visible
  thickness and declared preset fragment motion/contact behavior.
- [x] Couple existing heat/tracer fields and the generic camera through the new
  scene/material contract. Keep fixed-radiance output separate from AGC video.
- [x] Produce one 12 s sequence, unobscured geometry views, and a comparison with
  the frozen reference. Verify every frame and complete video decoding.

**Acceptance:** no abrupt asset/material swaps, missing frames, invalid fields,
obvious penetration or floating debris in the inspected stages. Damage remains
preset geometry; no claim of structural response follows from the animation.

## First implementation scope and later work

First complete Tasks 0–2. Deliver an isolated module, moved tests, one mesh
building and static visual/IR inspection views before changing heat sources or
adding further effects. Task 3 follows once the mesh/material mapping is stable;
Task 4 follows after static thermal review.

Local grid refinement, higher-order/conservative transport, settling dust and
measured camera SRF/MTF/noise are later tasks. Do not mix them into the mechanical
migration or imply that the generic camera parameters are measured calibration.

## Paths, records and validation status

- Active code/tests/tools: `C:/work/streamstudio`.
- New outputs: a fresh case/run under
  `D:/cases/StreamStudio/supplemental/impact_gpu_20261107/mesh_scene_20261108/`.
- Existing `visual_20261108`, `transport_20261108` and earlier cases are references.
- Validation labels: passed / failed / not run / static-only. Build success is
  not physical validation. Run only checks related to the modified boundary.
- No automatic commit, push, installer packaging, release, migration or deletion.
- Execution status (2026-11-08): Tasks 0-4 passed their listed native build,
  interface, numerical, image and video checks.
- Evidence: `D:/cases/StreamStudio/supplemental/impact_gpu_20261107/mesh_scene_20261108/`
  (`final-verification.json`, `isolation-verification.json`,
  `shader-isolation-verification.json`, `progress.md`, `index.html`).
- Source remains uncommitted on `codex/animation-mesh-isolation`; no integration
  or release was requested. Unrelated pre-existing edits remain protected.

Task3 evidence: `D:/cases/StreamStudio/supplemental/impact_gpu_20261107/thermal_field_20261108/`.
Native GPU FacetEB uses25 two-hour nodes over48h with24h initial warmup; every
rendered triangle maps to native geometry. The supplied instantaneous deltaK
interface is verified; dynamic transport coupling is provided by Task4.

Task4 evidence: `D:/cases/StreamStudio/supplemental/impact_gpu_20261107/mesh_video_20261108/`.
Final288frame/12s IR, fixed-scale and old/new comparison videos decoded in full.
Damage and landing endpoints are authored presets; no structural/blast prediction.
