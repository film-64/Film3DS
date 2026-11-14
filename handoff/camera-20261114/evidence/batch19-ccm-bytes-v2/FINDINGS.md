# Batch19: device exited, no runtime evidence recovered

The v2 binary367520bytes SHA256d81f3959be6e4d94416f863c7194a9e5342cc119b9858b2dd1e68c2ed1b67825
was accepted once by192.168.0.14:17491. Host source/C/binary and validation were
preserved. The device did not expose RAM HTTP5000 during the360s session wait.
The user reported a screen flash and application exit, with no readable error;
the exact timing and destination were not observed. GDB4000 also refused.

No device files were recovered. A host status of capturing means deployment
finished and readiness was being awaited, not proof the sensor measurement ran.
There is no evidence that the logical-byte reference check completed, that a
CCM coefficient was read, or that camera settings were restored. Those states
remain UNKNOWN. The host session failed with a bounded TimeoutError and did not
re-deploy. Do not mark this run complete or substitute unit tests for device data.

The code still reserves28MiB for the evidence arena,614400bytes of linear image
buffer and later1MiB for networking, unchanged from v1. RAM allocation failure
already has a START-held error screen. These facts do not establish whether this
exit was an allocation failure, startup/loader failure, exception, APT exit or
sensor/service-related problem. The binary contains the expected v2 scope and
revision strings; no rollback or alternate binary was sent.

Prior MCU control-variable writes are not arbitrary RAM/ROM patches: v2 changes
only the registered observation method, reading logical-byte aliases through
Camera service. No actual successful byte read was observed in this run.

Runtime boundary: previously registered standalone hardware diagnostic,
local_trace only, no Vision LaneAdmission. This acquisition changed no source,
matrix, verifier or internal fields. Binary/source verification and one session
command ran; deployment succeeded, recovery failed. No device completeness test
can pass without original files. Prior model tests passed but cannot diagnose
this hardware exit. Any subsequent startup journaling needs its own registered
source rather than changing this v2 contract after the fact.
