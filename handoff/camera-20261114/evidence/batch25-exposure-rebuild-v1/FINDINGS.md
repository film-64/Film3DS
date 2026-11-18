# Batch25: 5fps matrix and first FD transition; strict run incomplete

161 original files5782808 bytes, all CRC/SHA verified.36 complete points
(indices0..35),144 target ROI images. Camera controls restored1, RAM failed0,
END complete0 aborted0 io_error0. The v1 strict report correctly fails.

Completed5fps reference,10 coarse points,18 adaptive gain points,4 independent
channel vectors,128-DMA hold,16-DMA FD-off-before and16-DMA FD-on. Point36
FD-off-after configured128 successfully, then first DMA after-state read32D4=133
rejected the expected128 control. A prefetched next DMA was canceled after
StopCapture. No acquisition retry or correction write occurred.15fps block and
final5fps reference remain unmeasured. This is a new transition-boundary result,
not proof of a crash or a permanently active FD after disabling its bit.

The screen reference was ISO100,f/2.8,1/60 and separately reportedEV9. Numeric
settings implyEV100~8.877744, not exactly9. User reports the global meter reading
unchanged after capture. Both original readings and manifest-bound reference
sidecar are retained; no claim of continuous DC illumination or instrument
accuracy. Source had one additional614400 linear buffer and24576 CRC scratch;
no MCU firmware or arbitrary memory patches.

Next source will treat all three FD transition points as gain observations,
keep manual image points strict, and always perform exactly one declared IFP128
intervention before frame8 of FD-off-after. No dependence on failure, retry or
repair loop. Images/statistics before and after this intervention will establish
whether a completed FD update can be followed by a held manual value.
