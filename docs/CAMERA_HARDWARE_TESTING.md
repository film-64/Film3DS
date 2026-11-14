# 硬體探針與公開文字資料的使用方法

先讀[目前結論](../CAMERA_HARDWARE_HANDOFF.md)。這份操作手冊不會自動連線裝置。

## 1. 乾淨環境與來源契約

建議Python3.11；原生C故障注入使用Linux/glibc，測試使用`fopencookie`等介面，
不能以macOS上的編譯失敗推論3DS原碼有問題。

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-camera.txt
python tools/bootstrap_camera_handoff.py
python tools/verify_camera_handoff.py
python tests/camera_hardware_handoff.py
```

bootstrap只建立忽略的本地連結：

```
build/worktrees/camera-deep-probe -> diagnostics/camera-deep-probe
build/hardware/native-trace -> diagnostics/native-trace
```

若已有不同工作樹／檔案，保留現狀並拒絕，不移除、不覆蓋。它不建立資料集連結，
不把缺圖的公開資料偽裝成完整原件。

權威來源在`tools/exposure_sweep_contract.py:CONTRACT_MATRIX`。C使用生成標頭，
禁止直接修改生成內容後跳過來源契約。

```sh
python tools/exposure_sweep_contract.py --header diagnostics/camera-deep-probe/probes/exposure/sweep_contract.h --check
```

## 2. 重建原生模型資料並測試

以下只執行Linux主機模型，不觸碰3DS。合成像素產生在忽略的`build/`，不加入Git。

```sh
make -C diagnostics/camera-deep-probe build/probe-driver build/probe-plan CC=gcc LDFLAGS=
cd diagnostics/camera-deep-probe
EXPOSURE_DENSE_ONLY=1 ./build/probe-driver
EXPOSURE_REBUILD_ONLY=1 ./build/probe-driver
GAIN_MATRIX_ONLY=1 ./build/probe-driver
CCM_BOOT_ONLY=1 ./build/probe-driver
./build/probe-plan
cd ../..
python tests/exposure_dense_report.py
python tests/exposure_rebuild_report.py
python tests/camera_gain_matrix_report.py
python tests/camera_ccm_boot_report.py
python tests/exposure_dense_fit.py
python tests/exposure_response_fit.py
python tests/exposure_luma_transfer.py
python tests/exposure_luma_validate.py
python tests/exposure_sweep_session.py
```

四組C情境分別為12 Dense、16 Rebuild、13 Gain Matrix、15 CCM Boot。
Python報告器使用真實C模型輸出，包含缺圖、錯誤讀值、偽造來源、DMA槽位、
命令順序、失敗／恢復及校準有效域拒絕。合成測試通過不是硬體校準通過。
session測試使用本機HTTP/socket，不會呼叫真實NetLoader。

可選CI配方在`docs/examples/camera-hardware-contracts.yml`，不發布影像artifact。
它是文件範例，未啟用：目前上傳憑證沒有新增workflow的權限。具備相應權限的
維護者可審閱後自行安裝到`.github/workflows/`；既有CI沒有改動。
其他歷史測試在原碼中保留；需要相應C情境或withdrawn field樣本時，不能拿Dense
樣本代替。`field/v1`單點等效快門模型已撤回，不作曝光真值或安裝預設。

## 3. ARM建置

本輪驗證使用SDK映像：

```
devkitpro/devkitarm@sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b
```

在已配置devkitPro/3ds-dev環境中：

```sh
cd diagnostics/camera-deep-probe
make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=handoff-20261114
cd ../..
python tools/verify_3dsx.py diagnostics/camera-deep-probe/FilmExposureSweep.3dsx
```

或在倉庫根目錄用Docker：

```sh
docker run --rm --net=none -v "$PWD:/work" -w /work/diagnostics/camera-deep-probe devkitpro/devkitarm@sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b sh -lc 'make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=handoff-20261114'
```

歷史binary SHA僅作已部署原件的來源證據。重建時revision與路徑等差異可能導致
雜湊不同，不能把可建置宣稱為bit-for-bit重建。

## 4. 最新硬體矩陣入口

下列命令會真正設定Sensor1、採集、恢復，再開啟RAM回收。先確認裝置已在
NetLoader等待、目標畫面固定。NetLoader17491不能用空TCP探測，這可能消耗等待。
RAM HTTP在5000；歷史GDB追蹤工具需要額外debugger與匹配身份，不是此採集的前提。

`before.json`範例：

```json
{
  "target_id": "center-gray-01",
  "metering_mode": "central",
  "iso": 100,
  "f_number": 2.8,
  "raw_shutter_reading": "約1/60",
  "precision": "approximate_not_quantified",
  "screen_brightness_changed": false
}
```

```sh
python tools/exposure_sweep_session.py DEVICE_IP --exposure-dense --before before.json --out build/hardware/exposure-sweep/batch-new-dense
```

最新固定入口是`camera-exposure-dense/v1`／
`--calibration-part=camera-exposure-dense-v1`：5/15fps、74點、444DMA、296ROI。
每點6DMA，保留前2和後2張；預先掛起的DMA可能跨越設定變更，前兩張是延遲
證據，響應使用後兩張。兩個緩衝區、最多一個pending Receive，不重用未完成槽位。

其他明確入口：

| session選項 | 來源／用途 |
|---|---|
| `--exposure-rebuild` | `camera-exposure-rebuild/v2`，74點／748DMA，額外獨立通道、128DMA保持與FD off/on/off |
| `--gain-matrix` | `camera-gain-matrix/v4`，8節點／24組FD／幀率／曝光／ISP條件 |
| `--ccm-normalization` | `camera-ccm-normalization/v3`，歷史bit6對照、位元組觀察、SD啟動留痕 |

來源不能混用，未知scope拒絕。診斷compute使用明示的
`standalone_hardware_diagnostic` override，證據只在local_trace；不冒充普通
scheduler-admitted工作。任何後續控制流程變更都先改矩陣，再改驗證器及測試。

## 5. RAM回收與保留錯誤

完成後相機會關閉／恢復，畫面停在凍結RAM UI。先回收原件，再按START回到
NetLoader；本版沒有遠端退出。不能為下載失敗而自動重拍。

若出現EOF截斷，保留初始session失敗狀態。核對`ready.json`和新`/info`的同一
index SHA、run與file count後，才對同一凍結RAM作明示唯讀續傳。`retrieve()`會
檢查既有檔案CRC、部分檔頭尾及最後完整檔案CRC；snapshot改變即拒絕。

```python
from retrieve_probe_ram import retrieve
# 僅在已比對原索引／run後執行；sys.path需包含探針tools。
retrieve("DEVICE_IP", "LOCAL_FULL_CAPTURE", chunk=16384,
         request_gap=0.02, recovery_policy="frozen-resume-v2")
```

`frozen-resume-v2`只對其登記的暫態例外作有界處理；不在未登記的EOF上隱藏
補拍／無限重試。Batch27/28的唯讀續傳結果與初始失敗有分開保存。

## 6. 完整原件的報告與模型

以下必須指向自己保存的完整採集，不能指向本包缺圖的`evidence/`：

```sh
python tools/exposure_dense_report.py LOCAL_FULL_CAPTURE --out build/dense-report.json
python tools/exposure_dense_fit.py LOCAL_FULL_CAPTURE --out build/rgb-fit.json
python tools/exposure_luma_transfer.py LOCAL_FULL_CAPTURE --out build/luma-model.json
python tools/exposure_luma_validate.py NEW_FULL_CAPTURE --model FROZEN_MODEL.json --model-sha256 EXPECTED_SHA256 --out build/fixed-validation.json
```

報告器檢查原始CRC/SHA、來源、計畫、呼叫順序、狀態、DMA、影像統計、留痕
與恢復。RGB與Y分析是不同來源，失敗RGB不會被改成成功。固定Y驗證拒絕拿
模型訓練批次驗證自己，拒絕配置不符，禁止重擬合與域外推算。

Y只描述指定場景平均碼值到baseline-equivalent曝光；不直接提供radiance、ISO、
RAW、完整色彩校準。時間估計使用DMA節奏和行／積分設定；raw VSYNC時間單位
尚未以本機量測核實，不能自行把它當成ns。

## 7. 外部參考與下一批資料

往後統一中央測光，固定相機／畫面及中央區域，保存前後原始ISO、光圈、快門
或EV與讀數精度。舊全局讀數保留原標記，不補成缺失的中央讀數。測光儀ISO100
不是已校準的3DS ISO100；螢幕碼值／測試圖標籤不是已知入射EV。

下一輪應採完整圖或多分區：灰階響應、高光剪裁、暗部噪聲、色卡／固定WB、
原生色彩與自訂AE相容、明暗切換收斂，再驗25fps以上及5fps CCM時序。
不要把curve/HSL後製或螢幕相對測試宣稱為增加感測器動態範圍。

本次匯出工具可從自己的私有原件建立同類文字子集：

```sh
python tools/export_camera_handoff.py PRIVATE_CAPTURE_ARCHIVES --out NEW_PUBLIC_TEXT_DIRECTORY
```

輸出需為新目錄且不在來源原件目錄內；不修改來源，也不包含像素。公開包驗證
只證明列出的文字檔未改動，完整影像驗證的要求不變。
