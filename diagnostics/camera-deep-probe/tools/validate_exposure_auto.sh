#!/bin/sh
set -eu
mkdir -p build
exec >build/exposure-auto-validation.log 2>&1
make probe-test CC=gcc LDFLAGS=
python3 tests/calibration_report.py
python3 tests/ram_evidence.py
make -f Makefile.probe PROBE_NAME=FilmExposureAuto PROBE_BUILD=build-exposure-auto REVISION=exposure-auto-v6
python3 tools/verify_3dsx.py FilmExposureAuto.3dsx
