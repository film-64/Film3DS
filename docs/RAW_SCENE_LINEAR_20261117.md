# RAW scene-linear darkroom

Worktree: `Film3DS-sync-20261117`. Package: `build/scene-linear-20261117-r2`.

The production RAW darkroom now decodes into `FilmRawScene`, an owned signed
linear float RGB image. Values above 1 and negative CCM results remain in that
scene. No ISP shoulder, local tone mapping, sRGB encoding or integer canvas is
applied to the scene. Each A preview develops the immutable original anew.

The production display stage applies linear WB and recipe exposure once, then
scene tone, optional local exposure and the selected late shoulder. Only the
display result goes through the existing LUT/final H/M/S and RGB16 cache.
Existing native-image grading and cached SELECT JPEG saving remain intact.
The old explicit ISP API remains compatible for existing offline tools.

Added RAW controls: calibration Off / Display color / Display gray; scene
shadows/midtones/highlights +/-2.5 EV in 0.5 EV steps; shoulder knee Off through
0.95; local exposure 0..100%, bounded to 1.5 EV in the UI. Scene tone interpolates
EV offsets at -6, 0 and +6 stops relative to 18% gray; legal extremes preserve
positive response slope. Existing final H/M/S controls still act after LUT.
These new session controls live with existing develop options, not in DNG
metadata or the legacy portable recipe. They are not persisted across restart.
No free-point curve editor, histogram or TN display correction was added.

Calibration is an explicitly selected, strictly parsed <=4096-byte `F3ISP1`
asset: black, white, WB, CCM, offset and spatial shading. Corrupt/unknown assets
fail rather than falling back. The two included display profiles are scoped to
the supplied screen captures, not universal camera profiles. Per-capture defect
candidate maps from the earlier offline tool are not installed as permanent
sensor maps; residual defective pixels may remain. RAW LUT sizes 2..256 now
match the existing ordinary CUBE catalog rather than rejecting 32-point LUTs.

## Governance

Classification: synchronous RAW development compute, explicit offline/on-device
probe compute, read-only calibration loading and package/deployment/local
trace side effects. `tools/camera_runtime_contract.py` was updated first.
New registered boundaries: `film-raw-scene/v1`, `film-raw-scene-grade/v1`,
`film-raw-profile-load/v1`, `film-raw-scene-probe/v1`,
`film-raw-scene-deploy/v1`. The existing app RAW canvas, darkroom and frozen
file evidence contracts describe the new path. No LaneAdmission is required,
no override is permitted, unknown/fake sources reject. No queue, recapture,
retry or camera path was introduced. The explicit scene probe does not open a
camera and never uses a warmup/manual/admitted escape string.

Verifier changes: source checks at scene boundaries, finite/range checks for
scene controls, bounded ordered calibration parsing, token-bound device
recovery and predefined PC/device numerical tolerances. The generated header
is synchronized. Central runtime-internal registry additions: `raw_isp`,
`scene_samples`, `scene_min`, `scene_max`, `scene_negative`, `scene_superwhite`,
`scene_qa_token`. The existing `host_seconds` remains internal. Portable recipe
and RAW formats are unchanged; package tests use the central registry.

Implementation: `include/film.h`, `include/raw_darkroom.h`,
`include/darkroom.h`, `source/image/raw_darkroom.c`, `raw_scene_probe.c`,
`source/storage/raw_export.c`, `raw_profile.c`, `source/image/image.c`,
`source/ui/darkroom.c`, `source/platform/main_3ds.c`,
`tools/raw_scene_host.c`, `raw_scene_validate.py`, the contract matrix/header,
Makefile and the explicit ROMFS calibration/reference assets. Existing unrelated
dirty work was preserved.

## Validation

Updated `tests/raw_darkroom.c`; new `tests/raw_scene_pipeline.py` and its frozen
reference samples. Tests cover signed/superwhite preservation, reducing EV
before display compression, immutable scene data, all extreme scene-curve
combinations on a dense log ramp, invalid controls, unknown/fake sources,
malformed calibration and actual C development of both user originals. Color
calibration mean DeltaE76 remains below 3 without optional local exposure; all
20 gray increments increase. Local exposure deliberately changes chart tone,
so its color-fit error is not substituted for the neutral calibration metric.

Commands run:

```
python3 tools/camera_runtime_contract.py --check
make -j4 test test-raw-darkroom CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
make -j4 test-raw-darkroom CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
python3 tests/simulator_http.py
python3 tests/hardware_qa_report.py
git diff --check
```

The full host run stopped only at sandbox loopback binding. HTTP and the
following hardware-report test passed separately with loopback permission.
The final RAW suite passed (one existing private grain-reference skip).
ARM11 cross-build, VFPv2/no-NEON/no-ARMv7-barrier audit, 3DSX verification, frozen
source comparison and all embedded-resource checks passed. Existing pinned
scalar JPEG archive reused; application sources rebuilt. Logs and hashes are
in the r2 package. Initial offline JPEG-cache setup attempts failed before the
successful build and did not produce a deployed candidate.

Not run: `pytest tests/test_runtime_contract_boundaries.py -q`,
`pytest tests/test_runtime_projection.py -q`,
`pytest tests/test_metadata_runtime_package.py -q`: these Vision-product files
are absent in Film3DS; the applicable Film3DS contract tests ran above.

## PC/device protocol

`tools/raw_scene_validate.py host --case color|gray --out NEW_DIR` runs the
production C core on the bundled originals with calibration, zero scene EV
adjustments, shoulder off, local strength .35 and local EV limit 1.5. Matching
r2 references are in `build/scene-validation/color-r2` and `gray-r2`.

`deploy --package PACKAGE --ip IP --case color|gray --out NEW_DIR` verifies the
frozen package and makes one official 3dslink attempt. It records a unique
launch token and invokes `f3sceneqa=TOKEN` (plus `f3scene-gray` for gray).
The console produces canvas RGB16, 1200 signed linear RGB samples, a JPEG and a
local report, then exposes only those files through the existing frozen RAM
HTTP transport. `retrieve --ip IP --token TOKEN --reference PC_DIR --out NEW_DIR`
requires the exact token and complete/nonfailed snapshot, checks transport CRC,
then compares numerical results. Acceptance: RGB16 max difference <=4 codes;
scene samples rtol 1e-5 / atol 1e-6. Failure is retained, not silently accepted.

User-specified current destination: `192.168.0.48`. Deployment receipt and any
recovered data are under `build/scene-validation/device-color-launch` and the
subsequent device-data directory. A launch acknowledgement is not a parity
result. Fixed-input device parity results are recorded below; physical UI controls
and broader resource behavior remain unverified. Independent captures/illuminants,
shadow SNR and local-tone halo quality are not certified by these two charts.

## Observed color-device result

The user changed NetLoader to `192.168.0.48` before transmission. The verified
r2 binary was uploaded once and the color probe launched. All four frozen
files were recovered with matching CRCs and exact launch token. RGB16 maximum
PC/ARM difference: 1 code out of 65535; mean 0.000043403 code. Scene sample
maximum absolute difference: 5.960464477539063e-08. The predefined comparison
passed. Receipt: package `device-color-parity.json`. This confirms this fixed
color case, not physical UI controls or all possible input images. After the user explicitly returned to NetLoader, a second single upload
launched the gray probe. Its four files also passed token/completion/CRC checks.
Gray RGB16 maximum difference: 1 code; mean 0.000014106 code. Scene sample
maximum absolute difference: 1.1920928955078125e-07. Both predefined comparisons
passed; aggregate receipt: package `device-parity.json`. The device remains in
the frozen gray evidence server until START exits. Neither transmission to
192.168.0.69 nor automatic capture was attempted in this change.
