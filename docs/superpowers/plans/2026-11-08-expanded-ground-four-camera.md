# Extend ground behind the unchanged four-camera view

User wants the same externalRGB/externalIR/forwardIR/forwardRGB arrangement,
same124m approach, but no visible finite scene floor border at the start.
Questions opted out. Execute inline, one fresh scoped review at end.

Choose ground extension only: original street/buildings/trees preserved, rather
than copying the impact building or adding unnecessary repeated vegetation.
Append a coplanar asphalt ring to supplemental scenery, leaving a precise hole
for original ground[-95,95]x[-80,85],z=-.03m. Multiple nested rings avoid long
thin triangles near the old join. Outer floor extends to+/-12000m, beyond every
primary traceable ground hit (native trace limit10000m, camera within100m).
Thus the boundary cannot occur inside primary traced ground rays; natural sky
remains.64or fewer added triangles, no scene mesh/physics code changes.

Use same asphalt optical material and emissivity. New far-ground baseline is
the median of actual parent unshaded asphalt temperatures (~324.02K), explicitly
a background proxy; do not claim full far-field energy balance. Original local
ground EB/trees and physical parameters remain as before. Compare actual field
bytes rather than assuming invariance: the larger BLAS changes ambiguous hits
on original coincident curb/soil faces.136cells differ, max0.0484K; receiver,
geometric sky and direct-solar factors exact. Keep shared new field and report
the comparison within the existing0.06K imaging noise parameter (not accuracy).
No new trees/buildings or thermal-shadow inconsistency introduced outside core.

New case D:/cases/StreamStudio/supplemental/impact_gpu_20261107/expanded_ground_four_camera_20261108.
Freeze old code/output and4764native frames. Preserve all camera/timing/source
inputs except the appended scenery reference. Engine/shaders unchanged, no build.

- [x] RED all-pixel ground-ray coverage on old first-frame floor must fail.
- [x] Generator writes new ring OBJ/scenery, both configs and tiny first-view
  pilots. Geometry/coverage certificate passes; inspect native first-frame images.
- [x] Full504external/290body native frames, same21s timing. Verify shared
  new ground/rigid/transport, preserve original data and report core-field delta.
- [x] Finalizer+compose_four_camera.py produce2560x1600 four-view video in the
  requested order, full decode, actual encoded first/middle/after-contact QA.
- [x] Independent review, source snapshot and preservation receipts, final
  coverage/pixel/timeline checks. No field/flight/device validation claims.

Result: native290/504frames complete; quad504frames/21s/2560x1600 fully decoded.
Actual encoded opening and6s frame visually inspected: rectangular floor edge
removed in both external/forward RGB/IR. Camera CSV, rigid/transport files exact
to parent; new field shared exactly, original tiny material-tie delta reported.
147current source files and4764old native images preserved. Actual geometry/
config/run hashes, independent hole/overlap/area checks, both all-pixel ground
coverage views outsideFloorRays0. Review P2binding fixed RED→GREEN with3fixtures;
no second review. Far-floor remains a prescribed proxy, not full-domain EB.
