# RAW / darkroom corrections, 2026-11-17

Worktree: `Film3DS-sync-20261117`, branch `codex/raw-darkroom-sync-20261117`, based on `54c3aa8`.

## Runtime boundary classification

This changes camera compute, darkroom compute, derived-preview compute and storage side effects. Film3DS has no RegionTask, TaskScheduler or LaneAdmission. Its authoritative matrix is `tools/camera_runtime_contract.py`; the Vision-product Python matrix/verifiers and three requested pytest files do not exist in this repository.

The matrix was updated before implementation. `film-roll-capture-save/v1` is a new registered boundary. Existing RAW, shutter, darkroom, catalog, publication, exposure and deletion rules were updated. All changed entries require no LaneAdmission and permit no admission override. Source-taking APIs continue to reject unknown sources. RAW proof remains local trace; independent memory ownership and validation qualify the output, never an admitted string or portable metadata.

## Changes and contracts

- `source/camera/raw_capture.c`, `include/raw_capture.h`: split `raw_received`, `raw_valid`, `camera_restored`, capture status and restoration status. A retained callback must have copied a stopped, cache-synchronized, guard-checked and low-bit-checked DMA frame to independent owned storage. Post-frame register verification must also succeed before `raw_valid` becomes true. Restoration errors remain errors.
- `source/platform/raw_camera_3ds.c`, `include/platform_camera.h`: retain only that validated independent RAW on restoration failure, close the camera and quarantine DMA when stopping remains unsafe. Diagnostic callers still receive the error. The production shutter can save the valid partial result. No register verification, DMA check or stop/clear operation was removed.
- `source/storage/roll_preview.c`, `include/roll.h`, `source/platform/main_3ds.c`: centralize original commit, one CRC readback and optional native preview publication in `film_roll_capture_save`. `written` records the commit separately from verification and preview results. A committed original enters Roll even when readback fails, with an explicit verification warning. No preview is published after failed readback. Missing native retention does not trigger RAW ISP in the shutter save path; original remains recoverable.
- `source/ui/darkroom.c`, `include/darkroom.h`, `source/platform/main_3ds.c`: WB movement/reset/touch changes pending values only; A commits and renders, X cancels without rendering. RAW and native SELECT both JPEG-encode the cached displayed output pixels. Crop, output size, sharpening and grain are baked into the A preview. SELECT neither reads the original nor reruns those stages. Further edits require another A preview. The full original-coordinate composition view and crop frame remain visible; geometry edits preserve that view and invalidate only the cached final output until the next A preview.
- `source/storage/roll_preview.c`: RAW imports and explicit/startup preview repair use the existing neutral RAW development path. Catalog preparation validates original CRC/binding, JPG CRC/decoding and index before reuse; missing or corrupt derived data is rebuilt once. Original read/CRC failures stop preparation without substituting data. Browsing remains JPG/index-only.
- `source/ui/roll.c`, `include/roll.h`, `source/platform/main_3ds.c`: delete the selected preview JPG/F3I before removing its F3S and catalog entry. Missing sidecars are allowed. Cleanup errors retain the original and catalog entry, though already removed derived files may need explicit repair. Independent exports are preserved.
- `source/platform/manual_exposure_3ds.c`, `source/platform/main_3ds.c`: logging is best effort for exposure initialization. Unsupported Manual is rejected before mutating/stopping the current owner; opening an unsupported lens visibly selects Native AE. Successful register traffic is verbose only with `FILM_CAMERA_VERBOSE=1`; failures and key phases remain logged.
- Removed three fixed-source checks in the shutter forwarding chain; the source-taking RAW transaction still validates its registered source. The remaining hardware abstraction and safety readbacks are preserved.

## Verifiers and internal fields

The generated `include/film_runtime_contract.h` now includes the registered save source. Source-check behavior at external/source-taking boundaries remains fail closed; no separate admission verifier exists here. The RAW verifier now reports capture/restoration results separately. Preview verification is stronger because an index alone is insufficient.

The central Film3DS `RUNTIME_INTERNAL_FIELDS` set now includes `raw_received`, `raw_valid`, `camera_restored`, `capture_status`, `restore_status`, `write_status`, `verification_status`, `written` and `composition_view`. Portable FilmRecipe/FilmFrame format did not change. The package test consumes this central set to check that none of these fields enters portable structures.

## Validation

Updated tests: `tests/raw_app.c`, `tests/darkroom.c`, `tests/white_balance_composition.c`, `tests/roll.c`, `tests/roll_preview.c`, `tests/camera_ui_package.py`.

Coverage includes successful RAW, failed retention, failed post-frame verification, failed restoration/idle, unknown-source rejection, missing native preview, original write failure, preview write failure with original retained, RAW repair, missing/corrupt JPG/index, original binding mismatch, cancellation, sidecar deletion failure preserving original/list, exports preserved, cached JPEG byte equality after destroying the old canvas, UI A-only WB dispatch and portable structure checks.

Commands:

```sh
python3 tools/camera_runtime_contract.py --check
make -j4 test test-raw-darkroom CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
python3 tests/raw_capture_qa.py
git diff --check
```

All passed. RAW export tests report one skip because the private grain reference is absent. The complete host run is recorded in `build/fix-validation-final.log`. Initial host compilation required the explicit installed JPEG include/library paths; the HTTP simulator requires permission to bind localhost, and passed with that permission.

`make 3ds` initially stopped because the native shell has no configured DEVKITPRO. A subsequent build used the existing workspace Lima VM and cached pinned devkitARM container, and succeeded; see the deployment section below. The following Vision-product commands were not run because their test files do not exist in Film3DS:

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

## Remaining validation and scope

- On-device camera/DMA fault scenarios, Manual lens transitions, optional logging and UI controls are not hardware-validated here; the production sources have now been 3DS-cross-compiled. Host callback tests establish the transaction state rules, not physical DMA behavior.
- Readback-failure catalog behavior is covered by the production dispatch check, not an injected physical SD readback failure. A CRC-failed original remains listed with a warning and cannot be trusted until successfully read/verified.
- Actual exposure microseconds/ISO remain unknown (zero); no uncalibrated values were invented.
- Native YUYV and RAW remain distinct sequential frames. Matching exposure settings does not make them the same instant. No unsafe hardware sequencing changes were made to claim otherwise.
- Register-check/idle reduction and broad diagnostic extraction from main_3ds.c remain outside this patch pending hardware evidence. No performance improvement from deleting safety checks is claimed.


## 3DS build and deployment, 2026-11-17

The existing Lima instance `film3ds` and cached image `docker.io/devkitpro/devkitarm@sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b` were reused. Only the Film3DS directory is mounted in that VM, so the frozen current-worktree package was copied into its build directory; no older application objects were reused. The package builder scripts in the two worktrees were byte-compared before use.

The frozen package includes all current source changes, 8 existing LUT/profile pairs and the existing pan400 grain texture. Compilation, 3DSX structure checks, ARM11 VFPv2/no-NEON/no-ARMv7-barrier audit, frozen-source manifest verification and every ROMFS resource embedding check passed. Build warnings remain in upstream JPEG and existing diagnostic helpers/string formatting; no build errors occurred.

- Current-worktree package: `build/raw-darkroom-fixes-20261117-r1/`.
- Binary: `Film3DS.3dsx`, 9,871,880 bytes.
- SHA256: `d94001ce6266487260b862d44b14999b9c0938772be5f33785149e94b41232d0`.
- Manifest SHA256: `5b5b791c95db90b00a3b99934f8b170f6bade56ac3d522089ec622cda5bf5e45`.
- Build proof: package `build.json`; log: `build/raw-darkroom-fixes-20261117-r1-build.log`.
- Authorized destination: user-supplied `192.168.0.69:17491`.

An initial connection incorrectly used the historical `.48` destination. It timed out before any transfer (`transfer_started=false`); that failure receipt is archived. The user-supplied `.69` destination is used for the actual transmission. The final result is recorded in the package `deployment.json`. A payload acknowledgement and launch request do not establish that the physical UI or camera behavior has passed testing.

This build/deploy step uses the existing registered package/deploy side-effect contracts. It makes no further runtime, matrix, verifier or internal-field changes, and adds no tests. Contract header synchronization and `git diff --check` were rerun; the earlier full host regression results still apply to these frozen sources.

Deployment completed: `192.168.0.69:17491` accepted the complete payload and the launch request was sent (`NetLoader_payload_accepted_launch_requested`). The application SHA256 matches the verified build above. Physical UI/camera behavior remains unobserved.


## Composition-frame regression correction

The first patch incorrectly introduced `darkroom_preview_displayed` to hide the composition frame after A. This was an unsolicited behavior change and has been removed. The frame is again drawn in original F3S coordinates over the full-frame image, including after A, after saving, and while changing crop percentage or panning.

The darkroom contract now explicitly owns two outputs from one graded canvas on A: `composition_view` is the full original view used by the UI; `preview` is the final crop/output cache used by SELECT. Geometry edits retain the canvas and full view, invalidate only the final crop cache and do no rendering. SELECT cannot save that stale crop; A prepares the new crop. WB adjustments still render only on A. No new task sources, admission overrides, retries or camera actions were introduced.

Changed files for this correction: `tools/camera_runtime_contract.py` (matrix and central internal-field registry), `include/darkroom.h`, `source/ui/darkroom.c` (view ownership and cache invalidation), `source/platform/main_3ds.c` (full-view display, restored frame and pan handling), `tests/white_balance_composition.c`, `tests/camera_ui_package.py`, and this document. The generated header is unchanged. External source verification and portable formats are unchanged.

Added tests prove the full view retains both original horizontal edges while the output is cropped, geometry edits preserve the full-view/canvas pointers, stale crop saving fails, a subsequent A produces the changed crop, saved JPEG bytes equal the cached crop even without the old canvas, and the production frame dispatch is no longer gated by preview state. Portable structure checks use the updated central registry.

Validation: the same full `make -j4 test test-raw-darkroom` command with the documented JPEG paths passed (one existing private-reference skip); log `build/composition-frame-validation.log`. `python3 tools/camera_runtime_contract.py --check` and `git diff --check` passed. The three Vision-product pytest commands remain inapplicable because their files are absent in Film3DS. Extra full-view rendering occurs only on A; memory/latency and physical frame interaction require device validation.

Frozen replacement package: `build/composition-frame-fix-20261117-r2/`. It must not be confused with the previously deployed r1 package. Build results and any later deployment are recorded in this package's own receipts.

The r2 ARM build completed successfully: 3DSX, ARM11/no-NEON/no-ARMv7-barrier and all embedded-resource checks passed. Binary SHA256: `4c820442e6e19d8ff4f1b21da1c079c822b54679960a2e50d331a942f8cf8e15`. No r2 transmission has been attempted; receiver readiness is required after the previous app launch.
