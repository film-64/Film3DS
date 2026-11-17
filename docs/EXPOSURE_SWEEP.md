# 可驗證的離線曝光模型

本工具是 Film3DS 獨立硬件診斷，不進入 Vision 的 RegionTask、TaskContext、TaskScheduler 或 LaneAdmission。主相機曝光行為沒有改動。

## 合同與覆蓋

權威矩陣是 `tools/exposure_sweep_contract.py:CONTRACT_MATRIX`。設備的 `build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h` 由它生成；測試逐字比對生成結果。未知 source、階段、設定、範圍或缺失讀回均拒絕。

`exposure-sweep/v2`：Sensor1、5fps、278 階段。

- 原生通道比例一組。
- 類比代碼 `10/18/20/28/30/3c` × 核心數字設定 `1/2` × ISP 原生／unity，共 24 組。代碼是實驗設定，不是實測光度倍率。
- 每組粗積分 `4/8/16/32/64/128/256/528/1024/1585`。
- 一個 seed、開始參考、每組後的相同設定參考、最後恢復；每階段 8 幀等待、4 幀測量。
- 每個正式幀前後驗證 AE/AWB、五項控制、時序、類比／核心數字／ISP 讀回。每組內完整狀態必須一致。
- ISP 只允許寫 `32d4/32d6/32d8/32da`，設定為 unity128 或本輪觀察到的原生值（1..512）。退出時嘗試恢復原始五項控制與四個 ISP 值；恢復失敗不能報完成。

完整指上述矩陣，並非任意快門、所有增益、所有 ISP 模式或所有相機。每個狀態都必須獨立通過；未知狀態沒有插值或增益代碼公式可繞過驗證。

## 採集與原件

v2 一次使用固定 28 MiB RAM，最多 320 個文件；不寫 SD、不壓縮、不丟棄舊幀騰空、不重試測量。容量不足立即失敗。相機關閉、所有流關閉後凍結，使用 `FILMRAM 3` 只讀 HTTP 範圍回收；裝置 CRC、範圍、快照指紋、主機 SHA-256 都保留。

每階段保存最後正式幀中央 `x=256,y=192,w=128,h=96` 的**原始 YUYV 像素**，不是 JPEG、線性化圖或均值。四個正式幀的 ROI 統計和寄存器證據全部保留。該範圍外的測光不能由本次原件重建；`--roi` 必須在這個區域內。完整畫面 CRC 與中央原件 CRC 是不同記錄，不混用。

固定目標、機位、測光區域與光源；採集前後各記一次 A 的讀數。低增益全黑不等於硬件不支援，更不等於模型有效：需要足夠照明讓每個通道覆蓋非裁切區域。新光照使用新批次、新讀數。不同色溫／光譜的遷移也需要獨立驗證，不能只靠寄存器一致宣稱通過。

## 模型與驗收

模型直接從處理後 RGB 的曝光序列識別逆響應，沒有預設 sRGB 逆轉換就是感測器線性響應，也沒有 unit-slope fallback。

對固定處理狀態、同一像素：

`g(z_ij) = log(t_j) + log(E_i)`。

每個通道採固定 10×10 像素網格。對同一像素在訓練曝光間做加權中心化，消去未知 `E_i`，用受限最小平方求 `g` 的正增量，再加二階平滑約束。每個像素有自己的未知光度，沒有假設整個目標均勻。增量下限僅確保單調，不固定斜率；模型仍須通過沒有參與擬合的曝光。

- 訓練索引 `0/2/4/6/8/9`；固定留出 `1/3/5/7`，不依結果挑選。
- 訓練碼值限於實測的 4..250 範圍，每通道至少 32 個碼值跨度。
- 至少兩個可評估的留出曝光，各通道誤差均須 ≤0.25 EV；域外點明列為無覆蓋，不計為通過。
- 逐像素反響應後才做測光統計，不能先把碼值平均再反轉。
- 固定設定參考幀漂移 ≤0.125 EV；單階段四幀中央 Y 均值跨度 ≤2 碼值。這兩者不是同一物理量。
- 精確 A 讀數的前後差不得超過 0.125 EV。區間讀數若確定相離超過 0.125 EV 則拒絕；區間重疊僅表示相容，不證明照明穩定，仍必須通過影像參考幀檢查。錨點保留完整區間，預測減去驗證參考的整個誤差區間都在 ±0.25 EV 內才通過。
- 時間採 `(coarse + fine/line_pck) × DMA_period/frame_lines` 估計。這不是獨立光學快門標定，輸出明示 `optical_time_verified=false`。

每個狀態的逆響應及場景光度有任意相對尺度；本輪 A 讀數用來錨定 EV100。跨光照驗證時使用**先前凍結的模型及錨點**；新 A 讀數只用於計算誤差，不用於修正預測。要求新舊批次至少相差 1 EV、同一目標且處理狀態精確匹配。新批次自身的參考幀穩定性獨立檢查；為穩定性識別的曲線不參與 EV 預測。

目前輸出是針對固定目標／區域、指定處理狀態的可驗證測光模型。未宣稱標準 ISO、光學快門、任意場景彩色測光或主應用自動曝光已完成。

## 操作

在 Film3DS 根目錄執行。Python 需要 NumPy、Pillow 和 SciPy 1.15.3；本工作區 SciPy 安裝於 `../.tools/exposure-calibration`。OpenCV 4.12 的既有交叉驗證工具保留，並非本模型的真值。

```sh
python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check
python3 tools/exposure_sweep_session.py DEVICE_IP --out build/hardware/exposure-sweep/NEW_BATCH --before BEFORE.json
python3 tools/exposure_sweep.py verify NEW_BATCH/recovered --out NEW_BATCH/verified.json
python3 tools/exposure_sweep.py fit NEW_BATCH/recovered --out NEW_BATCH/model.json
python3 tools/exposure_sweep.py reference NEW_BATCH/recovered --batch batch-name --target fixed-target-01 --before 100,2.8,1/32 --after 100,2.8,1/32 --out NEW_BATCH/reference.json
```

也接受 `--before EV100=8` 或 `--before EV100=8.1:8.3`。區間不取中點，輸出 EV 與等效快門區間。參考文件包含原始輸入及 capture manifest SHA，不只靠相同目錄名匹配。前後讀數不一致時保留失敗值，禁止偷偷更新錨點。

新照明批次拍完後，先生成不讀取新測光值的預測：

```sh
python3 tools/exposure_sweep.py predict TEST_BATCH/recovered --model TRAIN_BATCH/model.json --training-reference TRAIN_BATCH/reference.json --out TEST_BATCH/blind-predictions.json
python3 tools/exposure_sweep.py compare TEST_BATCH/recovered --model TRAIN_BATCH/model.json --training-reference TRAIN_BATCH/reference.json --reference TEST_BATCH/reference.json --out TEST_BATCH/comparison.json
```

`fit`／`compare` 同時輸出 Markdown 摘要。每個狀態、驗證點、RGB 誤差、域外或未知狀態原因都有 JSON 明細。輸出採 exclusive create，不覆蓋原件或舊結果。

## 測試

```sh
python3 tests/exposure_sweep.py
python3 tests/exposure_automation.py
```

在既有 Linux/devkitARM 容器及探針工作樹：

```sh
make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=
EXPOSURE_SWEEP_ONLY=1 ./build/probe-driver
./build/probe-plan
python3 tests/ram_sweep.py
python3 tests/ram_evidence.py
make probe-test CC=gcc LDFLAGS=
python3 tests/calibration_report.py
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-sweep REVISION=exposure-sweep-v2
python3 tools/verify_3dsx.py FilmExposureSweep.3dsx
```

C 測試使用真實採集／恢復／RAM 實作與明示的模擬感測器、時鐘；TCP 測試使用真實 socket。數值測試使用已知非線性響應與故意破壞的留出圖。這些測試通過不代表實機校準通過。

## 2026-11-12 首輪失敗紀錄

SD v1 `run-1794515772-000` 在 phase62 停止，沒有 END／SWEEP_END。使用者回報 `-916437685`（`0xC960454B`）；工程既有長 SD 採集有同樣錯誤歷史。本輪 70 份原件、39,099,187 bytes 已只讀回收並逐文件重算 SHA-256，存於 `build/hardware/exposure-sweep/batch01-capture/recovered`。

原件、最初源碼與二進制 SHA 全部保留。部分完整序列只作開發診斷，不把整輪補成通過；尤其低增益在 EV≈5 的場景有大量黑幀。RAM v2 是新合同、新完整採集，不接續或拼接失敗輪次。

## Runtime 治理分類

- 本地硬件診斷 compute／凍結證據只讀回收／離線模型分析。
- Film3DS 的 `CONTRACT_MATRIX` 新增上述明示邊界；Vision 的矩陣未改，沒有新增 Vision task source。
- 新增 sweep 驗證器；既有寄存器驗證只增加新 frame scope。RAM v3 增加明示索引容量，v1/v2 仍限制 40 文件。
- 新的 `exposure_sweep` RAM profile flag 屬獨立探針內部狀態，沒有寫入 portable image metadata。Vision `runtime_internal_fields.py` 未改。
- Vision 三條 pytest 未執行：這個 Film3DS 倉庫不包含相應文件，也不調用那些執行邊界。

## Batch02 RAM 失敗證據

`build/hardware/exposure-sweep/batch02-ram/FINDINGS.md` 記錄本輪結論及重算命令。退出前背景已回收 156 份原件，CRC/SHA 全數核對；phase148 首個 unity ISP 在 settling 後回復原生值，採集按契約停止。不得把原生 ISP 的 13 組完整掃描當作 25 組矩陣完成。13 組探索性響應均未通過既定留出條件，部分幀間亮度不穩與飽和也需處理；未產出有效校準模型。

已登記 `exposure-sweep-check/v1`：只走 seed、native reference、state13/coarse4、reference、restore 共 5 階段，沿用完整讀回、RAM 與停止契約。它只供定位首次 unity ISP 邊界，不能輸入完整校準驗證器；尚未在實機部署驗證。

## 精簡通信模式（2026-11-13）

採集器仍保留完整證據，網路回收的原始資料直接寫入電腦檔案，不送進對話。`exposure_sweep_session.py` 現在預設只輸出 deploying、capturing、recovering、recovered（或 failed）狀態；每檔校驗及錯誤堆疊寫入 `session.log`。`session-status.json` 採臨時檔後原子替換，可只讀此小檔查進度。讀取狀態不會重新部署或重拍。recovered 只代表原件回收完成，capture_complete 與 calibration_validated 分開標示。

短診斷使用：

```sh
python3 tools/exposure_sweep_session.py DEVICE_IP --check --out build/hardware/exposure-sweep/UNIQUE_CHECK
```

`--check` 只允許既有五階段診斷，不需要 A 讀數，也不能生成有效校準。完整採集仍必須提供新的 `--before`。RAM 仍須在退出程式之前回收；此變更不宣稱具備 SD 持久化。相較重新引入尚未證明穩定的長時間 SD 寫入，先藉本地檔案與有界摘要降低對話輸出。測試 `python3 tests/exposure_sweep_session.py` 使用真實 loopback HTTP 分段回收，驗證詳細輸出留盤、未知協定拒絕、不完整採集不能變成校準成功、部署僅一次。

### 安靜模式實機驗證

`batch03-isp-check` 已部署到 172.20.10.2 並自動回收 13 份原件（760,210 bytes），CRC32/SHA-256 全數核對。37 captures 後，phase5 的第一個 unity ISP 點再次由即時讀回 128 變為 R167/Gr162/Gb162/B133，於第一個正式幀前停止；`restored=1`、`io_error=0`、RAM failed=0。這證明不必先跑前 13 組也會重現問題，但尚未識別覆寫來源。`batch03-isp-check/diagnostic-summary.json` 是精簡分析入口；原件仍保留。session console 僅四行狀態，詳細紀錄寫入 `session.log`。

本次驗證：session 真實 HTTP 測試 2 項、模型測試 15 項、寄存器／回收邊界測試 13 項通過；3DSX/SMDH 驗證通過。變更限於本地 session 契約、狀態／日誌輸出與既有短診斷部署入口；模型驗證器與 Vision runtime-internal fields 未改，未新增重拍或重部署重試。

## ISP 覆寫定位與限定修正試驗

`batch04-isp-transition`（check/v2）保存 160 次額外唯讀證據。第一幀 before_arm、after_arm、after_start 均為四通道 128；after_wait 已為 128/162/162/133，after_stop 為 165/162/162/133。這將首次覆寫定位於第一幀接收期間，並不能單獨識別寫入者；跨通道讀取也不是原子快照。原件 13 份、773,641 bytes，已回收。

參考：[GBATEK sequencer 定義](https://mgba-emu.github.io/gbatek/#dsi-aptina-camera-variables-seq-sequencer-mcu21xxh)。MCU byte logical address A102 是運行中的 SEQ_MODE，bit0 AE、bit2 AWB；原有 A11D/A11F 是 preview 狀態設定，不能獨自證明 live gate 關閉。

`exposure-sweep-check/v3` 在初始參考後保存 A102，只寫 saved & ~5，再逐正式幀前後驗證整個 byte。試驗結束（包括失敗）嘗試恢復 saved，讀回失敗不報完成。最多兩次新 MCU 寫入，僅允許 saved 或 target 值；未增加一般 MCU 寫入入口、補寫循環或重拍。新增內部字段 sweep_seq_gate/saved/target 僅在本地探針與本地矩陣註冊。這是待實機證實的限定修正，完整 sweep/v2 尚未改為使用此 gate。

### check/v3 結果與 check/v4 MCU 同步證據

`batch05-live-seq-gate` 實測 A102 saved=10、target=10、restore=10，均驗證成功；AE/AWB live bits 本來已關閉，IFP 仍漂移。因此「live AE/AWB 未關」假設在本設備本輪不成立。

check/v4 改測 MCU A355（AWB_MODE）bit5 Force_unit_dgains。先保存 byte，只設 bit5；native 參考前或任意失敗後恢復，最大兩次寫入，拒絕其他地址／值。測試涵蓋設定失敗與恢復失敗，沒有重試或逐幀補寫。

依使用者指定，每個測量幀前後及第一組 unity 的 8 個 settling frames × 5 個邊界，同時保存：MCU A102、A104、A355、A34E、A34F、A350、221F，加物理 32D4/32D6/32D8/32DA。邊界額外讀取上限 440；七項 MCU 不重複記錄既有 221F。逐筆保留 timestamp，不能聲稱原子快照。MCU 保存 AWB 增益與實際 IFP 值分別報告，不強行視為相等。

離線診斷報告：

```sh
python3 tools/exposure_isp_report.py BATCH/recovered --out BATCH/isp-report.json
python3 tests/exposure_isp_report.py
```

報告先核對原件 CRC/SHA、明示來源與完整性，再核對四個正式幀前後的七項 MCU／四通道 IFP 證據；要求 AE/AWB live 關閉、Force Unity 保持、sequencer 穩定在 preview 或 capture、物理 IFP 全128、原值恢復成功。缺失／重複／讀取失敗／錯狀態都拒絕。即使此診斷通過，calibration_validated 仍為 false。

首次 batch06 部署在 NetLoader 第一個確認前 EOF，未傳入程式、未採集；部署失敗與硬件測量停止分別記錄。


## check/v5 與獨立 AWB-on Refresh 實驗（2026-11-13）

本次先更新本地 `tools/exposure_sweep_contract.py` 的矩陣，再改實作與驗證器。
`exposure-sweep-check/v5` 維持原有控制與停止規則，只加入 A103；八個 MCU 變數加四個 IFP 寄存器，在八個 settle frames 的五個邊界最多額外讀取 480 次。舊 v4 契約不改。

`exposure-sweep-awb-refresh/v1` 是獨立五階段硬體診斷 compute；不經 Vision LaneAdmission，明示 standalone_hardware_diagnostic override，全部證據僅在 local_trace。保持 AE 關閉、AWB driver 開啟，暫設 Preview AWB A11F=1，A355 bit5 開啟，再發 A103=5 一次。A103 必須先為 0，A104 必須為 3 或 7；最多 100 次完成輪詢、每次等待 10ms，要求命令歸零且原狀態與控制連續兩次匹配。失敗即停止，不重發或重拍。

任何退出路徑都嘗試恢復 A11F、A355、A102，再發一次 Refresh 套用恢復；原五項曝光控制與四個 IFP 值也必須恢復。A103 寫入總上限兩次，只許命令 5。AWB_MODE bit0/1 是可變狀態，新 Refresh 實驗驗證控制位 mask=0xFC；原 v5 OFF 控制仍驗證完整 byte。

新增本地內部欄位 sweep_refresh_active、sweep_preview_awb_saved、sweep_refresh_writes 已在本地矩陣註冊；不寫入 portable metadata。Vision 的矩陣、verifier、runtime_internal_fields 均未修改。

執行兩個明示且獨立的單次實驗（先回收前一輪原件，再由裝置回到 NetLoader）：

```sh
python3 tools/exposure_sweep_session.py DEVICE_IP --check --out UNIQUE_OFF_BATCH
python3 tools/exposure_sweep_session.py DEVICE_IP --awb-refresh --out UNIQUE_ON_REFRESH_BATCH
python3 tools/exposure_isp_report.py BATCH/recovered --out BATCH/isp-report.json
```

新版報告拒絕錯 sensor/label、未知 source、命令未完成、缺失或錯誤 Refresh 與恢復證據；用實測 221F 計算 round(AWB*221F/128) 並列出符合與不符合，不預設 221F=162。batch08 實際 A104=3、221F=128。AWB-on 加 Refresh 同時改變兩個因素，結果只能支持這個組合，不能單獨歸因 AWB 或 Refresh。

參考：GBATEK SEQ 表定義命令 5 為 Refresh：https://mgba-emu.github.io/gbatek/#dsi-aptina-camera-variables-seq-sequencer-mcu21xxh 。同系列 MT9D111 的手動 CCM 流程同時啟用 AWB 與 Force unit gains，僅作實驗依據，不能視為 MT9V113 固件已被證明：https://files.niemo.de/aptina_pdfs/MT9D111_Dev_Guide.book.pdf （第54頁）。


### Refresh/v1 實測與 v2 比較條件修正

batch09（check/v5）完成回收 13 份、813529 bytes；A103=0、A104=3、221F=128，仍於 frame0 after_wait 開始覆寫，恢復成功。batch10（Refresh/v1）完成回收 13 份、834848 bytes，沒有進入 S13。A103 命令已歸零、A104=3、A102=14、A11F=1、Force Unity 保持，但 A355 從34變42；恢復時從2變26。先前 v1 將未知低位也當成固定控制，導致兩次 bounded polling 均不接受；v1 原結果保留 complete=0/restored=0，不重寫成成功。

新來源 exposure-sweep-awb-refresh/v2 明確只以 A355 已定義控制位5/6（mask=0x60）作匹配；低位0/1是狀態，2..4保留原始證據但不賦予控制語意。仍嚴格要求 Force Unity、AE關閉/AWB開啟、A103歸零、A104穩定、A11F及原控制恢復。增加實測低位變化成功反例及 Force位清除拒絕測試；沒有修改舊 v1 契約、延長 deadline、重發命令或放寬物理 IFP=128 的要求。


### 新版實機結果

batch11（Refresh/v2）回收13份、824291 bytes，原件CRC/SHA全部通過。設定Refresh 16 polls、恢復Refresh 9 polls驗證成功。40組邊界讀取均為A102=14、A103=0、A104=3、A355=58、MCU AWB=128/128/128、221F=128；物理IFP第一幀after_wait開始為133/133/133/133。3012由immediate=4回到frame8 before=264，等於原保存值與MCU2222=264，契約停止且restored=1。AWB色彩比例已變中性，但尚未維持物理unity或曝光鎖定，不能宣稱校準成功。完整原件、變更與驗證命令見build/hardware/exposure-sweep/batch11-awb-on-refresh-v2/FINDINGS.md；跨輪摘要見build/hardware/exposure-sweep/awb-refresh-comparison.json。


## Post-refresh manual 單一 S13 診斷（2026-11-13）

上一輪batch11的實際程式已先完成Refresh，再寫3012=4與四通道128；曝光變化發生在其後的settle期，不能歸因成「在Refresh之前寫3012」。本輪真正變因是在AWB歸一後再次關閉Preview及live AWB，驗證關閉成功，再作最後的物理寫入。

先登記新來源 `exposure-sweep-post-refresh-manual/v1`，compute分類、獨立硬體diagnostic override與local_trace不變；不進入Vision scheduler或portable metadata。新的 `sweep_manual_locked` 已登記在本地矩陣，作為禁止採集中Refresh的寫入邊界。原有來源與測試停止條件不改。

流程：保存設定與原生參考 → AE off/AWB on、Force Unity → 一次Refresh並確認命令歸零且AWB RGB全128 → A11F=0、A102清除AE/AWB且立即讀回 → 再次驗證AWB仍全128、A103=0、A104=3或7 → 最終3012=4與S13 core codes、再寫四通道128且讀回 → 8 settle + 4 measured frames。採集中寫入入口拒絕A103；採集結束才解除邊界並恢復原設定、執行恢復Refresh。關閉失敗即停止，不隱藏補Refresh或重新測量。

五個settle邊界額外保留3012，總計最多520筆（8×5×13）；MCU仍為A102/A103/A104/A355/A34E/A34F/A350/221F，另四個物理IFP。首幀保留作診斷，不計入正式測量。

離線報告v3按來源要求3012=4、AE/AWB bits都關閉、Force Unity保持、命令歸零、物理IFP全128，並核對真實calls.csv中的寫入順序與採集中沒有Refresh。測試新增非unity AWB、關閉失敗、3012漂移、IFP漂移、恢復失敗、鎖定後拒絕Refresh，以及假造中途Refresh的負例。

執行入口：

```sh
python3 tools/exposure_sweep_session.py DEVICE_IP --post-refresh-manual --out UNIQUE_BATCH
```

SNAP Camera原頁：https://snap-camera.neocities.org/dev/camera 。其DSi實作僅作參考，不新增REFRESH_MODE或Capture切換到本輪3DS測試；以上每項行為由本地契約與實際讀回驗證。


### Batch12 結果

13份、828953 bytes原件完整回收且CRC/SHA核對通過。歸一後停用AWB並作最終物理寫入，40組邊界（520筆）中3012始終4；A102=10、A103=0、A104=3、A355=58、AWB三通道128、221F=128均不變。第一幀after_wait物理IFP開始變成133，after_stop四通道均133；8個settle後仍如此，正式frame8前停止。採集中沒有Refresh，恢復成功。屬於「曝光保持但獨立ISP共同增益仍覆寫」的結果，尚非有效校準。詳見build/hardware/exposure-sweep/batch12-post-refresh-manual/FINDINGS.md與isp-report-final.json。


## ISP更新機制有界診斷（2026-11-13）

新增exposure-isp-update/v1：三組restart初值96/128/160及一組continuous128，各3幀；每組重新歸一後關閉driver。第一幀後一次計畫內重寫128，不Refresh/Context切換；連續組一個pending DMA、只Start一次，frame2確認重寫後新幀。讀取17項狀態及19個Mode變數；沒有Mode或任意RAM/XDMA寫入。物理IFP變化是觀察結果；讀取、固定控制、DMA、寫入讀回和恢復仍嚴格拒絕失敗，既有unity契約不改。

```sh
python3 tools/exposure_sweep_session.py DEVICE_IP --isp-update --out UNIQUE_BATCH
python3 tools/exposure_isp_update_report.py BATCH/recovered --out BATCH/isp-update-report.json
```

batch13全部四組完成，17份原件、1026239 bytes完整回收並核對CRC/SHA。96/128/160都在重寫前已全133；重寫128成功後，restart與continuous後續新幀均回到全133。continuous只有一次StartCapture，沒有中途Refresh/Stop/Start；A102=10、A103=0、A104=3、AWB=128、221F=128、3012=4、core528保持。19個Mode變數均不變且未發現133；不代表找出或排除shadow。只證明同一串流內再次覆寫，未證明每幀必寫或識別MCU/相機服務寫入者。證據完整passed=true，校準仍false。詳見build/hardware/exposure-sweep/batch13-isp-update/FINDINGS.md。


## 無新 Receive 的 VSYNC 判別診斷（2026-11-13）

先登記 `no-receive-frame-update/v1` 及相應 session/report：獨立本地 hardware diagnostic compute，明示 override，證據僅 local_trace；不進入 Vision LaneAdmission。兩組各重新歸一、關閉 driver。連續 DMA 對照沿用 batch13 三幀與一次重寫，取消 Mode 讀取；無新 Receive 組只提交一個 DMA，要求 WaitFrame=0、IsFinishedReceiving=true 並成功關閉 DMA 事件後，才重寫四通道128。

後者不再 SetReceiving、Start/Stop、Refresh、ClearBuffer 或切換 Context。每次取得 VSYNC 基線原始時間、清除事件、等待成功，再要求最新原始時間嚴格前進；共三次。沒有事件或新時間即無法判定，不將等待秒數換算成幀。每次確認 VSYNC 後連續讀八次32D4，再讀完整17項狀態。每組結束才停止，最後恢復設定。總 Refresh 最多3次（兩組準備及一次恢復），不新增 Probe 持久狀態欄位或任意 MCU RAM/Mode 寫入。

影像原件分清來源：對照保存最後 DMA ROI；無新 Receive 組保存首個 DMA ROI，不能當作後續 VSYNC 的影像。觀察報告 passed 只代表實驗證據完整，calibration_validated 始終 false。VSYNC 服務時間保留原始單位，寄存器讀取非原子，不能用八次 RPC 直接推論硬體寫入耗時。

```sh
python3 tools/exposure_sweep_session.py DEVICE_IP --no-receive --out UNIQUE_BATCH
python3 tools/exposure_no_receive_report.py BATCH/recovered --out BATCH/no-receive-report.json
```

API 依據為 libctru 的 CAMU_GetVsyncInterruptEvent、CAMU_GetLatestVsyncTiming(past=1)、svcClearEvent；已核對安裝 SDK 與原始碼：https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/services/cam.h 、https://github.com/devkitPro/libctru/blob/master/libctru/source/services/cam.c 。


### Batch14 結果

15份原件、860699 bytes全部回收並核對CRC/SHA。兩組重寫128後均再次全133；無新Receive組僅一次SetReceiving/Start，先驗證DMA完成且關閉同一事件，之後三個清除事件後的新VSYNC皆有嚴格前進時間及完整狀態。第一個新VSYNC後32D4八讀為128、133×7；其後完整四通道已全133。17組完整狀態共289讀，另24筆紅通道連讀全成功；A10210/A1030/A1043/A35558/AWB128/221F128/3012=4/core528皆固定。NO_RECEIVE_END complete=1 arms=2 restored=1。新SetReceiving不是覆寫的必要條件；尚不能分辨Sensor MCU與Camera sysmodule，亦無外部I2C監聽可證明寫入來源。passed=true為證據完整，calibration_validated=false。完整數據、VSYNC時間、讀回邊界、變更治理及測試命令見build/hardware/exposure-sweep/batch14-no-receive-frame-update/FINDINGS.md。


## Camera寫入者直接追蹤（2026-11-13）

採用先登記的camera-writer-window/v1/v2與Camera SendSyncRequest觀察器；裝置完成準備後等待READY/GO，主機核對現機全文/入口及I2C session，啟用kernel syscall catch後才GO；完整窗口結束DONE後先關閉catch/detach再RELEASE，最後恢復原設定。沒有程式斷點、process memory/Sensor韌體寫入。

Batch15/v1保留complete0：在debugger暫停下即時128讀回已變133，按原契約停止；RSP在途stop收尾問題另保存，不假裝detach成功。Batch16/v2以新登記writer觀察契約保留GO前嚴格128證明、四筆128寫入/成功服務回覆，接受隨後1..512原始觀察並要求三個新VSYNC、固定控制與恢復；舊unity及v1仍在133讀回下失敗。

Batch16全部完成：14份807627 bytes原件CRC/SHA通過，約87.104秒、1092 stop、546個Camera外送請求（278 I2C、268 pdn:c）。52個I2C寫入正好48次MCU讀取選址098C及我們自己的4次IFP128，沒有IFP133、0990–099E MCU資料或其他寄存器寫入；226次I2C讀回均成功。三個新VSYNC後IFP全133，A10210/A1030/A1043/A35542/AWB128/221F128/3012=4/core528保持。catch/detach/恢復皆成功。證據支持Sensor內部更新，尚未定位MCU函數或shadow，也未全域監聽所有process/實體總線。完整來源、測試、原件與限制見build/hardware/exposure-sweep/batch16-camera-writer-observe-v2/FINDINGS.md。
