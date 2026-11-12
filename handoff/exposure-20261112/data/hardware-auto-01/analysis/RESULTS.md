# 硬件自动曝光与探索报告

硬件证据核验：True；已登记采集周期完整：True。
正常曝光结果：converged。
参考参数状态：bound_to_batch。绝对ISO未宣称完成。

|Phase|目的|原始线性均值|JPEG测光差EV|验证|
|---:|---|---:|---:|---|
|4|auto_normal_base|0.078553|0.00333|verified|
|5|auto_normal_0|0.129265|-0.00408|verified|
|6|auto_normal_1|0.153741|-0.00060|verified|
|7|auto_normal_2|0.166741|0.00029|verified|
|8|auto_gain_base|0.027837|-0.00056|verified|
|9|auto_gain_0210|0.009187|-0.00879|verified|
|10|auto_gain_0218|0.017970|-0.00567|verified|
|11|auto_gain_0220|0.027239|-0.00199|verified|
|12|auto_gain_0228|0.037749|0.00261|verified|
|13|auto_gain_0230|0.049364|-0.00180|verified|
|14|auto_gain_023c|0.065766|-0.00050|verified|
|15|auto_digital_2|0.072102|-0.00011|verified|
|16|auto_digital_4|0.156998|-0.00088|verified|
|17|auto_restore|0.073506|-0.00103|verified|

JPEG来自同一YUYV，只验证编码及测光一致性，不冒充独立物理校准。
每张JPEG可直接查看；原始YUYV与CRC回收记录保留。

## 同批次曝光模型

状态：same_run_model_with_provisional_reference。
模型：Ceq=a0+a1*L+a2*L^2; L=measured nominal linear center RGB luminance。
系数：[37.64267227760783, 1695.6286961087178, 689.9893857087357]。
模型验证：0.036417459493985144 EV。
当前外部参考下基准EI：192.76501758406985；自然光只有后测参考，仍是暂定锚点。

|增益设置|同批响应推算有效倍率|参考EI|状态|
|---|---:|---:|---|
|auto_gain_0210|待补测|待补测|needs_brighter_reference_probe_no_extrapolation|
|auto_gain_0218|待补测|待补测|needs_brighter_reference_probe_no_extrapolation|
|auto_gain_0220|待补测|待补测|needs_brighter_reference_probe_no_extrapolation|
|auto_gain_0228|1.2090|233.1|within_measured_response_domain|
|auto_gain_0230|1.4508|279.7|within_measured_response_domain|
|auto_gain_023c|1.7941|345.8|within_measured_response_domain|
|auto_digital_2|1.9288|371.8|within_measured_response_domain|
|auto_digital_4|3.8775|747.4|within_measured_response_domain|

目标0.18略高于本轮最高实测亮度，目标曝光含小幅模型外推；最低两个增益点不外推。
自然光需前后测光和固定参考幀夹测；不得把本轮后测读数当作全程恒定照明。
