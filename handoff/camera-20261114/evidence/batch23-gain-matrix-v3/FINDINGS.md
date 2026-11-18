# Batch23: FD control discriminates repeated ISP overwrite

Twenty-one originals1374913 bytes recovered. Five nodes/fifteen trials completed;
strict eight-node report fails as intended, restoration verified.

5fps: FD retained(A1022), coarse4 and528: all target128/96/160 return133.
All four SEQ drivers disabled(A1020), coarse4 and528: all three targets remain
at their written values through eight consecutive VSYNC cycles per target,
in the same uninterrupted stream. AWB channels128 and AE221F128, fixed core528,
coarse and frame/line length remain verified. No new Receive during trials.
15fps: FD retained, coarse4 reproduces133. Measured period about66.66ms in host
span; no assumed raw service time-unit conversion. Full-driver-off15fps nodes
remain unmeasured.

Node5 (15fps, FD retained, coarse528) stopped before capture: matrix_initial
writes all128 succeeded, readback128/133/128/128 tripped the strict setup setter.
This is an observed subject-gain overwrite, not a failed IPC or fixed-control
drift. No firmware exception or RAM capacity failure occurred. Original controls
were restored. BufferErrorBeforeArm timeout records in older seed/preflight
paths are preserved, including the preflight continuous timeout; no hidden retry
was added to matrix nodes.

Static640 alias checks passed in every completed snapshot; dynamic CCM match
flags and all bytes are retained without atomicity claims. This supports FD as
a necessary control dependency for these sampled5fps conditions, not proof of
the internal writer identity, update formula or permanent arbitrary control.
Images are initial node images before trial rewrites, not calibration proof.
