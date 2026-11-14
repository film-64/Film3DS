# Batch18: first CCM control complete; disabled arm stopped at a signed-word read

One deployment to192.168.0.14:17491 of camera-ccm-normalization/v1, binary366540
bytes SHA25636f980be2285c82feae99fc58b17a1deb3a46f06126ad46a0ff8e43ff729e617.
14 original files859424bytes recovered completely; original CRC32/SHA256 verified.
No RAM failure or I/O error. All saved MCU and physical settings restored/read back.

The strict report correctly returns passed=false, conclusion=inconclusive. Only
first normalized arm completed four trials. Planned targets96/128/160/128 each
immediately read back on all channels, then returned133 after a new frame.

New raw MCU coefficients A365..A36B were acquired before and after that first arm:
32,175,128,87,130,134,130. These are observations, not a demonstrated formula133.

Second arm: A355 target98 read back98 before Refresh; after successful Refresh
A355106 retained control bits5/6. AWB RGB128, AE221F128, A1022 after HG gate,
coarse4, core528, IFP128 all verified before streaming. During the required
before_stream read-only snapshot, 233E returned C9602FEF, value logged0 (invalid,
not a zero coefficient). This is the sole failed register read. It happened
before StartCapture in arm1, hence there are no disabled-arm trial results and
no third return-control arm. The partial signed CCM rows must not be passed off
as a completed bit6 experiment.

An archived Camera sysmodule audit shows physical register read10A618 rejects
FFFF at10A64C–10A65A, and MCU read108574 calls that physical read on0990. A signed
CCM coefficient=-1 could therefore hit this existing sentinel restriction. This
is an explanation to test, not confirmation that the current233E value wasFFFF.
No failed result is reinterpreted as a measured value. No code or service patch,
retry, skipped coefficient or fallback was used in this source.

END complete0 aborted0 io_error0 captures30 failures1; HG_END complete0 arms1
restored1. Original sequence/AWB/preview controls and five exposure/core values,
plus all four original IFP values were verified restored. The frozen RAM is left
on device for user-controlled exit. This v1 binary/source/report remains immutable;
a different transport contract requires a new source/version.

Runtime classification: standalone hardware compute with existing explicit
standalone_hardware_diagnostic override; local_trace only, no LaneAdmission.
This acquisition made no matrix/verifier/internal-field/code changes. It used
the previously tested contract and binary; session command and strict report were
run, with successful recovery and an intentionally failed completeness verdict.
Vision pytest paths are absent and no Vision runtime was changed. Remaining risk:
actual signed coefficient, byte alias behavior, bit6 effect and writer identity
are not yet established.
