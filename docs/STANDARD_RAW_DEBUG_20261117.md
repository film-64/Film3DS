# Standard CUBE / DNG and Manual capture diagnostics

This product includes DNG technology under license by Adobe.

## User-visible behavior

- Place ordinary `.cube` files in `sdmc:/3ds/Film3DS/luts/`. No `.f3p` is required or loaded by the 3DS look selector. All regular files with a case-insensitive `.cube` extension are cataloged and sorted, including long names and more than 64 entries. Standard 3D sizes from 2 through 256 are accepted; malformed files, unsupported 1D/combined-shaper formats and memory allocation failures produce errors. A CUBE file does not specify its transfer encoding; the default is sRGB, without private per-file recipes.
- Bundled CUBE files are installed into that visible SD directory at startup only when their names are absent. Existing user files are preserved. Bundled grain data remains a separate rendering resource, not a prerequisite for loading a user CUBE.
- All new RAW frame writes use standard little-endian TIFF/DNG 1.2: one uncompressed 16-bit RGGB strip containing every original 10-bit sample. The camera high8/low2 representation is reversibly converted to uint16. Files are named `rolls/ROLLnnnn.DNG`. No F3S payload, private recipe tags or private preset files are required to read these RAW originals.
- Existing F3S originals remain readable. The original resolver selects DNG first, then an existing legacy F3S. Roll discovery recognizes both, counts a duplicated ID once, and deletion removes that ID's originals and derived preview/index while preserving exports. Legacy F3S remains read-compatible. Production shutter and command capture now reject closed cameras, unsupported sensors, simulated/native originals and non-DNG output paths; they have no YUYV fallback. Sensor transition and Manual seed frames are temporary hardware setup inputs, not saved photographs. Roll previews derive from the same committed RAW.
- The DNG includes standard `RawImageDigest` MD5. Capture readback additionally compares the dimensions, format and CRC32 fingerprint of reconstructed RAW samples against the acquired independent buffer. Writes retain the existing temporary-file/rename transaction. The full sensor area is saved; darkroom crop/LUT/grain settings never alter the RAW original.
- The color matrix is a generic sRGB approximation, explicitly not device calibration. Unknown exposure time/ISO are omitted. The in-app DNG reader intentionally accepts only the bounded uncompressed single-strip RGGB subset produced here; it is not a universal importer for arbitrary compressed/multiframe DNGs.

## Debug and error operation

Input **A, A, B, B, Up, Up, Down, Down** to toggle debug; repeat to turn it off. The top screen displays `debug` at the bottom right. The recognizer observes ordinary key-down events; it does not queue, delay or replay user commands. Existing actions still receive the keys when the camera is not busy.

Logs append to `sdmc:/3ds/Film3DS/log/film3ds.log`. Startup, toggle state, debug key events, Manual register requests/results, strict camera bool IPC, RAW preflight, stop/clear/busy proof, RAW stage transitions, receive steps and save/verification/preview results are recorded. Diagnostic requests are logged and flushed before potentially blocking register/IPC boundaries (before request construction, never between preparing and sending the shared TLS command buffer) so an interrupted operation can be identified. The same FILE remains alive across debug toggles, avoiding stale pointers during capture. Failed SD/log I/O or abrupt power loss cannot guarantee a complete durable log.

The actual RAW acquisition runs on one explicit worker, followed by storage on that worker. During acquisition the UI continues drawing the current stage and accepts the debug gesture; camera-mutating commands are held off, not queued. Once an ordinary API error returns, the worker ends, the UI joins it and surfaces the error. Valid independent RAW can still be committed after restoration failure; unproven DMA cannot be published. Thread creation failure reports an error without a synchronous acquisition fallback. Shutdown joins the owner before closing the camera or log. No automatic recapture or retry was added.

This does not guarantee that a camera sysmodule/kernel IPC call will return: the SDK's synchronous call has no user-space cancellation contract. Keeping the camera owner on the worker prevents the UI draw loop from waiting on the whole normal transaction; hardware testing is still needed. Save failure still ends the existing transaction and releases its memory; an explicit manual retry/recovery UI has not been added.

## Manual findings and fixes

The user identified the symptom as a freeze/camera error after A/R, before Saving, rather than an ARM11 exception. Source inspection alone does not establish a single physical root cause.

1. Actual libctru disassembly shows AE/AWB/busy/finished bool getters normalize the entire 32-bit reply word. The existing RAW decoder verifies the reply and narrows to the low byte first. All production camera flag reads now share that strict decoder, including Native/Manual setup and UI readback. High-bit garbage therefore cannot turn an encoded false into true. Evidence: `build/manual-libctru-disassembly.txt`; malformed/high-word cases exercise the real decoder in `tests/hardware_transport.c`.
2. Manual setup verifies service AE is false after locking. Manual shutter verification checks AE is still false before the existing register hold verifier. A Manual owner missing its hardware state fails closed rather than reaching Native exposure compensation. Native compensation remains disabled by the UI in Manual. Manual still uses the existing seven coarse/gain steps; this is not a new current-scene AE-lock control or a calibrated shutter-time UI.
3. Preview stop previously freed buffers immediately after StopCapture/ClearBuffer, whereas RAW used an explicit busy check. The common stop transaction now performs one Stop, one Clear and at most 20 strict busy reads separated by 10 ms. Preview DMA is released only after all stop proof succeeds. Unproven buffers remain quarantined until process exit; register restoration is skipped while stop safety is unproven. A completed, validated standalone native receive also permits release of its one-shot allocation. No register safety verification was deleted.
4. Synchronous shutter work formerly occupied the UI thread through multiple receives, checks and recovery. The explicit CAPTURING/SAVING/DONE owner makes those stages visible and lets normal error returns reach an operable UI.

These fix specific code risks and make the fault observable. They are not a claim that Manual capture has now passed on the physical 3DS.

## Runtime governance

Classification: camera compute ownership, camera stop/verification side effects, RAW storage/export and original reads, LUT catalog/install side effects, debug input/local logging. There is no RegionTask, TaskScheduler or LaneAdmission in Film3DS.

`tools/camera_runtime_contract.py` was updated before implementation. New registered entries are `film-dng-write/v1`, `film-dng-read/v1`, `film-lut-install/v1`, `film-lut-catalog/v1`, `film-debug-toggle/v1`, `film-camera-shutter-worker/v1` and `film-camera-stop/v1`. Existing save, original, package, capture and exposure entries express the changed rules. These require no LaneAdmission and allow no admission override. Source-taking APIs reject unknown/fake admitted sources. No hidden queue or automatic recapture was added.

Verifier changes: shared mask8 camera bool validation; Manual AE-off admission to capture; bounded stop proof before preview memory release; standard DNG digest validation; sample CRC readback comparison. Portable FilmRecipe/FilmFrame structs were not extended with runtime fields. The central internal-field set now includes camera/worker/debug state and callback/log fields (`debug_mode`, `debug_sequence`, `capture_phase`, `raw_capture`, `capture_zoom`, `expected_step`, `dma_safe`, `runtime_log`, stop callbacks); tests consume that set. New DNG output does not serialize private recipes or these fields.

Primary files: `source/platform/main_3ds.c`, `camera_3ds.c`, `manual_exposure_3ds.c`, `raw_camera_3ds.c`, `camera_bool_3ds.c`, `camera_stop_3ds.c`, `source/camera/camera_stop.c`, `source/storage/dng.c`, `lut_catalog.c`, `storage.c`, `roll_preview.c`, `raw_import.c`, `source/ui/debug_mode.c`, `roll.c`, their headers, `source/color/color.c`, `source/image/image.c`, package tooling and registry. Existing unrelated changes were preserved.

## Validation and artifacts

Updated/added tests include `tests/standard_media.c`, `tests/dng_interop.py`, `tests/camera_ui_package.py`, `tests/unit.c`, `tests/raw_app.c`, `tests/roll.c`, `tests/roll_preview.c` and the existing composition regression tests. Coverage includes standalone/32-point CUBE development, long and >64-file catalogs, no-overwrite install, debug ON/OFF/malformed input, unknown-source rejection, DNG full-sample roundtrip/digest corruption, stop/clear/read faults and bounded busy timeout with no release proof. Package checks cover the worker source, single owner, shutdown order, logging location and no portable internal-field leakage.

Commands:

```sh
python3 tools/camera_runtime_contract.py --check
make -j4 test test-raw-darkroom CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
PYTHONPATH=build/dng-validation-python python3 tests/dng_interop.py
LC_ALL=C exiftool -validate -warning -error -a build/standard-sensor.DNG
git diff --check
```

Host regression passed; one pre-existing export case skips because its private grain reference is absent. ExifTool validation reports OK. TIFF decoding and LibRaw/rawpy both recover all 307,200 exact sample values; independent hashlib MD5 matches the standard digest. Optional reader packages were installed only under `build/dng-validation-python`, not into system Python. Logs: `build/standard-workflow-final-validation.log`, plus native build logs.

The three requested Vision-product pytest commands were not run because `tests/test_runtime_contract_boundaries.py`, `tests/test_runtime_projection.py` and `tests/test_metadata_runtime_package.py` do not exist in Film3DS. Their applicable boundaries are checked by the tests above and the Film3DS matrix.

Final frozen package: `build/standard-raw-debug-20261117-r4/`, with eight CUBE files and existing grain resources. Build/deployment receipts in that directory are authoritative. Physical UI/debug gesture, Manual capture and SD fault behavior require device testing; deployment acceptance alone is not that evidence.

DNG specification reference: https://www.adobe.com/support/downloads/dng/dng_sdk.html

Final ARM build passed: `57b662cb8175407a6f0cca8f66a90123498e08ec1a34f551733bd7d343843ef6`, 9919496 bytes. Manifest SHA256: `1de9775fded9fcce407291083839908ad86f9bcdb4ad3f04df6b6132f5bc2a89`. The verified pinned scalar JPEG library was reused; all App sources were rebuilt, and the final linked ARM11/3DSX/ROMFS checks passed. A final package-boundary run passed all 13 tests, including the TLS IPC logging-order regression.

Deployment to `192.168.0.69:17491` completed: NetLoader accepted the entire payload and the launch request was sent (`NetLoader_payload_accepted_launch_requested`). Receipt: `build/standard-raw-debug-20261117-r4/deployment.json`. No automatic capture or physical Manual/UI verification was performed.


## Manual activation regression: source A/B and attached device log

The latest user symptom is **Manual cannot activate**, earlier than the previously reported shutter failure. The attached log was copied unchanged to `build/manual-failure-evidence-20261117/film3ds.log` (SHA256 `2a488314ca480690bae13ad3151565f33ce83714b7727c77d882c9bcc93e301e`). Lines 8118–8162 show camera initialization succeeds, seed 0 succeeds, seed 1 returns strict `finished=0` with successful IPC, the receive path immediately stops the camera, and Manual setup fails before AE lock/register initialization. This is not a storage failure.

Source A/B uses the preserved `composition-frame-fix-20261117-r2` and deployed `standard-raw-debug-20261117-r4` snapshots. Exact diffs are retained in the evidence directory. Manual setup order is unchanged. Native receive changed from the libctru full-word bool getter to strict low-byte decoding and common safe stop; both snapshots still have a single finished query after the event and immediate failure on false. This establishes the relevant changed boundary and the observed early-abort branch; it does not prove whether the old getter or changed timing explains every hardware difference. There has been no physical A/B replay of these binaries in this investigation.

`film-camera-receive-complete/v1` now explicitly registers one armed receive, one event wait and bounded observation of its finished flag within a total 1500 ms. A transient false does not rearm or recapture; success still requires the event and an actual true flag. Service errors and deadline expiry return failure to the existing safe stop. The platform adapter is shared by Manual seed/native diagnostics and RAW receives, rather than a special Manual bypass. Unknown source strings fail closed.

Regression tests execute first-seed-immediate success followed by second-seed-pending-then-complete. The test demonstrates that the deployed single-read branch rejects the latter transition while the new verifier accepts it only after completion. It also checks permanently false completion, wait failure, read failure and unknown source. Camera readiness remains unproven until this binary is tested on the physical console.

Boundary classification: receive verification/local diagnostic side effects; no new compute entry or LaneAdmission override. `CONTRACT_MATRIX` gained the receive completion contract before implementation. Verifier behavior changed from one observation to bounded completion proof. `wait`, `finished`, and `now_ms` callbacks are registered runtime-internal fields; none enter portable metadata. Tests changed in `tests/standard_media.c`; raw-only shutter/storage and package boundary tests cover the previously requested removal of native original capture. The three Vision-product pytest modules are absent here and were not run; Film3DS equivalent contract checks and host tests are used.

Candidate package: `build/raw-only-manual-receive-20261117-r1/`. Its build and deployment receipts, when present, describe completion; they do not certify Manual activation on hardware. This work is local; no GitHub push has been performed.

Candidate validation completed: host `make -j4 test test-raw-darkroom` passed (`build/manual-startup-ab-validation.log`); `python3 tests/camera_ui_package.py` passed 13 tests; matrix/header check and diff whitespace check passed. Frozen App source/include/tools hashes match the working tree. A concurrent host-only Makefile addition of `tests/display_chart_calibrate.py` was preserved and is outside the frozen package; no application source is stale. ARM11/3DSX/embedded resource audits passed; binary SHA256 `b8e5694e805aae5aac15795259fc4253f2b1782c7bf842d2d6ea42a6fab9ec11`, manifest `e91f95681d5f5375c179054e383c3aa63302c29a5fbfe96f280ca54a8ec52d51`. One authorized deployment to `192.168.0.69:17491` timed out during connect, with no transfer started. No automatic retry was made and the console has not received this candidate.
