# 暗房曲線修正與傳入紀錄

## 最終版本

- 包：`build/darkroom-curves-20261115-r4/Film3DS.3dsx`。
- SHA-256：`0df4e71b2d99abf49cbbb7d7ad5ed545b685f7c57918eda56be77fb7804e4ae3`。
- NetLoader `192.168.0.69:17491` 已接受 payload，並已發送啟動請求；憑證在包內 `deployment.json`。
- 相機基底沿用先前已部署、已驗證的 `darkroom-offline-20261115-r1`。
  `stage_darkroom_patch.py` 驗證基底 build／manifest，固定九個相機相關檔案雜湊，
  不把另一項工作的相機修改混入。main 只替換六處暗房值顯示／紀錄欄位。
- 中間實驗包 r2/r3 沒有部署；最終 r4 才傳入。

## 行為

前置曝光仍在 LUT 前。使用者 H/M/S 改在 LUT 與舊版影調之後、HSL／飽和度之前。
0 檔完全繞過新曲線，不改原濾鏡；保留 v4 前置曲線及 v1/v2/v3 舊修正原有語義。
新版 H/M/S 存在 v5 的獨立 `output_tone`，不重新解讀已保存的 v4 數值。

亮度映射在 sRGB 編碼後的亮度域處理，將錨點偏移以 cubic smoothstep 混合：

- 錨點 `0/.25/.5/.75/1`。
- 偏移 `-S*(S<0?.06:.025)`、`-.10*S+.03*M`、`.06*M`、`.10*H+.03*M`、`H*(H<0?.10:.025)`。
- 亮度映射後回到線性亮度，以相同倍率改 RGB，最後處理色域。
- H 正值提亮、負值壓低；S 正值加深、負值提起；M 正值提亮中間區。
- 相鄰錨點偏移的 blend 權重非負，因此每項控制的方向一致。
- 最小錨點距離 .12，大於 .25/3，保證所有 11³ 組合的曲線導數為正。
- 接點斜率左右均為 1；中間調向兩側保留一半幅度，避免只在狹窄中間位置有效。
- 黑白端點可調；最終仍受顯示色域及 8-bit 輸出邊界限制。

## 同批測量的改善

使用最終 r4 凍結 C 核心、相同 LUT／原圖和測量器，重跑 33,792 個灰階點、132 張完整影像。
資料在 `build/curve-stops-fixed-20261115-r4`。原圖是本地留存 ROLL0067，不是這次實機即時畫面。

Portra 的代表點（總差是 -5 到 +5 的整段差，單位為 RGB 加權的 8-bit 碼值）：

| 控制／輸入 | 舊總差 | 新總差 | 舊／新不同 RGB8 檔位數 |
| --- | ---: | ---: | ---: |
| H / 248 | 0.91 | 47.63 | 4 / 11 |
| H / 254 | 0.12 | 47.24 | 1 / 11 |
| S / 8 | 0.33 | 25.23 | 1 / 11 |
| S / 16 | 1.26 | 25.42 | 3 / 11 |

Portra 和 Gold 的 H（輸入 192…254）、M（64…191）、S（1…63）測試區，
均得到十一個不同 RGB8 結果；所有四種濾鏡的完整灰階沒有檔位方向反轉或空間亮度反轉。
無 LUT 及部分 Classic Chrome 深黑接近輸出極限時仍可能有重複量化值，不能要求所有像素永遠十一檔不同。

四種濾鏡各三項控制的 0 檔，共十二張完整輸出與舊版逐像素相同。
新舊比較與零檔證明：`comparison.json`；新舊曲線圖：`comparison.png`／`.svg`；
完整逐檔圖：`curve_stops.png`／`.svg`；各濾鏡的 `photo_stops.png` 可查看實際照片。
灰階 CSV 的 `pre_*` 在新版代表「移除 LUT 的對照」，不是後置曲線執行前的實際管線位置。

## 已知幅度與限制

這版極端檔位明顯強於舊版。留存照片在 Classic Chrome 的 S+5，全黑像素由 0.49% 增至 4.86%；
這是實際新增暗部裁切，不能稱為恢復細節或沒有副作用。建議實機先比較 ±1／±2，再評估端點強度。
本批影像各極端檔沒有全 RGB 白色裁切；這不證明其他照片或曝光設定不會裁切。
尚未量測真實螢幕觀感、3DS 上的曲線處理耗時或新拍攝樣本；它們由此次預覽確認。

## 治理與驗證

- 分類：`film-darkroom/v1` 離線計算行為，以及 `film-camera-ui-package/v1` 的來源凍結／打包邊界。
  不要求 LaneAdmission，沒有 admission override，proof 沿用 none／local_trace。
- CONTRACT_MATRIX 先更新曲線位置、形狀、係數、v5，以及已驗證基底的顯式暗房 patch 打包規則。
- verifier 未放寬未知來源；header 生成可使用已驗證基底矩陣，打包器核對 build receipt、manifest 和相機檔案雜湊。
- 未新增 runtime-internal metadata 欄位；`output_tone` 是可保存的影像參數，係數與快取不寫入配方。
- 更新／新增測試：1,331 組曲線單調性；全部灰階及其他控制極端組合的檔位方向；
  常數 LUT 後仍有十一檔不同輸出；C1 接點；端點；v4/v5 獨立往返；v5 損壞／v6 拒絕；
  換 LUT 保留設定；預覽保存與取消；十二張 0 檔逐像素相同。

執行並通過：

```sh
make test CC=/usr/bin/clang \
  CPPFLAGS='-Iinclude -D_POSIX_C_SOURCE=200809L -I/usr/local/opt/jpeg-turbo/include' \
  LDFLAGS='-L/usr/local/opt/jpeg-turbo/lib'

python3 tools/stage_darkroom_patch.py \
  --base build/darkroom-offline-20261115-r1 --out build/darkroom-curves-20261115-r4

python3 tools/test_curve_stops.py \
  --frozen build/darkroom-curves-20261115-r4 \
  --source build/worktrees/Film3DS_/rolls/ROLL0067.F3S \
  --out build/curve-stops-fixed-20261115-r4 --cc /usr/bin/clang \
  --jpeg-include /usr/local/opt/jpeg-turbo/include --jpeg-lib /usr/local/opt/jpeg-turbo/lib

python3 tools/camera_ui15_package.py deploy \
  --out build/darkroom-curves-20261115-r4 --ip 192.168.0.69 --port 17491
```

ARM11 build 在固定 devkitARM 容器執行既有 `camera_ui15_package.py build`，ARM11／VFPv2、
無已連結 NEON／ARMv7 barrier 與內嵌八個 LUT／顆粒資源驗證通過。保留既有 Y2R 診斷字串截斷 warning，
未改相機診斷功能。沒有新增重試、隊列或相機採集；NetLoader 只傳入一次。
