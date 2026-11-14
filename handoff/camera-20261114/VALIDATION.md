# Publication validation — 2026-11-14

This is an image-free handoff, not a new hardware measurement or an absolute
calibration release. The publication was assembled in a clean isolated worktree
at3f01442; the original dirty application/probe worktrees were not staged/reset.
All published current probe C/header hashes match the Batch28 deployment record.

## Boundary classification

`camera-hardware-handoff/v1` is a local evidence-package export/read boundary.
`camera-hardware-handoff-bootstrap/v1` is an explicit local path-setup side effect.
Both were registered in Film3DS `CONTRACT_MATRIX` before implementation:
compute=false, LaneAdmission=false, override=null, proof=local_trace. Unknown
sources, path traversal, symlinks, conflicting existing paths, image/binary
manifest entries and fake calibration flags reject. No runtime-internal field,
queue, suppress/retry path or scheduler entry is added.

The package verifier is new. Existing acquisition/report/calibration verifier
algorithms remain unchanged. They still require original images and must reject
the incomplete public image-free subset. Model/measurement failure is retained.
No Vision-product file or centralized runtime-internal field registry changed.

All447 published original text files were compared byte-for-byte with the local
archives; all70 existing capture/analysis contracts are unchanged. Raw CSV CRLF
is preserved with explicit Git attributes (`-text`, `cr-at-eol`), not normalized
away. One trailing space in the published Python reporter was removed without
changing its algorithm. Details are in `validation/source-and-data-audit.json`.

## Commands actually run

Fresh host venv: Python3.10, NumPy2.2.6, SciPy1.15.3, Pillow11.3.0. The CI recipe
uses Python3.11 with the same pinned packages and is published only as an inactive
example under `docs/examples/`; it was not installed because GitHub rejected the
new workflow without the OAuth `workflow` scope. The existing CI is unchanged.
The initial isolated-tree import correctly required installing the
declared SciPy dependency rather than relying on the developer's external cache.

```
python3 -m venv build/camera-handoff-venv
build/camera-handoff-venv/bin/python -m pip install -r requirements-camera.txt
python3 tools/bootstrap_camera_handoff.py
```

Linux/glibc C build and models used the pinned devkitPro SDK container with
network disabled. `validation/native-build-and-tests.txt` preserves its output
with ANSI screen-control sequences and trailing screen-padding whitespace removed;
the original log SHA and normalization are recorded.

```
make build/probe-driver build/probe-plan CC=gcc LDFLAGS=
EXPOSURE_DENSE_ONLY=1 ./build/probe-driver
EXPOSURE_REBUILD_ONLY=1 ./build/probe-driver
GAIN_MATRIX_ONLY=1 ./build/probe-driver
CCM_BOOT_ONLY=1 ./build/probe-driver
./build/probe-plan
```

PASS:12 Dense,16 Rebuild,13 Gain Matrix,15 CCM Boot scenarios, plus boot-journal
and scope/parser checks. Native test compilation retains unused static-function
warnings; the actual ARM build with its `-Werror` configuration passed.

From the enclosing repository root in the fresh venv:

| Command | Tests | Result |
|---|---:|---|
| `python tests/camera_hardware_handoff.py` |8|PASS|
| `python tests/exposure_dense_report.py` |10|PASS|
| `python tests/exposure_rebuild_report.py` |15|PASS|
| `python tests/camera_gain_matrix_report.py` |15|PASS|
| `python tests/camera_ccm_boot_report.py` |15|PASS|
| `python tests/exposure_dense_fit.py` |5|PASS|
| `python tests/exposure_response_fit.py` |6|PASS|
| `python tests/exposure_luma_transfer.py` |10|PASS|
| `python tests/exposure_luma_validate.py` |9|PASS|
| `python tests/exposure_sweep_session.py` |18|PASS|

Total111 Python tests. Exact per-command stdout/stderr/return codes are in
`validation/python-results.json`. Models use actual C-generated synthetic
fixtures; no hardware was contacted and generated fixture images are ignored.

```
python tools/exposure_sweep_contract.py --header diagnostics/camera-deep-probe/probes/exposure/sweep_contract.h --check
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=handoff-20261114
python tools/verify_3dsx.py diagnostics/camera-deep-probe/FilmExposureSweep.3dsx
git diff --check
```

ARM result:391920bytes, SHA256
`d3211d7650da96619f618aefc847049287e0897cc7ee03376af370a7a227626f`.
The rebuild revision differs from the historical deployed binary, whose SHA is
retained in `snapshot.json`; no binary is committed.

The final publication manifest and staged tree are separately checked for text
integrity, unchanged original bytes and absence of images/archives/binaries.
`verify_camera_handoff.py` does not turn a copied historical pass into a newly
reverified capture/model result. Known local build/bytecode exclusions are
registered and apply only under diagnostics; staged files are audited separately.

## Commands not run / limits

These user-specified Vision commands were not run: the paths are absent in
Film3DS and no Vision runtime boundary is modified.

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

No new 3DS run, minute-scale hold,25fps+ calibration, physical CCM timeline,
optical-time/ISO calibration, full color/dynamic-range validation or production
AE integration was performed as part of publishing. Archived field/GDB/legacy
paths retain their historical requirements; this validation covers the explicitly
listed current capture/report/fit/transport path, not every historical executable.
