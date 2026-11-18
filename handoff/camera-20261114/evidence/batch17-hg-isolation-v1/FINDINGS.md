# Batch17: internal gain update remains after HG disable, phase locked to frame cadence

## Result

`camera-hg-isolation/v1` completed once, all three ON/OFF/ON arms, all twelve
trials. The strict diagnostic report passed with no errors; calibration remains
false. 16 original RAM files, 998291 bytes, verified by original CRC32 and SHA256.
All MCU and physical original settings were restored and read back. Device END:
complete=1, aborted=0, io_error=0, captures=32, failures=1. The retained generic
preflight failure is not hidden and does not belong to the scoped HG experiment.

Deployed binary: 364660 bytes, SHA256
`b552938cf9689b4c734ba6635466dc5edc658aab73314d97e1ab6d1cc8ce8200`.
The binary, full C source, host/test source, deployment arguments, report and
original receipts are sealed with this batch. No GDB observer was attached.

## Gain result and controls

All twelve four-channel writes succeeded and immediately read back 128 on all
channels. Every later completed trial state returned 133/133/133/133. The middle
HG-OFF arm held A102=2; the ON arms held A102=10. A103=0, A104=3, A355=58,
MCU AWB=128/128/128, 221F=128, coarse3012=4 and all core codes=528 remained
verified. Each arm used exactly one StartCapture and one initial Receive; DMA
completion and closure preceded trials, with no new Receive/Refresh/Context
change inside the streaming trial window. The initial DMA image is retained;
it must not be presented as an image captured after the later rewrites.

Clearing the HG driver bit alone is therefore not sufficient to stop the update.
This does not prove that all HG-related internal state or every firmware routine
was disabled; Preview HG A120 remained 1, intentionally read-only.

## Timing result

At 5 fps, each new VSYNC was verified by clear/wait plus a strictly advancing
raw service timestamp. The following are conservative host read brackets, not
an exact write time or hardware interrupt timestamp. The three values correspond
to first HG ON, HG OFF, second HG ON.

| Planned delay after observed VSYNC return (ms) | First changed read end minus completed rewrite (ms), ON/OFF/ON | Last unity read begin to first changed read end after observed VSYNC return (ms), ON/OFF/ON |
|---|---|---|
| 25 | 173 / 172 / 171 | 196–202 / 196–202 / 196–201 |
| 75 | 125 / 124 / 125 | 200–205 / 199–204 / 198–204 |
| 125 | 73 / 72 / 72 | 197–203 / 196–202 / 196–201 |
| 175 | 25 / 24 / 25 | 199–205 / 199–204 / 198–204 |

The first changed read occurred 201–205 ms after the observed VSYNC return,
and each conservative bracket overlaps the neighborhood of 200 ms. The remaining
wait decreases almost one-for-one as the planned write delay increases. This
strongly supports an update synchronized with the frame cadence, rather than a
fixed delay started by our I2C write. A free-running timer synchronized to frames,
a sensor MCU frame callback, and a hardware frame latch cannot be distinguished
by these observations. One frame rate and a finite twelve trials do not prove a
particular periodic writer. The report intentionally leaves timing_cause_identified
and writer_identified false. Raw VSYNC service units are retained without conversion.

## Upstream snapshots

98 sequential read-only values were captured before stream and after stop for
each arm. No table, CCM, MCU RAM, ROM or PLL writes occurred.

The first ON arm changed physical Gamma knees, A354 saturation 128→121, logical
CCM coefficients, AB17 average luma 84→0, 2B1B brightness metric516→768 and
3210 pipeline control2488→2472. Its AB08 dark level stayed4. The two 19-byte MCU
Gamma source tables and AB37 Gamma morph control3 stayed unchanged.

The middle OFF arm's entire 98-value snapshot was unchanged across its capture,
and the second ON arm also had unchanged snapshots after the first arm's settling.
Nonetheless each of their four gain rewrites still returned133 on the next frame.
This rules out *a change in these sampled Gamma/CCM settings* as a necessary event
for every overwrite. It does not rule out recomposing gain from fixed CCM values,
fixed cached state, or a not-yet-sampled upstream scalar.

After settling, current logical CCM raw values2332..2342 interpreted as signed16:
`410,-74,-74,-78,376,-28,-6,-100,368`; trailing words2344/2346=`37,61`.
Physical scale words32C0/32C2 stayed`14627/1828`; this batch does not establish
how those packed fields combine. Buffered AWB words2356/2358 remained`4224/3360`
even though current MCU AWB output was128. These fixed values are candidate inputs,
not identified shadow addresses or a demonstrated formula for133.

## Governance and validation

Standalone hardware diagnostic compute, no Vision LaneAdmission, explicit
standalone_hardware_diagnostic override; proof local_trace only. Registered HG
source/session/report were added before implementation in exposure_sweep_contract.
New HG verifier; historical verifier contracts preserved. No new persistent
runtime-internal fields. Eight C producer/failure scenarios and eight HG report
tests, twelve session tests, historical regressions (total100 Python tests),
ARM build, 3DSX validation, generated-header and both diff checks passed before
acquisition. The hardware session and strict CRC/SHA report passed here.

Vision runtime pytest paths are absent in Film3DS and were not run; no Vision
matrix/verifier/internal-field changes were made. Exact commands and limitations
are in EXPERIMENT.md and PREDEPLOY-VALIDATION.json. No runtime source was edited
while collecting or interpreting this batch.
