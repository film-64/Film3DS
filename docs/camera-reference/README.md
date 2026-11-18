# Film3DS 相机／ISP 可追溯资料库（初版）

> 建立于 2026-11-15。事实基线：`codex/camera-hardware-handoff-20261114` @ `974b745e2c375bc5b04c01c11939397efff4fbe7`；硬件记录截至 2026-11-14。本目录为开发检索索引，不自动授权寄存器写入，也不代表所有列出的实验已通过。

## 查资料的顺序（参考 → 核对 → 实测 → 才考虑逆向）

1. 先查 `findings.csv`：是否已经得出结论；看 `evidence_grade`、`decision`、`limitation`。
2. 再查 `registers.csv`：定位物理寄存器或 MCU 逻辑变量；**地址已公开 ≠ 已在 N3DS 验证**。
3. 按 `source_ids` 到 `sources.csv` 找原厂、BlocksDS、GBATEK、SNAP、libctru 和项目原件。
4. 按 `project_evidence_path` 与 `experiments.csv` 查对应批次的真实结果及失败理由。完整图像与原始包如不在公开交接包，必须从保留的私有原件合法获取，不能将缺图包验证成成功。
5. 只有在原始型号文档、已公开实现和当前实验记录都无法回答时，新增**最小可证伪的 N3DS 实机测试**。只有当实验暴露出真正未知的写入者、时序或传输行为，才进行更深层逆向。

## 查询

本目录直接支持 GitHub 代码搜索，也可在本地使用仅依赖 Python 标准库的查询脚本：

```bash
python3 tools/camera_reference.py --verify
python3 tools/camera_reference.py 0x32D4
python3 tools/camera_reference.py "RAW"
python3 tools/camera_reference.py "CCM" --table findings
python3 tools/camera_reference.py "MT9V113" --table implementations
python3 tools/camera_reference.py --table registers --status dsi_implementation_only
python3 tools/camera_reference.py 133 --json
```

五张表：

- `sources.csv`：资料出处、权威性、硬件范围、开放状态、许可和使用限制。S02 为**存在但尚未找到全文**的原厂《Register and Variable Reference》。
- `registers.csv`：物理寄存器与逻辑 MCU 变量的**关键子集**，不是 MT9V113 完整寄存器表；每个地址区间带来源和 N3DS 状态。
- `experiments.csv`：由交接 `index.json` 生成的 31 个历史采集／分析目录索引；并非 31 次成功实验。
- `findings.csv`：可复用的实测结论、明确否定结果与缺口；**这张表是开发决策入口**。
- `implementations.csv`：9 个可运行应用、完整传感器驱动或可构建示例，含实际入口、用途、平台差异及许可注意事项。

## 权威层次与状态定义

| 证据层 | 示例 | 允许得出的结论 |
| --- | --- | --- |
| 原厂（S01） | Sensor 支持 10-bit ADC 与 RAW8/RAW10 | 芯片能力；不能据此断言 N3DS CAMU 可接收 |
| 开源定义（S03/S04） | `SEQ_MODE`、`AWB_CCM` 地址 | 候选编程接口；必须核查位宽与版本 |
| 第三方平台实现（S05/S06） | DSi RAW 旁路和 `snap_capture_raw` | DSi 路线可复用作实验思路；不是 N3DS 既成事实 |
| 本仓库实机证据（S09–S11） | Batch24 的 FD/增益矩阵 | 仅可用于对应 N3DS Sensor1、模式及测量窗口 |
| 离线模型与测试 | 批次拟合与测试代码 | 证明某个已规定数据域的推断；不证明新硬件能力 |

推荐状态：
- `documented_only` / `dsi_implementation_only`：**不要**标为 N3DS 已验证。
- `observed_n3ds`：读到过，未必有功能性因果证据。
- `measured_n3ds_limited`：限定配置下测过；跨模式必须复测。
- `definition_conflict`、`not_demonstrated`、`inconclusive`：**先解决证据缺口**，不要直接生产化。

`passed=true` 在历史报告里经常只代表**数据或诊断契约完整**，绝不自动代表「色彩标定通过」「ISO 准确」或「RAW 已取得」。`experiments.csv` 原样保留 `passed/null/false`，具体语义须查看相应报告。

## 完整可参考项目的优先顺序

1. **SNAP Camera / SNAP(lib)**：真实 DSi MT9V113 驱动、独立可用的 SNAP(lib) 和 10-bit Bayer RAW 旁路。参考 https://snap-camera.neocities.org/dev/snaplib 和 https://snap-camera.neocities.org/dev/camera 。代码 GPL-3.0-or-later；N3DS CAMU 接收仍未验证。
2. **Intel AtomISP MT9V113 V4L2 driver**：同型号原生 Linux 驱动和约千行初始化/CCM 表；逐项交叉比对 A348..A350、Refresh、PLL、增益和 CCM。参考 S15/S16；GPL-2.0，不能直接移植 Linux 主机驱动。
3. **DSi 全应用**：addisonking/dsi-camera、Epicpkmn11/dsi-camera，提供完整预览、YUV 拍照、存档和部分视频/图库；不等于 Bayer RAW。
4. **3DS 实现**：devkitPro 3ds-examples/camera/image、3DSCameraExample 和 PixelPix3D，解决 N3DS 端 CAMU 接收、DMA/显示和界面结构；不证明底层 RAW。

可通过 `python3 tools/camera_reference.py --table implementations RAW`（参数顺序不限）查询对应实现。详见 `implementations.csv` 中的 `entry_points`、`validation_gap` 和 `recommendation`。

## 已知高风险歧义

- “raw”有两义：当前探针中的 `raw=yuyv` 表示**完整 YUYV 未压缩缓冲区**，不是传感器 Bayer RAW。查询 `F019`。
- 芯片支持 RAW，SNAP 在 DSi 成功旁路；**N3DS 的 CAMU RAW 接收仍未证明**。查询 `F002`–`F005` 与 `REG-017`、`REG-018`。
- BlocksDS `camera.h` 的多个 AWB_GAIN 宏重叠映射到 `0x2348`；不可当作多个可靠地址复制。Intel MT9V113 驱动（S16）的实际表列出 A348 缓冲速度、A349 跳变除数、A34A..A34D 增益界限、A34E/A34F/A350 三通道 AWB 增益，且 Film3DS 实验已实际观察后三个地址。移植前仍应按位宽/固件交叉核对。查询 `REG-029` 与 `F016`。
- Batch24 的 FD OFF 可保持部分增益，但没有证明 ISP 内部写入函数、长期可靠性、任意数值与所有帧率。查询 `F006`–`F008`。
- Batch28 15fps Y 模型通过限定条件验证，同时 5fps 有 CCM 配置差异，RGB 模型失败。三者不可合并为「曝光已完整标定」。查询 `F012`–`F014`。
- SNAP(lib) 说明其发行采用 GPL-3.0-or-later；引用行为和文档可作为研究依据，**直接复制实现代码前必须单独完成许可兼容性审查**。

## 新任务的决策单（复制到 Issue/PR）

```text
问题 / 目标：
适用机型／Sensor／固件（chip_id、寄存器相关配置）：
已有原厂依据：Sxx + 页码/章节，或“未找到”：
已有开源实现：Sxx + 版本/函数，或“未找到”：
本项目实测：Fxxx / REG-xxx / batch，及测量边界：
复用方案与许可检查：
目前唯一未解的假设（可证伪）：
最小实验的输入 / 预期 / 失败标准：
保存原寄存器、恢复、超时、日志和原始数据方案：
是否真的需要内存／IPC／固件逆向？理由：
```

不允许新增没有来源、没有失败条件的批量盲写或“试一大组未知寄存器看看”。默认只读审计；任何写入都必须有保存、回读、恢复与失败中止要求。

## 维护与同步

1. 增加来源先写 `sources.csv`；每条只填可追溯的 URL，缺失全文时保留空 URL 与 `access=missing`。
2. 寄存器的 `space` 必须区分 `physical` 与 `mcu_logical`。逻辑变量的 8-bit 访问标志不等于 I2C 总线使用 8-bit 读写。
3. 新的 N3DS 事实先添加 `findings.csv` 及证据路径；旧失败结果不可被覆盖或删掉。
4. 新增实验后，依据 `handoff/.../index.json` 重新生成 `experiments.csv`，不要推断报告 `passed` 的跨报告含义。
5. 执行 `python3 tools/camera_reference.py --verify` 检查记录 ID、来源引用及本地证据路径。
6. 在补充新字段、导入更多芯片官方资料或开放源码前，检查其许可与再分发限制；本资料库**只存摘要和链接**，没有复制原厂 PDF、固件或 SNAP 代码。

后续要增加的是“原厂定义、DSi 实现、N3DS 实测、开发状态”四向关联，而不是再生成无法追踪来源的寄存器推测清单。
