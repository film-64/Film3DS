# Darkroom GPU composition and decode correction

This patch is for Film3DS. Runtime boundaries: RAW decode preprocessing,
darkroom output compute, GPU resource ownership and shutdown. The contract
matrix was updated before implementation. New entries are
`film-raw-bad-pixel/v1`, `film-grain-gpu-compose/v1`,
`film-darkroom-gpu-output/v1` and `film-darkroom-render-pair/v1`.
They require no LaneAdmission, permit no override and carry proof `none`.
Source-taking APIs reject unknown/fake sources. GPU errors propagate without
CPU retry or partially publishing output. The original acquisition/DNG write
sequence is unchanged.

## Stage ownership

- CPU: RAW unpack, conservative bad-pixel correction, demosaic, linear scene
  preparation and grading. The reusable decoded scene retains this result.
- GPU output with bilinear sampling and zero sharpness: upload the cropped
  graded source once, with replicated edge padding; GPU samples it at the
  requested size. CPU resizes scalar linear luminance for adaptive grain
  response, rather than reading a GPU-resized RGB image back.
- Bicubic or nonzero sharpness: explicit CPU resize/sharpen stage, then GPU
  grain composition. This is a declared stage partition, never a retry after
  GPU failure. Exact CPU density mode remains selectable.
- CPU: deterministic Fine/Medium/Coarse fields and adaptive scalar density
  response. A 256-entry sRGB decode table avoids repeated decode powers.
- GPU: three multiplicative gain layers into a resident target texture, then
  final photo multiply, positive response and negative response. The scalar
  layer encoding compensates cumulative RGBA8 quantization. The gray response
  also accounts for the sRGB linear toe. This is a fixed-function affine
  approximation, not bit-identical to the CPU's per-channel density transform.
- CPU after GPU completion: copy output layout to RGB24 and JPEG encode.
  There is no noise-field readback and no CPU post-readback pixel blending.

The CPU resize/sharpen partition passes an owned RGB image to GPU grain.
Success swaps in the completed output allocation instead of copying the whole
RGB image again; failure preserves the original allocation and contents.

PICA resources are main-thread owned and persist across tiles and A previews:
three L8 gain maps, one RGBA8 fallback photo map, one RGB8 positive response
map, one L8 negative response map, a VRAM gain texture/target, output target,
readback buffer and reusable CPU response buffers. The source texture is
reallocated only when its padded dimensions change. Resource release drains
submitted work and occurs before the application's `C3D_Fini()`.

There is one final RGB transfer per completed tile, not one for the whole
arbitrarily sized photograph: the tile target is 256x256 with a 254px output
core. Two asymmetric markers outside that core validate readback orientation
without scanning/reconstructing every expected noise value. Layer coordinates
use global output pixel centers and a deterministic photo seed. Source UVs
remain global across output tiles; edge padding prevents crop-neighbor leaks.
Physical target-texture orientation and seams still require device validation.

## One A render, one saved result

`film_develop_canvas_render_pair` renders final resize/sharpen/grain once.
The composition view uses the already graded full canvas as context and
places a resized copy of that exact final output inside the original-coordinate
crop rectangle. Outside the rectangle it displays graded context, without
another grain/sharpen pass. The frame remains visible and keeps its original
coordinates. SELECT JPEG-encodes the cached final crop with no new rendering.

## Bad pixels

Decode copies unpacked samples to a frozen reference. Only an interior sample
at least 128/1023 away from the median of its eight same-CFA neighbors is
replaced, and only when those neighbors span at most 64/1023. The outer two
rows/columns, uncertain edges and clusters are retained. Thresholds and scene
cache version are generated from the matrix. This single-frame heuristic is
not a persistent defect certification; an isolated real point highlight can
also match it. Original DNG bytes are untouched.

F3C cache version is now 2. Older caches must be explicitly decoded again;
there is no hidden startup rebuild. Existing explicit sensor bad-pixel maps
remain available in the ISP stage.

## Changed contracts and verification

- `tools/camera_runtime_contract.py`, generated `include/film_runtime_contract.h`:
  stage rules, source registry, thresholds, cache version, tile/gain bounds.
  Admission verification is unchanged; cache version rejection is stricter.
- `include/film.h`, `include/platform_develop.h`, `include/layered_grain.h`,
  `include/gpu_layout.h`: internal renderer callback, workspace release,
  scalar response preparation and shared tiled-address calculation.
- `source/platform/grain_gpu_3ds.c`: resident fixed-function composition,
  GPU bilinear output, pooled allocations and final-only tile readback.
- `source/platform/main_3ds.c`: explicit GPU output dispatch and workspace
  cleanup before Citro3D shutdown.
- `source/image/image.c`, `source/ui/darkroom.c`: single render pair and reuse.
- `include/raw_darkroom.h`, `source/image/raw_darkroom.c`,
  `source/storage/raw_export.c`, `source/storage/roll_decode.c`: decode-only
  bad-pixel correction and old scene-cache rejection.
- `source/image/layered_grain.c`: CPU luminance/field/mask preparation; the
  exact CPU backend and diagnostic field-mixer APIs remain available.

New renderer/workspace fields are in the centralized internal registry.
Portable FilmRecipe/FilmFrame layout and DNG metadata did not change.
Named C struct tags support the internal callback types; the portable-structure
test still checks their actual members against that same registry.

Targeted tests: `tests/gpu_pipeline.c` covers hot/dead samples, protected
borders/edges/clusters, source rejection, rectangular texture address coverage,
global 254px tile boundaries, deterministic three-scale masks, seed/size
changes, a software fixed-function model, one grain pass per A, composition
inset equality and byte-identical JPEG cache save. `tests/raw_app.c` now uses a
two-sample chroma cluster to test denoise independently of isolated-point
correction. `tests/camera_ui_package.py` checks the production GPU dispatch,
one transfer site, resource reuse, single render-pair call, shutdown ordering
and portable field registration.

Validation commands:

```sh
make -j4 build/gpu-pipeline-test build/raw-app-test build/roll-preview-test build/wb-composition-test CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
./build/gpu-pipeline-test
./build/raw-app-test
./build/wb-composition-test
./build/roll-preview-test
python3 tools/camera_runtime_contract.py --check
python3 tests/camera_ui_package.py UiPackageTests.test_contract_and_portable_recipe UiPackageTests.test_gpu_final_composition_reuses_resources_and_cache UiPackageTests.test_composition_frame_survives_A_and_geometry_edits UiPackageTests.test_home_close_cleanup_does_not_wait_for_another_vblank
git diff --check
```

All four C executables and the four selected Python checks passed. Final
centralization of unchanged numeric bounds was rebuilt with the
`build/gpu-pipeline-test` target. The software fixed-function fixtures reported
maximum differences of 3/255 for both grayscale and colored gradients against
the exact CPU reference. These are software-model results, not PICA measurements.
Initial testing caught a 10/255 dark-toe error: the implementation was corrected
with the scalar toe response and cumulative gain encoding; the error tolerance
was not increased. Initial denoise coverage used an isolated spike, which is
now correctly removed during RAW decode; its fixture now uses a coherent
same-phase cluster and still requires an actual denoise effect.

No timing sweep, capture automation or deployment was run. ARM/3DS compilation
was not run because the current environment has no configured DEVKITPRO/toolchain.
GPU source compilation, physical readback orientation, target-texture cache
coherence, seams, real CPU/GPU image differences and speed remain unverified.
The runtime markers reject invalid final readback orientation; they do not
certify intermediate gain-texture orientation or image fidelity.

The resident target uses Citro3D's [render-target texture API](https://github.com/devkitPro/citro3d/blob/master/include/c3d/renderqueue.h).
Every sampled texture is explicitly rebound after updates; this avoids relying
on upload alone to invalidate GPU texture state, a distinction described in
[the upstream texture-cache report](https://github.com/devkitPro/citro3d/issues/71).


## Verified 3DS build and NetLoader deployment

The existing workspace Lima instance and pinned devkitARM image were found
under `.tools/film3ds-lima` and reused. The current frozen sources were copied
into the VM-mounted Film3DS build directory; no old application objects were
reused. The older build-driver difference is confined to staging, which was
performed with the current driver before this copy. Compilation, ARM11/VFPv2
instruction audit, 3DSX structure, frozen-source hashes and embedded resources
all passed. This supersedes the earlier unavailable-toolchain status above.

Package: `build/deferred-gpu-20261118-r1/`; binary size: 11,184,120 bytes.
SHA256: `0073ec8871e9407957b1ae48d0bcc6bd6043c4d308877e4cf6984f0b954008d5`.
The single deployment to user-specified `192.168.0.27:17491` completed with
`NetLoader_payload_accepted_launch_requested`; see package `build.json` and
`deployment.json`. This establishes complete payload acceptance and a sent
launch request, not physical camera/GPU/HOME-close validation.

This step uses the existing package/deploy side-effect contracts. No further
runtime code, matrix, verifier, internal fields or tests changed; no host test
suite was repeated. Build log: `build/deferred-gpu-20261118-r1-build.log`.
