#!/usr/bin/env python3
"""Check Film3DS's 3DSX and embedded SMDH bounds."""
import argparse
import hashlib
from pathlib import Path
import struct


def verify(path):
    data = path.read_bytes()
    if len(data) < 44:
        raise ValueError("truncated 3DSX header")
    magic, header, relocation, version, flags, code, rodata, writable, bss, icon, icon_size, romfs = (
        struct.unpack_from("<4sHH9I", data))
    if magic != b"3DSX" or header != 44 or relocation != 8 or version != 0 or flags != 0:
        raise ValueError("unexpected 3DSX format")
    if not code or not rodata or writable < bss:
        raise ValueError("invalid segment lengths")
    if icon < header + relocation * 3 + code + rodata + writable - bss:
        raise ValueError("SMDH overlaps executable segments")
    if icon_size != 0x36C0 or icon + icon_size != len(data) or romfs:
        raise ValueError("unexpected SMDH length or RomFS")
    if data[icon:icon + 4] != b"SMDH":
        raise ValueError("missing SMDH")
    return len(data), hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    try:
        size, digest = verify(args.artifact)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"PASS 3DSX/SMDH {size} bytes sha256={digest}")
