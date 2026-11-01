# Source snapshot

This repository contains the Stream Studio 2.0.1 source snapshot updated on 2026-11-01, with the current built-in examples, assets, manuals, and both model engines.

- The previous StreamSim repository and its history are preserved separately.
- `models/nvpro_core` includes the locally modified dependency source based on commit `2d4ba63d4c54cedf687b12786983c153e52555d5`. Its own LICENSE and PACKAGE-LICENSES are retained.
- Compiled application/engine outputs, build caches, node_modules, local simulation results, and external projects in D:\examples are not included.
- The bundled Mesa lavapipe driver is retained as a runtime dependency; its source/version/license information is in models/histream/thirdparty/lavapipe/README.md.
- Install dependencies and build the GUI and native engines using the instructions in README.md before running from a fresh clone. This publication did not run builds, tests, or simulations.

## Latest source update (2026-10-31)

- Includes the current native STREAM assimilation implementation and its workspace/configuration updates.
- Bundles five curated examples, including MODIS and Landsat energy-balance inputs, reference products, convergence masks, and provenance. The experimental maize-geometry photovoltaic example has been removed.
- Keeps the nine external training examples and large research datasets outside the source repository.
- Filters redundant runtime build artifacts, excludes the old audit executable from packaging, and records the shared runtime-deployment script in future build manifests.
- This update synchronizes the current source checkout. Static checks, a GUI build, and asset staging were completed during the preceding audit; this publication runs no simulation or test and does not include a newly built installer.

## Release 2.0.1 (2026-11-01)

- Synchronizes the current Windows 2.0.1 source, manuals and stability-solver metadata with the existing 2026-10-31 installer. The solver changes relative to the preceding public snapshot are descriptive metadata and comments; physical calculations are unchanged.
- Adds the 2.0.1 release notes and current download/case entry points.
- The installer, user/theory manuals, test report, build manifest and external training-case archive are distributed through the GitHub release. Compiled outputs remain outside the source tree.
- The existing release report covers seven runtime checks and three 4-by-4-pixel regions. Full clean-machine installation/upgrade/uninstall, all 3-D modes, assimilation/inversion and independent field validation remain outside that evidence. This publication did not execute new tests, builds or simulations.
- Build-manifest fingerprints refer to the Windows build working tree, including its original line endings. Git may normalize text line endings in source archives. The original build manifest is preserved with the release.
