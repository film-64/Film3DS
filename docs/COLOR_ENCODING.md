# 色彩编码底座（阶段 1A）

`source/color/color_encoding.c` 集中管理 LUT 的色域和传递函数。
内部基准为线性 BT.709/D65；这描述数学坐标，不证明未校准的 3DS RAW 颜色准确。
原 FilmTransfer 数值及配方二进制布局保持兼容。

| 标识 | 色域／白点 | 传递函数 | 输入 | 输出 |
|---|---|---|---|---|
| SRGB | BT.709 / D65 | sRGB | 支持 | 支持 |
| LINEAR_SRGB | BT.709 / D65 | 线性 | 支持 | 支持 |
| REC709 | BT.709 / D65 | 既有 BT.709 分段函数 | 支持 | 支持 |
| FLOG2C | F-Gamut C / D65 | F-Log2 | 支持 | 新增支持 |

`FILM_VENDOR_LOG` 仍拒绝。F-Log2 C 不能解释为 F-Log2/F-Gamut 或其他品牌 Log。
转入顺序是工作线性 RGB → 目标色域矩阵 → 编码；转出先解码，再应用逆色域矩阵。
LUT 强度沿用现有 sRGB 编码域混合，不在本批改变已有风格。

## 使用

继续支持在 `.cube` 开头显式声明成对元数据，例如一个 Log 工作域的风格 LUT：

```text
# FILM3DS_INPUT FLOG2C
# FILM3DS_OUTPUT FLOG2C
```

只有 LUT 确实按此输入／输出制作时才能这样标记。若 LUT 输出已经是 sRGB，
OUTPUT 应为 SRGB；把 sRGB LUT 错标为 Log 不会使其获得正确适配。
此示例不要求改写现有用户 LUT；独立适配旁车在阶段 1B 实施。
旧 host 配方工具也接受 `{"input":"f-log2c","output":"f-log2c"}`。
没有标记的 LUT 保持既有配方假设；单独一项标记或未知编码继续拒绝。

## 数值与边界

- 编码模块保留浮点负值和超白，不做肩部、色域裁切或显示映射。
  标准摄影范围以外使用公式延伸，不宣称设备会产生这些值。
- checked API 拒绝 NaN、Inf、未知编码和产生非有限输出的转换，失败不改写输出。
  像素热路径使用预验证配方，避免每像素重复参数验证。
- F-Log2 C 逆变换包含 F-Gamut C → BT.709 矩阵，不能逐通道反伽马后直接显示。
- 不改变 LUT 定义域钳位／既有边界外推规则；不新增 1D 或组合 CUBE 支持。
- 本批不自动关闭 RAW shoulder，不添加显示 tone mapping，不改变原有曲线。
  Log-to-Log 的 LUT 在显示前仍受现有输出范围约束；完整的处理意图与高光策略在 1B/1C 建立。
- DNG 原件不保存 Log 图像；Log 是显影过程中的临时表示。

## 参考及验证

[Fujifilm F-Log2 C Data Sheet v1.0](https://dl.fujifilm-x.com/technical-data/F-Log2C_DataSheet_E_Ver.1.0.pdf)
给出分段编码／解码公式和 F-Gamut C 原色。正向矩阵保留已有系数，逆矩阵是其逆。
测试参考向量另用文档 xy 原色和 D65 推导 RGB→XYZ 矩阵，双精度计算后固定，
不把生产代码生成的结果作为唯一正确性依据。

`tests/color_encoding.c` 验证标准灰点、三原色／逆矩阵、分段点、负值与超白、
20 档灰阶、异常值、身份转换、真实 LUT 调色入口、RAW 曝光和 WB 组合、源场景不变。
它加入 `make test`，也随现有 `make sanitize` 执行。
`tests/integration.py` 增加 Python 配方→C 加载→Log LUT→JPEG 的贯通检查。

开发过程中已观察到新增核心数值测试通过，旧主机和 RAW 测试输出通过；
原生媒体测试曾被本地缺少 JPEG/PIC、capstone 依赖阻塞。最后一轮完整测试运行期间，
本地环境不可用，未取回最终结果。分支提交后的 GitHub CI 是完整提交的验证依据。
主机数值测试不替代实机颜色／速度、传感器标定或 Camera.app 集成验收。
