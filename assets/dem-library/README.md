# DEM 资源分类

- `flat/flat_65x65.asc`：平坦地表
- `slope/single_slope_60x60m.tif`：单一坡面
- `slope/compound_slope_60x60m.tif`：复合坡面
- `hill/central_hill_65x65.asc`：中央丘陵
- `basin/basin_65x65.asc`：盆地/洼地
- `rolling/rolling_hills_65x65.asc`：起伏丘陵
- `dune/transverse_dunes_100m.asc`：横向沙丘（100 m × 100 m，0.5 m 网格，起伏 6.1 m；西风，迎风坡约 13°，背风滑落面约 30° 朝东，丘间平地约 10 m 宽），由 `C:\cases\vehicle-tarp\scripts\generate-desert-example.mjs` 生成

界面中使用原有“场景 → DEM 地形 → 导入 DEM 文件”，选择对应文件即可；导入后自动拉伸到当前场景范围。
