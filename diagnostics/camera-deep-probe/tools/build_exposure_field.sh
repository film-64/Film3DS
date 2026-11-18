#!/bin/sh
set -eu
exec >build/exposure-field-build.log 2>&1
make -f Makefile.probe PROBE_NAME=FilmExposureField PROBE_BUILD=build-exposure-field REVISION=exposure-field-v1 PROBE_EXTRA_FLAGS=-DPROBE_FIELD_DEFAULT
python3 tools/verify_3dsx.py FilmExposureField.3dsx
sh tools/build_exposure_field_installer.sh
