# Batch24: complete FD/gain/exposure/frame-rate matrix

All eight nodes and24 trials completed in one v4 deployment. Twenty-four original
files,1746319 bytes, CRC/SHA verified. Strict report passed with no errors.
Original MCU and physical control values were verified restored. No RAM capacity
error, abort or camera exception recorded. Calibration validity remains false.

| fps | coarse3012 | A102 | ISP128 | ISP96 | ISP160 |
|---|---|---|---|---|---|
|5|4|2|133 ×8 cycles|133 ×2|133 ×2|
|5|528|2|133 ×8|133 ×2|133 ×2|
|5|4|0|128 ×8|96 ×8|160 ×8|
|5|528|0|128 ×8|96 ×8|160 ×8|
|15|4|2|133 ×8|133 ×2|133 ×2|
|15|528|2|133 ×8|133 ×2|133 ×2|
|15|4|0|128 ×8|96 ×8|160 ×8|
|15|528|0|128 ×8|96 ×8|160 ×8|

Each cell describes all four physical IFP channels. Initial readback and every
observed VSYNC state are retained. A1022 versus0 differs only bit1, the FD driver
control, after AE/AWB/HG were disabled in both conditions. Both frame rates and
both exposure settings reproduce the distinction. This establishes a useful
manual gain-hold condition in the sampled matrix; it does not identify which
internal function writes the registers or mathematically derive133.

AWB R/G/B128, AE221F128, A35542 (unit bit set, bit6 clear), core gains528,
coarse4/528, configured frame lines2114/704 and per-node line length were checked
throughout. There were168 VSYNC observations:24 period samples and144 trial
samples. Four FD-retained nodes have8/2/2 cycles; four all-driver-off nodes have
8/8/8. Raw timing remains unconverted; two-period OS-time spans validate both
frame-rate profiles. No new Receive, Refresh or StartCapture occurs during the
three subject trials within each node.

Eight initial node ROIs are included. They precede target rewrites and are not
images proving96/128/160 transfer functions. All16 snapshots passed independent
static width640 alias transport proof. One live2332 reference window was
420/420/412; it is explicitly non-atomic with match_before1/match_after0.
Snapshot CCM endpoints changed slightly only in FD-retained nodes0 and5;
all observed non-IFP endpoints in FD-off nodes were unchanged. Gamma and
normalization endpoints are observations, not a proved explanation of133.
No derivedFFFF CCM word was observed; earlier native233E failure remains
unexplained by any actual observedFFFF value.

The startup prefix has768 records through BEFORE_FREEZE. Matrix/node reports
complete, restored1; END complete1, aborted0, io_error0, captures37, failures1.
The retained failure counter belongs to the existing preflight path, not to a
failed matrix node. The strict verifier checks every matrix DMA/control/event
call independently. No hidden remeasurement was used.

Restored physical controls:3012=264; core555/616/561/555; ISP165/159/159/133.
Restored MCU A10210, A3552, previewAWB0; profile5 before original controls.

## Remaining evidence

* Long-duration arbitrary gain holding beyond eight observed cycles per value.
* FD off/on/off in a single unchanged stream to isolate the transition without
  another normalization or profile change.
* Target-specific DMA images and repeated photometric calibration, including
  exposure/CCM/Gamma controls; node-initial images are not this proof.
* Actual internal writer and133 formula. No MCU firmware patch, arbitrary RAM
  edit or whole-I2C-bus monitoring was performed.

The requested24-condition matrix has no missing condition. Batch21–23 partial
runs, their original failed reports and frozen code remain sealed separately.
