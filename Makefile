CC ?= cc
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -ljpeg -lm
CORE := $(wildcard source/camera/*.c source/color/*.c source/image/*.c source/storage/*.c source/ui/*.c source/debug/*.c)
HEADERS := $(wildcard include/*.h source/storage/*.h source/color/*.h)

.PHONY: all host test test-native-integration test-raw-darkroom test-standard-media sanitize bench 3ds clean
all: host
host: build/filmctl
build/filmctl: $(CORE) $(HEADERS) tools/host.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tools/host.c $(LDFLAGS) $(LDLIBS) -o $@
build/filmraw: $(CORE) $(HEADERS) tools/raw_darkroom_host.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tools/raw_darkroom_host.c $(LDFLAGS) $(LDLIBS) -o $@
build/filmscene: $(CORE) $(HEADERS) tools/raw_scene_host.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tools/raw_scene_host.c $(LDFLAGS) $(LDLIBS) -o $@
build/raw-darkroom-test: $(CORE) $(HEADERS) tests/raw_darkroom.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/raw_darkroom.c $(LDFLAGS) $(LDLIBS) -o $@
build/raw-app-test: $(CORE) $(HEADERS) tests/raw_app.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/raw_app.c $(LDFLAGS) $(LDLIBS) -o $@
build/gpu-readback-axes-test: tests/gpu_readback_axes.c include/gpu_layout.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/gpu_readback_axes.c $(LDFLAGS) $(LDLIBS) -o $@
build/ae-region-test: $(CORE) $(HEADERS) tests/ae_region.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/ae_region.c $(LDFLAGS) $(LDLIBS) -o $@
build/gray-capture-test: $(CORE) $(HEADERS) tests/gray_capture.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/gray_capture.c $(LDFLAGS) $(LDLIBS) -o $@
build/exposure-matrix-test: $(CORE) $(HEADERS) tests/exposure_matrix.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/exposure_matrix.c $(LDFLAGS) $(LDLIBS) -o $@
test-raw-darkroom: build/filmscene build/filmraw build/raw-darkroom-test build/raw-app-test build/roll-preview-test build/gpu-pipeline-test build/exposure-matrix-test build/gray-capture-test build/ae-region-test build/gpu-readback-axes-test
	python3 tools/camera_runtime_contract.py --check
	./build/exposure-matrix-test
	./build/gray-capture-test
	./build/ae-region-test
	./build/gpu-readback-axes-test
	python3 tests/gray_pair_report.py
	./build/raw-darkroom-test
	./build/raw-app-test
	./build/roll-preview-test
	./build/gpu-pipeline-test
	python3 tests/raw_darkroom_export.py
	python3 tests/display_chart_calibrate.py
	python3 tests/raw_scene_pipeline.py
build/unit: $(CORE) $(HEADERS) tests/unit.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/unit.c $(LDFLAGS) $(LDLIBS) -o $@
build/hardware-exposure-test: $(CORE) $(HEADERS) tests/hardware_exposure.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/hardware_exposure.c $(LDFLAGS) $(LDLIBS) -o $@
build/hardware-transport-test: $(CORE) $(HEADERS) tests/hardware_transport.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/hardware_transport.c $(LDFLAGS) $(LDLIBS) -o $@
build/darkroom-test: $(CORE) $(HEADERS) tests/darkroom.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/darkroom.c $(LDFLAGS) $(LDLIBS) -o $@
build/wb-composition-test: $(CORE) $(HEADERS) tests/white_balance_composition.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/white_balance_composition.c $(LDFLAGS) $(LDLIBS) -o $@
build/roll-test: $(CORE) $(HEADERS) tests/roll.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/roll.c $(LDFLAGS) $(LDLIBS) -o $@
build/roll-preview-test: $(CORE) $(HEADERS) tests/roll_preview.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/roll_preview.c $(LDFLAGS) $(LDLIBS) -o $@
build/gpu-pipeline-test: $(CORE) $(HEADERS) tests/gpu_pipeline.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/gpu_pipeline.c $(LDFLAGS) $(LDLIBS) -o $@
test: build/standard-media-test host build/unit build/hardware-exposure-test build/hardware-transport-test build/darkroom-test build/wb-composition-test build/roll-test
	python3 tools/camera_runtime_contract.py --check
	python3 tests/camera_ui_package.py
	./build/hardware-exposure-test
	./build/hardware-transport-test
	./build/unit
	./build/darkroom-test
	./build/wb-composition-test
	./build/roll-test
	./build/standard-media-test
	python3 tests/dng_interop.py
	python3 tests/integration.py
	python3 tests/simulator_http.py
	python3 tests/hardware_qa_report.py
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined -no-pie"
test-native-integration:
	python3 tools/camera_runtime_contract.py --check
	python3 tests/native_integration.py
	python3 tests/native_gallery_audit.py
bench: host
	python3 tools/fixtures.py build/luts
	for size in 17 33 65; do ./build/filmctl bench build/luts/nonlinear$$size.cube || exit; done
3ds:
	$(MAKE) -f Makefile.3ds
clean:
	rm -rf build build-3ds Film3DS.3dsx Film3DS.elf Film3DS.smdh Film3DS.map

build/standard-media-test: $(CORE) $(HEADERS) tests/standard_media.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/standard_media.c $(LDFLAGS) $(LDLIBS) -o $@
test-standard-media: build/standard-media-test
	./build/standard-media-test
	python3 tests/dng_interop.py
