# Batch12：曝光保持4，獨立的物理ISP共同增益仍回到133

2026-11-13，來源exposure-sweep-post-refresh-manual/v1，Sensor1、5fps、單一S13。13份原件、828953 bytes，CRC32/SHA-256/長度已全部重算核對，部署source hashes也與保存源碼一致。二進位343080 bytes，SHA256=e08a7d212c23fbd288383c63d1c0e3cc50ae3db67f2d51629918055013f00619。

## 決定性結果

Refresh完成後MCU AWB為128/128/128；再關閉Preview AWB與live AE/AWB，讀回A11F=0、A102=10。其後才寫入3012=4、S13 core codes與四個物理ISP=128，立即讀回全部成功。採集期間沒有再次Refresh；calls.csv的完整順序已由verifier核對。設定Refresh與恢復Refresh均完成，restored=1。

|frame0邊界|3012|IFP R/Gr/Gb/B|
|---|---|---|
|before_arm|4|128/128/128/128|
|after_arm|4|128/128/128/128|
|after_start|4|128/128/128/128|
|after_wait|4|128/128/133/133|
|after_stop|4|133/133/133/133|
|8個settle完成後，frame8 before|4|133/133/133/133|

40組邊界、520筆讀取全部成功。A102=10、A103=0、A104=3、A355=58、MCU AWB=128/128/128、221F=128、3012=4均不變。寄存器是依序讀取，不是原子快照；混合通道值不能拿來推導同一瞬間的全通道狀態。

本輪對應使用者提出的第二種結果：曝光保持，ISP仍恢復共同增益。與batch11的3012=264相比，本輪關閉AWB後曝光保持手動值；但不能單獨證明是AWB直接寫曝光。IFP共同倍率133/128=1.0390625仍不能由實測221F=128解釋。優先追查ISP共同增益合成/重載路徑，寫入者與來源尚未確定。

注意上一輪batch11也已先Refresh再寫物理寄存器；不能將兩輪差異描述成單純修正Refresh與3012的先後順序。本輪新增差異是AWB歸一後再次停用driver、關閉Preview AWB、強制最終物理寫入順序，並加入3012邊界讀回。

phase5在frame8正式擷取前因IFP=133停止，沒有S13正式測量幀或有效校準模型。capture_complete=false、calibration_validated=false、SWEEP_END complete=0/restored=1均保留。初版衍生報告把缺失幀的None狀態誤算成狀態切換；verifier已修正並新增負例，最終報告為isp-report-final.json。缺失證據仍拒絕通過，沒有放寬IFP或曝光要求。

## Runtime治理與變更檔案

- 分類：獨立本地硬體diagnostic compute，明示standalone_hardware_diagnostic override，不經Vision LaneAdmission；證據僅local_trace。
- 先改tools/exposure_sweep_contract.py，註冊manual compute/session及report/v3；保留舊來源契約。
- camera-deep-probe/probes/exposure/calibration_scope.h：獨立CLI scope與未知scope拒絕。
- camera-deep-probe/probes/exposure/main.c：新增3012邊界讀取、寫入入口拒絕採集中Refresh。
- camera-deep-probe/probes/exposure/sweep.h：歸一驗證、關閉與讀回、手動階段、退出恢復。
- camera-deep-probe/probes/exposure/calibration.h：只在新scope中，先寫曝光/core codes，再寫IFP並立即讀回。
- camera-deep-probe/probes/exposure/sweep_contract.h：由矩陣產生的新來源定義。
- tools/exposure_sweep_session.py：互斥的--post-refresh-manual，單次部署。
- tools/exposure_isp_report.py：八項MCU加3012/IFP、實際呼叫順序、禁止中途Refresh的proof驗證；缺失狀態拒絕但不虛構切換。
- 新本地內部欄位sweep_manual_locked已在Film3DS矩陣註冊。Vision CONTRACT_MATRIX、runtime_contracts.py、runtime_internal_fields.py及portable metadata均未改。
- docs/EXPOSURE_SWEEP.md：更新流程與實測。

## 測試與命令

測試修改：camera-deep-probe/tests/probe_driver.c、tests/probe_plan.c，以及主工作樹tests/exposure_isp_report.py、tests/exposure_sweep_session.py。

- 離線Linux/devkitARM：make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=；EXPOSURE_SWEEP_ONLY=1 ./build/probe-driver：27組情境通過；./build/probe-plan：通過。涵蓋歸一失敗、關閉失敗、曝光漂移、ISP漂移、恢復失敗及鎖定後拒絕Refresh。
- make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=post-refresh-manual-v1；python3 tools/verify_3dsx.py FilmExposureSweep.3dsx：通過。完整日誌../post-refresh-manual-c-build.log。
- python3 tests/exposure_isp_report.py：12項通過，包括真實C producer順序、假造中途Refresh拒絕、曝光4強制、錯source/identity、缺失幀不虛構狀態切換。
- python3 tests/exposure_sweep_session.py：5項loopback HTTP測試通過。
- python3 tests/exposure_automation.py：13項通過。
- PYTHONPATH=/Users/a64/Documents/Codex/.tools/exposure-calibration python3 tests/exposure_sweep.py：15項通過。
- python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check：通過。
- 兩個工作樹git diff --check：通過。
- python3 tools/exposure_sweep_session.py 172.20.10.2 --post-refresh-manual --out build/hardware/exposure-sweep/batch12-post-refresh-manual：採集與原件回收完成。
- python3 tools/exposure_isp_report.py build/hardware/exposure-sweep/batch12-post-refresh-manual/recovered --out build/hardware/exposure-sweep/batch12-post-refresh-manual/isp-report-final.json：exit2，正確拒絕IFP不等於128及不完整測量。

Vision以下三條未執行，因Film3DS無對應檔案，且此次未呼叫或修改Vision執行邊界：

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

剩餘風險：尚未定位物理共同增益寫入者；沒有S13正式測量影像、完整矩陣或光度模型驗證；穩定的抽樣狀態不能排除兩次讀取之間的瞬間狀態變化。
