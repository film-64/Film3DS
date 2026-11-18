#!/bin/sh
set -eu
mkdir -p build
exec >build/exposure-field-validation.log 2>&1
make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=
EXPOSURE_AUTO_ONLY=1 ./build/probe-driver
./build/probe-plan
make -f Makefile.probe PROBE_NAME=FilmExposureField PROBE_BUILD=build-exposure-field REVISION=exposure-field-v1 PROBE_EXTRA_FLAGS=-DPROBE_FIELD_DEFAULT
python3 tools/verify_3dsx.py FilmExposureField.3dsx
grep -R -n envSetNextLoad /opt/devkitpro/libctru/include/3ds /opt/devkitpro/libctru/include 2>/dev/null || true
