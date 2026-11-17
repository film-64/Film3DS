#!/usr/bin/env python3
"""Install the offline field app once; verify the device's SD CRC receipt."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "build/worktrees/camera-deep-probe"
sys.path.insert(0, str(PROBE / "tools"))
from run_deep_probe import deploy, receive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    installer = PROBE / "FilmFieldInstall.3dsx"
    app = PROBE / "FilmExposureField.3dsx"
    body = app.read_bytes()
    sha = hashlib.sha256(body).hexdigest()
    request = {"source": "exposure-field-install/v1", "device": args.ip,
               "app_sha256": sha, "app_bytes": len(body), "app_crc32": f"{zlib.crc32(body):08x}",
               "installer_sha256": hashlib.sha256(installer.read_bytes()).hexdigest(),
               "started_unix": time.time(), "capture_automatically": False,
               "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted((PROBE / "probes/exposure").glob("*")) if p.is_file()}}
    (args.out / "deployment.json").write_text(json.dumps(request, indent=2) + "\n")
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", 17491))
        server.listen(1)
        server.settimeout(75)
        deploy(args.ip, installer, ("--field",))
        conn, peer = server.accept()
        with conn:
            if peer[0] != args.ip:
                raise ValueError("unexpected installation receipt peer")
            conn.settimeout(45)
            if receive(conn, args.out) != "startup":
                raise ValueError("unexpected installation checkpoint")
    receipt = (args.out / "startup/receipt.txt").read_text()
    expected = (f"app_bytes={len(body)}\n", f"app_crc32={zlib.crc32(body):08x}\n",
                f"app_sha256={sha}\n", "readback_crc_verified=1\n")
    if not all(line in receipt for line in expected):
        raise ValueError("SD readback receipt differs from built application")
    (args.out / "installation-verified.json").write_text(json.dumps({"verified": True,
        "app_sha256": sha, "device_crc_readback": True, "observed_unix": time.time()}) + "\n")
    print("SD INSTALLATION VERIFIED; offline field UI launched; no camera capture started.", flush=True)


if __name__ == "__main__":
    main()
