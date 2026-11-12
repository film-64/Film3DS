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

将 `Film3DS.3dsx` 复制到 SD 卡的 `/3ds/Film3DS/Film3DS.3dsx`，从 Homebrew Launcher 启动。X 切换页面，B 回取景，方向键改变画幅、输出或暗房参数，A 确认，R 拍摄，L 切换镜头，SELECT 切换插值，Y 打开相机控制，START 退出。下屏也可触摸操作。源图在 `rolls/`，JPEG 在 `exports/`，目录均位于 `/3ds/Film3DS/`。

| 画幅 | 原尺寸 | 2× |
| --- | --- | --- |
| 4:3 | 640×480 | 1280×960 |
| 3:2 | 639×426 | 1278×852 |
| 1:1 | 480×480 | 960×960 |
| 16:9 | 640×360 | 1280×720 |

以上为全视野采集。实机取景时摇杆上下切换 2×／1×，由 CAM 服务裁切接收画面，2× 时源图为 320×240。十字键上下选择成片 2×／原尺寸，旧胶卷的 1.5× 配方仍可读取。3:2 的输出采用可容纳的最大整数比例矩形，居中裁切。画幅和成片档位在按下快门时写入源图。

## 从源图重新洗印

暗房按 ZL 循环切换 4:3、3:2、1:1、16:9，按 A 按当前画幅导出。取景页明确选过的画幅也会用于本次会话的导出。未选择时使用源片记录的画幅。下屏显示最终 JPEG 的实际宽高，重新取景裁切不会改写 F3S。

暗房按 Y 循环切换 LUT、颗粒和影调。影调页用左右选择 Post-LUT EV、黑点、中间调、高光、Shadows、Shoulder 或饱和度，上下改变数值。EV 范围为 −4 至 +4，步进 0.25 EV。按 A 导出当前效果，真卷在影调页按 SELECT 会另存并打开一份带新配方的 F3S 副本。打开真卷默认加载该文件已存的配方。

暗房颗粒档位使用本机 DxO Silver Efex Pro 3 的三款参考输出。细为 Ilford PAN F Plus 50，中为 Ilford Pan 400，粗为 Ilford Delta 3200 Pro。使用 `python3 tools/measure_grain.py --chart chart.tif` 生成九级灰阶，逐个选择胶卷类型并保存无损 TIFF，然后运行 `python3 tools/measure_grain.py rendered.tif --out measurements.json --profile stock.f3g`。Film3DS 在放大后、裁切和 JPEG 编码前，根据画面亮度查表调节颗粒 RMS 和相邻像素相关性。ROMFS 中保存的是输出统计拟合。

在 `VIEW` 按 `Y` 打开相机控制，上下选择项目，左右调整。CAM 自动曝光关闭并读回确认。`Metering` 默认是 `Manual`，手动曝光档保持锁定。`Full average` 等权采样取景框，`Center weighted` 给框内中央四分之一面积 4 倍权重，其余区域仍参与。两个自动模式都由 Film3DS 计算并下发硬件档位。切回手动时恢复原来的手动档。

`Exposure compensation` 只影响自动测光目标，范围 ±2 EV，步进 1/3 EV。0 EV 使用名义线性亮度 18% 的目标，+1 EV 将目标翻倍。取景显示当前档位和 AWB 状态。`LIMIT` 表示已到硬件档位上限或下限，仍未达到目标。`SW STEP` 表示目标落在两个硬件档位之间，已停在较接近的一档。相机饱和度随胶卷配方保存，取景超色域时沿 Y′ 收回色度，冲洗时在输出阶段按线性亮度收回色度。

`Sharpness step` 调整 CAM 原生锐化档位，范围 −2 至 +2。此设置作用于拍摄时的相机像素。

```sh
./build/filmctl develop input.f3s neutral.jpg
python3 tools/profile.py profile.json look.f3p
./build/filmctl develop input.f3s new-look.jpg look.cube look.f3p
```

把同名的 `.cube` 和 `.f3p` 放进 SD 卡的 `/3ds/Film3DS/luts/`，在暗房按左右键选色表，按 `A` 从源图重新冲洗。旧版根目录的 `look.cube` 和 `look.f3p` 也可读取。配置支持 `srgb`、`linear-srgb`、`rec709`，以及仅用于输入的 `f-log2c`。两个字段按具体色表的要求填写。

`.f3p` 的强度用于初始化滑块。照片保存当前的实际混合比例，重新打开时沿用该比例和已存色彩参数。旧卷里已记录的比例也按原值使用。

- 胶片版提供 Portra 400 / Endura、Gold 200 / Endura 和 Vision3 500T / 2383。它们来自 Andrea Volpato 的 spektrafilm 模型，由 NicePhoto 做中灰曝光归一化。`python3 tools/import_lut16.py manifest.json stock.lut16 stock.cube` 会核对下载文件的 SHA-256，并生成保持采样值的 `.cube` 和 sRGB 配置。 [来源与许可](https://huggingface.co/Norton0924/OpenSourceLuts/tree/484c3c1)
- 富士 GFX ETERNA 55 的 F-Log2 C 胶片模拟使用 `{"input":"f-log2c","output":"rec709"}`。Film3DS 先把相机 RGB 按 sRGB 线性化，再按富士公开的原色转入 F-Gamut C 并编码为 F-Log2 C，随后采样原厂色表。3DS 采集当前使用标称 sRGB，逐镜头色卡校准仍待实拍。 [原厂色表](https://www.fujifilm-x.com/ja-jp/support/download/lut/) · [F-Log2 C 数据表](https://dl.fujifilm-x.com/support/lut/F-Log2C_DataSheet_E_Ver.1.0.pdf)
- Leitz Phone 的 M9 模拟可用 `tools/import_m9.py` 从原始参数文件导出指定场景、亮度档和色温的 17³ 色表。将 `OffLineM9/m9` 加入 `PYTHONPATH`，传入 `leica_filter_param_m9_preview.bin`、目标 `.cube` 路径、`--lux-index` 和 `--cct`。该路径复用手机关闭 StyleTrans 时的后备色表，保留三线性采样，并以 sRGB 适配 3DS。 [OffLineM9 参考实现](https://github.com/benbaobaoshigemi/OffLineM9/tree/17cd094ee702c01149b6d1d0dec8aed172f81e5c)

如需把本机准备好的色表打进部署包，将它们放在 `<romfs-dir>/luts/`，并将仓库的 `romfs/grain/` 复制到 `<romfs-dir>/grain/`。编译后执行 `3dsxtool Film3DS.elf Film3DS-Color.3dsx --smdh=Film3DS.smdh --romfs=<romfs-dir>`。原厂色表独立于源码保存。

- [构建和 CIA 方案](docs/BUILD.md)
- [架构和文件格式](docs/ARCHITECTURE.md)
- [技术及 UI 调研](docs/RESEARCH.md)
- [验证记录](docs/TEST_RESULTS.md)
- [实机测试操作手册](docs/HARDWARE_TEST_PLAN.md)
- [兼容性矩阵](docs/COMPATIBILITY.md)
- [技术决策](docs/DECISIONS.md)
- [调试协议](docs/DEBUG_PROTOCOL.md)
