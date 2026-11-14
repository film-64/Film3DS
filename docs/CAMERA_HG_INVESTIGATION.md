# Camera internal gain investigation: HG isolation v1

This standalone Film3DS hardware diagnostic follows batch16's Camera-outbound
IPC evidence. That trace saw four host writes of ISP128 and no outbound Camera
write of ISP133, but was not a physical bus capture or proof of a particular
sensor firmware function.

The new entry `camera-hg-isolation/v1` is registered in
`tools/exposure_sweep_contract.py` before implementation. It creates standalone
hardware diagnostic compute, does not enter Vision's scheduler or require
LaneAdmission, uses the explicit `standalone_hardware_diagnostic` override, and
keeps proof in local traces only. Unknown scopes fail closed. Historical sources
and their strict Unity checks retain their existing contracts.

## Experiment

One deployment performs HG ON, OFF, ON. Each arm independently normalizes AWB
using the existing Refresh sequence, disables AE/AWB, changes only SEQ_MODE bit3,
checks readback, and finishes the five physical exposure/core settings and four
IFP128 writes after the last sequencer write. No further Refresh occurs in that
arm's capture window. A120 remains read-only; its configuration and the live bit
are separate evidence, neither proves that a particular internal routine stopped.

Each arm reads 98 upstream/pipeline values before streaming and after stopping:
63 MCU values (HG, two 19-byte Gamma tables, current CCM, AWB position/buffers,
Preview HG) and 35 physical values (pipeline, CCM scale words, YUV/offset and
30 packed Gamma-knee words). Reads are sequential, not atomic. No table, CCM,
HG variable, MCU RAM, ROM, clock or PLL is written.

One initial DMA is completed and its event closed. The stream remains running
without another Receive. Four trials each clear/wait for a new VSYNC and require
a strictly advancing service timestamp, then wait 25/75/125/175 ms, write all
four IFP128 once, record immediate readback as observation, and poll red at most
64 times with 4 ms sleeps until its first non128 value. Full fixed-control and
IFP snapshots follow each trial. Polls never cause corrective writes or retries.
A readback already changed makes timing unresolved; it is not a Unity success.
The saved image is from the initial DMA, not from the later register trials.

Timing uses osGetTime read-start/read-end bounds in ms. The delay is relative
to the observed VSYNC wait return, not an assumed exact sensor edge. The sleep
API returns void; its zero call record means issued, while elapsed timestamps
provide the timing evidence. Raw VSYNC service units are not converted. No GDB
observer is attached, because syscall stops heavily perturbed batch16 timing.
These finite observations alone cannot establish a free-running timer, a frame
interrupt, or an exact internal write duration.

Cleanup stops the stream, closes handles, restores saved MCU controls with the
existing bounded Refresh, then verifies original physical controls and ISP.
Capture failure, fixed-control drift, stale VSYNC, malformed evidence or failed
restoration cannot pass the report. `passed=true` means diagnostic evidence is
complete, never that arbitrary gain control or calibration succeeded.

## Source evidence

- [MT9V113 manufacturer datasheet](https://datasheets.su/DS/OnSemi/MT9V113-D.pdf):
  MCU/IFP architecture, separate processing functions, firmware driver ID11 HG.
- [BlocksDS/libnds camera register definitions](https://raw.githubusercontent.com/blocksds/libnds/master/include/nds/camera.h):
  SEQ_MODE HG bit3, A120, HG variables, logical CCM, packed Gamma-knee addresses.
  The repeated AWB-gain macros at 2348 in this header are not used; existing
  verified A34E/A34F/A350 addresses remain authoritative for those observations.

## Contract and validation changes

- Matrix: new diagnostic, single-deployment session and read-only report entries;
  generated C constants and explicit bounded write/read plans.
- Verifier: new `tools/camera_hg_report.py`; existing report semantics unchanged.
- Persistent runtime/internal fields: none added. All new state is bounded local
  stack state, using the already registered saved controls and target fields.
  Vision's `CONTRACT_MATRIX`, verifiers and internal-field registry are unchanged.
- Tests: C producer models for HG dependence, independent overwriter and held
  gains; snapshot/gate/VSYNC/control/restore failures and wrong-scope rejection.
  Python verifier tests reject missing snapshots, fake gate/control proof,
  misplaced Receive/config work, wrong handles, stale timestamps, poll mismatch,
  unknown source and tampered originals. Session tests require exactly one deploy
  and reject mixed diagnostic scopes.
- Commands: `HG_ISOLATION_ONLY=1 ./build/probe-driver`, `./build/probe-plan`,
  historical C diagnostic suites, `python3 tests/camera_hg_report.py`,
  `python3 tests/exposure_sweep_session.py`, existing Python diagnostic suites,
  generated-header equality, ARM build/3DSX validation and both diff checks.

Vision's three pytest commands are not run: this checkout is Film3DS, those test
paths are absent, and no Vision runtime boundary was changed:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Hardware results belong to an immutable batch directory with the deployed binary,
C and host source hashes, original CRC/SHA receipts, report and findings. Source
changes after a hardware run require a new registered version, not reinterpretation
of historical evidence.
