# Capture first, explicit Roll decode

Worktree: `Film3DS-sync-20261117`, based on `41762ad`.

## Runtime boundaries and governance

This changes camera diagnostic logging, original-save downstream compute,
startup preview preparation, explicit Roll batch compute and darkroom input
ownership. Film3DS has no RegionTask, TaskScheduler or LaneAdmission; its
matrix is `tools/camera_runtime_contract.py`. The matrix was updated first.
No admission override, automatic retry, capture queue or background worker
was introduced. Unknown source-taking calls remain fail closed.

New registered compute entries (no LaneAdmission or override, proof `none`):

- `film-roll-decode/v1`: explicit single/marked selection, one immutable
  original at a time, original integrity verification before decoding.
- `film-roll-decode-load/v1`: selected-photo local scene-cache load and checks,
  no original payload read or automatic rebuild.
- `film-darkroom-scene-preview/v1`: explicit A grades the cached linear scene
  and uses the existing composition/output render and cached SELECT save.

Existing shutter/save, preview, deletion, legacy repair, darkroom WB and
logging contracts were updated. The generated source registry header changed;
no admission verifier was added or weakened. Cache loading adds validation.
The central internal-field registry includes the batch selection/progress,
selected scene ownership and cache binding fields. FilmRecipe, FilmFrame and
the standard DNG format are unchanged; F3C is disposable local derived data.

## Behavior

- Shutter: acquire RAW, restore camera, atomically write DNG and perform the
  existing full readback/digest/payload comparison. Return to ready without
  RAW development, JPEG encoding or preview publication. Original write and
  verification failures retain their existing visible handling.
- Roll: originals initially have `?` placeholders. B toggles the current photo
  mark. SELECT snapshots marked IDs, or just the current ID if none are marked.
  The explicit batch processes one photo per main-loop iteration. B cancels
  the remaining items between photos; START exits. An error stops the batch,
  preserves originals and retains unprocessed marks. X remains page navigation.
- RAW decode: perform the existing default sensor-to-linear-scene preparation
  once. Save full-resolution linear RGB as little-endian binary32 in F3C,
  preserving negative/superwhite values; no LUT, WB edits, grain or clipping
  are baked in. Downsample the scene to 320x240 before neutral display grading
  and JPEG quality 70. Commit the existing JPG/index association last.
- Darkroom: keep only the selected linear scene in memory. Default calibration
  A reuses it for WB, exposure, tone, LUT, composition and later processing;
  SELECT still saves the cached final output. It never treats the Roll JPEG
  as the editing source. Explicit sensor-calibration changes use the original
  decode path because calibration precedes the cached scene. Legacy F3S recipes
  are read once on entry; native originals keep their existing editing path.
- Startup scans filenames only; it no longer prepares the complete catalog.
  Browsing reads JPG/index only, with at most one grid thumbnail decode per
  normal UI iteration. Missing/corrupt derived files require explicit decode.
- Original deletion also removes F3C. Selection changes, capture, batch decode
  and shutdown release the selected scene. Multi-select stores IDs rather
  than decoded photos, so it does not expand multiple RAWs in memory.
- Normal RAW capture retains stage/error summaries but skips detailed bool,
  Stop/Clear and completion-poll logging/flushes. Debug retains detailed IPC
  traces. The stop, busy, finished, cache, guard and register checks remain.

F3C checks version, ID, dimensions, source CRC against the pair index, header
and complete payload CRC, finite float values, exact length and original
size/mtime before and after loading. Original content is verified at explicit
decode. Reuse assumes app-owned originals are immutable; an external editor
that changes bytes while preserving size and timestamp requires explicit
redecode. F3C is about 3.69 MB for 640x480, plus a 64-byte header; this trades
SD cache space for avoiding repeated demosaic. It is not an original backup.

## Five receives

The production transaction still contains a native gated transition, two RAW
settling frames, one retained RAW and one native restoration frame. Existing
device evidence in `RAW_CAPTURE_AUTOMATION_20261117.md` proves that sequence
worked, not that five is minimal. There is no frame-by-frame evidence here
establishing that either settling frame or the transition is redundant.
No register/DMA safety operation or receive was removed. No claimed capture
speedup in seconds is made without a device run. This change removes known
post-capture compute and synchronous detailed logging first.

## Files and validation

- `tools/camera_runtime_contract.py`, `include/film_runtime_contract.h`: matrix,
  internal registry and generated source verification.
- `include/roll.h`, `source/storage/roll_preview.c`,
  `source/storage/roll_decode.c`, `source/storage/storage_internal.h`:
  deferred save, explicit decode, scene cache and source binding.
- `source/ui/roll_selection.c`, `source/ui/roll.c`: bounded explicit selection
  lifecycle and derived-cache deletion.
- `include/darkroom.h`, `source/ui/darkroom.c`: reusable linear-scene grading
  through the existing composition and output renderer.
- `source/platform/main_3ds.c`: placeholders, controls, one-photo batch dispatch,
  selected-cache ownership, startup and thumbnail budgets.
- `source/platform/raw_camera_3ds.c`: Debug-only detailed hardware diagnostics.
- `tests/roll_preview.c`: deferred-save/no-sidecars, fake/unknown-source rejection,
  explicit cache roundtrip, pixel equality with direct RAW processing, corrupt
  cache/original-stat mismatch rejection and original preservation.
- `tests/roll.c`: single/marked snapshots, cancellation/error and cache deletion.
- `tests/camera_ui_package.py`: dispatch, source contracts and portable structures
  checked against the centralized internal-field registry.

Commands run:

```sh
python3 tools/camera_runtime_contract.py --check
make -j4 build/roll-preview-test build/darkroom-test build/roll-test build/raw-app-test build/wb-composition-test CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
python3 tests/camera_ui_package.py
./build/roll-test
./build/roll-preview-test
./build/raw-app-test
./build/darkroom-test
./build/wb-composition-test
git diff --check
make 3ds
```

Host build, 14 Python contract/package tests and the five C test executables
passed. An initial old deletion-contract string assertion was updated to
require F3C cleanup, matching the expanded production deletion test. No
performance sweep, hardware automation or deployment was run. `make 3ds`
stopped before compilation because DEVKITPRO is not configured. UI controls,
SD cache latency, physical capture timing and ARM build remain unvalidated.

The following required Vision-product commands were not run because those
files do not exist in Film3DS; the Film3DS checks above cover its boundaries:

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```
