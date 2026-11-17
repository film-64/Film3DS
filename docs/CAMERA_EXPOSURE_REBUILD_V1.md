# Exposure reconstruction evidence v1

Registered source camera-exposure-rebuild/v1; standalone hardware diagnostic
compute, explicit standalone_hardware_diagnostic override, local_trace proof,
no LaneAdmission. Source/session/report entries in tools/exposure_sweep_contract.py
precede implementation. Old matrix source and its results remain unchanged.

Two blocks,5/15fps;74 points plus existing seed/reference stages76 total:
reference start;10/8 coarse integration points;18 gain combinations;4 independent
ISP color vectors;128-DMA hold;FD off/on/off16-DMA points;reference end.
748 new DMA captures,296 original128x96 YUYV ROIs; first two and last two images
per point, statistics and16 six-row bands every DMA. Images at a point boundary
can straddle requested settings due to an explicit prefetched frame; first
images are latency evidence, not asserted settled exposures.

Each frame has full20-register states before/after waiting, actual raw VSYNC,
timestamps, completed buffer slot, event and CRC. Settings are written once per
point. All AE/AWB/HG bits off; FD only enabled in its declared middle comparison.
Manual points require planned exposure/core/IFP values. FD-on intentionally
observes coarse within1..1585 at5fps or1..528 at15fps and IFP within1..512;
no time estimate based on requested coarse is reported for this auto-control
comparison. Other controls remain strict. No retries, skipped failed points,
per-frame repair or error-result-as-value fallback.

An eligible coarse point has last-two-image meanY16..224; choose nearest128,
tie by lowest index. Secondary index=max(0,primary-2). If none eligible, index5
is explicitly used with coverage=false; dark/saturated evidence is retained,
never labeled calibrated. Gain points are two anchors × packed core codes
528/544/1040 × ISP96/128/160. Codes are not asserted photometric multipliers.

One extra614400-byte linear buffer and24576-byte static CRC scratch; existing
28MiB evidence arena. Exactly one pending Receive and two fixed buffer slots.
Arm the other slot immediately after completed DMA release; read only completed
slot. Pending work across a point boundary is explicit, not hidden scheduling.
On failure stop capture then close pending handle, restore controls and free
extra buffer. Native tests assert completed slot is never reused as pending.

Four complete pipeline/Mode/PLL/sensor timing snapshots, static width640 alias
proof, live CCM reference windows with explicit non-atomic correspondence.
Startup prefix204 records within1024-record/2MiB journal bounds. Declared local
files: rebuild_points64KiB, rebuild_frames512KiB, rebuild_bands256KiB; maximum313
files. Snapshot reads never occur inside continuous capture.

New runtime internal fields: rebuild_pending_receive,rebuild_buffer_slot,
rebuild_frame_ordinal,rebuild_plan,rebuild_anchor,rebuild_band_profile. All local
only. Verifier checks exact point/anchor/frame plan, buffer/event ordering,
allowed calls, control proofs, raw pixel statistics/bands, snapshots/journal,
restoration and every original CRC/SHA. It does not certify optical time, ISO,
absolute scene EV, steady illumination or a fitted response model.

External screen reference may be supplied before/after capture and is preserved
with original instrument settings and reported EV separately. Fixed gray screen
is a relative target, not a certified spectral/color reference. Capture need
not reject real flicker. Metadata must not silently substitute reported EV for
EV computed from ISO/f-number/time. External reference anchoring and blind
validation remain distinct from successful evidence collection.

Validation commands, in the existing network-disabled Linux container:

```
make build/probe-driver build/probe-plan CC=gcc LDFLAGS=
EXPOSURE_REBUILD_ONLY=1 ./build/probe-driver
GAIN_MATRIX_ONLY=1 ./build/probe-driver
CCM_BOOT_ONLY=1 ./build/probe-driver
./build/probe-plan
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=5a1e62d-exposure-rebuild-v1
python3 tools/verify_3dsx.py FilmExposureSweep.3dsx
```

Host:

```
python3 tests/exposure_rebuild_report.py
python3 tests/exposure_rebuild_report.py RebuildTests.test_no_FD_effect_and_pipeline_delay_are_data
python3 tests/exposure_sweep_session.py
python3 tests/camera_gain_matrix_report.py
python3 tests/camera_ccm_boot_report.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
git diff --check
git -C build/worktrees/camera-deep-probe diff --check
```

PASS:14 new C models,13 report tests plus affected output recheck,17 session
tests,15 legacy matrix and15 strict CCM report tests, retained C regressions,
parser/header/diff checks, ARM verification. Dark/clip, no FD effect, one-frame
pipeline lag and intentional FD-on coarse change are real observations;
RPC/width/control/pending-buffer/restoration/allocation failures cannot pass.

Vision-product files are absent and untouched. These commands are not run:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Runtime changed files: registry, session, new exposure_rebuild.h and report;
explicit dispatch/scope/write budget/journal/stream capacity in main.c,
calibration.h/calibration_scope.h/boot_journal.h/ram_log.h; matrix-only shared
snapshot/initialization helpers in hg_isolation.h/isp_update.h; generated
sweep_contract.h; producer/parser/report/session tests. Other dirty work is
preserved. Hardware data will determine coverage and reproducibility; passing
software tests does not declare calibration valid.
