# RAW 雙曲線矩陣：中性／拍攝參考

**整合分支：** `feat/dual-raw-exposure-matrix` → `minmain`。本功能保留真實 RAW、DNG 格式、現有攝影機 AE 與手動曝光寄存器控制，僅在顯影管線中增加一組具來源依據的預設。Native AE 原有 ±3 補償範圍未擴張。

## 審閱結論及曲線責任

目前有三個不同的亮度控制位置，不能把它們當成相同的 3 個旋鈕：

1. **感測器／Native AE：** `CAMU_SetExposure` 的原生補償等級、感光元件粗積分與增益，是採集條件。Native `+1` 沒有實測證明等於 `+1 EV`。
2. **RAW 線性顯影：** `source/image/raw_darkroom.c` 在 demosaic 之後，先套 `input_tone[EV]`，再套 `raw_shadows/midtones/highlights` 三段 scene EV、可選 shoulder 和局部曝光。這裡由 **中性／Shot 雙曲線矩陣** 管理預設，避免反覆手動設定多個相近旋鈕。
3. **顯示輸出：** 原有 `input_tone` 非 EV 欄位、legacy `tone`、LUT 及 LUT 後 `output_tone` 是具有不同位置和相容性的編輯控制。新預設不清除、覆蓋或重複套用使用者在這層的 H/M/S，並且不重用 Camera 的原生等級作為浮點 EV。

## 雙曲線矩陣 F3EM/v1

標準 `ROLLnnnn.DNG` 原件保持不可變。拍攝時在 `previews/ROLLnnnn.F3E` 寫入 **88 bytes** 固定長度、明確小端的版本化中繼資料，內容包含：

- 照片 ID、對原始 10-bit Bayer payload 算出的 `source_crc`、曝光所有權（Native／Manual）與原生補償等級（不是 EV）。
- 拍攝前最後一張原生 YUV 預覽的採樣低／中／高線性亮度；同一套既有 16×12 測光取樣網格產生，不再掃描整張預覽。
- 明確兩組 `EV, Shadows, Midtones, Highlights, Shoulder`：**Neutral 全 0**；**Shot 在拍攝當下尚未計算，初始同為全 0**，不填入猜測的光學數值。
- fitted flag、內容 CRC32、來源檔案 CRC。損壞、錯誤照片 ID、錯配 RAW 或非有限參數必須拒絕，原 DNG 一律保留。

只有使用者在 ROLL **明確觸發 RAW 解碼**後，才對持久化、完整的 scene-linear RAW 採固定 16×16 子樣本，估算其三個分位亮度，與拍攝前 native YUV 的三個值比較：中位比值估全域 EV（界限 ±2 EV），暗部／高光殘差各限 ±1.5 EV，`Shot.midtones` 殘差為 0。結果經原有 scene curve 與 EV 的已知位置套用，不加不明來源的 Local Exposure，最後原子替換 F3E。此步不重新讀寫 DNG、不重拍、不建立自動背景解碼。

**適用範圍：** 這是「拍攝前原生 YUV 預覽 vs 稍後 RAW」的**亮度分佈近似**，沒有逐像素配對、時間同步證據或原廠 Sensor-to-sRGB CCM。AE 變化、移動主體、不同 crop、RAW 既有色彩校準缺失都會降低準確度。它不能修復硬體過曝、被裁剪的白點、暗部雜訊、AWB 色偏或量測出真正的快門／ISO。若沒有可用的預覽參考、場景過暗、分位無法解算，Shot 不被標記為 fitted；使用 Neutral，不聲稱恢復原生畫面。

## 使用者暗房

ROLL 明確解碼後，開啟同照片的 RAW 暗房時先依 F3I `source_crc` 驗證 F3E；若 fitted，預設選 `RAW curve: Shot (approx)`。使用者隨時能切至 `Neutral`（額外曝光參數 EV、RAW H/M/S、Shoulder、Local 全部為 0）；自行調這些欄位時顯示 `Custom`，不改寫儲存的 Shot。舊 DNG 或側車不存在時仍能正常開 RAW，只顯示 Neutral；錯配或損壞的 F3E 會提示，沒有任何降級寫回原件的步驟。

現有 ROLL 網格和 JPG 仍是 **中性預覽**，因為派生預覽生成與暗房的調色是在不同使用者操作階段。要讓 ROLL 也顯示 Shot，需要另外加入有明確版本與來源綁定的新預覽派生格式／指令，不能偷偷變更 F3I 既有 JPG 的意義。

## GPU 與 CPU

當前 `film_raw_scene_grade` 使用 CPU 進行 signed linear scene 的 WB、EV、RAW H/M/S、shoulder 和 LUT 前分級，GPU 已處理部分縮放與顆粒圖層合成、final tile render；`A` 預覽重用解碼後的 linear F3C／在記憶體中保留的 `FilmRawScene`，不會每次重做 RAW demosaic。Shot/Neutral 的切換直接重用這條管線，與最終 JPEG 一致。

**目前尚未實作 GPU 上的完整 RAW 三段曲線運算。** 3DS PICA200 為固定功能架構；是否以 1D LUT、低解析 preview 或預計算的 CPU 表加速，需先實測目前 CPU grading/resize/GPU readback 的比例與誤差，不能因為 CI 運行成功就宣稱即時 GPU preview 已達成。品質優先的最終輸出依然使用 CPU scene grading ＋現有 GPU resize/grain ＋ CPU JPEG。

## 驗證與回退

- `make test-raw-darkroom` 加入 `tests/exposure_matrix.c`：雙曲線身份／方向、無重複 EV、限幅、有限性、來源 CRC 綁定、檔案截斷／毀損、缺失／錯配處理。
- `tests/roll.c` 驗證只有已確認刪除原照片才清除其 `F3E`，未確認操作不觸碰原件。
- `tools/camera_runtime_contract.py --check` 驗證新存取與 fit 來源的 source registry 未偏離 generated header。
- GitHub CI 的主機／ASan／UBSan、RAW、原生媒體、FilmProbe 與 ARM11 檢查必須全通過，仍須真機觀察 `Shot` 與實際 native preview 亮度和色差、RAW 開關切換的回應、記憶體、VRAM、批次解碼與 SD 寫入失敗情境。
- 回退到 `minmain` 之前版本時，未修改的 DNG 一樣可讀，舊版會忽略新 F3E。不要依賴側車檔作原始 RAW 備份。
