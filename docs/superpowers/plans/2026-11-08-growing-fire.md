# Gradual supplied fire growth

User requests small-to-large fire; preserve clear optical/mildIR+2% display,
actual-contact camera trigger, geometry, background and rigid inputs. Create a
separate growing_fire_20261108 case, leave old source/results untouched.

Add optional per-source growthSeconds (0 disables;0.1..25.5s, quantized0.1s).
Pack8one-byte duration codes into unused FireBlock header.y/z. Preserve the
48byte source/400byte block and seed/profile packet. Shared GLSL source accessor
smoothly grows radius20%->100%, height15%->100%, heat/tracer rate3%->100% after
each source's ignition. Render, lighting and transport share the schedule;
ray bounds retain adult maxima, local stepping covers tiny growing sources.
Omitted/zero settings return the original source exactly. No combustion,
fuel/oxygen, ignition/structural prediction or operational guidance changes.

- [x] RED native missing growth metadata, early/late fire-size check, changed
  supplied transport, zero/default compatibility,8source header packing, guards.
- [x] Implement guarded source schedule and dependent GPU reads; build engine/resources.
- [x] GREEN task-specific tests and old exact fixture; one fresh scoped review.
- [x] Native288frames/12s RGB/IR videos, stage sheet and old/new comparison;
  preserve camera options/rigid/background/historical files; scoped source snapshot.

Completed: independent native growth/heat/smoke tests pass;8source GPU probe
784scalar checks, maxerror5.88e-6. Mature fire converges exactly to the adult
canonical image in the isolated fixture. Old default210outputs/logs exact.
Full288frames, four complete video decodes, native stage crops, exact camera
settings/motion, rigid geometry/background and frozen previous case verified.
120scoped source files captured. See growing_fire_20261108/final-verification.json.
Review/evidence limits and coarse-grid illustrative source assumptions remain
documented; no combustion/structural/device calibration claims or publication.
