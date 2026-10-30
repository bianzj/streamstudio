# OBJ 资产清查与默认推荐

所有推荐模型均为 Y-up、米制 OBJ。`typical` 用于正常真实场景，`simple` 用于超大数量实例；`reference` 只用于历史、特殊或快速展示，不进入默认推荐。

## 车辆

| 等级 | 模型 | 尺寸（宽×高×长，m） | 三角面 | 用途 |
|---|---|---:|---:|---|
| typical | `vehicle/typical/modern_sedan.obj` | 1.82×1.46×4.58 | 2,954 | 现代市售轿车 |
| typical | `vehicle/typical/modern_suv.obj` | 1.98×1.76×4.72 | 3,294 | 现代 SUV |
| typical | `vehicle/typical/modern_city_bus.obj` | 2.84×3.14×11.94 | 660 | 低地板城市公交 |
| typical | `vehicle/realistic/milk_truck_realistic.obj` | 2.79×2.58×4.87 | 3,624 | 厢式货车 |
| simple | `vehicle/city_sedan.obj` | 4.60×1.62×1.98* | 36 | 万级实例占位 |

`car_concept_realistic.obj` 面数较高，`covered_car_realistic.obj` 是苫盖车辆，因此不进入默认组；原 Kenney 卡通车辆已删除。\*旧简易模型的长轴定义与新模型不同，导入时以预览朝向为准。

## 船舶

| 等级 | 模型 | 尺寸（宽×高×长，m） | 三角面 | 用途 |
|---|---|---:|---:|---|
| typical | `ship/typical/modern_container_ship.obj` | 30.05×52×220 | 9,594 | 集装箱货轮 |
| typical | `ship/typical/modern_bulk_carrier.obj` | 28.00×34×190 | 1,854 | 散货船 |
| typical | `ship/typical/modern_cruise_liner.obj` | 42×65×290 | 858 | 大型邮轮 |
| simple | `ship/patrol_boat.obj` | 11.5×5.35×2.9* | 48 | 大数量快速占位 |

历史帆船、原卡通拖轮、渔船、快艇和小型客轮均已删除。\*旧简易模型长轴定义不统一，新的三艘现代大船统一以 Z 为船长方向。

## 植被

| 等级 | 模型 | 尺寸（宽×高×深，m） | 三角面 | 典型场景 |
|---|---|---:|---:|---|
| typical | `vegetation/realistic/jacaranda_tree_realistic.obj` | 24.42×19.47×19.15 | 120,000 | 大型成熟阔叶林、城市公园 |
| typical | `vegetation/typical/mature_fir_realistic.obj` | 18.78×18.94×6.51 | 69,999 | 成熟冷杉林 |
| typical | `vegetation/typical/mature_pine_realistic.obj` | 21.83×20.39×8.55 | 69,998 | 成熟松林 |
| typical | `vegetation/typical/street_tree_realistic.obj` | 6.40×10×9.43 | 59,999 | 行道树、城市场景 |
| simple | `vegetation/tree_pine.obj` | 3.03×12×3.08 | 78 | 大范围森林占位 |

幼树、灌木和草本继续保留为专用资产，不与成熟乔木混用。真实树建议在 Three.js 中采用共享几何实例；若单场景达到万株级，预览使用简易版，体元化时仍可按类别属性统计。

## 生成与许可

- `tools/import-realistic-models.py`：下载 Poly Haven/Khronos 源模型并生成中等面数 OBJ。
- `tools/build-typical-obj-library.mjs`：生成现代车辆、船舶标准模型并统一尺度和顶点颜色。
- 外部源文件与许可保存在模型旁的 `_..._source/` 目录。
- 程序化现代船舶和公交采用项目许可，不对应具体品牌或军民敏感型号。
