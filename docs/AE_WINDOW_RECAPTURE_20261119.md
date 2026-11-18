# Film3DS Native AE 可調測光窗口／回復測試（2026-11-19）

版本：`fix/ae-window-reset-roll-io` 經 PR 合併至 `minmain` 的 `FilmGrayCal.3dsx`。此版的目標是驗證相機硬體對自訂 AutoExposureWindow 的接受情況，收集新配對資料，並定位先前 ROLL `IO` 的實際失敗階段。無實機自動測試，**API 返回成功不等於已證明硬體內部測光權重完全符合窗口**。

## 已有兩組資料的結論

2026-11-19 上傳的 `Archive.zip` 含完整、CRC 互相匹配的 `ROLL0059 / ROLL0060` DNG+F3Y v1+F3E，兩者都是 Native AE 等級 0，預覽灰階條亮度不同，RAW 無滿碼裁切。舊 F3Y v1 **未記錄原生 AE 測光矩形**，不可從畫面中央亮區判斷硬體窗口。保留兩組當作原始對照資料，不回寫原件或改變歷史 sidecar。

## 新版相機操作

1. 以 `L` 啟用 REAR1，曝光模式選 Native AE，**1×** 縮放。
2. 上屏綠框顯示已成功提交至 `CAMU_SetAutoExposureWindow` 的 **VGA 感光元件座標矩形**；全畫面為 `640×480`。如果硬體 API 失敗，標記不顯示、狀態會報錯。
3. `SELECT + Y`：全域測光／置中的 320×240；`SELECT + 十字鍵上／下`：調小／調大（預設尺寸 640×480、480×360、320×240、160×120、80×60）；`SELECT + 十字鍵左／右`：水平方向每次 40 個 VGA 像素；`SELECT + 滑桿上／下`：垂直方向每次 30 個 VGA 像素。移動會嚴格限制在可視傳感器範圍。
4. 改變 Native 補償等級會**重新初始化相機**：先設定原生等級 0 作為當次確定基線、重新啟用 Native AE，再只寫入一次你選的等級。此處的 `+2` 仍只是 CAMU 原生等級，未量測證明為 2EV；相機初始化失敗會顯示狀態，不能暗中把 UI 目標數字當作已成功拍攝的實際寄存器值。
5. 在任一已生效的窗口下，按住 `SELECT` 再按 `R` 收集。新 `ROLLnnnn.F3Y` 是 F3YG **v2**，80-byte header + 614400-byte 原生 YUYV；記錄成功要求的 x,y,w,h、AE 等級、RAW CRC、原生 AE/AWB 狀態。舊 v1 的 64-byte F3Y 仍受支援。
6. 測光窗口不是「編輯 JPEG 的亮度遮罩」，也不是軟體測光反饋；設備可能在測光域內做其他廠商加權。從本版開始，用 paired RAW/YUV 的一致區域驗證它實際產生的曝光效果。為避免錯配，對非全畫面的窗口，F3E 中的舊版**全圖 RAW** Shot 三分位數曲線不予自動擬合；待取得 ROI 對齊的校準後再恢復。

## 現場最小資料建議

在固定灰階圖、固定距離、固定亮度的情況，先拍 Native AE 等級 0：全畫面 3 張、中央 320×240 3 張、中央 160×120 3 張；確認灰階條和外圍的暗區分別落入相機框中的哪些部分。之後挑一個窗口，對原生等級 -2、0、+2 各拍 3 張，直接觀察重初始化後是否重新定位到預期值。這是第一輪故障排查，不代表最終校準。

將同照片編號的 `/3ds/Film3DS/rolls/ROLLnnnn.DNG`、`/3ds/Film3DS/previews/ROLLnnnn.F3Y`、`/3ds/Film3DS/previews/ROLLnnnn.F3E` 以及 `/3ds/Film3DS/logs/film3ds.log` 打包回傳。**不要刪除既有 0059/0060 原件**。

## ROLL 解碼 IO 診斷

選照片後在 ROLL 按 `SELECT` 解碼；若仍出現 IO，畫面會寫 `Decode <stage>: IO (errno n)`，並在 `logs/film3ds.log` 輸出 `ROLL #nnnn decode failed stage=<...> status=IO errno=n; DNG unchanged`。

階段可能是 `DNG original path`、`DNG read / digest`、`RAW linear scene decode`、`F3C scene cache write`、`JPG encode/write`、`JPG readback` 或 `F3I index write`。這樣可直接判定是 SD FAT、快取檔、JPEG encoder 還是原件讀取，避免誤歸咎於新曲線。因為之前回收資料沒有解碼日誌，目前**未能證實舊版 IO 的根本原因**，不應聲稱問題已完全解決。

## 安全範圍

- 原始 DNG、SD 卡現有的 `.cube` LUT、先前 F3Y v1 檔案均不修改。
- F3YG v2 sidecar 只在 DNG 正式成功保存後由明確的 SELECT+R 收集，保存後 CRC 讀回驗證；原生 YUV 是快門前的最後完整一幀，**不是與 RAW 同步的幀**。
- 不改 Sensor AE 寄存器的低階時序、不導入循環重拍或軟體 AE。
- CI 通過代表 C 代碼、ARM11 打包、資料讀寫一致；API 測光窗口的實際亮度響應和 EV 等級仍須這次實機測試證實。
