# Batch20: full CCM bit6 contrast completed with persistent startup proof

## Acquisition

One camera-ccm-normalization/v3 deployment to192.168.0.14:17491, binary366452
bytes SHA25669bdf965c2cdaf29abe3f42ffb9dd126ad1cb066db16d9aa6029a8ea20b68ce1.
17 original files1062745bytes fully recovered and verified CRC32/SHA256. The
strict report passes with no errors. Calibration remains false. All three arms,
12 perturbations, six snapshots,132 successful logical-byte reads,66 derived
CCM words and six native2332-reference checks completed. Original MCU and five
physical controls plus four native IFP gains166/163/163/133 were restored/read back.
END complete1 aborted0 io_error0 captures32 failures1; retained generic preflight
failure is not hidden and is separate from the passing scoped diagnostic.

## Memory and exit evidence

298 ordered startup records were recovered, through BEFORE_FREEZE, in a34747byte
prefix of the persistent SD journal. No FAULT or APT event/error is present in
that prefix. Native SDK heap reservation95862784bytes (about91.4MiB), linear
reservation33554432bytes (32MiB). The arena29360128bytes (28MiB) and linear image
buffer614400bytes both explicitly allocated successfully. These are reserved heap
sizes, not a claim of free heap. v3 completed without the reported v2 exit.

This does NOT establish the cause of Batch19's screen flash/exit, nor prove a
memory fault was fixed. Batch19 has no recovered runtime evidence and remains
unknown. The added diagnostics can alter timing. No speculative attribution to
heap exhaustion, MCU failure or user action is made.

## Controlled result

All arms held HG/AE/AWB live driver bits off, A102=2 after the gate. Current
MCU AWB RGB128, AE221F128, coarse3012=4 and core codes528 were verified.
A355 measured42 →106 →42, i.e. control bit6 off/on/off while Force Unity remains
set. Before each arm its target34/98/34 was successfully read back before the
bounded Refresh. No Refresh, new Receive or Context switch occurred in timing
windows. Each arm had one completed/closed initial DMA, followed by new VSYNC
observations. The three saved ROIs are initial DMA images, not post-rewrite images.

Every arm writes four-channel targets96/128/160/128 in its four planned trials.
All12 writesets immediately read back exactly their target. Every final trial
state returns133/133/133/133. The bit6 candidate is therefore not sufficient to
stop gain reload or change its common133 value under these conditions.

CCM variables do change with the bit6 intervention. Raw signed row sums before
stream are272/278/269, then256/255/256, then271/276/269. This supports a real
CCM-path effect, independently of the unchanged physical gain reload. It does
not establish an exact firmware formula or exclude all possible matrix dependence.
The six normalization coefficients plus X0 remain32/175/128/87/130/134/130.

Timing stays tightly phase-associated: for25/75/125/175ms delayed rewrites,
first changed reads occur171–172/124/72/24–25ms after write completion, or
201–205ms after observed VSYNC return. These are read bounds, not exact internal
write times. Only5fps is covered, so frame event versus frame-synchronized timer
or hardware latch remains unresolved; no writer is identified.

## Byte transport and limits

All six native2332 versus logical-byte checks agree.36 individual FF byte reads
succeed. No derived word in this live run equalsFFFF; unit models cover FF/FF but
the prior Batch18 actual233E value remains unobserved. Therefore the sentinel
explanation for Batch18 is supported by archived code, not directly confirmed by
its failed read or by this batch's coefficient values. No failed read was accepted
as a value, and no physical memory/firmware patch was used. MCU control-variable
writes remain part of the explicit experiment, distinct from arbitrary RAM writes.

Known descriptive output defect: ccm-report.json arms.target serializes the last
trial target128 because of a local variable name reuse. It is not A102. The gate
header and every raw state are independently verified against2 before that local
variable is reused, so the pass/fail verdict is unaffected. The original report
is retained; diagnostic-summary.json derives live_sequence from actual state reads.
No post-hardware verifier change is used to make this batch pass.

## Governance and validation

Standalone diagnostic compute, explicit standalone_hardware_diagnostic override,
local_trace only, no Vision LaneAdmission. v3 source/session/report were registered
before implementation. Verifier adds bounded journal/lifecycle/byte-boundary proof;
prior v1/v2 verdicts and originals remain unchanged. New local-only boot fields are
registered in the Film3DS contract; no Vision matrix, verifier or field registry
changed.15 full C v3 scenarios plus bounded journal tests, prior C diagnostics,
15 v3/13 v2/11 v1/8 HG report tests and14 session tests passed. ARM/3DSX, header
and both diff checks passed. Exact commands and absent Vision pytest paths are
in EXPERIMENT.md. This hardware session and strict report also passed.

Remaining gaps: previous exit cause; gain133 upstream source and exact writer;
cross-frame-rate trigger discrimination; arbitrary gains held across at least
eight frames; remaining full exposure matrix. User has requested a bounded,
coherent adaptive matrix next, rather than separate one-condition deployments,
and enabled debugging/authorized direct subsequent execution.
