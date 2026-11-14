# Runtime boundary and validation record

Standalone camera hardware diagnostic compute; explicit
standalone_hardware_diagnostic override; proof in local_trace; no LaneAdmission.
CONTRACT_MATRIX in tools/exposure_sweep_contract.py registers source/session/
report v1–v4 before implementations. Unknown sources reject. No retries or
failed-node skips. Reports are local read-only boundaries, not scheduler work.

Verifier behavior changed in tools/camera_gain_matrix_report.py: exact eight
nodes, registered adaptive cycles, all controls/timing/DMA/ROI/byte/journal/
restore evidence, CRC/SHA receipts. Successful subject-gain overwrites are data;
failed reads, bad fixed controls or static alias proof still reject. Exact call
order and counts handle equal-millisecond final Stop boundaries.

Seven new local internal fields across this matrix series: matrix_node,
matrix_branch, matrix_period_raw, matrix_dgain_baseline, matrix_event_stream,
matrix_plan_stream, ccm_snapshot_coherence. No portable project metadata fields
or Vision-product files changed.

Runtime files changed: tools/exposure_sweep_contract.py (registration),
tools/exposure_sweep_session.py (one registered deployment/recovery),
probes/exposure/gain_matrix.h (bounded enumeration), calibration.h/scope.h and
main.c (explicit dispatch/budget), sweep_contract.h (generated registry),
boot_journal.h (source-specific bounded journal selection), ram_log.h (explicit
matrix evidence capacities), hg_isolation.h (matrix-only stable alias and live
CCM proof), isp_update.h (registered observational initial subject gains).
These producer files are under build/worktrees/camera-deep-probe; all executed
sources and host verifiers are frozen in this batch.

Tests added/updated: actual C probe-driver matrix normal/fault models, probe-plan
source rejection, tests/camera_gain_matrix_report.py proof/timing/transport/
branch/image/journal corruption, tests/exposure_sweep_session.py one-deployment
and exact-source tests. Existing strict behavior retained.

Commands run in the existing network-disabled Linux build container:

```
make build/probe-driver build/probe-plan CC=gcc LDFLAGS=
GAIN_MATRIX_ONLY=1 ./build/probe-driver
CCM_BOOT_ONLY=1 ./build/probe-driver
ISP_UPDATE_ONLY=1 ./build/probe-driver
./build/probe-plan
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=5a1e62d-gain-matrix-v4
python3 tools/verify_3dsx.py FilmExposureSweep.3dsx
```

PASS:13 matrix producer models; retained journal and ISP-update cases; scope
parser checks; ARM378128 bytes SHA256
bc508614d1c347ad2619a1e59b54804ef52c0fd9880c4a706051c0b98caa7af3.

Host commands:

```
python3 tests/camera_gain_matrix_report.py
python3 tests/camera_gain_matrix_report.py GainMatrixTests.test_millisecond_equal_stop_boundary
python3 tests/exposure_sweep_session.py
python3 tests/camera_ccm_boot_report.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
git diff --check
git -C build/worktrees/camera-deep-probe diff --check
python3 tools/camera_gain_matrix_report.py build/hardware/exposure-sweep/batch24-gain-matrix-v4/recovered --out build/hardware/exposure-sweep/batch24-gain-matrix-v4/matrix-report.json
```

PASS:15 new report tests plus boundary recheck,16 session tests,15 legacy report
tests, generated header, both diff checks and real-device strict report.
A test-file resource warning was fixed and its affected boundary test rerun.
No production runtime was changed after the verified executable was frozen.

These commands were NOT run because their files do not exist in Film3DS and
no Vision-product runtime files changed:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Remaining risks/untested paths are listed in FINDINGS.md: long holds, FD toggles
within one stream, target-specific images and internal writer/formula.
