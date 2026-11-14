# Standalone N3DS camera probe snapshot

Current exposure/gain/FD/CCM source and injected-failure models, copied from the
working probe at base5a1e62d through Batch28. No binary, firmware dump or image is
distributed here. The enclosing Film3DS root supplies Python contracts/reporters.

Read [hardware status](../../CAMERA_HARDWARE_HANDOFF.md) and the
[build/test/capture guide](../../docs/CAMERA_HARDWARE_TESTING.md).
Use explicit make targets from that guide; the preserved historical Makefile also
contains unrelated host targets whose application sources are in the root repo.

Latest dense scope: `--calibration-part=camera-exposure-dense-v1`.
Rebuild/v2 adds hold/color-vector/FD transition observations. Earlier field/auto
paths are retained for source history; field/v1 shutter predictions are withdrawn.
Unit models and diagnostic completion never imply optical-time/ISO calibration.

The generated `probes/exposure/sweep_contract.h` must exactly match root
`tools/exposure_sweep_contract.py`. Do not patch its checks independently.
