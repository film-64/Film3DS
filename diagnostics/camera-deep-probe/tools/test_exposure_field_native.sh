#!/bin/sh
set -eu
exec >build/exposure-field-native.log 2>&1
make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=
EXPOSURE_AUTO_ONLY=1 ./build/probe-driver
./build/probe-plan
