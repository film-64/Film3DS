#!/bin/sh
set -eu
exec >build/exposure-field-installer.log 2>&1
python3 tools/package_exposure_field.py
make -f Makefile.probe PROBE_NAME=FilmFieldInstall PROBE_BUILD=build-exposure-field-install REVISION=exposure-field-install-v1 'PROBE_EXTRA_FLAGS=-DPROBE_FIELD_DEFAULT -DPROBE_FIELD_INSTALLER'
python3 tools/verify_3dsx.py FilmFieldInstall.3dsx
