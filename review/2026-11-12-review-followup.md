# Scene-animation review follow-up - 2026-11-12

Scope: the items of `StreamStudio场景动画审阅问题清单_2026-11-12.docx` that were verified against the source,
plus four issues found during verification. Originals were backed up to
`C:/work/streamstudio_backup_20261112_1134/` (tools/scene-animation, models/histream/src and shader, and the
previous `runtime/Release` build).

## Changed

### Engine (models/histream/src/animation)

1. **requireSolidStatic is enforced for replays (new finding).** The static collider is classified before the
   cache lookup; `rigidRequireSolidStatic: true` now fails even when a recorded track exists. Replayed statistics
   report the guard and layout of the current run, not of the recording.
2. **Static collider per part (P1-02).** The ordered all-box layout is unchanged (fast path, same cache key).
   Otherwise closed convex 12-triangle windows and 12-triangle welded-vertex components stay solid convex hulls;
   only the remaining triangles form a skin inside the same compound (`staticCollider` = `closedConvexParts` |
   `mixed` | `triangleMeshSkin`, `staticLayout`). Degenerate faces no longer pass the convexity test (NaN).
   A partially solid layout gets a different cache key.
3. **Track cache writes (P2-07 and new findings).** The track is written before receipts
   (`flushTrack`), with `writeOutcome` written / skipped-existing / failed. An existing valid track with at least as
   many steps is kept (a shorter recording can no longer replace a longer one), rename is retried with back-off,
   orphaned `rigid_*.tmp` older than one day are removed before recording.
4. **Top-level labels and warnings (P0-01, documentation path).** `run-manifest.json` has `physicsValidated:false`,
   `impactModel` (authored, not predictive, not-implemented impact-v1, equivalent slow-motion factor against
   250 m/s) and `warnings[]` (skin/mixed static collider, contact outside the authored breach, failed cache
   write). Manifests stay ASCII so existing GBK-default readers keep working.
5. **fragmentExit (P2-06).** Optional `sequence.fragmentExit {normal, planeOffset}` for the authored backend;
   the default evaluates the original `-2.8` expressions.
6. **Acoustic shake onset (P0-01 item).** Optional `cameraObservation.blastArrival: "acoustic"`: onset =
   contact + camera distance / sound speed (upper bound; no blast model). Default unchanged.
7. **Performance (P1-01).** `DynamicMeshAdapter`: one persistent adapter per triangle count; positions/triangles
   staged and BLAS/TLAS rebuilt in place with mode BUILD and the original flags (no refit), one fence. Strip
   dispatches: one command buffer per 32-row strip (TDR-safe as before) in one queue submission.
   `profileSeconds.logging` split into `loggingTransportStatistics` / `loggingRigidStatistics`.
   Opt-outs: `sequence.persistentGeometry:false`, `sequence.batchedStrips:false`.

### Python (tools/scene-animation)

8. **assert -> require (P2-03).** `integrity.py` (`require`, `IntegrityError`); 76 statement asserts in nine
   production scripts converted mechanically (AST spans), handlers catch `IntegrityError`.
9. **Encoder probe (P2-04).** `video_encoder.py`: lists encoders and test-encodes two raw frames before any
   rendering; libx264 -> libopenh264 -> mpeg4; `--video-encoder` in all eleven video scripts; encoder recorded in
   receipts. ffmpeg exit/decode failures raise with stderr.
10. **GPU memory pre-check (P2-05).** `run_runway_day.py` clamps `--parallel` to free memory / `--per-run-vram-gb`
    (nvidia-smi), refuses if none fits, records `parallelRuns`/`gpuMemoryNote`.
11. **Per-node arrays (P2-02).** `steps_manifest.py`: `output/steps_manifest.json` (SHA-256 + Merkle root of
    `.facet_steps/*.bin` and `diagnostics/*.json`) written after each run and covered by the case seal;
    `render_runway_day.py --verify-steps lazy|full|off` checks every array/diagnostic on first read; a manifest
    written after sealing is refused until the case is re-sealed.
12. **Resume recomputation (F-01 remainder).** Both runners recompute the convergence summary from the index and
    diagnostics on resume and refuse a receipt whose stored numbers disagree.
13. **airfield-day pipeline (P2-08).** `run_case.py check|run` with `pipeline: airfield-day` runs the seven steps
    and verifies both seals, the render receipt and video hashes. Sample `samples/airfield-day.job.json`.
14. **Deck literals (F-05 remainder, new finding).** ADC bits, node spacing, frames/fps, IR size, target counts,
    parallel count, residual limit and the still layout come from case products; chart axis ranges follow the data.
    The old slide-3 text "预热后组平均差 ≤0.001 K" contradicted the case closure (0.002 K, also on slide 20); it now
    prints the computed value.
15. **Packaging/docs.** Video titles carry "作者设定，非物理预测"; README/CURRENT_FEATURES/capabilities, the
    top-level README (research module, not in 2.0.1) and docs/histream-scene-animation.md status updated.

## Verification

- Byte identity, old engine (backup) vs new engine, reduced 192x108 / 24-frame configurations of
  `rigid_collision_20261108/sequence.json`, all output files compared except manifests:
  authored 86/86, camera observation 187/187, rigid-v1 87/87 identical. adapterBuild 1.97 -> 0.15 s,
  6.05 -> 0.18 s, 1.65 -> 0.20 s; wall 6.2 -> 2.7 s and 11.1 -> 3.0 s for the first two.
- `tests/test_review_fixes.py` (new, engine): mixed static collider (4 skin triangles, >6000 solid parts),
  requireSolidStatic refused on record and on replay, guard reported for replay, writeOutcome, shorter recording
  keeps the longer track, labels/warnings, fragmentExit default/custom/invalid, acoustic onset = contact + R/c.
- Python: test_case_seal 8, test_energy_layout 5, test_coupled_receipts 4, test_simulation_entry 24 (3 new),
  test_runway_day 10, test_integrity_tools 9 (new; also under `python -O`). Note: the conda env
  `envs/python311` crashes (0xC06D007F) in test_runway_day with the original code too; use the base
  `C:/work/miniconda/python.exe`.
- Existing GPU regression tests on the new engine: test_rigid_sequence (canonical rigid sequence; complete including
  cache record/replay/heal, outputThreads=0 identity and the final seek sub-run, 785 s total; the 11-11 run of the
  same test was aborted in its seek stage after 15 min), test_mesh_sequence (mesh_video_20261108/assets_final),
  test_camera_observation (camera_physical_20261108), test_fire_scenery (fire_trees_20261108), test_optical_sequence:
  all PASS.
- Real case, probe render (`--probe-nodes 2`, scratch output) with the conda ffmpeg that lacks libx264:
  libopenh264 selected up front, video decoded, render seal written; legacy cases print a missing-manifest warning.
- Deck rebuilt from the existing airfield case into scratch: 20 slides; text diff against the old builder limited to
  the intended values.

## Not changed / needs a decision (first round; resolved in the second round below)

- impact-v1, GUI/product integration (P0-01/P0-02) and S3/S4/S6 plus the transport statistics readback were left
  open in the first round; all are implemented in the second round.
- The delivered airfield case on D: was not modified: write its manifests with `python steps_manifest.py CASE`
  and re-seal it with `python case_seal.py CASE` when convenient.

## Second round (same day): impact-v1, GUI integration, S3/S4/S6 (development build, testing, not released)

Decisions (user): generic example parameters; physics at the physical speed, video slowed by
`presentationTimeScale` and labelled; full GUI/product integration with a new installer; existing cases are not
re-rendered, one new impact-v1 example is added.

### impact-v1 (P0-01)

- `src/animation/impact/`: `blast` (Kingery-Bulmash free-air/surface, oblique reflection, Rankine-Hugoniot speed),
  `penetration` (Young SAND97-2426 with K_h, nose N, concrete S), `empirical` (recovery temperature, Planck,
  fireball D = 3.86 W^0.32, t = 0.299 W^0.32), `event_model` (strict `impactPhysics` parser with unknown-key
  rejection, event solve, `event.json` receipt).
- `mesh_timeline`: `motionBackend: impact-v1` (mutually exclusive with impactTime / flightSpeed / fragmentRelease /
  fragmentExit); physical-speed flight, wall deceleration to the fuze burst; failed non-pulverised preset blocks
  become rigid-v1 bodies released at their blast arrival with v = (i_r - i_c) A / m; intact blocks stay static;
  recovery temperatures on the projectile; `presentationTimeScale` (frame time x scale = physical time).
- Transport `transport_impact.comp`: heat share injected as a Gaussian at the burst; dust released into the growing
  fireball volume at rate exp(-a/tau)/tau (tau = fireball duration). The authored 550/900 K pulses are not used.
  `impactHeatClosure` in the manifest (sample case 0.67 over one 1/60 s step).
- Render `sequence_impact.comp`: empirical fireball radius/temperature/fade, fireball light, dust inside the fireball
  at the gas temperature, no jet unless `powered`.
- Camera observation: `blastArrival: "blast"` (default with impact-v1, refused otherwise): shake starts at burst +
  K-B arrival at the camera distance, amplitude x min(1, Ps / 6.9 kPa). Physical fps passes into exposure/response.
- Manifest: `impactModel.kind = impact-v1`, label "empirical model estimate, not independently validated",
  `predictive: false`, presentation (time scale, slow-motion factor); warnings for clamped Z, KE above the blast
  share, widened heat Gaussian, no failed block, outside Young validity. `frames.csv` gains `physical_time_seconds`.
- `package_mesh_sequence.py`: impact-v1 titles "经验模型估算，未经独立验证 · 慢放 xN", physical time stamps,
  detail video slowed 2 x N, receipt fields `presentationTimeScale` / `slowMotionFactor`.

### Performance (P1-01/P2-01)

- S3 `sequence.instancedGeometry` (default on for impact-v1 only): `InstancedMeshAdapter`, one BLAS per rigid part
  built once per phase, TLAS instance transforms per sample, `*_instanced` shader variants (instance custom index).
- S4 `sequence.additionalCameras` (<= 7): samples of all cameras in time order with one transport/rigid/geometry
  update per sample time; per-camera sensor AGC and observation state; output in `cameras/<id>/`.
- S6 `sequence.checkpoints`: transport cells + step, mask switch, sensor AGC and detector histories every N frames,
  keyed by input (minus frame range/output options) + engine + shader hashes; `firstFrame > 1` resumes from the
  nearest earlier checkpoint instead of rendering all warm-up frames.
- Transport statistics: host-cached staging memory; `capture()` on the render thread, `summarize()` and the CSV row
  on one ordered worker (`statisticsDrain`).

### GUI / product (P0-02)

- Server `server/scene-animation.mjs` (/api/scene-animation/*, shares the HiStream engine slot), schema
  `SCENE_ANIMATION_MODE` job document, renderer panel `scene-animation.js`, user manual chapter 18, theory manual
  chapter 17, example catalog entry type `"mode": "eSceneAnimation"`. Second round additions: frames of additional
  cameras (`cameras/<id>/<kind>`, id-checked) and the physical time / slow-motion factor in the frame caption.
- Product engine: `tools/build-engine.ps1` now configures `HISTREAM_ENABLE_SCENE_ANIMATION=ON` and
  `HISTREAM_ENABLE_ANIMATION_COLLISION=ON` (local Bullet 3.25 source); fixture executables excluded from the package.
- Built-in example `assets/examples/impact_v1_building` (generic parameters, 340 m/s so Young is within validity).
- Version stays 2.0.1 (user, 16:39: still testing, no new release). `npm run pack:win` was tried once before that instruction: both engines built (product engine now with the animation module); electron-builder then failed on a download timeout (ETIMEDOUT), so no installer exists. No D:/releases directory was created.

### Verification (second round)

- `histream_impact_tests`: 157/157 (IATG 01.80, UFC/JRC worked examples, Swisdak fits, reflection vs R-H, cube-root
  scaling, monotonicity/continuity, Young US/SI equations, Planck, recovery temperature). Two fixture expectations were
  corrected: the JRC 5.2 example is evaluated at the report's Z = 2.56 (its printed 27.04 / 1302^(1/3) is 2.476), and
  the continuity threshold accepts the published slope kinks of the piecewise K-B fits (4e-3 instead of 1e-3).
- `test_impact_v1.py`: impact-v1 receipt/labels/physical time, energy and impulse budget, heat closure, charge
  monotonicity (10/40/160 kg), blast-arrival shake, refused inputs; S3 mean |diff| <= 0.0007 grey levels;
  S4 primary and additional cameras byte-identical to single-camera runs; S6 resumed frames 19-24 byte-identical
  across display/optical/observed/detector outputs. PASS.
- Old/new identity (authored, observe, rigid; reduced configs): all frames, CSVs and radiance files identical
  (86 / 187 / 87 files). Manifest differences are the first-round labels plus `geometryUpdate` text.
- All legacy render/transport SPIR-V (sequence, _fire, _natural, _varied, _ground, transport, mask) byte-identical to
  the released engine's.
- Node: `test-scene-animation.mjs` 9/9 (incl. additional-camera frame confinement), `test-assimilation-config.mjs` 3/3;
  `vite build` OK. GUI API end to end with the dev engine: example listed, opened (copied), inspected, run (99 s),
  result with additional-camera kinds, PNG served.
## Third round (same day, 17:00-18:30): scene-animation UI entry hidden

User decision: keep the code (engine, server, schema, panel, example) but hide the scene-animation part in the UI and
do not dispatch or run it from the interface; the engine CLI and JSON stay.

- `project-schema.js`: new `SCENE_ANIMATION_UI_ENABLED = false` (single switch; comment records the decision and how to
  re-enable).
- `index.html`: `#openSceneAnimationBtn`, `#researchMenuLabel` and `#researchMenuSeparator` carry `hidden` in the
  markup, so the entry is invisible even before scripts run.
- `main.js`: the three elements are revealed and the click handler is bound only when the switch is on; the example
  path logs a warning and shows a toast instead of opening the panel.
- `scene-animation.js`: `open()` returns false for every caller (menu, example, shortcut) while the switch is off.
- `server.mjs`: catalog entries with `mode: eSceneAnimation` are filtered out of `/api/examples` and `openExample`
  refuses them (400, Chinese message).
- Documentation: user manual chapter 18 (notice + CLI instructions), CURRENT_FEATURES, module README,
  `docs/histream-scene-animation.md`, `docs/example-library.md`, the next-release draft and the example README.

Verification: `test-scene-animation.mjs` 9/9 after the change; rebuilt web bundle; `/api/examples` lists only the five
project examples (0 scene-animation entries); `POST /api/examples/open` returns 400 with the refusal message;
Electron capture of the open "解决方案" menu shows only 三维解决方案 and 解析解决方案 groups, with
`#openSceneAnimationBtn`, `#researchMenuLabel` and `#researchMenuSeparator` all `display: none`, height 0; the panel
stays `hidden`. The engine CLI (`histream.exe eSceneAnimation ...`) and the product engine build are unchanged.
## Fourth round (same day, evening): UTF-8 paths, and case packaging under D:

User request: split the impact-v1 sample into two cases (impact and mid-wave infrared) under D:\cases, with Chinese
directory names; also keep only the PPT figures/videos/tables of the airfield case.

### Engine: UTF-8 file paths (fixes failure for any non-ASCII path)

`std::filesystem::path::string()` returns the local code page on Windows, so a scenario stored in (or writing to) a
directory with Chinese characters produced raw GBK bytes inside JSON values, and `nlohmann::json::dump` threw
`[json.exception.type_error.316] invalid UTF-8 byte`; GDAL also received non-UTF-8 names and failed with
`Cannot write sequence PNG`.

- New `src/animation/scene/path_text.h`: inline `animation::pathText(path)` (UTF-8 text, from `u8string()`) and
  `animation::pathFrom(std::string)` (`u8path`); inline so the small fixtures that link only part of the animation
  library do not pull in the OBJ loader through `mesh_scene.cpp`.
- JSON values and messages now use `pathText`: asset paths (scene receipt and the preset-asset config), shader path,
  checkpoint directory, rigid track cache directory and its receipt, mesh renderer snapshots, ground-thermal source
  path, background-field sources.
- JSON values are read back with `pathFrom`: scene assets, sequence asset/remainder/fragments/chunks references,
  background field and its sources, ground-thermal sources, supplemental scenery, checkpoint and rigid-cache
  directories.
- GDAL `CreateCopy` receives `pathText(...)` so PNG output works under a non-ASCII output directory.
- Includes added where the helpers are used; `rigid_scene.cpp`, `ground_thermal.cpp` and `supplemental_scenery.cpp`
  include only `path_text.h` (no mesh-scene dependency), which keeps `histream_collision_tests` linking.

Verification: 8-frame run with input and output both under `D:\...\_pathtest\中文目录\子目录\` and
`...\输出_中文名\` completes: `status complete`, 8 PNGs, band 10 µm. Both delivered cases (`01_建筑打击`,
`02_中红外`) run in place with the product engine (`models\bin_x64\Release\histream.exe`). Formula fixture 157/157.
Old/new identity re-run for authored, observe and rigid after the change (see below).

### Cases under D:

- `D:/cases/StreamStudio/场景动画案例/01_建筑打击/` - impact-v1 sample: input, videos, receipt, key frames
  (complete frame set stays in the supplemental run).
- `D:/cases/StreamStudio/场景动画案例/02_中红外/` - same event at 3.8 µm with the generic camera chain (640x360 IR,
  display 280-800 K), full 288-frame result kept, visible frames removed to save space.
- `README.md` and `案例索引.json` in that folder; `D:/cases/StreamStudio/README_目录安排_20261101.md` updated.
- Both are the same physics: 126/178 blocks fail, 31.8 MJ fragments, 16.8 m fireball, 126 MJ heat, 674 kg dust.

### Airfield case reduced to its deck assets

`mwir_airfield_day_20261111` (120.5 GB, 12812 files) is no longer kept. The deck's 9 figures, 2 videos and the two
tables of slide 3 were extracted to `D:/cases/StreamStudio/airfield_day_ppt/` (23.5 MB) together with the .pptx and a
slide -> file map; the original directory was moved (same volume, not deleted) to
`D:/cases/StreamStudio/待删除/mwir_airfield_day_20261111_20261112/`.
`tools/scene-animation/CURRENT_FEATURES.md` now points at the deck's new location.
### Case videos

`tools/scene-animation/compose_case_videos.py` composes captioned MP4s from frame folders (single panel, or two
folders side by side/stacked), with frame number, physical time and slow-motion factor in the caption bar.
Produced 5 new videos from the two cases: street-camera IR and visible (case 01), detector-level `observed_ir`
(case 02), and three band comparisons (10 um vs 3.8 um slowed, visible vs 3.8 um slowed, and a real-time 120 fps
version at 2.4 s). All 14 case videos decode end to end with `ffmpeg -f null` (exit 0).
