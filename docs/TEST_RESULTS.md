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
