# Batch27 bounded luma transfer

analysis-final.json is the fixed model pinned before Batch28 deployment.
analysis.json is the earlier diagnostic output; the final gain check additionally
requires the full four-image shift range to pass, rather than anchor means only.
Curve knots, partitions, domains and holdout thresholds are unchanged.

Both profiles pass local Y interpolation and late baseline holdouts.
5fps: Y37.94..193.15, max odd0.01667EV, late0.00853EV, four-image gain span0.01912EV.
15fps: Y41.32..193.43, max odd0.01336EV, late0.00981EV, gain span0.01715EV.
All9 gain combinations have two valid anchors at both frame rates.

This is a fixed-screen central-ROI mean-Y transfer. It is not a full RGB response,
standard ISO, optically verified shutter-time model or independent scene-transfer
validation. The failed RGB model is sealed in batch27-response-fit-v2.
External meter values were not used to fit or correct residuals.
