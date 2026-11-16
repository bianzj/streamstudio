# Physical appearance and camera observation implementation plan

> Execute inline with executing-plans/TDD; independent scoped review at completion. User explicitly opts out of confirmation. Keep existing feature checkout and frozen case.

**Goal:** improve generic material/light response and add physically ordered camera effects to RGB/IR, with image/video comparisons.
**Architecture:** opt-in physicalAppearance shading plus opt-in cameraObservation GPU module. Canonical geometry/IR stay separate. Render Simpson3 temporal scene samples over the exposure, ending at the canonical frame timestamp; integrate in linear signal before detector electronics/display. Same CPU rigid solver; no new blast/penetration/structural model.
**Inputs:** generic profile in separate camera_physical_20261108/sequence.json; no device/model/calibration supplied. Parameters are illustrative, not FLIR/EMVA compliance.

- [x] Observe missing observation output RED on prior executable.
- [x] Add explicit roughness/metallicity/IOR, GGX/Smith/Schlick shading, stable material texture, resolved-tracer sun attenuation, conservative air haze. Preserve previous optical/default branch. Glass transmission and full global illumination remain outside scope.
- [x] Add GPU camera module/shaders: normalized separable PSF,4x4pixel-aperture quadrature at arbitrary detector dimensions, continuous SO3 camera jitter and fixed IR boresight error, Simpson3 exposure accumulation, detector response/FPN/read+row noise, ADC quantization before AGC,8bitdisplay reconstruction. Save native14bit IR codes in16bitPNG containers, observed RGB/IR and numeric receipt.
- [x] Native synthetic fixtures: constant-field preservation, offset-grid ramp, PSF edge spread, ADC limits and display independence, response and deterministic noise. Integration: enabled/disabled, ideal channel preservation, deterministic seeking, bad config, jitter/blur comparison.
- [x] Build/check scoped regressions, independent review, native full sequence, verify physical-trajectory identity and canonical IR scope, inspect encoded videos, freeze source/inputs/receipts.

Camera defaults:384x216IR and960x540RGB within1280x720source, exposure1/60s, Gaussian PSF sigma .85/.55 detector pixels; linear aperture coverage .9/.95, IR14bit/RGB12bit ADC,8bitdisplay. IR NETD-like .06K at300K, response .012s, ADC Planck10um range250..650K. These are selected generic values, not measured equipment specs. Raw sample units IR W m-2 sr-1 um-1; RGB uncalibrated linear scene units.

Files: new observation/camera_observation.{h,cpp}; camera/{filter,integrate,detector,display}.comp; mesh/physical_appearance.glsl; modify mesh_sequence.cpp, render_common.glsl, material_table.*, scene-animation.cmake. New native camera_fixture.cpp and Python integration test. New packaging mode for ideal/observed comparisons. Do not overwrite previous case/source. No commit/push/release.

References: EMVA1288Linear4.0 (https://www.emva.org/wp-content/uploads/EMVA1288Linear_4.0Release.pdf); PBRT microfacets (https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory). FLIR public page illustrates resolution/IFOV/NUC/NETD concepts only; this preset is not a reproduction of that camera (https://www.oem.flir.com/products/boson-plus/?model=23640A012).

- [x] Additional user request: rendered a single-pose fictional onboard IR view at t=1s using the same camera model; pose and 16-bit-container native ADC image retained. Camera translation held within this still exposure.

Final evidence: camera_physical_20261108/final-verification.json;82 source snapshot files;597 canonical reference files byte-identical and timeline/rigid/transport diagnostics exact;3 videos fully decoded288frames/12s each; no measured-device or structural validation claimed. Four independent-review findings fixed.
