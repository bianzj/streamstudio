# 下一版本更新说明（草稿，测试中）

状态：开发版测试阶段，**未发布**。版本号仍为 2.0.1，没有构建正式安装包，也没有创建新的交付目录。本文件记录待发布的变更，发布时再定版本号并改写为正式发布说明。

## 变更内容

**场景动画（研究功能）：引擎、服务端与面板代码已就位，界面入口暂时隐藏。** 面板、服务端接口、schema 与手册已实现，但按 2026-11-12 决定用 `SCENE_ANIMATION_UI_ENABLED=false` 隐藏界面入口（尚无参数表单，参数只能写 JSON），界面不调度也不运行；引擎命令行 `histream.exe eSceneAnimation` 与本地 `/api/scene-animation/*` 保留。恢复后菜单“解决方案 → 研究功能 → 场景动画（研究功能）…”打开面板：选择场景动画输入 JSON，运行 HiStream `eSceneAnimation`，查看红外、可见光、相机观测与附加相机的逐帧结果；任务与三维模拟共用 HiStream 引擎槽。使用说明见用户手册第 18 章，方法见理论手册第 17 章。

**impact-v1 经验撞击事件模型。** `sequence.motionBackend: "impact-v1"` 由弹体、装药和墙体参数计算：真实速度飞行与恢复温度、Young 侵彻与引信起爆点、Kingery–Bulmash 冲击波荷载、预设碎块按临界冲量失效并按冲击波到达时刻释放为刚体碎片、能量预算驱动火球/热量/烟尘、相机抖动从冲击波到达开始。物理按真实时间计算，视频按 `presentationTimeScale` 慢放并在画面标注。每次运行写出 `event.json`。结果标注“经验模型估算，未经独立验证”（`physicsValidated: false`）；`authored`/`rigid-v1` 仍标注“作者设定，非物理预测”。

**渲染提速选项。** `instancedGeometry`（每个刚体部件一个加速结构，impact-v1 默认开启）、`additionalCameras`（多相机共享一次输运与碎片计算）、`checkpoints`（部分帧请求从检查点继续，不再渲染全部预热帧）；输运统计回读改为主机缓存内存和有序工作线程。

**内置示例** `impact_v1_building`：建筑撞击，通用示例参数。已登记在 `catalog.json`，但界面入口隐藏期间不会出现在“打开示例”列表中，可用命令行运行（是否随正式版提供待定）。

## 兼容性

- `tools/build-engine.ps1` 构建的产品引擎现在包含场景动画与刚体碰撞模块（`HISTREAM_ENABLE_SCENE_ANIMATION`、`HISTREAM_ENABLE_ANIMATION_COLLISION`，使用本地 Bullet 3.25 源码）；三维 RT/EB 与 STREAM 遥感工作流的代码路径不变。`electron-builder.yml` 排除了 `histream_*_tests.exe`。
- 场景动画 `authored`/`rigid-v1` 的默认输出与上一版引擎逐字节一致（帧、CSV、辐亮度文件），运行清单增加了标注字段；原有 7 个渲染/输运着色器的 SPIR-V 与上一版相同。
- 三维工程 `schemaVersion = 6`、遥感工程 `schemaVersion = 2` 不变。

## 已做的检查

- impact-v1 经验公式夹具 157/157；引擎测试 `test_impact_v1.py`（能量与冲量预算、热量闭合、装药单调性、冲击波到达抖动、S3/S4/S6 一致性）通过；`test_review_fixes.py`、`test_body_camera.py` 通过；旧/新引擎三组缩减配置逐字节一致；GUI 接口端到端（列出、打开、检查、运行、读取结果）在开发引擎上通过。记录见 [review/2026-11-12-review-followup.md](../review/2026-11-12-review-followup.md)。
- impact-v1 只证明公式实现与公开表格一致，整条撞击链没有与实验或高保真计算做独立验证；不得用于毁伤、武器效应或安全评估。

## 发布前仍需

- 定版本号，`npm run pack:win` 生成安装包（2026-11-12 的一次尝试中引擎构建成功，electron-builder 因下载超时失败）；
- 在打包后的引擎上运行一次场景动画示例，确认安装目录下的 `shader/animation` 完整；
- 干净 Windows 环境安装、升级与卸载；HiStream 三维模式和 STREAM 的回归；
- 用 `tools/prepare_user_release.py` 生成交付目录（脚本已写好，尚未实际使用）。
