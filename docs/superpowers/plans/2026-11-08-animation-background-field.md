# HiStream mesh background thermal field implementation

Approved continuation of Task 3 in `2026-11-08-animation-module-and-mesh-plan.md`.
Execute inline using executing-plans; preserve the frozen Task0-2 cases.

Goal: replace prescribed mesh background temperature with immutable native
FacetEB outputs from the same geometry and declared synthetic meteorology.
Keep all additions in the animation module/tools; original solvers are unchanged.

## Contracts and choices

- Add `animation/thermal/background_field.{h,cpp}` and a versioned field JSON.
  Required: units K, right-handed Z-up metres, explicit timestamp/convention,
  complete coverage, material/instance/asset identity, solver revision/binary,
  forcing/history references and SHA256 of every source file.
- Transfer exact triangle front/back temperatures. Binary records contain three
  world vertices, material index and two temperatures (12 little-endian float32).
  Vertex/mapping tolerance is 0.1 mm; no spatial interpolation or nearest-neighbour
  guess. Missing coverage, stale hashes, wrong axes/units/time/mapping fail before
  creating output. Snapshot a portable replay field plus its source records.
- Extend only the animation Triangle layout with front/back baseline and a
  separately supplied additive deltaK. Existing prescribed material backgrounds
  stay available when no field is selected. Apply a supplied delta once; the
  background field is never heated in place.
- `prepare_thermal_case.py` reads the same OBJ/instances, exports an equivalent
  native Y-up scene and per-face material groups, then runs existing GPU FacetEB.
  World-to-native is `(x,y,z)->(x,z+0.6,-y)`. A centred identity placement and
  sufficiently large domain avoid clipping. Native ground boundary at -0.61 m
  world height is recorded separately; every rendered triangle must match an
  actual source triangle before transfer. Use the actual native output geometry
  to verify, never infer solely from matching counts.
- Use explicit opaque nonvegetation material heat capacities/optical properties.
  Record that thin two-sided facet energy storage is an approximation for solid
  buildings. No multilayer wall-conduction claim. Retain generated forcing and
  all warm-up states. Start with 48 h at 2 h step, report second-day noon and 20:00.
  Weather and architectural assets are synthetic, not observations.
- `export_background_field.py` validates completed/converged native records,
  maps geometry/material order, verifies source file hashes and exports fields.
  Field timestamp must equal the selected native node. No field accuracy claim.
- Deliver noon sun-facing, noon shade-facing and evening IR at the same fixed
  280-330 K radiance display scale, with float temperature/hit/radiance files.

## Execution steps

1. Freeze Task0-2 source/runtime/output hashes; ledger current dirty-file boundary.
2. Write native background-field tests and observe rejection/mapping failure on
   the current binary. Include wrong units, axes, timestamp, missing/duplicate
   coverage, changed geometry/material/instance/hash and additive-delta checks.
3. Implement loader, GPU fields and portable snapshots. Run the new tests plus
   existing mesh/procedural regressions and compare frozen static reference bytes.
4. Prepare/run native FacetEB on identical geometry with explicit weather/history.
   If a graph or convergence issue occurs, diagnose and preserve the failed run;
   do not silently substitute prescribed temperatures or unrelated geometry.
5. Export fields; render/check sun/shade/evening outputs. Compare pixel-centre
   temperatures against their exact source triangle and camera-facing side.
6. Fresh-context scoped code review, repair concrete findings, final targeted
   verification and source/hash/usage receipts. No commit/push/release.

Outputs: a new `thermal_field_20261108` case beside `mesh_scene_20261108`.
Task4 continuous damaged-mesh demonstration follows the static thermal review;
no penetration/blast/structural prediction is introduced.

## Completion record

Task3 completed: native_v5 has pre/post input receipts and25 converged GPU
nodes. Six targeted test groups passed. Existing prescribed mesh outputs
(18 files) and procedural outputs(15 files) remain byte-identical. Geometry
mapping max error7.63e-6m; centre-ray temperatures match source float32 values
exactly, and maximum spectral-radiance error is7.35e-6W m-2 sr-1 um-1.
No field, instrument or structural validation. Task4 remains future work.
