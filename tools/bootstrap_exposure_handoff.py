#!/usr/bin/env python3
"""Restore historical local tool paths without replacing existing work."""
from pathlib import Path
import os
root=Path(__file__).resolve().parents[1]
for link,target in [(root/"build/worktrees/camera-deep-probe",root/"diagnostics/camera-deep-probe"),(root/"build/hardware/exposure-deep/calibration",root/"handoff/exposure-20261112/data")]:
    if link.exists():
        if link.resolve()!=target.resolve():
            raise SystemExit(f"Existing path preserved; inspect manually: {link}")
        continue
    link.parent.mkdir(parents=True,exist_ok=True)
    link.symlink_to(os.path.relpath(target,link.parent),target_is_directory=True)
    print(link.relative_to(root),"->",target.relative_to(root))
