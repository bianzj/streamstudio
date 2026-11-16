# Distant body-camera approach

Goal: extend the existing CGI body camera's continuous far-to-near approach.
User opted out of questions. Reuse native mount/shaders/sensors; no engine or
guidance/dynamics changes. Retain all previous cases and their139source snapshot.

Design: prepend9seconds of authored straight translation at the current visual
translation rate. Nominal start-to-source distance becomes about124m instead
of31m; requested contact12s, actual mesh contact about12.0596s. Fixed55deg
FOV, actual translation rather than digital zoom. Body camera valid about290
frames/12.08s. Recompute external504frames/21s on this same extended timeline;
keep original9seconds after-contact coverage. Body panels mark signal lost.
RGB clear, IR384x216/14bit/mild PSF/2% display unchanged. Same meshes, materials,
ground physics, original fire settings; existing finite transport domain stays
unchanged (very distant trail outside that domain is not resolved).

New case: D:/cases/StreamStudio/supplemental/impact_gpu_20261107/distant_body_approach_20261108.
Config-only creative change: use existing codex feature checkout, add separate
generator/verifier and case, no C++/shader edits or rebuild. Source changes are
scoped scripts/docs only. No commits/push/release. Use executing-plans inline,
one fresh final scoped review, retain ledger and all failed checks.

- [x] RED: verifier duration gate on previous body case must fail(<10s).
- [x] make_distant_approach_case.py creates both configs from frozen observer
  and body inputs, rebases dependency paths; asserts shared fields except cameras,
  frame ranges. Native tiny pilot confirms contact/frame limit and far composition.
- [x] Full native external and body sequences, all original settings/geometry
  and causal shutter mount retained; inspect first/middle/near-end frames.
- [x] Reuse package_body_camera.py for3videos, same21s timestamps and loss cards;
  full decoding and encoded-frame correspondence. Contact/source hashes verified.
- [x] verify_distant_approach_case.py checks>=10s/>=3.8x distance, independent
  all-sample mounts, fixed FOV, monotone distance, image growth, matching shared
  field/rigid/transport, ADC/2% unchanged, frozen artifacts and source snapshots.
  One final read-only review, actual media checks; no broad scientific validation.

Approaches considered: digital zoom cannot establish translation/parallax;
retiming old3s footage adds no distant geometry. Chosen authored pre-roll renders
new rays from farther away. Remains a visualization, not a flight prediction.

Completed: native290body/504external frames, all3videos fully decoded; body
12.0833s, sync21s. Actual initial camera distance124.692m, final0.16066m,
870causal shutter poses, max independent position error1.289e-5m; IR percentile
error1.285e-6. Shared ground exact and rigid/transport prefix exact.142current
sources,139old source snapshots and444old native images preserved.65native
core files/engine unchanged. Encoded/native frames42.66-47.52dB PSNR; actual
first/middle/near and RGB/IR stage sheet visually inspected. No measured sensor
or flight validation claimed. Final scoped review had no actionable finding.
