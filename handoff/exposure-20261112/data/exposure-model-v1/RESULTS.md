# Sensor1 曝光模型与验证交付

状态：15fps积分响应及30fps同场景迁移通过本轮±0.25EV工程判据。绝对ISO、光学快门秒数及跨照明EI迁移未验证。

## 可执行模型

令u=中心Y均值/255，15fps等效粗积分 C=a0+a1·u+a2·u²+a3·u³。
系数：`[3.36242852336696, 138.87753535986718, 33.769154578397604, 665.7970983585049]`。
输入域：Y=20.598–212.532；暗端、过曝端拒绝外推。
原始15fps积分点拟合，不使用30fps点拟合；不是套用sRGB，也不是跨批次拼接亮度曲线。
固定全通道增益时，曝光决策为 Cnext=Ccurrent×C(Ytarget)/C(Yobserved)。
solve_fixed_gain返回预测值，不写硬件，也不把未采样组合宣称为已验证。

## 数值验证

|检查|结果|
|---|---|
|整档留出：16/64/176|最大绝对误差 0.1565 EV|
|30fps独立三点：64/129/388|最大绝对误差 0.0639 EV|
|三点之间6个有向曝光决策复算|最大误差 0.0780 EV；仍只有3个独立实测点|
|前6帧拟合、后6帧验证|19 帧，最大 0.0713 EV，RMSE 0.0376 EV|
|PGM长度及像素ROI对CSV复核|21 张通过|
|证据来源|重新运行原始CSV/逐帧寄存器/AE/AWB严格验证；原件只读|

## 外部参考下的条件性EI参数

外部参考使用用户meter02：ISO100、f/2.8、1/125秒。输出判据明确选Y118。
EI=100×(8ms/t7)×C(Y)/C(118)，t7为现有时序的粗积分项。
下表数字是指定曝光模型下的参考参数，不是标准ISO测定。跨照明的响应不变性仍是前提。
|四通道代码|中心Y|参考EI：粗积分模型|加入fine项的敏感性场景|状态|
|---|---:|---:|---:|---|
|0x0210|9.133|拒绝|拒绝|rejected_dark_domain|
|0x0218|18.160|拒绝|拒绝|rejected_dark_domain|
|0x0220|25.961|159.0|143.6|conditional_meter02_EI_not_absolute_ISO|
|0x0228|37.288|226.9|204.9|conditional_meter02_EI_not_absolute_ISO|
|0x0230|45.710|284.3|256.8|conditional_meter02_EI_not_absolute_ISO|
|0x023C|56.226|366.5|331.0|conditional_meter02_EI_not_absolute_ISO|

fine列只用于量化尚未确定的时序偏移影响，两列不是误差界。
同批0x0220恢复样本的参考EI跨度为 0.0845 EV；寄存器保持不等于光度完全不漂。
0x0220目标阶段与其基准/恢复中位数另有 -0.1444 EV 偏差，不能隐去。

## 可用范围与失败边界

完整实测设置逐行在model.json的observed_complete_controls，不允许任意交叉组合冒充已测。
5fps独立批次只用baseline拟合一个场景倍率，积分扫描留作验证，结果如下：
|粗积分|中心Y|预测误差EV|判定|
|---:|---:|---:|---|
|1|0.000|—|outside_response_domain|
|4|2.319|—|outside_response_domain|
|16|20.586|—|outside_response_domain|
|64|79.463|0.0642|within_engineering_tolerance|
|528|222.432|—|outside_response_domain|
|1585|255.000|—|outside_response_domain|

## 复现

`python3 exposure_model.py build`
`python3 test_model.py`

离线决策示例：`python3 exposure_model.py solve --fps 30 --current-coarse 64 --observed-y 57.326049805 --target-y 88.677205405`。
预测粗积分122.21；从完整实测行中选129，量化差约0.078EV。此命令不连接或写入设备。

边界分类：Film3DS离线模型拟合/原件验证；没有设备操作或运行时行为变更。
Vision CONTRACT_MATRIX、运行时验证器及runtime_internal_fields均未修改。
新增数值与负例检查见test_model.py；另运行既有探针calibration_report测试。
Vision三条pytest未运行：本次不涉及其运行时边界，Film3DS不包含这些测试。
本交付替代derived-ei-review中的未验证曲线试算；旧件保留供审计。
