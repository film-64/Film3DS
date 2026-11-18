# Keep the decoded scene across Roll → Darkroom

Previously `film_roll_decode` wrote F3C, published JPG/index and freed its linear
scene. Darkroom entry then allocated another scene and read/checksummed the
complete F3C. Page refresh also forced another decode of the selected JPEG.
This was redundant IO, not a second RAW demosaic in the default path. A selected
sensor calibration additionally caused original RAW decoding on every A.

The existing decode function now returns ownership of its current scene and
recipe through optional outputs. The main app keeps that same allocation for
the selected photo. Darkroom entry uses it directly; neither F3C nor the
original nor the selected JPEG is reloaded on this resident path. There is no
new task, worker, queue, wrapper API or pointer-to-pixel copy. Cold/nonresident
photos still load and validate their stored scene once.

Batch decode places the current selected photo last, so only one expanded
scene is needed and that final allocation survives into Darkroom. Other batch
items are published and freed. Selection changes, capture and exit retain the
existing release behavior. The selected scene's recipe is retained too,
including legacy F3S recipes, without rereading the original on resident entry.

A matching scene is reused for every A render. Only changing the sensor
calibration causes one explicit RAW reprepare, whose result is then retained
under the photo/calibration key. Missing RAW scene data returns an error rather
than silently falling back to original decode. JPEG SELECT behavior is unchanged.

Runtime boundaries: existing Roll decode ownership, explicit batch order,
Darkroom cache loading/rendering and selected-thumbnail read boundaries.
`tools/camera_runtime_contract.py` was updated before implementation. No new
source was registered; generated source verification behavior is unchanged.
The centralized internal registry adds `decoded_recipe` and
`decoded_calibration`. Portable image/recipe formats are unchanged.

Changed implementation: `include/roll.h`, `source/storage/roll_decode.c`,
`source/ui/roll_selection.c`, `source/platform/main_3ds.c`.
Tests: `tests/roll_preview.c` verifies direct owned-scene return, recipe return,
resident data equality and rejection of overwriting a live output;
`tests/roll.c` checks selected-photo-last ordering;
`tests/camera_ui_package.py` checks the resident branch has no reload/free,
page entry no longer invalidates the same JPEG, calibrated A reuses the key,
and internal fields stay out of portable structures.

Passed:

```sh
make -j4 build/roll-preview-test build/roll-test CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS=-L/usr/local/lib
./build/roll-preview-test
./build/roll-test
python3 tools/camera_runtime_contract.py --check
python3 tests/camera_ui_package.py UiPackageTests.test_roll_to_darkroom_uses_the_resident_scene UiPackageTests.test_deferred_decode_and_linear_cache_dispatch UiPackageTests.test_contract_and_portable_recipe
git diff --check
```

Current build package: `build/resident-darkroom-20261118-r4/`. Its build and
any deployment receipt are authoritative. Device page-transition timing has
not been measured. These tests do not establish physical SD or GPU performance.

ARM build, 3DSX structure, embedded resources and frozen-manifest verification passed. Binary SHA256: `8fb990b78326cdc3a032066e5b656ed1a4e39b6e0be79e34ff2d116d8fd3b763`. No device page-transition result is claimed.
