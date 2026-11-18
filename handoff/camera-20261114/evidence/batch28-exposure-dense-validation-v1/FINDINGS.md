# Batch28: independent fixed-model validation

One deployment of the verified latest Dense/v1 binary.74 points/444DMA/296ROIs,
313 originals9,996,130bytes complete; strict capture report passes and restoration
proof is present. Host HTTP download hit EOFError, not a capture/application
failure. Original failed status/log are retained; one explicit read-only resume
verified the same frozen index/run, partial segments and all CRC/SHA. No redeploy
or measurement repeat. Device remains in frozen RAM UI after camera closure.

Frozen Batch27 model SHA256:
5c7ef4b27946cacdbee8b89a68bd69fc58d86a317d236bc4f4e5b7e4a6ceb19d
No new curve/gain fitting. Only the predeclared reference_start image pair gives
one illumination offset. External approximate readings never correct residuals.

15fps passes against the fixed model:11 coarse points/22images max0.02641EV;
18gain points/36images max0.02026EV; reference_end2images max0.00034EV.
Reference-start offset+0.25988EV is a nuisance brightness offset, not measured
absolute meter drift. Out-of-domain low/high coarse conditions stay unmapped.

5fps is rejected before response acceptance: MCU CCM2332 changes432->430,
2334 changes65455->65456 from before_stream to after_stop. Native2332 windows
and byte reconstruction agree at each endpoint; snapshot is not atomic.
Diagnostic calls contain no writes to these MCU variables. Packed physical CCM
32C4..32D2 was not sampled, so neither IFP application timing nor writer is known.
Do not reinterpret this rejection as successful complete configuration control.

Both fixed-model-validation.json and final.json are retained with identical
profile outcomes. Final additionally records verifier source SHA and rejects
string-valued fake pass proofs. The numerical limits, coefficients, domains,
partition and validation samples are unchanged.

The user adjusted screen brightness; central reference before approximately
ISO100/f2.8/1over60 slightly short; after qualitatively unchanged. Approximate
fractional offset is not inferred. Screen/center protocol is recorded separately.
FD is off in this manual matrix. Full RGB dark response, optical shutter time,
standard ISO and FD-on manual control are not established.

Next control evidence: actual packed CCM versus MCU words during first-frame
settling and gain transitions. Do not start by arbitrary coefficient/firmware
writes or assume small MCU differences reached the pixels.
