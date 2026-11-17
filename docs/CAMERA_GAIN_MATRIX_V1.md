# Bounded adaptive camera gain matrix

Source `camera-gain-matrix/v1` runs one deployment: eight mandatory nodes in
registered order (5/15 fps × FD retained/all four SEQ drivers disabled × coarse
4/528), each with ISP targets 128, 96, 160. AE/AWB/HG are disabled after AWB
normalization; bit5 Force Unity is set and bit6 normalization-disable cleared.
This is standalone hardware diagnostic compute with explicit
`standalone_hardware_diagnostic` override and local trace proof. It does not use
Vision-product LaneAdmission or project portable metadata.

The contract is registered in `tools/exposure_sweep_contract.py` before producer
and verifier logic. New local internal fields are matrix_node, matrix_branch,
matrix_period_raw, matrix_dgain_baseline, matrix_event_stream and
matrix_plan_stream. Unknown sources remain rejected. There are no retries or
failed-node skips.

Every node has one StartCapture, one initial DMA and three trailing DMAs.
Each target is written once. No SetReceiving or sequencer write occurs during
the subsequent VSYNC observations. Target128 is observed for eight cycles;
96/160 each receive eight cycles if128 held throughout observations and its
trailing image, otherwise two cycles. A two-cycle hold is not reported as an
eight-cycle control success. Each image is a center YUYV ROI with surrounding
control-state proof. The register observations are sequential, not atomic.

Three fresh VSYNC samples establish each profile's raw service period and host
span. Subsequent steps must be within20% of that period, including consecutive
samples; a missed cycle stops the experiment. No raw service time-unit
conversion is assumed. Fixed controls include coarse integration, core gains,
frame lines, AWB gains and per-node AE digital-gain/line-length baselines.
Byte-derived CCM words retain the native2332 cross-check. Mode variables are
read only. Configuration/transport failures stop and attempt full restoration.

Maximum216 VSYNC observations,32 node DMA frames and24 ROIs. Existing seed and
reference stages are retained. Existing RAM arena and image allocations remain;
no arbitrary MCU RAM/ROM edits or firmware patching. Source-specific SD startup
journal is bounded to1024 records and2MiB cumulative file size; expected frozen
prefix768 records. Readiness deadline900 seconds permits recovery of this one
run, never automatic remeasurement or redeployment.

Validation uses the actual C producer in nine models: always overwritten,
FD-dependent hold, all held, VSYNC timeout, profile mismatch, exposure drift,
byte-read failure, pending DMA and restoration failure. The Python report tests
check real producer fixtures and tampered source identity, control
proofs, adaptive choices, event timing, handles, byte transport, image/journal
completeness and original-file integrity. Existing journal/report/session and
camera diagnostic regressions are retained.

Vision-product commands below are not run because this is the Film3DS tree and
these files are absent; no Vision-product files or verifier are changed:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Passing this diagnostic means evidence is complete, not that calibration or
arbitrary gain control succeeded. Even an observed eight-cycle hold does not
identify the MCU writer, prove a shadow register, or establish whole-bus absence
of host writes. A previous unexplained application exit remains unattributed.
