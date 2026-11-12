#!/bin/sh
# Check the linked program for the accidental NEON/ARMv7 paths seen in the toolchain audit.
set -eu
elf=$1
prefix=${2:-arm-none-eabi-}
export LC_ALL=C
attributes=$("${prefix}readelf" -A "$elf")
printf '%s\n' "$attributes"
printf '%s\n' "$attributes" | grep -q 'Tag_FP_arch: VFPv2'
if printf '%s\n' "$attributes" | grep -q 'Tag_Advanced_SIMD_arch'; then
    echo "unexpected Advanced SIMD attributes" >&2
    exit 1
fi

symbols=$("${prefix}nm" "$elf")
if printf '%s\n' "$symbols" | grep -Eq '(_neon|__sync_synchronize_dmb)$'; then
    echo "unexpected NEON or ARMv7 DMB symbol in linked output" >&2
    exit 1
fi

# devkitARM 16.1.0 may merge attributes from its discarded DMB helper even with
# section garbage collection. Inspect actual instructions for those barriers.
instructions=$("${prefix}objdump" -d "$elf")
if printf '%s\n' "$instructions" | grep -Eq '^[[:space:]]*[0-9a-f]+:.*[[:space:]](dmb|dsb|isb)[[:space:]]'; then
    echo "unexpected standalone ARMv7 barrier instruction" >&2
    exit 1
fi
echo "PASS ARM11 JPEG audit: VFPv2, no linked NEON or ARMv7 barriers"
