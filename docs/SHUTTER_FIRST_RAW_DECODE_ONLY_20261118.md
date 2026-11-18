# Prepared shutter, first RAW, decode-only Roll

The previous deployed package removed post-capture rendering but retained the
five-receive transaction. This change addresses acquisition itself.

- `film_raw_capture_prepare` verifies identity, geometry, timing and native
  controls once during camera readiness, after the first completed preview.
  Manual configuration is fully verified there after its existing settling.
  `FilmCamera` owns the ready state and configuration epoch. Open/close, frame
  rate, exposure mode and sensor-control changes invalidate it before mutation.
  Failed preparation is not retried each preview or on the shutter. Zoom only
  changes the verified transport window and preserves fixed sensor preparation.
- `film_raw_capture_transaction_result` accepts that preparation and current
  epoch. The old status/composite forwarding APIs are removed. A missing/stale
  preparation or unknown source produces no task/frame/register work.
- Shutter keeps one native gate, the first RAW receive and one restored native
  confirmation: three receives, one RAW, zero discarded RAWs and no recapture.
  First-frame failure remains failure; no later scene substitutes for it.
- Each shot still locks actual AE/AWB, snapshots current exposure/fine/gains,
  verifies changed RAW controls, receives with completion/stop/cache/guard/low2
  proof and verifies the post-frame hold. Manual expected values are compared
  using the same snapshot reads, not a second full hardware verification.
  Static identity/geometry/line probes are absent from the hot transaction.
- Shutter input precedes the live-preview poll. Successful normal log flushes
  are removed from the pre-retention path; errors and explicit Debug retain
  diagnostics. Capture timestamp records retained receive start, not completion
  of restoration. Original DNG write and integrity verification are unchanged.
- Roll publication is one function with an optional already decoded scene.
  Decode is unpack, bad-pixel correction, demosaic and linear RGB cache. The
  thumbnail directly samples that result and applies display sRGB quantization.
  No darkroom grade/render, LUT, recipe exposure, grain, sharpening or full RGB
  resize is called. Native legacy thumbnails also use direct sampled decoding.

Runtime classification: camera preparation/capture compute, preview-readiness
trigger and derived-preview compute. `CONTRACT_MATRIX` was updated, including
`film-camera-raw-prepare/v1` (local trace, no LaneAdmission/override). The generated
source registry and readiness/epoch verification changed. Preparation, epoch
and expected-exposure state are registered internal fields. No portable fields,
queues, implicit preparation, retry or fallback frame were added.

Changed production files: `include/raw_capture.h`, `include/platform_camera.h`,
`include/roll.h`, generated `include/film_runtime_contract.h`,
`source/camera/raw_capture.c`, `source/camera/raw_shutter.c`,
`source/platform/raw_camera_3ds.c`, `source/platform/camera_3ds.c`,
`source/platform/manual_exposure_3ds.c`, `source/platform/camera_receive_3ds.c`,
`source/platform/main_3ds.c`, `source/storage/roll_decode.c`,
`source/storage/roll_preview.c` and `tools/camera_runtime_contract.py`.

Targeted validation:

```sh
make -j4 build/raw-app-test build/standard-media-test build/roll-preview-test CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
./build/raw-app-test
./build/standard-media-test
./build/roll-preview-test
python3 tools/camera_runtime_contract.py --check
python3 tests/camera_ui_package.py UiPackageTests.test_prepared_shutter_and_decode_only_roll UiPackageTests.test_raw_only_shutter_and_existing_recovery UiPackageTests.test_contract_and_portable_recipe
git diff --check
```

All passed. Tests cover no fixed probes per prepared shot, stale/unprepared
rejection, first-RAW identity, no discarded RAW/retry, snapshot failure without
writing uninitialized exposure values, restoration errors, expected Manual
exposure mismatch, configuration invalidation, and thumbnail independence from
LUT/exposure/WB/grain/sharpen settings. The first-RAW transaction is not yet
hardware-validated; no elapsed-time or first-frame stability claim is made.

Package `build/first-raw-decode-20261118-r2/` was cross-compiled in the existing
pinned devkitARM container. ARM11/VFPv2, 3DSX, resource and frozen-manifest checks
passed. Only the unchanged pinned JPEG build cache was reused; application
sources were compiled afresh. Workspace EOF/trailing-whitespace cleanup after
freezing changes no compiled behavior. Build and any later NetLoader result
are recorded in this package's `build.json` and `deployment.json`.
