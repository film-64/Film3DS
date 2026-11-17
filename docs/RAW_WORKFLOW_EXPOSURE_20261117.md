# Film3DS：拍攝、預覽、ROLL、暗房與曝光

上機包：`build/raw-workflow-native-manual-20261116-r4/Film3DS.3dsx`。SHA256 `b13f3a5a510df7e05ae27124f61f99d83f6d32ae9add886591d68d2cda7449fc`。八組 LUT/F3P、三份顆粒 profile、Pan 400 紋理內嵌，來源凍結及 ARM11/ROMFS 檢查通過。部署結果以該目錄 `deployment.json` 為準。

## 各階段責任

1. **拍攝**只收取並保存原始來源。RAW 保留 high8/low2 的原始 payload；採集函式沒有去馬賽克、降噪、LUT 或顆粒。
2. **預覽處理**在原件保存及 CRC 回讀成功後，由既有唯一保存 worker 明確呼叫獨立的 pair publisher。它用既有中性 develop 流程產生 `previews/ROLLnnnn.JPG`，不套 LUT、顆粒或降噪，保留原件的 YUV 解釋與 legacy base saturation。最後提交 `ROLLnnnn.F3I` 關聯紀錄。沒有新增佇列或其他 worker。
3. **ROLL**從配對紀錄及 JPG 顯示大預覽和可見的 30 個縮圖。選圖不讀原始像素、不做 ISP、不缺圖補建。照片清單仍依原件檔名判斷存在性，未把匯出 JPG 混入底片清單。拍攝完成保持當前頁面，沒有自動跳入 ROLL。
4. **暗房**在進入時才開啟所選原件，核對來源格式、尺寸及 payload CRC 與配對紀錄一致。調參、LUT、色調、白平衡、銳度、構圖、顆粒沿用共同暗房流程；RAW 雙版本輸出從相同的 float RGB 分支。預覽 JPG 不作為暗房原件。

`F3RI/v1` 為 44 bytes，包含版本、照片 ID、來源格式／尺寸、來源 CRC、JPEG CRC、預覽尺寸及 index CRC。先提交 JPEG，再原子提交 index；中斷或內容不符會被拒絕，不顯示錯配照片。衍生配對可在明確的 publisher 階段重建；原件不被重寫。

為保留舊照片，啟動後有獨立、顯示進度的一次預覽準備階段：既有 index 直接重用，缺少 index 才讀一次原件建立配對。失敗停下並顯示錯誤，B 可停止。這個階段不在 ROLL 選圖函式裡執行。ROLL 的 SELECT 可明確準備選中原件的預覽；SELECT+Y 保留 RAW BIN 匯入入口。

## 曝光與降噪

拍攝控制為 **Native AE / Manual**，移除 App 的 Full／Center 軟體 AE。Native AE 由 CAMU 啟用並讀回確認，補償直接使用原生 `SetExposure` 等級，顯示 Native ±n，不冒充已標定 EV。Manual 沿用既有感測器 handoff、七級 coarse/gain 表、穩定幀與 readback；未 ready 或設定尚未套用時拒絕拍攝。

RAW 採集不再關閉重開相機並固定 coarse=128/gain=528。它沿用當前預覽的曝光所有權：先釋放 DMA、確認 Native AE／Manual 狀態，快門時鎖定並重新讀取 coarse、fine 和四通道 sensor gain，切換 8+2 格式時保留這些實際值。仍有晶片、幾何、frame lines、transfer unit、寄存器 hold、低位及 DMA guard 驗證。恢復後收取一張原生幀，成功時保留相機所有權供後續預覽；失敗關閉，不假裝降級為其他曝光方式。AE/AWB flag 未改變時不重寫服務，保留手動 sequencer 狀態。

RAW ISP 的固定 chroma median 已移除，原件及中性預覽不降噪。暗房新增 **Chroma denoise**，預設 0%，0 為精確 bypass。非零時在工作 RGB、LUT 前做明確的色度中值處理，適用於 RAW 解碼結果與 YUYV；輸入 RAW 永遠不改。顆粒仍在既有 LUT／色調、縮放／銳度後的位置。

## 治理與驗證

1. **分類**：曝光切換、降噪、pair 發布、JPG 預覽、原件驗證及啟動預覽準備是明確登記的同步 compute；pair index 是 read 邊界。沒有 RegionTask/TaskContext/TaskScheduler、LaneAdmission、override 或新 queue/retry。啟動準備是真實照片資產建立，不是模型 warmup。
2. **矩陣**：先登記 `film-camera-exposure-mode/v1`、`film-darkroom-denoise/v1`、`film-roll-pair-publish/v1`、`film-roll-pair-index/v1`、`film-roll-jpeg-preview/v1`、`film-roll-original-open/v1`、`film-preview-catalog-prepare/v1`，並更新既有 RAW capture／ROLL／ISP 契約。退役在 ROLL 內補建 RAW 預覽的 `film-roll-raw-preview/v1`。產生的 header 同步更新。
3. **Verifier**：來源精確匹配、未知來源拒絕；AE flag／手動 ready／sensor readback、pair index 長度／版本／CRC／ID／尺寸、JPEG CRC／尺寸、暗房來源 fingerprint 及降噪參數均驗證。沒有放寬既有 LaneAdmission 或 GPU 品質檢查。
4. **內部欄位**：中央集合加入 `exposure_mode`、`sensor_owner`、`saved_ae`、`saved_awb`、`preview_top`、`preview_bottom`。它們不進入 FilmRecipe/F3S。F3I 的格式／尺寸／內容 CRC 是資產關聯資料，不是 admission 或執行狀態。
5. **測試**：擴充 RAW transaction，涵蓋全部七個手動級、快門鎖定後 coarse 改變、四通道 gain 不相同、hold／恢復／各幀失敗及未知來源。降噪測試涵蓋 0 精確不變、保亮度、明確非零效果與 RAW 不變。pair 測試涵蓋沒有原件仍可只讀 JPG、來源不符拒絕、缺少／損壞配對不自動補建、舊 catalog 重用／建立／取消。YUYV 三種顆粒、既有 LUT／色調／銳度／WB／構圖／保存回歸保留。
6. **已執行**：`make test-raw-darkroom`（新增 C 測試、pair/grain 測試、八個 CLI regression）、`make test` 的 C／八項 package 測試及 integration、`make test-native-integration`（19+3）通過。`make test` 的 localhost HTTP 因沙箱禁止 bind 而中斷後，單獨授權執行 `python3 tests/simulator_http.py` 通過；`python3 tests/hardware_qa_report.py` 通過。RAW App 測試 ASan/UBSan 通過。完整 frozen App 在離線 devkitARM 容器編譯並驗證 ARM11 與內嵌資源。
7. **未測範圍**：本版真機 Native AE／Manual 切換、實際 RAW 曝光結果、連續拍攝、預覽速度及 GPU 顯示仍需裝置執行確認。未執行 Vision-product 三項 pytest：`tests/test_runtime_contract_boundaries.py`、`tests/test_runtime_projection.py`、`tests/test_metadata_runtime_package.py`，因 Film3DS 沒有這些模組／檔案；相同邊界以本倉庫 registry/C/package 測試驗證。

主要改動：registry/header、`platform_camera.h`、`manual_exposure_3ds.c`、`raw_camera_3ds.c`、`raw_capture.c`、`main_3ds.c`、`roll.h`／`roll_preview.c`、`film.h`／`raw_darkroom.h`、`raw_darkroom.c`／`raw_export.c`、新增 `denoise.c`、暗房列與上述測試。未修改原廠 CXI、既有照片原件或 F3S 檔案格式；未清理其他 dirty 變更。
