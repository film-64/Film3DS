# 验证记录

执行日期为 2026-11-08。云端工作区为 x86_64 Ubuntu 24.04.3 LTS，主机 GCC 13.3.0、Python 3.12.14、Pillow 13.0.0、Ubuntu libjpeg-turbo 2.1.5。ARM 使用官方 linux/amd64 devkitARM 容器的 `/opt/devkitpro` 内容，镜像 digest 为 `sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b`。devkitARM GCC 16.1.0、libctru 2.7.0、Citro2D 1.7.0、Citro3D 1.7.1、CMake 4.0.3，3DS JPEG 是关闭 SIMD 的 libjpeg-turbo 3.1.3。

| 状态 | 实际命令与结果 |
| --- | --- |
| 通过 | `make -j4 test`，带云端解包后的 JPEG include/lib 路径。C 单元测试覆盖语法拒绝、17³/33³/65³、六个四面体及等分边界、域钳位、YUYV/RGB565、HSL 与分区环绕、双线性/双三次、12 组输出尺寸、颗粒顺序逐像素对照、按键/触屏。 |
| 通过 | `python3 tests/integration.py` 由 `make test` 调用。模拟协议完整回读 614,400 字节，跨语言 CRC 一致，读源图重新洗印得到逐字节一致的 JPEG；替换 LUT 或配置得到预期变化。Pillow 解码 baseline JPEG 并验证尺寸、方向、软件与 Exif 色彩标签。也覆盖截断、篡改、多余字节及版本拒绝。 |
| 通过 | `python3 tests/simulator_http.py` 由 `make test` 调用。通过 loopback HTTP 请求测试双屏页面、模拟预览、触摸及键事件、胶卷新增、选中照片读回和 960×640 黑白导出，越界触摸返回 HTTP 400。提取页面脚本后用 Node 24.22.1 执行 `node --check` 通过。 |
| 通过 | `python3 tools/control.py mock --dump build/mock.yuyv`。模拟帧 640×480 YUYV，协议保留的源／输出为 1,536,000 字节。 |
| 通过 | `make bench` 测量四面体查询，见下表。 |
| 通过 | `DEVKITPRO=<镜像提取目录> make -j4 3ds`，然后运行 `sh tools/verify_arm11.sh Film3DS.elf "$DEVKITPRO/devkitARM/bin/arm-none-eabi-"`、`python3 tools/verify_3dsx.py Film3DS.3dsx`、`3dsxdump Film3DS.3dsx build-3ds/executable.dump`。生成真实 3DSX 和 ARM EABI5 ELF；VFPv2，无链接后的 NEON 符号或独立 ARMv7 屏障指令，3DSX 可解包，内嵌 SMDH 通过边界验证。 |
| 通过 | 上游 FilmOS `make -C ml-dev/filmos/sim -j2`；预先创建 `out/frames` 后，`./out/sim scenes/shelf.txt out/frames/shelf` 得到 97 帧。用 `python3 topng.py <末帧.idx>` 生成 720×480 PNG。 |
| 失败并修复 | 第一次 3DS 编译缺少 `<stdlib.h>`，补头后成功。第一次主机单测在受限环境调用 `tmpfile()` 失败，改用工作区内的可删除临时文件后通过。 |
| 失败并修复 | [首轮 PR CI](https://github.com/film-64/Film3DS/actions/runs/42518733392) 的 ARM 构建成功，检查步骤的 `arm-none-eabi-readelf` 因 PATH 缺失退出 127。改为工具链绝对路径后通过。 |
| 失败并修复 | 审计发现 devkitPro 预编译 JPEG 库链接了 NEON 路径。改为标量构建，增加 ELF 属性、符号与反汇编检查。[含标量 JPEG 的 PR CI](https://github.com/film-64/Film3DS/actions/runs/42519494070) 的主机与 ARM11 作业均通过，已上传 `Film3DS-ARM11`。 |
| 云端工作区失败，Actions 通过 | 本地 `make sanitize` 及带 `ASAN_OPTIONS=detect_leaks=0` 的尝试均在 LeakSanitizer 访问 `/proc/<pid>/task` 时失败。GitHub Actions 主机作业的完整 `make sanitize` 已通过，包括 ASan/UBSan。 |
| 未运行 | CIA 包构建／安装、厂商 LUT 文件导入、浏览器像素级截图测试、DCF 全规范校验。 |
| 硬件待验证 | New 3DS 启动、GPU 显示和按键、CAM 排列/范围/朝向、Y2R 与 DMA、SD 批处理和掉电恢复、原版相册、耗时与内存峰值、netloader、GDB。 |

ARM ELF 属性仍可见工具链的 `Tag_CPU_arch: v7`。链接映射将 devkitARM 默认 `_sync_dmb.o` 的指令段丢弃，最终 ELF 无该函数定义或对应指令。此审计针对已观察到的 NEON 与屏障路径；实际启动与执行列为硬件验证。

云端最终本地产物为 439,376 字节，SHA-256 为 `7cb4c11b751b4c2e0ad76be3f31fae6faae9a779546fb7bcdf217861a42854c1`。GitHub Actions 包含它自己生成的校验文件。

## 主机 LUT 基准

GCC `-O2`，每档 4,915,200 次查询，固定输入序列及合成非线性 CUBE；计时只包含四面体查询循环，不包含文件解析、HSL、缩放与编码。

| 网格 | 耗时 | 吞吐 |
| --- | ---: | ---: |
| 17³ | 160.212 ms | 30.68 MPix/s |
| 33³ | 175.930 ms | 27.94 MPix/s |
| 65³ | 203.155 ms | 24.19 MPix/s |

以上吞吐来自 x86_64 云端主机。ARM11 的实测数据另行记录。

## New 3DS LV6 离线冲洗实测

2026-11-09，用户回报 LV6 QA 下屏读数。整图 1.5×、2× 均触发 CPU 回退，LV6 未保留整图 GPU 误差。独立的 256→512 双线性探针在平滑素材上最大误差 1 个码值，GPU 渲染及读回 2.6 ms，CPU 上传 12.6 ms，CPU 对照 1008.7 ms。该探针只覆盖单块。

VGA 合成帧 CPU 冲洗无 LUT 为 1910.4 ms，33³ 为 3028.8 ms，两次差值 1118.4 ms。65³ 实机读数待补。颗粒对明度 48／128／224 的像素覆盖率为 71%／53%／25%，与同种子主机结果按下屏整数截断一致。

## New 3DS LV7 整图 GPU 实测

2026-11-09，1.5×、2× 均完成读回并显示 `CHECK`。回报的误差包括最大 220、P99 97、拼接边 200 个码值。1.5× 的 MAE 为 2.70、抽样最大 200、GPU／CPU 为 128／2690 ms。2× 的 MAE 为 2.93、抽样最大 225、GPU／CPU 为 174／4792 ms。计时是缩放阶段，两个 GPU 输出均未达到画质线。

## New 3DS 首次自动回传

2026-11-10，NetLoader 启动后，3DS 回连主机并发送了九个测试段。主机收到了最后一段的全部数值，TCP 在结束标记到达前重置，原始记录为 `build/hardware/remote-qa/run-01/report.log`。这轮自动测试未发送场景像素。

- Luma 报告 268 MHz、L2 关闭。三颗镜头均能采集 VGA YUYV 和 RGB565。YUYV 默认中位数 66.6 ms，请求 30 FPS 后约 33.3 ms。三颗镜头的 CAM 建议矩阵均为 709 full。
- 曝光步进 −5／0／+5 的 Y 中位数依次为右后 44／124／156、左后 45／106／180、前摄 41／91／193。三颗镜头接受对比度设定，固定白平衡 3200／5200／7000 K 均返回 `0xE0E053FD`。工厂画质校准查询均返回 `0xD8A053EA`。这轮场景未做基准校准。
- 整图 2× 的最大／P99／拼接边误差均为 1，GPU／CPU 为 177／4872 ms。1.5× 对应误差为 6／3／3，未达到验收线。
- 三层 GPU 合成在 256² 和 512² 的最大及 P99 误差均为 1，GPU 合成和读回分别为 2.64／6.40 ms，CPU 纹理准备和上传分别为 21.65／21.84 ms。该读数覆盖合成测试图。
