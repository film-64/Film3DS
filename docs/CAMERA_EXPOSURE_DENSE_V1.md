# Dense independent exposure capture and response fit

The first fit of Batch26 is rejected and sealed separately:5fps held-out green
error0.52EV/blue-0.44EV;15fps has only two effective training levels. Threshold
remains0.25EV. No old failed result, raw image or partition is overwritten.

Register camera-exposure-dense/v1, its session/report and
camera-exposure-response-fit/v2 before code. Standalone diagnostic compute,
standalone_hardware_diagnostic override, local_trace, no LaneAdmission. Offline
report/fit are local read/analysis boundaries and do not enter a runtime model
scheduler. Unknown source/scope rejects; old reconstruction/fitter unchanged.

5fps coarse32,48,64,80,96,112,128,160,192,224,256,320,384,448,528,768,1024,1280,1585;
15fps the first15 points through528. Per profile reference128 before/after,
18 gain points(two data-selected anchors × core528/544/1040 × ISP96/128/160).
74 points,444 DMA frames,296 center ROI originals,76 stages. Two fixed buffers,
one pending receive, same strict states/snapshots/journal/restore as v2. No FD,
color-vector or long-hold intervention in this independent response run.

Anchors: eligible meanY16..224; nearest80, then nearest120 at least0.5EV longer
using coarse+fine/line. Tie lowest index. If no pair, indices5/9 explicitly
fallback with coverage=false. This selection affects gain probing, never the
fixed alternating response fit partition. No retries, correction writes or
skipped failed stages. One additional registered local field rebuild_dense_mode;
no additional bulk memory. All runtime state remains local, no portable fields.

New capture: old/new parser and producer tests;12 new C success/failure models,
16 retained reconstruction models.10 dense report tests,15 retained reconstruction
report tests,18 one-deployment transport tests. Fixed even training/odd holdout
for the new19/15 grids;5 new fitter tests plus6 old fitter regressions. Minimum
three usable training levels/channel, two independent usable holdouts,80% common
pixel coverage, all RGB errors<=0.25EV unchanged. Training-only demonstrated
plateau masks both neighboring training channels conservatively. Holdouts never
select training/domain parameters. Failure produces no valid gain/ISO model.

Gain output is only a baseline-equivalent RGB exposure shift after the base
response passes, checked across two distinct anchors to0.25EV. It is not a
certified sensor analog multiplier, standard ISO or optical shutter time.
External approximate meter values remain separate and never correct residuals.

Commands:

```
EXPOSURE_DENSE_ONLY=1 ./build/probe-driver
EXPOSURE_REBUILD_ONLY=1 ./build/probe-driver
./build/probe-plan
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=5a1e62d-exposure-dense-v1
python3 tools/verify_3dsx.py FilmExposureSweep.3dsx
python3 tests/exposure_dense_report.py
python3 tests/exposure_rebuild_report.py
python3 tests/exposure_dense_fit.py
python3 tests/exposure_response_fit.py
python3 tests/exposure_sweep_session.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
git diff --check
git -C build/worktrees/camera-deep-probe diff --check
```

All pass. Runtime files: registry/session; typed dense branch in probe
exposure_rebuild.h and scope/dispatch/budget/journal; shared registered report
implementation with explicit source selection and dense-only wrapper; fixed-rule
fitter with separate dense-only wrapper; relevant producer/parser/session/report/
fitter tests. Capture original v2, its source and strict validation remain sealed.

Vision-product files are absent/untouched. Not run for that reason:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Remaining: real dense holdout results, optical timing, uncertainty/independent
external reference, illumination/scene transfer and full pipeline control.
