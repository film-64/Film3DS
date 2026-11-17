# RAW ISP 與多層顆粒：2026-11-16

本次完成可編譯到 ARM11 的 C 核心、主機端參數入口及實際 RAW 輸出驗證。既有 3DS 選片介面仍讀 F3S/YUYV；RAW 選片、參數 UI、RAW 與相簿關聯儲存、GPU 加速和實機速度測量尚未接入。原廠 Camera 修改保持暫停。

## 處理與輸出

`high8/low2 wire → uint16 Bayer → 黑白位/指定壞點修復 → 去馬賽克與白平衡 → 線性 EV/CCM → 可選高光肩部 → sRGB transfer/色度降噪 → 共用 float RGB → 暗房/LUT → RGB16 canvas → 銳度/輸出 → 多層顆粒 → JPEG/PPM`。

本感測器目前驗證的 wire 是每像素兩 byte，`(wire[2*i] << 2) | wire[2*i+1]`；不是 MIPI 的四像素五 byte 格式。低 byte 超過 3、資料長度錯誤、奇數尺寸和未知 source 均拒絕。C 解包測試涵蓋全部 1024 個碼值。

RAW 輸入保持不可變；每次輸出只做一次 ISP。套用及不套用 LUT 都從同一 float RGB 分支，沒有先轉 8bit 再跑 ISP。後段沿用現有暗房色調、LUT profile 與銳度核心；RGB16 canvas 到最終 RGB24/JPEG 才量化輸出。PPM 也是最終 RGB24，不是線性或 16bit 檔案。重新改線性 EV、WB、黑白位或 CCM 需由原 RAW 重新準備 ISP。

每次明確指定一個新輸出目錄，產生：

| 檔名 | 內容 |
|---|---|
| `plain.jpg` / `plain.ppm` | 共用暗房參數，不套 LUT、不加顆粒 |
| `look.jpg` / `look.ppm` | 同參數加指定 LUT，不加顆粒 |
| `plain-grain.jpg` / `.ppm` | 不套 LUT，加多層顆粒 |
| `look-grain.jpg` / `.ppm` | 套 LUT，加多層顆粒 |

未提供 LUT 時只輸出 plain 兩種版本。已存在的輸出目錄一律拒絕；失敗保留局部檔案、回傳非零，不產生成功標記，不自動重試或續跑。

## 顆粒

`source/image/layered_grain.c` 有三個尺度：細粒、結團、低頻密度起伏。亮度以連續的暗部／中灰／高光權重混合，搭配 F3G1 九階灰的 RMS 與相關性資料；在 linear-light 以對數密度增益作用並做均值偏差補償，接近黑白端點時限制幅度。RGB 共用密度增益，避免額外生成獨立彩色噪點。固定 seed 可重現；amount=0 完全不改像素。

現有私有 `build/hardware/dxo-grain/DxO-Pan400.tif` 與 `romfs/grain/medium.f3g` 的九階灰測量吻合。`tools/build_grain_texture.py` 從每階中心取 128×128 實際紋理、扣除亮度平面並正規化，產生 294972-byte F3GT atlas（九個 int16 residual band + tone codes + CRC32）。C loader 驗證長度、CRC、碼值排序、零均值與單位 RMS。

渲染以雜湊偏移、旋轉、鏡像、局部形變及重疊柔邊權重取樣實際紋理；混合邊界受低頻場調制，沒有硬矩形遮罩。這是連續不規則權重與紋理重組，沒有建立多邊形裁切網格。細／粗範例目前使用各自 F3G1 統計與程序生成場；它們的原始 TIFF 未保留在目前目錄，不能宣稱取用了其真實紋理。

標準範例參數：amount=.35、size=1、coarse=.5、shadow=.8、mid=1.1、highlight=.7、seed=1616。合成九階灰在 amount=.5 時，暗部 display code 22 的 RMS=2.50，中灰 137 為 6.32，高光 252 為 1.48；各區均值偏移小於 0.57 code。這證明目前響應與端點收斂，不是膠片物理層或感測器的標定。

目前是 CPU 參考實作，沒有 GPU pass、GPU 正確性或真機互動幀率結論。C float 準備影像每像素 12 byte，RGB16 canvas 每像素 6 byte；色度降噪會暫存另一份 float RGB，此外還有 RAW、影像與 LUT 緩衝。像素數上限不是總記憶體上限，實機接線前需量測峰值與延遲。

## 使用

```sh
make build/filmraw CC=/usr/bin/cc \
  CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' \
  LDFLAGS='-L/usr/local/lib'

./build/filmraw INPUT.bin PARAMS.txt NEW_OUTPUT_DIR romfs/grain/medium.f3g \
  LOOK.cube LOOK.f3p build/raw-darkroom-20261116/pan400.f3gt
```

LUT 的 cube/f3p 成對提供；最後的 F3GT 可省略，省略時只用樣本統計程序生成。無 LUT 的形式為 `filmraw INPUT PARAMS NEW_DIR PROFILE.f3g [TEXTURE.f3gt]`。

參數檔以 `F3RAW1` 開頭。width/height 必填，偶數且各至少 4，總像素最多 786432。其餘預設見 `tools/raw_darkroom_host.c`：

```text
F3RAW1
width 640
height 480
cfa 0
black 0 0 0 0
white 1023
wb 1 1 1
ccm 1 0 0 0 1 0 0 0 1
exposure_ev 0
shoulder 0.75
denoise 0.5
highlights 0
midtones 0
shadows 0
saturation 0
sharpness 1
lut_strength 1
grain_amount 0.35
grain_size 1
grain_coarse 0.5
grain_shadow 0.8
grain_mid 1.1
grain_highlight 0.7
seed 1616
# bad_pixel 2 2
```

CFA: 0=RGGB，1=BGGR，2=GRBG，3=GBRG；black 的四值按左上、右上、左下、右下相位。EV 範圍 ±8、WB .125..8、shoulder 0..0.99、denoise 0..1、sharpness -5..5、grain amount/coarse 0..1、size .25..8、三段 response 0..2。bad_pixel 可重複列不同座標，最多 4096；重複座標拒絕。修復只用未列壞點的同相位鄰居，不會自動把彩點加入永久壞點表。未知欄位、重複單值欄位與非有限數字均拒絕。

本次 RGGB 仍是候選，black=0、white=1023、identity CCM 沒有標定，範例 WB 是既有場景估計。降低曝光不能救回已飽和的 RAW 資訊，增加曝光也不會新增暗部訊號。殘餘固定彩點的跨影像定位與永久壞點表仍需另外驗證。

## 證據

實際資料：`../Film3DS-audit-20261115/raw-response-run05/recovered/c00-raw-00.bin` 與 `c00-raw-01.bin`。比較工具先檢查接收紀錄 SHA256/CRC32，再有限次執行 8 組實拍參數及 1 組合成灰階，共 9 次。完成後確認輸入、LUT 和 profile hash 未變；每個輸出保留命令、參數、hash 及成功標記。凍結的 host executable 位於範例根目錄，避免後續重編譯混淆該次證據。

成果：`../Film3DS-audit-20261115/raw-darkroom-texture-c-20261116/`，包含四版本總覽、EV -1/0/+1、細中粗顆粒及 2 倍細節比較、`report.json`。F3GT 位於 `build/raw-darkroom-20261116/pan400.f3gt`，來源與 hash 見同目錄 `pan400.json`。私有樣本及衍生紋理維持在本地 build/artifact 目錄。

## Runtime 治理與驗證

本工作目錄是 Film3DS；使用者提供的 Vision-product `roi_app` 治理實體檔案不在此目錄。本次遵循相同的先登記、再驗證、最後實作測試次序，使用既有 Film3DS registry，不建立另一套 scheduler。

1. **分類**：新增七個明確入口：`film-raw10-unpack/v1`、`film-raw-isp/v1`、`film-raw-grade/v1`、`film-grain-layered/v1`、`film-raw-darkroom-export/v1`、`film-raw-darkroom-comparison/v1`、`film-grain-texture-build/v1`。皆是同步、明確呼叫的計算工作，不建立 RegionTask/TaskContext/TaskScheduler，不需 LaneAdmission，無 override；前三個和 grain 的 proof=none，其餘為 local_trace。無新排程、queue、retry、suppress、capture 或 deployment。
2. **CONTRACT_MATRIX**：`tools/camera_runtime_contract.py` 先登記上述來源、輸入、輸出、限制與失敗方式，再生成 `include/film_runtime_contract.h`。歷史凍結矩陣不會因此接受新來源。
3. **Verifier**：新 C API 驗證精確 source 與 generated registry，未知來源拒絕；新參數白名單、RAW 低位驗證、alias 拒絕及 F3GT CRC/統計驗證。既有 admission/封裝規則沒有放寬。
4. **Runtime-internal fields**：中央集合新增 `isp_preparations`，僅用於本地執行完成紀錄；不納入照片參數或既有可攜封裝。測試遍歷中央集合，確認所有 internal 欄位不能當成 RAW 參數注入；沒有複製 denylist。
5. **新增測試**：`tests/raw_darkroom.c`（全部 RAW 碼值、四種 CFA、測量通道保留、線性 EV/WB、指定壞點、輸入不可變/alias、identity/非 identity LUT、seed/均值/RMS/端點）及 `tests/raw_darkroom_export.py`（8 個真實 C CLI 測試，包含四輸出、參數效果、覆寫拒絕、未知/偽 proof/internal 注入、CRC、真實 TIFF 匹配與紋理取樣）。
6. **已執行**：`make test-raw-darkroom`（上述 host compiler 參數）通過 C 測試與 8 個 Python 測試；`./build/unit` 16 組通過；`./build/darkroom-test` 6 組通過；`make test-native-integration` 19+3 通過；`python3 tests/camera_ui_package.py` 6 個通過；registry `--check` 通過。devkitARM 對兩個新核心以 ARMv6K/VFP hard-float、`-Wall -Wextra -Werror` 編譯通過。ASan/UBSan host exporter 對 640×480 實拍 RAW、真實 LUT、F3GT 四輸出通過，未報錯。
7. **未執行及風險**：未執行 `pytest tests/test_runtime_contract_boundaries.py -q`、`pytest tests/test_runtime_projection.py -q`、`pytest tests/test_metadata_runtime_package.py -q`，因三檔及對應 Vision-product 模組不在 Film3DS。未做完整機內 GUI、SD RAW 索引/關聯、GPU、相機校色、永久壞點表和實機性能驗證。上述新增 registry/core/tool/test/Makefile/header 是本次改動，既有其他 dirty 檔案不作清理或覆寫。
