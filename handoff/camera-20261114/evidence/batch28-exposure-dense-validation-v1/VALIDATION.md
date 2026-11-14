# Fixed-scene luminance transfer and independent validation

Batch27 RGB response v2 remains rejected. Blue is nonmonotonic around coarse64
to80 at both frame rates. The recorded U/V transition is consistent with a
luminance-dependent color process, but no individual ISP module is identified.
This is not repaired by changing the RGB threshold or removing its failed
holdout. The rejected model is sealed separately.

## Boundary and registered rules

Register `camera-exposure-luma-transfer/v1` and
`camera-exposure-luma-transfer-check/v1` in `tools/exposure_sweep_contract.py`
before implementation. Both are offline local read/analysis boundaries:
compute=false, LaneAdmission=false, override=null, proof=local_trace. They never
create a runtime scheduler task or write sensor state. No new runtime-internal
field, queue, retry, suppression or portable metadata is introduced. Existing
capture/verifier behavior and its fail-closed rules remain intact.

The transfer concerns mean raw encoded Y in the saved central96x128 ROI, not
general radiance, RGB color, sensor analog gain or standard ISO. Profiles remain
separate. Effective exposure units are coarse+fine/line; seconds remain an
estimate based on DMA cadence, not optical timing verification.

Training uses even coarse indices except both baseline gain-anchor conditions.
Only the last training image is used. Predeclared Y16..224,80% pixel coverage,
minimum3 training exposures and32-code span apply. Training-only demonstrated
high plateau masks both neighboring training exposures. Interpolate log2 exposure
against mean Y, with no extrapolation. All in-domain odd coarse conditions and
both late baseline gain anchors must pass on both saved late images to0.25EV;
at least2 odd conditions are required. Outside-domain points remain explicitly
unmapped, not accepted. Gain transfer is evaluated only after base validation,
and all4 images across2 distinct anchors must span at most0.25EV.

Batch27 yields a bounded local model at both frame rates. Approximate domains
are Y37.94..193.15 and41.32..193.43. Largest in-domain odd error is0.0167EV;
late baseline errors are below0.01EV. These are within-batch interpolation
residuals, not a general accuracy specification. The original RGB failure is
unchanged. A fixed-scene Y model can succeed even when a monotonic blue inverse
cannot describe the dark chroma transition.

## Independent next-batch check

Freeze and SHA-pin the model before the new capture. Reuse the verified latest
dense program:74 points/444 DMA/296 ROI originals in one deployment, no runtime
changes. Reject the training batch itself as validation. Require the registered
physical pipeline, CCM, gamma, contexts and timing controls to match; live gain
values in the pre-test snapshot are excluded from configuration comparison.

One predeclared reference_start condition at coarse128/core528/ISP128 defines a
single illumination offset using its last2 images. This is an explicit nuisance
offset, not a refit of the curve or external EV correction. Reference_end, all
in-domain coarse points and every gain point must pass against the fixed prior
curve/gain shifts to0.25EV. An out-of-domain mandatory gain/reference fails.
No model coefficients or acceptance rules are adapted to validation results.

## Reference protocol

Following the user's instruction, future external readings use **central
metering**, fixed camera against the screen, and the same central gray area.
Record ISO/aperture/shutter or EV, raw display annotations, approximate precision,
screen-brightness changes, and before/after readings. Do not interpret an
unquantified +/fraction as an exact EV increment. Do not substitute global
readings for central readings or relabel historical data. Batch27 has no central
after reading; its global after reading does not close that gap. Neither curve
fit nor validation uses the external meter to reduce residuals.

## Validation

Commands run successfully:

```
python3 tests/exposure_luma_transfer.py       #10 tests
python3 tests/exposure_luma_validate.py       #9 tests
python3 tests/exposure_dense_fit.py           #5 retained tests
python3 tests/exposure_response_fit.py        #6 retained tests
python3 tests/exposure_dense_report.py        #10 retained tests
python3 tests/exposure_rebuild_report.py      #15 retained tests
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
git diff --check
git -C build/worktrees/camera-deep-probe diff --check
```

Tests include unknown-source/failed-proof rejection, fixed-model hash checking,
training-batch self-validation rejection, fixed offset with no holdout fitting,
both-image checks, no mean cancellation, no extrapolation, missing/mismatched
pipeline fields and inconsistent gain rejection. Capture binary and every probe
source hash are unchanged from the already verified latest Dense/v1 build;
there is no ARM implementation change to rebuild in this step.

Vision-product runtime files are absent and untouched in this Film3DS workspace.
These commands were not run for that reason:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Remaining limitations: independent scene/illumination/spectrum transfer, absolute
ISO and optical timing, full dark/highlight response and the precise internal
color-processing/gain writer. A registered independent capture can reject this
local model without changing or concealing the original successful interpolation
or failed RGB results.

## Batch28 hardware result

The same latest binary/source hashes were deployed once. All74 points/444 DMA/
296 ROIs and313 originals(9,996,130bytes) were recovered and verified. A truncated
HTTP response interrupted the first download; the failed session/log are retained.
One explicit read-only resume verified the same frozen index/run and partial
segments before completing CRC/SHA checks. There was no remeasurement.

The user adjusted screen brightness and reported central ISO100/f2.8, shutter
approximately1/60, slightly short, qualitatively unchanged afterward. The raw
annotation has no inferred fractional EV. The frozen model was SHA-pinned before
deployment; no coefficients or gain shifts were refit.

15fps passes:11 in-domain coarse points/22 images have maximum0.02641EV residual;
18 gain points/36 images maximum0.02026EV; reference_end two images maximum
0.00034EV. The predeclared reference_start offset is+0.25988EV, not an independently
calibrated meter delta. No external meter correction was applied.

5fps is rejected by the configuration gate. Its before_stream CCM variables
match Batch27, but after_stop2332 changes432->430 and2334 changes65455->65456.
The2332 successful native-before/native-after/byte-combined windows agree at
each endpoint. There are no diagnostic writes to2332/2334. This is evidence of
time-separated MCU CCM-variable change, not an atomic whole-matrix snapshot or
proof of its writer, trigger or optical effect. Actual packed physical CCM
32C4..32D2 was not sampled; the current result cannot determine when/if those
coefficients reached the IFP. The original fixed-model rejection remains in
place rather than tolerating or hiding a small coefficient difference.

The next hardware control question is MCU CCM variables versus actual packed
CCM around first-frame settling and gain transitions. Existing global-driver and
normalization experiments are retained; do not infer an AWB_MODE bit's complete
meaning from a homolog chip or begin arbitrary coefficient writes.

These manual response matrices have FD disabled. FD-on gain coupling and a
validated policy for choosing manual anti-flicker exposure periods remain
separate from the successful15fps local transfer. Neither outcome establishes
complete camera control.
