#!/bin/sh
# Build the portable libjpeg-turbo implementation for the ARM11/VFPv2.
set -eu
build_dir=$1
devkitarm=$2
revision=af9c1c268520a29adf98cad5138dafe612b3d318  # libjpeg-turbo 3.1.3
source_dir="$build_dir/source"
object_dir="$build_dir/obj"

if [ ! -d "$source_dir/.git" ]; then
    git init -q "$source_dir"
fi
if [ "$(git -C "$source_dir" rev-parse HEAD 2>/dev/null || true)" != "$revision" ]; then
    git -C "$source_dir" fetch -q --depth=1 https://github.com/libjpeg-turbo/libjpeg-turbo.git "$revision"
    git -C "$source_dir" checkout -q --detach FETCH_HEAD
    test "$(git -C "$source_dir" rev-parse HEAD)" = "$revision"
fi

"${CMAKE:-cmake}" -G 'Unix Makefiles' -S "$source_dir" -B "$object_dir" \
    -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_C_COMPILER="$devkitarm/bin/arm-none-eabi-gcc" \
    -DCMAKE_C_FLAGS="-march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard -mtp=soft -ffunction-sections -fdata-sections" \
    -DCMAKE_BUILD_TYPE=Release -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF \
    -DENABLE_STATIC=ON -DENABLE_SHARED=OFF
"${CMAKE:-cmake}" --build "$object_dir" --target jpeg-static --parallel 4
test -s "$object_dir/libjpeg.a"
touch "$object_dir/libjpeg.a"
