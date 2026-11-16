# Scene-animation review fixes - 2026-11-11

Scope: the newly added `tools/scene-animation` impact/airfield modules, their
native receipts and the open-airfield day case. This is the implementation log
for the review in the current session; it records what changed and what remains.

## Fixed

1. **Resume no longer launders a failed convergence check.**
   `run_coupled_town.py` and `run_runway_day.py` now write `accepted` and
   `acceptanceIssues`, set `status=failed` when the rules are not met, and
   `--resume-completed` re-checks all nodes converged plus surface residual
   `< 2 W/m2` instead of trusting a stale `completed` status. A receipt written
   by the old code is still accepted only if its numeric fields pass the same rule.

2. **Portable case inputs.**
   `native_inputs()` now stores case-relative keys (with a legacy absolute-key
   canonicalizer on resume). `prepare_runway_day.py` hashes the copies inside the
   case (`scene.json`, `actors.json`, `assets/`) instead of the build-time absolute
   paths. `source_inputs.json` from new cases is relative; old absolute receipts
   still work on the same machine.

3. **Case seals and render input checks.**
   New `case_seal.py` writes SHA-256 manifests with relative paths. A case seal
   covers inputs, configuration, native receipts and GPU geometry; large
   `.facet_steps`/process/diagnostics and render media are excluded. `run_runway_day.py`
   writes `case_seal.json` at the end and verifies an existing seal before resume.
   `render_runway_day.py` verifies the seal (or `--allow-unsealed` for legacy cases),
   re-checks every native project's `configSha256`, receipt acceptance and index
   completeness, and writes `render_seal.json` over its products. Render products
   are excluded from the case seal so re-rendering does not invalidate it.

4. **Render provenance and multi-band record width.**
   `render_receipt.json` now records `bandPoints`, `irWidth`, `irHeight`,
   `nativeRunsSha256`, `nativeEngineSha256`, `caseSeal`, `renderSeal` and a
   `codeSha256` map. The native energy-record width is read from
   `sensor.bands` (`11 + 2 * bands`) instead of the hard-coded 13 in both
   `render_runway_day.py` and `render_coupled_town.py`.

5. **Report integrity.**
   `build_runway_day_deck.py` now derives the minimum vehicle gap, deep-ground
   time constant, band count, IR size and solar window from case products. The
   periodicity headline uses the maximum over every group including individual
   props (0.77 K and 19% on this case, not 0.34 K / 15% over category means only).
   The 2 W/m2 residual margin (0.0003 W/m2, boundary pass) is printed. `crop_sky`
   derives panel geometry from the image instead of assuming 1280x720. The deck
   was rebuilt and replaced the stale case copy.

6. **Robustness/performance.**
   `AsyncWriter` now bounds queued pixel bytes (512 MiB budget) rather than task
   count, so 4K and `outputThreads=16` cannot queue gigabytes. The rigid track
   cache temporary name includes the process id and timestamp; concurrent writers
   cannot interleave. `sequence.rigidRequireSolidStatic: true` turns the
   all-or-nothing triangle-skin fallback into a hard error. The rig record still
   reports `staticCollider` and now `requireSolidStatic`.

7. **Registry/docs.**
   `capabilities.json` now lists `rigid-track-cache` and `airfield-day`, marks
   `impact-v1` as not implemented, and updates the date. `CURRENT_FEATURES.md`
   and the scene-animation README document the missing GPU geometry pass, the
   seal/acceptance workflow, the deck command, the byte budget and the new
   option.

## Verification

- `test_case_seal.py`: 8/8.
- `test_energy_layout.py`: 5/5.
- `test_coupled_receipts.py`: 4/4 (the relative-key change also keeps the standalone-project
  fingerprint test working).
- `test_runway_day.py`: 10/10 (includes relative `source_inputs`, acceptance rules
  and legacy absolute-key canonicalization).
- `test_simulation_entry.py`: 21/21.
- `histream` animation target rebuilt successfully after the C++ changes.
- `test_rigid_sequence.py` reached and passed the cache record/replay, corrupted-track
  healing, appearance-only hit, `outputThreads=0` byte-identical and profiling checks.
  Its final seek sub-run was aborted after exceeding 15 minutes in the repeated-context
  test process; a fresh one-frame seek with sensor on and off completes in ~95 s, which
  confirms the cost is 23 fully rendered warm-up frames, not a deadlock. The new
  `rigidRequireSolidStatic` option was tested directly (invalid string rejected; true
  accepted by the canonical scene).
- Existing airfield case: `case_seal.json` (2725 files, 0.558 GB) verifies before and
  after re-render; `render_seal.json` (75 files, 45 MB) verifies; four 624-node runs
  pass the current acceptance rule; videos complete-decode; deck rebuilt to 20 slides.
- Re-render used the libx264-capable ffmpeg at
  `C:\Users\jiank\AppData\Local\kzip_sogou\ffmpeg.exe`; the conda ffmpeg lacks
  libx264. A failed video encode now raises a `RuntimeError` carrying the ffmpeg
  stderr instead of a bare `OSError`; an encoder pre-check is still desirable.

## Remaining (not changed here)

- `impact-v1` (charge/penetration/blast load/failure/energy budget) is still a
  design document (P2/P3), not code. The module remains an authored demonstration.
- The module is still not exposed in the Stream Studio GUI, user manual, built-in
  example catalog or release; the airfield chain is outside `run_case.py`.
- S3 instancing, S4 multi-camera, S6 checkpoints and S7 local substeps remain open;
  the per-sample adapter rebuild and per-frame transport statistics are still the
  main measured costs.
- Partial-frame renders still render every warm-up frame (the one-frame seek took
  ~90 s at 192x108). A sensor/transport checkpoint would be the real fix.
- `run_runway_day.py`/`render_runway_day.py` still use `assert` for several
  user-facing checks; `python -O` would disable them.
- Native `native_runs.json` keeps `actualInputHashes` for the current run; the
  portable seal is the case-level artifact, not a replacement for resuming a
  moved case with an old engine.
