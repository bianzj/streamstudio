# Body-mounted camera synchronization implementation plan

> Execute inline with executing-plans and one fresh final scoped review.

Goal: continuously render RGB and IR from a camera mounted on the existing
animated body and synchronize it with the completed external-view sequence.
User opted out of confirmation questions. Existing trajectory is authored,
straight, with fixed body orientation. This task adds camera/rendering only.

Architecture: optional sequence.cameraMount={body:"projectile",offsetMeters:[x,y,z]}
uses exactly the mesh body's right/up/forward basis and projectile(time).
Keep the selected camera's FOV, look along body forward. Evaluate every causal
shutter sample in native renderAt(), export camera.csv with sampled poses.
Reject conflicting chase camera and frame ranges reaching first contact.
Native footage ends before contact; synchronized display marks signal lost
thereafter. No fabricated surviving onboard observation after collision.

Keep current codex/animation-mesh-isolation checkout with existing uncommitted
animation source: a new checkout would omit this running implementation.
Freeze previous134source snapshot and output hashes; no commits or migration.
New case: D:/cases/StreamStudio/supplemental/impact_gpu_20261107/body_camera_sync_20261108.
Building/vegetation ground field, rigid, fire, transport, sensor remain identical.

Alternatives: static former onboard_still cannot provide motion; chase-offset
looks back at body and is not onboard; selected solution is native body mount.

Review focus: basis handedness/self-occlusion, exposure subframe synchronization,
contact time and final frame rounding, unchanged legacy cameras/flow, honest
post-contact display and camera records. No guidance or flight-model changes.

- [x] Test native moving pose against independent reference positions, all
  exposure timestamps; fixed-camera render equivalence; unsupported body,
  offset, tracking conflict and contact-range guards. Run existing engine RED.
- [x] Modify mesh_timeline.h/.cpp (mount parse/view/description), mesh_sequence.cpp
  (range guard/optional pose CSV/manifest); native build GREEN, relevant exact
  compatibility fixture. No shader edits.
- [x] make_body_camera_case.py copies configuration only; native full-resolution
  frames1..74 at24fps, same initial scene/field/camera settings. Render + inspect.
- [x] package_body_camera.py emits full-timeline external/RGB/IR comparison and
  short valid RGB/IR footage; source timestamps, full decode/hash receipts.
- [x] verify_body_camera_case.py verifies poses and field, prior source/output,
  original flow/rigid prefixes, frame/video counts/ADC and source snapshot;
  fresh scoped review once; preserve any failed checks and fixes.

Expected: RED mounted camera ignored/missing pose record; GREEN source/poses
and static-equivalent image exact; all video decode counts match manifest.
Only related checks, as project AGENTS.md requires; no broad/full scientific
validation or model performance claims. Ledger in new case/progress.md.

Result:74valid body frames,288synchronized display frames,222shutter poses.
Independent max position error3.255e-6m/forward1.933e-7; original ground field
exact and rigid/transport prefixes exact;139current source files,134previous
sources and1728previous native images preserved. All3videos fully decoded.
Fresh review: no blocking issue; relative-path rebasing in maker deferred minor
(delivered source inputs absolute). Final numerical/media checks passed.
