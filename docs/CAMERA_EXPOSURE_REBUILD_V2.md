# Exposure reconstruction v2: explicit FD transition experiment

Batch25 v1 sealed161 originals,5782808 bytes.36 points and144 target images
completed at5fps, including the adaptive matrix, color vectors,128-DMA hold,
FD-off and FD-on. After disabling FD, first DMA state read IFP133 instead of128,
so strict v1 stopped and restored. No crash, acquisition retry or RAM overflow.
This establishes a transition boundary, not permanent failure of disabled FD.

Source camera-exposure-rebuild/v2, session/report v2 are registered first.
The v1 source/contract/artifact remains unchanged. Two explicit changes:

* Move each reference_end before FD ABA, bracketing the main image matrix
  without the intentional FD intervention.
* All three FD transition points observe successful IFP1..512. Before frame8
  of FD_off_after, always perform exactly one planned four-channel128 write,
  independent of preceding values. Manifest timestamps and four calls prove
  this intervention. Report derives hold from all eight subsequent readbacks.
  There is no failed-operation retry or corrective loop. Any RPC error stops.

All manual photometric points retain strict controls, two-buffer ownership,
exact plan, adaptive anchor, ROI/band/journal/restore proofs. Source counts remain
74 points,748 DMAs,296 target ROIs,76 stages and at most313 files. No new
internal fields or bulk allocations beyond v1. FD-on coarse may change within
its declared bounds and has no exposure-time estimate from requested coarse.

Runtime classification: standalone diagnostic compute, explicit override
standalone_hardware_diagnostic, proof local_trace, no LaneAdmission. Matrix and
verifier change. New tests cover one final FD update and persistent FD-associated
updates as distinct observations, missing/retimed/misplaced intervention proof.
Sixteen actual C models plus full report and one-deployment transport tests;
header/parser/ARM/diff checks required. Exact commands match the v1 document,
with REVISION=5a1e62d-exposure-rebuild-v2 and --exposure-rebuild selecting v2.

No Vision-product file changes; its three named pytest files are absent and
those commands remain unrun, as listed in CAMERA_EXPOSURE_REBUILD_V1.md.
Remaining limits: seconds are timing estimates, external meter is a relative
reference, display spectrum/phase unknown, no absolute ISO, optical timing,
blind calibration validation or firmware-writer identification claimed.
