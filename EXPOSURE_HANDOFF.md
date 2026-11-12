# 曝光標定交接：現版模型已撤回

這是目前工作現場的完整交接快照，不是已校準的曝光模型。

## 入口

- `handoff/exposure-20261112/data/`：830份實機採集、參考標籤、回收記錄及分析文件；約150MB。既有分析結論需依原件重新核查。
- `handoff/exposure-20261112/data-sha256.json`：逐檔大小與SHA256。
- `diagnostics/camera-deep-probe/`：原隔離探針工作樹的可編譯原碼、Makefile、測試、離線採集及回收工具。
- 根目錄 `tools/exposure_session.py`、`exposure_field_report.py`、`exposure_opencv_crosscheck.py`：主機部署、離線分析及OpenCV交叉檢查。
- `docs/EXPOSURE_MODEL_SOURCE_AUDIT.md`：原始來源與錯誤實作核查。
- `handoff/exposure-20261112/binaries-withdrawn/`：已部署版本與安裝器，僅供重現；不要當作校準完成的版本使用。
- `handoff/exposure-20261112/validation-logs/`：先前原生C、故障、RAM/TCP及ARM構建紀錄。流程通過不代表曝光模型正確。

## 使用者需求與已知錯誤

使用者要沿用現有UI與採集方式：離線保存計算曝光、參考曝光與探索邊界，連線回收後修正模型，再用新資料驗證。每張候選應顯示相對EV、積分、增益、預測快門及照片。不是新增一套自動追亮度流程。

field/v1的`field_fit`單點預設斜率1，`field_dose`將積分、Gr類比代碼與ISP Gr配置直接相乘。這些整體響應假設未驗證，不可當作曝光標定。0.18內部亮度達標也不證明第三方曝光匹配。

使用者最新回報：程式預測1/389秒，第三方讀數約1/125秒附近，示意範圍為1/60—1/125—1/250。若以1/125秒計差約1.64EV。該輪仍在設備SD，尚未回收；不能偽造積分/增益配對點。探索邊界沒有充分顯示EV/快門，使用者明確拒絕目前交付。

A為後續指定參考工具。auto-04標籤是ISO100、f2.8、1/4至1/2秒，使用者補充約1/3秒、EV約4.5。B讀數為ISO100/f2.8/1/2秒，只作旁證，不與A平均。自然光變動，各輪參考不能互相挪用。

原件狀態：auto-01全部27份、auto-02全部13份、auto-04全部15份已安全回收；auto-03未完整回收。auto-04正常曝光C528、內部亮度0.184709；探索因亮度不穩停止，未完成完整標定。OpenCV舊序列留出誤差G約+0.0102EV、R約+0.0705EV、B約-0.3551EV，藍通道不通過。

## 重建原路徑

從倉庫根目錄執行：

```sh
python3 tools/bootstrap_exposure_handoff.py
```

建立原工具所需的忽略路徑連結。探針工具鏈使用既有固定映像：

```sh
docker run --rm -v "$PWD/diagnostics/camera-deep-probe:/src" -w /src devkitpro/devkitarm@sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b sh tools/validate_exposure_field.sh
python3 tests/exposure_automation.py
python3 tests/exposure_field.py
```

`tests/exposure_field.py`使用原生C故障注入樣本，需先執行上面的原生工具鏈腳本；不把樣本稱為實機校準。

重建安裝器需先將`handoff/exposure-20261112/reproduction/seed-labels.csv`放到探針的`build-exposure-field-payload/seed-labels.csv`。該單點種子也不代表完整曲線。

設備192.168.0.26。NetLoader17491只能建立帶完整上傳內容的連線；不要空連線測試，曾觸發namelen:11。現版按Y選取已保存輪次、A開啟HTTP5000回收；原始資料存SD `/3ds/Film3DS/probes/run-*`。獨立工具存 `/3ds/FilmExposureField/`。不要覆寫或清空SD資料。

## 保存邊界與驗證

本次只做交接快照，未修正曝光模型、未重新部署、未改Vision-product。Vision的CONTRACT_MATRIX、驗證器與runtime_internal_fields未變；其三條pytest在Film3DS不適用，沒有執行。Film3DS獨立診斷的來源、幀規則與失敗邊界保留在探針contract標頭與Python寄存器合同。

先前：Python曝光證據測試13項通過；field主機測試6項通過的結果應以重新執行確認；原生C正向、ISP漂移/寫入失敗停止、恢復、參考標籤真實檔案讀回及預測凍結測試已執行。完整舊探針回歸、RAM真實TCP與ARM構建紀錄保留。這些驗證的是程序與證據邊界，沒有證明等效快門模型正確。

CPU崩潰轉儲與OS/編譯快取未公開，清單與SHA在`omitted-local-files.json`；本機原件未刪除。既有回收清單可能仍列出這些未公開檔案。主應用與探針均保留原本未提交的工作，不做修正或重新格式化。
