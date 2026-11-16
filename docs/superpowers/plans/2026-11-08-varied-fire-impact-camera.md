# Varied fire, denser smoke and impact-triggered camera implementation plan

> Execute inline with TDD/executing-plans. User opts out of questions. Preserve existing feature checkout and all old cases; no commit/push/release. One fresh scoped review before final render.

**Goal:** distinct fire profiles, visibly denser advected smoke, zero camera angular jitter before computed contact, and spatially varying observation blur.

**Architecture:** `appearanceVersion="varied-v3"` selects a guarded new mesh variant. Encode optional source style/seed in unused FireSource.flame.w as an exactly representable integer, preserving48byte source/400byte block. Profile-specific flame geometry/temporal rhythm and smoke release; increased explicitly supplied tracer rates. Camera optional jitterOnImpact uses actual timeline contact; regional normalized linear Gaussian pass after base PSF and before pixel integration/electronics, with impact/plume centre projected from existing camera view. Old options retain old shader paths and zero extra blur.

**Scope:** reuse40tree/132000leaf geometry and existing rigid/background inputs.3sources independently seeded (window licking/window billowing/ground pool). Stronger normalized smoke source and extinction are illustrative parameters, not combustion chemistry. Angular motion/region blur are observation proxies, not measured shock mechanics/lens calibration. Keep12s/24fps/1280x720/4scene samples,IR384x216/14bit; new RGB detector640x360 with stronger PSF. Preserve clear reference frames separately.

- [x] RED native checks for missing varied-v3, named profiles/independent seeds, smoke increase, pre-contact jitter zero/post-contact nonzero, regional observation changes with canonical isolation, seeking and guards. GPU flat/edge regional-blur fixture.
- [x] Add style/seed parsing/metadata, profile-specific varied shader and transported smoke release; keep old source packet and branches.
- [x] Add camera trigger/decay and normalized regional blur pass with per-exposure projected centre; old camera behavior unchanged unless enabled.
- [x] Create separate varied_fire_smoke_20261108 case referencing previous tree geometry; inspect8s preview, scoped tests/legacy checks and independent review before full render.
- [x] Full native RGB/IR videos and old/new comparison, updated onboard still, complete decode/visual inspection, exact rigid/background and frozen-data/source verification. Document changed smoke transport and uncalibrated observation/appearance.

Verified native result: 288frames/24fps/12s; 498.624s wall time. Contact3.059619427s;74pre-contact frames with zero angular jitter,214post-contact frames. Final normalized smoke495.814 versus206.955 previously (2.396x). Exact original rigid timeline/CSV,12background files,111current source files,14protected repository files,102previous source files and1728previous native frames verified. Four videos completely decoded. Generic observation/fire parameters remain uncalibrated. Complete record: case final-verification.json.
