# 原廠 Camera 接入：首個實作階段

後續進度見[相簿邊界與像素解碼第二階段](NATIVE_GALLERY_BOUNDARY_20261116.md)。下列14項測試為第一階段當時的紀錄。

2026-11-16。目標是保留原廠3D／相簿，在單攝加入RAW，並接入機內暗房。本階段完成校驗備份及可在ARM11編譯的唯讀JPEG／MPO入口；沒有安裝補丁，沒有修改已安裝title、原件CXI、照片或相簿。

## 恢復能力與開發路線

原始CXI及解壓code.bin的hash與先前審查一致。已建立第二份本地副本：

`build/native-camera-integration-20261116/original-backup/`

包含original.cxi、original-code.bin、original-exheader.bin、逐檔大小／SHA256、manifest完整性校驗及RECOVERY.txt。目錄位於既有忽略的build下，不提交或散發原廠程式內容。備份工具拒絕不同版本、已存在目錄、缺檔、額外檔案、符號連結、內容變更、未知來源、錯置／偽造proof。它不會覆寫或自動續建半份備份。

**這是程式內容備份，不是完整還原包。** CXI不是已驗證可安裝的CIA；不含DCIM、原廠相簿savedata、系統title安裝資訊／ticket或整機NAND備份。本次沒有執行重裝或驗證安裝式還原，不能以此承諾能恢復所有問題。

開發採版本鎖定的可移除載入補丁，保留已安裝原程式。Luma上游支援title的code.ips／exheader載入，但本機Luma版本、既存補丁及實際撤回仍待確認。若只加載入補丁，退出相機、撤下本次補丁並重新啟動，是優先撤回路徑；若App無法啟動，可透過電腦操作SD。必須保留原先已有的補丁，不能整個刪除title目錄。

移除程式補丁不能逆轉已寫入的照片／相簿資料或相機service狀態。首次上機前仍需：備份DCIM、相簿savedata及既有overlay；確認安裝版與dump版本一致；驗證原廠正常啟動、正確退出與撤回流程。相機狀態異常時需退出／重啟後確認，不能只憑CXI已備份就忽略恢復契約。本階段尚未產生或部署任何code.ips／exheader覆蓋檔。

來源：[Luma上游patcher](https://github.com/LumaTeam/Luma3DS/blob/master/sysmodules/loader/source/patcher.c)，2026-11-16核對。

## 已實作的媒體入口

新增`include/native_media.h`、`source/storage/native_media.c`：

```c
FilmStatus film_native_media_inspect(const char *source,
    const uint8_t *data, size_t bytes, FilmNativeMedia *out);
```

來源只接受`film-native-media-inspect/v1`。函式讀取呼叫端持有的完整編碼緩衝區，回傳一張JPEG或兩張MPF索引JPEG的offset／bytes／尺寸；不分配記憶體、不讀寫檔案、不接觸相機，也不進行像素解碼。輸入生命週期由呼叫者維持，失敗時輸出清零。這是後續原廠相簿→暗房的C介面基礎，尚未hook到原廠選中照片物件。

檢查JPEG marker／SOF／SOS／EOI、尺寸／長度上限、MPF TIFF兩種byte order、兩張MP entry、成員邊界／連續性、第二張MP屬性及有界IFD鏈。損壞MPF不退化成單JPEG；未登記來源、scheduler字串、startup override和其他已登記但不適用的來源均拒絕。

原廠RomFS內`/debug_mpo_/resizable.mpo`提供了實際相容性案例：389379 bytes，兩張1024×768。它是App內附樣本，不是本次重新拍攝的照片，不能代表所有原廠拍攝模式已覆蓋。第一個MP entry包含EOI後31個零值填充，第二張帶獨立MP屬性IFD。這些規則先明示進matrix再實作：只允許第一個索引成員的0..63個零值填充，輸出JPEG span排除填充；第二張可有有界屬性IFD，但不能含另一個MP Index。禁止循環IFD、非零／過長填充、未索引間隙、重疊／越界成員、超過兩張及不匹配尺寸。

輸出span供明確的單JPEG解碼器使用。第一張仍可能保留指向完整MPO的MPF標記，不能將切片直接當成已重建的獨立MPO檔案保存。測試使用Pillow的單JPEG解碼類逐張解碼並確認尺寸，避免容器自動判型造成誤解。

解析器只驗證封裝和marker結構；壓縮熵資料是否有效仍由之後的JPEG解碼器驗證。本階段未實作像素解碼、MPO重新編碼、RAW關聯索引、原廠UI／GPU交接、照片登記或刪除。

## 契約分類

1. `film-native-recovery-archive/v1`：本機side-effect邊界，compute=false、LaneAdmission=false、override=null、proof=local_trace。只建立新備份目錄，不部署、不改已安裝title、無重試。
2. `film-native-media-inspect/v1`：唯讀封裝／描述建立邊界，compute=false、LaneAdmission=false、override=null、proof=none。它不解碼／顯影像素，不產生相機或模型工作。
3. 兩列先加入Film3DS權威`tools/camera_runtime_contract.py:CONTRACT_MATRIX`，再實作與更新產生的`include/film_runtime_contract.h`。沒有改Vision-product的矩陣，也沒有建立冒名Vision檔案。既有來源條目保留。
4. Verifier有新增：備份的版本、完整性、來源及local proof檢查；C入口的來源和封裝驗證。未更改既有採集／admission verifier，未削弱fail-closed。
5. Runtime-internal欄位未新增；媒體尺寸／offset為借用資料描述，備份診斷只存在本機manifest，不進F3S／portable配方。

## 驗證與命令

```sh
python3 tools/native_recovery.py create --cxi '<本機原始CXI>' --code build/hardware/native-trace/cxi-jp/exefs/code.bin --out build/native-camera-integration-20261116/original-backup
python3 tools/native_recovery.py verify build/native-camera-integration-20261116/original-backup
python3 tools/camera_runtime_contract.py --check
make test-native-integration
python3 tests/camera_ui_package.py
```

備份建立／重驗通過；新測試14項通過，包含真實原件、JPEG baseline／progressive、MPF兩種端序、原廠MPO實際像素解碼、padding／屬性IFD、截斷／錯誤索引／IFD循環、未知來源／冒充來源、1000輪有界變異、備份偽造／錯置proof及完整性拒絕。既有package契約測試6項通過。

新增模組使用既有快取SDK `devkitpro/devkitarm@sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b`，禁網容器、來源唯讀掛載，以以下參數編譯通過：

```sh
arm-none-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard -Iinclude -c source/storage/native_media.c -o /out/native_media.arm.o
```

第一次ARM編譯指出SOS長度比較的signedness警告，已使用明確unsigned運算修正；重新編譯通過，沒有關閉警告。object位於忽略的`build/native-camera-integration-20261116/`。這是ARM11 object編譯，並非原廠App注入／連結或真機驗收。

未執行以下Vision-product命令：本次變更位於Film3DS，不修改其所治理的RegionTask／TaskScheduler／metadata邊界，而且這些測試在Film3DS不存在。

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

沒有執行完整Film3DS真機／產品測試；此次使用新增唯讀模組、備份工具、既有package契約與ARM編譯的針對性驗證。所有既有dirty來源保留，沒有commit／push、NAND寫入、title重裝或部署。

## 下一個接入點與剩餘風險

下一個里程碑是鎖定本版相簿「選中項→影像載入→返回」的物件與生命週期，再把此C入口接到暗房。其後才是另存成片／相簿登記、RAW快門與photo_id關聯。任何新的解碼／顯影compute或保存／刪除side-effect入口，都要先新增相應matrix規則、verifier與測試，不能沿用本次read-only來源越權。

尚未測：本機撤回流程、DCIM／savedata備份、原廠選中照片hook、實際heap／服務ACL擴充、暫停與恢復原廠相機、像素顯影、MPO輸出、跨地區／版本。原始CXI只證明備份內容可重讀，不能被標為「整機恢復已測」。
