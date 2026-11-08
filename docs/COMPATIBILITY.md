# 兼容性边界

| 项目 | 已验证 | 待验证或约束 |
| --- | --- | --- |
| ARM11 / 3DSX | 官方工具链编译、链接、SMDH 结构和 3DSX 解包 | Luma3DS 及不同 Homebrew Launcher 启动方式、睡眠恢复、持续操作 |
| CAM | VGA、YUV422 与 RGB565 的 libctru 调用已链接，CPU 参考测试通过 | 真实端口、字节顺序、朝向、黑白电平、矩阵、曝光稳定、缓存同步和重启 |
| Y2R | 已编入合成图对比入口，CPU 参考值有测试 | RGB24 字节序、DMA 单元、转换耗时及真实误差 |
| CUBE | 17³/33³/65³、红轴最快、四面体，`TITLE`、`DOMAIN_MIN/MAX` 或 `LUT_3D_INPUT_RANGE` | 1D、混合 shaper、多 LUT、重复或冲突域、未知关键字与其它网格明确拒绝 |
| 色彩 | sRGB、线性 sRGB、Rec.709 传递函数；全局和 12 区 HSL | Rec.709 是传递函数支持；厂商 Log、广色域和设备实测校准待开发 |
| Fujifilm / Panasonic | 已研究厂商定义及条件；合成 CUBE 文件已验证 | 各厂商文件未导入实测。即使语法可读，仍需明确 F-Log/F-Log2/F-Log2C、V-Log/V-Gamut 输入及具体输出 |
| 源图 | F3SR/F3PR 完整性、重读及重新洗印 | SD/FAT 写入、复位和掉电表现，实机真实曝光和 ISO 数值当前记录为未知 |
| JPEG | Pillow 解码、像素尺寸、baseline、最小 Exif 及色彩标签 | DCF 全项符合性、DCIM 命名实验、原版 Nintendo 3DS Camera 缩略图和导入 |
| 桌面模拟器 | 单机 HTTP、导航、触摸坐标和黑白导出 | 3DS 屏幕布局、按键手感和 GPU 呈现 |
| 调试 | 主机管道和 SD 批文件生成；ARM11 端接口编译 | SD 批处理上机、运行时内存峰值、netloader、GDB |
| CIA | 打包条件与命令记录 | 标题配置、构建、安装与启动尚待完成 |
