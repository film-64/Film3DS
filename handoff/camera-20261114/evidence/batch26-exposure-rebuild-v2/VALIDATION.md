# Runtime boundaries and validation

Classification: standalone hardware diagnostic compute; explicit override
standalone_hardware_diagnostic; proof local_trace; no LaneAdmission. Frozen RAM
recovery and offline report are explicitly registered read-only boundaries.
CONTRACT_MATRIX changed first for reconstruction v1/v2, session and report.
Verifier changed for exact74 points/adaptive anchors,20-register before/after
proof, two-buffer ownership and one pending Receive, planned frame8 intervention,
ROI pixel/band statistics, snapshots/journal/restoration and CRC/SHA provenance.
Manual controls remain strict. All FD transition IFP values are subject data;
coarse is bounded observation only in the deliberately enabled FD-on point.

Six additional local runtime-internal fields: rebuild_pending_receive,
rebuild_buffer_slot,rebuild_frame_ordinal,rebuild_plan,rebuild_anchor,
rebuild_band_profile. No additional field in v2. No portable metadata fields
or Vision-product files changed. No hidden retries, repair loops, failed-node
skips, arbitrary MCU memory edits or firmware patches.

Changed runtime files: tools/exposure_sweep_contract.py; tools/exposure_sweep_session.py;
tools/exposure_rebuild_report.py; probe exposure_rebuild.h and explicit dispatch/
scope/budget/journal/capacity in calibration.h,calibration_scope.h,main.c,
boot_journal.h,ram_log.h; typed matrix-only helpers in hg_isolation.h/isp_update.h;
generated sweep_contract.h. Probe files are under build/worktrees/camera-deep-probe.
Unrelated dirty work preserved. All executed files are archived in source/ and
validation/frozen. No production source was modified after payload freeze.

Added/updated tests: native real producer/parser in tests/probe_driver.c and
probe_plan.c; host tests/exposure_rebuild_report.py and exposure_sweep_session.py.
Positive and corrupted/missing/misplaced proof, unknown source, adaptive plan,
DMA ownership, timeout, control/byte/static-alias failure, allocation/restore,
dark/clip, pipeline delay, FD tail and persistent update models are covered.

Commands in the existing network-disabled Linux build container:

```
make build/probe-driver build/probe-plan CC=gcc LDFLAGS=
EXPOSURE_REBUILD_ONLY=1 ./build/probe-driver
./build/probe-plan
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=5a1e62d-exposure-rebuild-v2
python3 tools/verify_3dsx.py FilmExposureSweep.3dsx
```

PASS16 native scenarios, parser, verified391404-byte ARM payload SHA256
45a10282ed49b17ca78ff8eaada6d8ce823a28950c1f051283d073ffeb7269c8.
Retained GAIN_MATRIX_ONLY and CCM_BOOT_ONLY C regressions passed during v1 setup;
legacy report suites15 matrix and15 CCM tests passed. v2 changes are scoped to
new reconstruction behavior and its source argument.

Host commands:

```
python3 tests/exposure_rebuild_report.py
python3 tests/exposure_rebuild_report.py RebuildTests.test_FD_tail_and_persistent_update_are_distinct_data RebuildTests.test_missing_or_retimed_reassert_rejected
python3 tests/exposure_sweep_session.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
git diff --check
git -C build/worktrees/camera-deep-probe diff --check
python3 tools/exposure_rebuild_report.py build/hardware/exposure-sweep/batch26-exposure-rebuild-v2/recovered --out build/hardware/exposure-sweep/batch26-exposure-rebuild-v2/rebuild-report.json
```

PASS15 report tests,2 affected-proof rechecks,17 session tests,header/diff checks,
real-device report. The one real v2 deployment recovered313 originals/11154284
bytes. Previous incomplete v1 remains sealed in Batch25 with its failed report.

These commands were NOT run: files absent from Film3DS and no Vision runtime
boundary changed:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Remaining risks/untested paths: optical timing, exact gain/ISO/radiance mapping,
longer holds, minimum FD handoff delay, firmware writer, independent calibration
holdout, display spectral/temporal behavior and reference-meter uncertainty.
