# LUT 旁车适配及显影顺序（阶段 1B）

本轮在编码底座上增加外部 `.cube.f3a` 描述文件，不改写原始 `.cube`。
设备暗房和主机 `filmctl` 使用同一路径加载器；显影核心统一解释已声明的输入／输出编码。
本轮不新增编码种类，不把任意厂商 LUT 自动判断成 F-Log2 C。

## 创建及安装

例如 LUT 确实要求 F-Log2 C/F-Gamut C 输入，并输出 sRGB，且已经完成高光映射：

```sh
python3 tools/lut_adapter.py look.cube --input FLOG2C --output SRGB --pre-lut-shoulder bypass
```

生成 `look.cube.f3a`，将它和原 `look.cube` 一起放在 SD 的 `/3ds/Film3DS/luts/`。
`--input`、`--output` 和 `--pre-lut-shoulder` 必须明确指定，不自动猜测。
已有旁车拒绝覆盖；需要更换时先自行保留旧文件。既有 LUT 原文完全不写入。
本版支持 SRGB、LINEAR_SRGB、REC709、FLOG2C，分别对应 COLOR_ENCODING.md 中的固定色域／白点。

本轮旁车只支持外置 SD 安装。旧 ROMFS 安装器只复制 CUBE，因此打包工具遇到旁车会在创建包之前
明确拒绝，避免静默遗漏。默认构建仍不附带任何第三方 CUBE。
设备已缓存的 LUT 需重启或切换到其他 LUT 再返回以重新加载；不承诺热监视 SD 文件。

## 格式与优先级

```text
F3LUT1
cube_bytes 12345
cube_crc32 01234567
input FLOG2C
output SRGB
pre_lut_shoulder bypass
```

字节数和 CRC32 由生成工具计算。C 端对解析时实际消耗的原文件字节累计 CRC，包含注释、
换行方式和末尾换行；不重新读取另一份文件计算身份。任何内容变化都需要重新生成适配文件。
CRC32 是意外改动检测，不是抗恶意碰撞的认证；未来可复现编辑资产存档须采用强内容摘要。

- F3LUT1 旁车必须包含版本及五个有序字段，拒绝重复、额外、超长、NUL、非法数字和未知编码。
- 没有旁车且原 CUBE 无声明时，保持原配方假设；预览提示 `assumed profile`。
- 原 CUBE 已有成对 FILM3DS_INPUT/OUTPUT 时继续使用，并提示 `CUBE tags`。
- 有旁车时提示 `adapter`。若与 CUBE 内声明冲突则拒绝，不悄悄覆盖任何一方。
- 旁车过期、损坏、不可读都不能当作“未提供”继续处理；失败不发布 LUT 或导出图像。
- 数值定义域继续由 CUBE 的 DOMAIN_MIN/MAX 或 INPUT_RANGE 控制；本轮不改变钳位、
  插值和旧的同编码边界外推行为，不新增 legal-range 自动缩放或 1D/组合 CUBE。

## 前置高光压缩与顺序

`preserve` 保留现有 RAW shoulder 行为；`bypass` 只在实际使用该 LUT 且强度大于零时，
绕过 `film_raw_scene_grade` 中的前置 RAW shoulder。暗房该项显示 `LUT bypass`，
不改写用户保存的 shoulder 数值。关闭 LUT（强度零）恢复旧行为。
降低强度但不归零时仍保持绕过；该选项是管线策略，不随强度连续混合。

仍按线性场景白平衡／曝光、RAW 自定义明暗、局部曝光、可选 shoulder、LUT 编码／采样／解码、
后置曲线顺序处理。用户主动设置的前置／后置创作调整不会被旁车清零。
F3LUT1 不额外添加输出显示映射；需要对场景输出进行映射时，使用下述 F3LUT2 显式策略。
旁车也不能撤销已写入 JPEG/YUV 的原生 ISP 高光压缩。

历史 `filmraw` 工具在 LUT 分支前已经生成一份供 plain/look 共用的 ISP 图像：
若旁车要求 bypass 且 ISP shoulder 非零，工具在创建输出目录之前返回 UNSUPPORTED。
将该工具的 ISP shoulder 明确设为零可继续使用；当前 DNG/场景路径使用上述分支策略。

## 验证和范围

- C：原文身份、CRLF／无末尾换行、损坏／过期／冲突／缺失旁车、失败输出不变。
- C：带非交换颜色映射的 LUT 验证 WB／曝光在 LUT 前；超白验证 bypass 与 preserve 差异；
  强度零中性、后置曲线仍生效、原场景不变。
- Python→C：生成工具的独立 zlib CRC 与加载器匹配；实际 CLI 显影结果等同于显式参考配方；
  换 LUT 后拒绝旧旁车且不生成成片；原片及原生 JPEG 保持不变。
- 打包：拒绝静默丢失旁车；历史 RAW CLI 拒绝不可能执行的逆高光操作。

这些是显影核心与宿主测试。完整编辑会话恢复、持久队列、相机挂接和实机画面／速度仍未完成。

## F3LUT2：显式输出意图与显示映射

新版保留 F3LUT1 的五个字段，在末尾增加 `output_intent` 和 `display_mapping`。
旧 F3LUT1 仍按原行为加载，不自动升级、不推断 Log 输出是否需要映射。

```sh
# 已完成显示处理的 LUT，明确不再映射：
python3 tools/lut_adapter.py display.cube --input FLOG2C --output SRGB \
  --pre-lut-shoulder bypass --output-intent display --display-mapping none
# 保留场景亮度的风格 LUT，解码后补上一次映射：
python3 tools/lut_adapter.py scene.cube --input FLOG2C --output FLOG2C \
  --pre-lut-shoulder bypass --output-intent scene --display-mapping peak-shoulder
```

两项新参数必须同时指定。省略两项继续生成 v1。两项允许的组合只有
`display/none` 和 `scene/peak-shoulder`；后者要求前置 shoulder 为 `bypass`。
损坏、缺项、未知策略和矛盾组合在加载时拒绝；旧程序会拒绝 F3LUT2 而非误读。
格式末尾如下（前面的文件身份与编码字段保持相同）：

```text
pre_lut_shoulder bypass
output_intent scene
display_mapping peak-shoulder
```

文件首行须为 `F3LUT2`。编码与意图独立：Log 可以承载已处理的显示数据，sRGB 曲线
也可以编码未映射的浮点场景值，不能根据编码名自动决定映射。
设备状态会显示 `scene mapped` 或 `display`；v1 仍显示 `adapter`。

### 本版映射定义与边界

在 LUT 解码到工作空间、完成既有编码空间强度混合之后，将 RGB 解码为线性值。
设最大通道为 p，固定拐点 k=0.75；p≤k 时不变，p>k 时将目标最大值设为
`k + (1-k) × (1-exp(-(p-k)/(1-k)))`，所有通道乘以 `目标最大值/p`，然后编码回
现有显示处理链。后置创作曲线继续生效。这里采用内部、固定版本的高光肩部算子，
不声称符合某个电影／HDR 显示标准，也不替代准确的传感器校准。

峰值缩放保留线性 RGB 比例；它不是色域压缩，负通道仍由原有最终输出规则处理。
不额外改变中灰、黑位、曝光或 LUT 本身。强亮部渐近白点，在有限精度 JPEG 中仍可能
合并层次；它不能恢复已被传感器、原生 ISP 或 LUT 自身裁掉的信息。

- 场景映射要求绕过前置 RAW knee，避免两次技术性高光压缩。
- 用户显式设置的前／后置创作肩部、曲线和其他调整不会被清零，仍可能进一步压缩高光。
- 保持历史 LUT 强度的编码空间混合。强度大于零时，映射作为管线策略完整应用一次；
  强度为零时完全关闭并恢复原 RAW shoulder，所以零点附近不保证连续过渡。
- 主机与设备共用核心处理。无 LUT、v1 配置、未描述 LUT 不启用本算子。
- 只作用于当前 LUT 显影分支；原生预览、原生 JPEG、DNG 原件均不改写。
- 旧 `filmraw` 已烘焙 ISP shoulder 非零时仍拒绝 bypass，不能撤销先前的处理。

本轮补齐 LUT 输出分支的显式映射。无 LUT 的独立输出策略、完整版本化编辑配方、
色域压缩及可持久恢复的渲染任务仍在后续阶段。
