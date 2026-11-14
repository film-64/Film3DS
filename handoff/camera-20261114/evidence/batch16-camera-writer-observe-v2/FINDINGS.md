# Batch16：IFP回到133，但Camera沒有對外寫回增益或MCU資料

2026-11-13。單次執行camera-writer-window/v2 + camera-writer-trace/v2。裝置與觀察器皆完整完成，Camera syscall catch已明確關閉、detach成功，裝置在收到RELEASE後恢復原設定。

## 結論與證據強度

在完整約87.104秒的觀察區間，Camera送出546個SendSyncRequest：278筆I2C與268筆pdn:c。1,092個entry/return stop全保留，278對I2C服務回覆均為成功。I2C的52次寫入精確分解為：

|類型|次數|內容|
|---|---:|---|
|ISP實體增益寫入|4|我們自己向device2的32D4/32D6/32D8/32DA各寫128一次|
|MCU讀取選址|48|向098C選擇A102/A103/A104/A355/A34E/A34F/A350/221F，各6次|
|MCU資料寫入0990–099E|0|沒有經此路徑修改MCU增益或控制變數|
|ISP寫回133|0|沒有觀察到Camera送出這類I2C寫入|
|其他感測器寄存器寫入|0|沒有其他直接、批次或連續資料寫入|

裝置在GO前四通道都嚴格讀回128；開始後第一個DMA完成，證明無pending Receive並關閉同一DMA事件，然後在追蹤中一次重寫四通道128。四個寫入請求、各自成功I2C reply及裝置call結果都保留。因syscall觀察有顯著暫停，立即讀回已是133；v2將這作為writer觀察，繼續要求三個確實前進的新VSYNC。三次完整快照均四通道133，整段沒有新SetReceiving或中途Refresh/Context切換。

**本輪實證排除的是：Camera透過本輪觀察到的對外I2C路徑，直接把IFP寫成133或透過MCU資料變數寫入重設它。這明顯支持Sensor內部MCU/IFP更新為主因。** 尚未定位具體內部函數或shadow，不能把「Sensor內部更新」與「已找到可修改shadow」畫等號。

範圍限制仍然明確：不是整條實體I2C總線監聽，不是所有process的全域追蹤；pdn:c操作、其他process或Sensor內部硬體latch與MCU程式之間仍需區分。本輪不把Camera所有動作或觸發關係一概排除。

## 觀察完整性

- 實際binary357692 bytes，SHA256=5c80239f537ce1bbe0b617d5a49d34490c2c515a5f8f4ce06f912cb014d7018c。
- 14份RAM原件807627 bytes，全數長度/CRC32/SHA256通過。裝置C源碼、主機契約/工具/測試源碼、實際3DSX與雜湊保存。trace/與C測試源碼另外封存在trace-and-test-sha256.json。
- 現機Camera debugger PID23，完整text86016 bytes SHA256=d599b6b685b1888b1e33c94ca54c4325719d371a697bb8b51b3481b16f5ead2f，與早前原件及六個入口指令逐一匹配。未盲用舊位址。
- I2C session handle0x50009由現機driver物件指標解析；另外268次對外請求全部使用0x5800A，134次caller LR=0x0010CBB1、134次LR=0x0010E591。依已驗證text中的載入/初始化程式，該路徑綁定pdn:c（0x00115004→0x0011D4D4）。沒有觀察到第三個對外session，避免漏掉Camera內另一個I2C session。service-binding-disasm.txt保存綁定反組譯。
- I2C讀取226次，包括32D4共31次，其餘三個IFP各7次、3012及四個core各6次、0990讀48次，另有背景364E輪詢96次：device1/2/4各32次。這證明背景服務仍在運作；那些是讀取，不能算成gain寫入。
- 正控制四筆IFP128寫入entry_id41/43/48/50，PC=0x0010BF8C、LR=0x0010FA3D，stack各128 bytes保留。所有I2C命令只有8（multi write16）及0xA（read16），沒有未知命令或遺失buffer；解碼同時支援command4與0xC，遇到未知命令不允許負向writer判斷。
- 追蹤下6組完整狀態，另GO前1組，共119筆；紅通道24筆連讀、4筆pre_GO嚴格128證明、4筆計畫內rewrite觀察讀回。完整狀態始終A102=10、A103=0、A104=3、A355=42、MCU AWB RGB=128、221F=128、3012=4、3056/3058/305A/305C=528。A355=42仍保持Force Unity；未知低位不賦予額外控制語意。
- WRITER_WINDOW_END complete1 restored1；NO_RECEIVE_WINDOW complete1 observations3；原件END complete1 aborted0 io_error0。通用啟動continuous preflight failures1保留，不冒充本輪來源的失敗或抹除。
- 觀察器complete/identity_matched/catch_disabled/detached/gate_released全部true，實際排空1個在途非同步stop封包，沒有重發命令、breakpoint、process memory寫入、Sensor RAM/ROM修改或patch。

## VSYNC與儀器擾動

|觀察|基線raw|最新raw|四通道|
|---|---|---|---|
|1|29527278957752|29527477903777|133/133/133/133|
|2|29543275150609|29543507866539|133/133/133/133|
|3|29558904469052|29559070053993|133/133/133/133|

每次均為清除事件後等待回傳0且時間嚴格前進。它們是三個新VSYNC觀察，不是三個連續影像幀；中間大量RPC/stop造成跳過多個幀週期。寄存器序列非原子快照，約87秒儀器窗口不能用來推論正常模式的覆寫延遲或寫入耗時。影像ROI只來自首個DMA，不冒充VSYNC影像。先前batch14的無debugger128→133時序原件仍獨立保存。

## Batch15失敗與v2的明確變更

Batch15/v1原件complete0，保持失敗，不重寫為成功。v1在追蹤暫停下要求immediate128，四通道讀回133，按契約於VSYNC前停止。新的v2先登記writer觀察用途：保留GO前嚴格128正控制、追蹤中的四次128寫入及成功服務回覆，讀回只作1..512原始觀察，其他曝光/AE/AWB/命令/新VSYNC條件不放寬。沒有改既有unity、no-receive或v1的128通過門檻。C測試在同樣延遲模型下證明v1拒絕、v2可完整觀察且calibration_validated始終false。

RSP收尾修正同樣先登記新trace/v2：在不重發原命令的情況下，最多排空8個非同步stop/output封包才讀真正命令回覆，並在catch關閉失敗時仍嘗試detach。本輪明確收尾成功。此kernel debug事件的register0不能直接當作IPC transport Result，v2保留raw register_r0，只以IPC reply word1及裝置call結果驗證成功；278筆reply均0。v1原欄位保持原樣並在其FINDINGS說明，沒有把非零CPU暫存器偷偷改為0。

## Runtime邊界分類與實作

獨立本地hardware diagnostic compute，明示standalone_hardware_diagnostic override，僅local_trace，不經Vision LaneAdmission。Film3DS/tools/exposure_sweep_contract.py先登記identity、writer-window/v1/v2、trace/v1/v2、session/v1/v2與report/v1/v2，再依註冊規則實作及測試。Vision CONTRACT_MATRIX與runtime verifier未改，未知CLI scope仍fail closed。

- tools/exposure_sweep_contract.py：同步READY/GO/DONE/RELEASE、單一連線、命令/等待上限、寫入/讀回/VSYNC證據、觀察器覆蓋與負向推論限制；sweep_contract.h由此生成。
- Probe calibration_scope.h、calibration.h、writer_window.h、no_receive.h：新明示scope/dispatch；已註冊的單次四通道rewrite策略以function參數傳入共用DMA/VSYNC流程。舊來源仍只傳strict128策略；新的觀察策略入口明確拒絕錯scope。沒有新增Probe持久欄位或隱藏補寫/重試。
- tools/camera_writer_identity.py：單次attach只讀全文、嚴格身份核對、finally detach。實機2.583秒完成，5項失敗/身份/unknown-source測試通過。
- tools/camera_writer_trace.py：Camera outgoing SendSyncRequest entry/return配對、service身份、64 words、bounded static buffer與caller stack、deadline120秒/3000 stops、明示最多8個inflight回覆證據追蹤。這個map只配對已發生IPC，不建立或排隊compute工作。
- tools/exposure_sweep_session.py：互斥writer-window/v2選項，單次部署及完整凍結回收。
- tools/camera_writer_report.py：device來源、原件CRC/SHA、新VSYNC、固定控制、預先128及寫入正控制、trace entry/return/stop proof、identity、未知命令拒絕、原設定恢復；passed只表示診斷證據完整，不等於unity或校準成功。
- 新主機runtime狀態attached/catch_enabled/running/stops/tls_cache/i2c_handle/inflight_by_thread/async_packets_drained及identity狀態已在Film3DS本地矩陣登記。Vision runtime_internal_fields.py、portable metadata、frontend及package未改。
- 測試檔：主機tests/camera_writer_identity.py、camera_writer_trace.py、camera_writer_trace_transport.py、camera_writer_report.py、exposure_sweep_session.py；Probe tests/probe_driver.c、probe_plan.c。舊測試未削弱。

## 已執行命令與結果

C、CLI及ARM（既有離線Linux/devkitARM容器；全部成功）：

```sh
make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=
WRITER_WINDOW_ONLY=1 ./build/probe-driver
NO_RECEIVE_ONLY=1 ./build/probe-driver
./build/probe-plan
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=camera-writer-window-v2
python3 tools/verify_3dsx.py FilmExposureSweep.3dsx
```

writer5個producer情境與協定錯誤/逾時/斷線檢查通過：正常v1、錯GO、無RELEASE、延遲下v1拒絕、延遲下v2觀察；NoReceive10組及CLI新舊來源/未知source拒絕通過。日誌../writer-window-v2-build-tests.log。先前同輪ISP_UPDATE_ONLY=1 ./build/probe-driver（9組）與EXPOSURE_SWEEP_ONLY=1 ./build/probe-driver（27組）亦通過，日誌../writer-window-c-regression.log。

主機90項測試全部通過：

```sh
python3 tests/camera_writer_identity.py              # 5
python3 tests/camera_writer_trace.py                 # 6
python3 tests/camera_writer_trace_transport.py       # 4
python3 tests/camera_writer_report.py                # 8
python3 tests/exposure_sweep_session.py              # 10
python3 tests/exposure_no_receive_report.py          # 9
python3 tests/exposure_isp_update_report.py          # 8
python3 tests/exposure_isp_report.py                 # 12
python3 tests/exposure_automation.py                 # 13
PYTHONPATH=/Users/a64/Documents/Codex/.tools/exposure-calibration python3 tests/exposure_sweep.py # 15
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
git diff --check
git -C build/worktrees/camera-deep-probe diff --check
```

4項trace transport以真實TCP/RSP端點覆蓋完整配對/buffer/收尾、identity不符不GO、在途T05/S02不重發命令、非零raw R0；日誌../writer-window-v2-transport-tests-final.log。10項session測試以真實loopback HTTP驗證單次部署、互斥scope及CRC/SHA回收，日誌../writer-window-v2-session-tests.log。

實機（已完成，沒有自動回退或重拍v1）：

```sh
python3 tools/camera_writer_identity.py 172.20.10.2 --gdb /Users/a64/Documents/Codex/.tools/film3ds-arm-gnu-13.3/bin/arm-none-eabi-gdb --expected build/hardware/exposure-sweep/writer-audit/expected-camera-identity.json --out build/hardware/exposure-sweep/writer-audit/live-identity-01
python3 tools/exposure_sweep_session.py 172.20.10.2 --writer-window --out build/hardware/exposure-sweep/batch16-camera-writer-observe-v2
python3 tools/camera_writer_trace.py 172.20.10.2 --expected build/hardware/exposure-sweep/writer-audit/expected-camera-identity.json --out build/hardware/exposure-sweep/batch16-camera-writer-observe-v2/trace
python3 tools/camera_writer_report.py build/hardware/exposure-sweep/batch16-camera-writer-observe-v2 --out build/hardware/exposure-sweep/batch16-camera-writer-observe-v2/writer-report.json
```

報告exit0、passed=true、calibration_validated=false；14份原件、C/host源碼與trace hash逐份驗證。實際啟動命令及版本由deployment.json與host-source凍結保存。

以下Vision命令未執行：當前Film3DS沒有對應檔案，亦未修改/呼叫Vision runtime執行邊界。

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

## 剩餘問題

實際Sensor內部寫入函數、Mode/HG/其他模組與133的上游來源尚未定位。A102仍保留其他已存在的控制位，AE/AWB關閉不等於整個MCU停用。不進行盲目133值搜尋、任意RAM/ROM寫入或停用整顆MCU；本輪未修改Sensor韌體。後續硬體控制研究應基於這個外部寫入負證據與既有穩定控制值，追內部更新來源；不能把本輪diagnostic pass作為曝光校準通過。
