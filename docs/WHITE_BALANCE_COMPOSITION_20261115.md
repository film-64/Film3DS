# 暗房白平衡與二次構圖

此版本使用上一個已驗證暗房包的相機基線，僅覆蓋明確列出的暗房檔案和 UI 區塊；9 個相機來源／標頭逐檔 SHA256 保持一致。

## 操作

列表依次為 Aspect ratio、Composition、Image size、White balance、WB shift，再接膠片模擬與原有曝光、曲線、顆粒、飽和度和銳化等設定。

- 白平衡：預設 Original 不調整。左右進入 6500 K，再以 100 K 步進調整 1000–10000 K；在 1000 K 向左或在該列按 Y 回到 Original。沒有水下檔。
- WB shift：左右打開二級窗口。19×19 漸變色格，水平 R−／R+，垂直 B−／B+，中心為零。方向鍵或點選色格；A 套用、X 取消、Y 歸零。移動時以原 F3S 重新產生低解析度全畫布顏色預覽；A 確認後重新處理原尺寸影像。這裡的 A 不保存 JPEG。
- 畫幅比例切換後，恢復該比例最大、置中的 100% 框。
- Composition 左右每次 1%，範圍 1–100%；百分比同時作用於寬高。4:3 原圖 640×480 的 50% 框為 320×240，70% 框為 448×336。像素取整以整數比例單位完成，連續縮放維持中心，不累積取整位移。
- 按住 B＋方向鍵，每次移動 1 個原圖像素；B＋搖桿，每次固定 10 像素，無加速，剩餘距離不足 10 時貼邊。框限於原 F3S 的有效尺寸。B 在暗房不返回；X 換頁。
- A 保留原圖全尺寸的完整處理畫布並顯示完整預覽，疊加構圖框。改構圖不會用已裁切的預覽再次裁切，也不會累積插值。
- 只有單獨 SELECT 保存；其他鍵繼續原本操作且不保存。幾何調整沿用完整已處理畫布，顏色調整使快取失效，需 A 重新預覽。
- 保存時才按當前框裁切，再插值至 Image size 指定的尺寸。50% 框輸出仍可為 640×480；Image size 和框的百分比分開設定。銳化、顆粒在輸出尺寸下計算。

## 白平衡方法與數據

這是已處理 F3S 的軟體色彩適應，不是 RAW 或相機感光元件校準的色溫重設。使用 Planck 輻射積分、CIE 1931 2° 配色函數取得源白點，以 Bradford 色適應轉至 D65；在線性 sRGB 中執行後，接入既有曝光與 LUT。原始模式且偏移為零時保持精確旁路。

R/B 偏移每格對應紅／藍通道 1/12 EV 相對增益，按 sRGB 亮度權重正規化，範圍各 −9 至 +9。這是本專案明確定義的步長，未聲稱復刻富士未公開的演算法。極端色溫仍受已處理輸入及 sRGB 色域限制；沒有實測色卡精度保證。

數據來源：CIE (2019), *CIE 1931 colour-matching functions, 2 degree observer*, DOI [10.25039/CIE.DS.xvudnb9b](https://doi.org/10.25039/CIE.DS.xvudnb9b)。[官方數據頁](https://cie.co.at/datatable/cie-1931-colour-matching-functions-2-degree-observer)、[官方 CSV](https://files.cie.co.at/Publications-datasets/CIE_xyz_1931_2deg.csv)。原數據 CC BY-SA 4.0；`source/color/cie1931_5nm.h` 為 360–830 nm 每 5 nm 取樣的改編表，該表亦以 [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) 提供，保留數據署名。原 CSV MD5：17cca777db64b17170f06f67ce9d3ab7；SHA256：fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1。

## 契約與驗證

分類：暗房離線 CPU 計算、全畫布預覽和 JPEG 輸出；沒有新增相機調用、排程、佇列或重試。Film3DS 的 `tools/camera_runtime_contract.py` 已先更新暗房、保存與新增的 WB 臨時預覽來源；來源驗證仍對未知名稱 fail closed，沒有放寬 verifier。沒有新增 runtime-internal 欄位；色溫、偏移、構圖屬可攜式影像配方，非執行狀態。非預設的新設定以 F3S v6 儲存，舊版 v1–v5 繼續可讀寫。

新增 `tests/white_balance_composition.c`：91 個色溫設定、已知 Illuminant A 白點、Bradford 中和、361 個色格方向、零旁路、無效範圍、50/70% 精確尺寸、1/10 像素移動、貼邊、重置、完整預覽與獨立輸出、不可變畫布保存、v6 roundtrip 與越界拒絕。現有契約與格式版本測試已同步更新。

驗證指令為 `make test CC=/usr/bin/clang CPPFLAGS='-Iinclude -D_POSIX_C_SOURCE=200809L -I/usr/local/opt/jpeg-turbo/include' LDFLAGS='-L/usr/local/opt/jpeg-turbo/lib'`，另用固定 devkitARM 容器編譯 3dsx，檢查 ARM11 JPEG 相容性及所有內嵌 LUT／顆粒資源。真機按鍵、速度和觀感仍需實機預覽確認。

## 本次構建結果

完整 `make test` 通過，包含新增 WB／構圖測試、契約與可攜式配方檢查、既有暗房曲線和銳化測試、F3S／JPEG 整合、模擬 HTTP 與硬體報告解析測試（沒有呼叫真機相機）。ARM11 相容性及 8 組內嵌 LUT／顆粒資源驗證通過。

最終包：`build/darkroom-wb-compose-20261115-r2/Film3DS.3dsx`。SHA256：`0b6985a03f92d6000eb85e6e847c6b378a54b276da5004d985ec2002508111ea`。構建及傳輸證據位於同目錄的 `package.json`、`build.json`、`deployment.json`；完整本機與容器日誌為 `build/wb-compose-host-test.log`、`build/wb-compose-arm-build.log`。
