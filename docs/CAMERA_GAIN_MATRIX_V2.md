# Gain matrix v2: initial node image and uninterrupted VSYNC trials

Batch21 camera-gain-matrix/v1 preserved all16 originals and restored controls,
but the first trailing DMA timed out after eight no-receive VSYNC cycles.
The registered v1 contract and sealed binary/report/source remain unchanged in
its hardware batch archive. The timeout mechanism is not established.

The next source camera-gain-matrix/v2 retains the eight mandatory nodes and24
conditions described in CAMERA_GAIN_MATRIX_V1.md, with these explicit changes:

* One initial DMA image per node, before period calibration and target writes.
  State is recorded after that DMA. The image is a node/context observation;
  it does not claim target-specific, last-cycle or photometric calibration proof.
* No further SetReceiving in that node, including after observations. The three
  targets are rewritten and observed in the same uninterrupted sensor stream.
  One StartCapture/StopCapture per node and eight node DMAs total.
* Target128 has eight consecutive VSYNC observations. Later targets have eight
  if immediate readback and all128 cycles held, otherwise two. Report fields
  say eight_cycle_hold and never claim a trailing-image hold.
* Eight node ROIs,216 maximum VSYNC observations; all other fixed-control,
  profile, byte-derived CCM, Mode snapshot, journal, restoration and failure
  contracts remain. A real DMA/configuration/transport failure still stops the
  whole experiment; no retry or failed-node skipping was added.

Runtime classification: standalone diagnostic compute, explicit override
standalone_hardware_diagnostic, local trace proof, no LaneAdmission. The matrix
registers source/session/report v2 before code. Verifier now demands one initial
DMA image per node and forbids additional receives. No additional runtime
internal fields beyond v1's six registered local fields. No Vision-product
files changed. Its three pytest paths are absent, so those commands were not
run; exact commands are listed in the v1 document.

Validation: actual producer nine normal/fault scenarios; report tests include
all-held/FD-dependent/always-overwritten branches and tampered source, adaptive
plan, fixed control, timing, extra work/image, handle, byte, missing image and
journal. Single-deployment transport tests and existing journal tests retained.
ARM executable is verified with tools/verify_3dsx.py. No result is promoted to
calibration validity or writer identification.
