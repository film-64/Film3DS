# Film3DS

面向运行 Luma3DS 的 New Nintendo 3DS 的独立双屏胶片相机。默认进入模拟取景，按 L 打开单个外摄，源图以未压缩像素保存，可从同一份源图反复导出 JPEG。相机模式和原版相册兼容性等待实机验证。

## 快速开始

```sh
# Ubuntu / Debian 主机，安装一次
sudo apt-get install build-essential libjpeg-dev python3-pil

make test
python3 tools/simulator.py
# 浏览器打开 http://127.0.0.1:8765
```

使用 devkitPro 的 `3ds-dev`、Git 和 CMake 构建实机程序。首次构建下载锁定版本的标量 JPEG 库。

```sh
export DEVKITPRO=/opt/devkitpro
make -j4 3ds
python3 tools/verify_3dsx.py Film3DS.3dsx
```

将 `Film3DS.3dsx` 复制到 SD 卡的 `/3ds/Film3DS/Film3DS.3dsx`，从 Homebrew Launcher 启动。X 切换页面，B 回取景，方向键改变画幅、输出或暗房参数，A 确认，R 拍摄，L 切换模拟与外摄，SELECT 切换插值，Y 运行 Y2R 自检，START 退出。下屏也可触摸操作。源图在 `rolls/`，JPEG 在 `exports/`，目录均位于 `/3ds/Film3DS/`。

| 画幅 | Native | 1.5× | 2× |
| --- | --- | --- | --- |
| 4:3 | 640×480 | 960×720 | 1280×960 |
| 3:2 | 639×426 | 960×640 | 1278×852 |
| 1:1 | 480×480 | 720×720 | 960×960 |
| 16:9 | 640×360 | 960×540 | 1280×720 |

3:2 的输出采用可容纳的最大整数比例矩形，居中裁切。画幅和档位在按下快门时写入源图。

## 从源图重新洗印

```sh
./build/filmctl develop input.f3s neutral.jpg
python3 tools/profile.py profile.json look.f3p
./build/filmctl develop input.f3s new-look.jpg look.cube look.f3p
```

`profile.json` 最小内容为 `{"input":"srgb","output":"srgb"}`，两个字段均须根据 LUT 的输入输出声明。接受 `srgb`、`linear-srgb` 和 `rec709`。输入为厂商 Log 或广色域的 LUT 须先明确并实现对应变换。将用户自备的 `look.cube` 和 `look.f3p` 放在 3DS 应用目录，可在暗房中选择 `SD LUT`。项目附带合成测试 LUT 生成器。

- [构建和 CIA 方案](docs/BUILD.md)
- [架构和文件格式](docs/ARCHITECTURE.md)
- [技术及 UI 调研](docs/RESEARCH.md)
- [验证记录](docs/TEST_RESULTS.md)
- [实机测试操作手册](docs/HARDWARE_TEST_PLAN.md)
- [兼容性矩阵](docs/COMPATIBILITY.md)
- [技术决策](docs/DECISIONS.md)
- [调试协议](docs/DEBUG_PROTOCOL.md)
