# Film3DS Display gray × Native AE：實機配對資料採集

狀態：可在 GitHub Actions ARM11 建立 `FilmGrayCal.3dsx`。此包沿用 Film3DS 正式 RAW 擷取流程，新增**明確的灰階配對採集**，不是已測好的新感光元件校準曲線。尚未取得你這部 3DS 新的光學量測；需依本文採集後再做數值擬合。

## 在 3DS 上操作

1. 從對應 GitHub Actions 的 `FilmGrayCal-ARM11` artifact 下載 `FilmGrayCal.3dsx` 和 SHA256；放入 SD 的 `/3ds/FilmGrayCal/FilmGrayCal.3dsx`（或使用你的 Homebrew Launcher 習慣位置）。這個 .3dsx 與 `Film3DS.3dsx` 具相同功能，只是為採集命名，不必覆寫你目前的正常程式。
2. 在電腦用 `python3 tools/gray_chart.py --out Film3DS-gray21.png` 產生 21 階 sRGB 灰階圖，固定顯示於你要校準的螢幕，設定 **100% 實際尺寸，關閉自動亮度、HDR、夜覽／True Tone／動態對比與螢幕保護**。保持螢幕、相機、距離、光源與構圖不動，灰階條位於畫面中央、盡量填滿寬度。
3. 在 FilmGrayCal 用 `L` 打開後置 REAR1，相機選 **Native AE**，變焦 1×。待預覽穩定，再以 `Y` 展開控制、上下選 `Exposure compensation`、左右調整 Native AE 等級。每次改檔後等待至少四張穩定預覽。
4. 按住 `SELECT` 再按 `R` 拍攝**灰階配對**（單按 `R` 仍是普通 RAW）。快門前在 UI thread 複製最近完整的 VGA YUYV；之後按正式路徑拍攝和校驗 DNG，完成才原子保存 F3Y 和配方基礎 F3E。顯示 `Gray pair saved` 才代表 YUV/DNG 配對已寫入且讀回校驗通過。若 `RAW camera not ready`／`Gray pair: need fresh ...` 重新開 L 並等預覽。**禁止將單獨的 F3E 視為灰階測量。**
5. 第一批先拍 **Native AE 0：5 組**；確認三種檔案都存在後，再拍 `-3,-2,-1,0,+1,+2,+3` 各至少 3 組。最後換一張有黑白高反差細節的靜止場景拍 5 組，再拍一般黑白場景 5 組。整套 36 組起步。
6. 測量前先備份這些原件，避免在 ROLL 誤刪。使用 ROLL SELECT 解碼可以產生一般縮圖，但**對含有效灰階 F3Y 的照片不做舊版未校準的 Shot 自動擬合**，避免污染對照。

## 每張照片保留的文件

- `sdmc:/3ds/Film3DS/rolls/ROLLnnnn.DNG`：原始 Bayer 10-bit 影像，由既有 RAW 捕獲與 MD5 驗證流程產出。
- `sdmc:/3ds/Film3DS/previews/ROLLnnnn.F3Y`：64 bytes F3YG v1 版本化標頭 + 614400 bytes 原生 VGA Y0-U-Y1-V；包括照片 ID、RAW payload CRC、native AE 等級、原生 AE/AWB 開關狀態、YUV 系數與預覽時間。**預覽取自快門前最近完成的一幀，與 RAW 非同時曝光**，不能視為相同快門時間。
- `sdmc:/3ds/Film3DS/previews/ROLLnnnn.F3E`：現有 Neutral/Shot 雙曲線 sidecar；灰階樣本的 Shot 先保留未擬合，待根據 Display gray 實際校準後再生成。
- `sdmc:/3ds/Film3DS/log/film3ds.log`：非敏感的擷取狀態與錯誤診斷，可協助查失敗項目。必要時再帶回。
- 若已使用 ROLL 解碼，`F3C / JPG / F3I` 都只是可重建的衍生資料，不是此次校準分析的必需品。

F3Y 逐項檢查版本、長度、CRC32、原始 RAW 來源 CRC、AE 狀態欄位；已存在的 F3Y 不覆寫。只有同一照片編號、RAW CRC 成功綁定後才接受配對。普通 RAW 存檔、LUT 目錄與原始 DNG 均不會因採集模式改變。使用 ROLL **正式確認刪除原件** 時會同時刪除對應 F3Y，因此請先備份。

## 離線檢驗與交付

把所有 `ROLLnnnn.DNG`、`ROLLnnnn.F3Y`、`ROLLnnnn.F3E` 連同你使用的 `Film3DS-gray21.png`、灰階圖位置／裁切範圍、螢幕亮度與 Native AE 等級資訊一起保留。可將整個採集目錄壓縮後傳回此對話，之後才進行 Display gray 真正的亮度校準及 Shot 擬合。

若已有電腦環境安裝 NumPy，可先用以下工具驗證每張配對並輸出灰階 21 點的相對亮度和裁切率：

```sh
python3 tools/gray_pair_report.py \
  --dng ROLL0001.DNG --yuv ROLL0001.F3Y \
  --calibration romfs/calibration/display-gray.f3isp \
  --preview-roi 0 210 640 290 --raw-roi 0 210 640 290 \
  --output ROLL0001.gray.json
```

上述 ROI 只是**語法範例**，不是你的實機量測座標。實際上必須先確認兩張照片各自完整灰階條的左右與上下邊界。不能自動認定 YUV 與 RAW 完全對齊，兩張 ROI 可不同。工具以每段中央區域的中位數量測；RAW CFA 先用簡化區域近似估亮度，**正式標定仍必須以產品 RAW scene-linear ISP 和原版黑位、CCM、shading 的相同順序重算**。

## 正式校準的門檻

- 同場景 Native YUV 與 Display gray 校準後 RAW 的配對區域，21 級灰階均須保序，不得把 clipped/raw-near-black 的色塊拿來推算 EV。
- 校準應對多個曝光等級使用同一基底，按照每檔**實測 RAW 亮度響應**建立 AE level → 相對 EV 表。
- `Neutral` 一定維持全零 RAW 額外曲線；`Shot` 必須來自當前校準版本與同來源配對，且明示估算誤差；任何缺失／不一致則保留中性。
- 用至少一組未參與擬合的灰階拍攝與黑白場景驗證，不得以「產出 JPG 看起來接近」取代數值誤差報告。
- 本階段未重寫 PICA200 GPU 或宣稱硬體 AE Lock 能工作；這個資料收集只讀相機預覽並沿用原有 RAW 拍照，不在快門路徑插入額外曝光寄存器控制。

## 測試

`make test-raw-darkroom` 另外執行灰階 F3YG 來源綁定、CRC、破損、重複寫入 C 測試，以及 Python DNG/F3Y 輸入完整性、21 點灰階報告測試；ARM11 workflow 發佈可檢驗 SHA256 的 `FilmGrayCal.3dsx`。CI 通過只保證主機邏輯與可編譯／打包，不能代表你手上的 3DS 已完成實拍。
