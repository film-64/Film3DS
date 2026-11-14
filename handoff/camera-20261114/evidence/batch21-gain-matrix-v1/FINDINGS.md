# Batch21: incomplete matrix, restored controls

One377828-byte payload deployed to192.168.0.14:17491. Sixteen original files,
820255 bytes recovered with CRC/SHA verification; no RAM capacity error.
Source camera-gain-matrix/v1; strict report fails as intended.

Node0 only:5fps, FD retained(A1022), coarse4, core528. Three period samples
and eight consecutive post-rewrite VSYNC cycles are valid. All eight readbacks
show IFP133/133/133/133, AWB128, AE221F128, coarse4 and stable line/frame length.
Raw period199949726 service units; two-period host span399ms. Raw units are not
converted. No new SetReceiving occurs between the rewrite and these eight cycles.

After observations, SetReceiving returned success but WaitFrame timed out after
2000ms with result09401bfe. No trailing trial image was obtained. The program
stopped, released handles and verified restoration. Original coarse176/core
547,609,552,547; ISP168/160/160/133; MCUA10210,A35526,A11F0 restored.
No crash, aborted request or I/O failure is recorded. This does not establish why
DMA could not resume after the no-receive interval. No other matrix nodes have
been measured. Do not interpret this as an eight-node or24-condition result.

The next registered source will collect one initial image per node before the
no-receive trials, retain all control/CCM/Mode/VSYNC evidence, and avoid another
DMA request in the same node after the interval. Images will be labeled initial
node images, not target-specific or end-of-eight-cycle images.
