# 长曝光段未进入正式测量

SD run-1293852469-000，calibration7，scope=limits。7份已保存原件全部只读回收，长度及本地SHA-256重新核验一致。

预检PGM实际47104字节，应为307215字节；manifest在ACQUISITION_SELECTED附近停止，calls.csv停在预检Stop/ClearBuffer，未持久保存正式阶段证据或END。屏幕用户报告INCOMPLETE、captures5、failures1。

没有正式测量帧，没有长曝光尝试；不能把此次失败视为积分上限或B门不支持。精确原始FS错误码未能持久保存，本轮不能直接沿用先前0xC960454B作为当前读到的errno。

下一步改用显式ram-evidence-1：固定20MiB、有界内存流、相机结束后冻结并只读HTTP回收，不退回SD、不压缩、不重试采样。先实机短测验证新存储与回收链路，再补有限长积分和原始增益代码。
