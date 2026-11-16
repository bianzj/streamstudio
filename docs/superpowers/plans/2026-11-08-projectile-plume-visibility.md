# Projectile and plume visibility repair

User reports missing projectile body, implausible exhaust and invisible smoke.
Proceed without confirmation; preserve the delivered mesh_video_20261108 case.

Verified causes: the entire projectile is650K, the650K three-metre tube covers
the body rather than starting at its nozzle, the flight has no persistent tracer
source, and binning2/PSF blur plus highlight clipping hides the thin silhouette.
Impact smoke exists but its hot volume is often rendered as a clipped white blob.

Implement an opt-in `projectileAppearance: separated-v2`; legacy remains default.
Add distinct body/nose/nozzle/fin mesh materials, a nozzle-origin tapered hot jet,
pre-impact moving tracer emission through existing GPU transport, and cooler
particulate opacity using explicit appearance parameters. Mix component emission
in radiance, not averaged temperature. Keep source appearance distinct from any
claim of combustion/engine performance or calibrated particle thermodynamics.

Add optional sensor controls with unchanged defaults: detector binning/blur,
minimum AGC span and logarithmic highlight compression. Raw radiance stays
untouched. Supply a tracking-camera flight close-up, plus a new full12s video.

Tests first: flight tracer must be positive before impact, geometry diagnostics
must distinguish cool body/hot nozzle, and new settings preserve deterministic
seeking. Recheck existing sensor/static/procedural outputs with legacy settings.
Inspect main and close-up frames, encode and decode every delivered video, then
save source/input/shader/engine/hash and scope receipts. No commit/push/release.

Active source C:/work/streamstudio. New case:
D:/cases/StreamStudio/supplemental/impact_gpu_20261107/projectile_plume_20261108/.
