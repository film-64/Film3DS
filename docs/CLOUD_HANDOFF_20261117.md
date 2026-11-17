# Film3DS 跨環境接手說明 — 2026-11-17

本分支是進一步改善 RAW 暗房前的程式快照，目標產品是 3DS App。本機／雲端的合成測試與離線工具只協助檢查程式，不取代實機驗證。

## 已完成與目前限制

- 正常 App：L 開 REAR1，A/R/拍攝觸控取得同一次曝光的原生 YUYV 與旁路 RAW10（high8 + low2）。曝光鎖定後 coarse、fine、四通道 gain 以既有 hold 核對，不另設診斷固定曝光。
- RAW 原件先保存為 SD `3ds/Film3DS/rolls/ROLLnnnn.F3S`；使用原生幀產生 `previews/ROLLnnnn.JPG`，F3RI 索引綁定 RAW 指紋和 JPEG CRC。ROLL 只讀 JPG，RAW 僅進暗房才解碼。
- 真正按鍵拍攝 #0072 的 RAW/JPG/索引已在線取回並核驗；使用者確認暗房可顯示預覽及保存。證據摘要在 `evidence/2026-11-17/`，不含任何影像。
- 機內 RAW ISP 使用基本雙線性去馬賽克；黑位0、白位1023、identity CCM 尚未標定。RAW 準備階段固定 shoulder=.75，後續畫布仍有0..1限制。高光保護、線性分區曝光/WB、曲線與 LUT 順序是下一輪工作，**尚未實作完成**。
- 目前 App 未主動啟用 New 3DS 804MHz/L2。GPU 已用於部分縮放與顆粒混合；顆粒場生成及部分處理仍在 CPU。不要把「已有 GPU 入口」描述成全管線 GPU 加速。
- 最新實機 r12：SHA256 828d6ffc062d8904748a02d444af707c63d17c0bfa10ee38ff8f24d7ea7b5073。這是本地帶私有素材的已部署包之身分，不保證乾淨雲端建置會產生相同 binary。

## 程式入口

|內容|位置|
|---|---|
|正式快門與保存 worker、ROLL、暗房 UI|source/platform/main_3ds.c|
|旁路採集與狀態回覆解碼|source/platform/raw_camera_3ds.c、source/camera/raw_capture.c、source/camera/hardware_transport.c|
|原生 JPG 與 RAW 配對、JPEG-only ROLL|source/storage/roll_preview.c|
|RAW 解包、ISP、線性曝光|source/image/raw_darkroom.c、source/storage/raw_export.c|
|WB、曲線、LUT|source/color/white_balance.c、source/color/color.c|
|顆粒與 GPU 混合|source/image/layered_grain.c、source/platform/grain_gpu_3ds.c|
|運行時合約|tools/camera_runtime_contract.py、include/film_runtime_contract.h|
|已驗證按鍵工作流紀錄|docs/SHUTTER_NATIVE_RAW_20261117.md|

## 雲端／Linux 檢查

```sh
sudo apt-get update
sudo apt-get install -y build-essential libjpeg-dev python3-pil python3-numpy python3-scipy python3-capstone
make -j2 test test-raw-darkroom test-native-integration
python3 tools/camera_runtime_contract.py --check
```

`make test` 包含只監聽 localhost 的測試，執行環境需允許本地 TCP。部分私有 CXI／顆粒參考測試缺素材時明確 skip。額外研究腳本可能需要 matplotlib 或 OpenCV，並非正常 App 建置的必要依賴。不要執行 NetLoader/校準 session 命令假裝取得了實機結果。

3DS 建置沿用 `.github/workflows/ci.yml` 的 pinned devkitARM 容器，或具備 devkitPro/3ds-dev 的環境：

```sh
export DEVKITPRO=/opt/devkitpro
make -j2 3ds
python3 tools/verify_3dsx.py Film3DS.3dsx
sh tools/verify_arm11.sh Film3DS.elf "${DEVKITARM:-${DEVKITPRO}/devkitARM}/bin/arm-none-eabi-"
```

首次建置由既有 `tools/build_arm_jpeg.sh` 取得鎖定版本的 scalar JPEG 來源，需要下載權限；未上傳本地編譯快取。

## 既有在線回收

正常 App 在 VIEW/ROLL 保存完成後按 SELECT+START，把選中原件/JPG/索引/日誌凍結並使用既有 FILMRAM HTTP5000。START 返回 App。底層 headers 是原樣複本，provenance SHA 可核對。原有 host helper 模組已原樣收進 tools，來源摘要見 `docs/EXISTING_TOOL_SOURCES.json`。

```sh
python3 tools/retrieve_probe_ram.py DEVICE_IP --out NEW_RECOVERY_DIRECTORY
```

原工具的 `retrieve(..., chunk=16384, recovery_policy="frozen-resume-v2")` 支援對同一快照核對後有界續傳，不能重新採集替代失敗資料。完整歷史日誌在本輪未完全取回；RAW/JPG/索引三個關鍵檔案已驗證。早期新寫的 raw_capture_qa 工具也保留作歷史程式，但它的反向 socket 全量回收未通過，不作正式驗收路徑。

## 素材與外部環境

此分支**沒有** RAW/F3S/BIN、照片、截圖、TIFF、F3GT紋理、私人 LUT/cube、CXI/ROM、反組譯 binary、模型、3DSX/ELF、建置快取或憑證。保留三個小型 F3G1 數值統計 profile，屬必要參數，不是圖像。

因此雲端可編譯、跑合成測試及修改演算法，但不能重現未提供的實拍對照或私人 LUT 視覺效果。沒有 F3GT 時使用現有統計顆粒路徑；未提供私有 cube/F3P 時不會嵌入本地的8款 look。不可把缺失素材改成偽造通過。

歷史曝光/原廠 Camera 研究文件含當時的本地路徑；路徑不是雲端可用資源。歷史硬體 session 仍需要原有 probe 裝置程式、實機、SD 與除錯服務；本快照沒有打包這些外部 binary。原廠 CXI 測試缺私有 fixture 時應 skip，而非下載 ROM。

## 修改界線

本次只做快照、補齊既有 helper 程式與接手文件，沒有新增 runtime 行為。後續更改必須先更新 Film3DS CONTRACT_MATRIX，再改 verifier/實作與契約測試；runtime-internal 狀態不得寫進 portable recipe。使用者提供的 Vision-product 三條 pytest 不存在於本專案，不得以空測試代替。

## 本次同步驗證

在獨立工作樹執行 `make -j2 test test-raw-darkroom test-native-integration`（使用本機 cc/libjpeg 路徑）成功，包含 localhost HTTP、RAW/ROLL/WB/暗房、原生媒體及 package checks；缺私有 fixture 的測試明確 skip。`git diff --cached --check` 通過。同步過程不修改相機/ISP runtime、CONTRACT_MATRIX、verifier 或 runtime-internal 欄位；本分支保留的是已驗證的現有版本。

GitHub Actions 保留倉庫既有 workflow。額外 RAW/native integration 的完整依賴與命令列在上方，供雲端工作環境直接執行。現有 OAuth 登入不具 workflow 修改權限，因此本次不變更 CI；不能用本地結果冒稱雲端 CI 已通過。
