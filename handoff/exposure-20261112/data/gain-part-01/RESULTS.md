# 独立增益段：控制编码不符合预期

来源 SD `run-1293850416-000`，calibration3，scope=gain，计划14阶段。全部21份原件共11194485字节已只读回收，长度与本地SHA-256复核一致；CSV字段与末尾换行完整。

- 正式测量：9个阶段、108帧完整。phase11没有正式测量，phase13–16未执行。185次成功取帧包括5次预检与9×20次阶段取帧。失败1次来自连续预检。
- END complete=0、aborted=0、io_error=0；不是SD写入中止。本轮 CAL_PROFILE seed_restored=1，phase12恢复采样通过。
- phase11写入BLUE_GAIN 0x03CF成功，读回0x02CF成功；差位0x0100未保持。其他目标寄存器读回一致，AE/AWB MCU均为0。不能由此单次现象断言所有通道或模式的硬件极限。
- 原先按参考公式编码的nominal2×：绿色0x0240保持、积分264保持，但中心Y约55.78，低于基准约170.51；nominal0.5×中心约121.49，nominal1×约167.26。阶段完整不等于增益倍率解释正确，不将这些代码换算成ISO。
- 对应原始绿色增益代码寄存器302A：基准8、nominal0.5×为4、nominal2×为0。与原先解码假设不一致，需做原始代码/位域响应实验；也不能在未确认映射前把302A数值视为线性感光度。
- 下一步把数字倍率独立采集（scope=digital），保持模拟控制代码；不再为补齐数字样本重跑被拒绝的nominal4×模拟点。

此次全部原件保存于 recovered，严格报告 calibration-analysis.json 仍判 measurements_complete=False，完整性清单为 final-verification.json。原驱动与主应用未修改。
