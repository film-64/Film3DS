# Gain matrix v4: initial IFP readback is the subject, not a fixed control

Batch23 completed five nodes/fifteen trials. At5fps both coarse4 and528:
A1022 (FD retained) returned all three targets to133; A1020 held128,96,160 for
eight consecutive VSYNC cycles each. At15fps A1022/coarse4 again returned133.
The remaining three nodes were not measured. Full evidence and original strict
v3 report are sealed in batch23-gain-matrix-v3.

Node5 stopped before capture because setup wrote128 to all four IFP channels,
but the sequential initial readback was128/133/128/128. All IPCs succeeded.
This is the experiment's gain-overwrite subject, not fixed-control admission
proof. The v4 source contract explicitly registers both initial labels
isp_update_initial and matrix_initial: four planned128 writes must succeed;
four successful readbacks within1..512 are observations and need not equal128.
Invalid values or RPC errors still stop the whole matrix. Original-value
restoration still requires equality. No rewriting, retry, new Receive, failed
node skipping or fixed exposure/control relaxation is introduced.

Everything else remains the v3 contract: eight mandatory nodes/24 conditions,
one initial node image, adaptive8/2 consecutive VSYNC observations, stable width
alias proof, explicitly non-atomic live CCM windows, strict controls, profiles,
restoration and bounded local journal. Runtime classification is standalone
hardware diagnostic compute with standalone_hardware_diagnostic override,
local trace proof and no LaneAdmission. Source/session/report v4 are registered
before implementation and verifier changes. No new internal fields beyond v3.
No Vision-product file changes; its three runtime pytest paths are absent and
not run, as listed in CAMERA_GAIN_MATRIX_V1.md.

Thirteen actual C models include setup gains overwritten to valid133 (continue)
and overwritten to invalid0 (stop). Report tests verify both and reject forged
initial readbacks, configuration/source/adaptive/timing/handle/byte/image/journal
proof. Legacy CCM and ISP-update tests retain their strict behavior. ARM3DSX is
verified and generated contract header must match Python registry.

Eight-cycle holds are observed register control in these conditions, not proof
of arbitrary duration, image calibration, the MCU writer or update formula.
Images are node-initial observations before per-target rewrites. Live CCM values
remain sequential observations, including mismatches; they never become fixed
control or admission proof.

The report also permits a normal final StopCapture at the same millisecond as
the last read only with exact call counts, call order after the final timing
query and no timestamp preceding any observed state. A one-millisecond-early
stop and extra receive/work still reject. This accounts for device timestamp
resolution without permitting an additional operation in the observation window.
