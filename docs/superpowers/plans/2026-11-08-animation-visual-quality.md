# Native GPU visual-quality pass

User requested implementation of the proposed realism path. Preserve every
previous case and create `D:/cases/StreamStudio/supplemental/impact_gpu_20261107/visual_20261108`.

1. Add a `sceneStyle: detailed` variant: varied setbacks/roofs, material scales,
   street furniture/parked vehicles and a staged irregular breach with exposed
   slabs/columns. Shared major geometry drives rendering and transport; tiny
   visual accessories are explicitly subgrid/render-only. No structural solver.
2. Improve prescribed background thermal heterogeneity and interior materials;
   keep the existing transported temperature perturbations and supplied sources.
3. Optional native GPU `thermalSensor`: operate on float linear radiance before
   display mapping, with detector binning/PSF, configured noise, fixed pattern,
   temporal AGC and display quantization. This is a generic illustrative camera,
   not a calibrated instrument. Save radiance samples and AGC diagnostics.
4. Tests: invalid options fail; sensor processing preserves grayscale and finite
   radiance; blur/noise affect output; requested frames match a warmed sequence.
   Run relevant existing animation/transport checks after shared geometry edits.
5. Inspect static visible/IR scene and unobscured damage first, then render the
   12 s IR video. Deliver side-by-side comparison to the previous version, plus
   source/snapshot hashes, numerical and full-decode receipts. Do not equate
   aesthetic improvement with independent physical validation.
