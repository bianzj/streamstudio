# 真实结构模型

以下模型用于需要可见枝干、叶片、车辆部件或船体结构的场景。OBJ 已转换为 Y-up、米制，并将源材质颜色写入顶点 RGB；源 glTF 与 1K 纹理保存在各模型旁的 `_..._source` 目录中。

| 类别 | 模型 | 三角面 | 来源 | 许可 |
|---|---|---:|---|---|
| 植被 | `vegetation/realistic/fir_sapling_realistic.obj` | 70,000 | Poly Haven | CC0-1.0 |
| 植被 | `vegetation/realistic/island_tree_realistic.obj` | 90,000 | Poly Haven | CC0-1.0 |
| 植被 | `vegetation/realistic/searsia_tree_realistic.obj` | 69,998 | Poly Haven | CC0-1.0 |
| 植被 | `vegetation/realistic/jacaranda_tree_realistic.obj` | 120,000 | Poly Haven | CC0-1.0 |
| 植被 | `vegetation/typical/mature_fir_realistic.obj` | 69,999 | Poly Haven | CC0-1.0 |
| 植被 | `vegetation/typical/mature_pine_realistic.obj` | 69,998 | Poly Haven | CC0-1.0 |
| 植被 | `vegetation/typical/street_tree_realistic.obj` | 59,999 | Poly Haven | CC0-1.0 |
| 车辆 | `vehicle/realistic/car_concept_realistic.obj` | 120,000 | Khronos / Unity Fan | CC0-1.0 |
| 车辆 | `vehicle/realistic/covered_car_realistic.obj` | 12,592 | Poly Haven | CC0-1.0 |
| 车辆 | `vehicle/realistic/milk_truck_realistic.obj` | 3,624 | Khronos / Cesium | CC-BY-4.0 |
| 车辆 | `vehicle/typical/modern_sedan.obj` | 2,954 | Quaternius | CC0-1.0 |
| 车辆 | `vehicle/typical/modern_suv.obj` | 3,294 | Quaternius | CC0-1.0 |
| 车辆 | `vehicle/typical/modern_city_bus.obj` | 660 | Stream Studio | Project |
| 船舶 | `ship/typical/modern_container_ship.obj` | 9,594 | Stream Studio | Project |
| 船舶 | `ship/typical/modern_bulk_carrier.obj` | 1,854 | Stream Studio | Project |
| 船舶 | `ship/typical/modern_cruise_liner.obj` | 858 | Quaternius | CC0-1.0 |

默认真实场景清单见 `ASSET_AUDIT.md`。历史帆船、卡通车辆和小型卡通船已删除。真实树不适合按原始高面数直接放置数万实例；大森林建议同一种模型共享实例，并根据体元大小控制 OBJ 填充阈值。
