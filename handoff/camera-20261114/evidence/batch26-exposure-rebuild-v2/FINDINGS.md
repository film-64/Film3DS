# Batch26: complete exposure/gain images and FD transition evidence

313 original files,11154284 bytes, device CRC/host SHA verified. Strict rebuild
report passed with no errors:74 points,748 matrix DMA captures,296 original
128x96 YUYV target images. Source camera-exposure-rebuild/v2. Original controls
restored1, no RAM capacity error, no abort or camera exception. END complete1,
io_error0,captures777,failures1; retained failure is from the existing preflight,
not a failed reconstruction point. No failed point was skipped or retried.

## Control results

Both5/15fps complete the exposure sweep, two-anchor core/ISP product,
independent channel vectors,128-DMA hold, paired references and FD off/on/off.
Coarse grids4/8/16/32/64/128/256/528 plus1024/1585 at5fps. Adaptive primary
index6(coarse256), secondary4(coarse64), selected from actual last-two-image
means. Packed core codes528/544/1040, ISP96/128/160. Packed codes remain
experimental settings, not certified analog/ISO multipliers.

All manual-point coarse/core/four ISP values held across before/after state
reads. Four independent channel vectors held, including asymmetric96/128/160.
Hold point33 at5fps:128 completed DMAs spanning25.394s from first to last wait;
IFP128 throughout, meanY134.7806..134.9242. Hold point69 at15fps:128 DMAs,
8.466s, IFP128 throughout, meanY135.5906..135.6867. This is bounded control
verification, not an arbitrary-duration guarantee.

## FD handoff

At both frame rates, FD-off-before preserves128. Enabling FD restores133
(15fps has an initial transition sample at128). After disabling FD again,
frames0..7 still read133. The unconditional, pre-registered four-channel128
write before frame8 succeeds; frames8..15 all remain128. No Refresh, context
switch or capture restart occurs between these three points.

This proves a usable delayed manual write after disabling FD in these runs.
It supports an outstanding/frame-boundary update explanation but does not
count internal writes or prove only one update. Minimum necessary delay was
not measured; eight frames is the tested intervention point, not a hardware
specification. The133 formula and actual writer function remain unidentified.

## Reference and response limits

Before: userISO100,f/2.8,1/60,reportedEV9. Nominal settings implyEV1008.877744.
After: user says approximately1/55, other parameters unchanged, error~5%.
Nominal1/55 impliesEV1008.752213; endpoint difference-0.125531EV and nominal
shutter-time difference+9.09%. Preserve the user's5% description separately;
these approximate readings do not establish an exact calibration tolerance.
No midpoint substitution, silently fitted anchor or ISO certification.

Per-profile ROI references are repeatable in code space:5fps meanY85.59257 to
85.56820;15fps87.33626 to87.17411. These are not EV measurements. Different
instrument field/uncertainty and nonlinear response prevent treating a small
Y-code span as proof that external illumination was perfectly constant.

Exposure curves and raw data cover the output range, including near-black
short exposures and a high-exposure plateau: at5fps coarse1024/1585 give
Y223.47/224.01. Saturation/domain limits cannot be determined solely by Y>=250.
The CSV includes converted encoded-RGB observations, not linear radiance.

Physical pipeline endpoints are unchanged within each profile. Across profiles,
3210 changes2488 to2472, line length842 to843, plus HG variables and several
CCM coefficients. Gamma/CCM/config fingerprints must bind future models;
do not merge5/15fps into one calibrated response without proving equivalence.
OS median DMA intervals200/67ms support timing estimates only. Raw service
VSYNC units are retained unconverted. True optical integration time remains
unverified; no physical shutter or ISO values are certified here.

## Artifacts

* rebuild-report.json: full strict evidence verification.
* recovered/:313 unchanged originals, receipts and raw images.
* parameter-observations.csv: requested controls, estimated seconds, raw Y,
  decoded encoded RGB and clipping observations.
* response-and-fd.png: response curves and actual FD register transitions.
* external-reference.json: original user readings bound to capture manifest SHA.
* validation/,source/,host-tools/: executed source/binary and test provenance.

Next data/model work: optical shutter validation; per-pipeline RGB response,
black/saturation/noise and empirical gain calibration; independent illumination
or batch holdout with quantified external-reference uncertainty. A fixed-screen
color patch is a relative target, not a certified spectral/color standard.
