# Batch14：沒有新的 SetReceiving，ISP 仍於新 VSYNC 後再次覆寫

2026-11-13，no-receive-frame-update/v1，單次部署最新版 no-receive-frame-update-v1。產物354940 bytes，SHA256=50f4b81ecf0ddb837a6627ca3158ee7c15bd2bc92d99beb856af6d4a8d64dd5e。已保存實際3DSX、裝置端源碼、主機契約/報告器/測試與雜湊。15份RAM原件、860699 bytes，全部長度、CRC32、SHA256及源碼快照驗證通過。

## 決定性結果

|觀察|連續DMA對照|無新Receive|
|---|---|---|
|本組SetReceiving / StartCapture|3 / 1|1 / 1|
|首個DMA之後的一次重寫讀回|128/128/128/128|128/128/128/128|
|重寫後後續觀察|兩個DMA完成後全133|三個新VSYNC後均全133|
|中途Stop / Refresh / SwitchContext|無|無|

無新Receive組：WaitFrame=0、IsFinishedReceiving=true、成功關閉同一DMA事件handle後，才重寫四通道128並逐一立即讀回；完整after_rewrite快照亦全128。之後沒有SetReceiving、Start/Stop、ClearBuffer、Refresh或Context切換，直到第三次VSYNC後完成讀取才Stop。VSYNC handle獨立，清除/等待使用同一handle，最後成功關閉。

三個VSYNC各有「取得基線 → 成功清除事件 → 等待回傳0 → 最新時間大於基線」的完整證據：

|觀察|基線原始時間|最新原始時間|四通道post_vsync|
|---|---|---|---|
|1|26243556287585|26243756235647|133/133/133/133|
|2|26243756235647|26243956188610|133/133/133/133|
|3|26243956188610|26244156137671|133/133/133/133|

保留原始服務時間單位，不把它自行解釋成奈秒。裝置端time_ms記錄的三次WaitVsync分別為4003588174006、4003588174206、4003588174406，間距200ms，與本組5fps配置相符；有效新幀週期證據仍是清除事件後的新VSYNC與前進時間，而不是等待秒數。

**可排除「新的SetReceiving是每次覆寫的必要條件」。尚未識別寫入者。** 這不能排除Camera sysmodule獨立的幀事件處理、寄存器查詢IPC相關路徑或Sensor MCU/IFP內部更新，也不能證明存在可修改Shadow。沒有外部I2C總線監聽資料，不把內部MCU寫入當作已證實事實。

## 第一個VSYNC後的128→133邊界

單一紅通道32D4的八次讀值：

- 第1次新VSYNC後：128、133、133、133、133、133、133、133。
- 第2、3次：各八次全133。

第1次WaitVsync在time_ms=4003588174006返回，GetLatestVsyncTiming成功在4003588174007記錄；第一筆32D4=128的記錄時間為4003588174007，第二筆133為4003588174008。這是在無pending DMA、沒有新Receive的觀察區間內，直接捕捉到值從128變133的讀回邊界。時間戳是在RPC返回時記錄，毫秒解析度且含IPC/I2C延遲，不能據此聲稱硬體寫入需1ms，亦不能證明與VSYNC的精確相位鎖定或每幀必寫。四通道依序讀取，非原子快照；首個DMA後的混合128/133/133/133亦保持原樣。

## 固定條件、完整性與恢復

17組完整狀態、289筆讀取，另24筆紅通道連讀，共313筆全成功。所有完整快照：A102=10、A103=0、A104=3、A355=58、AWB R/G/B=128、221F=128、3012=4、3056/3058/305A/305C=528。A355只驗證已定義控制位5/6（mask96），未知低位保留；沒有新增AWB位元組合、Mode掃描或任意RAM/XDMA寫入。

對照三個DMA及無新Receive組的一個DMA都成功；後者影像只來自首個DMA，不冒充三次VSYNC的新影像。三次Refresh分別16、15、14 polls完成（兩組準備與最後恢復）。NO_RECEIVE_END complete=1 arms=2 restored=1；原件END complete=1 aborted=0 io_error=0。恢復3012=264及原始core codes 544/573/549/544，MCU A102=10/A355=26/A11F=0及原生IFP均有讀回證據。

通用啟動preflight連續擷取失敗仍如實留在END failures=1，沒有抹除；本來源獨立5fps對照三個DMA均成功，無新Receive的VSYNC也均成功。沒有重新部署、重拍、回退或替代資料。no-receive-report.json passed=true表示本輪有界診斷證據完整；calibration_validated=false，並非ISP任意值控制成功。

## Runtime邊界與變更

分類：獨立本地hardware diagnostic compute，明示standalone_hardware_diagnostic override，證據只在local_trace。先更新Film3DS的tools/exposure_sweep_contract.py CONTRACT_MATRIX，再進行實作、verifier與測試。新增no-receive-frame-update/v1、對應session/v1與report/v1。Vision LaneAdmission、Vision CONTRACT_MATRIX與其verifier沒有改動。

- tools/exposure_sweep_contract.py及生成sweep_contract.h：固定兩組、首個DMA完成/關閉證明、三次新VSYNC、8次紅通道連讀、313筆狀態讀取上限、兩次準備加一次恢復Refresh；事件失敗/時間未前進即無法判定，不允許sleep代替幀證據或重試。
- Probe的calibration_scope.h、calibration.h、main.c：新scope、dispatch及僅本來源Refresh上限3。isp_update.h抽出共用準備與基準函數，加顯式Mode讀取參數；舊來源仍保留Mode讀取，原有9組情境回歸通過。
- probes/exposure/no_receive.h：一次DMA、確認完成且關閉handle、一次重寫128、三次新VSYNC與讀取、停止及恢復。VSYNC handle/時間僅局部變數，沒有新增Probe持久runtime-internal欄位；沿用已在Film3DS矩陣登記的欄位。沒有隱藏queue、補寫、重拍或fallback。
- tools/exposure_sweep_session.py：互斥--no-receive，單次部署及有界凍結證據回收，來源與二進位雜湊保存。
- tools/exposure_no_receive_report.py：新增本來源verifier，驗證長度/CRC/SHA、身份、固定控制、原始寫入讀回、DMA完成/同handle關閉、事件順序/同handle/新時間、無新Receive呼叫視窗、VSYNC後採樣次序、ROI來源及恢復；缺證據只可報inconclusive。沿用舊state解析純函數，沒有放寬舊unity/verifier條件。
- tests/exposure_no_receive_report.py、tests/exposure_sweep_session.py；Probe tests/probe_driver.c、tests/probe_shim/3ds.h、tests/probe_plan.c：新情境、CLI來源及API故障注入。docs/EXPOSURE_SWEEP.md記錄流程與結果。

沒有新增runtime-internal持久欄位，Vision runtime_internal_fields.py、portable metadata、package及frontend都未改。

## 已執行命令與結果

Linux既有離線devkitARM容器（全部成功，C日誌../no-receive-c-tests.log）：

```sh
make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=
NO_RECEIVE_ONLY=1 ./build/probe-driver
ISP_UPDATE_ONLY=1 ./build/probe-driver
EXPOSURE_SWEEP_ONLY=1 ./build/probe-driver
./build/probe-plan
```

新C來源10組：DMA/VSYNC皆覆寫、僅DMA覆寫、均不覆寫、正值VSYNC timeout、過期時間、取得事件/時間/清除失敗、曝光漂移、最後Refresh恢復失敗；檢查事件回收與原設定恢復。舊ISP 9組及sweep 27組全通過。僅既有host shim的兩項unused函數warning，無新編譯錯誤。

ARM建置與3DSX驗證成功，日誌../no-receive-3ds-build.log：

```sh
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=no-receive-frame-update-v1
python3 tools/verify_3dsx.py FilmExposureSweep.3dsx
```

主工作樹65項Python測試全通過（9+8+12+8+13+15）：

```sh
python3 tests/exposure_no_receive_report.py
python3 tests/exposure_isp_update_report.py
python3 tests/exposure_isp_report.py
python3 tests/exposure_sweep_session.py
python3 tests/exposure_automation.py
PYTHONPATH=/Users/a64/Documents/Codex/.tools/exposure-calibration python3 tests/exposure_sweep.py
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
git diff --check
git -C build/worktrees/camera-deep-probe diff --check
```

新報告器負例包含隱藏Receive/Start/Stop/Context/Clear/Refresh/IFP補寫、假pending-DMA證明、錯event handle、偽造/缺失VSYNC時間、錯sensor/失敗讀取/提前讀取、未知source、原件被竄改；生命週期結束後cleanup可接受，但觀察視窗內操作不能混入。Session8項loopback測試全部通過（../no-receive-session-tests.log），只部署一次、互斥scope拒絕、回收CRC/SHA驗證；網路與VM呼叫使用已授權工具權限。

實機命令：

```sh
python3 tools/exposure_sweep_session.py 172.20.10.2 --no-receive --out build/hardware/exposure-sweep/batch14-no-receive-frame-update
python3 tools/exposure_no_receive_report.py build/hardware/exposure-sweep/batch14-no-receive-frame-update/recovered --out build/hardware/exposure-sweep/batch14-no-receive-frame-update/no-receive-report.json
```

單次採集與回收完成、verifier exit0；原件、first-DMA ROI及所有失敗計數均保留。

下列Vision命令未執行：當前Film3DS沒有對應檔案，未修改或呼叫Vision runtime邊界；沒有把硬體diagnostic誤接Vision scheduler。

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

## 尚未測試／剩餘風險

未監聽外部I2C總線、未追蹤Camera CXI幀事件處理、未讀任意MCU RAM、未定位或修改真正Shadow。三次VSYNC觀察期間只重寫一次，不能推論每幀必然重寫。IPC/I2C讀取與事件API仍經過Camera service，無法單憑這組結果把內部Sensor與sysmodule分開。下一步研究應圍繞兩條寫入路徑的證據；不再把新Receive當必要觸發，也不盲搜133或宣稱整顆ISP可控。

API原始來源（本輪已核對安裝SDK與官方原始碼）：
https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/services/cam.h
https://github.com/devkitPro/libctru/blob/master/libctru/source/services/cam.c
