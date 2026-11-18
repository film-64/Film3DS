# Batch22: first node complete; asynchronous CCM mismatch stops second node

Single verified v2 deployment;17 originals,895010 bytes, complete0 failed0.
Node0 (5fps, FD retained, coarse4) completed initial node image and all three
planned target trials in the same uninterrupted stream.128 observed8 cycles,
96 and160 each2; every VSYNC IFP read133/133/133/133. Fixed controls held.
No new DMA after the initial node image. Node1 (coarse528) prepared controls
successfully, then snapshot native2332=431 and later logical bytes1/173 combine
429. All reads returned success; manifestCCM_ALIAS_CHECK verified0 correctly
stopped the v2 source. No second-node capture or later node was completed.
SD byte-boundary journal writes separate these reads in time. Temporal CCM
change and torn-byte observation remain possible; mismatch alone does not prove
bad transport or prove the exact MCU update mechanism. Original controls were
verified restored. Strict report remains failed. No crash or DMA timeout here.

A future source must explicitly distinguish time-separated CCM observations
from stable transport/control proof and must not reinterpret failed reads as
values or pretend the CCM is an atomic snapshot. All originals and this strict
v2 behavior are preserved.
