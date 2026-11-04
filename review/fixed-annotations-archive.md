# 代码内 FIXED 审阅注释归档（2026-11-04）

2026-10-28 起的几轮代码审阅把修复说明以 `FIXED[级别] …` 单行注释写在源码里。为保持源码整洁，这些注释已从代码中移除，原文按文件归档于此；行号为移除前的位置。尚未处理的问题仍以 `DEFERRED[…]` 注释保留在代码中。整体修复记录见 [2026-10-28-fix-log.md](2026-10-28-fix-log.md)。

共 227 条，涉及 48 个文件。

## desktop/main.mjs

- L71: FIXED[低] 错误处理: 单次 fetch 没有超时，若连接建立后服务端迟迟不响应，循环无法回到 deadline 判断，启动会无限挂起；已修复：fetch 传入 signal: AbortSignal.timeout(剩余 deadline)，超时后立即回到 deadline 判断（不新增超时数值）
- L168: FIXED[低] 安全: 页面中任何 http(s) 链接（包括结果或工程内容渲染出来的链接）都会不经确认直接交给系统浏览器打开，openExternal 返回的 Promise 也没有 catch；另外没有设置 session 的 setPermissionRequestHandler。已修复：两处 shell.openExternal 都补上 .catch() 并记录错误；白名单域名/确认弹窗与权限请求处理器属产品决策，DEFERRED

## desktop/settings.mjs

- L14: FIXED[中] 错误处理: 设置文件损坏（手动编辑、磁盘错误、写入被截断）时 JSON.parse 或下方格式检查抛错，createWindow 失败导致应用每次启动都直接退出，用户无法自救；已修复：读取/解析/格式校验失败时把文件改名为 .corrupt 备份并以空设置继续启动

## dev.mjs

- L7: FIXED[低] 错误处理: 两个子进程都未监听 'error'，且只处理 vite 的退出；API 后端崩溃后 Vite 继续运行，前端只看到代理错误而无提示；已修复：bridge 监听 'exit'/'error' 时打印原因并一起退出，vite 也补 'error' 监听

## electron-builder.yml

- L45: FIXED[中] 安全: 整个 docs 目录被打进安装包，其中有 docs/superpowers/plans/*（内部审计与修复计划）、model-remaining-issues-*.md、源 .docx 等内部开发文档；已修复：filter 白名单只打包用户手册与理论手册 PDF，排除 superpowers/、内部问题清单、release notes、figures 与 .docx 源文件

## models/histream/src/base/accelstruct.cpp

- L89: FIXED[中] 安全: meshId 未校验是否 < BLAS 数量，越界时 getBlasDeviceAddress 越界访问；已修复：构建实例前校验 meshId ∈ [0, BLAS 数量)，越界抛 std::runtime_error 并报出实例下标、meshId 与网格数量；instanceCustomIndex 的 24 位截断需改场景组织，仍为 DEFERRED。

## models/histream/src/base/appsetting.cpp

- L185: FIXED[中] 错误处理: glfwInit/glfwCreateWindow 失败此前只返回 0 且 init() 忽略，随后以空窗口创建 surface；已修复：initWindow 失败时记录 GLFW 错误并释放窗口/GLFW 后返回 0，init() 检查返回值并抛异常。
- L262: FIXED[低] Bug: 手工把 AS/RT 管线/BDA 特性结构串到 enabledAtomicsFeatures.pNext，与 addDeviceExtension 已注册的同 sType 结构重复（nvvk 构建链时会覆盖 pNext，此链被丢弃，若未覆盖则违反 VUID-VkDeviceCreateInfo-pNext）；已修复：删除重复的 AS/RT 管线/BDA 手工链，只保留随扩展注册的 shader-atomic-float 特性结构。
- L370: FIXED[低] Bug: 在 lavapipe 上 requestedQueues 被缩减为仅 GCT 后，m_queueC/m_queueT 是空句柄却仍被放进 m_queues[1]/[2]；已修复：句柄为空时回退为已创建的 GCT 队列，避免后续向 VK_NULL_HANDLE 提交。
- L395: FIXED[中] 错误处理: Release 下 assert 被移除，surface 创建失败后仍以无效 m_surface 查询 present 支持；已修复：创建失败时抛异常，查询失败或不支持 present 时记录警告并返回 0（CPU 批处理后端可能不支持 present）。

## models/histream/src/base/atmosphere_lut.h

- L74: FIXED[中] 安全: 窄字符 std::ifstream(path) 按 ANSI 代码页解析，中文/UTF-8 路径的 LUT 打开失败后只返回无效表，sampleAtmosphereLut 静默返回“无校正”；已修复：保留原回退行为，但打开失败时输出含完整路径的明确告警并说明大气校正被跳过（u8path 全链路属超范围，未做）。
- L112: FIXED[中] Bug: 波长按文件行序追加，未排序也未去重；spectralSample 依赖 wavelength 升序做 upper_bound，LUT 行顺序被打乱时插值结果错误；已修复：load 结束后对每个 Spectrum 调用 sortSpectrum，按波长把 wavelength/transmittance/pathRadiance 三个数组一起稳定排序。
- L129: FIXED[中] 并发: 函数内 static std::map 的查找与插入无锁，并行首次加载会数据竞争；已修复：为静态缓存加 std::mutex（返回的 Table 引用在 map 生命周期内稳定，查找/插入语义不变）。

## models/histream/src/base/compo.cpp

- L85: FIXED[中] 错误处理: 自定义光谱的反射率/透射率个数与传感器波段数不一致时对所有波段静默返回第一个值（平坦错误光谱且无提示）；已修复：按 wavelengths/values 的较短长度插值，并对不一致组合用 std::call_once 打一次含材质名与两侧长度的告警（单值广播的既有用法保持原行为）。
- L221: FIXED[中] 数值: 未校验 PROSPECT 参数，N<=0 除零、N<1 使 pow(b,N-1)/Tsub 非物理、浓度为负使 Kall<0 令 expint 发散并产生被 bounded() 静默替换的 NaN；已修复：入口校验 N>=1 且 Cab/Cw/Cdm/Cs 为有限非负值，非法时抛 std::runtime_error 并报出各参数（OptCoeff 仍按值传递，改 const 引用属接口改动，未做）。
- L499: FIXED[中] Bug: 循环以 gsv1_.size() 为界同时索引 gsv2_/gsv3_/kw_/nw_，gsv1_ 更长时越界读；已修复：上界改为 gsv1_/gsv2_/gsv3_/kw_/nw_ 长度的最小值，两个循环共用该上界。

## models/histream/src/base/engine.cpp

- L167: FIXED[中] 未初始化: eRaytracing/eVoxelEB/eVoxelRT 分支直接解引用求解器指针，未调用 input()/init() 时为空指针崩溃（destroy() 已判空）；已修复：入口按模式判空求解器与 IO 指针，未初始化时抛出带说明的 runtime_error（run() 同改，main 已有异常捕获）。
- L221: FIXED[中] 未初始化: 求解器虽已用 && 短路判空，但 IO 指针未判空且未初始化时静默返回 1；已修复：入口按模式判空求解器与 IO 指针，未初始化时抛出带说明的 runtime_error（与 create() 一致）。
- L222: FIXED[中] 未初始化: run() 入口判空漏掉 m_pFileio，eRaytracing/eVoxelEB/eVoxelRT 分支把空 shared_ptr 传给求解器 run()（前轮补漏发现，原文件未标注）；已修复：这三个分支的守卫条件补 && m_pFileio，未初始化时抛出与 create() 一致的 runtime_error（create() 未使用 m_pFileio，无需改动）。
- L261: FIXED[低] 错误处理: 未判空直接解引用 m_pRaytracing/m_pVoxeleb/m_pVoxelrt，且 appSetting.destroy() 对未创建的 surface 调用 vkDestroySurfaceKHR；已修复：与 Facet 分支一致先判空，AppSetting::destroy() 仅在 m_surface 非空时销毁 surface。

## models/histream/src/base/engine.h

- L37: FIXED[低] 未初始化: m_mode 无默认值，input() 收到未识别的模式字符串时保持未初始化，create/run/destroy 按垃圾值分支；已修复：m_mode 默认初始化为 eRaytracing，input() 对未知模式抛 std::runtime_error。

## models/histream/src/base/fileio.cpp

- L50: FIXED[中] 内存: 成员 RootElement 曾指向局部 mydoc，函数返回后成为悬垂指针（UAF），且空文档时 RootElement 为 nullptr 仍被解引用；已修复：文档改为函数内 static 对象（生命周期覆盖 RootElement 的全部使用），RootElement 判空，为空时报错并返回 false。
- L146: FIXED[高] 错误处理: node/AeroNode 未判空（缺 <Attribute> 或 <Aerodynamics> 时空指针崩溃），各子元素 FirstChildElement(...)->GetText() 未判空，stoi/stof 对非法文本抛未捕获异常；多个子节点时只保留最后一个；无子节点时返回未初始化的 aeroCondXml；已修复：node 与 <Aerodynamics> 判空后返回值初始化的默认结构，循环内逐项判空（缺项时打印元素名并跳过该条），stoi/stof 用 try/catch 包裹，非法文本不再终止进程。本文件 readMeteoXML/readPropertyXML/readThermalXML/readCanopyXML/readSceneXML 的同类链式解引用另行标注。
- L208: FIXED[高] 错误处理: node 为空（XML 缺 <Meteorology>，VoxelEB 模式必经）时空指针崩溃，GetText() 返回 nullptr 时构造 std::string 为 UB；已修复：node 判空、每项先取文本再判空（缺失时抛出带字段名的异常并在本函数内捕获打印），数值解析失败同样捕获，返回默认结构而不是崩溃。
- L259: FIXED[高] 空指针: 缺 <Attribute> 或 <Biochemistry> 时 node/chemstry_node 为空立即崩溃；已修复：两级均判空后返回空列表。
- L284: FIXED[高] 内存: Tparam 被解析 5 次（每次 mySplitFloat 都泄漏 new[]），且未检查分割结果至少 5 个元素，少于 5 个时 [3]/[4] 越界读；Attribute("name") 返回 nullptr 时赋给 std::string 崩溃（soilSet/waterSet 同）；已修复：Tparam 只解析一次存入局部 vector 并校验 size()>=5（不足时补 0 并告警），leafBio/soilSet/waterSet 的 name 均判空后赋值。
- L339: FIXED[中] Bug: 空元素 <brdfModel/> 时 GetText() 返回 nullptr，用其构造 std::string 为 UB/崩溃（下方 waterSet 的 brdfModel 及 hapke*/refractiveIndex 等 stof(GetText()) 同理）；已修复：GetText() 先判空（空文本按非 Hapke 处理），下方 hapke*/refractiveIndex/slopeVariance/diffuseFraction 均先检查文本非空再 stof。
- L393: FIXED[中] 空指针: 父节点为空（如缺少 <Geometry>/<Control> 等父元素）时直接解引用崩溃；已修复：parentEle 为 nullptr 时返回 false。
- L437: FIXED[中] 错误处理: type=file 时通常只给 spectral_file，但这里无条件解引用 <reflectance>/<transmittance>，缺失即空指针崩溃（Prospect/BSM 分支同理）；已修复：三条分支统一通过 parseCurve 先判元素与文本是否存在，缺失时保留空曲线并打印波段名告警；path.string() 的 ANSI 转换问题保留待确认。
- L501: FIXED[中] Bug: 起点按名字 "thermal" 查找，但迭代用无名 NextSiblingElement()，会把 <Thermal> 下其他任意元素当作 thermal 解析；Attribute("name") 为空时构造 std::string 崩溃，子元素缺失时链式解引用崩溃；已修复：迭代改为 NextSiblingElement("thermal")，name 与子元素文本均判空（thermalxml 值初始化，数值解析包 try/catch）。
- L528: FIXED[中] 空指针/Bug: node 或 <Canopy> 缺失时空指针崩溃，迭代用无名 NextSiblingElement() 会把 <Canopy> 下其他元素当冠层，Attribute("name") 为空时构造 std::string 崩溃；已修复：两级判空后返回空列表，迭代改为 NextSiblingElement("canopy")，name 判空。
- L589: FIXED[中] 错误处理: node（<Geometry>）与 sensorEle 未判空，缺失即空指针崩溃；pixelResolutionX/Y、controlBand、viewAngle 均为链式解引用；Attribute("name") 为 nullptr 时赋给 std::string 崩溃；已修复：node/<Sensor> 判空后返回默认 sensorxml，name/projection/分辨率/波段文本与 viewAngle 元素均判空，数值解析包 try/catch（SPP/CSPP/enabled/angleNum 用 readInt 取默认值），缺失项打印带元素名的告警而不是崩溃。
- L633: FIXED[低] Bug: <projection/> 为空时 GetText() 返回 nullptr，构造 std::string 崩溃；已修复：GetText() 判空后再构造（空文本保持默认投影）。
- L683: FIXED[中] 空指针: 缺 <controlBand> 时 FirstChildElement 返回空指针、<controlBand/> 空文本时 GetText() 返回 nullptr，而 mySplitFloat(const std::string&) 由 nullptr 构造 std::string 即崩溃（上方 587 行的"已判空"注释此前并未落实到本行）；已修复：用同函数已有的 elementText 取文本并判空，缺失/空文本时打印告警、波段保持默认（空）。
- L693: FIXED[中] 错误处理: subEle->Attribute("type") 为 nullptr 时构造 std::string 崩溃；自定义角度用 mySplitFloat(...)[0]/[1] 未检查元素个数（"30" 只有一个数时 [1] 越界读）且同一文本解析两次；已修复：type 判空后构造（缺失时按空类型跳过），自定义角度只解析一次并校验 size()>=2（不足时打印告警并跳过该组），viewAngle 元素缺失时循环直接不执行。
- L720: FIXED[高] Bug: vzaStep<=0 时 for (k=0; k<=VzaMax; k+=vzaStep) 死循环并无限 push_back 直至内存耗尽；另外 SAA 是未初始化成员；已修复：SPP/CSPP/vzaMax/vzaStep 改用 readInt（缺失或非法时取 0，不再 stoi(nullptr)），并校验 VzaMax>0 且 vzaStep>0，非法时打印错误并跳过该 BRF 角度集（两个分支均已处理）；SAA 在 readXml 入口初始化为 0，readLightXML 会写入真实值。
- L789: FIXED[中] 数值: hemiAngleNum<=0 时 sqrt(pi/k0)、360/k00 除零；迭代中 k1 由 float 截断为 int，环数增加后 k1 先变为 0 而不是 <0，下一轮 k0=0 同样除零得到 inf/NaN 角度并静默写入 viewAngles，break 条件 k1<0 几乎不会触发；pi 仅取 7 位；已修复：仅在该角度集 hemiAngleNum>0（且 enabled 解析成功）时才生成半球角度，否则跳过该角度集；break 条件由 k1<0 改为 k1<=0，避免 k0 归零后继续除零；enabled/angleNum 改用 readInt（缺失/非法不再崩溃）。pi 取值精度属数值常量，未改动（待确认）。下方高光谱分支同一段代码同样修复。
- L869: FIXED[中] 错误处理: bandsFilePath 未按 m_inputDirectory 解析相对路径（spectral_file 却会），且用窄字符 ifstream 打开；文件打不开时只打印一行仍继续，waves 为空，后续按波段数分配/循环为 0 或越界；已修复：相对路径先拼接输入目录，路径按 UTF-8（u8path）解释后交给 ifstream（Windows 下最终走宽字符 API），元素或文本缺失、文件打不开、读完 waves 仍为空时打印带路径的错误并跳过该传感器，避免空波段继续传播。
- L1006: FIXED[中] 数值: hemiAngleNum<=0 时 sqrt(pi/k0)、360/k00 除零；迭代中 k1 由 float 截断为 int，环数增加后 k1 先变为 0 而不是 <0，下一轮 k0=0 同样除零得到 inf/NaN 角度并静默写入 viewAngles，break 条件 k1<0 几乎不会触发；pi 仅取 7 位；已修复：仅在该角度集 hemiAngleNum>0（且 enabled 解析成功）时才生成半球角度，否则跳过该角度集；break 条件由 k1<0 改为 k1<=0，避免 k0 归零后继续除零；enabled/angleNum 改用 readInt（缺失/非法不再崩溃）。pi 取值精度属数值常量，未改动（待确认）。
- L1062: FIXED[中] 错误处理: 成员 RootElement 依赖 readXml 中局部 mydoc 存活，且 controlnode 为空（缺 <Control>）时 ->ToElement() 空指针崩溃；已修复：readXml 的文档改为 static 保证生命周期，此处 RootElement/controlnode 均判空（sonExists 也已内部判空），缺 <Control> 时跳过这些开关项。
- L1107: FIXED[中] 空指针: geometryNode（缺 <Geometry>）与 lightEle（缺 <Light>）均未判空即链式解引用（静态复核发现，原文件未标注）；已修复：两级均判空并打印告警后返回默认 lightxml，不再崩溃。
- L1276: FIXED[中] Bug: 判断的是 bgBioType 是否存在，却无条件解引用 bgBioName；只给 bgBioType 而缺 bgBioName 时空指针崩溃；两个 GetText() 也未判空；已修复：bgBioName 与 bgBioType 分别查元素并判空文本，缺 bgBioName 时沿用默认背景材质名 "soilset"，缺 bgBioType 时保持默认背景类型。
- L1296: FIXED[中] 空指针: sonExists 只判断元素是否存在，<bgSpectral/> 为空文本时 GetText() 返回 nullptr，赋给 std::string 成员 bgSpectralName 即崩溃（静态复核发现，原文件未标注）；已修复：照下方 bgThermal 的既有写法先判 GetText() 非空再赋值，空文本保持默认背景光谱名。

## models/histream/src/base/fileio_json.cpp

- L248: FIXED[中] 错误处理: project.viewAngles() 为空时 sensorData.viewAngles 为空，Geometry::createGeometry 随后 angles[0] 越界；已修复：解析完成仍为空时补一个天底观测 {0,0}（与 Geometry 侧的回退一致）；角度范围（0<=vza<90）、zenith/azimuth/skyTemperature 范围及 sza>=90 时直射处理属于物理约定，未改动（见报告）。
- L580: FIXED[低] 数值: Tparam 不足 5 个时原先用 0 补齐，后三项是温度（K），补 0 会使光合温度响应函数出现 1/T 或 exp(…/(R·T)) 的除零/溢出；已修复：缺失项改用同一分支已内置的默认 Tparam 值（0.2,0.3,288,313,328）逐位补齐并告警，不再补 0。

## models/histream/src/base/geometry.cpp

- L390: FIXED[高] 内存: sensorxml.viewAngles 为空（配置缺省观测角）时 angles 为空，下方 angles[0] 越界读（UB/崩溃）；已修复：解析完 viewAngles 后若 angles 仍为空，则补一个天底观测 {0,0}（沿用当前太阳角），保证 angles[0] 有效；solarAngle 为 glm::vec2，无“元素个数不足”问题。
- L590: FIXED[高] 内存: viewAngles 为空时 angles[0] 越界读（同 RaytracingIO 重载）；已修复：解析完 viewAngles 后若 angles 为空，则补一个天底观测 {0,0}（沿用当前太阳角）。
- L825: FIXED[高] 内存: viewAngles 为空时 angles[0] 越界读（同 RaytracingIO 重载）；已修复：解析完 viewAngles 后若 angles 为空，则补一个天底观测 {0,0}（沿用当前太阳角）。

## models/histream/src/base/hexvoxel.cpp

- L599: FIXED[中] 数值: gx*gy*gz 以 int 相乘，体元数 >2^31 时溢出（而 cells 用 size_t 计算），parity 被分配为错误大小，后续 cellIndex 越界写；已修复：total 改为 std::size_t 逐维强制转换后相乘，parity 按该 size_t 分配；网格总数上限（阈值）未加（DEFERRED）。
- L647: FIXED[高] Bug: 同一三角形的 AABB 若跨越 x 与 x+1 两个胞，会同时出现在两个胞的列表中，被测试命中一次却 cross^=1 两次而抵消，导致中心连线穿过斜面时奇偶性不翻转、封闭体内部整行填充错位；已修复：把命中的三角形按索引去重后再翻转奇偶性，hits 在行内复用避免每个体元都分配。
- L1118: FIXED[低] Bug: objPath 与 label 未做 JSON 转义，Windows 路径中的反斜杠（C:\data\tree.obj）或引号会生成非法 JSON；已修复：新增本地 jsonEscape（转义反斜杠、双引号、换行、回车、制表）并用于 objPath 与每个 label；"grid" 仍输出为含一个字符串的数组，字段语义未改（DEFERRED）。

## models/histream/src/base/myfunction.cpp

- L34: FIXED[中] 内存: strs 与 d 两个 new char[] 从未 delete[]，每次调用都泄漏（mySplitInt/mySplitFloat 同样）；已修复：三个 mySplit* 均改为基于 std::string::find_first_of 的无堆分配分割，不再 new char[]，函数签名与全部调用点不变。

## models/histream/src/base/objloader.cpp

- L882: FIXED[中] 未初始化: m_worldOrigin/m_worldSize 从未被赋值（nvmath::vec3f 默认不清零），范围判断与归一化使用垃圾值，m_worldSize 为 0 时还会除零；m_heightmap 为空时 at<float> 越界；已修复：getHeightAt 入口先判 m_heightmap 为空、再判 m_worldSize.x/y 非正（含 NaN），命中即返回，creatBackgroundFromDEM 中按 sceneSize 设置 m_worldOrigin={-sceneSize.x/2,0,-sceneSize.y/2}、m_worldSize=sceneSize。
- L987: FIXED[低] 数值: 未校验 sceneSize.x/y 为有限正数（NaN/inf 时 ceil(...) 转 int 属未定义行为，cv::resize 可能抛异常或分配巨量内存）；已修复：creatBackgroundFromDEM 入口检查 std::isfinite 且 >0，不满足时抛出 std::runtime_error。
- L1107: FIXED[低] 数值: sceneSize.x 或 y 为 0 时 wx==0 仍通过范围检查，0/0 得 NaN，(int)NaN 属未定义行为并作为下标访问 m_heightmap；已修复：函数开头校验 sceneSize.x/y 有限且 >0，不满足时打印错误并直接返回，不再做归一化。

## models/histream/src/base/projectjson.cpp

- L99: FIXED[低] 错误处理: std::filesystem::exists 的无 error_code 重载在权限不足/网络盘断开时抛 filesystem_error，会导致整个项目加载失败；已修复：两处均改用 exists(p, ec) 重载，查询失败（ec 置位）按“不存在”处理并继续回退查找，不再抛异常。
- L159: FIXED[低] Bug: 逗号分隔的名字未去除首尾空白，"leaf, bark" 得到 " bark"，后续按名字查找光谱/网格失败（numbers() 却把空格当分隔符）；已修复：每个逗号分隔的 token 先 trim 空白字符，空 token 仍被跳过。
- L190: FIXED[低] 数值: 整数经 float 中转，>2^24（16,777,216）的值（如随机种子、大计数）会丢失精度，超出 long 范围时 lround 结果未指定（Windows 上 long 为 32 位）；已修复：JSON 整数直接取 64 位整型并按 int 范围钳制，仅非整数数值/字符串仍走原 float 路径。

## models/histream/src/base/scene.cpp

- L142: FIXED[低] 数值: 未检查 stepSize>0；为 0 或极小时 sceneSize/stepSize 为 inf/巨大值，static_cast<int> 属未定义行为，columns*rows 分配可能耗尽内存；已修复：入口校验 stepSize 为有限正数，否则抛异常；顶点数/格数上限与共享顶点属性能改写，仍为 DEFERRED。
- L1114: FIXED[低] 内存: 未校验 triOffset+triCount <= completeScene.tris.size()，对象表与三角形数组不一致（OBJ 解析异常）时迭代器越界属未定义行为；已修复：插入前用 size_t 求和做范围检查，越界时抛异常。
- L1598: FIXED[高] 内存: spectralNames[kmesh] 与下方 types[kmesh] 未做长度检查，JSON 中光谱/类型绑定条目少于 meshNames 时越界读（UB/崩溃）；已修复：spectralNames 为空时抛异常，否则用 min(kmesh, size-1) 索引；types 按 kmesh < types.size() 判断，缺省用 Type{}。
- L1653: FIXED[高] 内存: 位置直接写在 JSON 中（非文件）时 scales/rotations 不会按 objDistributions 补齐，二者条目少于位置数即越界读；已修复：进入实例循环前 resize(objDistributions.size(), 1.0f/0.0f)，索引安全。
- L1844: FIXED[高] 内存: PrimEntity 默认 scales{1}/rotations{0} 只有 1 个元素，JSON 直接给出多个 primDistributions 而未给等长 scales/rotations 时（非文件路径），下方 scales[kinstance]/rotations[kinstance] 越界读；已修复：进入实例循环前按 primDistributions.size() resize scales/rotations（补齐 1.0f/0.0f）。
- L1845: FIXED[中] Bug: 下方 Instance/InstanceLink 在插入体元之前 push_back，但本次放置若没有新增体元（isValid==0：全部与已有体元重叠或越界）会 continue 而不递增 n_instance，instances.size() 与 n_instance 脱节，后续 voxelLink.instanceId 指向错误实例（变换/meshId 错位）；已修复：isValid==0 时 pop_back 刚压入的 instances/instanceLinks 再 continue，使 instances.size() 始终等于 n_instance。
- L1972: FIXED[中] 安全: 形状类型由浮点列直接强转为枚举，未校验 0..2 范围；height/width/length 未校验 >0，非法值使 createTriEntity 产生 0 个或海量体元；已修复：静默强转前校验 row[0]∈[0,2]（ShapeType 只有 CUBE/ELLIPSOID/PLANE），并校验三个尺寸为有限且非负，非法时抛出带形状文件名的异常。
- L1986: FIXED[中] 内存: meshNames/spectralNames/canopyNames/propNames 直接取 [0] 未判空，缺少任一绑定即越界；下方 MeshLink meshlink1 未值初始化（水体类型时 bioId 及 vertexAddress 等为垃圾值），thermalId 固定为 0 而忽略 thermalNames；已修复：改用 requiredMappedId/mappedId（内含空表判空与下标钳制）取首个绑定，MeshLink 改为 meshlink1{} 值初始化；thermalId 仍为 0，行为不变。
- L2069: FIXED[中] Bug: isValid==0 时已经 push 实例但 n_instance 未递增，后续 voxelLink.instanceId 与 instances 下标错位（与 1844/2590 同源，原文件未标注）；已修复：pop_back 撤销本次压入
- L2218: FIXED[中] 内存: isValids[kvoxel] 未校验 isValids.size()==voxelIds.size()（createTriEntitiesFromTif_wall 等生成器若未逐体元填写即越界读）；已修复：循环前把 isValids 补到 voxelIds.size()（int5{} 全零），保证下方索引不越界。
- L2298: FIXED[中] 数值: 未校验 stepsize_surface>0，为 0 时除零得到 inf，转换为 int 属未定义行为；float→int 截断使 sceneSize 不是步长整数倍时最后不足一格的条带被丢弃，而 DEM 背景用 floor、OBJ 路径用 floor(x/2+0.5) 求半宽，三处取整规则不一致可能产生半格偏移；已修复：入口校验 stepsize_surface 为有限正数，否则抛异常（取整规则的统一仍为 DEFERRED）。
- L2428: FIXED[中] 错误处理: readImageinout1 的返回值被忽略，LAD 影像打不开时 lads 保持为空（不会走 else 分支补 0），着色器按体元索引读取空缓冲；已修复：检查返回值，读取失败或结果为空时抛出带路径的异常，避免零长度 lads 被按体元索引；影像 width×height 与背景网格尺寸的一致性校验仍为 DEFERRED。
- L2593: FIXED[高] 内存: PrimEntity 默认 scales{1}/rotations{0} 只有 1 个元素，JSON 直接给出多个 primDistributions 而未给等长 scales/rotations 时（非文件路径），下方 scales[kinstance]/rotations[kinstance] 越界读；已修复：进入实例循环前按 primDistributions.size() resize scales/rotations（补齐 1.0f/0.0f）。
- L2594: FIXED[中] Bug: 下方 Instance/InstanceLink 在插入体元之前 push_back，但本次放置若没有新增体元（isValid==0：全部与已有体元重叠或越界）会 continue 而不递增 n_instance，instances.size() 与 n_instance 脱节，后续 voxelLink.instanceId 指向错误实例（变换/meshId 错位）；已修复：isValid==0 时 pop_back 刚压入的 instances/instanceLinks 再 continue，使 instances.size() 始终等于 n_instance。
- L2721: FIXED[中] 安全: 形状类型由浮点列直接强转为枚举，未校验 0..2 范围；height/width/length 未校验 >0，非法值使 createTriEntity 产生 0 个或海量体元；已修复：静默强转前校验 row[0]∈[0,2]（ShapeType 只有 CUBE/ELLIPSOID/PLANE），并校验三个尺寸为有限且非负，非法时抛出带形状文件名的异常。
- L2735: FIXED[中] 内存: meshNames/spectralNames/canopyNames/propNames 直接取 [0] 未判空，缺少任一绑定即越界；下方 MeshLink meshlink1 未值初始化（水体类型时 bioId 及 vertexAddress 等为垃圾值），thermalId 固定为 0 而忽略 thermalNames；已修复：改用 requiredMappedId/mappedId（内含空表判空与下标钳制）取首个绑定，MeshLink 改为 meshlink1{} 值初始化；thermalId 仍为 0，行为不变。
- L2820: FIXED[中] Bug: isValid==0 时已经 push 实例但 n_instance 未递增，后续 voxelLink.instanceId 与 instances 下标错位（与 1844/2590 同源，原文件未标注）；已修复：pop_back 撤销本次压入
- L2955: FIXED[中] 内存: isValids[kvoxel] 未校验 isValids.size()==voxelIds.size()（createTriEntitiesFromTif_wall 等生成器若未逐体元填写即越界读）；已修复：循环前把 isValids 补到 voxelIds.size()（int5{} 全零），保证下方索引不越界。
- L3035: FIXED[中] 数值: 未校验 stepsize_surface>0，为 0 时除零得到 inf，转换为 int 属未定义行为；float→int 截断使 sceneSize 不是步长整数倍时最后不足一格的条带被丢弃，而 DEM 背景用 floor、OBJ 路径用 floor(x/2+0.5) 求半宽，三处取整规则不一致可能产生半格偏移；已修复：入口校验 stepsize_surface 为有限正数，否则抛异常（取整规则的统一仍为 DEFERRED）。
- L3166: FIXED[中] 错误处理: readImageinout1 的返回值被忽略，LAD 影像打不开时 lads 保持为空（不会走 else 分支补 0），着色器按体元索引读取空缓冲；已修复：检查返回值，读取失败或结果为空时抛出带路径的异常，避免零长度 lads 被按体元索引；影像 width×height 与背景网格尺寸的一致性校验仍为 DEFERRED。
- L3233: FIXED[中] 内存: 以 model.nVertices 为循环上界而非 vertices.size()，二者不同步（如加载器只填了 vector 未更新计数）时越界读；已修复：上界取 min(nVertices, vertices.size())，计数大于向量长度时不再越界（按值传参/整份复制 indices 的性能问题仍为 DEFERRED）。

## models/histream/src/base/utils.cpp

- L104: FIXED[低] 错误处理: 文件打不开时 num 保持调用方传入的旧值且函数仍返回 1，调用方无法区分"空文件"与"打开失败"；atof 对非数字字段静默返回 0；已修复：打不开时把 num 置 0、data 保持已清空并返回 0，调用方可区分失败（atof 的静默解析仍为 DEFERRED）。
- L166: FIXED[中] Bug: 只识别 '/'，Windows 原生路径（C:\a\b.obj）找不到分隔符，返回空目录/整串文件名，相对资源路径解析错误（getFileName 同理）；已修复：两个函数的分隔符集合改为 "/\\"，Windows 原生路径也能正确切分。
- L207: FIXED[中] 内存: nband==0 时 GetRasterBand(1) 返回 nullptr 被解引用；sizeof(float)*width*height 中 width*height 先以 int 相乘，>2^31 像素溢出；RasterIO 返回值未检查，读取失败仍返回 1（readImageinout/readImage 同理）；已修复：nband<1 或尺寸非正时关闭数据集并返回 0（readImageinout 同步加上同样的波段/尺寸判空），像素数与字节数改用 size_t 计算，poBand/CPLMalloc 判空后再使用；RasterIO 的 CPLErr 当时仍未检查（DEFERRED），本轮已在下方 RasterIO 处补齐检查。
- L214: FIXED[中] 错误处理: GetRasterBand(1) 在判空之前就被解引用以取 GetRasterDataType（前轮补漏发现，原文件未标注）；已修复：先取出 poBand 并判空，再取数据类型，空指针时 GDALClose 数据集并返回 0（readImageinout/readImage 同改）。
- L231: FIXED[中] 错误处理: RasterIO 的 CPLErr 未检查，读取失败时仍把未初始化的 bandData 当作影像返回 1（前轮补漏发现，原文件未标注）；已修复：检查 != CE_None 时 CPLFree(bandData)、GDALClose 数据集并返回 0。
- L278: FIXED[中] 错误处理: GetRasterBand(1) 在判空之前就被解引用以取 GetRasterDataType（前轮补漏发现，原文件未标注）；已修复：先取出 poBand 并判空，再取数据类型，空指针时 GDALClose 数据集并返回 0（readImageinout1/readImage 同改）。
- L286: FIXED[中] 内存: sizeof(float)*width*height 与 bandData+width*height 的像素数仍按 int 相乘（>2^31 像素时溢出，分配/拷贝越界）；已修复：像素数改用 size_t 逐项相乘，分配前回除校验，分配字节数与指针偏移均用 pixelCount（readImage 同改）。
- L294: FIXED[中] 错误处理: GetRasterBand(kband)/CPLMalloc 返回值未判空、RasterIO 的 CPLErr 未检查，波段为空或分配/读取失败时解引用空指针或把未初始化数据当作影像返回（前轮补漏发现，原文件未标注）；已修复：三者逐一判空并检查 CE_None，失败时释放已分配的 bandData、GDALClose 数据集并返回 0。
- L355: FIXED[中] 内存: 与 readImageinout 同源的 int 像素数乘法，bandData+width*height 在大图上溢出越界；且本函数缺少波段/尺寸判空（第 207 行注释声称已同步，实际未加）；已修复：补上 nBands<1 或尺寸非正的判空，像素数用 size_t 计算并回除校验，分配与偏移改用 pixelCount。
- L368: FIXED[中] 错误处理: GetRasterBand(1) 在判空之前就被解引用以取 GetRasterDataType（前轮补漏发现，原文件未标注）；已修复：先取出 poBand 并判空，再取数据类型，空指针时 GDALClose 数据集并返回空 vector（readImageinout1/readImageinout 同改）。
- L379: FIXED[高] Bug: 循环条件 kband < nBands 少读最后一个波段（GDAL 波段号 1..nBands），collected[nBands-1] 恒为空，单波段影像返回空数据，调用方按 width*height 索引即越界；已修复：循环上界改为 kband <= nBands，最后一个波段也被读取。
- L380: FIXED[中] 错误处理: GetRasterBand(kband)/CPLMalloc 返回值未判空、RasterIO 的 CPLErr 未检查，波段为空或分配/读取失败时解引用空指针或把未初始化数据当作影像返回（前轮补漏发现，原文件未标注）；已修复：三者逐一判空并检查 CE_None，失败时释放已分配的 bandData、GDALClose 数据集并返回空 vector。
- L430: FIXED[中] 错误处理: driver/ods 未判空（目录不存在、无写权限、路径编码问题时 Create 返回 nullptr 直接崩溃），未校验 c.size()>=width*height（不足时 RasterIO 越界读），RasterIO 结果被忽略；已修复（saveImage 同改）：driver/ods 判空后返回 0，写入前校验尺寸为正且 c.size()>=width*height；RasterIO 的 CPLErr 与 proj/trans 地理参考仍未使用，保持 DEFERRED。
- L559: FIXED[高] 内存: 文件打不开时 num 未被赋值（保持调用方传入的值，可能是未初始化垃圾/负数），new float[num] 可能抛 bad_array_new_length 或分配巨量内存；已修复：打不开时置 num=0 并返回 nullptr，num<=0 时不再分配；返回裸 new[] 的接口改为 std::vector<float> 属 API 变更，保持 DEFERRED。
- L608: FIXED[低] 内存: new int[k] 从未 delete[]，每次调用泄漏；连续数字串较长时 int result 有符号溢出（UB）；已修复：去掉临时数组，直接 push_back 到 numVec，不再泄漏（超长数字串的 int 溢出仍为 DEFERRED）。

## models/histream/src/base/virtualscreen.cpp

- L13: FIXED[低] 越界/性能: 此前未校验 size 不超过 bufferIn/bufferOut 的实际大小，超出时 vkCmdCopyBuffer 越界（命令池复用需重构，本轮未改）；已修复：复制前用 vkGetBufferMemoryRequirements 取两缓冲容量，size 超出时返回 false，不再提交越界复制。

## models/histream/src/base/voxel_boundary.h

- L36: FIXED[低] Bug: 外层遍历的是原始 cells 而非去重后的 occupied，输入含重复体元（多个实例/子网格重叠时常见）时同一外表面被重复输出，产生重合三角形（光线求交 z-fighting、面积统计翻倍）；已修复：外层改为遍历去重后的 occupied 集合（输出顺序随哈希顺序变化，几何语义不变）。

## models/histream/src/base/voxeldesigner.cpp

- L33: FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 ceil(inf) 转 int 属未定义行为（本文件所有 createXxx 同理）；下方椭球判定用体元角点 i 而非中心 i+0.5 与 size/2 比较（注释中的推导不成立），整个椭球向负方向偏移半格、形状不对称；已修复：入口校验 stepSize/height/width 有限且 >0，非法时清空 m_voxelModel 并返回空结果；角点判定未改（DEFERRED，改变形状）。
- L90: FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 ceil(inf) 转 int 属未定义行为（与 createEllipsoid 同源）；已修复：入口校验 stepSize/length/width/height 有限且 >0，非法时清空 m_voxelModel 并返回空结果。
- L189: FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 height/stepSize 为 inf，循环上界转 int 属未定义行为（createTriCube_wall/_roof 同源）；已修复：入口校验 stepSize 与 shape 三维有限且 >0，非法时返回空 mesh。
- L442: FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 height/stepSize-1 为 inf，循环上界转 int 属未定义行为（与 createTriCube 同源）；已修复：入口校验 stepSize 与 shape 三维有限且 >0，非法时返回空 mesh。
- L661: FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 height/stepSize-1 为 inf，转 int 属未定义行为（与 createTriCube 同源）；已修复：入口校验 stepSize 与 shape 三维有限且 >0，非法时返回空 mesh。
- L717: FIXED[低] 物理: 输出时交换 y/z（Z-up→Y-up）是镜像变换，三角形绕序被反转，导出 OBJ 中法向朝内；窄字符路径打开中文目录失败；PrimMesh 按值传参整体复制；indices.size() 不是 3 的倍数时 kk+1/kk+2 越界；建议：交换坐标时同时交换每个面的两个索引，const 引用传参，循环条件用 kk+2<size；已修复：循环条件改为 kk+2<size 并使用 std::size_t 步进，尾部不足 3 个的索引不再被读出；其余三项未改（DEFERRED）。
- L992: FIXED[中] 数值: 浮点叉积被隐式截断为 int，|值|<1 的结果变为 0，导致靠近边的点被判为"不在多边形内"（严格 >0/<0），且大坐标时 float→int 溢出为 UB；ploys 未检查至少 4 个点即越界读；建议：叉积用 float/double 保存，必要时用 >=0 包含边界，并校验 ploys.size()>=4；已修复：入口校验 ploys.size()>=4，不足 4 点直接返回 false，不再越界读；叉积仍为 int（DEFERRED，边界判定语义需确认）。
- L1056: FIXED[低] 数值: 循环上界 i < sceneLength/stepSize 等价于 ceil，而 createBackground 用 int 截断（floor），scene.cpp 中背景格数也用截断；非整数倍时两者相差一列，背景网格与体元 ID 错位；stepSize 未校验 >0；建议：统一使用一个格数计算函数；已修复：入口校验 stepSize/sceneLength/sceneWidth 有限且 >0，非法时返回空 mesh；格数仍为循环上界与 int 截断并存（DEFERRED，改变体元数量）。
- L1298: FIXED[高] 内存: 缓冲区只分配 nImgSizeX*nImgSizeY 个 float，但 RasterIO 以 nBandCount=bandcount（panBandMap=nullptr 即读全部波段）读取，多波段 TIF（如带 alpha 的 RGB/高度图）时写入 bandcount 倍数据，堆溢出；int 乘法对 >2^31 像素溢出；RasterIO 返回值未检查；new[] 在后续抛异常时泄漏；建议：只读第 1 波段（GetRasterBand(1)->RasterIO）、用 std::vector<float>(size_t) 分配并检查 CPLErr（_roof/_wall 两个函数同理）；已修复：改从 GetRasterBand(1) 读第 1 波段，缓冲区用 std::vector<FLOAT>(size_t 像素数)，校验 bandcount/尺寸/CPLErr 并在失败时 GDALClose 后返回；minReHeightScanline 的 new[] 也改用 size_t 像素数（三处），原有 delete[] 保留；_roof/_wall 的读取错误处理未改（DEFERRED）。
- L1344: FIXED[中] 内存: heightScanline[(j±1)*nImgSizeX+i] 等下标的乘法仍按 int 计算，超大尺寸时 j*nImgSizeX+i 超过 INT_MAX 溢出为负下标/越界（前轮补漏发现，原文件未标注）；已修复：索引乘法的每个操作数逐项 static_cast 为 std::size_t，下标值不变。
- L1547: FIXED[中] 错误处理: 忽略 readImageinout1 的返回值，影像打不开时 width/height 未赋值，convertVector2Mat 用未初始化的行数、高度图为空或长度不符时越界（_wall 同源）；已修复：检查返回值与 width/height 为正、heightScanline0.size()==width*height（size_t 计算），失败时记录日志并返回空 mesh；INTER_LINEAR→INTER_NEAREST 属结果变化，保持 DEFERRED。
- L1595: FIXED[中] 内存: heightScanline[(j±1)*nImgSizeX+i] 等下标的乘法仍按 int 计算，超大 targetSize 时 j*nImgSizeX+i 超过 INT_MAX 溢出为负下标/越界（前轮补漏发现，原文件未标注）；已修复：索引乘法的每个操作数逐项 static_cast 为 std::size_t，下标值不变。
- L1715: FIXED[中] 错误处理: 忽略 readImageinout1 的返回值，影像打不开时 width/height 未赋值，convertVector2Mat 用未初始化的行数、高度图为空或长度不符时越界（与 _roof 同源）；已修复：检查返回值与 width/height 为正、heightScanline0.size()==width*height（size_t 计算），失败时记录日志并返回空 mesh；INTER_LINEAR→INTER_NEAREST 属结果变化，保持 DEFERRED。
- L1765: FIXED[中] 内存: heightScanline[(j±1)*nImgSizeX+i] 等下标的乘法仍按 int 计算，超大 targetSize 时 j*nImgSizeX+i 超过 INT_MAX 溢出为负下标/越界（前轮补漏发现，原文件未标注）；已修复：索引乘法的每个操作数逐项 static_cast 为 std::size_t，下标值不变。

## models/histream/src/base/xmlexamples.cpp

- L176: FIXED[中] Bug: 按位置初始化时 Tsoil 与 smc 互换（Tsoil=0.25、smc=25）；已修复：改为按成员名赋值，Tsoil=25、smc=0.25。

## models/histream/src/base/xmlexamples.h

- L96: FIXED[高] Bug: 按位置初始化时 Tsoil 与 smc 互换（Tsoil=0.25、smc=25 超过饱和含水量）；已修复：改为按成员名赋值。

## models/histream/src/facetrt/facetrtcore.cpp

- L4657: FIXED[低] 数值: scene.facetCount()*32U 以 uint32 相乘，面元数 >1.34e8 时溢出导致缓冲区过小、后续 data 指针越界写；已修复：改以 size_t 相乘（此处与下方 facet * 32U 的数据指针偏移同为 size_t 运算），不再溢出。

## models/histream/src/voxeleb/buffer.cpp

- L710: FIXED[低] 内存: 通量缓冲只分配不清零，显存内容未定义（先读后写会读到垃圾值甚至 NaN）；已修复：缓冲增加 TRANSFER_DST 用途，并在 cmdBufEvapo（与 tLast 上传同一次提交）中用 vkCmdFillBuffer 全零清零。
- L718: FIXED[低] 数值: n_voxel * TLASTNUM 为 int 乘法，n_voxel > 2.68e8 时有符号溢出（UB）；已修复：元素总数改用 size_t 计算，分配前用回除校验确认未回绕（并拒绝负的 n_voxel），溢出时抛 std::overflow_error。

## models/histream/src/voxeleb/voxeleb.cpp

- L477: FIXED[中] GPU同步: 原先在销毁缓冲/SBT/AS/描述符/管线之后才 vkDeviceWaitIdle，异常路径下会销毁仍在执行的资源；已修复：与 Voxelrt::destroy 一致，先等待设备空闲再销毁。

## models/histream/src/voxelrt/buffer.cpp

- L27: FIXED[高] Bug: spectral/canopy/wave/instanceLink/voxelLink/nano 只在非空时创建，但 Descriptor::createDescriptor 与 Buffer::destroy 无条件解引用这些 shared_ptr（如 meshio->m_pBufferSpectral->buffer），任一为空即空指针崩溃；已修复：spectral/canopy/wave/instanceLink/voxelLink/hex/nano 为空时仍创建 1 元素全零占位缓冲，createDescriptor 与 destroy 的解引用不会再命中空 shared_ptr；thermal 绑定缺失一项本轮未改（见下方 DEFERRED）。
- L323: FIXED[中] 空指针: m_pBufferThermal 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L326: FIXED[中] 空指针: destroy 无条件解引用 voxelio->m_pTempeBuffer，Buffer::create 未执行或中途失败（createBuffer 抛异常等）时为空即崩溃（静态复核发现，原文件未标注）；已修复：按同函数内 spectral/canopy 的写法先判 shared_ptr 非空再销毁。
- L332: FIXED[中] 空指针: m_pBufferMeshLink 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L335: FIXED[中] 空指针: instanceLink/voxelLink/voxelNano 三个缓冲在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：逐个先判 shared_ptr 非空再销毁，销毁顺序与资源依赖关系保持不变。
- L341: FIXED[中] 空指针: m_pBufferLad 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L344: FIXED[中] 空指针: m_pBufferSensor 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L346: FIXED[中] 空指针: destroy 无条件解引用 voxellstio->m_pBufferWave，create 未执行或中途失败时为空即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L349: FIXED[中] 空指针: m_pBufferLight 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L359: FIXED[中] 空指针: m_pDirBuffer 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L361: FIXED[中] 空指针: m_pRadsBuffer 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L363: FIXED[中] 空指针: m_pNetRadBuffer 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。
- L366: FIXED[中] 空指针: m_pBufferStorage 在 create 未执行或中途失败时为空，destroy 无条件解引用即崩溃（静态复核发现，原文件未标注）；已修复：先判 shared_ptr 非空再销毁。

## models/histream/src/voxelrt/voxelrt.cpp

- L421: FIXED[中] GPU同步: vkDeviceWaitIdle 放在销毁缓冲/描述符/管线之后才调用；若有仍在执行的提交（如异常路径跳过 waitFence），先销毁正在使用的资源是未定义行为；已修复：把 vkDeviceWaitIdle 提前到销毁任何资源之前，并检查其返回值（失败只记录，后续仍继续销毁）。

## models/stream/app/image.cpp

- L238: FIXED[中] Bug: pack.lastHour() 返回 lastHour_，只在后面的 pack.load() 里赋值，此处恒为默认值 -1，条件永远不成立、lastHour 不会延到当天 23 UTC；已修复：改用已有的 pack.covers(variables, dayStartHour, dayLastHour, gap) 判断数据包是否覆盖当天末小时，覆盖时才延长 lastHour（gap 提前声明，未新增常量）
- L731: FIXED[中] 错误处理: 原来只检查 ta 和 longwave，d2m/sp/u10/v10/ssrd 缺测时 ea/p/wind/shortwave 为 NaN，残差变 NaN、组分温度停在初值却看似有效；已修复：用 validForcing 对 ta/ea/p/wind/shortwave/longwave 全部做 isfinite 检查，并在逐时次循环里同样检查 meteo，无效像元计入 ebNoForcing/ebFailed
- L941: FIXED[中] Bug: EB 模式下若该像元没有算出 diffuseFractionOut（ebNoForcing、SPA 失败等）或处于 RT 模式，会回退到 config.diffuseFraction，该值可为 −1（Erbs），使 canopyReflectance 里 direct=2、diffuse=−1；已修复：回退值不在 [0,1] 时用 requestedDiffuseFraction 现算（EB 有短波值时用该值，否则按 0 短波），不再把 −1 传进反射率
- L1099: FIXED[低] 错误处理: components.json 的 Save 返回值原来没有检查（scene_summary.json 有检查），写失败时后续 RT 复用会找不到元数据；已修复：与 scene_summary.json 一致，Save 返回 false 时抛异常

## models/stream/forcing/era5.cpp

- L180: FIXED[中] 内存: 原来没有先检查时间变量的维数就把 dimid 写进单个 int，valid_time 为二维 (time, step)（cfgrib/xarray 由 GRIB 转换的 ERA5-Land 常见）时会越界写栈；已修复：先 nc_inq_varndims 并要求 ndims==1，否则报错提示先展开 step，同时检查 nc_inq_vardimid 的返回值
- L232: FIXED[低] 错误处理: 原来按维度名识别时间轴，而 scanFile 按时间变量的 dimid 判断 timed，两者不一致，时间维名称不是 valid_time/time 时 timeAxis=-1，readFrame 会把整条时间轴当作"额外轴"读入并取第一个有效值，所有时次静默变成同一帧；已修复：用时间变量的 dimid（与 scanFile 相同的 firstVariable(kTimeNames)）定位时间轴；未知额外维仍按首个有效成员读取

## models/stream/physics/brightness.h

- L18: FIXED[低] 错误处理: radiance≤0 时 log1p(+inf)=inf 会静默返回 0 K（radiance<0 时返回 NaN 或负温度），调用方得不到提示；已修复：radiance≤0 或非有限时显式返回 NaN（std::nan），调用方 image.cpp 已在 radiance>0 时调用，不影响有效像元

## models/stream/physics/hapke.h

- L68: FIXED[中] 内存: viewZenithDeg 为 NaN 时 std::clamp 原样返回 NaN，static_cast<int>(NaN) 是未定义行为（MSVC 通常得到 INT_MIN），std::min 之后 index 为负，values_[index] 越界读；已修复：先检查 !std::isfinite(viewZenithDeg)，非有限角度直接返回天底值 values_[0]，再做插值

## server.mjs

- L208: FIXED[低] Bug: resettingProcesses 置 true 后，facetWorker.terminate() 的 await 位于 try/finally 之外，若其抛错则标志永不复位，之后所有运行/重置请求都会被拒绝；已修复：terminate 移入 try 块并单独 catch，finally 一定复位标志
- L240: FIXED[中] 安全: 页面可被其他网站用 iframe 嵌入（点击劫持）；已修复：所有响应都带 X-Frame-Options 与 CSP frame-ancestors
- L295: FIXED[低] 性能: 每次 /api/histream-proxy/info、run、result 请求都同步 stat 并校验全部文件，首次（或 mtime 变化后）会在主线程同步计算大型体数据的 SHA256，可能阻塞事件循环数十秒；manifest 内容为 null 或非对象时，deployment.format 会抛 TypeError；已修复：解析后先确认 manifest 是普通对象再读字段（哈希仍在主线程同步计算，未改）
- L477: FIXED[低] 性能: 所有接口共用 160MB 上限，请求体先整体缓冲，再 concat、toString、JSON.parse，峰值内存可达数倍；请求体为 "null" 或数字时，调用方 data.xxx 会抛 TypeError；已修复：解析结果必须是普通 JSON 对象，否则报 400（按接口分配上限未改）
- L769: FIXED[低] Bug: 缓存只按路径命中，从不检查 size/mtime。用户修改或替换 LUT 后，主线程仍沿用旧表（Worker 每次重新加载，两者结果不一致），必须重启才能生效；已修复：缓存项记录 size/mtimeMs，与原文件不一致时重新加载
- L1187: FIXED[低] 安全: dataFile/geometryFile 来自结果目录中的 JSON，允许 "../" 或绝对路径，读取时没有限制在元数据所在目录（删除逻辑里有 dirname 相同的检查）；字段 offset 也没有校验是否小于 recordFloats，越界时只会抛出不直观的 RangeError；已修复：字段 offset 增加 0 ≤ offset < recordFloats 校验并给出明确错误；路径未收窄（geometryFile 的默认值就是 ../faceteb.json，收窄会改变现有结果目录用法）
- L2014: FIXED[中] 错误处理: project.json 直接 writeFileSync 覆盖写入，没有采用 run-manifest 那样的“先写 .tmp 再 rename”；已修复：writeUnifiedProject 先写同目录 .tmp 再 renameSync 原子替换，失败时删除 .tmp（usd/sync.json 仍是直接覆盖写入）
- L2102: FIXED[低] Bug: 先改写全局 projectFile/projectDir，之后才 readProject/migrateProject。打开 STREAM 工程或迁移失败而抛错时，服务端已经指向新目录，GUI 却仍显示旧工程，后续导入、删除等相对路径会落到错误目录；已修复：先在局部变量里完成解析、读取与迁移，全部成功后才写全局变量（/api/run 中的先赋值后校验未改）
- L2583: FIXED[低] 资源泄漏: 写入失败（磁盘满、Worker 被 terminate）时 .partial 文件不会被删除，残留在结果目录；statistics CSV 也是直接覆盖写入；已修复：写入抛错时先 closeSync 再 unlinkSync 删除 .partial，CSV 改为先写 .tmp 再 renameSync（原子替换）
- L2636: FIXED[低] Bug: 转换时固定按小端、无 header offset、BSQ 排列读取数据，而 readEnvi 会处理 byte order、header offset 和 interleave。引擎若输出 BIL/BIP 或带头偏移的数据，生成的 TIFF 会静默错位，随后（体元模式下）原始 .img/.hdr 还会被删除，无法恢复；已修复：byte order/interleave/header offset 不是小端+BSQ+0 时直接返回 null，既不写 TIFF 也不删除源文件（未复用 readEnvi 的索引逻辑）
- L3111: FIXED[低] 性能: 整个 TIFF 用 readFileSync 读入，并在主线程逐像素遍历全分辨率（dem-info、results/read、change 都会走到这里）；inflateSync 没有设置 maxOutputLength，畸形或压缩炸弹文件可能耗尽内存；已修复：inflateSync 传入按分块几何尺寸推出的 maxOutputLength，解压上限不再无限（整文件读入与主线程解析未改）
- L3769: FIXED[低] 错误处理: spawn 返回的 ChildProcess 没有 'error' 监听，explorer 启动失败时 'error' 事件无人处理，会抛出未捕获异常，导致服务（Electron 主进程）崩溃；已修复：加 .on('error', () => {}) 并调用 .unref()
- L3870: FIXED[低] 安全: OBJ/气象/波谱导入只用 basename 处理文件名，没有像 DEM/纹理那样做字符白名单。Windows 上 "x.txt:s.obj" 这类名字会写入 NTFS 备用数据流，CON.obj 等保留名、末尾带点或空格的名字也会出错，而且同名文件被静默覆盖；已修复：OBJ/气象/波谱三处导入复用 DEM 导入的 stem 字符白名单（非法字符替换为 _），重名时自动加 _2 后缀，不再静默覆盖
- L3993: FIXED[低] 安全: pathWithin 只做字面比较，工程目录内的符号链接或 junction 可以指向任意位置的 PNG/JPG，从而读取范围外的文件；/api/project/read 用的是 realPathWithin，两处不一致；已修复：两处白名单都改用 realPathWithin（与 /api/project/read 一致）
- L4034: FIXED[低] 兼容性: 用字符串 !== 比较目录，Windows 路径不区分大小写，"Positions/x.txt" 或盘符大小写不同的合法路径会被误拒；扩展名也没有限制，positions 下任意文件都可能被覆盖；已修复：改用 sameResolvedPath 比较目录，并把扩展名限定为 .txt
- L4168: FIXED[中] 资源泄漏: spawn 之后还要先执行 writeRunManifest/emit，才注册 'close'/'error' 监听。只要其中任一步抛错（如磁盘满、目录被删），外层 catch 只会把清单标为 failed，引擎进程继续运行且无人回收，全局 child 永远不清空，之后再也无法运行；已修复：见本路由的 catch：对已 spawn 的引擎执行 taskkill/置空并复位 child/childExecutable，清单写入也包了 try/catch
- L4177: FIXED[低] Bug: 逐块 chunk.toString() 时，跨块的多字节 UTF-8 字符（中文日志）会被截断成乱码；已修复：先 setEncoding('utf8')，data 事件拿到的是已按字符边界解码的字符串
- L4222: FIXED[中] Bug: 'error' 处理中不清空 child/childExecutable。spawn 失败（如 EACCES、被杀软拦截）时不能保证一定会触发 'close'，此后所有 /api/run 都会报“已有模拟任务正在运行”；这里的 writeRunManifest 一旦抛错还会变成未捕获异常；已修复：spawn 失败（没有 pid）时复位 child/childExecutable，清单写入包 try/catch
- L4229: FIXED[中] 错误处理: close 回调中 unlinkSync（finally 内和下面第二处）、writeRunManifest、runProductPaths、emit 都没有保护，任何一个抛错都会成为 EventEmitter 内的未捕获异常，使整个服务（Electron 主进程）崩溃，child 也不会被清空；已修复：整个 close 处理体抽成函数并在 try/catch/finally 中调用，finally 一定复位 child/childExecutable，抛错只上报不再崩溃（非流式出图仍在主线程，未移到 Worker）
- L4282: 见上方 FIXED[中]（spawn 处）：本轮已启动的引擎在这里回收，避免 child/childExecutable 永不复位。
- L4298: FIXED[低] 错误处理: 原先只在 taskkill 无法启动时退回 kill，不检查退出码；已修复：与 STREAM 版一致，taskkill 退出码非 0 时也退回 child.kill()
- L4383: FIXED[中] 安全: 可以让资源管理器打开任意已存在的路径，包括 \\host\share 这样的 UNC 路径（会向远程主机发起 SMB/NTLM 认证）；已修复：spawn 加 error 监听与 unref，并拒绝 UNC / 设备路径（\\ 或 // 开头）
- L4412: FIXED[低] 兼容性: mime 按扩展名大小写敏感查找，且缺 .mjs/.wasm/.json/.ico/.woff2 等类型，.mjs 模块脚本会因 octet-stream 被浏览器拒绝执行；已修复：扩展名先 toLowerCase 再查表并补全这些类型，同时加 X-Content-Type-Options: nosniff（未加 CSP）
- L4439: FIXED[中] 资源泄漏: Windows 上 child.kill('SIGTERM') 只结束 histream.exe 本身，不结束其子进程树（/api/stop 用的是 taskkill /T /F，两处不一致）；而且 Electron 的 before-quit 不等待 'close'，本轮 run-manifest 会一直停在 running 状态；已修复：win32 下同步执行 taskkill /PID <pid> /T /F 结束进程树并置 histreamStopped（退出路径无法等到 close，run-manifest 的 stopped 状态仍未同步写入）

## server/stream/config.mjs

- L19: FIXED[低] Bug: Number(null) 与 Number('') 都是 0（有限值），旧工程 timezone 为 null/空串时按 UTC+0 而不是注释里的默认 +8 换算，过境时刻偏 8 小时；与 workspace.js 的 localToUtc 是重复实现；已修复：timezone 为 null/undefined/空白串或非有限数时取默认 +8，不再被 Number() 折成 0（与 workspace.js 的重复实现未合并）
- L113: FIXED[低] Bug: startsWith('..') 会把工程目录下以“..”开头的子目录（如 "..data\x.tif"）误判为在工程外而存成绝对路径；已修复：改为排除 inside === '..' 或以 '..' + sep 开头的路径
- L156: FIXED[低] Bug: 带宽输入框清空后 Number('') 得 0，结果静默变成单色波段；已修复：空串视为无效（缺省字段仍按 0 处理）
- L319: FIXED[中] Bug: Number('') 与 Number(null) 都是有限值 0，表单清空后 diffuseFraction 变成 0（而不是默认 0.2），spectralBinNm/referenceHeight/soilEmissivityK/soilResistance.value 变成 0 并导致引擎报“参数超出有效范围”；已修复：下列取值统一改用 hasNumber()，空串/空白/null 视为未提供，保持原默认值或省略该字段
- L369: FIXED[低] Bug: 输入框清空后为 ''，Number('') 得 0，起始行列被静默当作 0；已修复：空值视为无效

## server/stream/index.mjs

- L33: FIXED[低] 错误处理: 向已断开的 SSE 响应写入时没有 error 监听；已修复：SSE 响应注册 'error' → 从 clients 移除（避免写入已断开连接后无监听者抛未捕获异常）；心跳 ': ping' 未加
- L64: FIXED[低] 可维护性: 产品列表只排除 run-manifest/run-config，输入副本 scene.json 和 scene_summary.json 也会被当成“产品”写进 manifest 并返回前端；已修复：排除名单同时包含 scene.json / scene_summary.json
- L129: FIXED[中] 错误处理: spawn 失败（ENOENT/EACCES/ENAMETOOLONG）时这里只记日志，Node 不保证之后会触发 close，任务会一直占着 active，串行队列永久阻塞；已修复：error 回调同样调用 finish(job, 'failed', ...)，finish 用 job.finished 去重，避免与 close 重复结束
- L130: FIXED[中] 并发: 'error' 在进程仍存活时也可能触发（例如 kill 失败），此时直接 finish 会让队列启动下一个引擎、旧进程成为孤儿；已修复：只有 spawn 本身失败（没有 pid）时才在 error 里结束任务，其余情况等 'close'
- L143: FIXED[低] Bug: id 缺省（undefined/null）时会停止当前任意任务（包括 ERA5 登记）；传入非数字 id（如前端的 'pending'）时 Number() 得到 NaN，静默返回 stopped:false；已修复：id 非整数时抛错（路由层转成 400），不再退化成“停任意任务”
- L155: FIXED[低] 错误处理: 只处理 taskkill 启动失败，不检查它的退出码；已修复：监听 taskkill 的 close，退出码非 0 时退回 child.kill()
- L273: FIXED[低] 资源泄漏: 任务失败或被停止时，引擎可能已写出不完整的 pack-*.json，这里不清理，之后会出现在列表中；已修复：status 非 completed 时删除该索引文件
- L383: FIXED[低] 资源泄漏: 退出时只 kill 引擎主进程，Windows 下不像 stop() 那样用 taskkill /T 结束整个进程树；已修复：win32 下同步执行 taskkill /T /F 结束进程树（ERA5 未完成索引的清理未加）

## server/stream/rasters.mjs

- L54: FIXED[中] Bug: 坐标原点直接取 tie[3]/tie[4]，忽略了 tie[0]/tie[1]（对应的栅格行列不一定是 0,0）；已修复：originX = tie[3] - tie[0]*scaleX、originY = tie[4] + tie[1]*scaleY（PixelIsPoint 半像元约定与 GeoAscii 引文回退未改，仍需与 readTiff 统一）
- L87: FIXED[低] Bug: readTiff 会把越界的波段序号静默钳到最后一个波段，前端按请求的 band 缓存，结果会显示成别的波段而没有报错；已修复：读回后用 tiff.bands 校验请求的 band，越界直接报错（nodata 的 float32 容差比较仍在 readTiff 中，未改）

## src/renderer/src/i18n.js

- L989: FIXED[低] 内存泄漏: click 与 languagechange 监听器每次调用都会重复注册（observer 有防重入守卫，监听器没有），重复调用后单击会切换两次语言而看似无效；已修复：与 observer 一样加 listenersBound 一次性守卫，重复调用只重新本地化

## src/renderer/src/main.js

- L1177: FIXED[中|内存泄漏]: 只释放 material.map，normalMap/alphaMap/emissiveMap/roughnessMap 等其它贴图及 uniforms 中的纹理不会 dispose，反复重建场景会累积 GPU 纹理；已修复：遍历材质自身的全部属性与 uniforms，凡 isTexture 的统一 dispose（同一纹理多次 dispose 是幂等的，未做引用计数）
- L2749: FIXED[中|错误处理]: 本函数和 chooseXml 都不检查 state.running，运行中切换工程会改写 outputDir/inputPath，运行结束事件落到新工程上；已修复：运行中拒绝切换工程（沿用“打开示例”的同一提示）
- L2770: FIXED[中|错误处理]: loadActualScene(config) 未 await 也无 catch（saveXml 的 replaced 分支、applyXmlBtn 处理器同样），OBJ 读取/解析失败成为未处理 rejection，场景对象静默缺失；已修复：改用自带 catch 的 restoreActualScene()（saveXml 的 replaced 分支与 applyXmlBtn 处理器同样替换）；运行中拒绝切换工程已在上方 loadXml 入口加入
- L2860: FIXED[中|并发]: 与 importDem 同类问题——上传期间若切换了工程，下面会把气象文件写进新工程的 state.config 并立即 saveXml，而文件实际导入在旧工程目录；已修复：await 前缓存 state.inputPath，返回后不一致则中止并提示
- L2898: FIXED[中|并发]: 读取 64MB 文件并上传期间若切换了工程，下面会把 DEM 写进新工程的 state.config 并立即 saveXml，而文件实际导入在旧工程目录（importMeteo 同样问题）；已修复：await 前缓存 state.inputPath，返回后不一致则中止并提示
- L3445: FIXED[低|错误处理]: JSON.parse 位于 try 之外，data-classes 损坏时整个函数 reject，容器停留在旧内容且调用方多为未 await 的 fire-and-forget，异常无人处理；已修复：本地 try/catch 解析，损坏时回退为 []
- L4184: FIXED[中|错误处理]: api.chooseObj() 在 try 之外，读取/选择失败会变成未处理的 Promise rejection，用户无任何提示；另外若已有 pendingObject 未释放就被 preparePendingObject 覆盖，旧 Object3D 的几何/材质泄漏；已修复：把 await 放进 try/catch 并 toast 提示，覆盖前先 disposeObject(state.pendingObject.object)
- L4203: FIXED[中|Bug]: 各物化参数直接 Number(values.xxx)：空输入框得到 0（如 Vcmax、cs、rhos、lambdas 变 0），非数字得到 NaN；Wood 分支 Math.max(1, NaN) 仍为 NaN，会原样写进 project.json 并交给引擎；已修复：返回前过滤掉 params 中非有限的数字项（缺失/非法即跳过，不引入新的默认值），NaN 不再写进工程；空串被当成 0 的字段（未包 Math.max 的 Vcmax/SMC 等）行为未改，见下方说明
- L4542: FIXED[低|错误处理]: 新建的光谱/温度/物化预设在此直接 upsert 进 state.config，后续校验或保存失败时 catch 只回滚 objects.items，预设库残留半成品条目（同名时还会覆盖已有预设）；已修复：upsert 前对三个预设数组做浅快照（upsert 是替换/追加元素，不原地改元素），catch 中一并恢复
- L4845: FIXED[低|错误处理]: 先原地修改 item（name/distribution/movement/instanceCount）再 saveXml，保存失败时 catch 不回滚，界面与磁盘上的 project.json 不一致；另外传给 saveDistribution 的是未 trim 的名称，与 item.name=nextName 不一致；已修复：进函数时留存 item 快照，catch 中删除新增键并还原字段；saveDistribution 统一用 nextName
- L5290: FIXED[中|错误处理]: 切换波段/彩色模式时由事件监听直接调用本函数且不 await、不 catch；任一 api.readResult 失败都会成为未处理 rejection，此时 3D 视图已被 dispose，预览区停留在旧画面而无错误提示；已修复：Promise.all 包 try/catch，失败时（且 previewRequest 仍为最新）先 disposeResultViewer，再用 emptyResultPreview + toast + addLog 提示
- L5437: FIXED[低|并发]: 本函数不递增 state.resultPreviewRequest，只比较文件路径；同一文件的 drawEnviPreview 尚在等待 readResult 时切到图像预览，旧请求返回后会覆盖当前画面；已修复：开头同样 ++state.resultPreviewRequest，await 后比对 request 与路径
- L5561: FIXED[低|错误处理]: 返回 2D 时 drawImagePreview/drawTiffPreview 返回的 Promise 没有 catch，解码失败成为未处理 rejection 且预览区停留在空 <img>；sourceResult 为 null 时 drawImagePreview(null) 会因 result.mimeType 抛 TypeError；已修复：sourceResult 与 rasterResult 都为空时直接返回，绘制用 async 处理器 await 并 try/catch，失败时先 disposeResultViewer 再用 emptyResultPreview + addLog 提示
- L5733: FIXED[中|Bug]: 与 showSoilProfile3D 的做法对齐——JSON 中的 NaN/NoData 序列化为 null，Number(null) === 0 会被当成有效值参与 min/max；已修复：非数字（含 null）不再被 Number() 强转为 0，按无效值处理（下方着色循环同样处理；showFacet3D 未改）
- L5838: FIXED[中|内存泄漏]: 此处在 disposeResultViewer() 之前就用 emptyResultPreview 覆盖 innerHTML，旧 resultViewer 的 WebGL 上下文/几何/ResizeObserver 未释放，且 animate 中 renderResultViewer 仍对已脱离 DOM 的画布持续渲染；已修复：把 disposeResultViewer() 提到校验之前，空结果直接释放旧视图
- L7062: FIXED[低|错误处理]: 本行中停止按钮 () => api.stop() 的 Promise 没有 catch，停止失败（网络/进程已退出）时无提示且为未处理 rejection；已修复：补 .catch 并 toast + addLog 提示

## src/renderer/src/material-specular.js

- L45: FIXED[低] Bug: 输入框清空时值为 ''，?? 不会回退到 previous，finite('') 再回退到 0.5/0.25 默认值，用户原有参数被重置；已修复：空字符串时改用 previous 的对应值，仅真正无输入时才回退默认值

## src/renderer/src/menubar.js

- L67: FIXED[低] Bug: 全局捕获 Ctrl+N/Ctrl+O 并 stopImmediatePropagation，模态对话框打开或 project.json 源码编辑器聚焦时也会触发新建/打开；已修复：原生 dialog[open]、应用内 .drawer-backdrop 弹层打开或焦点在可编辑元素时直接跳过

## src/renderer/src/project-schema.js

- L161: FIXED[中] 数据一致性: 只要 cs/specificHeat 缺失或非数字，下方就把 params 整体替换为预设值，用户填写的 rhos、lambdas、Tsoil、convectiveScale 等全部丢失；另外 Number(null)===0 使 cs:null 被判为“有参数”；已修复：params 始终 { ...预设, ...用户 } 逐字段合并，仅 cs/specificHeat 为 null/空白/非数字时单独回退预设 cs
- L246: FIXED[低] 错误处理: sunAzimuth 为非数字字符串/NaN 时 Number() 得 NaN，所有主平面角度的 VAA 都变成 NaN 并写给引擎（默认参数只覆盖 undefined）；已修复：改用 number(sunAzimuth, 0)，非数字回退 0
- L583: 入口深拷贝，避免归一化结果与输入 value 共享嵌套引用（对应下方 FIXED[低] 可维护性）。
- L664: FIXED[低] 错误处理: items 中若有 null/非对象条目（手改 JSON 或旧版残留），item.materialName 抛 TypeError，整个工程无法打开；mesh 为 null 时同样崩溃（canopies 映射同理）；已修复：map 前过滤非对象条目，meshes 数组同样过滤非对象网格
- L727: FIXED[低] 可维护性: 结果只做了一层浅拷贝，objects.items[*] 的 placements/position/dimensions（非 Fire/Fog）、materials[*].params（非 wood）、thermals 等嵌套对象与输入 value 共享引用，调用方修改归一化结果会同时改动原 state.project（反之亦然）；已修复：normalizeProject 入口对 value 执行 structuredClone（与 main.js/workspace.js 既有用法一致）
- L1017: FIXED[低] Bug: 回退值 name 已经过 xmlEscape，下方 <meshNames> 再次 xmlEscape，对象名含 & < " 时被双重转义（&amp;amp;），与 objName 不一致导致网格匹配失败；另外网格名/光谱名含逗号时，逗号拼接的列表会错位；已修复：回退改用未转义的 rawName；名称含逗号问题仍待名称校验策略确认
- L1055: FIXED[中] Bug: 下一行 item.reflectance || '0.20' 把数值 0（如黑体/吸收面，或从数值输入框保存的 0）当作缺省值，导出为 0.20 反射率；已修复：改用显式缺失判断（仅 null/undefined/空白字符串回退），数值 0 原样保留
- L1078: FIXED[中] 安全: 光伏分支用 params 的任意键直接拼成 XML 标签名，键含空格、<、> 或 "/x><y" 时生成非法 XML 或注入额外元素；已修复：输出前用 /^[A-Za-z_][\w.-]*$/ 校验标签名，非法键跳过

## src/renderer/src/project-unified.js

- L31: FIXED[中] 数据一致性: Number(null)/Number('')/Number(false) 都等于 0，被当作有效值而不回退；纬度/经度/时区、观测角、天顶角等为 null 或空串时会变成 0（例如位置跑到 0°N 0°E、时区变成 UTC），与 project-schema.js 的 number() 行为不一致；已修复：先按 number() 口径把 null/undefined/空白字符串视为缺失并回退，再解析数值
- L186: FIXED[低] 可维护性: source 中的 spectra/canopies/thermals/materials 数组以及 scene.background、objects.items 等嵌套对象按引用并入结果，未 clone（默认值一侧却 clone 了）；调用方修改归一化结果会回写到输入对象；已修复：入口对 source 做 clone()，后续合并全部基于副本
- L246: FIXED[低] 可维护性: fluid/objects 直接引用旧工程对象（objects.items 及其 placements 共享），scene/sensor/control 只是浅拷贝（scene.background 共享），而 spectra 等却做了深拷贝；统一工程被修改时会连带改动仍在内存中的 HiStream 工程；已修复：入口改为 clone(configuration)，fluid/objects/scene 等均基于副本
- L279: FIXED[低] 数据一致性: year 缺省 2024，与 project-schema 默认 2019、normalizeUnified 使用的 configuration.meteo.year 不一致；下方 cpuThreads 也缺少 1024 上限（normalizeUnified 有）；已修复：cpuThreads 增加 1024 上限；year 默认数值属禁止修改项，未改动
- L282: FIXED[中] 数据一致性: 每次从 HiStream 保存都强制 mode: 'manual' 并丢弃 previousShared.sky，STREAM 侧设置的 'from-time'（按过境时刻计算太阳位置）被静默覆盖；已修复：先展开 previousShared.sky 并保留其 mode（仅合法值），再覆盖角度字段
- L437: FIXED[低] 数据一致性: componentsPackage 位于 temperature 上而不是 components 上，这里的 delete 删错对象；下方 ...temperature 仍把 temperature.componentsPackage 原样带入结果并持久化到统一工程，与 components.file 形成两份可能不一致的路径；已修复：从 temperature 解构中剔除 componentsPackage（不再 delete 无用的 components.componentsPackage），只保留归一化后的 components.file
- L526: FIXED[低] 数据一致性: rasterField 只保留 file/band，engine.surface/view 中栅格字段的其它属性（如 scale、nodata 或将来新增的键）在打开统一工程时被丢弃，下次保存即永久丢失；已修复：先展开 field 再规范化 file/band（band 为 0 时删除该键，其余属性保留）

## src/renderer/src/scene-loader.js

- L789: FIXED[中] 性能: 整个 OBJ 以文本读入后在主线程同步 parse，没有体积上限也没有按 fileName 缓存；大模型会长时间阻塞 UI，多个对象引用同一 OBJ 时重复读取解析；已修复：单次加载内按 fileName 缓存解析结果，复用时 clone 并克隆各 Mesh 的 geometry（UV 处理会 dispose 几何体，直接共享会破坏缓存）；体积上限与 Worker 化仍 DEFERRED

## src/renderer/src/storage.js

- L16: FIXED[中] 错误处理: setItem 未捕获 QuotaExceededError / SecurityError 以及桌面 IPC 异常（preload 在失败时 throw），调用方（保存工程目录、切换语言）会直接中断；已修复：try/catch 包住两条写入路径，失败时 console.warn 告警并返回 false，成功返回 true，调用方不再中断

## src/renderer/src/stream/api.js

- L3: FIXED[低] 错误处理: fetch 没有超时/AbortSignal，后端卡住（如 pick 对话框或同步读取大 TIFF）时调用方会一直等待，界面无法取消；网络错误时抛出原始 TypeError “Failed to fetch”，不是中文提示；已修复：fetch 包 try/catch，网络失败转换为可读中文错误（调用方传入的 options.signal 原本就会透传给 fetch，可用于取消）；未加默认超时，避免引入新的超时数值常量

## src/renderer/src/stream/workspace.js

- L153: FIXED[低] Bug: 与 server/stream/config.mjs 的 localToUtc 重复实现，问题也相同：timezone 为 null/'' 时 Number() 得 0，按 UTC+0 而不是默认 +8 换算；已修复：先判空（null/undefined/空白）再取默认 +8，不再被 Number() 当成 0；两处重复实现合并成共享模块属跨文件重构，未做
- L352: FIXED[低] 性能: 最大约 22 MB 的 base64 用 atob + 逐字节 charCodeAt 解码，再加上 JSON 解析，切换波段时主线程会卡顿；字节数不是 4 的倍数时 Float32Array 构造会抛错；已修复：按 4 字节向下取整后再构造 Float32Array，尾字节丢弃，不再抛 RangeError；改用 octet-stream + arrayBuffer() 需要改服务端接口，未做
- L826: FIXED[低] 并发: 防抖触发的 inspect 和 startRun 中 await 的 inspect 可能并发，返回顺序不保证，较早请求的结果会覆盖较新的 inspections，validate() 可能基于过期的文件状态放行或拦截；已修复：照 view.token 的做法加递增 token，响应回来时 token 不是最新就丢弃
- L957: FIXED[低] 可维护性: 页面中没有 data-path="outputRoot" 输入框和 data-browse-folder 按钮（refreshStatus 里也有同样的死代码）；如果以后加了文件夹按钮而没加输入框，input 为 null 会抛 TypeError；已修复：取 input 时给 row 加可选链并判空后直接返回，不再抛 TypeError（死分支本身保留，删除属清理）
- L1257: FIXED[中] Bug: 断线期间任务已结束、job-closed 丢失时 currentJob 不会清空，运行卡片一直停在运行中；
- L1769: FIXED[中] Bug: layer.crs 来自 server.mjs 的 readTiff，它只解析投影 EPSG（3072），经纬度栅格得到的是 GeoAscii 引文或“经纬度坐标”，从不会是 "EPSG:4326"；isGeographic 判断失败，于是把度数当成“X / Y … m”显示，而且不显示经纬度；另外南纬、西经会显示成负的 °N/°E；已修复：改用 layer.unit === '°' 或 EPSG:4326 判定经纬度栅格（并回退到 [y, x]），经纬度按符号显示 N/S、E/W，经纬度栅格不再输出带 m 的 X/Y 行
- L1819: FIXED[低] 资源泄漏: document/window 上的 fullscreenchange、keydown、beforeunload 监听器和 ResizeObserver 都没有保存引用，也不会解除；目前 bind 只调用一次，问题不大，但工作台如果被重建或销毁（root.innerHTML 重写时 rendered 复位）就会重复绑定，F5 会触发多次 startRun；已修复：这三处全局监听器用 globalBound 守卫，只在首次 bind 注册；元素级监听器仍每次 bind 都注册（重建后新 DOM 不会丢监听）；工作台没有销毁路径，AbortController + dispose() 的全量解绑未加
- L1859: FIXED[低] Bug: Number('') 为 0，清空某个输入框会被当作 0（°C 模式下是 273.15 K）而不是报错；已修复：先判断 trim() === '' 得到 NaN，交给下面已有的“拉伸范围无效”提示拦截，不再退回成 0
- L1883: FIXED[低] 资源泄漏: document/window 上的 fullscreenchange、keydown、beforeunload 监听器和 ResizeObserver 都没有保存引用，也不会解除…；已修复：这三处全局监听器改为仅首次 bind 注册（globalBound 守卫，详见 bind() 开头说明），重建工作台不会重复绑定，F5 不会再触发多次 startRun
- L1935: FIXED[中] 错误处理: EventSource 断线自动重连后，服务端只补发一条 status 事件，handleEvent 直接忽略了它；断线期间的 job-closed 丢失时，界面会永远停在“运行中”（停止按钮也无效）；已修复：handleEvent 收到 status 事件时调用 refreshStatus，按服务端 running/queue 重新同步 currentJob 与运行/停止按钮状态
- L2001: FIXED[中] 错误处理(对应 show() 中 EventSource 重连的 REVIEW)：status 事件原本被直接 return 丢弃；已修复：已知过一次状态后再收到 status（断线重连时服务端补发的那条）就调用 refreshStatus 重新同步，避免界面永远停在“运行中”

## src/renderer/src/web-api.js

- L24: FIXED[低] 错误处理: file.text() 失败（文件被占用/过大）时 async 回调抛出的异常不会传给外层 Promise，chooseFile 永远不 resolve，调用方一直挂起；oncancel 在旧版 Chromium 中不触发，也会一直挂起；已修复：onchange 内 try/catch，读取失败 resolve(null) 并 console.warn，Promise 任意路径都会结算，不再挂起
- L47: FIXED[中] 兼容性: Electron 不支持 window.prompt（抛错或返回 undefined），原先 value.trim() 会抛 TypeError；已修复：捕获异常并把 undefined 视同取消
