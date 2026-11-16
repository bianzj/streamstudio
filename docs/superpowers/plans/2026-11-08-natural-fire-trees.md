# Natural tree and fire appearance implementation plan

> Execute inline with executing-plans/TDD. User opts out of confirmation. Preserve previous cases and existing feature checkout; no commit/push/release. One fresh scoped review before full render.

**Goal:** replace solid polygon crowns and regular cone flames with porous leaf geometry, smooth branches, leaf backlighting and an irregular animated volume appearance; retain optical/IR videos and onboard still.

**Architecture:** separate natural_vegetation.py and case generator; opt-in `sequence.appearanceVersion="natural-v2"` chooses sequence_natural.comp. Original sequence/sequence_fire shader paths remain. Supplemental material `thinLeaf:true` tags static leaf triangles for an approximate two-sided diffuse transmission term in RGB. natural_fire.glsl supplies advected multi-scale noise, reaction-sheet-like flame shapes, detached tips, mottled embers and smoke sunlight attenuation. Actual existing supplied heat/tracer transport and Bullet dynamics remain unchanged.

**Spec/choices:** use actual small leaf meshes rather than denser closed crown ellipsoids or downloading external models. Use existing low-speed fields plus bounded procedural fire detail rather than claiming reactive combustion. Keep40tree positions/clearance bounds,3source timings/heat rates,12s/24fps/1280x720render,384x216IR/14bitADC and existing camera degradation. Tree temperatures remain prescribed; no wind-driven geometry, vegetation EB, ignition prediction or fire spread.

**Review focus:** triangle normal/winding/finite-area and storage budgets; canopy bounds/porosity; leaf double-sided illumination and geometric self-intersection offsets; no natural path changes to old shader arithmetic; natural fire before onset, finite IR mixing/Planck, deterministic seek; smoke shadow cost/consistency and clear visual improvement before full render.

- [x] RED geometry tests: explicit normalized normals, nondegenerate triangles, bounded leaves and porous canopy; native natural-v2 metadata missing on old engine.
- [x] Add smooth wood tubes, curved leaf meshes, irregular branched/spray distribution and separate case inputs; freeze previous93source files and old native/video records.
- [x] Add opt-in shader variant, thin-leaf tag/lighting, natural volume flame and ember appearance and smoke sunlight attenuation. Preserve old models and source/rigid physics.
- [x] Run numerical/native isolation/seek/guard tests, inspect medium-size RGB/IR preview and compare with old; fresh scoped review and consolidated repair.
- [x] Full native render, completely decode videos, update onboard still, inspect actual encoded frames and save source/runtime/previous-data protection receipts. State no physical-camera/combustion calibration.

Files: tools/scene-animation/natural_vegetation.py, make_natural_fire_trees_case.py, tests/test_natural_vegetation.py, tests/test_natural_appearance.py; shader/animation/mesh/natural_fire.glsl,sequence_natural.comp; guarded modifications in persistent_fire.glsl,sequence_effects.glsl,physical_appearance.glsl; render/mesh_sequence.cpp,scene/supplemental_scenery.cpp,cmake/scene-animation.cmake. Existing original assets/configs and formal runtime paths retained.

Final evidence: natural_fire_trees_20261108/final-verification.json.102source files; native full507.82s,288frames/12s,4completely decoded videos including matched old/new comparison.40trees/132000canopy leaves/589280scenery triangles,4scene samples/pixel. Rigid timeline/CSV,transport.csv and12original background files exact. Leaf backlight changesRGB and leavesIR byte-identical in isolation. Actual encoded frame, source reference and new native tree/onboard stills inspected. No combustion/vegetation/device calibration claimed.
