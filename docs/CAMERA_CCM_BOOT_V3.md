# v3: persistent startup and byte-read evidence after an unexplained exit

Batch19 v2 was deployed once to192.168.0.14:17491. The user observed a flash and
application exit, without readable error or known timing. No device files were
recovered within the registered360s deadline. Cause and camera restoration are
UNKNOWN. Host deployment completion is not proof that sensor acquisition ran.
The v2 binary/source and failed host session remain sealed.

## Boundary and storage contract

The new `camera-ccm-normalization/v3` entry, session and report are registered in
`tools/exposure_sweep_contract.py` before implementation. It remains standalone
hardware diagnostic compute, explicit standalone_hardware_diagnostic override,
local_trace proof only, with no Vision LaneAdmission. v1/v2 semantics are retained.
Unknown/mixed sources fail closed. Latest --ccm-normalization selects explicit v3.

Only v3's exact scope argument activates a persistent local SD journal:
`/3ds/Film3DS/ccm-boot-v3.log`. It is append-only, never truncated or evicted,
limited to2MiB total,768 records/run,160bytes/record. Records are synchronously
written/flushed with no queue or retry. A failed/full journal stops further camera
work, preserves existing records, attempts configured restoration if reached,
and cannot silently continue as RAM-only. Startup/open/parse errors hold a screen
for START. The UI and RAM_EVIDENCE explicitly disclose the SD journal for v3;
older RAM-only sources retain sd_writes=0 and never activate this writer.

Camera images and original measurement files remain in the existing28MiB RAM
arena. Pixels remain614400bytes of linear memory; HTTP later uses the existing
1MiB SOC buffer. No new bulk RAM allocation, arbitrary memory access or firmware
patch is introduced. New journal storage uses a file handle and small stack records.

## Evidence

Markers cover MAIN_ENTER, graphics and request parsing, arena/log/image allocation,
Camera/preflight, APT lifecycle events, each arm, each logical byte request/result, camera close, run
completion and pre-freeze. Actual SDK heap/linear reservation sizes are recorded.
Fault callback attempts a raw SD record of PC/FAR before the existing fatal hold;
it never resumes execution. If execution fails before main, or SD cannot be opened,
no first marker is guaranteed. Thus absence is not proof of an allocation failure.

The current-run prefix through BEFORE_FREEZE is copied to startup_journal.txt,
capacity128KiB, before RAM freezes. Later HTTP_BEGIN/EXIT markers are best effort
SD-only and never mutate frozen evidence or cause a repeated acquisition. The SD
file is the persistent full journal, the recovered copy is its pre-freeze prefix.
No SD marker is written inside the timing poll window; byte markers surround
read-only snapshots while capture is stopped. No additional sensor write is added.

The v3 report requires the exact lifecycle prefix/end, one monotonically ordered
run, stage/record bounds, no fault/error or APT event before freeze, all three arms, six snapshots,132 paired
byte request/result boundaries matching reads.csv and all v2 sensor/reference/
restoration/CRC/SHA proof. Missing device originals or journal cannot pass.

New local-only runtime fields are registered in the Film3DS matrix: boot_journal,
boot_stage, boot_address, boot_result, boot_heap_bytes, boot_linear_bytes and
boot_run_ms. These never enter image metadata or portable packages. No Vision
runtime registry, matrix, verifier or portable field set was changed.

## Validation

```
CCM_BOOT_ONLY=1 ./build/probe-driver
CCM_BYTES_ONLY=1 ./build/probe-driver
CCM_NORMALIZATION_ONLY=1 ./build/probe-driver
HG_ISOLATION_ONLY=1 ./build/probe-driver
./build/probe-plan
python3 tests/camera_ccm_boot_report.py
python3 tests/camera_ccm_bytes_report.py
python3 tests/camera_ccm_report.py
python3 tests/camera_hg_report.py
python3 tests/exposure_sweep_session.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
```

C journal tests exercise source selection/inactivity, append preservation, sink
failure, full/cap/unknown-stage rejection and no retry.15 full v3 model/failure
scenarios retain all v2 checks.15 v3 report tests add missing/faulted journal,
broken sequence and forged byte-boundary rejection. Legacy diagnostic C suites,
ARM/3DSX validation and both diff checks also run. Host models do not prove the
live SD journal, live byte aliases or the cause of Batch19's exit.

Vision commands below were not run: paths are absent in Film3DS and no Vision
runtime code changed:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Remaining untested hardware paths include pre-main failures, SD service failure,
live byte mapping, actual233E value, bit6's gain effect and exact internal writer.
