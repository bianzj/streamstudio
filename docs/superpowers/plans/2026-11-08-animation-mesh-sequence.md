# HiStream mesh impact sequence execution plan

User requests the explosion section and video. Execute Task4 of the approved
module/mesh plan inline using executing-plans; do not pause for confirmation.

Deliver a12s,24fps,1280x720 native GPU infrared sequence: approach, impact flash,
fireball, moving debris, plume and persistent cavity. Retain fixed spectral
radiance frames and an independently mapped generic camera video. Preserve all
earlier code/output snapshots and unrelated user edits; no commit/push/release.

## Design decisions

- New `sequence/mesh_timeline.*` owns immutable preset assets and kinematics;
  `render/mesh_sequence.*` owns continuous GPU dispatch and receipts. Existing
  static mesh and procedural commands remain regression references.
- Generate a closed thick-wall remainder and partition the removed volume into
  explicit cuboid fragments, with fracture-surface materials. Their union at rest
  reconstructs the target. Keep background native FacetEB temperatures and exact
  source-triangle links for surviving faces; newly exposed fracture temperatures
  and fragment heating are explicitly supplied. No blast/penetration/failure
  prediction or damage-radius estimation.
- Preset chunks move continuously through the opening, then follow gravity and
  declared resting contact behavior on the actual ground mesh. No full rigid-body
  or structural solver. Preserve damaged OBJ assets and fragment/link tables.
- Reuse the existing low-speed GPU heat/tracer numerics through a shared shader
  body. The mesh variant supplies its own contact point and a ray-query-derived
  coarse obstacle mask from the original/damaged mesh, rather than using the old
  hardcoded procedural city. Debris is render-only at this coarse flow scale.
- Use Vulkan BLAS/TLAS rendering. Reuse background geometry; update visual mesh
  geometry as fragments move. Thermal volume transfer uses GPU air heat/tracer;
  unchanged walls receive a single field perturbation on top of native baseline.
  Fracture/fragment supplied heating is disjoint from that wall perturbation.
- Reuse the generic SceneSensor for display PSF/noise/AGC. Preserve raw radiance
  and fixed-display frames separately from sensor video; camera is not calibrated.
- Use one fixed viewing geometry and12s timeline. Native writes all288frames;
  floating products at selected before/impact/plume/aftermath frames, plus a
  no-media before/after geometry inspection pair and old/new comparison.

## Steps and evidence

1. Freeze Task3 source/runtime/output hashes; create a fresh case and ledger.
2. Add failing native timeline tests: output count, deterministic seeking,
   invalid assets/timing, cavity persistence, positive finite radiance, and
   fragment contact bounds. Keep tests small; production video separately.
3. Implement asset generator, timeline, mesh mask/transport adapter, renderer
   and sensor coupling; compile matched C++/GLSL targets.
4. Run targeted old/new tests, verify static/procedural reference outputs remain
   byte-identical, review the new code and fix concrete findings.
5. Render full12s, inspect frame stages and unobscured cavity/debris views,
   encode video and decode it completely. Record all inputs/source/shader/engine
   hashes, frame coverage and validation limits. Return the actual video.

Active source: `C:/work/streamstudio`. New case:
`D:/cases/StreamStudio/supplemental/impact_gpu_20261107/mesh_video_20261108/`.
Old thermal_field_20261108 and earlier cases remain frozen references.

## Completion

All steps completed. Seven targeted test groups, old static24file and procedural
15file byte comparisons, clean parallel7shader build and moduleOFF build passed.
Final1280x720/24fps/288frame sequence and allthree12s videos decoded completely.
Actual landing SAT has0overlaps; max vertex ground penetration1.87e-9m and
rest support gap9.32e-10m. Coarse mask and preset contact limits are documented.
No field/structural/camera accuracy validation; no commit/push/release.
