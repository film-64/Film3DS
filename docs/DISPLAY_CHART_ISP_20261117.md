# Display-chart RAW ISP, 2026-11-17

Implementation worktree: `/Users/a64/Documents/Codex/Film3DS-sync-20261117`.
Evidence: `/Users/a64/Documents/Codex/Film3DS-audit-20261115/display-chart-20261117/final`.

Both supplied F3S records are CRC-valid v1 RAW10 RGGB, 640x480. ROLL0002 is the
24-patch color chart; ROLL0003 is the grayscale chart. Originals are unchanged.
Reference PNGs have sRGB markers, not P3 ICC profiles. Reference pixel samples
come from the supplied images rather than nominal ColorChecker reflectances.
The calibration assumes the viewer honored those tags when showing the chart.

## Boundary classification and governance

This changes synchronous RAW ISP compute, explicit host calibration compute,
and local export side effects. Film3DS does not contain RegionTask,
TaskScheduler, LaneAdmission, the Vision-product governance file or its Python
matrix/verifiers. The existing authoritative Film3DS matrix is
`tools/camera_runtime_contract.py`; it was updated before implementation.

- `film-raw-isp/v1`: explicit image-space polynomial shading, affine CCM offset,
  optional bounded local exposure, then existing shoulder and transfer function.
- `film-raw-darkroom-export/v1`: explicit new parameter keys and a local native
  float32 prepared RGB cache, in addition to existing exports.
- `film-display-chart-calibrate/v1`: new explicit CLI source, registered before
  use. CRC/shape/low-byte validation, declared chart coordinates and white
  regions, per-capture defect candidates, shading/CCM fitting, validation and
  fresh-directory exports. It invokes the actual C exporter synchronously.

These require no LaneAdmission and permit no override. Proof is `none` for the
pure ISP and `local_trace` for host workflows. Unknown sources still reject;
the generated header now recognizes the registered calibration source. The C
parameter verifier additionally rejects nonfinite/out-of-range controls.
No scheduler, camera capture path, queue or recovery behavior changes.

`host_seconds` was added to the central Film3DS `RUNTIME_INTERNAL_FIELDS` set.
Existing package tests consume that centralized set. Portable FilmRecipe and
FilmFrame structures/formats are unchanged. Shading/CCM/local-exposure controls
are image-development parameters, not admission proof or scheduler metadata.

## Pipeline and controls

The existing lossless RAW10 unpacker is preserved. The pipeline now supports:

1. Per-phase black subtraction and explicit bad-pixel median repair.
2. Existing demosaic/WB, exposure and optional polynomial flat-field gains.
3. Affine linear RGB color correction.
4. Optional local exposure from a bilateral-filtered 32-pixel log-luminance
   grid, interpolated without hard tile boundaries. Shared RGB gain preserves
   channel ratios. `local_strength` is 0..1, `local_limit_ev` is 0..4.
5. Existing shared-channel shoulder and sRGB transfer.

New parameters default to identity/off, preserving current app rendering.
`ccm_offset` is three linear output-space offsets in [-1,1]. `shading_r/g/b`
each contain six log2-gain coefficients for `1,x,y,x²,xy,y²`, with x/y normalized
to [-1,1]; coefficients are bounded [-4,4] and actual gains bounded [0.25,4].
This calibration uses local strength 0.35 and a 1.5 EV limit for the separate
local rendering. The color fit always disables local exposure and shoulder.

The offline tool estimates this setup's pedestal from the darkest gray patch
with a noise margin, not from optical-black sensor pixels. White background
regions estimate display/camera spatial falloff by a quadratic fit with four
fixed robust fitting passes. It cannot distinguish lens shading from screen
angular falloff. Per-capture defect candidate maps reject larger components
that may be real detail; these are not certified persistent sensor maps.

The color fit is a regularized 3x3 matrix plus offset. All 24 patches are
individually held out in leave-one-out validation. The grayscale capture has a
different exposure; it gets a separately recorded per-channel neutral endpoint
fit, without forcing every gray patch through an interpolating lookup table.

## Measured result

- Color mean DeltaE76: baseline 25.443, flat-field-only 19.644, corrected 2.088.
- Corrected max DeltaE76: 5.782. Leave-one-out mean/max: 2.552 / 7.105.
- All 20 adjacent gray-patch median luminance increments are positive after
  the separate gray-capture calibration. This is not an SNR/dynamic-range test.
- RAW samples at 1023: one in the color capture, zero in the grayscale capture.
- Explicit isolated defect candidates: 521 color / 455 gray, scoped to each file.
- Warm host exporter invocation: about 0.09 s baseline, 0.12 s calibrated,
  0.13 s calibrated plus local exposure. This includes file writes/encoding and
  process startup; it is not an isolated decoder benchmark or 3DS timing.

Outputs include labeled comparisons, actual sample overlays, per-capture
parameter files, profile JSON, reports, extended linear sRGB float32 NPY master
files, ICC-tagged sRGB and Display P3 PNGs and RGB16 TIFFs. P3 output converts
linear sRGB through XYZ D65 into P3 before encoding and embedding the P3 ICC;
it does not relabel sRGB numbers. The NPY masters preserve values above 1;
display PNG/TIFF exports clamp to their representable gamut/range. Negative
CCM output is clamped in the existing ISP working-space contract.

## Reproduction

Build `build/filmraw` with the host JPEG include/library paths, then run:

```sh
python3 tools/display_chart_calibrate.py \
  --color-raw /Users/a64/Desktop/ROLL0002.F3S \
  --gray-raw /Users/a64/Desktop/ROLL0003.F3S \
  --color-reference /var/folders/ch/vy4f335j4jn25fxmd05nmp880000gn/T/codex-clipboard-e88e77d9-7131-473c-8d97-165e4e86fa08.png \
  --gray-reference /var/folders/ch/vy4f335j4jn25fxmd05nmp880000gn/T/codex-clipboard-079395d4-3e26-4842-9c8a-019811cd22ac.png \
  --layout /Users/a64/Documents/Codex/Film3DS-audit-20261115/display-chart-20261117/layout.json \
  --p3-profile '/System/Library/ColorSync/Profiles/Display P3.icc' \
  --output /path/to/NEW-output-directory
```

Layout corners are explicit TL/TR/BR/BL sample geometry, in original pixels.
`white_regions` contains raw-gray `[x0,y0,x1,y1]` rectangles. All input file
hashes and the layout are recorded in `report.json`. Failed runs preserve
partial outputs and return nonzero; they do not resume or overwrite results.

## Tests and validation

Changed tests: `tests/raw_darkroom.c`, `tests/raw_darkroom_export.py`.
New: `tests/display_chart_calibrate.py`; added to `test-raw-darkroom`.
Coverage includes actual C rendering, EV bounds and shared-channel ratios,
shading and offsets, invalid controls, source masquerade rejection, bad CRC,
truncation and low bytes, phase-aware candidate detection versus real edges,
independent affine-fit samples, transfer round trips, chart geometry and true
16-bit TIFF low-bit/ICC preservation. Existing package tests check the central
runtime-internal registry against portable structures.

Commands run:

```sh
python3 tools/camera_runtime_contract.py --check
make -j4 test test-raw-darkroom CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
make test-raw-darkroom CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
python3 tests/simulator_http.py
python3 tests/hardware_qa_report.py
git diff --check
```

The combined make run passed preceding tests but stopped at the HTTP simulator
because sandboxed loopback binding was denied. That simulator passed when run
with loopback permission; the subsequent hardware QA report test also passed.
The final standalone RAW suite passed, with one existing skip for the absent
private grain-reference image. RGB16 TIFFs were independently opened in Pillow
and checked for 16/16/16 sample tags, low-bit preservation and embedded ICC.
Logs are copied into the evidence directory.

These required Vision-product commands were not run because the named files do
not exist in Film3DS:

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

Changed implementation files: `include/raw_darkroom.h` (ISP parameter API),
`source/image/raw_darkroom.c` (ISP compute), `tools/raw_darkroom_host.c` (host
parameter/export boundary), `tools/display_chart_calibrate.py` (registered host
workflow), `tools/camera_runtime_contract.py` and generated
`include/film_runtime_contract.h` (contract registry/verification), `Makefile`
(test integration), the tests above and this document. Pre-existing unrelated
worktree changes were retained.

## Limits

This is an offline calibrated workflow and reusable opt-in C ISP support. The
camera UI does not automatically load the fitted profiles, and default camera
rendering is unchanged. No 3DS cross-build, deployment or on-device timing was
performed for this change. Screen brightness, viewing angle, color management,
True Tone/Night Shift, ambient light and camera gain can invalidate this fit.
The color leave-one-out test still shares the same physical capture; independent
captures and illuminants remain untested. Screen charts do not measure sensor
spectral response or native stops of latitude. No clipped sensor signal is
reconstructed. Shadow noise, flare/reflections and some residual false color
remain visible; spatial gains can amplify that noise.
