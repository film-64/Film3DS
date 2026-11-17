# 機內正式 RAW 採集自動化（2026-11-17）

使用者回報 r4 無法採集 RAW；既有桌面測試不構成實機採集成功證據。

## 邊界與實作

`film-raw-capture-automation/v1` 是顯式啟動的機內診斷 compute，入口為 NetLoader `f3rawqa=<32 hex token>`；不使用 LaneAdmission，也沒有 admission override。只執行一次 Native AE 案例：正式 camera open → 15fps → 正式 Native AE 選擇 → 正式 preview start/poll 收到三幀（總上限 5 秒）→ 正式 `film_camera_capture_raw` 一次。沒有另一個診斷 sensor driver、失敗後重試、YUYV 冒充 RAW 或 ROLL 目錄變動。

成功必須符合正式 transaction 的寄存器驗證、RAW low2 驗證、還原和原生恢復幀，並在診斷獨立目錄存 F3S 後重讀、比較全部原始 bytes。相機關閉後才回傳完整 trace 與 RAW。失敗只回傳失敗結果及 trace；主機不能把無 payload 的 ok、錯 token、錯來源 IP、截斷／無效／全常數 RAW 當作成功。

新增 `film-raw-capture-automation-deploy/v1` 為 side-effect 邊界：先檢查 frozen manifest、build receipt、ARM11/ROMFS，監聽回傳，再以官方 3dslink 向指定 IP 單次傳送。保留所有失敗 receipt；再次傳送必須有使用者接收端就緒回覆、相同 binary/manifest，禁止隱藏重試。

正式 RAW transaction 增加 local-only phase 與失敗寄存器的 expected/observed 記錄；adapter 記錄 idle、AE/AWB、DMA 接收各步 Result，不改既有寄存器判定和收幀順序。新增本地 runtime-internal 欄位 `raw_qa_token`、`raw_qa_phase`、`raw_trace`，不寫圖片 portable metadata。`CONTRACT_MATRIX` 和生成 header 已更新；沒有修改 Vision verifier，也沒有降低 fail-closed。

## 驗證

- `python3 tests/raw_capture_qa.py`：5 tests passed，含 fake proof/token、成功必須有有效 payload、失敗不冒充成功、禁止無就緒重傳／換 binary。
- `make test-raw-darkroom CC=/usr/bin/cc CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS='-L/usr/local/lib'`：RAW transaction、RAW ISP、暗房、ROLL 和 8 export tests 全部通過；新增失敗寄存器 expected/observed 與 token tests。
- `python3 tests/camera_ui_package.py`：8 tests passed。
- `python3 tools/camera_runtime_contract.py --check`：通過。
- 離線 pinned devkitARM 容器 `python3 tools/camera_ui15_package.py build --out /src/build/raw-capture-automation-20261117-r5`：通過，含 ARM11 JPEG、3DSX 和 ROMFS 核驗。
- 未執行 `pytest tests/test_runtime_contract_boundaries.py -q`、`pytest tests/test_runtime_projection.py -q`、`pytest tests/test_metadata_runtime_package.py -q`：此工作樹為 Film3DS，這三個 Vision 測試及對應 Python runtime 模組不存在。

## 實機證據

r5 binary SHA256：`a44050bab8ca752206926a6b5a597db1a20101d5ac30191d69b7acf2199b2a40`。

第一次向舊 IP 172.20.10.2 傳送：官方 3dslink 回覆 Invalid response，未確認啟動，保留 `build/raw-capture-automation-20261117-r5/device-run/upload.log` 和 `rawqa-failed-01.json`。

使用者提供 192.168.0.46:17491 並明確表示可燒錄後，第二次傳送同一份 binary，證據位於 `build/raw-capture-automation-20261117-r5/device-run-02/`。

第二次傳送在 TCP connect 階段失敗，官方 3dslink 記錄 `Connection to 192.168.0.46 failed`，沒有開始 payload 傳送。只讀網路檢查顯示該 IP 經 en0，但 ARP 為 incomplete。尚未取得實機回傳，不能宣稱 RAW 修復或實機成功。手動模式、完整 ROLL/暗房/UI 操作尚未由本自動化覆蓋。

## r7：沿用 run05 的布林 IPC 解碼

SD 原始 trace 已實際讀回（`build/raw-capture-automation-20261117-r5/sd-recovered/trace.txt`，2196 bytes，SHA256 813af53c9599273c04ee7fe93abcbb15f771870e15c09b5ff200f011ad1978dd）。預覽三幀及原生 gate 寄存器驗證成功，但 StopCapture/ClearBuffer 成功後 idle 始終不能證明停止，未進入 RAW 切換。

邊界分類：RAW 相機狀態讀回的 verifier；不新增 compute 路徑或重試。先登記 `film-camera-bool-reply/v1`，再將 run05 的 reply command/normal word 驗證與 mask8-before-bool 解碼接入 RAW 的 busy/finished/AE/AWB getters；先複製 IPC reply，再進行任何日誌／其他服務呼叫。輸出完整 header、原始 word、transport/service Result 供實機核對。這是目前失敗現象的候選修復，未取得實機資料前不宣稱因果或修復成功。

CONTRACT_MATRIX 和生成 header 已更新。Verifier 變更為先驗證完整回覆及低8位0/1，再產生布林值；不是跳過 busy 或放鬆 idle 條件。本次沒有新增 runtime-internal struct 欄位；新增資料僅寫本地診斷 trace。圖片 metadata 不變。

新增 `tests/hardware_transport.c` 覆蓋高位填充值的 false/true、低8位非法值、不足 normal words、錯命令、未知來源命令及 service error；`make build/hardware-transport-test CC=/usr/bin/cc CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS='-L/usr/local/lib'` 與 `./build/hardware-transport-test` 通過。`./build/raw-app-test` 既有回歸通過；`python3 tests/raw_capture_qa.py` 五項通過；registry header check 通過。Vision 三條 pytest 仍因本工作樹不存在相應模組而未跑，原因與前述相同。

在線回收保持在程式內，r6/r7 有即時 LIVE 階段及顯式 SD trace recover 模式；後續不使用 FTP。r7 部署及實機風險以 frozen package 的 receipt、device-run/trace.txt、report.json 為準；手動曝光與完整 UI 路徑未由單一 Native AE 案例覆蓋。

## r8 實機結果及 r9 傳輸修正

r7 建置因 SDK enum 名稱錯誤失敗（RS_INVALIDRESULT，已更正為 RS_INVALIDRESVAL）；未部署。r8 建置及 ARM11/ROMFS 驗證成功，binary SHA256 0eb6cb8825aadc2125979a360efb412a3ddcebef7f289faca98a30291470ddf9，已實際送入 192.168.0.48。

`build/raw-capture-automation-20261117-r8/device-run/trace.txt` 在線回傳證明：idle stopped=1、兩個 RAW settle 及一個 retained RAW 成功、transaction=ok、dma_quarantined=0、camera_live=1，capture bytes=614400，F3S source_roundtrip=ok。這證明本次 Native AE 實機採集及還原成功；不等同完整 UI/手動模式驗證。完整 RAW body 接收逾時，不能宣稱資料完整回收。

r9 只修改既有在線回傳邊界：先在矩陣定義 16384-byte chunk 與 partial evidence，再將機內大 fwrite 改為分塊 flush；主機用 read1 分塊並同步寫 capture.bin.part 和進度，只有全部長度與 low2 驗證後才改名為 capture.bin。沒有傳輸重試、重拍或 FTP fallback。新增 runtime-internal 診斷欄位 bytes_received（僅 local progress）；原圖 metadata 不變。`python3 tests/raw_capture_qa.py` 五項通過。

## r9/r10 最終部署觀察

r9 成功部署；再次確認 RAW transaction=ok、camera_live=1、source_roundtrip=ok。主機僅收到 4056/614400 bytes，完整資料傳輸仍失敗，partial 及 progress 已保存，沒有標示完整回收。

r10 在矩陣新增 side-effect `film-online-evidence-send/v1`，既有發送 callback 每次 send 限制4096 bytes（沿用 run05 RAM_SEND_CHUNK），不新增重試／重拍／新佇列。`python3 tests/raw_capture_qa.py` 六項通過，新增中斷保留實際 received bytes 的測試。r10 離線 SDK、ARM11、3DSX/ROMFS 建置核驗成功，使用者確認就緒後已部署到192.168.0.48:17491。

r10 在線收到 LIVE 至 after_close 及部分正式採集 trace，但連線再次 reset，沒有完整 RAW body。4096-byte 上限未解決在線傳輸問題，不能聲稱在線完整回收完成；後續需要定位現有 socket 發送失敗原因，不得繼續盲目更改 chunk 大小。既有 busy 解碼修正已在 r8/r9 證明完成 RAW 採集及存檔重讀；完整正常 GUI/手動模式仍未本輪驗證。所有 receipt/partial trace 均保留，未使用 FTP。

本次最終變更：matrix/生成 header 及 bool reply verifier 已改；runtime-internal 僅本地 bytes_received 進度欄位增加。測試命令和 Vision 三條未跑原因見前述。沒有更改 portable 原圖／recipe 欄位。
