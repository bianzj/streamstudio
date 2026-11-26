# 体元代理网格：仅位置顶点格式（2026-11-26）

适用模式：VoxelRT、VoxelEB。RayTracing 与面元模式（FacetRT/FacetEB）不受影响，仍用 44 B 顶点 + 索引。

## 背景

体元着色器（`shader/shade_state_voxel.glsl`）只读顶点**位置**，法线由叉积得到。旧格式却给每个代理面 6 个顶点 × 44 B（`VertexAttribute`：位置、法线、颜色、UV）+ 6 个 32 位索引 = 288 B。
单个网格的顶点缓冲超过 `maxMemoryAllocationSize`（本机 NVIDIA：4,292,870,144 B）后，nvvk 分配器直接崩溃（访问冲突），对应约 1626 万个面（平坦场景约 4033 × 4033 个 1 m 体元）。

## 三种格式（`control.voxelProxyGeometry`，环境变量 `HISTREAM_VOXEL_PROXY` 优先）

| 值 | 顶点 | 索引 | 每面（2 三角形）顶点+索引 | 说明 |
|---|---|---|---|---|
| `full` | 44 B | 4 B × 6 | 288 B | 旧格式；上传前先检查大小，超限给出明确报错 |
| `position`（B） | 12 B | 4 B × 6 | 96 B | 仅位置顶点 + 索引缓冲，分块上传 |
| `positionNoIndex`（B′，**缺省**） | 12 B | 无 | 72 B | 仅位置的三角形列表（第 p 个三角形 = 顶点 3p、3p+1、3p+2），BLAS `indexType = NONE` |

- 三角形、顶点顺序、重心坐标都与 `full` 相同：`positionNoIndex` 按原索引顺序展开顶点，所以 `gl_PrimitiveID` 与重心坐标含义不变，着色器的几何计算是同一组浮点运算。
- 上传分块：设备本地缓冲一次创建，之后经固定 48 MiB 的 host-visible staging 逐块拷贝，不需要与缓冲等大的 staging，也不需要整块主机临时数组。
- 主机端 `PrimMesh::vertices` 类型没有改（仍是 44 B 的 `VertexAttribute`，约 30 处构建代码不变），只在上传时打包。主机内存里仍有 44 B 版本（2000 万面约 5.3 GB）。
- 位定义：`voxelFlags` 第 12–13 位（`src/base/voxelflags.h`、`shader/voxel_flags.glsl`）；旧宿主没有 `VOXEL_FLAG_VALID` 位时着色器按 `full` 处理。
- 对共享顶点很多的 OBJ 网格（如 forest 的树），`positionNoIndex` 因展开而比 `position` 大（forest 某网格：8.5 MB 对 3.3 MB + 索引），绝对量很小；若关心此类场景的显存，可用 `position`。

## 检验

引擎：`scratch\voxel\engine_proxy`（私有构建）。逐文件 SHA-256 比对（`scattering_node=*.json` 诊断文件除外：其中含计时和 GPU 原子累加顺序，同一模式重复运行本身就不逐位相同）。

- `full` / `position` / `positionNoIndex` 三者输出逐位相同：forest、forest_media（雾/火焰）、forest_periodic（周期边界）、crop、urban_block、urban_accel（加速求解器）、hotcube、rt_small、eb_small，共 27 次运行、零差异文件。
- 缺省（`positionNoIndex`）与**改动前**引擎（`engine_prev`）逐位相同：forest、urban_block、crop（427 个文件）、forest_periodic。
- 三种格式下 BLAS 大小完全相同（几何相同的旁证）。耗时：crop 各 2 次重复，三种格式均 ≈ 13.4 s，阶段累计差 < 3%。
- 工程字段、环境变量覆盖（环境优先）、非法取值报错（`control.voxelProxyGeometry must be "full", "position" or "positionNoIndex"`）均验证。

## 规模实测（RTX PRO 6000 Blackwell，96 GB；平坦土壤 1 m 体元，每体元 2 个三角形）

| 场景 | 面片数 | 三角形 | `full` | `positionNoIndex` | BLAS |
|---|---|---|---|---|---|
| 4033² | 1626 万 | 3253 万 | 临界 | — | — |
| 4500² | 2025 万 | 4050 万 | **明确报错**（顶点缓冲 5.35 GB > 4.29 GB；旧版崩溃） | VoxelRT 9.3 s、VoxelEB 22.9 s | 2.17 GB |
| 5000² | 2500 万 | 5000 万 | — | 11.5 s | 2.68 GB |
| 6300² | 3969 万 | 7938 万 | — | 17.7 s | 4.25 GB |
| 6400² | 4096 万 | 8192 万 | — | **明确报错**（BLAS 4.39 GB > 4.29 GB） | 4.39 GB |
| 8000² | 6400 万 | 1.28 亿 | — | **明确报错**（顶点缓冲 4.61 GB） | — |

- BLAS 约 53.6 B/三角形（与面元模式测得的约 53 B 一致，无压缩前的分配值）；单个 BLAS 不能超过 `maxMemoryAllocationSize`，所以新的上限是**约 4000 万个面**（≈ 6300 × 6300 个 1 m 体元，或 0.5 m 体元的约 3.1 km × 3.1 km），是原来的 2.5 倍。
- 4500² 的 VoxelRT（660/860/10500 nm）与 VoxelEB 结果与 2000²、3200²、5000²、6300² 小/大场景一致（VoxelRT 660 nm 反射率 0.163007，10.5 µm 亮温 303.574 K；VoxelEB 6 个波段统计量到所有位数相同）。
- 含树冠的场景每个浊介质体元 3 张面，同样体元数下面片数更多，上限相应更低。

## 仍然存在的上限与下一步

1. 约 4000 万面片：单个 BLAS 的分配上限。继续往上需要把一个网格拆成多个（C，分块），超过 4000 万面才需要；需要逐处检查 `scene.cpp` 里旋转 OBJ 放置、世界对齐代理网格、`meshId = -1` 的纯编号实例。
2. VoxelRT 其他缓冲：8950 万体元；VoxelEB：`TLAST` 约 6700 万体元（见 `docs/voxel-adaptive-and-direction.md` 第 7 节）。
3. 顶点共享（A）未做：只省约 31%，把上限从约 1600 万推到约 2400 万，且不改变 BLAS 大小；有了 B/B′ 之后对上限没有帮助（瓶颈已经转到加速结构）。
4. 自定义 AABB 求交（D）不采用：命中语义（穿过体元中心面）与路径长度、散射采样都要重写，结果会变。

## 相关代码

`src/base/proxymesh.h`（格式与分块上传）、`src/base/voxelflags.h`、`src/base/accelstruct.cpp`（BLAS 几何、上传前后的大小检查、`HISTREAM_VOXEL_PROXY_STATS=1` 打印 `PROXY_GEOMETRY` / `PROXY_BLAS`）、`src/voxelrt/buffer.cpp`、`src/voxeleb/buffer.cpp`、`shader/shade_state_voxel.glsl`、`shader/bindingLayout.h`（`PositionVertices`）、`src/base/fileio_json.cpp`、`src/renderer/src/project-schema.js`。
