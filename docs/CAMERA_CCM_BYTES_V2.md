# CCM v2: signed-word observation without accepting failed reads

Batch18 on192.168.0.14 completed one normalized control arm and stopped before
streaming the disabled arm when logical233E returned C9602FEF. The14 original
files859424bytes, deployed v1 binary/source and failed strict report are sealed.
All original MCU and physical settings were restored/read back. No v1 result is
relabelled complete. Raw233E was not supplied as a valid value in that result.

Archived Camera code shows108574 reads MCU through physical0990 via10A618,
which rejects FFFF. A signed coefficient=-1 is a plausible cause, not yet a
confirmed actual value. v2 does not reinterpret C9602FEF as FFFF, skip a required
coefficient, retry the failed call, patch service code, or silently fall back.

## Registered runtime boundary

`camera-ccm-normalization/v2`, its session and report are registered in
`tools/exposure_sweep_contract.py` before implementation. Standalone diagnostic
compute, explicit standalone_hardware_diagnostic override, no Vision LaneAdmission,
proof local_trace only. Unknown/mixed scopes fail closed. New persistent
runtime-internal fields: none. v1 is retained as a separately registered source.
The latest --ccm-normalization session now selects only the explicit v2 scope.

## Read transport and proof

The controlled bit6 OFF/ON/OFF, HG OFF, unit AWB, fixed AE gain/exposure/core and
four96/128/160/128 perturbations are unchanged. Only signed CCM observation
transport changes for v2. All11 current CCM words2332..2346 are read via the
logical8-bit access flag (word|8000) and its next byte. Each byte must have a
successful service result and lie0..255. A derived word=(MSB<<8)|LSB is recorded
as CCM_WORD in the manifest; it is never forged as a successful native-word RPC.
For rawFFFF, both successful bytes must independently beFF. Any byte error stops
and restores, with no fallback or retry.

At each of six snapshots a successful native2332 read is also required. Its
value must equal the first reconstructed word, with an explicit CCM_ALIAS_CHECK.
This tests the proposed byte alias and ordering on hardware before relying on
the derived coefficients. Live alias behavior remains unproven until acquisition.
The bus still uses16-bit register transactions; only MCU variable access width
changes. Width flag reference: [BlocksDS/libnds camera.h](https://raw.githubusercontent.com/blocksds/libnds/master/include/nds/camera.h).

Each snapshot contains94 original native reads,22 logical-byte reads and one
native reference=117 calls; it reconstructs the same105 logical/pipeline values.
Six snapshots yield132 byte reads,66 derived words and six native checks.
Snapshots and pairs are sequential, not atomic. No MCU variable/table/firmware
writes are added. Existing configuration writes and capture budgets stay fixed.

The shared read-only verifier accepts exactly registered HG/v1/v2 sources.
Only v2 admits derived words, and only with exact address, width, result, byte,
value and native-reference proof. v1 still requires original native reads and
fails on the saved sentinel error. Historical device records remain unchanged.

## Tests and commands

```
CCM_BYTES_ONLY=1 ./build/probe-driver
CCM_NORMALIZATION_ONLY=1 ./build/probe-driver
HG_ISOLATION_ONLY=1 ./build/probe-driver
./build/probe-plan
python3 tests/camera_ccm_bytes_report.py
python3 tests/camera_ccm_report.py
python3 tests/camera_hg_report.py
python3 tests/exposure_sweep_session.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
```

15 v2 C scenarios includeFFFF with successful FF/FF, byte failure, ignored-bit
failure, byte width300, native reference failure, wrong alias value, wrong scope,
DMA/VSYNC/control/restoration failures and independent/suppressed/held/reloaded
models. v1 adds a real-style sentinel rejection scenario, retaining its failure.
13 v2 report tests add tampered native reference, mismatched derived word,
failed/wide/misaddressed byte rejection to prior control/timing/source tests.
Historical C diagnostics, ARM/3DSX, generated-header and both diff checks run too.

These Vision test paths are absent in Film3DS and were not run; no Vision code,
CONTRACT_MATRIX, verifier or centralized internal-field registry was edited:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Remaining risks: actual byte alias mapping, actual233E value, bit6 gain effect,
writer identity and exact arithmetic remain hardware questions. Passing unit
models or a built binary is not hardware proof or calibration success.
