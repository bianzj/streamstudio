# 下一版本更新说明（草稿，测试中）

状态：开发版测试阶段，**未发布**。版本号仍为 2.0.1，没有构建正式安装包，也没有创建新的交付目录。本文件记录待发布的变更，发布时再定版本号并改写为正式发布说明。

## 变更内容

**场景动画剥离到 STREAM IMAGING（2026-11-16）。** 场景动画研究功能（HiStream `eSceneAnimation`：撞击 impact-v1、火球/烟尘输运、刚体碎片、相机观测、耦合小镇与机场全天等）不再属于 Stream Studio，整体迁到 `C:/work/streamimaging`：

- 引擎代码（`models/histream/src/animation`、`shader/animation`、`cmake/scene-animation.cmake`、C++ 夹具）改为 STREAM IMAGING 的 HiStream 外挂扩展 `engine/scene-animation`，由其 `scripts/build-animation-engine.ps1` 编进本仓库的 HiStream 核心，输出到 STREAM IMAGING 自己的构建目录，不覆盖 `models/bin_x64`。
- 工具链 `tools/scene-animation`、示例 `assets/examples/impact_v1_building`、开发文档与研究计划、用户手册第 18 章与理论手册第 17 章随之迁出。
- 已隐藏的桌面面板、`/api/scene-animation/*` 服务、schema 中的场景动画导出、示例目录的 `eSceneAnimation` 条目、相关翻译和桌面设置键均已删除；代码存档在 STREAM IMAGING 的 `animation/studio-panel/`。

**HiStream 通用扩展接口。** 核心不再知道任何具体扩展：`src/base/extension.h` 提供模式注册表，CMake 选项 `HISTREAM_EXTENSION_DIRS` 列出外部扩展目录（各含 `histream-extension.cmake`），配置时生成注册函数。产品构建不带扩展，`histream.exe eSceneAnimation …` 返回用法提示（退出码 64；2026-11-20 起用法错误与 FacetEB 未收敛的退出码 2 分开）。`HISTREAM_ENABLE_SCENE_ANIMATION` 选项已删除。

**之前的草稿内容**（场景动画面板、impact-v1、渲染提速选项、内置示例）随功能迁出，原文存档在 STREAM IMAGING `docs/scene-animation/history/streamstudio-release-draft-20261112.md`。

### 模型修正（2026-11-20）

以下修正会改变计算结果，旧结果不能与新版逐值比较。方法见理论手册与 `models/stream/README.md`，修正清单见 `docs/model-remaining-issues-20261005.md` 顶部。

- **VoxelRT 反射率归一**：参考辐照度为 E_dir·cosθs + E_diff（天空不再乘 0.5），固体表面反射环境项去掉 0.5；固体接收面按外法线半球余弦加权，步数上限不再补天空。
- **烟/雾亮度**（VoxelRT、VoxelEB）：单次散射项按每球面度 HG 相函数计 π·p·E_b，环境项取 ½，介质光学亮度明显升高。
- **FacetEB 直散比**：取光照设置的 `directScatteringRatio`（此前存在 Esun/Esky 文件时被文件比例覆盖，约 0.806）；低太阳或封顶丢弃的直射计入漫射。
- **FacetRT 光照**：与 FacetEB 共用 Esun/Esky 光谱形状和直散比（总量仍取 700·cosθs + 50 W/m² 的历史值），不再固定 700/50。
- **Hapke 背景能量**：能量平衡改用守恒约定（Lambert 底值作白空反照率，直射按 A(μ0)/A_bh 保留太阳角变化），长波按 Lambert 半球发射率；影像改为天顶观测方向的方向反射因子/发射率。strength = 0 时不变。
- **STREAM**：亮温加反射天空项 (1−Σw)·B(T_sky)（`skyLongwave` 可设 `{"mode":"none"}` 恢复旧行为）；BSM 湿土改为 SCOPE soilwat（湿土短波红外反射率降低约 10–20%）；夜间及局地背光坡光照组分并入阴影；地形短波先水平分直散再投影；冠层上方阻抗 rar 括号位置更正（北京 Landsat 示例收敛像元 180326→180187）；地类 0/16 无气孔、水体按水面地面。
- **Facet 可见性 A-buffer 自动扩层**（不影响数值）：面元场景溢出时自动把片元层数由 64 加倍到 128/256 并重建资源，不再需要手工设置 `HISTREAM_FACET_RASTER_LAYERS`；crop_gpp_npp 等稠密冠层示例可在默认设置下运行。上限仍为 256 层与显存预算（`HISTREAM_FACET_RASTER_BUDGET_MB`）。
- **示例副本同名覆盖**（界面行为）：再次打开同一内置示例时覆盖 `<示例名>` 副本的工程文件并复用同一目录，副本 `output/` 中的历史结果保留；此前会累积 `forest_2`、`forest_3` 等重复副本。打开日志与复制对话框说明同步更新。
- **VoxelEB 夜间跳过短波**（不影响数值）：太阳在地平线下且实测短波为零的时间节点，先清零短波状态再跳过可见性、直射与散射阶数。forest 示例全天 5.5 min → 4.2 min，输出逐字节不变；日志新增 `SHORTWAVE_SKIPPED` 行。

## 兼容性

- `tools/build-engine.ps1` 构建的产品引擎只含核心（显式清空 `HISTREAM_EXTENSION_DIRS`），不再含场景动画与 Bullet 碰撞模块，也不再需要本地 Bullet 源码；三维 RT/EB 与 STREAM 遥感工作流的代码路径不变。
- 耦合小镇和机场全天所用的 FacetEB 集中参数热节点（`facetrt/lumpedthermal.h` 等）属于核心，保留；其解析夹具从 `tools/scene-animation/tests/lumped_thermal_fixture.cpp` 移为 `tools/test-facet-lumped-thermal.cpp`（目标 `histream_lumped_thermal_tests`）。
- 三维工程 `schemaVersion = 6`、遥感工程 `schemaVersion = 2` 不变。STREAM IMAGING 运行时仍从本仓库导入 `project-schema.js` 和 `tools/runtime-scene.mjs`。
- HiStream 退出码：64 为命令行用法错误（含不带工程路径运行，原先静默跑内置示例；内置示例须显式 `--builtin-example`），2 只表示 FacetEB 计算结束但有未收敛节点；1/3/4 为运行、资源创建、初始化失败。按旧退出码 2 判断用法错误的脚本需修改。
- `voxelrt.json` 过程字段：2.5–5 µm 波段由 `illumination_<波长>` 改名为 `spectral_radiance_<波长>`（与 > 5 µm 波段一致，按 Planck 辐亮度计），读取这些字段的脚本需改名；`radiosity_<波长>` 别名不变。

## 已做的检查（2026-11-16）

- 产品引擎重建通过；`eSceneAnimation` 被拒绝（退出码 2）、`--list-devices` 正常；`histream_lumped_thermal_tests` 9 项通过；核心 eFacetEB 集中参数场景（`test_lumped_scene.py`）6 项通过。
- STREAM IMAGING 动画引擎（核心 + 扩展）构建通过；impact-v1 公式夹具 157/157；`test_animation.py`、`test_mesh.py`、`test_sensor.py` 与统一入口最小网格样例在 GPU 上通过。
- 界面重建（`npm run build`）通过；新服务示例列表为 5 个精选案例，`/api/scene-animation/*` 返回 404，“解决方案”菜单只含三维与解析两组。

## 发布前仍需

- 定版本号，`npm run pack:win` 生成安装包；
- 干净 Windows 环境安装、升级与卸载；HiStream 三维模式和 STREAM 的回归；
- 用 `tools/prepare_user_release.py` 生成交付目录（脚本已写好，尚未实际使用）。
