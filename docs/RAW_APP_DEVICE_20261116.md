# Film3DS 機內 RAW 暗房版

本次交付是現有 Film3DS App，完整 ARM11 `.3dsx`：`build/raw-app-device-20261116/Film3DS.3dsx`，SHA256 `6a48b12323befea796194ee4a26c98af1de6ae3a120592171db49b0b36f35eb0`，9629272 bytes。凍結來源、八組 LUT/F3P、三份顆粒 profile 與 Pan 400 F3GT 紋理均由 package/build manifest 驗證；沒有修改原廠 Camera CXI。

## 機內操作

1. 在 Homebrew Launcher 載入此 `.3dsx`，或由 NetLoader 接收並啟動。
2. L 開啟 REAR1；R 採集 640×480 的 8+2 RAW。這版使用已驗證 case0 的固定 coarse=128、四通道 gain=528，畫面有明確提示；不是依照預覽的曝光步進拍 RAW。完成後關閉相機，L 可重新開啟。REAR2／FRONT 保留原有 YUYV 路徑。
3. 等待 SD 保存完成，X 進 ROLL 選照片，再 X 進 DARKROOM。RAW 保存在既有 `rolls/ROLLnnnn.F3S`，包含原始 high8/low2 payload 與 CRC；沒有先轉 JPEG 作為底片。
4. 暗房沿用方向鍵、觸控及 A 預覽；曝光在線性 RAW ISP 調整，白平衡、色調、色彩、銳度、構圖及 LUT 使用現有介面。SELECT 保存時，RAW 會同時生成 `exports/Rnnnn-rrrr-plain.JPG` 和 `...-look.JPG`，兩者使用同一份 ISP float RGB 與共同參數；已存在檔案不覆寫。
5. 顆粒有 Size、Shadows、Midtones、Highlights 和 Backend 項目。CPU 為預設，GPU mix 可明確切換。Medium 使用內嵌 Pan 400 實際樣本紋理；Fine／Coarse 使用對應樣本統計的程序場。
6. 如已有獨立採集的 RAW，放入 SD 的 `3ds/Film3DS/raw/RAWnnnn.BIN`（640×480、614400 bytes），在 ROLL 按 SELECT，每次匯入一張未占用相同編號的 F3S；原 BIN 保留。這不是使用 App 新拍 RAW 的必要步驟。

GPU 確實執行三層紋理的加法混合與讀回；CPU 負責樣本取樣、亮度響應與線性密度作用。GPU 結果逐像素檢查密度場誤差不超過 0.4，失敗直接報錯，不暗中改用 CPU。這是混合後端，不是宣稱整個 ISP 或顆粒流程都已搬上 GPU，也沒有未經實測的加速承諾。

## RAW 採集邊界

App 先釋放既有預覽／手動曝光所有權，重新以 context B、15fps、VGA、5120-byte transfer 初始化 REAR1，取得三張原生 seed frame。然後驗證 chip=8832、原點 0、末點 487/647、read mode=0x25、frame lines=704、native bypass clear，保存十個控制寄存器及 sequencer。

鎖定 AE/AWB 後，先做一次原生手動 gate，再套用 run05 case0 的 479/639、format=131、8+2 bypass、datatype=30，讀回驗證，兩張 settle 後保留一張 RAW。每幀收取有 1500ms 上限、idle proof、DMA guard 及 RAW low2 驗證。fine integration 與 frame lines 在 hold 中檢查。

結束時先證明 idle，再反向恢復十個寄存器與 sequencer、讀回驗證、恢復 auto、收取一張原生幀後關閉相機。失敗不發布 RAW；無法證明 DMA idle 時不釋放該 DMA，保留到程序結束。沒有自動重試或隱藏佇列。

## 治理與測試

- 分類：新增同步機內 compute 入口 `film-camera-raw-capture/v1`、`film-app-raw-canvas/v1`、`film-app-raw-import/v1`、`film-app-raw-export-pair/v1`、`film-grain-gpu-mix/v1`；無 RegionTask、TaskContext、TaskScheduler，無 LaneAdmission 或 override。採集、匯入、輸出為 local_trace，canvas/GPU kernel 為 none。
- CONTRACT_MATRIX：先登記以上邊界，再接入 C/App。更新既有 darkroom controls/RAW format、package 可選顆粒資源政策，生成 header。
- Verifier：精確來源、chip/geometry/readback、RAW low2、F3S CRC、輸出碰撞、GPU 密度誤差；既有 fail-closed 行為保留。
- Runtime-internal fields：中央集合新增 `grain_renderer`、`grain_gpu`、`dma_quarantined`、`unsafe`、`dma`、`reg`；沒有加入 FilmRecipe 或 FilmFrame 的可攜資料欄位。集中集合的注入／可攜測試通過。
- 新增 `tests/raw_app.c`：採集交易正常、錯誤來源、錯誤晶片、native bypass 錯誤、各幀失敗、假 hold、恢復失敗；RAW F3S 往返、實際暗房入口、無 LUT／有 LUT 雙輸出、防覆寫、RAW 匯入／損壞拒絕、混合後端等價與失敗不改圖。
- 已執行 `make test-raw-darkroom`（C 核心／App 測試及八個 CLI regression）、`./build/unit`、`./build/darkroom-test`、`make test-native-integration`（19+3）、`python3 tests/camera_ui_package.py`（6）、registry `--check`；皆通過。使用系統 cc 與本機 JPEG include/lib 路徑。App 核心另經 ASan/UBSan。完整 frozen App 在離線 devkitARM 容器編譯、ARM11 指令稽核與內嵌 ROMFS 檢查通過。
- Vision-product 的 `pytest tests/test_runtime_contract_boundaries.py -q`、`pytest tests/test_runtime_projection.py -q`、`pytest tests/test_metadata_runtime_package.py -q` 未執行：此工作目錄是 Film3DS，三檔及其對應模組不存在。

此次新增／修改：`raw_capture.h/.c`、`raw_camera_3ds.c`、`raw_import.c`、`raw_export.c`、`raw_darkroom.h`、`grain_gpu_3ds.c`、`layered_grain.h/.c`、`film.h`、`platform_camera.h`、`platform_develop.h`、`darkroom.h/.c`、`image.c`、`storage.c`、`main_3ds.c`、兩份 Makefile、registry/header、package tool 與 `tests/raw_app.c`。其他既有 dirty 變更未清理。

實機成像、恢復和 GPU 視覺／速度仍需這個新包的機內執行結果。CFA、黑白位、CCM 未完成校準；永久壞點表與雙攝 RAW 不屬於本包已驗證能力。部署狀態以凍結目錄的 `deployment.json` 為準，NetLoader 接受啟動請求不等同於相機／GPU 實測通過。

## NetLoader 中斷處理

第一次連線至既有 `192.168.0.69:17491` 逾時、零傳輸；取得使用者再次就緒回覆後的新一次傳送，標頭被接受但 payload 傳送逾時，未收到完整檔案確認，也未發出啟動參數。失敗回執保留，沒有自動重試。

原本的 connect-only 就緒契約不適用於已開始傳送的接收器。因此另外登記 `film-camera-ui-receiver-reset-deploy/v1`，僅允許使用者明確確認 NetLoader 已重新啟動、上一筆為 payload 失敗、同一 binary／manifest 的一次新傳送；先封存舊回執並記錄其 SHA256。launch 結果不明、不同 binary、未確認重開都拒絕。新 source 是外部部署副作用邊界，非 compute，無 LaneAdmission/override，證據為本地回執；沒有修改或重編譯已凍結的 App 以繞過失敗。

registry 先登記新邊界，再更新 package tool 的 verifier；新增正向重開證據及非 payload／不同檔案／缺失證據的負向測試。中央 internal 集合新增 `receiver_reset_note`、`prior_failure_sha256`。`python3 tests/camera_ui_package.py` 七項通過，既有未重開拒絕測試保持不變。等待使用者的接收器重開回覆後才可執行；App 功能與部署仍須分別驗證。
