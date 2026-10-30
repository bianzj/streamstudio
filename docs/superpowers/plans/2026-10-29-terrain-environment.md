# HiStream terrain environment implementation

User authorized direct implementation without confirmation. Work in the current canonical checkout, preserving its existing changes. Scope: VoxelEB (the mode with the integrated wind solver); reject enabled settings in FacetEB rather than silently ignoring them.

- [x] Add validated JSON/XML configuration and GUI controls. Old projects default off.
- [x] Connect T=Tref-lapse*(h-href), e=eref*exp(-(h-href)/Hv), capped at local saturation, to EB and fluid initial/inlet/buoyancy fields.
- [x] Connect theta=thetaDry+(thetaValley-thetaDry)*exp(-max(0,hground-hdrain)/L) to local soil evaporation and thermal properties. h is relative to the DEM minimum. hdrain is an explicitly prescribed common drainage elevation, not computed HAND. Clip theta by each soil's saturation.
- [x] Preserve legacy behavior with features disabled; use local humidity in leaf physiology and evaporation; fix terrain underground fluid mask when enabled.
- [x] Document parameters/limits; compile histream and histream_runtime_resources, syntax/build GUI, refresh build manifest. No tests, simulations or SCOPE runs per AGENTS.md.

Decisions: independent air/soil switches, referenceHeight=2 m above scene zero; lapse=0.0065 K/m (negative permits inversion); vapor scale=2000 m; soil dry=.08, valley=.35, decay length=30 m. These are illustrative uncalibrated defaults. Prescribed soil moisture is static, without hydrological water balance. Humidity is vapor pressure, not an imposed RH gradient. Fluid height correction is applied once to initial/inlet background; transported temperature is not corrected again.

Verification: native Release build and runtime resource deployment exited 0; GUI Vite build and JS syntax checks exited 0. Runtime shared shader sources and affected SPIR-V binaries match the build. Static review identified solid-cell scalar interpolation; fixed with a feature-gated solid exclusion and rebuilt. No tests/simulations executed per AGENTS.md. Receipt: review/terrain-environment-receipt-20261029.json. Existing checkout changes were preserved; no commits, resets or cleanup.
