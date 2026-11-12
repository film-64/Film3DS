# New 3DS 实机测试手册

以下步骤在装有 Luma3DS 的 New 3DS 上执行。每轮记录主机 Git SHA、3DSX SHA-256、机型、系统及 Luma3DS 版本、地区、启动方式、New 3DS 时钟/L2 设置、测试照片与失败画面。主机侧保存到 `build/hardware/<case>/`，SD 侧日志在 `/3ds/Film3DS/film3ds.log`。

## H01 基线与部署

```sh
make test
export DEVKITPRO=/opt/devkitpro
make -j4 3ds
python3 tools/verify_3dsx.py Film3DS.3dsx
sh tools/verify_arm11.sh Film3DS.elf "$DEVKITPRO/devkitARM/bin/arm-none-eabi-"
export FILM_SD=/path/to/mounted/sd
mkdir -p "$FILM_SD/3ds/Film3DS" build/hardware
git rev-parse HEAD > build/hardware/commit.txt
sha256sum Film3DS.3dsx > build/hardware/3dsx.sha256
cp Film3DS.3dsx "$FILM_SD/3ds/Film3DS/Film3DS.3dsx"
```

用 Homebrew Launcher 启动。预期上屏显示 `SIM` 和画幅，下屏三页可切换。先按 X、B、方向键、A 并用触摸测试同一组状态，确认 START 正常退出，随后尝试一次休眠恢复。黑屏时先核对 SD 上的哈希和 Homebrew Launcher 启动方式，保存错误或异常截图，再试已知可运行的 3DSX。

## H02 模拟胶卷、文件与再洗印

在模拟模式分别拍摄四种画幅、三个档位，各导出一张，另用 SELECT 验证双线性和双三次。预期 `rolls/ROLL####.F3S` 增加，`exports/R####-####.JPG` 不覆盖先前导出，重新启动后可选择原卷。拔卡读回时保留整个 `rolls/` 与 `exports/`。使用 Pillow 或 ExifTool 比对 README 中的 12 组宽高和 Exif 方向。

```sh
mkdir -p build/hardware/storage
cp -R "$FILM_SD/3ds/Film3DS/rolls" "$FILM_SD/3ds/Film3DS/exports" build/hardware/storage/
cp "$FILM_SD/3ds/Film3DS/film3ds.log" build/hardware/storage/
./build/filmctl develop build/hardware/storage/rolls/ROLL0001.F3S build/hardware/storage/redeveloped.jpg
```

若保存失败，先检查空间和 SD 文件系统，再核对 log 与遗留的 `.tmp.*` 文件。在备用卡上单独测试关机、写入失败及旧卷恢复。先备份源图，测试时避免操作原始相册目录。

## H03 CAM VGA、像素排列、范围与恢复

```sh
mkdir -p build/hardware/yuyv build/hardware/rgb565
python3 tools/control.py batch --dump --output build/hardware/yuyv/commands.txt
python3 tools/control.py batch --rgb565 --dump --output build/hardware/rgb565/commands.txt
cp build/hardware/yuyv/commands.txt "$FILM_SD/3ds/Film3DS/commands.txt"
```

分别复制两种命令文件，重启应用一次，取回 `replies.txt`、`debug.f3s`、`debug.jpg`、`film3ds.log` 到对应日志目录。预期 `INFO` 为 `640 480 1 614400` 或 `640 480 2 614400`，整帧 2400 块，全部回包编号对应。可按下列命令将十六进制回复与 F3SR 数据做字节比较。

```sh
python3 - build/hardware/yuyv <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
rows = [line.split(maxsplit=3) for line in (p/'replies.txt').read_text().splitlines()]
chunks = [bytes.fromhex(r[3]) for r in rows if int(r[1]) >= 13 and r[2] == '0']
assert len(chunks) == 2400 and all(len(c) == 256 for c in chunks)
assert b''.join(chunks) == (p/'debug.f3s').read_bytes()[304:]
PY
```

用非对称标记的红、绿、蓝、灰阶与白黑目标测试，记录实际朝向和光照。检查灰阶 Y 值分布、色块上 Cb/Cr 的方向，以及 RGB565 中通道的相对位域。对相同源图比较 601/709、full/limited 四种候选，保存所选矩阵、理由和卡片照片。用 L 启用实际取景、拍摄后退出并重复 20 次，检查卡顿、损坏和服务释放。失败时先读 `ERROR` 的十六进制 Result，按 `camInit → Activate → SetTransferBytes → Wait → IsFinishedReceiving → cache` 顺序定位。遇到色偏先检查字节排列、行宽和范围，再判断 LUT。

## H04 Y2R 与颜色诊断

应用内按 Y，保存 `film3ds.log` 中 `Y2R comparison` 和结果。预期基于合成帧的 RGB 逐分量最大误差不超过 4，此阈值用于初始定位。如果误差偏高，分别检查 RGB/BGR、标准系数、块布局、缓存同步和传输完成顺序。若硬件超时，记录 4 行传输单元配置，再逐个对照 2/8 行，保留每个版本的日志。

```sh
python3 tools/fixtures.py build/luts
./build/filmctl profile build/test-look.f3p
cp build/luts/swap17.cube "$FILM_SD/3ds/Film3DS/look.cube"
cp build/test-look.f3p "$FILM_SD/3ds/Film3DS/look.f3p"
```

在暗房选择 SD LUT，分别使用 0% 与 100%，检查红蓝互换和强度混合。再用 33³、65³ 合成 LUT 重复测试。记录 `TIME`、`MEM` 与时钟配置，分别测原生、1.5×、2×及两种插值。协议报告保留缓冲区大小；用 GDB 或实机内存采样另外记录瞬时堆与线性堆峰值。云端基准只作主机参考。

## H05 原版相册对照

使用备用卡，先用原版相机拍一张 2D JPEG，备份原机文件、目录名与 Exif。离线从应用导出 Native 4:3 JPEG，优先在与本机拍照相同的目录中选一个未占用的原机风格编号。仅复制测试文件，用 `test ! -e "$target"` 确认目标空闲后再执行 `cp candidate.jpg "$target"`。先测原尺寸，再测 1.5×、2×；每轮记录索引、缩略图、打开以及复制表现。失败时逐项对比原机 JPEG 的 SOF、APP1、缩略图、文件名和编号。恢复备用卡副本时移除该轮测试文件。

## H06 网络部署与 GDB

在同一受控局域网启动 Homebrew Launcher，按 Y 开启 netloader，再执行：

```sh
"${DEVKITPRO}/tools/bin/3dslink" -a "$N3DS_IP" Film3DS.3dsx
```

预期出现同版本模拟取景。失败时确认 IP、子网和网络隔离，再比较 SD 启动。调试时在 Rosalina（默认 L＋下＋SELECT）启用 GDB stub，在 Process List 选择运行 Film3DS 的进程，使用菜单显示的端口。

```sh
"${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gdb" Film3DS.elf \
  -ex "set logging file build/hardware/gdb.log" \
  -ex "set logging enabled on" \
  -ex "target extended-remote $N3DS_IP:$N3DS_GDB_PORT"
```

检查 `info registers`、`info files`，对照映射地址后再设 `break film_camera_capture` 或 `break film_develop`。3DSX 的装载地址可能与链接地址不同，需按实机映射重定位符号。按 R 或执行暗房导出，检查断点、调用栈和返回状态，最后 detach 并禁用 GDB stub。协议测试完成后将 SD 上的 `commands.txt` 移到日志目录，后续启动即回到正常 UI。 [Rosalina GDB 文档](https://github.com/LumaTeam/Luma3DS/wiki/Rosalina#GDB-Stub) · [hbmenu Netloader](https://github.com/devkitPro/3ds-hbmenu#netloader)

## H07 离线冲洗画质

用 LV8 QA 版进入 DARKROOM，等写卡结束后按住 SELECT 再按 ZL。下屏 `Develop quality` 跑固定素材，不需要拍照或回传 SD 文件。拍下整个结果页即可。

- GPU 双线性使用同一张 640×480 非对称斜坡与棋盘测试图，与 CPU 逐通道对照。分别记录 960×720 和 1280×960 的最大、P99、拼接边最大和平均绝对误差，另记录任一通道误差大于 4 的像素数和最大误差坐标。暂用 P99 ≤ 1、最大与拼接边 ≤ 2 个 8 位通道码值作为验收线。超出时显示 `CHECK`。若未完成读回，显示失败阶段和已完成块数。耗时只记录。正式导出走 CPU。
- LV6 已用固定种子 42 和 50% 强度验证明度 48、128、224 的颗粒覆盖率。主机结果同时记录亮度扰动 RMS 和相邻像素相关，0% 对照逐像素保持原值。
- `90 QA Portra 33` 和 `91 QA Portra 65` 由同一 Portra 400 / Endura 模型分别生成，曝光补偿一致。在电脑上对模型直接输出测量 ΔE00 和 RGB 误差。实机需要做观感对照时，用同一张 F3S、同一画幅及输出尺寸、LUT 100%、颗粒 0%，各导出一次。然后固定 65³ 色表再导出一张颗粒 30% 的图，观察暗部团簇、肤色和高光。对照图保留原始 JPEG 和对应 F3S。

无色卡可用时，实拍选择一张同时包含有纹理的暗部、灰或白物体、肤色或暖色物体、饱和红蓝的照片，并保留当时的光源和相机设置。该实拍用于判断观感和差异，模型误差以电脑上的固定输入为基准。

## H08 LV9 输入映射、颗粒与 GPU 合成

在暗房对同一张 F3S 先关闭颗粒和 Halation，比较 `91 QA Portra 65` 与 `92 QA Portra 65 Dark`，两个预设的曝光补偿相同。然后固定同一色表，按 Y 进入颗粒，左右切 Fine、Medium、Coarse，上下独立调整强度。首轮以 30% 比粒径，再用同一粒径比 20% 和 70%。ZR 单按切换高光晕染，使用带点光源和暗背景的照片做开／关对照。以上都从原始 F3S 冲洗。

暗房按住 SELECT 再按 ZR 测 PICA200 的三层 alpha 合成。底图、暖色光晕、明度遮罩控制的颗粒各占一张 256×256 RGBA 纹理。分别以 256×256 和 512×512 输出记录最大误差、P99、MAE、CPU 纹理准备、GPU 合成与读回、CPU 对照时间。SELECT+ZL 仍测全尺寸 GPU 缩放。这两页使用合成数据，正式导出暂走 CPU。

每颗镜头可配置独立的输入矩阵，文件放在 `/3ds/Film3DS/rear1.rgbmat`、`rear2.rgbmat`、`front.rgbmat`。在相同稳定光线下让 3DS 和可信参考相机拍同一组哑光色块，固定白平衡、曝光及中性设定，避开高光和饱和剪切。没有标准色卡时，结果只用于匹配参考相机。参考图先转为 sRGB 并保存为无损图像。每个镜头分别执行：

```sh
./build/filmctl calibration-rgb ROLL0001.F3S rear1.ppm
python3 tools/fit_camera.py rear1.ppm reference.png patches.csv rear1.rgbmat
```

`patches.csv` 的表头为 `src_x,src_y,ref_x,ref_y,split`。至少六个不同颜色的色块用于 `fit`，另留至少三个 `check`。工具对色块区域取中位数，在假定的 sRGB 线性空间拟合，并要求留出色块的 RGB RMS 下降才输出矩阵。保留配对图片、照明、镜头和白平衡设置。矩阵缺失时沿用现有解码，日志标记 `absent`。
