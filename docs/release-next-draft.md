# 下一版本更新说明（草稿，测试中）

状态：开发版测试阶段，**未发布**。版本号仍为 2.0.1，没有构建正式安装包，也没有创建新的交付目录。本文件记录待发布的变更，发布时再定版本号并改写为正式发布说明。

## 变更内容

**场景动画剥离到 STREAM IMAGING（2026-11-16）。** 场景动画研究功能（HiStream `eSceneAnimation`：撞击 impact-v1、火球/烟尘输运、刚体碎片、相机观测、耦合小镇与机场全天等）不再属于 Stream Studio，整体迁到 `C:/work/streamimaging`：

- 引擎代码（`models/histream/src/animation`、`shader/animation`、`cmake/scene-animation.cmake`、C++ 夹具）改为 STREAM IMAGING 的 HiStream 外挂扩展 `engine/scene-animation`，由其 `scripts/build-animation-engine.ps1` 编进本仓库的 HiStream 核心，输出到 STREAM IMAGING 自己的构建目录，不覆盖 `models/bin_x64`。
- 工具链 `tools/scene-animation`、示例 `assets/examples/impact_v1_building`、开发文档与研究计划、用户手册第 18 章与理论手册第 17 章随之迁出。
- 已隐藏的桌面面板、`/api/scene-animation/*` 服务、schema 中的场景动画导出、示例目录的 `eSceneAnimation` 条目、相关翻译和桌面设置键均已删除；代码存档在 STREAM IMAGING 的 `animation/studio-panel/`。

**HiStream 通用扩展接口。** 核心不再知道任何具体扩展：`src/base/extension.h` 提供模式注册表，CMake 选项 `HISTREAM_EXTENSION_DIRS` 列出外部扩展目录（各含 `histream-extension.cmake`），配置时生成注册函数。产品构建不带扩展，`histream.exe eSceneAnimation …` 返回用法提示（退出码 2）。`HISTREAM_ENABLE_SCENE_ANIMATION` 选项已删除。

**之前的草稿内容**（场景动画面板、impact-v1、渲染提速选项、内置示例）随功能迁出，原文存档在 STREAM IMAGING `docs/scene-animation/history/streamstudio-release-draft-20261112.md`。

## 兼容性

- `tools/build-engine.ps1` 构建的产品引擎只含核心（显式清空 `HISTREAM_EXTENSION_DIRS`），不再含场景动画与 Bullet 碰撞模块，也不再需要本地 Bullet 源码；三维 RT/EB 与 STREAM 遥感工作流的代码路径不变。
- 耦合小镇和机场全天所用的 FacetEB 集中参数热节点（`facetrt/lumpedthermal.h` 等）属于核心，保留；其解析夹具从 `tools/scene-animation/tests/lumped_thermal_fixture.cpp` 移为 `tools/test-facet-lumped-thermal.cpp`（目标 `histream_lumped_thermal_tests`）。
- 三维工程 `schemaVersion = 6`、遥感工程 `schemaVersion = 2` 不变。STREAM IMAGING 运行时仍从本仓库导入 `project-schema.js` 和 `tools/runtime-scene.mjs`。

## 已做的检查（2026-11-16）

- 产品引擎重建通过；`eSceneAnimation` 被拒绝（退出码 2）、`--list-devices` 正常；`histream_lumped_thermal_tests` 9 项通过；核心 eFacetEB 集中参数场景（`test_lumped_scene.py`）6 项通过。
- STREAM IMAGING 动画引擎（核心 + 扩展）构建通过；impact-v1 公式夹具 157/157；`test_animation.py`、`test_mesh.py`、`test_sensor.py` 与统一入口最小网格样例在 GPU 上通过。
- 界面重建（`npm run build`）通过；新服务示例列表为 5 个精选案例，`/api/scene-animation/*` 返回 404，“解决方案”菜单只含三维与解析两组。

## 发布前仍需

- 定版本号，`npm run pack:win` 生成安装包；
- 干净 Windows 环境安装、升级与卸载；HiStream 三维模式和 STREAM 的回归；
- 用 `tools/prepare_user_release.py` 生成交付目录（脚本已写好，尚未实际使用）。
