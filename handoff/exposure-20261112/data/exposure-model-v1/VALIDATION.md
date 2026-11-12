# 本轮实际执行与边界

交付范围：Sensor1固定增益的离线曝光响应模型、曝光决策求解、原始证据复核及条件性参考EI。没有修改主相机、没有连接设备、没有部署新程序。

新增文件均位于本目录：exposure_model.py、test_model.py、model.json、reference-ei.csv、RESULTS.md、VALIDATION.md。旧derived-ei-review试算保留，但本交付替代其结果。

## 已执行

从工作区 `/Users/a64/Documents/Codex`：

```sh
python3 Film3DS/build/hardware/exposure-deep/calibration/exposure-model-v1/exposure_model.py build
python3 Film3DS/build/hardware/exposure-deep/calibration/exposure-model-v1/test_model.py
python3 Film3DS/build/hardware/exposure-deep/calibration/exposure-model-v1/exposure_model.py solve --fps 30 --current-coarse 64 --observed-y 57.326049805 --target-y 88.677205405
```

- build退出0：重新验证四个原始CSV数据源的逐帧控制证据，拟合只使用原始15fps积分扫描，保留30fps三点作独立验证。21张原始PGM长度及像素ROI均通过。
- test_model.py退出0：13项检查全部通过，覆盖整档留出、独立30fps、时间分割、曝光决策、像素一致性、暗端/过曝/非有限值拒绝、单调性、缺失/漂移证据拒绝、参考EI不冒充绝对ISO、原件SHA、5fps域外点及完整组合选择。
- solve退出0：预测粗积分122.2103；选择真实已测完整组合129/[0220,024A,0225,0220]/数字1×；量化差0.0780EV；hardware_applied=false。

从 `/Users/a64/Documents/Codex/Film3DS/build/worktrees/camera-deep-probe`：

```sh
python3 tests/calibration_report.py
```

退出0。既有真实C探针夹具通过完整范围、缺帧、伪锁定、未知种类/范围、拒绝增益编码保留失败、RAM流证据验证。本轮未修改该验证器或其测试。

首次新增像素覆盖检查误写了预期数量23，实际交付依赖5张训练+3张独立验证+13张增益/恢复，共21张。已改为检查明确的原始图像文件集合及13张增益图覆盖；像素误差门限未改，数据未删。

## 数值验证结果

- 整个积分档留出（16/64/176）：最大绝对误差0.15653EV。
- 未参与拟合的30fps三点（64/129/388）：最大绝对误差0.06392EV。
- 从上述三个点派生的六个有向曝光决策：最大绝对误差0.07801EV；不能当作六个新独立实测点。
- 前6帧拟合、后6帧检查：19个在声明输入域内的留出样本，最大0.07132EV，RMSE0.03759EV。
- ±0.25EV是本轮选择的工程判据，不是厂商保证或标准ISO准确度。
- 5fps另一个批次仅一个扫描点落在模型域内并可检验，误差0.0642EV；其他点明确域外，不能据此宣布完整5fps曲线验证。

## 没有完成或没有宣称的内容

- 光学快门秒数独立标定。时间列仍使用现有粗积分/帧长/DMA周期模型，fine项未纳入主模型。
- 绝对ISO及跨照明响应不变性验证。参考EI使用名义f/2.8、用户meter02和Y118输出判据；fine敏感性列不是误差界。
- 全场景、全画面、其他两颗相机、连续预览或任意快门×增益组合验证。
- 主应用接入或设备上的新曝光决策执行。

主应用现有测光代码stability.c使用名义sRGB线性亮度目标0.18，且执行逐像素YUV→RGB。这里拟合的是中心Y均值；Y118仅是中性输出的近似目标，两种统计不完全相同。本模型未冒充现有全画面彩色测光的逐像素校准。

## Runtime governance

边界分类为Film3DS离线证据分析/模型求解，不创建或提交RegionTask、TaskContext、TaskScheduler或LaneAdmission工作。没有运行时行为变化。

- Vision CONTRACT_MATRIX：未改。
- Vision运行时验证器：未改；既有Film3DS原件验证器也未改。
- runtime_internal_fields：未改。
- 新增测试：本目录test_model.py，13项；没有弱化既有合同测试。
- 以下Vision命令未运行，因为Film3DS中不存在这些文件，且本次不涉及这些运行时边界：

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```
