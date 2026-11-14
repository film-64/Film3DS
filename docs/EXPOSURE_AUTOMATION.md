# 实机曝光自动化合同

本工具属于Film3DS独立硬件诊断，不进入Vision的RegionTask/TaskScheduler/LaneAdmission。主相机应用没有被改写。

## 固定模型，辨识参数

每通道的模型结构固定为：场景入射光与积分产生信号，经过原生模拟增益、核心数字增益及ISP处理后形成相机RGB，再编码JPEG。光圈固定；白平衡、ISP通道增益、色彩处理和时序都是模型状态。

核心数字倍率与ISP数字倍率分开解码；核心1×不表示整个管线没有数字增益。模拟代码不使用已经与实测矛盾的旧编码公式。寄存器配置值不冒充管线使能证明。

正常曝光保留相机的原生模拟通道比例，将核心数字设为1×，固定ISP增益，只调粗积分。控制误差是 `log2(0.18 / measured_linear_RGB_luma)`；每步使用半幅误差调整，最多3个不同设置，不重复采集同一点作重试。触边、重复设置、未收敛分别记录。

探索使用独立的等通道状态。依据实际Gr模拟代码选择覆盖范围，采集短参考、中参考、高参考、六个增益代码及数字2×/4×。正常拍摄与等通道探索不能共用白平衡状态假装同一颜色处理。

同批数据辨识 `Ceq = a0 + a1*L + a2*L²`。这是固定模型中的响应参数辨识；正常WB与等通道响应分开处理。模型域外、光线漂移或验证失败不产生“已校准ISO”。

## 注册与失败边界

权威设备合同：隔离探针工作树的 `probes/exposure/automation_contract.h`、`calibration_scope.h`、`probe_plan.h`、`calibration_contract.h`。Python寄存器与帧合同在 `tools/exposure_register_contract.py`。

- source：exposure-auto/v4，Sensor1，最多18阶段。
- 仅允许15fps（704行、粗积分1..528）或5fps（2114行、1..1585）。这些是本诊断的已注册范围，不是硬件极限声明。
- 每阶段2帧等待、4帧测量；每帧前后验证控制、AE/AWB及数字层。
- 关AE/AWB后的等待帧完成后，第一次测量前建立一次ISP基准；以后不允许重建基准来接受漂移。
- 四帧亮度跨度超过1/8EV拒绝；没有测量重试。
- 自然光以相同设置的开始／中间／结束参考幀检查；超门限不能直接合并拟合。
- 原始五项控制在退出时恢复；只有干净结束才采样验证恢复。
- 不写PLL、OTP、复位、保留地址或任意用户地址。
- RAM上限28MiB，文件上限40；容量失败停止，不丢证据或回退SD。

## 对时与数据回收

主机首先获取CRC核验过的几个KB摘要：manifest、采集计划、帧节奏。立即报告完成采集，取得同一目标的第三方测光。大IPC日志和完整YUYV随后归档；不得等大文件下载完成才把新测光值绑定到过去的采样。

每轮有独立照明批次、主机启动/摘要时间、设备原始帧时间、参数及源码/二进制SHA。自然光参考必须保留前后时点或明确仅后测；手机给区间就保留区间，不擅取精确值。新读数不回填旧批次。

JPEG由同一YUYV生成，用来验证独立解码/编码测光一致性；不能冒充独立物理参考。第三方曝光读数用于参考EI锚定。标准ISO、光学快门秒数和T-stop仍是独立声明。

冻结RAM只读恢复使用探针 `tools/frozen_transfer_contract.py` 的显式矩阵。frozen-resume-v2最多4次/范围、全快照累计32次瞬时失败；每次重新核对快照指纹。CRC/格式/身份变化立即停止，没有测量重跑。sender使用 `ram_transport_contract.h` 的有界非阻塞发送合同。

## 命令

```sh
python3 tools/exposure_session.py run DEVICE_IP --fps 5 --batch UNIQUE_BATCH --out build/hardware/UNIQUE_RUN
python3 tools/exposure_session.py analyse RUN/recovered --out RUN/analysis --reference RUN/reference.json --batch UNIQUE_BATCH
python3 tools/exposure_session.py recover RUN
python3 tests/exposure_automation.py
```

设备原生验证：在既有Linux/devkitARM容器内执行 `sh tools/validate_exposure_auto.sh`。该脚本属于探针隔离工作树，运行真实C驱动故障测试、旧校准合同、RAM真实TCP传输和ARM构建。

Vision CONTRACT_MATRIX、运行时验证器及runtime_internal_fields未修改。新增状态只属于本地诊断，未放入可移植图片元数据。Vision三条pytest不适用于Film3DS且文件不存在；未运行它们。

## 外部曝光校準基準

使用者指定後續以測光工具A作曝光校準基準。auto-20261112-04的A讀數為ISO100、f/2.8、約1/3秒，使用者估讀EV100約4.5；依名義1/3秒計算為約4.56EV。這是近似曝光參考，原始1/4至1/2秒區間仍保留。B的讀數只作旁證，不與A平均。

提高環境光後必須使用當輪A的新讀數，不能沿用上一批的曝光錨點。比較A的EV100變化與實機在固定處理狀態下的曝光響應，驗證相對曝光模型；通過前不宣稱絕對ISO校準完成。色溫獨立記錄，不用於修正曝光值。
