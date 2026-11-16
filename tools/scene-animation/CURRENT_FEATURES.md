# 当前模拟功能与代码入口

2026-11-12核对。正式源码：`C:/work/streamstudio`。功能注册表为[simulation/capabilities.json](simulation/capabilities.json)，入口为[run_case.py](run_case.py)。表中“已有”指实现存在；各功能的启用情况和收敛状态需要查具体运行记录。

> **定位**：研究模块；开发版（测试中，未发布，不在 2.0.1 安装包内）已接入项目 schema、服务端接口、用户手册第 18 章和一个内置示例，GUI 面板代码也已就位，但界面入口按 2026-11-12 决定暂时隐藏（`SCENE_ANIMATION_UI_ENABLED=false`：菜单与示例列表都不显示，界面不调度也不运行）。引擎命令行与本地 `/api/scene-animation/*` 接口保留，当前请用命令行运行。三种运动后端的性质不同：`authored`/`rigid-v1` 是**作者设定的效果演示，不是物理预测**（`impactModel.kind: "authored"`，视频标题“作者设定，非物理预测”）；`impact-v1` 是**经验模型估算，未经独立验证**（Young 侵彻、Kingery–Bulmash 爆炸荷载、冲量破坏、能量预算；`impactModel.kind: "impact-v1"`，视频标题“经验模型估算，未经独立验证 · 慢放 ×N”）。所有结果 `physicsValidated: false`。

| 类别 | 当前已有功能 | 主要实现与边界 |
|---|---|---|
| 场景 | OBJ网格、材质、实例、树木、道路跑道、地面扩展、多个相机 | `src/animation/scene`、`bridge`；资产与参数显式给出 |
| 动画与相机 | 接近过程、统一时间、外部视角与物体前向相机、接触前有效观测 | `sequence/mesh_timeline.*`；给定轨迹，没有飞行动力学 |
| 接触与破口 | 几何扫掠、Bullet碎片刚体、摩擦/恢复/重力、建筑开洞显示；`impact-v1`经验撞击事件 | `collision/rigid_scene.*`、`impact/`；authored/rigid-v1 的破口、碎片及初始运动预设；impact-v1 由弹体/装药/墙体参数经 Young 侵彻、K-B 荷载和临界冲量判定预设块的失效，碎片速度 (i_r−i_c)A/m、按冲击波到达时刻释放，能量预算驱动火球/热源/烟尘（`event.json`）；没有结构有限元、爆轰 CFD 或燃烧化学；静态碰撞体按部件分区（非盒构件只让自身成为薄壳，`staticCollider=mixed`并写入顶层`warnings`），`rigidRequireSolidStatic`在缓存回放时同样生效；authored 出墙面可用`fragmentExit`设定 |
| 烟火 | 低速热/烟示踪输运、障碍遮挡、尾流、多火源外观、逐渐增长的火势 | GPU `transport_solver.glsl`及火源模块；源强给定，没有燃烧化学 |
| 热物理 | FacetEB温度缓存、局地植被阴影地温、通用道具内部热源—表皮—场景长波双向耦合 | `thermal/`、`facetrt/lumpedthermal.h`与`facetrtcore.cpp`；后者为集中参数导热和表面EB |
| 红外 | 单波长灰体路径、3–5μm积分路径、理想辐亮度、原始ADC | 原生IR与`thermal_imaging.py`需区分；当前MWIR积分在CPU，无完整大气光谱传播 |
| 观测质量 | RGB/IR独立分辨率、曝光、PSF、像元积分、噪声、响应、ADC、撞击后抖动、区域模糊、2%显示增强 | `observation/`与GPU camera shaders；通用未标定相机，2%增强仅影响显示；`blastArrival:"acoustic"`可让抖动从“接触+距离/声速”开始（上界估计，非冲击波计算） |
| RGB | 原生动态材质/光照/阴影/烟尘/火光 | GPU外观近似；小镇热耦合v5的RGB另为原静态参考，不是动态光学重算 |
| 输出与复现 | 连续帧、外部RGB/IR和前向RGB/IR四路合成、视频完整解码、实际输入/引擎/shader哈希、原结果保护 | `compose_four_camera.py`与各包装器；统一入口保存命令/日志/失败状态；所有视频脚本启动前探测编码器（libx264→libopenh264→mpeg4，`--video-encoder`可指定）；完整性检查用`integrity.require`，`python -O`下仍生效 |
| 提速与缓存 | 刚体基础步长轨迹录制/回放、按物理输入哈希复用、PNG编码线程池、分段耗时、运动几何持久化与条带批量提交；S3 实例化几何、S4 多相机、S6 检查点、输运统计异步扫描 | `rigid_scene.cpp`、`mesh_sequence.cpp`、`histream_mesh_adapter.cpp`；回放不创建Bullet世界；缓存写入结果`writeOutcome`在回执前确定；运动几何按三角形数各保留一个适配器、原位重建BLAS/TLAS，输出与旧路径逐字节一致；`instancedGeometry`每个刚体部件一个 BLAS、每样本只更新实例变换；`additionalCameras`多相机共享同一次输运/刚体/几何更新，主相机输出与单相机逐字节一致；`checkpoints`保存输运/传感器AGC/探测器历史，部分帧请求从最近检查点继续而不再渲染全部预热帧，结果逐字节一致；输运统计改为主机缓存内存回读+有序工作线程扫描 |
| 机场全天对照 | 开阔机场显示场景、四组2×2（太阳/载荷）原生全天求解、3–5μm固定范围成像、周期闭合/能量摘要/PPT | `run_case.py`的`airfield-day`流水线一次跑完7步；`case_seal.py`封印输入与GPU几何，`steps_manifest.json`（已封印）保护逐节点数组；启动前按空闲显存自动降低并行数（每组约14.6GB） |

## 统一命令

下面命令均在`C:/work/streamstudio`执行。使用含NumPy/Pillow/Matplotlib的项目Python；只有features和check本身仅依赖标准库。

```powershell
# 查询代码功能，不启动GPU。
python tools/scene-animation/run_case.py features
python tools/scene-animation/run_case.py features --json

# 小样例：先编辑任务中的engine/output；check只读，run显式运行。
python tools/scene-animation/run_case.py check tools/scene-animation/samples/native-scene.job.json
python tools/scene-animation/run_case.py run tools/scene-animation/samples/native-scene.job.json

# 原小镇：准备→成对原生热求解→四视角MWIR与升温/冷却视频。
# 先编辑sourceCase/actors/engine/ffmpeg/output；此运行耗时明显更长。
python tools/scene-animation/run_case.py check tools/scene-animation/samples/coupled-town.job.json
python tools/scene-animation/run_case.py run tools/scene-animation/samples/coupled-town.job.json

# 开阔机场全天：make→prepare→两次GPU几何→四组原生求解+封印→成像/视频→报告。
python tools/scene-animation/run_case.py check tools/scene-animation/samples/airfield-day.job.json
python tools/scene-animation/run_case.py run tools/scene-animation/samples/airfield-day.job.json
```

任务schemaVersion=1。native-scene字段为`pipeline,engine,scene,output`及可选`ffmpeg`；coupled-town字段为`pipeline,engine,sourceCase,actors,ffmpeg,output`；airfield-day字段为`pipeline,engine,townCase,actors,ffmpeg,output`及可选`spinupDays,parallel,perRunVramGb,videoViews,videoEncoder,deck`。未知字段拒绝。相对路径按任务JSON所在目录解析；output必须全新且父目录存在。原生场景的具体物理配置仍由引擎校验；check只检查调度、文件和资产哈希，不代替原生完整配置检查。

机场全天链路已进入`run_case.py`（`airfield-day`）。结束时统一入口校验`case_seal.json`、`render_seal.json`、渲染回执和视频哈希。单步命令仍按[README](README.md)的机场段落可用。渲染前自动校验封印并逐节点核对`steps_manifest.json`；旧案例可先运行`python steps_manifest.py CASE`再重新封印，或用`--allow-unsealed`（会退化为核对native回执和source_inputs），后者不应作为长期做法。

native-scene沿用procedural、mesh和meshSequence三个backend；提供ffmpeg时对完整连续序列包装IR视频，静态mesh或非完整帧范围在check阶段拒绝视频。不编码视频时仍允许原生部分帧运行。动态RGB与四路包装仍使用已有[详细入口](README.md)，没有自动改成一条四相机原生调用。

coupled-town准备器针对当前小镇，包含固定ROI、热网格细分、等效深层和合成气象假定，不是任意场景的通用自动建模器；保留原显示几何。它复用同一通用内部热节点方程，不按道具名称选算法。由于准备器按原路径复制资产，统一入口仅接受复制目标位于新案例内的相对资产路径。

```text
新输出目录/
  job.json          # 原始任务的记录；不是改路径后的独立回放配置
  run.json          # 解析后的绝对路径、输入哈希、命令、步骤状态与错误
  logs/             # 各阶段stdout/stderr
  result/           # 现有工具实际输出及其原生来源记录
  video/            # meshSequence可选包装；其他视频依原工具布局
```

来源核对包含任务文件、声明的场景资产、程序、SPIR-V及本链直接使用的调度工具。原生背景缓存、时间轴和间接依赖继续由原模块/运行清单校验；本入口不声称已枚举全部环境依赖。run.json的completed只表示所列步骤与结果完成，科学收敛以原生诊断为准。

## 已有结果与检查

最新小镇热耦合：`D:/cases/StreamStudio/supplemental/impact_gpu_20261107/mwir_town_coupled_v5_20261109/index.html`。该封存案例76个原生节点收敛，局部地面最大增温约1.43K；20分钟过程压缩为12秒，60秒节点之间做辐亮度插值。这是指定参数下的数值结果，不是现场精度验证。

最新机场全天对照：`D:/cases/StreamStudio/airfield_day_ppt/airfield_day_mwir.pptx`（2026-11-12 只保留 PPT 用到的 9 张图、2 段视频和第 3 页两张表；原 120 GB 案例目录已于 2026-11-15 删除，只保留 1 天 + 有太阳的 2.0 GB 子集：`D:/cases/StreamStudio/场景动画案例/02_中红外_机场全天/data/`）。四组各624个原生节点全部通过收敛验收；四组并行。该案例已补`case_seal.json`（输入、配置、native回执和GPU几何，共2725个文件）；渲染另写`render_seal.json`。表面能量残差最大1.9997 W/m²，距2 W/m²上限仅0.0003，属边界通过，不能据此声称有充分余量。

本轮检查记录：`C:/cases/StreamStudio/simulation_catalog_20261109`。入口检查：

```powershell
cd tools/scene-animation
python tests/test_simulation_entry.py
python -m unittest discover -s tests -p test_case_seal.py
python -m unittest discover -s tests -p test_energy_layout.py
python -m unittest discover -s tests -p test_runway_day.py
python tests/test_integrity_tools.py
# 引擎：混合碰撞体、回放时的requireSolidStatic、缓存写入结果、顶层标注、fragmentExit、声速抖动起点
python tests/test_review_fixes.py ENGINE RIGID_SEQUENCE.json NEW_OUTPUT
# 引擎：impact-v1 回执/能量与冲量预算/热量闭合/装药单调性/冲击波到达抖动；S3/S4/S6 一致性
python tests/test_impact_v1.py ENGINE RIGID_SEQUENCE.json NEW_OUTPUT
# 经验公式夹具（157 项，无 GPU）：cmake --build <build> --target histream_impact_tests 后运行 histream_impact_tests.exe
```

各功能对应的已有检查文件见注册表的checks字段；本次整理不自动运行所有历史GPU检查。2026-11-12 修改了引擎（碰撞体分区、缓存、标注、fragmentExit、抖动起点、运动几何持久化、条带批量提交；随后加入 impact-v1、S3/S4/S6 与输运统计优化），authored/rigid-v1 默认路径对旧引擎输出做了逐字节比对，记录见`review/2026-11-12-review-followup.md`。

## 仍未实现

完整结构应力/裂纹预测（impact-v1 只判定预设块是否失效）、可压缩冲击波/爆轰 CFD（冲击波量来自经验拟合）、燃烧化学/蔓延、壳体破片、地冲击与弹坑、钢筋、三维固体导热与空气流场双向反馈、经过标定的大气光谱与相机测量链；S7 局部子步。impact-v1 的经验公式已对公开表格和算例做了核对，但整条撞击链没有与实验或高保真计算做独立验证。原生热核/波段/观测的[全面归并计划](../../docs/superpowers/plans/2026-11-09-simulation-code-consolidation.md)仍是独立计划，本次统一调度不等于该计划已完成。
