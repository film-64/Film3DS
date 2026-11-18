CC ?= cc
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -ljpeg -lm
CORE := $(wildcard source/camera/*.c source/color/*.c source/image/*.c source/storage/*.c source/ui/*.c source/debug/*.c)
HEADERS := $(wildcard include/*.h source/storage/*.h)

.PHONY: all host test sanitize bench 3ds clean probe-test
all: host
host: build/filmctl
build/filmctl: $(CORE) $(HEADERS) tools/host.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tools/host.c $(LDFLAGS) $(LDLIBS) -o $@
build/unit: $(CORE) $(HEADERS) tests/unit.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/unit.c $(LDFLAGS) $(LDLIBS) -o $@
test: host build/unit probe-test
	./build/unit
	python3 tests/integration.py
	python3 tests/simulator_http.py
	python3 tests/hardware_qa_report.py
build/probe-driver: probes/exposure/main.c $(wildcard probes/exposure/*.h) tests/probe_driver.c tests/probe_shim/3ds.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-unused-parameter -Itests/probe_shim -ffunction-sections -fdata-sections tests/probe_driver.c $(LDFLAGS) -Wl,--gc-sections -o $@ -lm
build/probe-ipc-decode: tests/probe_ipc_decode.c probes/exposure/ipc_decode.h
	@mkdir -p build
	$(CC) $(CFLAGS) tests/probe_ipc_decode.c -o $@
build/probe-plan: tests/probe_plan.c probes/exposure/probe_plan.h
	@mkdir -p build
	$(CC) $(CFLAGS) tests/probe_plan.c -o $@
probe-test: build/probe-driver build/probe-ipc-decode build/probe-plan
	./build/probe-driver
	./build/probe-ipc-decode
	./build/probe-plan
	python3 tests/exposure_report.py
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined -no-pie"
bench: host
	python3 tools/fixtures.py build/luts
	for size in 17 33 65; do ./build/filmctl bench build/luts/nonlinear$$size.cube || exit; done
3ds:
	$(MAKE) -f Makefile.3ds
clean:
	rm -rf build build-3ds Film3DS.3dsx Film3DS.elf Film3DS.smdh Film3DS.map
