#!/bin/sh
set -eu
mkdir -p build
exec >build/exposure-auto-quick-validation.log 2>&1
make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=
EXPOSURE_AUTO_ONLY=1 ./build/probe-driver
./build/probe-plan
python3 tests/ram_evidence.py
make -f Makefile.probe PROBE_NAME=FilmExposureAuto PROBE_BUILD=build-exposure-auto REVISION=exposure-auto-v6
python3 tools/verify_3dsx.py FilmExposureAuto.3dsx
