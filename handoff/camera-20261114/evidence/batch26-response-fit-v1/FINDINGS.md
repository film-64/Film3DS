# First relative response fit rejected

Input Batch26 passed strict hardware evidence verification. All original data
remain separately sealed. Fit source camera-exposure-response-fit/v1, fixed
training0/2/4/6/8/9 and holdout1/3/5/7. External meter values never enter fitting.
Six synthetic positive/negative tests passed; dependencies NumPy2.2.6,SciPy1.15.3.

5fps: effective R/G training indices4/6/8, B4/6/8/9. Training-only near-plateau
masks remove high exposure9 for R/G. Holdout coarse128: RGB errors
+0.2067,+0.2009,-0.2722EV. Coarse528:-0.0934,+0.5205,-0.4373EV.
Both fail the fixed0.25EV all-channel criterion. The short holdouts are outside
usable domain. Solver output is diagnostic, not an accepted prediction model.
15fps: only training indices4/6 have usable coverage for all channels; fewer
than required three effective exposures. No fallback curve or ISO anchor is made.

Sparse broad steps, near-black low points and a high-end plateau leave limited
information for curve identification. A denser independently captured sequence
is needed before drawing any calibrated gain/ISO conclusion. Old failure and
thresholds must not be overwritten or relaxed. No absolute calibration or
optical shutter time validated.
