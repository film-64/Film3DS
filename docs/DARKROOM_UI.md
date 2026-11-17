# 暗房列表與離線沖洗（2026-11-15）

本次修改只涉及暗房、離線影像處理、配方儲存格式和相應測試。相機開啟、
預覽採集、CAMU 控制、硬體銳化及拍攝流程由相機工作另行處理，沒有在暗房
新增相機調用。本文的離線測試不代表已通過實機相機或畫面驗證。

後續白平衡、全畫布預覽與二次構圖操作以 [新版說明](WHITE_BALANCE_COMPOSITION_20261115.md) 為準；以下操作段落記錄上一版。

## 操作

- 暗房使用單一列表，不再用 Y 切換 LUT／顆粒／曲線面板。
- 上下選列，左右改值；控制字體維持 0.62，行高縮為 18 px，同屏七列，超出時捲動。
- 項目為：畫幅比例、輸出尺寸、膠片模擬、曝光、Highlights、Midtones、Shadows、
  顆粒強度、顆粒種類、飽和度、銳化、LUT 強度、縮放插值、Halation。
- 裁切、尺寸／放大和插值直接編輯當前暗房配方。尺寸顯示裁切後的實際像素。
- A 沖洗並顯示預覽，只保留一張全解析度記憶體影像，沒有寫 JPEG 或 F3S。
- 預覽中只有單獨按 SELECT 確認保存。其他按鍵先退出預覽且不保存，**繼續執行原本按鍵行為**；
  例如方向鍵繼續調整、B 返回、X 換頁、A 重新預覽。SELECT 與其他鍵的組合不是保存確認。
- SELECT 將已預覽的同一張快取影像寫成新 JPEG，不重算、不覆蓋已有檔案；無論成功或失敗均釋放快取。
  再按 SELECT 不會再次保存舊預覽。沒有另加保存選單或二次確認。
- 原來的暗房 SELECT「另存配方副本」與全域插值切換已由上述操作取代。

## 膠片模擬與曲線

膠片模擬是獨立設定，並非整套自訂配方。換 LUT 時只替換 LUT ID 及其 input/output
編碼；保留當前曝光、HSL、曲線、飽和度、銳化、顆粒、比例、尺寸和 LUT 強度，包括刻意設定的零值。
切回原 LUT 不從已保存配方恢復舊值。非零 profile 預設調整不會自動覆蓋使用者設定。

曝光增益留在 `input_tone`，在線性光中、LUT 採樣前執行。
新版 H/M/S 使用獨立的 `output_tone`，在 LUT 及舊版影調修正之後、HSL／飽和度之前執行。
曝光為 ±4 EV，1/3 EV 步進；H/M/S 各為 -5…+5，內部對應 -1…+1，0 為中性。
正 Highlights 提亮高光、正 Shadows 加深暗部、正 Midtones 提亮中間調。這些是本產品
明確定義的級數，不宣稱等於富士專有演算法或原廠數值範圍。

新版曲線在編碼後的亮度域使用 `0/.25/.5/.75/1` 錨點，目標分別為
`-S*(S<0?.06:.025) / (.25-.10*S+.03*M) / (.5+.06*M) / (.75+.10*H+.03*M) / (1+H*(H<0?.10:.025))`。
在 identity 曲線上用 cubic smoothstep 混合相鄰錨點的偏移，11³ 組合保持單調，
且同一控制的方向不會隨亮度反轉。接點左右斜率均為 1；深黑可提起，白點可壓低。
全零時精確繞過，不改 LUT 原效果。映射後回到線性亮度，等比例縮放 RGB，再處理色域。
中間調向暗部、亮部各保留一半幅度，避免只在中間的小區域有效。
係數登記於 `tools/camera_runtime_contract.py`，由生成 header 提供給 C 核心。

既有 v1/v2/v3 配方中的 `profile.exposure`、post-LUT `tone`，以及 v4 的前置曲線都保留原行為；
它們不會在開檔時被移到新位置或偷偷重寫。新控制獨立於這些舊修正。
普通 3D LUT 自帶的明暗特徵仍是濾鏡的一部分；本次沒有擅自移除、反算或歸一化 LUT 的明暗響應。
使用者 H/M/S 已移到 LUT 後，因此它不再被後續 LUT 的 shoulder/toe 壓縮。
但不同 LUT 的基底畫面仍然不同，不宣稱同檔位在所有照片／像素產生相同最終亮度。

## 離線銳化與配方格式

現有拍攝端 `CAMU_SetSharpness` 保持不動。暗房的 Sharpness 是已拍影像的獨立軟體處理：

- -5…+5，0 精確繞過；正值以 3×3 Gaussian 低通的亮度差做 unsharp mask，負值逐步柔化。
- 正值向 RGB 加相同亮度差，避免獨立放大各色通道的銳化差異。
- 在縮放後、顆粒前執行，不銳化後加的顆粒；邊界使用邊緣延展。
- 不把 bicubic 插值稱為銳化，不寫感測器寄存器。

F3SR v4 header 為 368 bytes：保留 v3 的所有欄位，再增加 7 個 input_tone float32
與 sharpness（uint32，儲存 level+5），最後是既有規則的 CRC32。
F3SR v5 header 為 396 bytes，在 v4 後再加 7 個 `output_tone` float32，最後是 CRC32。
只在使用相應欄位時寫較新版本，其餘配方仍寫可表達它的 v1/v2/v3/v4。舊版本新欄位為零，
不會把既有前置曲線數值偷偷重新解讀為新曲線。
目前 A/SELECT 流程只輸出 JPEG，不自動另存 F3S。

## Gold／Portra 離線核對

```sh
python3 tools/darkroom_lut_check.py \
  --luts build/camera-ui15-20261114-r3/romfs/luts \
  --out build/darkroom-lut-check-20261115
```

檢查器拒絕覆蓋既有目錄，產生合成 sRGB 色塊與灰階源圖，使用實際 `build/filmctl develop`
套用兩個已打包 LUT 及同名 F3P，輸出對照圖、JPEG 和 JSON。兩者像素不同；此目標的
RGB 平均絕對差約為 `3.80/0.68/2.50` 個 8-bit 碼值，最大差 `16/4/10`。
這不是感知 ΔE，也不是實拍或膠片校準結果。沒有任意加色偏補丁或替換 LUT。
實機色彩相近的成因仍需相機工作穩定後用固定輸入核對，包括實際載入來源和 ISP 色彩。

## 契約與驗證

- `tools/camera_runtime_contract.py` 的 `film-darkroom/v1` 登記離線計算、前置曝光／舊曲線、新的後置曲線、銳化、
  單列表及不寫檔的單張預覽；`film-darkroom-save/v1` 登記 SELECT 後的 JPEG 寫入副作用，
  不創建新計算、不重算、不調用相機，其他鍵繼續原本操作。
- 沿用來源 fail-closed 驗證，生成 header 更新已登記來源；未放寬未知來源。
- 本次沒有新增 runtime-internal metadata 欄位；預覽像素與游標只存 UI 記憶體，不寫進 F3SR 配方。
- `tests/darkroom.c` 覆蓋換 LUT 保留零值與當前設定、十一檔、1,331 組曲線單調性、
  前置曝光的實際非線性 LUT 結果、後置曲線在常數 LUT 下仍有十一檔不同結果、C1 平滑接點、
  黑白端點、銳化、v4/v5 往返、取消零寫入、SELECT 快取保存、
  既有檔案保護及失敗後舊快取不可保存。
- `tests/camera_ui_package.py` 覆蓋預覽／保存計算分類及取消操作；
  原有 darkroom 和版本拒絕測試更新為十一檔與 v4/v5 的正確邊界。

macOS 本機完整測試命令（明確指定已安裝的 libjpeg 路徑）：

```sh
make test CC=/usr/bin/clang \
  CPPFLAGS='-Iinclude -D_POSIX_C_SOURCE=200809L -I/usr/local/opt/jpeg-turbo/include' \
  LDFLAGS='-L/usr/local/opt/jpeg-turbo/lib'
```

結果：完整 `make test` 通過。另以固定 devkitARM 映像執行 ARM11 UI 語法檢查通過。
首版獨立完整編譯包：`build/darkroom-offline-20261115-r1`。
新版曲線包：`build/darkroom-curves-20261115-r4`，編譯驗證憑證見該目錄 `build.json`；
測試與部署紀錄見 `CURVE_REWORK_20261115.md`。新包使用首版相機基底，只疊加暗房修改。
未連接、部署或調用相機；實機字高、觸控、處理耗時與實拍色彩尚未驗證。
舊的 HTTP 桌面原型保留其既有操作，並非這次原生暗房列表的視覺驗收工具。
