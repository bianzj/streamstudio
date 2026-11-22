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

### 精度修正（2026-11-22，改变数值）

- **反射率不再截断到 1**：RayTracing、VoxelRT、VoxelEB 成像，FacetRT 的 GPU/CPU 光谱输出和 FacetEB 状态输出只剔除 NaN/Inf 与负值。朝阳坡面、热点、白墙、水面镜面等 BRF > 1 的像元不再被压低；星下点常规场景数值不变。显示拉伸不受影响。
- **RayTracing 散射深度**：新增独立参数 `control.raytracingDepth`（默认 16，范围 1–64），不再与 VoxelRT 的步数参数 `depth` 共用；第 3 次散射起按俄罗斯轮盘赌无偏终止（存活概率 = clamp(最大通量, 0.05, 1)）；周期边界穿越不再占用散射次数。原先实际截断在 4 阶，植被 842/860 nm 均值升高约 0.4%–1.5%，p99 升高 1.4%–5.7%，可见光基本不变。
- **RayTracing 阴影射线**：首次命中即终止，起点偏移由 11 mm 降为 1 mm，近距离遮挡不再漏光。
- **VoxelEB 地面自由对流**：土壤、水体、建筑表面温度高于气温时，并联加入 Kondo–Ishida/TSEB 自由对流导度 ≈ 0.0025·ΔT^(1/3) m/s。forest/urban 示例正午土壤显热由 7–14 W/m² 升到 30–66 W/m²，G/Rn 由 0.87–0.92 降到 0.76–0.84。完整 Monin–Obukhov 稳定度迭代仍未实现。
- **FacetEB 收敛判据更严**：在“残差 ≥ 2 W/m² 的面元 < 0.1%”之外，要求任一面元侧的温度等效残差 |R|/(dR/dT) ≤ 0.5 K。此前作物示例有节点在最大残差 43.8 W/m² 时被判为收敛，现在如实标为未收敛。
- **天空盒关闭时的场景边缘**（RayTracing、VoxelRT、VoxelEB）：射出场景的样本不再被整体丢弃或按 0 计入，像元值按“场景覆盖率”加权平均；完全无覆盖的像元仍由主机端填天空值。街景透视示例内部像元不变，500–630 个边缘像元变亮（VoxelRT 边缘 860 nm 均值 0.053→0.246）。
- **Facet 周期边界下的可见性分辨率**：投影窗口 x/y 收紧到中心块外接球（深度方向仍覆盖全部副本），GPU/CPU 后端一致。周期 8 的作物示例辐射度偏差由 +16% 降到约 +2%，反射率 > 1 的伪像元由 36 个降到 0；代价是可见性边数增加约 5 倍，周期 20 的城市 FacetEB 单节点 8.7 s → 11.8 s。
- **PROSPECT 指数积分 E1**：原 9.9 万步矩形积分丢掉了尾项（x = 0.01 时误差约 5%），改为级数加连分式（相对误差约 1e-15）。叶片近红外反射率变化约 1e-4。

### 性能（2026-11-22，不改物理）

以下改动经冻结算例对照，结果逐位相同或差异 ≤ 1e-4（VoxelEB 温度阻尼为 ≤ 0.05 K）。同一台机器上各模式总耗时降到原来的 1/1.5–1/7。

- 五种模式启动时都调用的 PROSPECT 准备由约 10 s 降到毫秒级（E1 改写、系数按引用传递）。
- VoxelEB：未碰撞天空场只追踪一次、各光谱组按天空辐照度缩放（短波散射约 1.8×）；温度阻尼随迭代递减（迭代 29–35 → 16–20 次）；气动阻抗移出迭代循环。辐射缓冲区因此增加约 50%。
- Facet：耦合迭代中长波 Jacobi 从上一轮解热启动；FacetEB“去直射”求解只在末节点做；短波窗口与影像波段按残差提前收敛；窗口光学参数按材质预计算并行化；A-buffer 溢出时直接扩到所需层数。
- VoxelRT：太阳方向不变时多视角/航点只重跑成像（61 视角 14 s → 4.5 s）。
- RayTracing：批次按实测 GPU 耗时自适应切分（约 120 ms/批），消除开周期边界时的低占用和整图一次提交的 TDR 风险。
- 服务端：FacetEB 逐节点出图改为每次运行一个常驻出图线程（超过 300 万面元时仍逐节点开线程，阈值 `STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS`）。

### 新工程默认值（2026-11-22）

- VoxelRT 辐射求解方法默认“光谱批处理加速”（与传统方法均值相同，快 1.1–1.6×）。
- FacetEB 温度松弛系数默认 0.8（原 0.5；作物/城市收敛所需迭代 16–19 → 9–12）。
- 已有工程中显式保存的 `radiationSolver=traditional`、`temperatureRelaxation=0.5` 保持不变。

### 界面（2026-11-22）

- **只显示实际生效的参数**：FacetRT/FacetEB 不再显示追踪深度与采样数；RayTracing 显示“最大散射次数”和“每像元路径数”；VoxelRT 的“追踪深度”改名“体元射线步数预算（×128）”；VoxelEB 新增“体元射线步数上限”（`radiationMaxSteps`）与“短波散射阶数”，隐藏引擎不读的耦合迭代/阈值/松弛；FacetEB 隐藏只记录不参与判据的温度收敛阈值；采样数上限 256 → 1024，与引擎一致。
- **运行前预检**：点击运行时先调用 `POST /api/run/preflight`，日志写出面元数/上限、可见性栅格、体元网格与辐射场显存估计、节点/方向/波段数、按本工程最近 3 次同模式运行估计的耗时和单次运行上限；预计超时、面元接近上限、显存过大等风险弹窗确认。预检失败不阻止运行。
- **运行质量摘要**：解析 `FACET_CONVERGENCE`、`FACET_TIMING`、`ENERGY_BALANCE`、`SHORTWAVE_SCATTERING`（含 `truncations=`），运行结束汇总节点数、未收敛节点、最多迭代、最大温度等效残差、耗时构成与步数截断，有问题时给出调参建议。
- **进度与结果**：进度栏显示当前阶段与按引擎进度估算的剩余时间；运行成功（或 FacetEB 未收敛但结果保留）后自动打开结果窗口；stdout 按行重新拼接，不再丢失被拆开的 PROGRESS 行。
- **体元预览**：按钮此前无效，现在按当前体元大小近似体元化场景对象，显示网格尺寸与占用体元数（体元过多时粗化显示，计数为上限估计）。
- **运行期间视口降帧**：引擎运行时三维视口约 10 fps 渲染，减少与引擎争用 GPU；在视口内拖动、缩放或按键后 1.5 s 内恢复全帧率。
- **FacetEB 自动后端**：设备不支持 shaderFloat64（或缺少 `energy.comp.spv`）时，“自动”不再报错，改为辐射仍用 GPU、能量平衡走 CPU 路径，并在日志中提示；显式选择 GPU 时仍报错并提示改选 CPU 或自动。

### 运行管线（2026-11-22，不改数值）

- **桌面版服务端移出主进程**：`desktop/main.mjs` 用 Electron `utilityProcess.fork` 启动 `desktop/server-process.mjs`，HTTP 服务（含 FacetEB 出图线程）在独立进程中运行，主进程只负责窗口、文件对话框（经进程消息转发）和桌面设置 IPC。运行准备、出图、ENVI 转换、读结果不再冻结窗口。退出时先通知服务进程停止引擎再退出，5 s 未退出则结束整棵进程树；服务进程意外退出时清理它遗留的引擎进程，自动重启并重新载入页面，1 分钟内第 3 次退出则报错关闭。网页模式不变。
- **结果读取缓存**：`/api/results/read` 按 (路径, mtime, size) 缓存原始数据、已解码 TIFF 分块和已编码响应，按字节计的 LRU，默认 512 MB（`STREAM_STUDIO_RESULT_CACHE_MB`，0 为关闭）；响应头 `X-Cache: hit|miss`。彩色合成三个波段共用一次读盘/解码；变化分析和 STREAM 栅格读取也共用该缓存。文件被覆盖即失效，“删除所有结果”清空缓存。
- **三维结果二进制传输**：请求头 `Accept: application/vnd.stream-studio.result-v1` 时，面元/体元结果的几何坐标和逐面元/体元指标以 Float32（不能无损表示时用 Float64）二进制块回传，其余字段仍为 JSON；界面已适配。不带该请求头时仍返回原 JSON。2 万面元测试结果 3.66 MB → 1.92 MB，逐值一致。
- **运行准备不阻塞事件循环**：引擎 exe、工程与气象文件的 SHA-256 改为流式计算并按 (路径, mtime, size) 缓存；运行输入目录（含裁切后的 OBJ）与气象快照改为异步复制；准备期间拒绝第二个运行请求。

## 兼容性

- `tools/build-engine.ps1` 构建的产品引擎只含核心（显式清空 `HISTREAM_EXTENSION_DIRS`），不再含场景动画与 Bullet 碰撞模块，也不再需要本地 Bullet 源码；三维 RT/EB 与 STREAM 遥感工作流的代码路径不变。
- 耦合小镇和机场全天所用的 FacetEB 集中参数热节点（`facetrt/lumpedthermal.h` 等）属于核心，保留；其解析夹具从 `tools/scene-animation/tests/lumped_thermal_fixture.cpp` 移为 `tools/test-facet-lumped-thermal.cpp`（目标 `histream_lumped_thermal_tests`）。
- 三维工程 `schemaVersion = 6`、遥感工程 `schemaVersion = 2` 不变。STREAM IMAGING 运行时仍从本仓库导入 `project-schema.js` 和 `tools/runtime-scene.mjs`。
- HiStream 退出码：64 为命令行用法错误（含不带工程路径运行，原先静默跑内置示例；内置示例须显式 `--builtin-example`），2 只表示 FacetEB 计算结束但有未收敛节点；1/3/4 为运行、资源创建、初始化失败。按旧退出码 2 判断用法错误的脚本需修改。
- `voxelrt.json` 过程字段：2.5–5 µm 波段由 `illumination_<波长>` 改名为 `spectral_radiance_<波长>`（与 > 5 µm 波段一致，按 Planck 辐亮度计），读取这些字段的脚本需改名；`radiosity_<波长>` 别名不变。
- 2026-11-22 精度修正后，旧工程重新运行会有以下数值变化：VoxelEB 地面显热升高；RayTracing 近红外升高；反射率可大于 1；天空背景下物体边缘变亮；开周期边界的 Facet 工程更准但更慢。FacetEB 判据更严，已保存的低迭代上限（如 12 次）可能被报告为未收敛（退出码 2，结果保留）。
- 工程新增 `control.raytracingDepth`（RayTracing 散射次数），旧工程缺省按 16 处理；`control.depth` 只对 VoxelRT 有意义（步数预算 ×128）。
- 新增环境变量：`HISTREAM_FACET_RASTER_SIZE`（Facet 可见性栅格边长，用于生成高分辨率参考解）、`STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS`（常驻出图线程的面元数阈值，默认 3,000,000）、`STREAM_STUDIO_RESULT_CACHE_MB`（结果读取缓存上限，默认 512）、`STREAM_STUDIO_RESULT_CACHE_DEBUG=1`（服务端打印缓存命中日志）。
- 回归测试工具 `tools/regression/`：从内置示例生成 13 个冻结算例，用两版引擎运行并对比；`compare.py --tolerance` 可作为“纯性能改动不得改变结果”的门禁。

## 已做的检查（2026-11-22）

- 每项性能/精度改动单独编译引擎，在覆盖五种模式的冻结算例上对照；第二轮用 `tools/regression/` 13 个算例对比第一轮引擎：结果变化只出现在周期边界 Facet 算例与街景边缘像元，其余不变或 ≤ 1e-4。
- `tools/test-facet-energy-convergence.cpp` 补充 0.5 K 上限用例并通过。
- Electron 中端到端：五种模式参数列表、体元预览、运行、自动打开结果、运行质量摘要与预检弹窗。
- 未做 SCOPE 对比；VoxelEB 自由对流与 RayTracing 深度会改变既有 SCOPE 指标，发布前如需对比应单独运行。
- 运行管线（服务端独立进程、结果缓存、二进制传输）：网页模式接口检查（示例列表、打开示例、RayTracing 运行、同一 TIFF 第二次读取 `X-Cache: hit`、touch 后失效）；二进制与 JSON 逐值一致；桌面模式 CDP 端到端（打开 urban_block → RayTracing → 结果自动打开），进程树确认服务在 utility 进程且 histream 为其子进程；运行中强杀服务进程后自动重启、页面重新载入、无遗留 histream；运行中关闭窗口后无遗留进程；asar 内 utilityProcess + Worker 可用。

## 已做的检查（2026-11-16）

- 产品引擎重建通过；`eSceneAnimation` 被拒绝（退出码 2）、`--list-devices` 正常；`histream_lumped_thermal_tests` 9 项通过；核心 eFacetEB 集中参数场景（`test_lumped_scene.py`）6 项通过。
- STREAM IMAGING 动画引擎（核心 + 扩展）构建通过；impact-v1 公式夹具 157/157；`test_animation.py`、`test_mesh.py`、`test_sensor.py` 与统一入口最小网格样例在 GPU 上通过。
- 界面重建（`npm run build`）通过；新服务示例列表为 5 个精选案例，`/api/scene-animation/*` 返回 404，“解决方案”菜单只含三维与解析两组。

## 发布前仍需

- 定版本号，`npm run pack:win` 生成安装包；
- 干净 Windows 环境安装、升级与卸载；HiStream 三维模式和 STREAM 的回归；
- 用 `tools/prepare_user_release.py` 生成交付目录（脚本已写好，尚未实际使用）。
