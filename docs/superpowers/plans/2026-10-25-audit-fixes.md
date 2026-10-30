# Stream Studio audit fixes

**Goal:** Repair the four issues identified in the current workspace audit.
**Scope:** Packaging, unified-project Save As, STREAM raster alignment, desktop persistence.
**Constraints:** Preserve existing uncommitted work. Do not create or run tests or simulations (AGENTS.md). Work in the authoritative checkout because the reviewed implementation is uncommitted there.

- [x] Include the shared unified-project module in the desktop package.
- [x] Carry the source unified document into Save As; rebase STREAM paths to copied assets and preserve external references.
- [x] Reject mismatched CRS, geotransform availability and pixel alignment before reading auxiliary raster bands. Permit matching grids without georeferencing for explicit-location input.
- [x] Persist renderer settings and drafts through a restricted Electron preload bridge to an atomic JSON file under userData; retain browser localStorage for web use and migrate available old keys.
- [x] Review changes and compile GUI/STREAM; perform syntax and diff checks only. Do not launch applications, engines or test cases.

Implementation choices: keep random loopback ports and isolated desktop servers; storage must not depend on HTTP origin. Preserve the source project on Save As and do not silently resample scientific inputs.

## Completion record

- Read-only independent review identified and resolved two Save As edge cases: inputs reused from the original output directory must remain source references; shared DEM paths and their selected band must survive the copy.
- GUI build completed (`npm run build:gui`). Vite retains its bundle-size warning.
- STREAM Release build completed (`cmake --build build/stream --config Release --parallel`). Updated executable: `models/bin_x64/Release/stream-image.exe`, SHA256 `4b1b4b98060df058d19aa989061b141a178dc9aef01a0d9753408e6371c037b7`.
- Changed JavaScript files passed `node --check`. Relevant tracked files passed `git diff --check`; repository-wide checking also reports an existing EOF blank line in untouched `material-specular.js`.
- Updated the runtime build manifest. No tests, simulations, application launches, installer builds, commits or publishing performed.
- Existing browser drafts can only migrate when readable from the current origin. New desktop drafts and preferences are stored under userData/renderer-settings.json independently of the port.
