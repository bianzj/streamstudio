# HiStream 地形水热经验场

入口：**VoxelEB → DEM 地形 → 地形水热经验场**。气温/水汽和土壤水可以分别启用。旧工程缺少这些字段时全部关闭。当前仅支持 VoxelEB，其他模式启用时明确报错。

## 参数与高度基准

配置保存在 `configuration.scene.terrainEnvironment`，同时支持 XML 的 `Scene/TerrainEnvironment` 属性。高度单位 m，统一以导入 DEM 的最低有效高程为零；无 DEM 时以平地为零。场景显示偏移不会改变高度基准。

| 参数 | 默认值 | 含义 |
|---|---:|---|
| airEnabled | false | 启用气温和水汽压高度变化 |
| referenceHeight | 2 | 气象温湿度参考点相对场景零面的高度，含传感器离地高度 |
| temperatureLapseRate | 0.0065 | 气温递减率，K/m；可为负以表达逆温，也可为零 |
| vaporScaleHeight | 2000 | 水汽压指数尺度高度，必须大于零 |
| soilEnabled | false | 启用指定的土壤水空间场 |
| drainageHeight | 0 | 统一沟谷基准面相对场景零面的高度 |
| soilDry | 0.08 | 高坡位渐近体积含水量，m³/m³ |
| soilValley | 0.35 | 沟谷基准面处的体积含水量，m³/m³ |
| soilScaleHeight | 30 | 土壤水指数衰减尺度，必须大于零 |

所有数值默认值均为未标定的示例参数，不是当地实测结果。含水量输入为体积分数，不是百分数；要求 `0 ≤ soilDry ≤ soilValley ≤ 1`，最终按各土壤材质的 `Satwater` 限制到饱和含水量。

例如 DEM 最低海拔 1200 m，气象站地面海拔 1450 m，温湿度仪离地 2 m，则 `referenceHeight=252`。若假定沟道海拔 1210 m，则 `drainageHeight=10`。不要直接把海拔填入相对高度字段。

## 公式与计算连接

```text
Tair(h) = Tref − temperatureLapseRate × (h − referenceHeight)         [°C]
e(h) = eref × exp[−(h − referenceHeight) / vaporScaleHeight]          [hPa]
e(h) ≤ esat(Tair(h))
RH(h) = 100 × e(h) / esat(Tair(h))                                  [%]
H = max(0, hground − drainageHeight)
θ(H) = soilDry + (soilValley − soilDry) × exp(−H / soilScaleHeight)   [m³/m³]
```

这里 `e` 是水汽压，**相对湿度不是随高度固定递减**，它同时取决于局地气温和水汽压。饱和限制只是经验场的数值约束，不包含云、凝结潜热、降水或水量守恒。

未开启流体时，各体元直接使用高度修正后的空气温度和水汽压。土壤表面取上方 2 m 空气，植被等体元取自身中心高度。它们进入显热、潜热和两种叶片生理方法。开启流体时，气温和水汽压用于初始场及每个气象节点的入流边界，内部由原有平流扩散和表面换热推进；不会对已输送温度再次叠加递减率。浮力温差相对同高度的背景温度计算。地形背景体元以下填为固体，避免地下单元被当作空气。

土壤水按土壤表面高度计算并限制到该材质饱和含水量；用于干土蒸发判定和方法 2 的土壤热传导。蒸发阻力采用简单的相对含水量缩放：`rss_local = rss_material × max(θ_material,0.001) / max(θ_local,0.001)`；材质 `rss` 因而表示该材质给定 `SMC` 下的参考阻力。它也是需标定的经验近似。建筑、水面和叶片不应用土壤水公式。植被的根系水分胁迫参数仍按原有材质给定，未加入根系水力学。

## 工程配置示意

以下对象放在 `configuration.scene.terrainEnvironment` 中：

```json
{
  "airEnabled": true,
  "soilEnabled": true,
  "referenceHeight": 2,
  "temperatureLapseRate": 0.0065,
  "vaporScaleHeight": 2000,
  "drainageHeight": 0,
  "soilDry": 0.08,
  "soilValley": 0.35,
  "soilScaleHeight": 30
}
```

## 适用边界

### 给定 CO2 高度场与局地交换闭合

`scene.terrainEnvironment.co2ValleyExcess`（ppm，默认0）与 `co2ScaleHeight`（m，默认500）提供给定浓度增量：`Ca(z)=meteo.Ca+co2ValleyExcess*exp(-max(z,0)/co2ScaleHeight)`，其中 z 为相对场景基准（DEM最低点）的采样高度。增量送入叶片生化与气孔相关计算；不求解 CO2 输送，也不新增大气气体吸收路径。两个参数需有限，增量非负、尺度大于0。

`fluid.localExchange` 默认 `false`。开启后，土壤和叶片阻力直接使用局地风速及原有局地温湿度，不再通过整体冠层参考风廓线重复衰减。叶片单面阻力为 `140*sqrt(leafWidth/max(u,0.05))`，土壤为 `ln(z/0.01)^2/(kappa^2*max(u,0.05))`，土壤交换高度取流体首层中心 `z=fluidVoxelSize/2`。该闭合仍是经验近似；200 m 等效多孔植被不可解释为真实树冠。

2026-10-30 虚拟地形敏感性案例对 F0/F1 均启用此闭合：F0 为统一局地风，F1 为解析流场。原始未指定此参数的工程保持旧闭合。结果应分别检查速度限幅、表面能量残差及热反馈形式；能量残差通过不等于全耦合稳态或观测验证。

运行时在输出目录写入 `diagnostics/terrain-environment.json`，记录启用开关、高度基准、参数和建模边界。启用气温/水汽高度变化、流体和能量过程输出后，`voxelfluid` 过程产品附带水汽压（hPa）与相对湿度（%）字段；消费者应按 JSON 的 `recordFloats` 和字段偏移读取。关闭新功能时保留原来的 8 浮点过程记录。

- 沟谷基准面是用户指定的常数；未执行河网提取、流向分析或自动 HAND 计算。多沟谷、沿河落差较大或跨流域场景需要当地标定或另行提供真实水文空间场。
- 土壤水是固定的经验输入场，不随降雨、入渗、径流、蒸发扣水或地下侧向交换更新，也不生成土壤水的垂直剖面。
- 湿度高度场不是完整水汽循环；蒸腾和土壤蒸发尚未作为流体水汽源，凝结扣水也未耦合。
- 气压和空气密度仍沿用原模型处理，没有新增高海拔气压订正或可压缩大气过程。
- 未经当地观测标定，不应把经验场生成的温度差异当作已验证的实地精度。

经验依据：Zhang et al. (2002), *Spatial Distribution of Surface Soil Moisture and Evaporation in a Small Watershed of Tiksi, Eastern Siberia*, https://doi.org/10.3178/jjshwr.15.486 。本文采用的是带干端下限的简化参数化形式，不是论文原式及原场地参数的直接移植。
