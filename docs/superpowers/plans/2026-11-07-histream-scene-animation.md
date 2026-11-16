# HiStream GPU scene animation

**Goal:** Native HiStream Vulkan/GPU continuous images and MP4 of a fictional
projectile hitting an empty building, followed by procedural fire and smoke.

**Architecture:** An explicit `eSceneAnimation` CLI mode uses HiStream's
`AppSetting` GPU context and a compute shader. Geometry, animation and volume
integration run on the GPU. CPU code validates input, dispatches frames and
writes PNGs. A Python presentation tool adds captions and encodes MP4.

**Scope:** Prescribed motion, facade opening, fragments and procedural effects.
No guidance, detonation, compressible-flow, structural or calibrated spectral
solver. Existing RT/EB modes remain unchanged. No automatic commit or release.

1. Add a failing CLI regression for invalid dimensions and a small GPU sequence
   spanning before/after impact. Require a GPU receipt, complete frame count,
   distinct frames and nonempty PNGs; reject overwriting existing outputs.
2. Add `src/base/scene_animation.{h,cpp}`, CLI routing, the native Vulkan compute
   pipeline, checked barriers/readback, deterministic times and run manifest.
3. Add `shader/animation/scene.comp` and CMake compile/deploy dependencies.
4. Build only `histream` and `histream_runtime_resources`; run targeted checks.
5. Render keyframes, inspect appearance, then render 288 frames at 1280x720,
   24 fps. Deliver MP4, preview, PNG sequence, config and provenance under
   `D:/cases/StreamStudio/supplemental/impact_gpu_20261107`.

The prior Blender draft is superseded. Use a separate native output directory.
Validation covers code, GPU execution and rendered visuals, not physical accuracy.
