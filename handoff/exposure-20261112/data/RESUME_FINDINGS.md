# 本地原件回收后的核验

来源：`/Users/a64/Documents/Codex/Film3DS/build/worktrees/Film3DS_/probes`。四次新尝试共 41 份原件已复制到 `card-recovered` 并逐文件核对 SHA-256；索引为 `card-recovered/source-sha256.json`。

- 最新 `run-1794400970-000` 为 smoke，仅计划 phase 3。实际只保存了 2 个有效测量帧及第 3 帧的处理检查点；没有阶段完成、SENSOR_END、MEASUREMENTS 或 END 标记。报告 `measurements_complete=False`，没有完整通过的阶段。
- 三次进入 phase 3 的尝试均在 frame 10 附近中断；execution.csv 已记录到该帧 preview_end。不能据此单独认定为显示代码异常。
- 未见新增 CPU 转储或 lifecycle.txt。缺少事件不证明系统从未休眠；也没有足够证据把退出归给磁场。
- 清单 STORAGE：65536 字节/块，1439908 可用块，即约 87.9 GiB。空间不足可排除。
- 用户保留的错误屏幕显示 archive failed / errno=-916437685，即 0xC960454B。SDK 未将该码映射为 POSIX errno，因此保留了原始 FS Result。
- [3dbrew 的 FS PXI 错误表](https://3dbrew.org/wiki/Filesystem_services_PXI) 将它列为 SDMC/NAND bus error。当前程序目标是 SD 文件；优先排查 SD 路径，不能直接断定卡损坏、NAND 损坏或路由器问题。
- 相机、ISO、快门范围或 B 门没有在这些中断尝试中取得新的完整证明。

后续门槛：独立 storage-check-1 自检，不启动 camera/I2C、socket、压缩或文件修复，只在独立新目录打开 10 个文件，交错写入 64×4096 字节/文件、flush/fsync、关闭重开并核对 CRC32。遇错立即失败，不重试、不删除旧文件。2.5 MiB 自检通过也只证明本次有限负载，不构成整张卡的健康或容量认证。

自检源为隔离工作树 `probes/storage/main.c`，本机真实文件测试通过，覆盖原件保留、唯一目录、写入失败和错误读回。`make -f Makefile.storage` 与 3DSX 格式校验通过；设备已通过，报告来自 `storage-check-01/recovered/run-1293845800-000-result.csv`；10 份设备读回 CRC 与独立生成图样的预期 CRC 全部一致，核验见同目录 `verification.json`。

边界分类：Film3DS 独立存储诊断与本地证据核验。未修改 Vision-product 的 CONTRACT_MATRIX、运行时验证器或内部字段；不涉及 RegionTask/LaneAdmission。主应用曝光逻辑未修改。


## 已完成的离线短测与无线回收

- `offline-smoke-01/recovered` 保留 `run-1293846085-000` 的全部 13 份原件，长度与本地 SHA-256 索引已核验。设备时钟已回到 2011 年，后续不得按目录日期排序认定最新测试。
- SD-only 短测正常结束：一个阶段完整，12 个正式测量帧有效；总采集包括预检共 25 帧。一个已知连续模式预检失败被保留，不能据此声称连续预览已经验证。
- 严格报告 `measurements_complete=True`；最后 PGM 的图像均值与记录一致。中心均值约 172.59，高低亮阈值像素比例均为 0，短测未见端点裁切。
- 无线回收改用同一 FTP 控制连接、16 KiB 有界 REST 分段及明确 ABOR/NOOP 响应同步，已完整取回短测原件；无需强制拔卡。采样期间关闭网络与压缩，采样后无线回收。
- 新应用已安装到 `/3ds/FilmCalOffline`，二进制与图标读回 SHA-256 均一致，记录见 `offline-app-install.json`。程序内 X 启动完整 58 阶段，A 为短测；完整采集与绝对 ISO、快门秒数、光圈的物理标定尚未完成。
