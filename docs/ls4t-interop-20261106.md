# LS4T / STREAM 同化反演接口修复

实现、输入单位、配置字段、使用示例、验证与科学边界的完整说明：
`C:/work/ls4t/docs/STREAM_INTEROP.md`。

新增可选同化参数：`uncertainty`、`covariance`（主观测 N×N 行优先，K²）、`qa`、
`opticalUncertainty`、`opticalQa`、`views[].uncertainty`、`views[].qa`、
`sigmaBrightnessK`、`sigmaLstK`、`sigmaComponentK`、`sigmaReflectance`、
`lstWavelengthUm`、`observationLevel`、`rejectQaBits`、`sourceIds`、`priorSourceIds`。
省略时保留原误差/波长默认值；提供误差栅格后，非正/非有限像元不会退回默认值。
完整协方差与标准差文件互斥，跨光热和跨视角误差仍按独立块处理。

`lsct_fractions.tif` 是归一化有效权重；新增 `lsct_radiance_fractions.tif` 才是
温度相关的实际组分辐射贡献。两者均参考中心像元第一条有效观测。

EB 导数退化会拒绝同化；导数仍是条件于过境历史/稳定度的局部隐式导数，
没有宣称完整时间轨迹梯度或新的地面精度。代码未改动 HiStream 或 StreamEarth EnKF。

验证入口：

```powershell
C:/work/miniconda/python.exe -m pytest tools/test-assimilation-contract.py -q --basetemp .test/interop-check
node --test tools/test-assimilation-config.mjs
```
