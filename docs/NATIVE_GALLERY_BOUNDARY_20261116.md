# 原廠相簿載入邊界與像素解碼：第二階段

2026-11-16。沿用JP Camera `0004001000020400`／remaster0005的固定code SHA256。完成更深的靜態資料流追蹤及獨立C像素解碼，不建立或部署原廠hook；原始CXI、安裝title、相簿、照片及RAW原件不變。

## 已縮小的原廠資料流

```text
不透明64位媒體key
  → 原廠編碼資料查找（可能等待，不可當成非阻塞API）
  → 編碼緩衝區＋長度
  → 32位載入請求ID
  → 共用worker分派JPEG／MPO解碼
  → 消費端查狀態、收集結果
  → 釋放載入請求
```

上圖是固定版本原件的靜態資料流，尚未證明每個物件都專屬於相簿當前選中照片；縮圖、幻燈片和其他影像消費者可能共用相同模組。所有位址只作版本鎖定的分析證據，不是安全hook清單。

### 照片key與載入請求ID必須分開

- `0x002eeeac–0x002eeeb0`以兩個word複製8 bytes到消費物件`+0xc8`。
- `0x002096f4–0x00209714`以低word相減、高word帶借位相減作無符號64位排序，支持這是原廠不透明媒體key。
- `0x002ef09c`把這個key送進`0x0020b13c`；後者成功分支取出兩個欄位，沿呼叫鏈送入解碼器的輸入pointer／bytes。
- `0x002ef0cc`建立載入請求後，`0x002ef0d4`把返回值寫到`+0xc4`。之後狀態查詢和釋放使用這個請求ID。

**RAW關聯不能使用`+0xc4`的暫時請求ID。** 原廠64位key可作查找線索，但完整位元定義、跨SD／刪除重建後的穩定性未確認；長期關聯仍需自己的photo_id與內容hash。這次不把不明key轉成猜測的DCIM路徑，也不加入portable metadata schema。

### JPEG／MPO物件與所有權

RTTI鏈核對出`imgdb::JpegDecoder`、`imgdb::MpDecoder`、`notes::pnote::SceneBrowse`、`ScenePaint`。解碼器存在兩組建構路徑：

| 路徑 | JPEG | MPO |
|---|---|---|
| 由解碼器管理輸出 | `0x00255520` | `0x0022763c` |
| 接受外部輸出指標 | `0x001ffd54` | `0x001ffe2c` |
| 釋放路徑 | `0x002550c4` | `0x002275a8` |
| 取消請求 | `0x00240210` | `0x001ffd4c` |

建構器把輸入r1／r2保存到物件`+0x1c/+0x20`；JPEG header準備路徑讀取該pair。外部輸出建構路徑把`+0x52`清零，自管輸出路徑把它設1；釋放路徑會依這個旗標決定是否釋放`+0x54`等輸出，另釋放`+0x18`的工作區。不能把原廠自有指標交給我們的`free`，也不能讓解碼器釋放借用的暗房緩衝區。

共用worker在`0x002bb140`進MPO外部輸出建構器，在`0x002bb254`進JPEG外部輸出建構器，後續分別呼叫`0x002013e8`與`0x0022bab8`。這證明兩種解碼路徑共享載入框架；仍未證明輸出格式一定是RGB24，不能將原廠輸出直接套入我們的RGB24介面。

取消函式只沿物件偏移設定請求byte；取消不等於完成，也不等於指標已可釋放。`0x002ef150`消費路徑收集結果，並在`0x002ef220`按請求ID釋放。原廠`0x0020b13c`和`0x001fffa0`包含依狀態等待的迴圈；後續新入口不直接複製這種等待，更不能把它偷偷放進UI每幀呼叫。

### 狀態回呼與返回路徑

`0x0034b200`保存`0x002ef0f0`，`0x0034b208`保存`0x002ef150`，兩者後續word均為0。相關程式以成對word載入／保存回呼狀態，所以僅掃描直接BL不足以追出所有呼叫。

SceneBrowse與ScenePaint有各自vtable；SceneManager另有場景虛擬清理／析構流程。這說明暗房頁面需要參與既有場景生命週期。**尚未定位可直接使用的「開暗房／返回同一選中照片」入口**；不以換vtable或在共用worker攔所有解碼冒充完成整合。

完整可重跑證據：`build/native-camera-integration-20261116/gallery-static-v3.json`。`tools/native_gallery_audit.py`核對原件hash、4個RTTI鏈、13條直接呼叫、狀態回呼及指令anchors；輸出明示`hook_approved=false`、`selected_photo_identity_proven=false`、`return_path_proven=false`。校驗器重建全部證據，拒絕錯版本、未知來源、錯置proof、被改寫的邊或擅自授予hook批准。

## 已完成C像素解碼介面

新增`include/native_decode.h`、`source/storage/native_decode.c`：

```c
FilmStatus film_native_jpeg_decode(const char *source,
    const uint8_t *data, size_t bytes, unsigned member,
    uint8_t *pixels, size_t stride, size_t capacity,
    unsigned *width, unsigned *height);
```

接受完整JPEG／MPO、明示member0或1，先通過既有封裝verifier，再同步解碼成呼叫端持有的RGB24 rows。caller維持編碼資料和輸出buffer生命週期；函式不建立task／thread、沒有queue、沒有相機／檔案IO、沒有重試。兩眼可依次解碼，控制峰值記憶體；解碼產物可再交給現有暗房RGB後段，但這輪尚未把它接入原廠UI。

- baseline、progressive、grayscale→RGB均支援；CMYK等格式明示不支援。
- 最大786432 pixels，覆蓋原廠內附1024×768樣本；更大來源拒絕，不偷偷縮圖。
- 驗證member、row stride、capacity、算術溢位與輸入／輸出／尺寸指標重疊。
- 輸出為編碼raster方向，不執行Exif旋轉或感測器RAW解碼。
- libjpeg警告和損壞資料均拒絕；失敗尺寸清零，pixel buffer可能有局部資料，只有FILM_OK才可顯示。尺寸指標alias輸入／輸出時不寫入任何資料直接拒絕。
- virtual-array預算4MiB；這不是整個解碼器的硬性heap上限。使用已核對的libjpeg `jmemnobs`構建，禁止磁碟backing store。內存／無backing-store錯誤回傳FILM_NOMEM，無磁碟或低品質fallback。
- 原廠MPO樣本兩張已由實際C函式解碼，逐像素與Pillow單JPEG解碼比較最大差≤2碼值；保留row padding及首尾canary。沒有把Pillow解碼當成C模組成功。

新增compute來源`film-native-jpeg-decode/v1`，與metadata／封裝inspect的read來源分開。沒有用原本`film-native-media-inspect/v1`偷渡像素compute，也沒有假LaneAdmission或診斷override。

## 契約與驗證

1. Runtime分類：新增inline顯影前段compute及本機靜態審查side-effect。
2. 先在Film3DS `tools/camera_runtime_contract.py:CONTRACT_MATRIX`登記`film-native-jpeg-decode/v1`（compute=true，LaneAdmission=false，override=null，proof=none）與`film-native-gallery-audit/v1`（compute=false，proof=local_trace），再更新verifier與實作；同步生成header。
3. Verifier新增原件資料流核對、strict JPEG像素解碼／邊界及錯誤分類。既有採集／admission規則沒有放寬。舊frozen matrix可生成header，但不會因此接納新的native來源；新增拒絕測試覆蓋此相容性。
4. Runtime-internal欄位沒有新增到metadata／公開schema。上述native物件偏移只是本機靜態分析結果，不讀取或輸出實際runtime queue／worker狀態。
5. 測試現在為19項native integration、3項gallery audit，及6項既有package契約。新增像素decode比對、CMYK拒絕、無量化表拒絕、來源冒充拒絕、容量／alias／pixel budget、實際4:4:4 progressive超virtual-array預算的FILM_NOMEM、舊header不接納新來源、錯版／偽hook聲明拒絕。沒有放寬舊測試。

執行：

```sh
make test-native-integration
python3 tests/camera_ui_package.py
python3 tools/native_gallery_audit.py --code build/hardware/native-trace/cxi-jp/exefs/code.bin --out build/native-camera-integration-20261116/gallery-static-v3.json --verify
make build/unit build/darkroom-test CC=/usr/bin/cc CPPFLAGS='-Iinclude -I/usr/local/include -D_POSIX_C_SOURCE=200809L' LDFLAGS='-L/usr/local/lib'
./build/unit
./build/darkroom-test
```

結果：native integration 19項、gallery audit 3項、既有package契約6項均通過；現有核心unit與暗房回歸亦通過。第一次沿用主機環境的LLVM／include設定建置時找不到jpeglib.h，改用上列明示本機libjpeg路徑後成功；未為此改動產品Makefile的全域編譯設定。

新增模組亦使用既有禁網devkitARM容器與`-Wall -Wextra -Werror -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard`編譯成`build/native-camera-integration-20261116/native_decode.arm.o`；不是原廠注入或真機解碼結果。SDK source/CMake build已確認連結`jmemnobs.c`，開backing store會直接報錯。

未執行以下Vision-product命令：Film3DS沒有這些測試，且本輪不修改其治理的Vision scheduler／metadata邊界：

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

沒有整機／原廠UI部署驗證，沒有完整產品或網路測試。原廠相簿選中項與64位key的對應、原廠pixel格式／GPU布局、明示開暗房動作及返回恢復、SD照片／savedata備份、Luma版本／撤回仍待實證。RAW快門、MPO重新編碼與photo_id映射尚未接入。下一個入口應從明示相簿動作及其key快照建立，避免攔截共用worker而誤處理縮圖／其他照片。
