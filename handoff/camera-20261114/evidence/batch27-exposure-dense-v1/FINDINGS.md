# Batch27 dense capture: complete

Single deployment,74 points,444 DMA frames,296 center ROI images. All313 originals
(9,995,931bytes) recovered and strict verification passed; camera restored.
Original session failed only during HTTP RAM download with EOFError.
Its failed session status/log are preserved. One explicit read-only frozen-RAM
resume checked the identical index/run, partial segments and file CRCs; no
measurement or deployment was repeated. recovery-summary.json records completion.

RGB response fit v2 failed blue0.573/0.534EV at coarse48. No threshold was relaxed.
Dark blue median64->80 changes17->14 at5fps and18->16 at15fps, while UV transitions
away from neutral. This supports investigating brightness-dependent color
processing, not a confirmed identification of Color Kill or another module.
The separately registered bounded Y model passes local holdouts and is frozen
for the next independent batch. See observation-summary.json and the plot.

Before: global1/60+, central1/75 with reportedEV9.0+, inheritedISO100/f2.8.
After: approximate1/65 explicitly global; no central after value exists.
Nominal global delta+0.1155EV is not precision-qualified. Historical readings
remain in their original modes; future protocol is central-only, fixed against
screen and same center gray area. No absolute calibration is claimed.
