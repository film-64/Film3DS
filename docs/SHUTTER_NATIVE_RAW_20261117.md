# 正常 App 按鍵拍攝與 RAW 暗房（2026-11-17）

## 要求與實作

Camera 的 A／R／拍攝觸控使用同一個正式入口。REAR1 按快門時鎖住當前 Native AE 或已驗證的手動曝光；保留鎖定後既有 native gate 的 YUYV 幀，再以相同 coarse、fine、四通道 gain 旁路採集 RAW。既有寄存器 hold 在兩種格式幀前後驗證，不另設固定曝光，不新增採集幀。還原原生狀態並完成原生恢復幀後，既有主迴圈重新啟動預覽。

先把原始 RAW 寫為 `rolls/ROLLnnnn.F3S`，重讀校驗；再以同次曝光的原生 YUYV 走既有 neutral/JPEG 流程生成 `previews/ROLLnnnn.JPG`。原有44-byte F3RI 索引綁定 RAW 指紋及 JPEG CRC。JPG 生成不讀 RAW 解碼器；ROLL 只讀 JPG 和索引。RAW 留到進暗房後才讀取、解碼。JPG 失敗保留已保存的 RAW。

本次不新增或改寫回收工具：原有 `ram_log.h`、`ram_http.h`、`ram_transport_contract.h` 原樣引用，來源與 SHA 見 `include/diagnostics/existing_ram/provenance.json`。正常 App 的 SELECT+START 只把選中照片的既有 SD 原件/JPG/索引/日誌凍結為4MiB上限快照，呼叫原有 FILMRAM HTTP5000 服務；主機使用原有 `retrieve_probe_ram.py` 及其 frozen-resume-v2。不使用 FTP，不重拍，不把回收成功當作採集 admission。

## Runtime 治理

- 分類：正式相機 shutter compute、native-JPEG pair compute；照片保存沿用單一既有 worker；凍結證據服務為 side-effect/read-only，沒有相機 compute。
- 先更新 CONTRACT_MATRIX：`film-camera-shutter-pair/v1`、`film-roll-native-raw-pair/v1`、`film-app-frozen-evidence/v1`；更新 RAW gate 可選保留、一般 JPG publish 拒絕 RAW、legacy RAW 缺 native JPG 時拒絕補建；生成 header。
- Verifier：保留 RAW/原生曝光 hold、low2、F3S CRC；pair 檢查格式/尺寸/來源時間及相機一致，索引沿用原 RAW 指紋和 JPG CRC；一般 preview publisher 拒絕 RAW。未知來源仍 fail closed。
- Runtime-internal 新增 `native_frame`、`native_capture`、`ram_log`，僅機內所有權/診斷。沒有新增 portable recipe/圖片 metadata 欄位，沒有 LaneAdmission 或 override。
- 沒有新增工作佇列、採集重試、隱藏降級。native 幀保留若僅配置記憶體失敗，依明確矩陣合約繼續 RAW，JPG 缺失另報。

## 測試與建置

- `make test-raw-darkroom CC=/usr/bin/cc CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS='-L/usr/local/lib'`：通過，含 RAW、暗房、ROLL、8 export tests。
- 擴充 `tests/raw_app.c`：同交易保留 native+RAW，無額外幀，五個幀位置故障仍還原寄存器。
- 擴充 `tests/roll_preview.c`：RAW 不可作 JPG 解碼來源；即使 RAW low byte 非法，native JPG 路徑仍成功且 CRC 與原有 native publish 相同；錯配與未知來源拒絕；legacy RAW 缺 native JPG 不解碼、不改原件。
- `python3 tests/camera_ui_package.py`：9 tests passed，新增來源分類與既有回收 headers SHA 不變驗證。
- `python3 tools/camera_runtime_contract.py --check`：通過。
- 原有 `tests/ram_evidence.py` 在離線 Linux SDK 以未修改副本通過，涵蓋真實 C memory、TCP HTTP、CRC、slow reader、FIN drain、續傳及無效請求。macOS 首次測試因沒有 GNU fopencookie 未跑通，改在原有 Linux SDK 執行；直接掛載其他工作樹亦不可見，故僅把原樣測試/headers/tools 放在 build/existing-ram-validation 測試。
- r11 因新增呼叫缺 draw 前置宣告而建置失敗，未部署；r12 修正宣告後正常 App 3DSX、ARM11 JPEG、ROMFS 驗證通過。
- Vision 三條 pytest（test_runtime_contract_boundaries.py、test_runtime_projection.py、test_metadata_runtime_package.py）未跑：Film3DS 不存在這些 Python 模組/測試，以上為本倉庫對應契約與實作測試。Vision runtime 模組未變更。

## 第一段實機成果

r12 binary SHA256：`828d6ffc062d8904748a02d444af707c63d17c0bfa10ee38ff8f24d7ea7b5073`。使用者確認 NetLoader 就緒後正常啟動，沒有 f3rawqa 自動化參數；使用者實際按 L、A/R 拍攝後開啟回收。

同一凍結快照 `film-app-0072-4003916663900` 回報 complete=1、failed=0。已實際從 SD 取回並核對：

|檔案|長度|凍結 CRC32|
|---|---:|---|
|ROLL0072.F3S|614704|e191d3b6|
|ROLL0072.JPG|25976|0ed92d68|
|ROLL0072.F3I|44|2144df1c|

F3S 包含304-byte header及614400-byte、640x480 RAW10 high8+low2；F3S內部CRC與全部 low2 亦通過。RAW payload SHA256：882f5b90168164eb602ad818cb37f3550f4c0335143150d7cf976f359c831891。索引的 source fingerprint 與這份 RAW 一致，JPG CRC 與這份 native JPG 一致。

主機原樣工具預設64KiB range在後續區段逾時；使用既有 frozen-resume-v2/16KiB range 後三個照片關鍵檔案完整驗證。616577-byte歷史日誌傳輸仍中斷，僅保留 partial/events，不把完整日誌回收當作已完成；不因此阻塞暗房驗證。第一段按鍵 RAW 採集及 SD 保存已有真實原件證據。

資料：`build/shutter-native-raw-20261117-r12/button-capture-recovered/`；完整歷史日誌、手動曝光、暗房/GPU畫面及雙輸出仍待後續實機確認。

## 第二段實機確認及交付範圍

使用者依序退出回收、進 ROLL 選 #0072、進暗房並操作預覽/保存後回覆：「可以進行暗房操作 已經顯示預覽和可保存」。這是正常 App、真實 SD RAW 與暗房入口的機內確認，不是桌面 renderer 或另一條自動化拍攝路徑。主機記錄於 `button-capture-recovered/verified-workflow.json`。

已完成：正常快門 RAW 採集/保存、原生 JPG 配對、ROLL 選圖、機內 RAW 暗房預覽及使用者確認可保存。既有曝光/WB/LUT/銳度/顆粒管線保留，本次沒有另寫影像效果或工具。

證據限制：完整歷史 film3ds.log 未回收成功；本輪未再次覆蓋所有手動曝光級別、GPU/CPU 顆粒視覺與耗時組合，也未把使用者暗房輸出的兩份 JPEG 另行下載核對。以上不冒稱已逐項實機驗證。原始 RAW、JPG、索引三檔 CRC/關聯已實際核驗，機內暗房顯示與保存能力由使用者回覆確認。

交付沿用 r12，不再為非必要診斷更換可用版本或擴大範圍。
