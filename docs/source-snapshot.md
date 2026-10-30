# Source snapshot

This repository contains the Stream Studio 2.0.0 source snapshot published on 2026-10-31, with the current built-in examples, assets, manuals, and both model engines.

- The previous StreamSim repository and its history are preserved separately.
- `models/nvpro_core` includes the locally modified dependency source based on commit `2d4ba63d4c54cedf687b12786983c153e52555d5`. Its own LICENSE and PACKAGE-LICENSES are retained.
- Compiled application/engine outputs, build caches, node_modules, local simulation results, and external projects in D:\examples are not included.
- The bundled Mesa lavapipe driver is retained as a runtime dependency; its source/version/license information is in models/histream/thirdparty/lavapipe/README.md.
- Install dependencies and build the GUI and native engines using the instructions in README.md before running from a fresh clone. This publication did not run builds, tests, or simulations.