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

- 旁车必须包含版本及五个有序字段，拒绝重复、额外、超长、NUL、非法数字和未知编码。
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
本轮不额外添加输出显示映射；Log-to-Log 风格表的最终高光映射仍需后续阶段设计。
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
