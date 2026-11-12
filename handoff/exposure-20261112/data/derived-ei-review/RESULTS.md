# 现有数据的条件性参考 EI 初估

这是离线模型拟合结果，不是已验证ISO或硬件ISO上下限。无需接入主程序即可计算。

计算：t7=7×66.667ms/704=0.662882ms；EI=100×(8ms/t7)×C(Y)/C(118)。
C通过已有积分扫描在Y—log(coarse)之间分段插值求逆；不做区间外推。
两列来自不同帧率的旧响应曲线，列间差异不是误差条。

|统一四通道代码|实读302A|中心Y|30fps曲线推算EI|15fps曲线推算EI|sRGB均值代理EI|
|---|---:|---:|---:|---:|---:|
|0x0210|4|9.133|76.8|59.5|18.6|
|0x0218|6|18.160|117.0|116.9|41.0|
|0x0220|8|25.961|147.5|160.6|69.1|
|0x0228|10|37.288|206.3|213.9|125.7|
|0x0230|12|45.710|264.8|264.7|181.1|
|0x023C|14|56.226|361.6|345.4|267.2|

## 前提

- Nominal f/2.8 is applicable; external meter-02 matches illumination and ROI of gain run.
- 15fps cadence/frame-lines gives row period; fine integration and timing offsets omitted.
- Processed Y transfer remains invariant across earlier integration and later gain runs, despite different illumination and channel gains.
- Y=118 is a declared middle-output convention; no known-reflectance gray target was measured.
- Piecewise log-coarse interpolation represents the unsampled response between measured points.

## 限制

- 30fps/15fps estimates are alternate models, not confidence limits or independently validated ISO measurements.
- No extrapolation beyond observed code points; maximum usable sensor ISO remains unknown.
- Y statistics are medians of frame center means; mean-Y inversion is not pixelwise inverse radiometry.
- Gain baseline returns differ from the target 0220 stage: evidence of drift/settling not removed by code hold.
- A same-batch fixed-gain integration sweep is absent; response-curve transfer is an unverified assumption.
- sRGB proxy is intentionally included as model sensitivity, not a calibrated competing ISO result.

原始数据只读；没有新增硬件采集、主程序或Vision运行时变更。
计算脚本calculate.py可重复运行；JSON包含输入SHA-256和全部响应点。
