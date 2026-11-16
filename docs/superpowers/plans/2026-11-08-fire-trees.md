# Sustained fire and supplemental trees implementation plan

> Execute inline with executing-plans/TDD. User explicitly opts out of questions/approval. Retain current feature checkout and all prior cases; no commit/push/release. One fresh scoped review before final render.

**Goal:** add persistent visible/IR fire and heat/smoke sources, plus trees in a separate case while retaining the existing camera observation chain.

**Architecture:** optional fixed-size FireSources storage buffer shared by mesh rendering and low-speed transport; optional separately hashed MeshScene scenery merged only into the cached rendering background and its material table. Original FacetEB binding, rigid geometry/dynamics and authored flight/fracture remain unchanged. Added trees use explicit prescribed temperatures, not fabricated energy-balance records.

**Spec/design:** This document records the scoped design. Existing one-shot fireball decays quickly. Options considered: shader-only flames (no added advected smoke/heat), rebuilding the entire urban FacetEB case (different scientific scope), or supplied fire sources plus separately tagged static scenery. Choose supplied sources plus scenery to satisfy the visual request while preserving native background provenance.

**Constraints:** formal repo C:/work/streamstudio; new case D:/cases/StreamStudio/supplemental/impact_gpu_20261107/fire_trees_20261108; reference camera_physical_20261108 frozen. Existing14 unrelated modified files protected. Mesh push constants remain128bytes and legacy transport80bytes. Extra tree geometry affects RGB/IR visibility/shadows only, not Bullet contacts or the coarse fluid obstacle mask. No ignition prediction, fuel chemistry, tree combustion/spread, structural solver or device calibration. Keep8bitdisplay/native14bitIR,384x216IR,960x540RGB,1280x720render,24fps/288frames.

**Review focus:** disabled options retain exact output; scenery snapshot replays without original paths; extra material indices remain valid for static and moving triangles; no stale background bindings/false tree thermal provenance; source timing/domain validation and deterministic partial seeking.

## Tasks

- [x] Write native integration RED: manifest distinguishes sustainedFire/scenery; late RGB flame and raw IR change; pre-ignition equality; tree silhouette/known prescribed temperature; larger transported smoke; seek/replay identity; invalid source/hash guards.
- [x] Add FireSources parser/GPU block, shared activity envelope and shader appearance; bind it to renderer/mesh transport. Normalize thermal mixture in radiance space, preserve default zero-source path. Record supplied parameters, limitations and per-source counts/timing in manifest.
- [x] Add SupplementalScenery loader/append/snapshot. Reuse strict MeshScene parser and asset hashes; append only to cached render background with material remap; mark surfaces as prescribed thermal state. Keep original FacetEB and rigid source untouched.
- [x] Generate original procedural tree meshes (trunks/branches/irregular crowns, varied sizes/colors) on explicit vacant patches; add soil/grass patches. Separate scenery recipe/metadata and fire source parameters. Freeze inputs and check exclusion zones.
- [x] Build histream +histream_runtime_resources, run targeted fire/scenery and camera/default compatibility tests, fresh scoped review and one repair pass.
- [x] Render new full12s RGB/IR sequence, package/decode3videos, inspect encoded scenes, preserve prior source/results and record source/runtime hashes, exact rigid timeline identity and explicit thermal/physical validation scope. Regenerate the auxiliary onboard still with the added scenery.

Files: scene/fire_sources.{h,cpp}, scene/supplemental_scenery.{h,cpp}, shader/animation/fire_sources.glsl, mesh/persistent_fire.glsl; integrate render/mesh_sequence.cpp, transport/scene_transport.{h,cpp}, sequence_effects.glsl, transport_solver.glsl, scene-animation.cmake. Tools: make_fire_trees_case.py, tests/test_fire_scenery.py and case verification. Existing camera/rigid solvers retained.

Final evidence: fire_trees_20261108/final-verification.json. Full native423.38s,288frames/12s,3videos completely decoded.40trees/57600supplemental triangles,3prescribed sources. Rigid timeline/CSV exact and12original background files unchanged.93source files;82prior source,308packaged artifacts,1728prior native frames and14protected repository files verified. Native/onboard/decoded comparison images visually inspected. No independent fire, device, vegetation-energy or structural validation.
