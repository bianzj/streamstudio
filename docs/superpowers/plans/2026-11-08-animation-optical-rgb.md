# Visible RGB sequence beside infrared

User requests optical images/video of the existing illustration. Continue directly without confirmation; preserve frozen rigid_collision_20261108 and its68file source receipt.

- [x] Observe RED: current engine ignores sequence.optical and emits no optical_frames.
- [x] Add opt-in sequence.optical boolean. Existing GPU material/sun/shadow/sky shading stays as geometry diagnostic; additional RGB output uses separate unused Pixel.color.w. Same camera, meshes, actual rigid trajectory, contact time and transported tracer. Prescribed jet/fire emission, smoke/dust single-scatter appearance, local fire light and a generic fixed tone curve; not a spectral optical/camera calibration.
- [x] Keep existing IR/radiance/geometry outputs and default mode unchanged. Test paired optical-on/off IR identity, visible non-grayscale, media ablation, deterministic seek and invalid option.
- [x] Extend packaging with --optical for RGB captions, complete decode, stage images and slow collision crop. Render separate native288frame12s dual sequence, compare physics/thermal receipts with prior frozen run, inspect actual encoded frames and deliver images/video with source/check receipts.

Files: shader/animation/mesh/{render_common.glsl,sequence_effects.glsl}, src/animation/render/mesh_sequence.cpp, tools/scene-animation/{package_mesh_sequence.py,tests/test_optical_sequence.py,README.md}. Same existing feature checkout; no collision solver changes or new external dependencies. Case: D:/cases/StreamStudio/supplemental/impact_gpu_20261107/optical_rgb_20261108.

Completed:3targeted groups; full native288IR+RGB frames/12s.602prior IR/geometry/physics files byte-identical, including timeline/transport/rigid/sensor receipts.4videos decoded completely (3x288frames/12s,131frame10.916667sslowcrop).6native1280x720RGB stage images +1920x800contact sheet. Source70files and prior68source/461output receipts protected. No collision solver changes, independent optical calibration, commit/push/release.
