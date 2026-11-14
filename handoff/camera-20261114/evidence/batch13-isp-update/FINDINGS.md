# Batch13：同一串流內重寫128後，ISP再次回到133

2026-11-13，exposure-isp-update/v1。17份原件、1026239 bytes，CRC32/SHA-256/長度及保存源碼雜湊全部驗證。最新版3DS產物349724 bytes，SHA256=04a87d000be19504f730379cc76080fc11e7e954aa509d447ecba5bbb30cbcc0。

## 結果

|模式|初始IFP四通道|首次已讀到的完整覆寫值（before_rewrite）|一次重寫後立即讀回|後續獨立新幀（frame2 after_wait）|
|---|---|---|---|---|
|restart|96|133/133/133/133|128/128/128/128|133/133/133/133|
|restart|128|133/133/133/133|128/128/128/128|133/133/133/133|
|restart|160|133/133/133/133|128/128/128/128|133/133/133/133|
|continuous|128|133/133/133/133|128/128/128/128|133/133/133/133|

每個arm都先重新執行歸一Refresh、關閉Preview與live AWB，再寫初值；同一arm內重寫後沒有Refresh/Context切換。Restart三組各有三次StartCapture。Continuous只有一次StartCapture，三個DMA幀均成功完成，重寫後沒有Stop/Start，frame2的DMA是在重寫完成後才arm。

共64組狀態、1088筆讀取與12份Mode快照、228筆讀取全部成功。固定控制：A102=10、A103=0、A104=3、A355=58、AWB RGB=128、221F=128、3012=4、四個core code=528。19個已知Mode變數在所有快照都相同，沒有讀到十進位133；這不是完整Mode空間或MCU RAM搜尋，不能排除其他地址、編碼或計算產物。

初次after_wait有混合通道值，後續before_rewrite已四通道全133；所有讀取都是依序進行，不能視為原子快照。關鍵是已先觀察到全133，再成功重寫全128，之後同一串流又回到全133。嚴格一次性載入、或必須重新StartCapture才覆寫，都不足以解釋本輪結果。

尚不能斷言每一幀必然寫入：只作了一次計畫內重寫。也尚未識別寫入者。連續接收仍呼叫SetReceiving，且下一個DMA先於診斷讀取arm，不能單憑本輪把MCU幀更新與camera service/SetReceiving的副作用分開；frame2另提供重寫後新幀確認。

全部四組、12個診斷DMA幀、最後ROI原件與恢復證據完整。ISP_UPDATE_END complete=1 arms=4 restored=1；原件END complete=1 io_error=0。通用啟動前測的continuous逾時仍保留在END failures=1，沒有抹除或視為新診斷幀；本輪5fps連續arm三幀均具獨立成功DMA證據。

最終報告isp-update-report-final.json passed=true只表示有界診斷證據完整，不代表ISP=128或校準通過；calibration_validated=false。第一版衍生報告誤把採集後相機cleanup Stop計入continuous arm；已先登記正確呼叫視窗，再新增cleanup允許及中途Start/隱藏rewrite拒絕反例。初版報告保留，沒有修改原件。

## Runtime治理與變更

分類：獨立本地硬體diagnostic compute，明示standalone_hardware_diagnostic override；不經Vision LaneAdmission，證據僅local_trace。先更新Film3DS/tools/exposure_sweep_contract.py，再實作、verifier、測試。新compute/session/report來源均註冊。未新增任意RAM/XDMA/Mode寫入、重拍、一般補寫或隱藏queue。

固定四個arm：restart96/128/160、continuous128，各三幀；frame0後一次IFP=128是計畫內擾動，不是重試。每arm重新歸一，總Refresh最多5次（4次準備+1次恢復）；A102/A11F最多各9次，A355兩次。連續接收只允許一個明示的pending DMA，frame1可與重寫重疊，使用frame2確認重寫後新幀。

變更檔案與契約：
- tools/exposure_sweep_contract.py：新來源、四arm、狀態/Mode讀取白名單、寫入與命令預算、報告呼叫視窗。
- camera-deep-probe/probes/exposure/isp_update.h：新來源實作，固定控制和DMA失敗即停止恢復；物理IFP變動只作本來源的觀察資料，不改既有unity測試。
- camera-deep-probe/probes/exposure/main.c：僅新scope的Refresh預算5，其餘仍2。
- camera-deep-probe/probes/exposure/calibration_scope.h、calibration.h、sweep_contract.h：註冊CLI scope、dispatch及生成常數。
- tools/exposure_sweep_session.py：互斥--isp-update，單次部署與回收。
- tools/exposure_isp_update_report.py：原件完整性、固定arm/frame、控制值、原始寫入與讀回、同串流/重啟呼叫順序、Mode與ROI證據、恢復驗證；觀察結果與校準驗證分開。
- 測試：主工作樹tests/exposure_isp_update_report.py、tests/exposure_sweep_session.py；camera-deep-probe/tests/probe_driver.c、tests/probe_plan.c。
- docs/EXPOSURE_SWEEP.md：新增流程及結果。

沒有新增runtime-internal欄位，沿用已在Film3DS本地矩陣註冊的狀態。Vision CONTRACT_MATRIX、runtime_contracts.py、runtime_internal_fields.py及portable metadata均未改。

## 已執行驗證

- 離線Linux/devkitARM：make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=；ISP_UPDATE_ONLY=1 ./build/probe-driver：9組情境通過；EXPOSURE_SWEEP_ONLY=1 ./build/probe-driver：既有27組通過；./build/probe-plan：通過。日誌../isp-update-c-tests.log。
- C情境包含逐幀覆寫、僅首次覆寫、不覆寫、continuous正值timeout、曝光漂移、Refresh設定/恢復失敗、Mode讀取失敗、AWB未歸一，事件handle回收及原設定恢復均檢查。
- make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=isp-update-v1；python3 tools/verify_3dsx.py FilmExposureSweep.3dsx：通過。日誌../isp-update-3ds-build.log。
- python3 tests/exposure_isp_update_report.py：8項通過，包括額外Start、隱藏IFP補寫、未知source、錯identity、雜湊失敗與生命週期cleanup視窗。
- python3 tests/exposure_isp_report.py：12項通過。
- python3 tests/exposure_sweep_session.py：6項loopback HTTP測試通過。
- python3 tests/exposure_automation.py：13項通過。
- PYTHONPATH=/Users/a64/Documents/Codex/.tools/exposure-calibration python3 tests/exposure_sweep.py：15項通過。
- python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check：通過；兩工作樹git diff --check：通過。
- python3 tools/exposure_sweep_session.py 172.20.10.2 --isp-update --out build/hardware/exposure-sweep/batch13-isp-update：單次部署、全部採集與回收完成。
- python3 tools/exposure_isp_update_report.py build/hardware/exposure-sweep/batch13-isp-update/recovered --out build/hardware/exposure-sweep/batch13-isp-update/isp-update-report-final.json：exit0，診斷證據完整，calibration_validated=false。

以下Vision命令未執行：Film3DS無對應檔案，亦未修改或呼叫Vision執行邊界。

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

## 研究依據與未解問題

MT9T111原廠開發指南第87頁確實描述Mode shadow registers及初始化/Context/Refresh套用機制：https://files.niemo.de/aptina_pdfs/MT9T111_developer_guide.pdf#page=87 。這是同系列機種，不能證明本N3DS MT9V113的32D4–32DA具有相同影子映射。

MT9V113原廠資料第31頁列Mode Driver ID7與獨立變數空間：https://files.niemo.de/aptina_pdfs/Image_sensor_1slash11-inch_SOC_component_data_sheet.pdf#page=31 。本輪Mode唯讀白名單依GBATEK：https://mgba-emu.github.io/gbatek/#dsi-aptina-camera-variables-mode-modecontext-mcu27xxh 。未使用未核實的XDMA/RAM寫入方法。

仍需區分MCU持續更新、接收服務的增益套用，以及其他ISP/HG/CCM計算來源；尚未找到可擾動的真正shadow地址。不進行任意RAM擾動或固件修改，也不把查找數值133當成來源證明。
