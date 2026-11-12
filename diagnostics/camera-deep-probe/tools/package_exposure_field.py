#!/usr/bin/env python3
"""Embed verified field app + user-labelled seed into an SD installer build."""
import hashlib
from pathlib import Path
import zlib
from verify_3dsx import verify

root = Path(__file__).resolve().parents[1]
app = root / "FilmExposureField.3dsx"
verify(app)
seed = (root / "build-exposure-field-payload/seed-labels.csv").read_bytes()
parts = []
for name, data in (("app", app.read_bytes()), ("icon", (root / "FilmExposureField.smdh").read_bytes()), ("seed", seed)):
    parts.append(f"#define FIELD_{name.upper()}_CRC 0x{zlib.crc32(data):08x}u\n")
    if name == "app":
        parts.append(f'#define FIELD_APP_SHA256 "{hashlib.sha256(data).hexdigest()}"\n')
    parts.append(f"static const unsigned char field_{name}[]={{\n")
    parts.extend(",".join(str(v) for v in data[i:i+32]) + ",\n" for i in range(0, len(data), 32))
    parts.append("};\n")
(root / "build-exposure-field-payload/field_payload.h").write_text("".join(parts))
print("Packaged verified field app and measured reference seed")
