# Gain matrix v3: transport proof and live CCM observations

Batch22 v2 completed node0, then rejected a time-separated CCM reference mismatch
(native2332=431; subsequent bytes combine429). Every read succeeded. v2's sealed
files, binary and strict result remain unchanged. This observation establishes
neither transport failure nor atomic CCM coherence.

The v3 contract is registered before producer/verifier changes. It keeps v2's
eight nodes,24 target conditions, one initial node image and uninterrupted
no-receive trials. All control, profile, timing, RPC, restoration and storage
errors still stop the whole run, with no retry or skipped failed node.

Every snapshot independently checks a stable configured output width: native
MCU2703 before, logical bytesA703/A704, native2703 after must all yield640.
Wrong address, width, value or RPC error rejects the entire matrix. No writes to
these variables are introduced. The live CCM is observed separately: native2332
before, its two successful byte reads, native2332 after are stored with explicit
match_before/match_after flags. Neither outcome claims atomicity for the first
word or the remaining ten coefficients. Different successful values are a live
observation; they cannot be treated as a failed-read substitute or a coherent
CCM. All original data and timestamps remain available.

The report verifies static alias proof, reconstructs successful byte pairs and
recomputes temporal correspondence flags. It rejects forged match flags and
misplaced static proof. CCM observations never serve as admission/control proof
or invalidate/validate the separately checked fixed camera controls.

Runtime classification: standalone diagnostic compute; explicit override
standalone_hardware_diagnostic; local trace proof; no LaneAdmission. Source,
session and report v3 are registered in tools/exposure_sweep_contract.py.
ccm_snapshot_coherence is one additional registered local runtime field; no
portable metadata or Vision-product files change. The three Vision pytest
commands listed in CAMERA_GAIN_MATRIX_V1.md remain unrun because their files
are absent from Film3DS.

Validation exercises eleven actual C scenarios, including dynamic CCM mismatch
that remains an explicitly non-atomic observation and a static alias mismatch
that must stop the matrix. Report tests cover these scenarios and forged proof,
wrong controls, extra work, adaptive-plan/event integrity, handles, images,
byte failures and journal provenance. Existing strict CCM report and single-run
transport checks remain. The141 actual register reads per snapshot include
19 Mode observations and five additional reference/transport reads. Startup
journal remains768 prefreeze records within its1024 record cap.

Limits: images precede trial writes; eight-cycle holds are register observations,
not calibrated images. MCU writer, update formula and the earlier no-receive DMA
resumption timeout remain unidentified. Sequential reads are never an atomic
snapshot. A passing evidence report is not calibration validation.
