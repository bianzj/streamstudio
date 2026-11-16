# 现有模拟技术归并实施计划

> 执行方式：使用executing-plans按依赖顺序在当前任务实施；另有明确授权时才使用代理。
> 状态：尚未实施。本轮交付设计与计划。任务之间不自动提交、推送、打包发布或清理旧代码。

**Goal:** 将当前游戏道具热模型、波段与观测方法接入已有HiStream模块，案例只保留输入和预设。

**Architecture:** 保留`eSceneAnimation`和现有可选模块。轻量热状态CPU更新，辐射与相机优先复用GPU链；
Python仅做准备、独立参考和包装。按可单独验收的增量实施。

**Tech Stack:** 现有C++17、CMake、Vulkan／GLSL、nlohmann JSON、Python／NumPy／Pillow及显式FFmpeg。

**Spec:** [代码归并设计](../specs/2026-11-09-simulation-code-consolidation.md)。

## Global Constraints

- 正式仓库：`C:/work/streamstudio`；所有下列相对路径均以此为根。
- 维持`HISTREAM_ENABLE_SCENE_ANIMATION`默认OFF；保留原公开函数与旧配置路径。
- 原案例、原清单、原相机与原实例不覆盖；检查输出使用`C:/cases/StreamStudio/simulation_consolidation_<run-id>`的新目录。
- 轻量检查沿用`tools/scene-animation/tests`；未运行的检查必须标为未运行。
- 不从历史案例目录导入生产代码；不新增引擎副本、仓库或必需的外部服务。
- 原生模块或shader变化后构建`histream`与`histream_runtime_resources`；不附带全量科学回归。

## Review Focus

1. 多相机及视图重排不得多次推进热状态；任务4用视图排列与子帧测试覆盖。
2. 缓存／实例ID／资产哈希不匹配必须在输出前报错；任务1与4覆盖。
3. 相机固定图样与时间噪声不得因分段或重排改变；任务5做分段与全程对比。
4. 显示P2—P98不得改变DN和原始辐亮度；任务5覆盖原始产品哈希。
5. 暖机、时间压缩和缺失观测不能静默错位；任务4与6验证完整timeline与有效性标记。

## 现有检查入口

以下命令的参数形式已从脚本核对，本轮未执行：

```powershell
python tools/scene-animation/tests/test_mesh.py <engine> <new-output>
python tools/scene-animation/tests/test_sensor.py <engine> <new-output>
python tools/scene-animation/tests/test_camera_observation.py <engine> <sequence.json> <new-output>
```

以下任务列出的新测试文件是待编写检查，不应当作已经存在或已经通过。
使用项目可用Python解释器；本机已核验的运行时为
`C:/Users/jiank/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`。

### Task 1: 固定参考数据与消除活动代码的历史路径依赖

**Files:**
- Create: `tools/scene-animation/reference/actor_thermal_reference.py`
- Create: `tools/scene-animation/reference/mwir_reference.py`
- Create: `tools/scene-animation/import_legacy_case.py`
- Create: `tools/scene-animation/tests/test_case_import.py`
- Update documentation: `tools/scene-animation/README.md`

**Interfaces:**
- `import_case(source_scene: Path, actor_config: Path, destination: Path) -> Path`：返回新案例配置路径；不替换已有实例或相机。
- 配置首轮使用`kind="histream-animation-case"`、`schemaVersion=1`；嵌入／引用原scene、actors、clock、radiometry、observation和outputs六项。
- 只转换已知字段。未知schema、无对应实例、资产哈希不匹配和已存在目标目录均拒绝。

- [ ] 先写转换测试：原场景11资产／20实例／4相机逐项一致；旧资产引用转为显式路径；未知schema、缺资产、重复ID、已存在输出报错。
- [ ] 运行`python tools/scene-animation/tests/test_case_import.py`确认新功能缺失的失败，再实现转换。
- [ ] 从v4复制精确热数值参考，从v2／v3／v4相同版本复制MWIR参考，保留原SHA和出处；参考中不导入运行脚本，不调用引擎。
- [ ] 生产配置不再依赖`sys.path`导入历史案例源码；支持资产外部引用和显式复制，两者均登记哈希。
- [ ] 重跑该项检查；旧案例与代码保持原样。转换结果供任务4使用，不更新当前案例指针。

### Task 2: 原生通用道具热核

**Files:**
- Create: `models/histream/src/animation/thermal/actor_thermal.h/.cpp`
- Create: `tools/scene-animation/tests/actor_thermal_fixture.cpp`
- Create: `tools/scene-animation/tests/test_actor_thermal.py`
- Modify: `models/histream/cmake/scene-animation.cmake`

**Interfaces:**
- 使用设计文档中的`ActorHeatParameters`、`ActorHeatState`、`ActorHeatStep`及`advanceActorHeat(...)`。
- 新建`histream_actor_thermal_tests`为EXCLUDE_FROM_ALL的原生检查目标。
- fixture命令为`histream_actor_thermal_tests <input.json> <new-output.json>`；写出每步`deltaK`与`residualW`。
- Python检查入口为`test_actor_thermal.py <fixture-exe> <new-output-directory>`。

- [ ] 先编写零功率、线性稳态50K、独立隐式冷却解析解、非线性收支与无效参数测试；验证新接口不存在时失败。
- [ ] 移植现有公式与隐式二分，不更改热损失定义；参数必须有限且容量为正，`dt>0`、`activity∈[0,1]`。
- [ ] 加入新目标与源文件，构建`histream_actor_thermal_tests`。
- [ ] 与Python参考在相同输入／步序比较：温升绝对差≤1e-7 K；非线性参考样例单步残差≤1e-4 W。
- [ ] 运行`python tools/scene-animation/tests/test_actor_thermal.py <fixture-exe> <new-output-directory>`，记录实际检查和浮点精度。

### Task 3: 原生3–5 μm波段与产品单位

**Files:**
- Create: `models/histream/src/animation/radiometry/spectral_band.h/.cpp`
- Create: `models/histream/shader/animation/radiometry/band.glsl`
- Modify: `models/histream/shader/animation/mesh/render_common.glsl`
- Modify: `models/histream/src/animation/render/mesh_renderer.cpp`
- Modify: `models/histream/cmake/scene-animation.cmake`
- Create: `tools/scene-animation/tests/test_band_radiance.py`

**Interfaces:**
- `SpectralBand`包含`wavelengthsUm`和`integrationWeightsUm`；首轮3…5 μm等间距17点Simpson。
- 保留单波长路径；新增band字段互斥选择模式，积分产品记录单位`W m-2 sr-1`。
- 新Python检查入口：`test_band_radiance.py <engine> <new-output-directory>`，自行生成小平面／统一材质夹具。

- [ ] 先写280／300／380／600K、ε=0／0.5／1、天空反射、无效波段及17点权重测试。
- [ ] 实现配置权重与GPU统一Planck计算；沿用当前常量，暂不加入太阳或大气项。
- [ ] 输出明确的理想波段float产品；不从8bit图反推辐亮度。
- [ ] 构建`histream histream_runtime_resources`，执行新检查；均匀夹具GPU相对误差≤1e-4，暗信号绝对容差1e-8 W·m⁻²·sr⁻¹。
- [ ] 原单波长夹具及无相机的mesh检查通过；如果采样定义不同，拆开中心射线与AA产品比较。

### Task 4: 道具绑定、统一时间与当前场景接入

**Files:**
- Create: `models/histream/src/animation/sequence/frame_time.h`
- Create: `models/histream/src/animation/thermal/actor_binding.h/.cpp`
- Modify: `models/histream/src/animation/render/mesh_renderer.cpp`
- Modify: `models/histream/src/animation/render/mesh_sequence.cpp`
- Modify: `models/histream/src/animation/scene/mesh_scene.h/.cpp`
- Modify: `models/histream/src/animation/scene_animation.cpp`
- Create: `tools/scene-animation/tests/test_actor_scene.py`
- Modify: `models/histream/cmake/scene-animation.cmake`

**Interfaces:**
- `FrameTime{frame,simulationSeconds,presentationSeconds}`；clock控制视频与模拟时间映射。
- `PreparedActorHeat{instanceIndex,parameters,triangleWeights}`；其中parameters为任务2的`ActorHeatParameters`，triangleWeights为按该实例三角形顺序排列的`std::vector<double>`。
- `prepareActorHeat(const MeshScene&, const nlohmann::json& actor) -> PreparedActorHeat`负责局部权重、面积尺度、有效面积和非线性散热系数预计算，不推进状态。
- 活动道具仅引用已有instance ID和显式热参数／局部热权重；几何与权重预处理后缓存。
- `timeline.csv`写出frame、simulation_seconds、presentation_seconds和validity。
- 新检查入口：`test_actor_scene.py <engine> <imported-case.json> <new-output-directory>`。

- [ ] 先写原布局保护、多相机重排、第二相机不改变温度、背景像素隔离、ID错配和非法时间映射测试。
- [ ] 将任务1配置接入现有入口；旧backend与配置保持原路径。
- [ ] 实现prepareActorHeat，移植当前spatial_weights、effective_area和from_mesh的有效输入行为；角色权重来自显式配置，避免在运行时猜测材质名称。
- [ ] 绑定热状态和权重的独立GPU缓冲区，不改变旧128字节Triangle ABI；一次推进状态，多视角共享。
- [ ] 暖机与部分帧从起点恢复确定状态；曝光时刻按模拟时间取样，禁止为每个视角额外推进状态。
- [ ] 构建两个原生目标；在新目录执行检查。逐项比较11个原资产、20个原实例与4个原相机。
- [ ] 在原场景输出0／600／1200模拟秒的热状态；与参考状态按1e-7 K比较，渲染浮点产品按任务3容差比较。

### Task 5: 统一新观测链并显式区分显示预设

**Files:**
- Modify: `models/histream/src/animation/observation/camera_observation.h/.cpp`
- Modify: `models/histream/shader/animation/camera/detector.comp`
- Modify: `models/histream/shader/animation/camera/display.comp`
- Modify: `models/histream/src/animation/render/mesh_renderer.cpp`
- Create: `tools/scene-animation/presets/camera/clear_rgb_generic_ir.json`
- Create: `tools/scene-animation/tests/test_band_camera.py`
- Modify: `models/histream/cmake/scene-animation.cmake`

**Interfaces:**
- 波段IR的输入与噪声单位显式采用积分辐亮度；旧Kelvin等效单波长参数继续作为兼容配置。
- 新预设IR为320×180、12bit；RGB额外退化关闭；显示百分位明确为[2,98]。
- 新检查入口：`test_band_camera.py <engine> <imported-case.json> <new-output-directory>`。

- [ ] 先写DN范围0…4095、PSF常数场守恒、只换显示预设不改变DN、固定图样跨帧一致和分段重放一致测试。
- [ ] 使新mesh热场走一次CameraObservation；不再调用案例observe或串联SceneSensor。
- [ ] 按稳定相机ID建立历史与随机流，区分FPN和逐帧读出噪声。
- [ ] 构建两个目标，运行新检查与原`test_camera_observation.py`相关夹具；保留旧链单独兼容。
- [ ] 将区域显示偏移和显示四级量化登记为显示效果；配置要求未实现物理项时明确报错。

### Task 6: 单一案例运行与输出收尾

**Files:**
- Create: `tools/scene-animation/run_case.py`
- Modify: `tools/scene-animation/render_video.py`
- Modify: `tools/scene-animation/compose_four_camera.py`
- Create: `tools/scene-animation/tests/test_case_outputs.py`
- Modify: `tools/scene-animation/README.md`

**Interfaces:**
- `run_case.py <case.json> <new-output-directory> --engine <engine> [--ffmpeg <ffmpeg>]`；不指定视频输出时无需FFmpeg。
- 生产运行只调用正式引擎，不导入reference；保存实际引擎、shader、输入和资产哈希。
- 每项产品按请求单独落盘；缺失、失败或未请求分别登记。
- 新检查入口：`test_case_outputs.py <engine> <imported-case.json> <new-output-directory>`。

- [ ] 先写已有目标拒绝、检查失败无半成品、按产品开关存储、帧时间和无效观测标记测试。
- [ ] 将固定Windows字体与FFmpeg路径变为包装参数；热／辐射计算不依赖字体或编码器。
- [ ] 在新目录生成当前案例对照，逐项审核原始浮点、DN、PNG和时间表，最后完整解码所请求视频。
- [ ] 运行本轮涉及的轻量检查与有／无animation开关的相关构建；记录通过／失败／未运行。
- [ ] 交付新案例与迁移记录。旧案例和当前导航指针保持原样，用户选定新版本后再登记为当前。

## 后续独立工作

效果shader变体与大量make_*脚本在本轮归并稳定后再收拢：先建立确定的视觉参考，
将相同算法的差异改为预设，再减少重复路径。该项涉及火／烟外观回归，不能用热核单元检查替代。
大气、背景遮阴热反馈、标定响应与更复杂热网络是新增模型任务，各自需要输入与验证目标。

## 本轮设计交付的实际验证

- 核对了正式源码、当前v4与v1—v3方法文件、构建开关及现有检查参数。
- 新文档、JSON来源路径和引用做静态检查；原模块和当前案例代码哈希应保持原值。
- 未运行上述新测试、构建、仿真或视频生成；它们属于后续实施任务。
