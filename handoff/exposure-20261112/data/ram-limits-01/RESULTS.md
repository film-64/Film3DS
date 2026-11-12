# RAM有限积分：FFFF哨兵冲突

快照 ram:/run-1293854560，18份原件全部收到；每份原生CRC32、本地SHA-256和长度复核一致。RAM failed=0，无SD写入。

两个有限65535目标均未采样：WriteRegisterI2c成功，随后ReadRegisterI2cExclusive传输成功、服务结果C9602FEF，原始response字段明确为0000FFFF。两个目标后的立即恢复与seed读回校验通过，但逐帧记录显示恢复采样中参数发生漂移；因此不能把它们计为稳定恢复。

严格报告：48个正式帧通过，phase5/6/9/10不完整，所有计划阶段都有记录。不得称78帧完整或硬件长曝光不支持。

已保存的原驱动反汇编在0x0010A64C读取返回半字，0x0010A64E–652与FFFF比较，0x0010A654–65A构造module11/description3EF错误，恰与原始服务返回一致。这是服务的哨兵值碰撞；失败返回不会被当作有效读回。新增独立limits-fffe合同改测65534，保留本轮FFFF负证据，未修改原驱动或错误校验。

逐帧复核：phase6原计划coarse390、blue592/red549，测量时为coarse260、blue612/red552；phase10原计划coarse264/green544，测量时为coarse260/green552。AE/AWB MCU仍为0。脚本在锁定后再次调用SetFrameRate，形成设置混杂；calibration12删除此重复调用，并在每个测量帧前后实时检查控制目标，漂移即拒绝，不再仅由主机事后发现。原始数据不改写。
