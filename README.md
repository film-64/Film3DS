# Film3DS

原生 Camera.app 高级模式的开发顺序与验收边界见 [闭环执行计划](docs/PRODUCT_EXECUTION_PLAN.md)。
本批 Log 输入／输出适配及限制见 [色彩编码底座](docs/COLOR_ENCODING.md)。

目前 3DS 工作流：标准 `.cube`、标准 `.DNG` RAW、完整原图上的二次构图，以及可切换 debug 日志。操作与验证记录见 [标准 RAW / LUT / debug 说明](docs/STANDARD_RAW_DEBUG_20261117.md)。历史实验记录不代表当前界面或已完成的实机验证。

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

将 `Film3DS.3dsx` 复制到 SD 卡的 `/3ds/Film3DS/Film3DS.3dsx`，从 Homebrew Launcher 启动。X 切换页面，B 回取景，方向键改变画幅、输出或暗房参数，A 确认，R 拍摄，L 切换镜头，暗房 A 预览、SELECT 保存当前预览，Y 打开相机控制，START 退出。下屏也可触摸操作。源图在 `rolls/`，JPEG 在 `exports/`，目录均位于 `/3ds/Film3DS/`。

| 画幅 | 原尺寸 | 2× |
| --- | --- | --- |
| 4:3 | 640×480 | 1280×960 |
| 3:2 | 639×426 | 1278×852 |
| 1:1 | 480×480 | 960×960 |
| 16:9 | 640×360 | 1280×720 |

以上为全视野采集。实机取景时摇杆上下切换 2×／1×，由 CAM 服务裁切接收画面，2× 时源图为 320×240。十字键上下选择成片 2×／原尺寸，旧胶卷的 1.5× 配方仍可读取。3:2 的输出采用可容纳的最大整数比例矩形，居中裁切。画幅和成片档位在按下快门时写入源图。

## 从源图重新洗印

暗房上下选择参数，左右调整；WB shift 的网格只改数值，按 A 才预览。完整原图与构图框保留，B＋方向键移动框，构图比例改变框大小。A 同时准备最终裁切的保存快取，SELECT 只编码保存该快取；改变构图后再按 A。RAW 原件为 `rolls/ROLLnnnn.DNG`，原件不会被构图或 LUT 改写；旧 F3S 仍可读取。

暗房颗粒档位使用本机 DxO Silver Efex Pro 3 的三款参考输出。细为 Ilford PAN F Plus 50，中为 Ilford Pan 400，粗为 Ilford Delta 3200 Pro。使用 `python3 tools/measure_grain.py --chart chart.tif` 生成九级灰阶，逐个选择胶卷类型并保存无损 TIFF，然后运行 `python3 tools/measure_grain.py rendered.tif --out measurements.json --profile stock.f3g`。Film3DS 在放大后、裁切和 JPEG 编码前，根据画面亮度查表调节颗粒 RMS 和相邻像素相关性。ROMFS 中保存的是输出统计拟合。

在 VIEW 按 Y 打开相机控制，选择 Native AE 或 Manual。Native AE 使用相机自动曝光与原生补偿；Manual 验证 AE 关闭后使用既有传感器 coarse/gain 档位，禁止走 Native AE 补偿。实际快门时长与 ISO 未标定，不写入虚假数值。

输入 A、A、B、B、上、上、下、下切换 debug；上屏右下角显示 `debug`。重复同一组合关闭。日志追加到 `/3ds/Film3DS/log/film3ds.log`，记录用户按键、模式、寄存器读写、DMA 阶段与错误。RAW 由一个工作线程采集，主屏持续显示阶段；错误返回后恢复操作，不自动重拍。

以下是保留的旧主机配方兼容命令；当前 3DS LUT 载入不需要 `.f3p`：

```sh
./build/filmctl develop input.f3s neutral.jpg
python3 tools/profile.py profile.json look.f3p
./build/filmctl develop input.f3s new-look.jpg look.cube look.f3p
```

直接把 `.cube` 放到 SD 卡 `/3ds/Film3DS/luts/`。启动时按文件名扫描全部 `.cube`（不区分扩展名大小写），不需要同名 `.f3p`。支持 2 至 256 点的 3D CUBE；超出设备内存会报告分配失败。内建 `.cube` 会首次复制到该目录，已有同名文件不覆盖。DNG 采用未压缩 16-bit 容器保存全部 10-bit RGGB 样本，无私有配方标签。

This product includes DNG technology under license by Adobe.

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
