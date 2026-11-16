# Native animation heat and smoke transport

User continuation authorizes extending the native GPU infrared example.
Preserve all previous runs and keep the existing procedural mode as default.

## Design

Add an optional `physicsMode: transport` dense-grid module to eSceneAnimation.
Use a low-speed incompressible velocity projection, semi-Lagrangian advection,
explicit eddy diffusion, thermal buoyancy, and grey radiative cooling. A thin
solid surface layer carries a temperature perturbation, exchanges convective
heat with adjacent air and loses heat by radiation. Material baseline surface
temperatures remain prescribed. Heat/smoke sources and the initial hot patch
are supplied conditions, not predicted impact energy or combustion.

Use a 64x56x56 grid at 1 m; fixed 1/60 s substeps; warmed from t=0 even for
partial frame exports. GPU ping-pong buffers and bounded command batches;
snapshot the transport shader and export field statistics per frame. Render
thermal gas/smoke directly from the evolved field, not animated envelopes.
This is an illustrative coarse-grid transport solver, not blast/structural
analysis or a calibrated IR sensor model. Existing LBM is not claimed as used.

## Steps and checks

1. Add failing integration tests for physics mode validation, ambient equilibrium,
   passive tracer translation/spreading, plume rise, finite/nonnegative fields,
   hot-surface cooling, and deterministic warmed frame seeking.
2. Implement `src/base/scene_transport.{h,cpp}` and `shader/animation/transport.comp`;
   connect state readout to `scene.comp` and expose precise provenance/diagnostics.
3. Build histream and histream_runtime_resources; run only relevant native tests.
4. Render a separate 12 s infrared sequence. Check full video decoding and
   numerical diagnostics, inspect stage frames, and produce a before/after
   image/video with the same camera and display range.
5. Preserve source/snapshots and scientific limits in the new case directory:
   D:/cases/StreamStudio/supplemental/impact_gpu_20261107/transport_20261108.

No automatic commit, push or release. A small apparent change or reversed
numerical result must be reported honestly. Semi-Lagrangian transport is not
strictly mass-conservative; report tracer drift rather than claiming conservation.
