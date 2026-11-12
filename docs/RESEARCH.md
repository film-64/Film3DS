# 技术与 UI 调研

## 3DS 工具链、CAM 和 Y2R

| 类型 | 核对结果 | 原始资料 |
| --- | --- | --- |
| 官方开发模板 | `3ds_rules` 提供 ARM11 链接、3DSX 与 SMDH 规则。目标为 ARMv6K、MPCore、硬浮点。PICA200 通过 Citro2D/Citro3D 绘制上下屏。 | [应用模板](https://github.com/devkitPro/3ds-examples/blob/be2001fee08cf8ec7d3366e095e83f6f75da8942/templates/application/Makefile) · [Citro2D](https://github.com/devkitPro/citro2d) · [Citro3D](https://github.com/devkitPro/citro3d) · [官方容器源](https://github.com/devkitPro/docker) |
| libctru 声明与 IPC | `SIZE_VGA` 是 640×480，CAM 支持 `OUTPUT_YUV_422` 与 `OUTPUT_RGB_565`。`camInit` 初始化驱动，退出调用驱动 finalize。官方示例通过 `GetMaxBytes`、`SetTransferBytes`、启动、`SetReceiving`、等待事件、停机，示例目标缓冲区使用普通堆。 | [cam.h](https://github.com/devkitPro/libctru/blob/62b7c42c88b1ee837f513358409d2e5c8f9001a9/libctru/include/3ds/services/cam.h) · [cam.c](https://github.com/devkitPro/libctru/blob/62b7c42c88b1ee837f513358409d2e5c8f9001a9/libctru/source/services/cam.c) · [采集示例](https://github.com/devkitPro/3ds-examples/blob/be2001fee08cf8ec7d3366e095e83f6f75da8942/camera/image/source/main.c) |
| Y2R 源码 | `INPUT_YUV422_BATCH` 明确是 Y0 Cb Y1 Cr，`OUTPUT_RGB_24` 和 `BLOCK_LINE` 可生成线性 RGB888。传输单元要整除字节长度，单元加 gap 小于 32768。头文件允许非线性堆的 FCRAM 接收缓冲区，并记录某些单行 DMA 配置可能提前通知。 | [y2r.h](https://github.com/devkitPro/libctru/blob/62b7c42c88b1ee837f513358409d2e5c8f9001a9/libctru/include/3ds/services/y2r.h) · [y2r.c](https://github.com/devkitPro/libctru/blob/62b7c42c88b1ee837f513358409d2e5c8f9001a9/libctru/source/services/y2r.c) |
| 社区逆向资料 | Camera Services 记录 CAM 与 Y2R 服务，GSP Services 记录缓存清理与失效 IPC。 | [Camera Services](https://www.3dbrew.org/wiki/Camera_Services) · [GSP Services](https://www.3dbrew.org/wiki/GSP_Services) · [GPU 纹理格式](https://www.3dbrew.org/wiki/GPU/Internal_Registers) |
| 项目选择与待测 | VGA packed 缓冲区为 614,400 字节，Y2R RGB24 为 921,600 字节。项目选用 0x80 对齐线性内存、CPU/DMA 交接处清理与失效、有限等待及完成状态检查。Y2R 使用每四行 5,120/7,680 字节、gap 0。CAM 的 YUYV 排列、相机颜色范围、Y2R BGR 内存顺序、单外摄朝向和最小对齐条件需要实测。0x80 为保守实现选择。 | [实机测试手册](HARDWARE_TEST_PLAN.md) |

## 色彩资料

| 类型 | 核对结果 | 原始资料 |
| --- | --- | --- |
| 开源格式实现 | OpenColorIO 区分 IRIDAS 和 Resolve 两种 CUBE 语法。本项目用红轴最快的 3D 表，支持域三元组或 Resolve 单一范围，拒绝 1D/shaper 混合、未知字段和不支持的尺寸。 | [OpenColorIO 格式指南](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/docs/guides/using_ocio/using_ocio.rst) · [IRIDAS 解析器](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/src/OpenColorIO/fileformats/FileFormatIridasCube.cpp) · [Resolve 解析器](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/src/OpenColorIO/fileformats/FileFormatResolveCube.cpp) |
| 标准 | 区分 Y′CbCr 矩阵／量化范围、RGB 传递函数和色域。本轮的相机工作色彩使用待校准的 sRGB 近似。 | [ITU-R BT.601](https://www.itu.int/rec/R-REC-BT.601) · [ITU-R BT.709](https://www.itu.int/rec/R-REC-BT.709) · [W3C CSS Color 4 sRGB](https://www.w3.org/TR/css-color-4/#predefined-sRGB) |
| Fujifilm 厂商资料 | 下载页按机型提供 F-Log、F-Log2、F-Log2C 的 CUBE。逐文件须核对网格、shaper、输入传递、输入原色与输出用途。支持该文件扩展名只说明存在格式交集。 | [FUJIFILM 3D-LUT](https://www.fujifilm-x.com/ja-jp/support/download/lut/) · [FUJIFILM 技术资料](https://www.fujifilm-x.com/ja-jp/support/download/technical-data/) |
| Panasonic 厂商资料 | V-Log/V-Gamut 到 V-709 的变换有明确的输入前提，导入前要确认目标档案及表变体。CAM 交付经过相机管线的 YUV；即使将其数学转换到某种 Log，原传感器动态范围仍不可恢复。 | [Panasonic LUT 下载与说明](https://av.jpn.support.panasonic.com/support/global/cs/dsc/download/lut/) |
| OM SYSTEM 厂商资料 | E-P7 支持 12 色的饱和度分区。项目借鉴分区数量，自行实现循环插值及 H/S/L 调节。 | [E-P7 官方规格](https://jp.omsystem.com/product/dslr/pen/ep7/spec.html) |

本轮用合成 CUBE 验证语法和插值。厂商文件未下载、未导入，真实视觉匹配需由用户自备合法文件和参考片验证。

## JPEG、DCIM 和原版相册

- libjpeg-turbo 的 SIMD 可由 CMake 禁用。链接审计发现本轮预编译的 3DS JPEG 包含 NEON 例程，因此 ARM 端固定 3.1.3 并显式关闭 SIMD。主机与 3DS 共享 libjpeg API。 [libjpeg-turbo CMake 源码](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/af9c1c268520a29adf98cad5138dafe612b3d318/CMakeLists.txt) · [许可证](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/af9c1c268520a29adf98cad5138dafe612b3d318/LICENSE.md)

- CIPA 的 DCF 2.0 规定 DCIM、目录／对象命名和 DCF 图像要求。独立导出的 baseline JPEG 通过标准解码并带最小 Exif；DCF 全项校验需要按 CIPA 规范另跑。 [CIPA 标准索引](https://www.cipa.jp/e/std/std-sec.html) · [DCF 2.0 原始文本](https://www.jeita.or.jp/cgi-bin/standard_e/pdf.cgi?jk_n=51&jk_pdf_file=CP)
- 3dbrew 记录 3DS Camera 的 `xxxNINxxx/HNI_####` 视频命名例，以及 MPO 文件的 Exif 观察。JPEG 的候选 `HNI_####.JPG` 须先从本机原生拍照确认。不同地区与软件版本的目录、索引和缩略图策略由实机取样验证。 [Nintendo 3DS Camera](https://www.3dbrew.org/wiki/Nintendo_3DS_Camera) · [MPO](https://www.3dbrew.org/wiki/MPO)
- 当前输出放在应用自己的 `exports/`。原版相册实验使用备用 SD 卡、保留原机 JPEG 作阳性对照，分别测命名、尺寸与 Exif 的影响。 [实机步骤](HARDWARE_TEST_PLAN.md)

## FilmOS 移植评估

核对版本 `cfeb0e31ee9a015fa0b7c59c677847ee862fe39c`。软件画布为 720×480 的 8 位索引色，`fos_gfx` 实现绘图原语，`fos_font` 以及外部 FOSFONT 字体包提供位图字形。UI 处理局部损伤与背景重绘，HUD、胶卷架、系统页和暗房分布在不同模块。键由 `fos_port_key` 及抽象事件映射；呈现、调色板、时间、内存和文件都经过 `fos_port_*`。桌面后端可运行同一套 C 页面，Canon 端在 Magic Lantern 的 `filmroll.c` 里映射 Canon 键、overlay、调色板、卡与镜头状态。暗房另有 Canon CR2 解码、相机矩阵和 lossless JPEG 切片依赖。

720×480 直接等比缩放到 320×240 会留下约 27 像素的纵向空白，并将文字与按钮降至原宽度的 44%。强行全屏会改变比例。原尺寸画布为 345,600 字节，多个背景缓冲会累加，3DS 端仍需把索引色转换成 GPU 纹理。原型移植可重写最小 `fos_port_*` 并显示单页。面向产品的移植需要分拆取景和触控布局、重配字体／字号、映射触摸、重写 Canon 暗房后端，工作量高于复用画布本身。云端已实际构建上游模拟器并运行 shelf 场景，产生 97 个索引帧，末帧转成 720×480 PNG。生成过程没有外部电影 LUT 表。

FilmOS 仓库采用 GPL-2.0，资源另见第三方清单。分发由其代码组成的衍生程序时，应评估 GPL-2.0 的组合分发和对应源代码义务，连同字体／素材各自的条款一起审查。当前分支采用独立界面实现，保留仓库已有许可。可将完整 FilmOS 适配作为单独的许可证评估项目。 [PORTING.md](https://github.com/HNGSeries/mfilm-canon-film-simulation/blob/cfeb0e31ee9a015fa0b7c59c677847ee862fe39c/PORTING.md) · [FilmOS 源码及模拟器](https://github.com/HNGSeries/mfilm-canon-film-simulation/tree/cfeb0e31ee9a015fa0b7c59c677847ee862fe39c/ml-dev/filmos) · [Magic Lantern 接入源码](https://github.com/HNGSeries/mfilm-canon-film-simulation/blob/cfeb0e31ee9a015fa0b7c59c677847ee862fe39c/ml-dev/fork/src/filmroll.c) · [第三方清单](https://github.com/HNGSeries/mfilm-canon-film-simulation/blob/cfeb0e31ee9a015fa0b7c59c677847ee862fe39c/THIRD_PARTY.md) · [GNU GPL v2 条文](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)

| 候选 | 实际源码与许可 | 可移植程度 |
| --- | --- | --- |
| RPi Film Simulation Camera，Fujifilm 风格 | Python `camera.py` 和 `camera-pi-zero.py`，GPL-3.0，运行在 Raspberry Pi / Picamera2、OpenCV、GPIO。 [固定版本](https://github.com/prtk1910/RPi-Film-Simulation-Camera/tree/eaf092c3f85d20203b52af4b8ad796ad6fcd31f8) | 预览和操作流程可参考，窗口、相机和色彩代码与 Python/OpenCV 耦合，3DS 需要重写。 |
| PolaPi-Zero，Polaroid 风格 | Python `polapizero_08.py`，仓库主许可 MIT，另含第三方打印机代码与字体文件。运行在 Raspberry Pi Zero / PiCamera、GPIO、Sharp LCD、热敏打印机。 [固定版本](https://github.com/pierre-muth/polapi-zero/tree/5129c5befdec7b802edf18fbcf1b13dc1200a364) | 拍摄、查看与打印交互可参考，显示和硬件调用须更换；复用前还需逐项核对第三方资源。 |
| T-CameraPlus-S3，嵌入式触屏 | `examples`、`components`、`main`，仓库 GPL-3.0，ESP-IDF 和 LVGL，240×240。 [固定版本](https://github.com/Xinyuan-LilyGO/T-CameraPlus-S3/tree/d2d6d36194569706c4a48b54d5af7b7d81f6592c) | 适合比较小屏和触摸交互。迁移需要 LVGL 显示／输入端口和摄像头重写。 |
| Saturnix，实体胶片风格 | `firmware/` 为 MIT 的 Python 固件，文档和媒体另有许可；依赖 Raspberry Pi Zero 2 W、Picamera2 和 LCD。 [固定版本](https://github.com/Yutani140x/saturnix-camera/tree/9c37beffdb6d51b2edf4dccf40cb2df9e90dd6e9) | 可研究交互，硬件和图形栈都要替换。 |
| Camera Sim，检索 Leica／Hasselblad 风格时核查的候选 | MIT 的 Vue 3、TypeScript、WebGL 浏览器光学模拟，资源许可另见 README。 [固定版本](https://github.com/congduan/camera-sim/tree/8f76d46a4f39108db27541d250ae442c5434b36b) | 有实际界面源码，运行环境与设备端无直接交集。此次检索没有确认兼具可复用源码、清晰许可与 3DS 可移植性的 Leica 或 Hasselblad 风格项目。 |

## 胶片响应与相机取景

Film3DS 的取景复用系统 `cam:u`。按原版相机离线分析确认的方式保持采集运行，完成一帧后重新提交下一块 DMA 缓冲，渲染循环只检查完成事件。参考 Magic Lantern 的分层方式，现场曝光与白平衡在相机侧处理，保存源图后才进行胶片冲洗。 [devkitPro CAM 接口](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/services/cam.h) · [Magic Lantern 使用手册](https://github.com/frantony/magic-lantern/blob/master/doc/MANUAL.txt)

胶片响应可使用 Kodak 技术资料中的感光、特性和染料密度曲线，以及 spektrafilm 已数字化的负片、相纸模型。在电脑上计算负片显影与印相，将点对点色调响应烘焙为 3DS 可采样的 33³ 色表。逐镜头校准仍需要参考输入。CAM 服务可读出的出厂参数用于记录硬件基线。 [Kodak E-4050](https://kodakprofessional.com/sites/default/files/wysiwyg/pro/resources/e4050_portra_400.pdf) · [spektrafilm 0.3.4](https://github.com/andreavolpato/spektrafilm/tree/3bb2c2d2801ff68b92019cf1dbcbb133d60832bc) · [Dehancer 的胶片测量流程](https://blog.dehancer.com/articles/how-we-build-film-profiles/)
