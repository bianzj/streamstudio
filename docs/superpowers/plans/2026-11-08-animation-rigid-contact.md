# Geometric contact and rigid fragments

> Execute inline with executing-plans and test-driven-development; scoped fresh review at completion. No commit, push or release. User approved continuation of priorities1+2.

Goal: replace timed contact and authored landing in a new opt-in `motionBackend: rigid-v1`, retain legacy code and frozen videos.

Architecture: native Bullet3.25 CPU convex sweep for first geometric contact; native fixed-step rigid boxes collide with the actual static remainder and surrounding mesh and with each other. Existing HiStream Vulkan GPU rendering/IR/transport stays in place. Supplied fragment release times, launch velocities and preset fracture geometry remain authored. No blast, penetration, fracture-energy or structural-failure prediction. Dependency archive pinned by SHA256.

- [x] Observe existing engine fail a rigid-backend behavior check.
- [x] Add isolated collision/rigid_scene module and optional build flag. Test analytic convex sweep, thin-wall miss/contact, free fall/time-step refinement, stack support, body-body collision and deterministic repeated stepping.
- [x] Integrate actual first contact and rigid poses into MeshTimeline without changing default authored branch. Synchronize existing thermal effects and obstacle switch with derived contact time. Export solver identity, source scope and measured contacts/penetration instead of fabricated old landing guarantees.
- [x] Build histream, runtime resources and targeted tests. Run legacy output comparisons. Render separate native12s sequence and contact slow-motion clip, decode completely, inspect frames, freeze source/inputs and verify protected receipts.

Files: `models/histream/src/animation/collision/rigid_scene.{h,cpp}`, `sequence/mesh_timeline.{h,cpp}`, `render/mesh_sequence.cpp`, `cmake/scene-animation.cmake`, `tools/scene-animation/tests/collision_fixture.cpp`, `test_rigid_sequence.py`, README. Case: `D:/cases/StreamStudio/supplemental/impact_gpu_20261107/rigid_collision_20261108`.

Validation: generic fixture tolerances are numerical checks, not measured material accuracy. First-contact plane test <=1mm; free-fall error decreases on halved step; settled stack surface gap <=1cm; body-body collision resolves ordering; repeated runs identical on same runtime. Observe/record actual full-case penetration and remaining visual limits; never substitute preset no-overlap endpoints for a contact test.

Completed:9targeted groups;102legacy reference files byte-identical;178disjoint fragments with24000arrangement cells preserving removed-volume union. Final native288frames/12s passed both .02m acceptance gates: sampledground.000714870m and all-substep contact.003546137m. Bullet CPU base1/1920s with1..16adaptive subdivisions;90621internal steps over8.899s.4videos fully decoded. Earlier result/ candidate rejected; result_final/ is delivery. Static reviewer findings fixed and regraded. No independent material/field validation; no commit/push/release.
