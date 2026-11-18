# AWB ON + Force Unity + Refresh：取得中性 AWB，但尚未維持物理 unity

2026-11-13，Sensor1，5fps，新版產物分別完成 batch09 OFF 對照與 batch11 ON+Refresh（各輪來源與二進位 SHA 留存在 deployment.json）；中間 batch10 是保留的 Refresh/v1 比較條件失敗。全部39份原件的 CRC32、SHA-256 與長度已重算核對，合計2472668 bytes。

|證據|batch09 AWB OFF|batch11 AWB ON + Refresh|
|---|---|---|
|來源|exposure-sweep-check/v5|exposure-sweep-awb-refresh/v2|
|原件|13 / 813529 bytes|13 / 824291 bytes|
|A102|10|14|
|A103 / A104|0 / 3|0 / 3|
|A355|58，Force Unity 保持|58，Force Unity 保持|
|MCU AWB R/G/B|135 / 128 / 103|128 / 128 / 128|
|第一幀 after_stop IFP R/Gr/Gb/B|174 / 165 / 165 / 133|133 / 133 / 133 / 133|
|221F|128|128|
|MCU 2222|264|264|
|3012 immediate → frame8 before|4 → 4|4 → 264|
|恢復|restored=1|restored=1|

每輪 S13 有480筆額外讀取（8 settle frames × 5 boundaries × 12 registers），全部成功。讀取依序進行，不是原子快照。batch11 設定 Refresh 16 polls、恢復 Refresh 9 polls，都確認命令歸零與原狀態3穩定；A103只寫兩次命令5。A11F、A355控制位、A102、五項曝光控制及原IFP均依契約恢復。

結論：AWB-on + Force Unity + Preview AWB + Refresh 組合使MCU色彩增益成為unity，物理增益也成為四通道相同；不能單獨歸因AWB或Refresh。共同倍率仍為133/128，實測221F始終128，不能用 round(AWB*221F/128) 解釋。曝光3012在settle期間回到264，與MCU2222與原保存曝光值相同；尚未識別寫入者及精確覆寫邊界。正式frame8前因3012漂移停止，也未通過IFP=128，不是有效校準；沒有放寬物理unity、曝光鎖定或停止規則。

batch10 13份 / 834848 bytes：Refresh/v1在設定與恢復時命令均已歸零，但A355未知低位變動使原比較不接受，沒有進入S13；原complete=0/restored=0保留。新v2單獨登記已定義控制位mask=0x60，新增低位變化與Force位清除反例，舊v1契約不變。

## Runtime治理與變更

本地硬體診斷compute，明示standalone_hardware_diagnostic override，不經Vision LaneAdmission；證據僅local_trace。先更新Film3DS/tools/exposure_sweep_contract.py，再改實作、verifier與測試。新來源check/v5、Refresh/v1/v2與session來源皆註冊；未知CLI/source拒絕。沒有Vision task或portable metadata變更。

實作變更（camera-deep-probe工作樹）：probes/exposure/calibration_scope.h新增明示scope；main.c新增八項MCU讀取、限定寫入入口及三項本地內部欄位；sweep.h實作有界Refresh與所有退出恢復；calibration.h只在註冊Refresh scope允許Preview AWB=1。欄位sweep_refresh_active、sweep_preview_awb_saved、sweep_refresh_writes已在Film3DS本地矩陣註冊。Vision runtime_internal_fields.py未改。

主工作樹：tools/exposure_sweep_contract.py為契約；tools/exposure_sweep_session.py新增互斥的--awb-refresh單次實驗；tools/exposure_isp_report.py新增v2來源、sensor/label、A103、Refresh命令/恢復與乘法模型驗證；docs/EXPOSURE_SWEEP.md補流程與實測。

測試變更：tests/exposure_isp_report.py、tests/exposure_sweep_session.py；camera-deep-probe/tests/probe_driver.c、tests/probe_plan.c。新增未知source/scope、錯identity、錯命令/第三次命令、33非法state、設定失敗、100 polls逾時、Preview設定失敗、恢復命令失敗、Force保持仍物理漂移、A355未知低位變化及Force位清除測試。

## 已執行命令與結果

- Linux/devkitARM離線容器：make -B build/probe-driver build/probe-plan CC=gcc LDFLAGS=；EXPOSURE_SWEEP_ONLY=1 ./build/probe-driver（21組情境通過）；./build/probe-plan（通過）。日誌../refresh-v2-c-build.log。
- make -f Makefile.probe PROBE_NAME=FilmExposureSweep PROBE_BUILD=build-exposure-refresh REVISION=exposure-sweep-v5-refresh-v2；python3 tools/verify_3dsx.py FilmExposureSweep.3dsx：通過，340984 bytes，SHA256=758a6a7495287918d7d4d4663dbb14a651c594a533d71bdf9aaec68eea4f7d66。
- python3 tests/exposure_isp_report.py：9項通過。
- python3 tests/exposure_sweep_session.py：4項loopback HTTP測試通過。
- python3 tests/exposure_automation.py：13項通過。
- PYTHONPATH=/Users/a64/Documents/Codex/.tools/exposure-calibration python3 tests/exposure_sweep.py：15項通過。
- python3 tools/exposure_sweep_contract.py --header build/worktrees/camera-deep-probe/probes/exposure/sweep_contract.h --check：通過。
- 兩個工作樹git diff --check：通過。
- 實機session：batch09、batch10、batch11均完成原件回收；診斷驗證器均exit2，拒絕不完整/不符合條件的採集。

以下Vision命令未執行，因Film3DS不包含相應測試檔案，亦未新增或修改Vision執行邊界：

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

## 剩餘限制

尚無正式S13測量幀或有效校準模型；共同倍率來源、3012被寫回264的寫入者與觸發操作未證明。AWB與Refresh組合尚未拆分成因果對照。未執行完整矩陣或變更正式產品路徑。
