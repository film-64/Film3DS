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

## NetLoader 自动实机报告

Homebrew Launcher 按 Y 后，主机执行：

```sh
python3 tools/hardware_qa.py "$N3DS_IP" build/hardware/remote-qa/Film3DS-Hardware-QA-LV10.3dsx \
  --link "$DEVKITPRO/tools/bin/3dslink" --out build/hardware/remote-qa/run-01
```

脚本先在主机对应的局域网地址监听 TCP 17491，再用 3dslink 上传。应用凭启动参数运行一次固定测试并回连上传主机。主机核对来访 IP 和本次运行令牌，收到全部分节及结束标记后写 `report.json`，再回复确认，3DS 等待确认后关闭连接。中断时 `report.log` 保留已收到的记录。

报告包括当前 CPU 频率、L2 状态、内存、三摄采集时间和颜色统计、曝光／白平衡／对比度命令结果、CAM 2× 裁切与全视野恢复、Y2R、CPU LUT、GPU 双线性缩放和三层混合误差。Luma 查询不可用时 CPU 与 L2 标为 `-1`。应用读取配置，测试时不更改时钟和 L2。

相机控制探针用 `CAM_CALL` 记录各次 CAMU 返回码，用 `CAM_AUTO` 记录 AE、AWB 状态和查询返回码。确认硬件 AE 关闭后，`SW_METER` 记录软件全画面平均及中央重点在不同 EV 补偿下的实际档位、建议档位、线性亮度、近白比例和稳定标记。白平衡和锐化的连续帧对照用 `COLOR` 记录中位亮度、色度均值、裁切计数和水平亮度差。最后四帧的中位亮度跨度 ≤2、Cb 和 Cr 均值跨度分别 ≤1 时，`STABILITY` 标记相应统计稳定。比较时保持相机、场景和光照固定。

同一轮报告会对每颗镜头测曝光档位响应，再用一个固定曝光档跨两次取流采样，并在采样后重新查询 AE。固定白平衡会逐个记录两种调用顺序和两种 CAM 设定接口。主机在 `report.json` 的 `camera_verification` 中分别列出 AE 读回、曝光档位响应、固定档输出稳定性、物理曝光锁定可测性和固定白平衡结果。固定档画面稳定只描述当前样本。固定色温命令和 AWB 关闭即使都得到确认，也还需要受控色度测量来验证实际色温。

`python3 tools/hardware_qa.py --summarize path/to/report.json` 可离线分析已有报告。旧报告没有记录的检查会标为证据不足。

默认只返回统计值。已布置灰阶或颜色参考物时可加 `--frames`，同时返回各镜头成功采集的基线 VGA YUYV 和相应矩阵编号。未经控制的现场亮度和色度分布仅用于检查响应，绝对 Gamma 和颜色矩阵需要已知参考输入。
