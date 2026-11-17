# 15fps 拍攝與沖洗 UI 整合版

## 目前部署：r5，相機與曝光控制分開

2026-11-15已將r5傳至192.168.0.69:17491，設備接受payload並收到啟動請求。
使用者已回報「有畫面，手動曝光可調」，確認r5的實體出圖與手動調整可用；
此為使用者操作回報，非EV／ISO或幀率量測。AE與切回手動的保持效果待確認。
r2–r4傳輸成功不能代表其相機功能通過。

此版恢復3f01442的原生開相機、DMA預覽、單張預覽備援與快照順序，透過既有
CAMU_SetFrameRate接口要求15fps。原生preview_poll不讀手動寄存器、不做曝光
狀態驗證；完成幀先按原順序重新掛接下一個DMA。

手動控制保留，由獨立`manual_exposure_3ds.c`接入cam:c積分／增益控制。
UI手動與軟體AE共用同一個寄存器寫入接口：Manual模式僅UI寫入，兩種自動測光
模式僅AE寫入；完成手動硬體handoff後原生AE關閉。初始／切檔穩定檢查放在
原生preview完成並重新掛接DMA後；曝光ready時不再逐幀讀寄存器。
曝光設定錯誤記錄並顯示為曝光錯誤，原生預覽保留；手動曝光未ready或讀回錯誤
時拒絕保存照片，不自動降級成未驗證的原生曝光。拍攝前再次核對手動控制。

Runtime分類：原生預覽、曝光設定、手動／AE寫入、拍攝均為獨立3DS compute；
不使用LaneAdmission或override，證據在local_trace。先更新矩陣，新增
`film-camera-native-preview/v1`、`film-camera-native-exposure/v1`、
`film-camera-native-capture/v1`及`film-camera-manual-setup/v1`，保留既有
`film-camera-exposure/v1`／`film-camera-capture/v1`的手動驗證。
驗證器允許登記的manual-setup來源，未知來源仍拒絕；未新增重試或排隊。
內部欄位`exposure`、`exposure_transport`、`exposure_trace`、`exposure_requested`
集中登記於RUNTIME_INTERNAL_FIELDS，不加入照片配方或portable資料。

工作區另有同時進行的暗房v4修改。為限制這次相機修復的範圍，r5使用r4凍結的
既有UI／8款LUT／顆粒資源，僅疊加相機與手動控制接線；工作區其他改動保留。
完整輸入雜湊見r5的package.json，沒有把其他未完成的暗房修改一起發布。

驗證：r5凍結來源成功完成ARM構建、ARM11指令／JPEG審查、3DSX與逐項ROMFS
嵌入檢查；同一凍結核心配合原版unit測試通過。獨立相機控制測試覆蓋手動來源、
七檔範圍、偽來源／漂移拒絕、RPC失敗與還原，通過；根目錄Python契約／打包
五項測試、生成標頭同步及diff檢查通過。新增原生邊界與手動單一owner的契約檢查。
目前全工作區make test未全綠：既有integration對暗房新檔案版本的unsupported
斷言失敗；未修改或放寬該測試。r5凍結舊UI的原版unit和相機控制測試另行通過。
Vision-product三項pytest在此倉庫不存在，仍未執行，命令見下文。

* binary：`build/camera-ui15-20261115-r5/Film3DS.3dsx`。
* SHA256：`64a85073f83d248f5faed60c6396495faace43b1a02e1fcec55436b53db65c28`。
* 構建／部署回執：同目錄`build.json`、`deployment.json`。
* 使用者觀察紀錄：同目錄`user-validation.json`。
* 待確認：自動測光收斂及切回手動保持；持續15fps與曝光標定仍未量測。
  ±3仍為名義相對硬體檔位，未宣稱通用EV／ISO校準。

這是主應用的整合版本，保留雙屏拍攝／膠卷／暗房 UI。Sensor1 的手動保持
研究接入日常預覽與拍攝；新整合的啟動順序、AE收斂與持續15fps仍需實機驗證。
歷史 Batch26–28 的局部響應結果不能當作這個版本的實機測試結果。

## 操作

* L：開啟右外攝 Sensor1，或明確切回模擬；其他鏡頭尚未納入此硬體契約。
* Y：拍攝端控制。上下選擇手動曝光、測光模式、AE目標補償，左右調整。
* 手動曝光：−3、−2、−1、0、+1、+2、+3，共七檔；切到自動測光後由AE選檔。
* 測光：Manual／Full average／Center weighted；返回Manual恢復已存手動檔。
* AE目標補償：−3..+3，1/3步進；曝光執行仍限制於上述七檔。
* `SETTLE`：硬體穩定中，仍可觀察預覽，不能拍攝。`LIMIT`：AE已抵達範圍邊界。
* R或A拍攝；拍攝記錄相機輸出的原始YUYV，不套用沖洗效果。
* 暗房Y切換LUT／顆粒／影調；影調左右選L低光、S高光、M中間調、飽和度，
  上下改值。L/S/M各為−2／0／+2；飽和度−100%..+100%，5%步進。
* LUT強度、顆粒種類／強度沿用暗房既有控制，A執行沖洗、SELECT另存配方副本。

L/S/M是曲線強度級數，不是感測器曝光EV。沿用既有影調數學與v3配方：
L對應Shadows，S對應Highlights，M對應Midtones；UI±2對應檔案中的±1。
舊配方的其他影調與小數強度仍能讀取、執行，不自動改寫。

## 硬體控制與限制

名義相對曝光表如下，四通道Core使用同一編碼，四通道ISP固定128：

| 檔位 | 粗積分3012 | Core3056/58/5A/5C |
|---|---:|---:|
| −3 | 8 | 528 |
| −2 | 16 | 528 |
| −1 | 32 | 528 |
| 0 | 64 | 528 |
| +1 | 128 | 528 |
| +2 | 256 | 528 |
| +3 | 256 | 544 |

這是名義積分／增益比，不宣稱標準ISO、光學快門秒數或通用精準EV。
尤其Fine integration與ISP響應會影響實際比值；UI不顯示虛構ISO／快門。
AE使用既有sRGB假設下的線性亮度目標與雙檔遲滯，沒有套用舊的已撤回等效快門模型。

初始化先切回Context A，依已驗證的原生設定流程開啟AE／AWB，使用一個暫存
DMA緩衝接收固定20幀後停止，再鎖定AE／AWB。seed不保存影像、沒有重拍。
寄存器使用已在Batch28成功的`cam:c`，晶片ID從`0000`讀取；不走被拒絕的
`cam:u`寄存器命令，也不將`3000`當作晶片ID位址。
接著要求chip2280、704行、sequencer state3；profile最多100次10ms狀態觀察。
依既有流程開啟AWB預覽與force-unity，僅一次refresh，最多64次4ms觀察，
連續兩次符合才通過；確認三通道128後關閉AWB預覽、AE/AWB/FD/HG並關閉
色彩歸一模式bit64。先觀察8幀，固定一次寫四通道ISP128，再嚴格觀察8幀。
AE不恢復原生AWB，拍攝端也不開放可改變此管線的WB／對比／飽和度設定。

曝光變更沿用兩個DMA槽／最多一個pending receive；單次設定粗積分與必要的
Core增益，不停流重建。積分切換等待2幀、增益切換等待8幀；期間禁止快照。
每個完成幀檢查控制／增益／時序，快照前再次檢查。失敗或漂移停止、嘗試
還原已保存設定並關閉相機，不補寫漂移、不重拍、不轉入單張採集或自動模擬。
還原失敗記在local trace；重新拍攝必須明確按L重開。

## Runtime治理

權威生產契約是`tools/camera_runtime_contract.py:CONTRACT_MATRIX`，生成
`include/film_runtime_contract.h`；歷史診斷矩陣維持原樣，不將生產UI冒充探針。

| source | 類型 | LaneAdmission | override | proof |
|---|---|---|---|---|
| film-camera-preview/v1 | 硬體預覽compute | 無，獨立3DS應用 | 無 | local_trace |
| film-camera-exposure/v1 | 手動／AE控制compute | 無 | 無 | local_trace |
| film-camera-capture/v1 | 已穩定快照compute | 無 | 無 | local_trace |
| film-darkroom/v1 | 沖洗compute | 無 | 無 | none |
| film-camera-ui-package/v1 | 本地打包side effect | 無 | 無 | local_trace |
| film-camera-ui-deploy/v1 | NetLoader傳輸side effect | 無 | 無 | local_trace |

沒有RegionTask、TaskScheduler或Vision admission物件；未知來源、startup override、
偽造scheduler admitted字串均不能進入曝光控制器。沒有新佇列或量測重試。
驗證器新增來源、狀態、範圍、穩定期與還原檢查。新runtime-internal狀態集中在
`FilmHardwareExposure`及矩陣旁`RUNTIME_INTERNAL_FIELDS`；它們不加入FilmRecipe、
F3S、EXIF或JPEG。硬體讀寫、幀、快照與還原證據僅寫既有本地應用日誌。

## 構建和已授權部署

此流程在新目錄凍結主UI原碼與現有8組LUT/.f3p、顆粒資源，保留舊binary。

```sh
python3 tools/camera_ui15_package.py stage --out build/camera-ui15-20261114-r2
DEVKITPRO=/path/to/devkitpro python3 tools/camera_ui15_package.py build --out build/camera-ui15-20261114-r2
python3 tools/camera_ui15_package.py deploy --out build/camera-ui15-20261114-r2 --ip 192.168.0.14 --port 17491
```

build必須成功完成ARM11／3DSX檢查，逐個資源核對實際嵌入內容，才生成build.json。
deploy先核對凍結原碼／資源／binary雜湊，再單次上傳；無診斷參數、無自動拍攝。
NetLoader接受payload只代表傳輸／啟動請求成功，不等於UI與相機已實機驗收。
NetLoader啟動不等於永久寫入SD安裝；本版不改寫舊安裝、照片或配方。

## 驗證

```sh
python3 tools/camera_runtime_contract.py --check
make -j2 test
```

macOS若Homebrew JPEG不在預設路徑：

```sh
make -j2 test CPPFLAGS='-Iinclude -D_POSIX_C_SOURCE=200809L -I/usr/local/opt/jpeg-turbo/include' LDFLAGS='-L/usr/local/opt/jpeg-turbo/lib'
```

新增`tests/hardware_exposure.c`以實際控制器加寄存器模型測試：未知／假來源拒絕、
錯chip／fps、固定handoff、七檔及界限、穩定前禁止控制、漂移不補寫、啟動／切檔
每個RPC故障、還原失敗仍嘗試剩餘設定、27種曲線單調性與F3S往返，以及拍攝
配方不繼承沖洗效果。主機模型通過不代替真機驗證。

Vision-product的以下三個pytest檔在Film3DS不存在，所以不執行：

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

尚待真機：新啟動順序與首次穩定、明暗切換AE／遲滯、全部七檔、長時間DMA、
讀回耗時下實際15fps、按R與預覽一致、暗房LUT／顆粒／曲線／飽和度輸出，
以及錯誤後關閉／還原。其他鏡頭和原生AWB混用不在此版本契約內。

## 2026-11-15 初始化修正 r3

使用者回報r2按L顯示`HW15 init DEVICE`。核對Batch28的`manifest.txt`及
`reads.csv`，`cam:u`寄存器讀取回傳`d8a053ea`，`cam:c`成功，晶片ID位址為
`0000`。r2錯用`cam:u`及`3000`，且未接入原生AE／AWB seed階段。
r2的NetLoader傳輸成功不代表相機初始化成功；保留其失敗版本及回執。

r3修正先更新`camera_runtime_contract.py:CONTRACT_MATRIX`：登記cam:c、
ID0000、Context A、原生AE／AWB固定20幀、單暫存buffer／單pending及失敗停止。
此邊界為獨立3DS相機compute，無LaneAdmission、無override，proof為local_trace。
新增`hardware_transport.h/.c`，實際服務與IPC封包皆經可測試的傳輸層；
`hardware_exposure.c`修正ID位址，`camera_3ds.c`接回原生seed／lock與handle釋放，
`main_3ds.c`在初始化失敗時顯示階段及十六進位錯誤碼。未降低原有硬體讀回檢查。

新內部欄位`hardware_transport`、`register_handle`、`native_result`在中央
`RUNTIME_INTERNAL_FIELDS`登記，不加入F3S／EXIF或portable recipe。
新增`tests/hardware_transport.c`覆蓋cam:c選擇、ID0000、完整16-bit回覆、
拒絕／錯誤回覆／關閉handle；更新`hardware_exposure.c`測試使3000不能假冒ID，
及`camera_ui_package.py`契約測試。修正後完整`make -j2 test`於11月14日通過；
11月15日再跑兩個C契約測試及Python打包測試均通過。生成標頭同步和diff檢查通過。

上次ARM構建被自動核准審查的用量限制擋下，命令未執行；11月15日已重新通過
正常審查並使用同一Lima／固定devkitARM映像成功構建。ARM11／3DSX／8組LUT
與顆粒嵌入檢查通過。

* 修正版：`build/camera-ui15-20261114-r3/Film3DS.3dsx`，9,274,304 bytes。
* SHA256：`f448f3292d4e88cfd2d8eddbbc8e3327bc526806291af5c6c582fb5905f6d8f9`。
* 建置回執：`build/camera-ui15-20261114-r3/build.json`。

首次連192.168.0.14:17491在socket建立前逾時，未傳輸程式。部署契約新增明確規則：
只有使用者再次回報NetLoader就緒，且前次有「connect失敗、尚未開始傳輸」證據、
同一binary／來源雜湊，才允許新的單次調用；不在同次調用重試，不重傳已接受或
部分傳送的payload，並保留失敗回執。`camera_ui15_package.py`依此記錄失敗階段、
傳輸狀態及使用者就緒文字；這些是local_trace欄位，已集中登記。
新增Python契約測試，拒絕無使用者就緒、未知狀態、部分傳輸、成功後重傳及雜湊變更；
三項Python測試通過。使用者隨後確認192.168.0.69:17491已開啟，已向此位址
單次傳送成功，設備接受完整payload並已送出啟動請求。前次未傳輸的失敗紀錄
保存在`deployment-failed-01.json`，本次回執為同目錄`deployment.json`。
相機按L實際出圖、AE收斂與持續15fps仍待設備回報，未以主機模型或傳輸結果代替。

## 2026-11-14 本輪結果

上述帶Homebrew JPEG路徑的完整`make -j2 test`通過，包含新增C契約測試、
兩項Python打包／portable欄位測試、既有unit／integration／HTTP simulator／QA報告。
HTTP simulator在允許loopback的執行環境完成。生成標頭同步與`git diff --check`通過。
新控制器與沖洗測試亦經AddressSanitizer／UndefinedBehaviorSanitizer執行通過。

已凍結帶UI／8組LUT／顆粒的原碼與資源，共47個雜湊條目。
已回查歷史成功命令，找到工作區內既有Lima與快取的固定devkitARM映像。
原先根據系統PATH判斷沒有工具鏈不準確；本輪成功沿用既有VM，不需要另裝SDK。
實際ARM建置命令（在Film3DS根目錄）：

```sh
LIMA_HOME=/Users/a64/Documents/Codex/.tools/film3ds-lima/instances /Users/a64/Documents/Codex/.tools/film3ds-lima/bin/limactl shell film3ds -- sudo nerdctl run --pull=never --rm -v /Users/a64/Documents/Codex/Film3DS:/src -w /src docker.io/devkitpro/devkitarm@sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b python3 tools/camera_ui15_package.py build --out /src/build/camera-ui15-20261114-r2
```

編譯成功，ARM11 JPEG審查通過（VFPv2、無連結NEON或ARMv7 barriers），3DSX
檢查與逐個ROMFS資源嵌入檢查通過。編譯器報告既有JPEG上游陣列界限警告及
Y2R診斷訊息截斷警告；本輪未改動這些既有實作。

已單次執行上述deploy命令，目的192.168.0.14:17491。設備接受payload並送出
NetLoader啟動請求；沒有自動拍攝，也沒有聲稱已觀察到實體UI或AE測試結果。
本輪最後只更新建置／部署文件，矩陣、驗證器及runtime-internal欄位未再變動；
打包／部署仍沿用既有side-effect契約，沒有新增測試或重試。

* binary：`build/camera-ui15-20261114-r2/Film3DS.3dsx`，9,273,228 bytes。
* SHA256：`dc88e2246156dc37b67080b7247de30e17bcbbb629a96e7f20569a249239e507`。
* 建置回執：`build/camera-ui15-20261114-r2/build.json`。
* 部署回執：`build/camera-ui15-20261114-r2/deployment.json`，
  `NetLoader_payload_accepted_launch_requested`。

本輪變更對照：`hardware_exposure.h/.c`、`platform_camera.h`、`camera_3ds.c`與
`main_3ds.c`負責拍攝compute邊界；`ui.h/.c`與`image.c`負責拍攝中性配方及
沖洗邊界；`camera_runtime_contract.py`與生成標頭負責矩陣／驗證限制；
`camera_ui15_package.py`負責打包與單次部署side effect；新增兩個測試檔與
Makefile測試入口，及本文。原先其他未提交改動保留。
