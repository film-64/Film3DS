# 调试协议 F3D1

ASCII 文本，一行一个命令。请求为 `F3D1 <uint32-id> <COMMAND> [args]`，回复为 `F3D1 <id> <status> [data]`。请求最多 255 字节（含换行），编号由控制端递增。客户端校验编号和分块长度。返回码按 `include/film.h` 的 `FilmStatus` 定义，0 表示成功。协议保留单帧和单个输出，新的成功捕获或读取会释放之前的图像。

| 命令 | 成功回复或作用 |
| --- | --- |
| `STATUS` | ready、captured、developed |
| `INIT [YUYV\|RGB565]` | 初始化，默认为 YUYV。主机模拟后端目前生成 YUYV |
| `CAPTURE` | 使用当前取景参数拍摄一帧 |
| `INFO` | 宽、高、格式、源帧字节数 |
| `READ offset count` | 读取 1–256 字节的十六进制文本 |
| `SOURCE` / `LOAD` | 保存到或读取指定源文件路径 |
| `DEVELOP [PLAIN\|LUT\|MONO\|WARM]` | 执行管线，回复输出宽和高。LUT 模式要求已加载用户配置 |
| `SAVE` | 编码到指定 JPEG 路径 |
| `ERROR` | 上次操作的 FilmStatus 及后端原始 Result |
| `TIME` | 上次状态变更操作的耗时，单位微秒 |
| `MEM` | 当前保留的源、输出和 LUT 缓冲区字节数。JPEG、GPU、系统及临时峰值另测 |
| `UI [key\|x y\|ADD]` | 导航，或通知模拟器已有一张新照片。回复页、画幅、档位、选中序号、照片数、外观、强度百分比及动作 |

`UI` 键编号依次为下一页、返回、左、右、上、下、确认，范围 0–6。所有下标从零开始。动作 0 为无，1 为拍摄，2 为导出。模拟器负责把动作转换成对应命令。

主机端通过标准输入输出提供交互传输。3DS 在启动时处理应用目录中的 `commands.txt`，将结果写入 `replies.txt`，输出 `debug.f3s` 与 `debug.jpg`。保留命令文件会在下次启动时重新执行。用 `python3 tools/control.py batch --output commands.txt` 生成实机探针，`--dump` 增加整帧分块，`--rgb565` 改用 RGB565。相机可换格式的测试分别重新启动应用。

网络命令传输尚未实现。Luma GDB 与 hbmenu netloader 的实机验证见测试手册。
