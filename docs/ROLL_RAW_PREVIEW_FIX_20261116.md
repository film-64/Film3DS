# ROLL 刪除、RAW 預覽與 YUYV 顆粒修正

上機包：`build/raw-roll-preview-device-20261116-r2/Film3DS.3dsx`，SHA256 `486b33e0e12c0d8c9b3b6c4c0140953c8970002c6fa29350677472dca33a6ebf`。八組 LUT/F3P、三份顆粒 profile、Pan 400 紋理已內嵌。來源凍結、ROMFS 及 ARM11 檢查均通過；部署結果見同目錄 `deployment.json`。

## 行為

- 刪除：移除 `ROLLS` 尾斜線再組合刪除路徑，避免原本的 `rolls//ROLLnnnn.F3S`。libctru archive driver 會將路徑字串轉給 FS，沒有在該層合併重複斜線，見[官方原始碼](https://github.com/devkitPro/libctru/blob/master/libctru/source/archive_dev.c)。確認鍵集中處理：A 的 down 事件不再被方向／觸控位元擋住；X 仍取消，A 與其他動作按鍵的組合不刪除。原 ID／索引／選取核對保留。這兩項是已確認的程式缺口；是否涵蓋機器上的全部失敗原因仍以新版實測為準。失敗畫面會顯示照片編號、狀態與 errno。
- RAW：原始 F3S 保存及 CRC 回讀成功後，在既有唯一保存 worker 內產生 320×240 中性 JPEG 預覽。完成後進入 ROLL，選中新照片並顯示上屏預覽、`RAW 10bit` 標記與選中格的小預覽。
- 既有 RAW：選中時找對應預覽；缺少時同步建立一次。預覽存於 SD `3ds/Film3DS/previews/Rnnnn-xxxxxxxx.JPG`，名稱含原始尺寸／RAW bytes 的 CRC32，原件變動會使用另一個 key。命中時嚴格解碼單張 320×240 JPEG；損壞直接報錯，清空舊顯示，不自動覆寫或反覆重建。沒有改寫原始 RAW。刪除仍只刪原始 F3S，匯出的 JPEG 與衍生預覽保留；沒有新增自動清理。
- YUYV：新的多層顆粒在 RGB 端作用，適用於 YUYV 來源。Fine／Medium／Coarse 的完整 CPU 暗房預覽已驗證。GPU 編碼原先使用固定 `42+8*layer`，對合法的大負向尖峰可能超界而回傳 invalid；改為依整個密度場決定尺度，讓三層各落在 2..82，混合後不飽和。逐像素密度誤差上限 0.4 保留，沒有放寬 verifier 或暗中切回 CPU。錯誤畫面區分 color/render 與 CPU/GPU。

## 治理與驗證

1. **Runtime 分類**：`film-roll-delete/v1` 是確認後的原始檔副作用；`film-roll-preview/v1` 是選片預覽計算；新增 `film-roll-raw-preview/v1` 是保存後／明確選取 RAW 觸發的同步預覽計算與衍生檔寫入。沿用既有保存 worker，不新增 task、scheduler、queue、retry 或 admission override；三者無 LaneAdmission，proof=none。GPU 密度混合沿用既有登記來源。
2. **矩陣**：先更新 `tools/camera_runtime_contract.py` 的刪除路徑／按鍵契約、預覽來源／快取／失敗方式與 GPU 編碼界限，再實作並生成 header。完整方格縮圖仍未啟用，只有選中格預覽。
3. **Verifier**：保留來源白名單、未知來源拒絕、過期刪除目標拒絕；加入精確路徑、動作鍵分類、RAW 預覽 JPEG 尺寸／長度／解碼驗證與 GPU 編碼有限值／精度驗證。CPU/GPU 失敗不顯示舊照片。
4. **Runtime-internal**：中央集合新增 `preview_status`、`roll_preview_id`、`roll_preview_format`、`delete_errno`；暗房失敗階段沿用已登記的 `failure_phase`。皆是本地 UI／執行狀態，未放入 FilmRecipe/F3S。
5. **測試**：擴充 `tests/roll.c` 的 SD 尾斜線路徑與 A+方向／觸控、A+其他動作、X 取消；新增 `tests/roll_preview.c`，實際建立及解碼 JPEG、檢查原 RAW 不變、CRC-key 變更、損壞拒絕、未知來源拒絕，以及 YUYV 三種顆粒、不同照片 seed、GPU 編碼與負向尖峰。`tests/camera_ui_package.py` 驗證新來源與可攜欄位邊界。
6. **命令與結果**：`./build/roll-test`、`./build/roll-preview-test`、ASan/UBSan 版本、`make test-raw-darkroom`（含八個 CLI regression）通過。`make test` 的 C／暗房／構圖／ROLL／七個包裝測試及 integration 通過；localhost HTTP 因沙箱 bind 被阻而中止後，以授權環境單獨補跑 `python3 tests/simulator_http.py` 通過，`python3 tests/hardware_qa_report.py` 亦通過。編譯使用系統 cc、`-I/usr/local/include -L/usr/local/lib`。frozen App 由既有離線 devkitARM 容器完整建置並驗證。
7. **未測與限制**：尚無本版實機刪除、預覽速度及 GPU 顯示結果。Vision-product 三項 `pytest tests/test_runtime_contract_boundaries.py -q`、`pytest tests/test_runtime_projection.py -q`、`pytest tests/test_metadata_runtime_package.py -q` 未執行，因本目錄是 Film3DS，對應三檔及模組不存在。舊診斷 BIN 的自動資料夾發現與全列表縮圖不在本次改動範圍。

修改檔案：registry／generated header、`roll.h`／`roll.c`、新增 `storage/roll_preview.c`、`main_3ds.c`、`layered_grain.h/.c`、`grain_gpu_3ds.c`、`darkroom.h/.c`、Makefile、上述三個測試檔。未變更本版的相機寄存器採集策略、原始 F3S 格式或既有照片內容；未清理其他 dirty 檔案。
