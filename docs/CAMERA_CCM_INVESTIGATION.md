# CCM normalization bit6 isolation v1

Batch17 completed all twelve phase perturbations and HG ON/OFF/ON, with original
files and source sealed in `build/hardware/exposure-sweep/batch17-hg-isolation-v1`.
All writes immediately read128, then red changed to133 at a bracket around the
next 200ms frame boundary, including HG OFF. The CCM/Gamma snapshots could remain
unchanged while gain was repeatedly overwritten. This motivates testing a fixed
normalization input rather than assuming Gamma changes on every gain update.

This new standalone compute source `camera-ccm-normalization/v1`, its single
session and report are registered first in `tools/exposure_sweep_contract.py`.
No Vision LaneAdmission; explicit `standalone_hardware_diagnostic`; evidence in
local_trace only. Unknown/mixed scopes fail closed. No persistent runtime-internal
fields are added. Original HG source semantics and its historical report remain
unchanged; shared functions select between only these explicit registered sources.

## Controlled intervention

Three independent arms set A355 bit6 = 0,1,0, keeping bit5 Force Unity =1 and
HG live bit3 =0 throughout each measurement arm. Before each arm's Refresh, the
new A355 target is written and immediately checked with defined-control mask96.
Then the existing AWB-on Refresh normalizes the AWB output, AE/AWB are disabled,
HG is disabled, and final physical exposure/core/IFP values are written last.
No Refresh or repeated Receive occurs inside the streaming phase trials.

The exact meaning of bit6 on this MT9V113 firmware is a hypothesis. The primary
manufacturer evidence is MT9D111 SOC2010DS Table13, page78: AWB mode bit6 disables
CCM normalization, independently of bit5 Force Unity. This is a homolog device;
readback is not proof that every implementation detail transfers to MT9V113.
[Manufacturer document](https://download.kamami.pl/p562853-MT9D111_SOC2010DS.pdf)

The same four 25/75/125/175ms delays and 64-poll/4ms bounds as HG v1 are reused.
Unlike HG v1, the planned four-channel targets are96/128/160/128, respectively.
Each poll stops on the first value different from its own planned target. This
separates a reload value changed to128 from suppressed rewriting that leaves
96/160 intact; the128 trials alone cannot distinguish these mechanisms.
One initial DMA is completed and closed per arm. Eight strict full state groups,
including AE221F128, protect the controls. Readbacks remain observations in
range1..512; four successful128 writes do not imply the values stay128.

Read-only snapshots add A365..A36B (X0 and six left/right normalization
coefficients) to the previous98 values. Their MT9V113 logical addresses are
listed in [BlocksDS/libnds camera.h](https://raw.githubusercontent.com/blocksds/libnds/master/include/nds/camera.h).
This produces105 values before/after each arm. No normalization coefficient,
Gamma/CCM table, firmware, arbitrary RAM, clock or PLL is written.

All exits attempt original MCU and physical restoration. At most four Refresh
writes, ten A102 writes, seven A11F writes and five A355 writes (baseline force,
three planned mode targets, saved restore) are allowed by the registered plan.
There is no retry, fallback Refresh, hidden rewrite or reuse of failed frames.

## Interpretation and proof

A reversible reload value133→128→133, including96/160 probes, would support
bit6 dependence of the common gain; it would not prove an exact CCM arithmetic
formula or identify the firmware writer. If all arms still overwrite, this bit
alone is insufficient. Fixed controls, gate/mode readback, new VSYNC, DMA closure,
105-value snapshots, original CRC/SHA and restoration are required for a passing
evidence report. Changing HG and normalization together is excluded: HG stays OFF.

`tools/camera_hg_report.py` shares its bounded verifier with the separate
`tools/camera_ccm_report.py` entry. Sources cannot masquerade as each other.
Historical HG mask and count checks retain their old expectations. New CCM proof
requires exactly three target writes/readbacks and their ordering before Refresh;
per-arm state A355 expectations include the appropriate bit6. No admitted strings,
Vision matrix or Vision verifier are introduced or changed.

## Validation commands

```
CCM_NORMALIZATION_ONLY=1 ./build/probe-driver
HG_ISOLATION_ONLY=1 ./build/probe-driver
./build/probe-plan
python3 tests/camera_ccm_report.py
python3 tests/camera_hg_report.py
python3 tests/exposure_sweep_session.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
```

C tests cover normalization-dependent, independent, held and selectively suppressed gain models plus
snapshot, HG gate, VSYNC, control, restoration, bit6 write failure and ignored
bit6 readback. Python adds negative mode proofs, source masquerade and ordering
before Refresh to the existing boundary tamper cases. Historical C diagnostics,
ARM build/3DSX validation and both diff checks are also run.

The following Vision commands are not run because these files are absent from
Film3DS and no Vision runtime code changed:

```
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```
