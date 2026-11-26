# Stream Studio 运行原理说明

## 0. 本文定位与阅读方法

### 0.1 三类文档的分工

Stream Studio 的技术文档分为三类，各管一件事，互不重复：

| 文档 | 回答的问题 | 典型读者 |
| --- | --- | --- |
| 《理论与方法》（docs/theory-manual.md） | 模型算什么：辐射传输方程、能量平衡方程、反演与同化的数学表达 | 做科学判断、写论文的人 |
| 《使用手册》（docs/user-manual.md） | 怎么用：界面、参数含义、推荐流程 | 操作软件的人 |
| 《运行原理说明》（本文） | 程序实际怎么跑：进程、数据流、状态、文件、退出码、限制 | 复现结果、排查问题、修改或扩展代码的人 |

本文只描述**当前源码实际执行的行为**，不重复推导公式；物理定义以《理论与方法》为准，界面操作以《使用手册》为准。三者出现分歧时，**以本文所述的代码行为为运行事实**，分歧处记在第 7 章。

### 0.2 作为运行依据时的使用规则

1. 每个关键事实后括号内的 `文件:行号` 是证据位置，行号随代码演进会漂移；核对时以函数名和文件为准，行号只作定位线索。
2. “待核实”表示撰写时没有读完相关代码或没有实测，**不得当作结论引用**。全部待核实项在 7.5 汇总。
3. 凡写“据代码推得”的内容，是读代码后的推论，没有运行验证。
4. 本文对应的代码快照：软件版本 2.0.1，提交 `d6eefc7`（2026-11-26，含当日审阅修复）。代码变更后，应同步更新相应章节，并重新生成 PDF（`python tools/build-core-manuals-pdf.py runtime`）。

### 0.3 全文结构

| 章 | 内容 | 主要依据文件 |
| --- | --- | --- |
| 1 | 平台与进程架构：桌面/Web 两种形态、本地 HTTP API、后台线程、目录与环境变量、构建与部署 | desktop/、server.mjs、server/task-pool.mjs |
| 2 | 三维工程数据模型与一次运行的完整生命周期 | project-schema.js、server.mjs |
| 3 | HiStream 引擎核心：启动、场景构建、RayTracing 与 VoxelRT | models/histream/src/base、raytracing、voxelrt |
| 4 | FacetRT、FacetEB、VoxelEB 的求解流程与耦合迭代 | models/histream/src/facetrt、voxeleb |
| 5 | STREAM 影像级引擎：配置、强迫数据、运行流程 | models/stream、server/stream |
| 6 | 结果数据、前端运行机制、质量保障与检查清单 | server.mjs、src/renderer |
| 7 | 运行判据、文档与代码差异、已知缺陷与待核实项 | 汇总 |
| 附录 A | 代码模块索引 | 汇总 |

### 0.4 系统全景

#### 0.4.1 两个相互独立的引擎

Stream Studio 集成两个引擎，输入、输出与格式互不混用：

| 引擎 | 可执行文件 | 面向对象 | 工程格式 | 驱动路径 |
| --- | --- | --- | --- | --- |
| HiStream（三维高分辨率） | `histream.exe` | 三维场景（面元/体元）的光谱辐射与能量平衡 | `stream-studio-project`（磁盘），运行时转为 `histream-project` | `/api/run`（server.mjs） |
| STREAM（影像级） | `stream.exe` | 遥感像元尺度的辐射传输与能量平衡，含同化与反演 | `stream-image-project` v2 | `/api/stream/*`（server/stream） |

#### 0.4.2 一次运行的数据流（总览）

```
界面（浏览器 / Electron 窗口）
   │  JSON over HTTP（127.0.0.1），进度走 SSE
   ▼
本地服务 server.mjs（Node）
   │  ① 校验工程（validateProject）与引擎白名单
   │  ② 建立隔离运行目录 + run-manifest.json
   │  ③ 生成运行时场景（实例筛选、OBJ 预裁切、DEM）
   │  ④ spawn 引擎进程，读 stdout 的 PROGRESS/OBSERVATION 行
   ▼
引擎进程 histream.exe / stream.exe（Vulkan 计算）
   │  写出 TIFF / CSV / JSON / 二进制 sidecar
   ▼
服务端后处理（格式转换、质量警告、manifest 终态）
   │  SSE 推送 closed 事件
   ▼
界面读取结果（/api/results/*），经缓存与后台线程解码后显示
```

#### 0.4.3 五种三维计算模式与两种影像模式

| 模式 | 引擎 | 一句话 | 详述 |
| --- | --- | --- | --- |
| RayTracing | HiStream | 基于网格（OBJ）的 Vulkan 光线追踪成像；唯一支持镜面反射表面的模式 | 第 3 章 |
| VoxelRT | HiStream | 体元辐射传输成像（含光学与热红外） | 第 3 章 |
| FacetRT | HiStream | 面元辐射传输，无温度演化 | 第 4 章 |
| FacetEB | HiStream | 面元辐射传输 + 能量平衡，逐气象节点 | 第 4 章 |
| VoxelEB | HiStream | 体元辐射传输 + 能量平衡，逐气象节点 | 第 4 章 |
| STREAM RT | STREAM | 影像级辐射传输（组分温度为输入） | 第 5 章 |
| STREAM EB | STREAM | 影像级能量平衡（气象强迫驱动） | 第 5 章 |

### 0.5 全局约定

| 项 | 约定 | 备注 |
| --- | --- | --- |
| 三维坐标 | +X 北、+Y 上、+Z 东；工程与位置文件中的 (x, y, z) 为（北、东、高） | models/histream/src/base/scene.cpp:22-25；工程 `scene.y` 对应运行时 Z 轴 |
| 温度 | 对外接口多为 K；体元土壤历史缓冲以 ℃ 存储，着色器内部有换算 | 具体见第 4 章 |
| 波长 | nm；热红外与光学分界为 2.5 μm | 第 3 章 |
| 路径 | 引擎侧经窄字符接口读取，**中文/非 ANSI 路径可能打开失败** | 第 3、7 章；运行用例请放在全英文可写路径 |
| 运行目录 | 每次运行独立目录，输入冻结，输出互不覆盖 | 第 2 章 |
| 本地 API | 仅监听本机，校验 Host/Origin/Sec-Fetch-Site/Content-Type；**无会话令牌** | 第 1 章 |

## 1. 平台与进程架构

本章描述 Stream Studio 2.0.1（package.json:3）在运行时由哪些进程、线程和目录构成，HTTP 接口如何分类与校验，后台线程池与结果缓存如何工作，以及构建产物如何落到磁盘。物理方程见《理论手册》，界面操作见《用户手册》，本章只讲“程序实际如何运行”。除非特别注明，“桌面模式”指 Electron 安装版，“Web 模式”指 `npm start`（`node server.mjs`）加浏览器访问。文中括号内为仓库相对路径与行号；无法由当前代码确认的内容标注“待核实”。

### 1.1 总体结构与两种运行形态

Stream Studio 由四类部件组成：

1. **界面（渲染层）**：源码 `src/renderer`，由 Vite 构建到 `gui/`（vite.config.js:19-20,35）。界面只通过 HTTP `/api/*` 与 SSE 事件流和服务端通信（src/renderer/src/web-api.js:74-126）。
2. **Node 服务端**：`server.mjs` 单文件加 `server/*.mjs`、`server/stream/*.mjs`。它既是 HTTP 服务、SSE 广播源，也是 HiStream 引擎子进程的调度者，同时又被自己加载为后台工作线程脚本（server.mjs:5596-5643）。
3. **计算引擎（原生子进程）**：`histream.exe`（三维 HiStream）与 `stream.exe`（STREAM 影像级引擎），均由服务端 `spawn` 启动（server.mjs:5076；server/stream/index.mjs:212）。同一时刻最多各有一个引擎进程：HiStream 由全局变量 `child` 互斥（server.mjs:136,4968-4970），STREAM 由串行队列 `active` 互斥（server/stream/index.mjs:92,173-175）。
4. **桌面外壳（仅桌面模式）**：Electron 主进程 `desktop/main.mjs`、预加载脚本 `desktop/preload.cjs`、服务进程入口 `desktop/server-process.mjs`、监听辅助 `desktop/local-server.mjs`、设置存储 `desktop/settings.mjs`。

两种形态的进程树如下：

```
桌面模式 (StreamStudio.exe)
Electron 主进程 (desktop/main.mjs)            窗口 / 对话框 / 设置 IPC / 进程监护
 ├─ BrowserWindow 渲染进程 (sandbox, contextIsolation)
 │      └─ 载入 http://127.0.0.1:<随机端口>/  ← 回环 HTTP
 └─ utilityProcess "Stream Studio Server" (desktop/server-process.mjs)
        └─ import ../server.mjs  → http.Server 监听 127.0.0.1:0
              ├─ worker_threads: task-service ×N (常驻线程池，默认 N=2)
              ├─ worker_threads: task-once / facet-step / facet-step-service (按需)
              └─ 子进程: histream.exe / stream.exe / explorer.exe / powershell.exe

Web 模式 (node server.mjs)
node 进程 (server.mjs)  监听 127.0.0.1:4173 (默认)
 ├─ 同上的 worker_threads
 └─ 同上的引擎子进程
浏览器 ──HTTP──> 127.0.0.1:4173

开发模式 (node dev.mjs)
dev.mjs ─ spawn → node server.mjs --api-only   (仅 /api，4173)
        └ spawn → vite (127.0.0.1:5173, /api 代理到 127.0.0.1:4173)
```
（dev.mjs:25,27；vite.config.js:26-31）

桌面与 Web 的关键差异汇总：

| 项目 | 桌面模式 | Web 模式 |
| --- | --- | --- |
| 启动入口 | package.json `main` 指向 `desktop/main.mjs`（package.json:8），`npm run desktop` 即 `electron .`（package.json:19） | `npm start` = `node server.mjs`（package.json:15） |
| 服务端所在进程 | 独立 utilityProcess，主进程不被计算阻塞（desktop/main.mjs:34-35,223-225） | 与 node 主进程同一进程 |
| 监听端口 | `listen(0,'127.0.0.1')`，系统分配随机端口（desktop/local-server.mjs:31） | 默认 4173，`STREAM_STUDIO_PORT` 可改（server.mjs:5602,5611） |
| 自动监听 | `STREAM_STUDIO_SERVER_AUTOSTART=0`，由 `server-process.mjs` 控制（desktop/main.mjs:195） | 未设置则 `server.mjs` 自行监听（server.mjs:5603） |
| 资源根 RESOURCE_ROOT | `process.resourcesPath`（打包）或源码根（开发）（desktop/main.mjs:313-315） | 缺省为源码根，可由环境变量覆盖（server.mjs:71） |
| 选择工程文件对话框 | 主进程 `dialog.showOpenDialog`（desktop/main.mjs:202-215），经 IPC 代理 | 服务端用 PowerShell WinForms 对话框（server.mjs:2608-2630），对话框出现在服务端所在机器 |
| 界面偏好存储 | `%APPDATA%\Stream Studio\renderer-settings.json`（desktop/settings.mjs:33） | 浏览器 `localStorage`（src/renderer/src/storage.js:19,31） |
| 自定义引擎路径输入 | 依赖 `window.prompt`，Electron 窗口不支持时抛错，要求改用浏览器模式（src/renderer/src/web-api.js:80-83） | 可用（同上） |
| 单实例 | `requestSingleInstanceLock`，第二实例仅前置窗口（desktop/main.mjs:83-94） | 无；端口被占用即启动失败（server.mjs:5604-5610） |
| 崩溃恢复 | 服务进程意外退出自动重启（1.3.5） | 进程退出即服务停止，无守护 |
| 退出 | 先请求服务进程优雅停止，5 s 后强杀进程树（desktop/main.mjs:105-115,167-178） | 收到 SIGINT/SIGTERM 时停引擎并 `server.close`（server.mjs:5665-5672） |

### 1.2 线程角色

`server.mjs` 同时是主脚本和工作线程脚本。模块顶层代码在每个线程中都会执行（创建 `http.Server`、`streamService`、线程池对象等，但只有主线程监听端口），随后按 `workerData.kind` 分支（server.mjs:5603-5643）：

| 角色（kind） | 创建者 | 生命周期 | 作用 |
| --- | --- | --- | --- |
| 主线程 | 进程入口 | 与服务同寿 | 监听、路由、SSE、引擎子进程管理（仅当 `isMainThread` 且未禁用自启，server.mjs:5603） |
| `task-service` | `createTaskPool` 的 `createWorker`（server.mjs:161-167） | 常驻池，懒创建，空闲 5 分钟回收 | 执行 `HEAVY_TASKS`；`resultCache.limit` 设为 `cacheBytes`（server.mjs:5636-5639） |
| `task-once` | `runPostprocessTask`（server.mjs:169-181） | 一次性，任务结束并退出后才返回 | 运行结束后的面元出图 `facet-tiff` 与 ENVI 转换 `envi-convert`；缓存上限 0（server.mjs:5640-5643） |
| `facet-step-service` | `exportFacetStep`（server.mjs:2430-2459） | 每次 FacetEB 运行一个，几何只解析一次 | 逐节点出图，结果文件的大小/mtime/旁路 `.bin` 未变则复用已解析几何（server.mjs:5618-5635） |
| `facet-step` | `exportFacetStepOnce`（server.mjs:2408-2423） | 每节点一个，退出后才放行引擎 | 面元数超过阈值时的逐节点出图（server.mjs:5613-5616） |

`facet-step-service` 与 `facet-step` 的选择阈值为 `STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS`，缺省 3,000,000 个面元（server.mjs:2396,2431）。超过阈值时每个节点单独启线程，以便线程退出后释放整块堆再让引擎继续（server.mjs:2390-2395）。`execArgv` 会过滤掉 `--input-type` 开头的参数（server.mjs:2402）。代码中没有为工作线程设置 `resourceLimits`（server.mjs:2399-2404），因此堆上限取 Node 默认值。

### 1.3 桌面模式的进程管理

#### 1.3.1 启动顺序

```
app 启动
 ├─ app.setName('Stream Studio')；userData = %APPDATA%\Stream Studio（有 APPDATA 时）
 ├─ requestSingleInstanceLock  失败 → app.quit()
 ├─ migrateLegacyUserData()   旧 %APPDATA%\HiStream Studio → 新目录（仅一次）
 └─ whenReady → createWindow():
      1. root = resourceRoot()；dataRoot = %LOCALAPPDATA%\StreamStudio（无则回退 userData）
      2. startServerProcess(serverEnvironment(root,dataRoot))   ≤30 s 等 listening
      3. waitForServer(url)  每 150 ms GET /api/defaults，≤15 s
      4. 注册设置 IPC；计算 16:9 窗口；创建 BrowserWindow
      5. mainWindow.loadURL(url)；ready-to-show 后显示
```
（desktop/main.mjs:42-51,83-101,338-416）

用户数据目录必须在 `app.ready` 之前设置（desktop/main.mjs:44-51）。旧版配置迁移仅当新目录没有 `Local State` 且旧目录有时才执行：先复制到 `<userData>.migrating` 暂存目录，再复制到新目录（`force:false`），无论成败都删除暂存目录，失败只记录日志不阻止启动；缓存目录与 `Singleton*`、`lockfile` 不迁移（desktop/main.mjs:54-80）。

#### 1.3.2 服务进程环境

`serverEnvironment(root, dataRoot)` 复制当前环境后删除 `ELECTRON_RUN_AS_NODE`，把 `<root>/engine` 放到 PATH 最前（Windows 下按大小写不敏感查找实际的 PATH 键名，分隔符固定写 `;`），并设置（desktop/main.mjs:184-198）：

| 变量 | 值 |
| --- | --- |
| `STREAM_STUDIO_RESOURCE_ROOT` | `root`（打包后为 `process.resourcesPath`） |
| `STREAM_STUDIO_DATA_ROOT` | `%LOCALAPPDATA%\StreamStudio`，缺省为 userData |
| `STREAM_STUDIO_SERVER_AUTOSTART` | `0` |
| `HISTREAM_ROOT` | `<root>\engine` |

`HISTREAM_ROOT` 在 JavaScript 代码和 `models/` 下的 C++ 源码（`getenv` 检索结果）中均没有读取点，其实际效果待核实。`STREAM_STUDIO_DATA_ROOT` 在服务端只有一个读取点：`/api/open` 未给出路径时的回退目录（server.mjs:5451）。

#### 1.3.3 消息协议

主进程与服务进程用 `process.parentPort` 通信（desktop/server-process.mjs:15-20）。

| 方向 | 消息 | 含义 |
| --- | --- | --- |
| 子→主 | `{type:'listening', url, pid}` | 监听成功，`url` 形如 `http://127.0.0.1:50321`，`pid` 为服务进程号（desktop/server-process.mjs:73） |
| 子→主 | `{type:'startup-error', message}` | 动态导入或监听失败，100 ms 后以退出码 1 结束（desktop/server-process.mjs:74-78） |
| 子→主 | `{type:'choose-project-file', id, currentProjectFile}` | 请求弹出“打开工程”对话框；`id` 自增，与回复配对（desktop/server-process.mjs:66-70） |
| 主→子 | `{type:'choose-project-file-result', id, path, error}` | 对话框结果；取消为 `path:null` |
| 主→子 | `{type:'shutdown'}` | 优雅退出 |

文件对话框过滤器为 `json`、`usd/usda/usdc/usdz`，单选（desktop/main.mjs:204-213）。注意只有“打开工程”走主进程对话框；STREAM 的 `/api/stream/pick`（选择栅格、气象数据包、元数据、目录等）在两种模式下都由服务端启动 PowerShell WinForms 对话框，没有超时也不限并发（server/stream/index.mjs:631-653,691-695）。

#### 1.3.4 超时与时限

| 项 | 值 | 超时后果 |
| --- | --- | --- |
| 等待 `listening` | 30 000 ms | 拒绝并 `terminateServer`（desktop/main.mjs:234-237） |
| `waitForServer` | 每 150 ms 轮询一次 `GET /api/defaults`，总 15 000 ms | 抛出最后一次错误或“启动超时”（desktop/main.mjs:319-332） |
| 优雅退出 `stopServerProcess` | 5 000 ms | 强杀进程树后 resolve，不会卡住退出（desktop/main.mjs:167-178） |
| 服务进程内部关闭兜底 | 1 500 ms | `process.exit`（desktop/server-process.mjs:44） |
| 记录的 stderr 尾部 | 4 000 字符 | 用于报错展示（desktop/main.mjs:230） |

服务进程在就绪前退出时，错误信息包含退出码与 stderr 尾部（desktop/main.mjs:262-265）。

#### 1.3.5 崩溃恢复

服务进程在就绪后意外退出时，`handleServerExit`（desktop/main.mjs:277-310）按下列步骤处理：

1. 若应用正在退出，或该记录已不是当前服务进程，直接返回（:278）。
2. Windows 下用 PowerShell 查找“父进程号等于崩溃进程、创建时间不早于启动时刻减 2 s”的子进程并 `taskkill /T /F`（:148-163）。
3. 丢弃 60 s 之前的崩溃记录，再记入本次；记录数大于 2（即 60 s 内第 3 次崩溃）或处于 `--smoke-test` 时弹窗并退出应用（:282-290）。
4. 否则重启服务进程、等待就绪、更新 `appOrigin`（新的随机端口），重新载入窗口并弹出警告“正在进行的模拟已中断”（:292-304）。重启失败则弹窗并退出（:305-309）。

由于服务端的当前工程、运行状态、SSE 连接都保存在服务进程内存中（server.mjs:136-148），重启后这些状态全部丢失，窗口重新载入页面后需重新打开工程（据代码推得）。运行产物仍保留在磁盘的运行目录中。

#### 1.3.6 窗口、导航与桌面设置

- 窗口取主显示器工作区内最大的 16:9 窗口并居中，最小尺寸 `min(1100, 工作区宽) × min(720, 工作区高)`，`show:false`，就绪后显示（desktop/main.mjs:364-399,412-414）。
- 页面隔离：`contextIsolation:true`、`nodeIntegration:false`、`sandbox:true`，仅通过 `preload.cjs` 暴露 `window.streamStudioSettings`（`read/setItem/removeItem`，同步 IPC）（desktop/main.mjs:393-398；desktop/preload.cjs:9-13）。
- 导航策略：`window.open` 一律拒绝，`http(s)` 链接交给系统浏览器；`will-navigate` 目标非同源（`appOrigin`）时阻止并外开（desktop/main.mjs:402-411）。
- 设置 IPC `stream-studio:settings` 只接受来自主窗口主框架且同源的请求，否则返回 `{ok:false,error:'桌面设置请求来源无效'}`（desktop/main.mjs:353-362）。
- 设置文件 `renderer-settings.json`（`app.getPath('userData')`）与端口无关，所以随机端口变化不会丢设置（desktop/settings.mjs:30-33）。允许的键白名单共 9 个：`streamStudioLanguage`、`histreamStudioLanguage`、`histreamExampleDirectory`、`histreamProjectDirectory`、`histreamExecutable`、`streamStudioImage`、`streamStudioScene`、`histreamStreamScene`、`streamStudioImageProject`（desktop/settings.mjs:20-24）。值只能是字符串或 `null`（`null` 为删除墓碑），单值上限 5 MB（UTF-8 字节）；写入先写 `.tmp` 再 `rename`；文件内容不是 JSON 对象时改名为 `.corrupt` 并以空设置继续（desktop/settings.mjs:36-67）。该文件头注释写“10 个键”，与白名单实际 9 个不一致，以代码为准（desktop/settings.mjs:61）。
- 渲染层 `appStorage` 在桌面模式读到未记录的键时，会把旧 `localStorage` 的值迁移进桌面设置（src/renderer/src/storage.js:18-27）。
- `--smoke-test`：不显示窗口，载入成功后输出 `STREAM_STUDIO_SMOKE_TEST_OK <url>` 并退出（desktop/main.mjs:413,418-421）。

#### 1.3.7 退出与进程清理

第一次 `before-quit` 被阻止，调用 `stopServerProcess` 发送 `shutdown`；服务进程内 `shutdown()` 依次：`stopEngines()` → 拒绝等待中的对话框请求 → `server.close` 并 `closeAllConnections()`（断开 SSE）→ 退出（desktop/server-process.mjs:35-45）。`stopEngines()` 终止出图线程、线程池、HiStream 引擎进程树（Windows 用 `taskkill /T /F`）并关闭 STREAM 服务（server.mjs:5652-5662）。主进程 `exit` 事件另有兜底，强杀仍存活的服务进程树（desktop/main.mjs:117-120）。非 Windows 平台 `killProcessTree` 只杀该 pid 本身（desktop/main.mjs:125-132）。

### 1.4 HTTP 服务

#### 1.4.1 监听与静态文件

- **监听地址**：只绑定 `127.0.0.1`（server.mjs:5611；desktop/local-server.mjs:31），不对外网开放。Web 模式默认端口 4173，`Number(env || 4173)`，没有范围校验（server.mjs:5602）；端口被占用时输出“端口 N 已被占用…设置 STREAM_STUDIO_PORT”并令 `process.exitCode=1`（server.mjs:5604-5610）。
- **`--api-only`**：只提供 `/api`，其余路径返回 404 `{error:'开发模式请访问 http://127.0.0.1:5173'}`（server.mjs:74,5583）。此时启动日志固定打印 5173 而非实际监听端口（server.mjs:5611）。`dev.mjs` 会同时启动该后端与 Vite，Vite 把 `/api` 代理到固定的 `127.0.0.1:4173`（vite.config.js:29-31），因此开发模式下不要修改 `STREAM_STUDIO_PORT`（据代码推得）。
- **静态托管**（非 `--api-only`）：以 `GUI_OUTPUT_DIR = <RESOURCE_ROOT>/gui` 为根；请求路径解码后必须通过 `pathWithin` 检查，否则 404；文件不存在时回退 `index.html`（单页应用），连 `index.html` 都没有则 404 `文件不存在`；响应带 MIME、`X-Content-Type-Options: nosniff` 与禁嵌入头（server.mjs:72,5584-5589）。静态路径不经过 1.4.2 的校验。
- **未捕获异常**：路由内未处理的异常统一返回 500 `{error}`；若响应头已发送则直接销毁连接（server.mjs:5590-5593）。
- 服务器未设置 `requestTimeout`、`headersTimeout` 等参数，沿用 Node 默认值（server.mjs 中无相关设置，grep 无结果）。

#### 1.4.2 `/api/*` 请求校验

所有 `/api/` 请求在进入 `api()` 前依次执行 `foreignRequestReason`（server.mjs:5576-5579,776-787）。任一检查失败返回 403 `{error}`：

| 检查 | 规则 | 失败信息 |
| --- | --- | --- |
| Host | 主机名（忽略端口）必须属于 `127.0.0.1`、`localhost`、`[::1]`（server.mjs:755,765-767,777） | `Host 不是本机地址` |
| Origin | 若出现，其主机名必须属于上述集合；`Origin: null` 无法解析，会被拒绝（server.mjs:778-779） | `拒绝跨站请求` |
| Sec-Fetch-Site | 若出现，只允许 `same-origin` 或 `none`（server.mjs:782-783） | `拒绝跨站请求` |
| Content-Type | 方法不是 GET/HEAD 时，必须匹配 `^application\/json\b`（不区分大小写）（server.mjs:784-785） | `API 请求必须使用 application/json` |

设计目的是防止其他网站（CSRF）与 DNS 重绑定访问可启动程序、写文件的接口；JSON 内容类型会触发 CORS 预检，而服务端从不批准预检（server.mjs:771-773）。源码明确说明：**没有会话令牌，同一台机器上的其他进程仍可调用 API**（server.mjs:774-775）。`/api/devices` 与 `/api/project/image` 另外对 `Sec-Fetch-Site: cross-site` 做了重复检查（server.mjs:4616,4826）。

请求体由 `body()` 读取：累计超过 160 MB（`MAX_REQUEST_BYTES`）抛错；超过 4 MB（`LARGE_REQUEST_BYTES`）的在后台线程解析（`parse-json`，字节以 Transferable 方式移交）；结果必须是 JSON 对象，否则报“请求体须为 JSON 对象”（server.mjs:757,763,791-805）。DEM 上传另有上限：解码后 64 MB，Base64 文本长度由此推出（server.mjs:761-762,4757-4760）。

响应约定：JSON 响应一律带 `X-Frame-Options: DENY` 与 `Content-Security-Policy: frame-ancestors 'none'`（server.mjs:437-448）；错误统一为 `{error:'…'}`，多数输入错误为 400，未匹配的 `/api` 路径返回 404 `API 不存在`（server.mjs:5581）。`/api/project/image` 与代理结果下载等直接 `writeHead` 的响应不附带禁嵌入头（server.mjs:4836,4514-4516）。

#### 1.4.3 SSE 事件流

HiStream 事件流 `GET /api/events`：响应 `text/event-stream`，先写注释行 `: connected`，再推送一条 `{type:'status', running, pid, …当前运行信息}` 让重连的界面补同步；之后由 `emit()` 向 `clients` 集合广播 `data: <JSON>\n\n`，连接关闭时移出集合（server.mjs:4625-4634,748-752）。服务端发出的事件类型为 `status`、`started`、`stdout`、`stderr`、`error`、`closed`（server.mjs:5028-5294）。没有心跳；浏览器 `EventSource` 自动重连（src/renderer/src/web-api.js:126）。

STREAM 事件流 `GET /api/stream/events` 独立：先推 `status`，之后为 `job-queued`、`job-started`、`job-progress`、`job-log`、`job-closed`（server/stream/index.mjs:166,196,228,233,702-708）。

#### 1.4.4 路由分类

`api()` 先把请求交给 `streamService.handle`，未处理（返回 `false`）才继续匹配 HiStream 路由（server.mjs:4490-4492）。全部路由如下：

| 类别 | 路由 | 说明 |
| --- | --- | --- |
| STREAM 服务 | `GET /api/stream/status`、`events`、`forcing/presets`；`POST /api/stream/` 下的 `run`、`stop`、`inspect`、`raster`、`runs`、`run-info`、`class-stats`、`project/open`、`project/save`、`era5/list`、`era5/register`、`era5/remove`、`forcing/preset`、`forcing/check`、`forcing/register`、`forcing/metadata/save`、`forcing/metadata/read`、`pick` | 路由表键为“方法 路径”，成功 200，异常 400 `{error}`（server/stream/index.mjs:658-714） |
| 代理（histream-proxy） | `GET /api/histream-proxy/info`、`GET …/result`、`POST …/run` | 固定抓取帧的代理后端，不调用原生引擎；需环境变量清单（server.mjs:4494-4595） |
| 基础信息 | `GET /api/defaults`、`POST /api/help/open`、`GET /api/devices`、`GET /api/events` | `defaults` 返回引擎路径及存在性、当前工程、`runTimeoutSeconds`、`appVersion` 等，桌面主进程用它探测就绪（server.mjs:4598-4634） |
| 示例 | `GET /api/examples`、`POST /api/examples/open` | 示例复制到用户目录后打开（server.mjs:4636-4650） |
| 工程 | `POST /api/project/` 下的 `open`、`pick`、`create`、`save-as`、`save`、`tarp-cover`、`import-obj`、`import-meteo`、`import-dem`、`dem-info`、`import-spectrum`、`import-texture`、`distribution`、`read`；`GET /api/project/image` | 工程读写与资源导入（server.mjs:4652-4902） |
| 运行 | `POST /api/run/preflight`、`/api/run`、`/api/stop`、`/api/reset` | 预检、启动、停止、重置（server.mjs:4933-5362） |
| 结果 | `POST /api/results/` 下的 `delete-all`、`list`、`read`、`change`；`POST /api/open` | 结果删除/列表/读取/变化分析，及在资源管理器中打开（server.mjs:5365-5461） |

`/api/run` 的 `mode` 取值必须属于 `eVoxelEB`、`eFacetEB`、`eFacetRT`、`eVoxelRT`、`eRaytracing`，否则报“不支持的计算模式”（src/renderer/src/project-schema.js:31；server.mjs:4979）。运行互斥：重置中、已有运行、已有准备中的运行三种情形都拒绝新运行（server.mjs:4967-4970）。

#### 1.4.5 路径限制策略

服务端对不同接口使用不同范围。路径比较用 `pathWithin`（纯字符串，根本身算在内，server.mjs:529-534）；需要防链接逃逸时用 `realPathWithin`，先 `realpath` 再比较，`allowMissing` 时对尚不存在的目标改查最近的已存在祖先（server.mjs:538-554）。

| 接口 | 允许范围 | 违反时 |
| --- | --- | --- |
| `/api/run` 的引擎 `executable` | 内置候选路径或 `STREAM_STUDIO_ENGINE_ALLOWLIST`（按系统路径分隔符分隔），Windows 比较忽略大小写并比较 realpath；缺省取内置引擎（server.mjs:196-211） | 抛错“不允许的 HiStream 引擎路径”（400） |
| `/api/results/read`、`/results/change` | `resultFilePathAllowed`：位于当前工程输出根；或其目录/上级目录带有效 `run-manifest.json`；或目录含 `faceteb.json`/`facetrt.json`/`scene_summary.json`（server.mjs:3036-3047） | 400“只能读取当前工程输出目录或已识别运行目录中的结果文件” |
| `/api/results/list`、`/results/delete-all` | `resultDirectoryAllowed`：与任何工程输入文件/输入目录相交即拒绝；正在运行的输出目录拒绝；允许输出根、带运行清单的目录或其 `runs/` 下含有效运行目录者，`runs` 为符号链接则拒绝（server.mjs:3013-3029）；删除另拒绝磁盘根，且运行中拒绝（server.mjs:3470,5367） | 400 |
| `/api/open` | 拒绝 UNC 路径；仅 Windows 实际调用 `explorer.exe`；路径须在当前工程、输出根、`<RESOURCE_ROOT>/assets` 或 `~/Documents/Stream Studio` 内（server.mjs:3051-3058,5452-5459） | 400 |
| `/api/project/read` | 当前工程目录或 `<RESOURCE_ROOT>/assets`，文件 ≤ 80 MB（server.mjs:4894-4896） | 400 |
| `/api/project/image` | 同上范围，仅 PNG/JPG，≤ 32 MB（server.mjs:4829-4835） | 400 |
| `/api/project/save` | 必须在当前工程目录内；换目录须用“另存为”（server.mjs:4911） | 400 |
| `/api/project/distribution` | 仅当前工程 `positions/` 下的 `.txt`，实例 ≤ 100 000（server.mjs:4871-4876） | 400 |
| 各 `import-*` | 文件名取 `basename` 并清洗为 `[字母数字_-]`，重名追加 `_2`、`_3`…，写入工程的 `models/`、`meteorology/`、`terrain/`、`spectral/`、`textures/`（server.mjs:4713-4863） | 400 |
| `/api/project/open`、`create`、`save-as`、`pick` | 用户给出的任意路径；`create`、`save-as` 要求目录为绝对路径，名称不得含 `<>:"/\|?*` 与控制字符；打开要求入口文件名为 `project.json`（或 USD）（server.mjs:942,2810-2811,2845-2847） | 400 |
| `/api/help/open` | 仅 `user`、`theory` 两个白名单键，对应两本 PDF（server.mjs:62,4606） | 400“未知的帮助文档” |
| 静态文件 | `GUI_OUTPUT_DIR` 内（server.mjs:5585） | 404 |

内部路径换算：`normalizeHostPath` 在 Linux 下把 `X:\…` 转为 `/mnt/x/…`，在 Windows 下把 WSL 路径转回盘符路径；`windowsPath` 把 `/mnt/x/…` 转回 `X:\…` 供界面显示；`projectAssetPath` 把相对路径按工程目录解析，`assets/` 开头且工程内不存在时回落到 `<RESOURCE_ROOT>/assets`（server.mjs:847-873,967-974）。

### 1.5 后台线程池与重任务

#### 1.5.1 哪些任务进线程

`HEAVY_TASKS` 共 18 个任务（server.mjs:5484-5536），分三类：

| 执行方式 | 任务（任务名 → 线程亲和 key） |
| --- | --- |
| 常驻池 `heavyTasks.run` | `parse-json`（请求体大于 4 MB，无 key）；`result-read`（key=结果路径）；`result-image`；`results-change`（key=第一幅路径）；`project-read`；`dem-info`（key=DEM 路径）；`dem-inspect`；`tarp-cover`；`import-obj`（key=工程目录）；`run-preflight`；`facet-scale`；`runtime-scene`（key=输入工程路径）；`usd-export`（key=工程目录）；`results-list`；`stream-raster`（key=栅格路径）；`stream-class-stats`（key=类型文件） |
| 一次性线程 `runPostprocessTask` | `facet-tiff`（面元 JSON → TIFF）、`envi-convert`（ENVI → TIFF），仅在运行结束后的收尾阶段调用（server.mjs:5222,5246） |
| 专用线程 | `facet-step-service` / `facet-step`（FacetEB 逐节点出图，见 1.2） |

调用点见 server.mjs:491,801,4704-4796,4898,4944,5037,5383,5418,5442,5542,5558；STREAM 通过注入的 `runTask` 使用同一线程池（server.mjs:184-185）。没有进入线程池、仍在服务线程同步执行的文件操作包括：`import-meteo`、`import-spectrum`、`import-texture`、`distribution` 的写盘、`create`/`save-as`/`examples/open` 中的目录复制、文本结果读取（≤ 4 MB）与删除结果（server.mjs:4739,4818,4860,2877,2962,5420-5424,3475）。

线程内任务的参数与返回值必须可结构化克隆；返回 JSON 的任务在线程内预先序列化为字节（`jsonPayload`），主线程用 `toJsonBytes` 包装后由 `json()` 原样发送，避免主线程二次 `JSON.stringify`（server.mjs:5468-5474,440-448）。任务名用 `Object.hasOwn` 查表，防止原型链名称被调用（server/task-pool.mjs:263；server.mjs:5562-5565）。

#### 1.5.2 常驻池机制

线程数 `TASK_WORKERS`：`STREAM_STUDIO_TASK_WORKERS` 未设置或为空时取 2；否则取 `floor(Number(值))` 并夹到 [0, 8]，非有限值取 2；0 表示不使用线程，全部在服务线程内同步执行（server.mjs:154-158,161-167）。`createTaskPool` 的行为（server/task-pool.mjs:86-219）：

- 线程懒创建，槽位数等于 `size`；`size ≤ 0` 时始终内联（:87,90）。
- 选线程：任务带 `key` 时按 key（转小写）的 32 位 FNV-1a 哈希取模，使同一文件的任务落在同一线程并命中其内部缓存；无 key 时选待决任务最少的槽位（空槽位负载计 0）（:74-79,153-161）。
- 同一线程内任务逐个处理：线程端任务函数目前按同步重活实现，一个任务执行期间后续消息排队（docs/model-remaining-issues-20261005.md:143 亦如此说明）。
- 空闲回收：线程上没有待决任务时开始计时，默认 5 分钟（`idleMs = 5*60*1000`，server.mjs 创建池时未覆盖），到期回收并释放内存；定时器 `unref` 不阻止进程退出（:86,108-114）。
- 大字节结果以 Transferable `ArrayBuffer` 回传：仅当 `bytes` 独占整块非共享、非空的缓冲时才转移，否则退化为结构化复制；`options.transfer` 为真时请求方的 `args.bytes` 同样转移，调用方此后不得再使用（:36-60,169-191）。
- 消息协议：主→线程 `{id,task,args}`、`{type:'clear'}`；线程→主 `{type:'ready'}`、`{id,ok,result|error}`（:6-12,258-272）。`delete-all` 通过 `broadcast({type:'clear'})` 让各线程清空自己的缓存（server.mjs:5368-5369）。

#### 1.5.3 失败与回退

| 情形 | 行为 |
| --- | --- |
| 线程在 `ready` 之前出错或退出（启动失败） | 累计 `startupFailures`；未达 2 次（`STARTUP_RETRIES`）则重新投递并重建线程；达到 2 次后永久改为服务线程内联执行，只打印一次日志“后台计算线程无法启动…改为在服务线程内同步执行”；收到 `ready` 后计数清零（server/task-pool.mjs:32,119-150,195-203） |
| `createWorker()` 同步抛错 | 同样计为启动失败并回退（:121-124,173-175） |
| 线程在任务中途崩溃 | 该线程所有待决任务以“后台计算线程异常退出…本次操作未完成，请重试”失败，槽位清空，服务继续，下次任务重新建线程（:67-70,136-146） |
| 任务函数抛错 | 以其 `error.message` 拒绝（缺省“后台计算失败”），线程保持存活（:63-65,268） |
| 线程被回收时仍有待决任务 | 以“后台计算线程已回收”拒绝，调用方 Promise 不会悬挂（:99-103） |
| 一次性线程 `runTaskOnce` | 收到成功消息 → resolve；收到失败消息 → reject；未 `ready` 就退出 → 回退内联；`ready` 之后无结果就退出 → 崩溃错误。结果在线程 `exit` 之后才 resolve，保证堆与 ArrayBuffer 已释放（:225-254） |
| 一次性线程创建失败 | 记录日志并内联执行（:228-232） |
| “停止”或“重置” | `running.facetWorker` 被 `terminate()`；后处理失败时清单记录“结果影像生成已被停止”（server.mjs:5340,5231） |

打包环境（asar）中后台线程能否启动源码文档称“未实测”，无法启动时依靠上述回退（docs/model-remaining-issues-20261005.md:143），在打包安装版下的实际线程启动情况待核实。

### 1.6 结果缓存

#### 1.6.1 缓存键与失效

`fileStamp(path)` 返回文件身份：解析后的绝对路径 + `mtimeMs` + `size` + `ctimeMs`，键为 `路径|mtime|size|ctime`；非普通文件或不存在时返回 `null`（server/result-cache.mjs:45-55）。加入 `ctimeMs` 是因为它无法被 `utimes` 或保留时间戳的拷贝工具还原，因此就地改写而 mtime、size 不变也会失效（:50-52）。

响应缓存键为 `result:<variant>|<binary|json>|<各依赖文件的身份键>`；任一依赖文件不存在则不缓存（server.mjs:483-484）。`variant` 区分 ENVI/TIFF 波段（`envi:<band>`、`tiff:<band>`）、风场（`wind`）、面元（`facet`）与过程结果（`process`）（server.mjs:5403-5414）。读取期间文件被改写（引擎仍在写出）时不入缓存（server.mjs:494）。同一键的并发请求合并为一次计算（`resultInFlight`，server.mjs:475,489-501）。

#### 1.6.2 `ByteLru` 与容量

`ByteLru(limitBytes)`（server/result-cache.mjs:60-127）：

| 方法 | 行为 |
| --- | --- |
| `get(key)` | 命中则移到最近使用端，计 `hits`；未命中计 `misses` |
| `set(key,value,bytes,stamps)` | 先删同键旧条目；同一路径（小写）出现新身份时，依赖旧身份的其他条目一并作废；`limit` 为 0 或单条超过上限则不存入（返回 `false`）；否则存入并从最旧端淘汰直到不超限 |
| `resize(key,bytes)` | 条目内容增长（TIFF 解码块累积）后重新计费；超限则自身被淘汰 |
| `delete` / `clear` | 删除条目 / 清空 |

总上限由 `STREAM_STUDIO_RESULT_CACHE_MB` 决定：未设置或为空为 512 MB；其他值需为有限正数，按 `floor(MB×1024×1024)` 字节；**非数值、负数或 0 一律得到 0，即关闭缓存**（server/result-cache.mjs:37-41）。

缓存分布在多个线程，各线程一个 `resultCache` 实例：

| 位置 | 内容 | 上限 |
| --- | --- | --- |
| 服务线程 | 已编码的接口响应（`{payload, contentType}`）；`TASK_WORKERS=0` 时同时承载原始数据 | `resultCacheLimitBytes()`（server.mjs:152） |
| 每个 `task-service` 线程 | 原始 TIFF/ENVI 文件字节与已解码分块（键前缀 `source:`） | `floor(总上限/线程数)`；`TASK_WORKERS=0` 时为 0（server.mjs:163-165,5637） |
| `task-once` 线程 | 不缓存 | 0（server.mjs:5641） |

因此内存峰值最多约为设置值的 2 倍（服务线程一份加各线程均分的一份，据上述公式推得，与 docs/release-next-draft.md:244 的说法一致）。`readTiff`/`readEnvi` 经 `cachedResultSource` 读盘：读前后文件身份不一致或缓存拒绝时该次不缓存（server.mjs:3837-3846,4441）。`STREAM_STUDIO_RESULT_CACHE_DEBUG=1` 时每次读取打印 hit/miss、大小与占用（server.mjs:506-508）。

#### 1.6.3 二进制结果编码

请求头 `Accept` 包含 `application/vnd.stream-studio.result-v1` 时，面元/体元结果中名为 `vertexPositions`、`voxelPositions`、`values` 的大数值数组（长度 ≥ 256 且元素只含数字/null/undefined）被抽成连续数值块，其余字段保持 JSON；没有可抽数组时退回普通 JSON；不带该 `Accept` 的旧客户端始终得到 JSON（server/result-cache.mjs:32,132-133,169-207；server.mjs:451-456,481）。格式 SSR1（小端）：4 字节魔数 `SSR1`、`uint32` 头部 JSON 长度 L、头部 `{value,buffers}`，数据区起点为 `ceil((8+L)/8)*8`，每块按 8 字节对齐；全部值可用 float32 精确表示时用 `f32`，否则 `f64`；非数值记为 NaN；被抽走的数组在 `value` 中以 `{$ssBuffer:序号}` 占位（server/result-cache.mjs:11-14,148-163,196-205）。响应头带 `Cache-Control: no-store` 与 `X-Cache: hit|miss`（server.mjs:510-513）。单个结果文件或数组超过 `STREAM_STUDIO_MAX_RESULT_BYTES`（缺省 8 GiB）时拒绝读取（server/facet-result.mjs:16,26-30）。

### 1.7 目录与环境变量

#### 1.7.1 目录

| 目录 | 桌面模式 | Web 模式 | 依据 |
| --- | --- | --- | --- |
| 源码根 `PROJECT_ROOT` | 应用包根（`desktop` 的上两级，打包后位于 asar 内） | 仓库根 | desktop/main.mjs:31；server.mjs:60 |
| 资源根 `RESOURCE_ROOT` | `process.resourcesPath` | 缺省 `PROJECT_ROOT`；`STREAM_STUDIO_RESOURCE_ROOT` / `HISTREAM_STUDIO_RESOURCE_ROOT` 覆盖 | desktop/main.mjs:313-315；server.mjs:71 |
| 前端 `gui/` | `<RESOURCE_ROOT>/gui` | 同左 | server.mjs:72 |
| 引擎 | `<RESOURCE_ROOT>/engine/histream.exe` 优先 | 依次回落到 `models/bin_x64/Release|Debug/histream.exe`、`models/histream/bin/Debug|Release/histream.exe` | server.mjs:124-130 |
| STREAM 引擎 | `<RESOURCE_ROOT>/engine/stream.exe`，数据在其旁 `stream-defined/` | 回落 `models/bin_x64/Release|Debug/stream.exe` | server/stream/index.mjs:79-96 |
| 内置资产 | `<RESOURCE_ROOT>/assets`（含 `examples/`） | 同左 | server.mjs:2909 |
| 手册 | `<RESOURCE_ROOT>/docs/*.pdf`，Web 模式回落 `PROJECT_ROOT/docs` | 同左 | server.mjs:62,4608 |
| 用户数据 userData | `%APPDATA%\Stream Studio` | 无 | desktop/main.mjs:46-51 |
| 数据根 DATA_ROOT | `%LOCALAPPDATA%\StreamStudio`（无则 userData） | 未设置 | desktop/main.mjs:340-342 |
| 示例默认保存目录 | `~\Documents\Stream Studio\examples` | 同左 | server.mjs:2929-2931 |
| STREAM 默认输出与数据包目录 | `~\Documents\Stream Studio\stream`；`era5-packs`、`forcing-metadata` | 同左 | server/stream/index.mjs:84-97 |
| 工程输出 | 工程内 `output/`；每次运行的目录为 `<outDir>/runs/<UTC时间戳>-<8位随机>/` | 同左 | server.mjs:330-333,2828 |

要点：

- 新建工程时创建 `models/`、`positions/`、`spectral/`、`meteorology/`、`terrain/`、`images/`、`output/` 七个子目录，`outDir` 写为绝对路径 `<工程>/output`（server.mjs:2817,2828）。另存为复制其中前六个（不含 `output`）并把 `outDir` 重置为 `output`（server.mjs:2870,2873-2880）。
- 每次运行在 `runs/<runId>` 下写 `source-project.json`、`run-manifest.json`、`input/`（冻结工程副本、裁切后的 OBJ、`meteo.txt`）和引擎输出（server.mjs:379-380,5006-5026）。
- 打开示例时把示例目录复制到“目录/示例文件夹名”，已有同名副本就地覆盖示例自带文件、保留 `output/`；`output` 本身不被复制（server.mjs:2946-2963）。
- `DATA_ROOT` 在服务端只作为 `/api/open` 的回退目录（`data.path || projectDir || DATA_ROOT || PROJECT_ROOT`）；默认输出与示例目录为 `文档\Stream Studio` 下的子目录，与 README 说明一致（README.md:9；server.mjs:5451）。回退到 `DATA_ROOT` 时，Windows 上的 `userOpenPathAllowed` 范围检查是否放行，待核实。

#### 1.7.2 环境变量

应用变量前缀为 `STREAM_STUDIO_`。`envSetting` 取按顺序第一个非空值，并兼容旧前缀 `HISTREAM_STUDIO_`（server.mjs:63-70；README.md:36）。下表“旧别名”列标明是否通过 `envSetting` 读取：

| 变量 | 默认值 | 取值与解析 | 旧别名 | 读取位置 |
| --- | --- | --- | --- | --- |
| `STREAM_STUDIO_PORT` | 4173 | `Number()`，无范围校验 | 有 | server.mjs:5602 |
| `STREAM_STUDIO_SERVER_AUTOSTART` | 未设置（自动监听） | 值为 `0` 时不监听；测试与出图脚本也用它导入 `server.mjs` | 有 | server.mjs:5603 |
| `STREAM_STUDIO_RESOURCE_ROOT` | `PROJECT_ROOT` | 路径，`resolve` | 有 | server.mjs:71 |
| `STREAM_STUDIO_DATA_ROOT` | 未设置 | 仅用作 `/api/open` 回退 | 有 | server.mjs:5451 |
| `STREAM_STUDIO_ENGINE_ALLOWLIST` | 空 | 路径列表，按系统路径分隔符（Windows 为 `;`） | 有 | server.mjs:201 |
| `STREAM_STUDIO_PROXY_MANIFEST` / `_RUNNER` / `_PYTHON` | 空（代理未配置） | 路径；清单中的 runner 与 python 必须与后两者一致 | 有 | server.mjs:586,621-622 |
| `STREAM_STUDIO_RUN_TIMEOUT_MS` | 900 000（15 分钟） | `Number(值) \|\| 缺省`，0 或非数值回到缺省；工程 `control.runTimeoutMinutes`（1–1440）优先 | 无 | server.mjs:106-111 |
| `STREAM_STUDIO_MAX_FACET_TRIANGLES` | 20 000 000 | `floor` 后须 ≥ 1000，否则回到缺省；工程 `control.maxFacetTriangles` 优先 | 无 | server.mjs:88-95 |
| `STREAM_STUDIO_TASK_WORKERS` | 2 | 整数夹到 [0, 8]，0 = 不用线程 | 无 | server.mjs:154-158 |
| `STREAM_STUDIO_RESULT_CACHE_MB` | 512 | 有限正数；其他得 0（关闭） | 无 | server/result-cache.mjs:37-41 |
| `STREAM_STUDIO_RESULT_CACHE_DEBUG` | 未设置 | 仅 `1` 生效 | 无 | server.mjs:506 |
| `STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS` | 3 000 000 | `Number(env ?? 缺省)`；空字符串得 0（每节点都单开线程），非数值使比较恒为假（总是常驻线程）（据代码推得） | 无 | server.mjs:2396 |
| `STREAM_STUDIO_MAX_RESULT_BYTES` | 8 GiB | `Number(值) \|\| 缺省` | 无 | server/facet-result.mjs:16 |
| `STREAM_STUDIO_STREAM_TIMEOUT_MS` | 7 200 000（2 小时） | 有限且 ≥ 0 才采用，0 = 关闭看门狗；空字符串得 0，即关闭（据代码推得） | 无 | server/stream/index.mjs:43-45 |

引擎侧变量：服务端启动 HiStream 时以 `{...process.env, HISTREAM_FACET_STEP_SYNC: '1'|'0'}` 作为子进程环境，FacetEB 为 `1`（逐节点握手），其余为 `0`，用户同名设置会被覆盖（server.mjs:5075-5078）。其余 `HISTREAM_*`（如 `HISTREAM_COMPUTE_BACKEND`、`HISTREAM_FACET_OUTPUT_FORMAT`、`HISTREAM_CPU_THREADS`）原样继承给引擎，由引擎自行读取（models/histream/src/base/engine.cpp:95；facetrt/facetrtcore.cpp:108,5801），其语义不在本章范围。STREAM 引擎子进程同样继承环境（server/stream/index.mjs:212）。

### 1.8 构建与部署产物

#### 1.8.1 npm 脚本

| 脚本 | 命令 | 作用 |
| --- | --- | --- |
| `dev` | `node dev.mjs` | 后端 `--api-only` + Vite 开发服务器 |
| `build` / `build:gui` | `vite build` | 把 `src/renderer` 构建到 `gui/`，`emptyOutDir:true` 每次先清空 `gui/`，故 `gui/` 内不要手放文件（vite.config.js:35-37） |
| `build:engine` | `powershell … tools/build-engine.ps1` | CMake 构建两个引擎并写 `build-manifest.json` |
| `build:all` | `powershell … tools/build-all.ps1` | 先 `npm.cmd run build:gui`，再 `build-engine.ps1`，任一步失败即以其退出码退出（tools/build-all.ps1:23-27） |
| `start` | `node server.mjs` | Web 模式 |
| `desktop` | `electron .` | 桌面模式（开发） |
| `stage:package` | `node tools/stage-package-assets.mjs` | 生成 `.packaging/assets` |
| `pack:win` | `npm run build:all && npm run stage:package && electron-builder --win nsis` | 打包 Windows 安装包 |
| `test` | `node --test` 四个测试文件 | 见 package.json:16 |
| `validate:assets`、`lut:modtran` | 对应 `tools/*.mjs` | 资产校验、大气 LUT |

（package.json:9-22）开发依赖版本：electron `^38.0.0`、electron-builder `^26.0.12`、vite `7.3.6`、three `0.185.1`（package.json:24-27）。

#### 1.8.2 引擎构建流程

```
build-engine.ps1 [-Configuration Debug|Release(默认)] [-VcpkgRoot]
 ├─ VcpkgRoot：参数 / 环境变量 VCPKG_ROOT / 仓库同级的 vcpkg
 ├─ cmake：PATH 优先，否则在 Visual Studio 目录搜索
 ├─ 配置 + 构建 HiStream：  源 models/histream → 构建 build/histream
 │     -DOUTPUT_PATH=models/bin_x64，Release 另加 -DHISTREAM_REQUIRE_USD=ON
 │     可执行文件输出到 ${OUTPUT_PATH}/$<CONFIG>/，并拷贝着色器、defined/、
 │     vcpkg 运行库、lavapipe 到同一目录
 ├─ 配置 + 构建 STREAM：    源 models/stream → 构建 build/stream
 │     -DSTREAM_RUNTIME_DIR=models/bin_x64，输出到 .../$<CONFIG>/，数据在 stream-defined/
 └─ node tools/write-build-manifest.mjs models/bin_x64/<Configuration>
```
（tools/build-engine.ps1:17-91；models/histream/CMakeLists.txt:420,499-526；models/stream/CMakeLists.txt:9,36,87-90）

`write-build-manifest.mjs` 写入 `build-manifest.json`（`kind:'stream-studio-build'`，`schemaVersion:1`）：应用版本、构建时间、git `commit` 与 `dirty`（git 不可用时只告警，`commit` 为空串、`dirty` 为 `null`）、`sourceTreeSha256`（对源码清单的 JSON 求 SHA-256；清单含 histream 与 stream 源码、着色器源、`src/renderer`、`server.mjs`、`server/*.mjs`、六个运行时 `tools/*.mjs` 和 `package.json`）、引擎 SHA-256、运行时附带二进制、已部署的 `.spv` 着色器二进制，以及“过期”（`staleShaderBinaries`，非本次构建产物）与“缺失”（`missingShaderBinaries`）列表（tools/write-build-manifest.mjs:41-56,67-114）。每次 HiStream 运行准备时，服务端读取引擎目录旁的该文件，把其路径、字节数、哈希、`applicationVersion`、`sourceTreeSha256` 记入运行清单的 `build` 字段，读取失败则标记 `unreadable:true`（server.mjs:371-377）。当前开发机的清单创建于 2026-11-26，`dirty` 为 `true`（models/bin_x64/Release/build-manifest.json:5-7）。

#### 1.8.3 运行时目录布局（`models/bin_x64/Release`）

观察到的内容：`histream.exe`、`histream-usd.exe`、`stream.exe`；vcpkg 复制的各类 `*.dll`（GDAL、PROJ、netCDF、HDF5、USD、TBB、OpenCV 等）；子目录 `defined/`（气象、光谱、土壤等引擎输入，含 `meteo.txt`）、`shader/`（`facetrt`、`faceteb`、`voxelrt`、`voxeleb`、`RayTracing`、公共 GLSL）、`stream-defined/`（`classes.json`、`optipar_new.csv`、`Esun_.dat`、`Esky_.dat`）、`lavapipe/`（Mesa CPU Vulkan 驱动）、`proj/`、`usd/`；以及 `build-manifest.json`。该目录整体被 `.gitignore` 排除（.gitignore:32），由 `npm run build:engine` 产生。服务端依赖该目录中的 `defined/meteo.txt` 作为内置气象（server.mjs:335-336），`stream-defined` 的四个文件用于判断 STREAM `dataReady`（server/stream/index.mjs:127）。

#### 1.8.4 桌面安装包布局

`electron-builder.yml` 定义（electron-builder.yml:3-10,71-78；appId `cn.stream.studio`，产品名 Stream Studio，`asar:true`，输出目录 `release/`，仅打包 `zh-CN`、`en-US` 语言包，Windows NSIS x64，安装包名 `Stream-Studio-Setup-${version}-x64-slim.${ext}`）：

| 位置 | 内容 | 来源 |
| --- | --- | --- |
| `app.asar` 内 | `desktop/**/*`、`server.mjs`、`server/**/*.mjs`、`tools/` 下六个运行时脚本（`runtime-scene`、`surface-textures`、`png`、`tarp-cover`、`usd-export`、`usd-import`）、`src/renderer/src/` 下的 `project-schema.js`、`project-unified.js`、`material-specular.js`、`package.json` | electron-builder.yml:11-24 |
| `resources/gui` | 前端构建结果 | `gui/` |
| `resources/assets` | 精简资产副本 | `.packaging/assets` |
| `resources/engine` | 引擎与运行库 | `models/bin_x64/Release`，排除 `*.pdb/*.lib/*.exp/*.ilk`、`stream_old_audit.exe`、`log_nvprosample.txt`、`shader/cases/**/*.spv`、`*.cmake`、`*.xlsx`、`histream_*_tests.exe` 等 |
| `resources/docs` | 仅 `Stream_Studio_User_Manual_CN.pdf` 与 `Stream_Studio_Theory_Manual_CN.pdf` | `docs/` |
| `resources/statistic` | `*.py`、`*.mjs`、`*.md`、`*.txt`（排除测试与缓存） | `statistic/` |
| `resources/src/renderer/src/` | `project-schema.js`、`material-specular.js` | 供 `statistic` 脚本按相对路径导入（statistic/run_scene_examples.mjs:23） |
| `resources/branding/stream-studio.ico` | 图标 | `design/branding/stream-studio-logo.ico` |

（electron-builder.yml:25-70）NSIS 为非一键安装，可选安装目录，非 perMachine，创建桌面与开始菜单快捷方式，卸载时不删应用数据（`deleteAppDataOnUninstall:false`）（electron-builder.yml:79-87）。

`.packaging/assets` 由 `stage-package-assets.mjs` 从 `assets/` 生成：先清空目标再复制；排除大于 15 MiB 的 OBJ、`examples/` 下的 `output/` 目录、名为 `_*_source` 的目录、`log_nvprosample.txt`、`crop/` 下不在白名单中的 OBJ（只带 4 个作物模型），以及含超限 OBJ 的 RAMI 场景；并同步重写 `obj-library` 与 `rami-library` 的 `catalog.json`（tools/stage-package-assets.mjs:20-32,83-91,112-136,139-144）。当前仓库的 `release/` 目录为空（代码库检查时的观测）。

#### 1.8.5 版本一致性与运行时探测

`APP_VERSION` 读取 `package.json` 的 `version`，经 `/api/defaults` 返回（server.mjs:61,4600）。引擎退出码约定见 `HISTREAM_EXIT`（`ok 0`、`failed 1`、`facetNonConverged 2`、`resources 3`、`initialization 4`、`deviceQuery 5`、`usage 64`），仅 FacetEB 的退出码 2 视为“有结果”（server.mjs:119-121）。计算设备枚举调用 `engine --list-devices`，按引擎路径缓存，超时 60 s，`refresh=1` 强制重新枚举（server.mjs:217-231,4615-4623）。

### 1.9 本章待核实事项

1. `HISTREAM_ROOT` 在引擎与服务端中的读取点：未发现，实际作用待核实（desktop/main.mjs:196）。
2. 打包（asar）安装版中 `new Worker(new URL(import.meta.url))` 是否实际成功创建线程，源码文档称未实测（docs/model-remaining-issues-20261005.md:143），回退路径见 1.5.3。
3. `STREAM_STUDIO_DATA_ROOT` 仅用于 `/api/open` 回退，且其默认值（`%LOCALAPPDATA%\StreamStudio`）不在 `userOpenPathAllowed` 的允许范围内，该回退在 Windows 上的实际表现待核实（server.mjs:5451,5457,3051-3058）。
4. 服务进程崩溃后窗口重新载入页面时当前工程的恢复方式（界面是否自动重新打开工程）待核实（desktop/main.mjs:298）。

## 2. 工程数据模型与运行生命周期（HiStream）

本章描述三维引擎 HiStream 一侧“程序实际如何运行”：磁盘上的工程文件是什么形态、如何被规范化与校验、一次“运行”从点击到终态经过哪些步骤、产生哪些文件。物理方程见《理论手册》，界面操作见《用户手册》，本章只陈述代码行为。每条关键事实后括号内为“仓库相对路径:行号”；无法由代码确认者标“待核对”。

涉及的组件与职责：

| 组件 | 文件 | 职责 |
|---|---|---|
| 工程模型 | `src/renderer/src/project-schema.js` | 常量、默认工程、规范化 `normalizeProject`、校验 `validateProject`；前端、服务端、tools 共用（`src/renderer/src/project-schema.js:1-21`） |
| 统一工程转换 | `src/renderer/src/project-unified.js` | `stream-studio-project` 与旧 `histream-project` 互转（`src/renderer/src/project-unified.js:1-16`） |
| 服务端 | `server.mjs` | 工程读写、预检、隔离运行目录、spawn 引擎、SSE、结果后处理（`server.mjs:1-28`） |
| 运行时场景 | `tools/runtime-scene.mjs` | 运行前筛选实例、预裁切 OBJ、路径绝对化、偏移归零（`tools/runtime-scene.mjs:1-22`） |
| 引擎 | `models/histream/src/main.cpp` 等 | 命令行入口、读 `project.json`、计算、退出码（`models/histream/src/main.cpp:1-15`） |

总体数据流：

```
project.json(磁盘) ──readProject/normalizeProject──► 内存 histream-project 对象
      │                                                   │ validateProject
      │ copy                                              ▼
      ▼                                        createIsolatedRun（runs/<runId>/）
source-project.json                                       │
                                                          ▼
                                   runtime-scene（筛选/裁切/绝对化）→ 暂存目录
                                                          │ cp
                                                          ▼
                                       runs/<runId>/input/project.json (+位置/OBJ/气象)
                                                          │ spawn histream.exe <mode> <input/project.json> [结果JSON]
                                                          ▼
                    stdout/stderr ──SSE──► 浏览器；FacetEB 另有 stdin 握手
                                                          │ 退出
                                                          ▼
                       后处理（面元JSON→TIFF / ENVI→TIFF）→ run-manifest 终态 → closed 事件
```

### 2.1 工程文件与格式关系

#### 2.1.1 两种 project.json 形态

| 形态 | `format` | `schemaVersion` | 谁读写 |
|---|---|---|---|
| 统一工程 | `stream-studio-project`（`src/renderer/src/project-unified.js:40`） | 1（`src/renderer/src/project-unified.js:41`） | 磁盘上的权威形态；服务端保存时总是写它（`server.mjs:2502-2513`） |
| HiStream 工程（旧形态） | `histream-project`（`src/renderer/src/project-schema.js:28`） | 当前 6（`src/renderer/src/project-schema.js:27`） | 前端/GUI 的内存形态；也是**引擎唯一接受的输入形态**（`models/histream/src/base/projectjson.cpp:106-112`） |

统一工程把两个引擎的输入放进一个文件：`shared`（地理、DEM、时间、天空与大气、观测角、输出、计算设备）、`engines.histream`（三维场景独有输入）、`engines.stream`（遥感影像独有输入）（`src/renderer/src/project-unified.js:18-30`）。引擎二进制不认识统一格式，读写两侧在边界上互转（`src/renderer/src/project-unified.js:26-29`）。格式识别 `projectFormatOf`：`format` 为统一格式 → unified；`kind` 为 `stream-image-project`/`stream-scene-project` → STREAM；`format` 为 `histream-project` 或存在 `configuration` → histream（`src/renderer/src/project-unified.js:68-75`）。

HiStream 旧工程与统一工程的字段映射（`unifiedToHistreamProject`，`src/renderer/src/project-unified.js:379-438`）：

| 旧工程字段 | 统一工程位置 |
|---|---|
| `configuration.outDir` | `shared.output.root` |
| `scene.demFile` / `scene.demInfo` | `shared.location.dem.file` / `shared.location.demInfo` |
| `meteo.latitude / longitude / timezone` | `shared.location.latitude / longitude / timezone` |
| `meteo.year` | `shared.time.year` |
| `light.*`（zenith、azimuth、direct、diffuse、skyTemperature、shortwaveIrradiance、diffuseFractionMode） | `shared.sky.*` |
| `sensor.vza / vaa` | `shared.observation.vza / vaa` |
| `control.cpuThreads / gpu / computeBackend` | `shared.compute.cpuThreads / device / backend` |
| `atmosphere` | `shared.atmosphere` |
| 其余（scene、sensor、control、fluid、objects、spectra、canopies、thermals、materials、meteo 的其余字段、mode） | `engines.histream.*` |

光谱波段 `sensor.bands` 刻意留在 `engines.histream.sensor`，不进共享层，因为 HiStream 的渲染波长列表与 STREAM 的带响应函数语义不同（`src/renderer/src/project-unified.js:31-33`）。

#### 2.1.2 读取（readProject）

`readProject(inputPath)`：入口若不是 `project.json` 则取同目录的 `project.json`（`server.mjs:2462-2464`）；文件不存在返回 `{project:null}`；JSON 解析失败抛“project.json 格式错误”；去 UTF-8 BOM（`server.mjs:2517-2524`）。非统一工程且 `schemaVersion` > 6 抛错“请使用更新版本的 Stream Studio 打开”（`server.mjs:2526-2528`）；STREAM 工程（`kind` 为 `stream-*-project`）在此拒绝打开（`server.mjs:2533-2535`）。统一工程经 `normalizeUnified` → `unifiedToHistreamProject` → `normalizeProject` 得到 HiStream 内存对象（`server.mjs:2529-2532`）。注意 `parseUnifiedProject` 不检查统一工程的 `schemaVersion`，高版本会被当 v1 处理并在保存时降级（`src/renderer/src/project-unified.js:650`，源码注释已记为 DEFERRED）。

`resolveProjectFile`（打开/运行/另存的入口校验）：路径可为目录或文件；目录则补 `project.json`；文件名必须是 `project.json`；非统一工程需通过 `validateProject`，错误以“；”连接抛出，统一工程直接放行（`server.mjs:930-950`）。

#### 2.1.3 写入（writeProject）与旧文件备份

`writeProject`：`normalizeProject` → `normalizeStoredProjectPaths`（资源路径转存储形式）→ 刷新 `updatedAt`；磁盘上已是统一工程则只替换 HiStream 段（保留 STREAM 段）；否则先备份旧文件为 `project.v<schemaVersion>.json`（已存在不覆盖，失败不抛出），再整体转换为统一格式写入（`server.mjs:2502-2513`、`server.mjs:2468-2479`）。写入为原子操作：先写 `.tmp` 再 rename（`server.mjs:2482-2492`）。因此**首次保存后磁盘上的 project.json 即为统一格式**，而送给引擎的运行时 `input/project.json` 则由 `stringifyProject` 重新生成为 `histream-project` 格式（见 2.5.4）。

路径存储规则 `projectStoredPath`：空串保持空；相对路径统一为正斜杠；位于工程目录内的绝对路径存为相对路径；其他绝对路径存为 Windows 绝对路径（`server.mjs:877-887`）。涉及字段：`outDir`、`meteo.path`、`scene.demFile`、各对象的 `fileName`/`positionFile`、各光谱的 `fileName` 及其 `physicalTexture/classTexture/opennessTexture.fileName`（`server.mjs:890-907`）。

打开工程时的一次性迁移 `migrateProject`：没有 OBJ 的首个非植被对象补默认房屋模型 `models/default_building.obj`（`server.mjs:2541-2553`）；气象路径被误写成 `…/C:…` 时回落内置 `defined/meteo.txt`；路径规范化；仅在有实际变化时才回写（`server.mjs:2556-2579`）。**运行（/api/run）不会迁移或改写源工程**，路径规范化只作用于该次调用的内存副本（`server.mjs:4984-4986`）。

#### 2.1.4 与旧 XML 的关系

历史上引擎读取 XML；当前引擎入口只接受名为 `project.json`、`format=histream-project`、`schemaVersion` 1–6、含 `configuration` 对象的文件（`models/histream/src/base/projectjson.cpp:95-96`、`models/histream/src/base/projectjson.cpp:106-115`；引擎侧上限常量 `kLatestProjectSchemaVersion = 6` 须与前端同步，`models/histream/src/base/projectjson.cpp:41`）。XML 现仅是引擎**内部**表示：

- Voxel/RayTracing：`FileIO::readJson` 把 JSON 转成 XML 风格的配置结构（`models/histream/src/base/fileio_json.cpp:1-19`）。
- Facet 两模式：`readTextFile` 对 `.json` 经 `facetInputView` 生成内存 XML 文本再用 `<tag>` 取值（`models/histream/src/facetrt/facetrtcore.cpp:4172-4179`）。
- `project-schema.js` 中 `projectToXml` 仍可导出 `<HiStreamProject>` XML（`src/renderer/src/project-schema.js:1794-1850`），但服务端运行路径不调用它；仓库内仅 `tools/test-vehicle-parts.mjs` 引用（`tools/test-vehicle-parts.mjs:19`）。
- `main.cpp` 的 `--builtin-example`（仅 eVoxelEB/eVoxelRT/eRaytracing）才使用引擎内置 XML 示例；空工程路径一律返回 64（`models/histream/src/main.cpp:135-149`）。

#### 2.1.5 与 USD 的关系

每次 `/api/project/save` 成功后，在后台线程把工程镜像导出为同目录 `project.usd`（失败只在响应 `usd` 字段报告，不影响保存）（`server.mjs:4917-4927`、`server.mjs:5557-5559`）。`usd/sync.json` 记录本程序最近写出/导入的各 USD 文件 SHA-256；覆盖 `project.usd` 前若无记录、哈希不符或该文件是“导入”登记的，先复制为 `project.external-<时间戳>.usd`（`server.mjs:2643-2708`）。打开 `.usd/.usda/.usdc/.usdz`：同目录有 `project.json` 且 USD 哈希与同步记录一致则直接按工程打开；否则调用 `histream-usd.exe read` 导出 OBJ 与 `usd-import.json`，按清单生成对象与位置文件，已有工程先备份为 `project.before-usd-import-<时间戳>.json`（`server.mjs:2719-2775`）。USD 不参与引擎运行输入。

### 2.2 HiStream 工程数据模型（schemaVersion = 6）

#### 2.2.1 顶层与计算模式

`PROJECT_MODES = ['eVoxelEB','eFacetEB','eFacetRT','eVoxelRT','eRaytracing']`（`src/renderer/src/project-schema.js:31`）。

| 模式 | 界面名称 | 求解特征 |
|---|---|---|
| eRaytracing | 简单光线追踪（`src/renderer/src/main.js:6449`） | 逐观测角追踪，输出 ENVI→TIFF，无时间节点（`models/histream/src/raytracing/raytracing.cpp:598-621`） |
| eVoxelRT | 体元辐射传输（`src/renderer/src/main.js:6448`） | 逐观测角/航点，输出 ENVI→TIFF（`models/histream/src/voxelrt/voxelrt.cpp:303-343`） |
| eVoxelEB | 体元辐射传输与能量平衡 | 对 `[start,end)` 内每个气象节点做能量平衡再观测，ENVI→TIFF（`models/histream/src/voxeleb/voxeleb.cpp:490-611`） |
| eFacetRT | 面元辐射传输 | 单次求解，写 `facetrt.json`，服务端转 TIFF（`models/histream/src/facetrt/facetrtcore.cpp:5823-5876`） |
| eFacetEB | 面元能量平衡 | 逐节点耦合迭代，支持 stdin 握手逐节点出图（`models/histream/src/facetrt/facetrtcore.cpp:9206-9217`） |

“能量平衡模式” = eFacetEB 或 eVoxelEB，只有它们按气象节点逐个输出（`server.mjs:1416-1418`）。引擎模式由 `/api/run` 请求体的 `mode` 决定，服务端只检查它在 `PROJECT_MODES` 内，**不与 `project.mode` 比较**（`server.mjs:4979`）；但 `validateProject` 的若干模式限制读取的是 `project.mode`（`src/renderer/src/project-schema.js:1502-1503`、`src/renderer/src/project-schema.js:1520`）。前端运行时传 `state.mode`（`src/renderer/src/main.js:6410`）；两者是否总一致待核对。

顶层字段（`createDefaultProject`，`src/renderer/src/project-schema.js:889-907`）：

| 字段 | 缺省 | 说明 |
|---|---|---|
| `schemaVersion` | 6 | 规范化总是写当前版本（`src/renderer/src/project-schema.js:1222-1223`） |
| `format` | `histream-project` | 同上 |
| `name` | `未命名工程` | |
| `mode` | `eVoxelEB` | 非法值回退缺省（`src/renderer/src/project-schema.js:1224`） |
| `createdAt` / `updatedAt` | 当前时刻 | 保存时刷新 `updatedAt`（`server.mjs:2504`） |
| `folders` | models/positions/spectral/meteorology/terrain/output | 与输入合并（`src/renderer/src/project-schema.js:1225`）；新建工程另建 `images` 目录（`server.mjs:2817`） |
| `configuration` | 见下 | 所有物理与运行输入 |

`configuration` 的节：`outDir`（缺省 `output`）、`scene`、`light`、`sensor`、`control`、`fluid`、`objects`、`spectra`、`canopies`、`thermals`、`materials`、`meteo`、`atmosphere`（`src/renderer/src/project-schema.js:908-1051`）。旧字段 `inputFile` 在规范化时丢弃（`src/renderer/src/project-schema.js:1067`）。

#### 2.2.2 scene 节（场景）

| 字段 | 缺省 | 规范化/校验 |
|---|---|---|
| `x`, `y`, `height` | 60, 60, 15 m | 规范化**原样展开不夹紧**（源码标 DEFERRED，`src/renderer/src/project-schema.js:1232-1233`）；校验要求 x>0、y>0、height≥0，否则“场景尺寸无效”（`src/renderer/src/project-schema.js:1497`） |
| `offsetX/Y/Z` | 0 | `number()` 解析；校验要求有限数（`src/renderer/src/project-schema.js:1235-1237`、`src/renderer/src/project-schema.js:1498`） |
| `voxel` | 1 m | 校验要求 >0（`src/renderer/src/project-schema.js:1499`） |
| `voxelFillThreshold` | 0.05 | 原样展开 |
| `terrain` / `demFile` / `demInfo` | false / '' / null | `terrain` 为真且有 `demFile` 才启用地形（`server.mjs:4379-4381`） |
| `terrainEnvironment` | airEnabled/soilEnabled=false；referenceHeight 2 m；temperatureLapseRate 0.0065 K/m；vaporScaleHeight 2000 m；drainageHeight 0；soilDry 0.08；soilValley 0.35；soilScaleHeight 30 m | 缺字段取缺省（`src/renderer/src/project-schema.js:47-63`）；校验：启用空气/土壤场仅支持 eVoxelEB；vaporScaleHeight>0、soilScaleHeight>0、0≤soilDry≤soilValley≤1（`src/renderer/src/project-schema.js:1500-1506`）。引擎同样在非 eVoxelEB 时抛错（`models/histream/src/base/engine.cpp:139-146`） |
| `background` | spectralName `soil`、thermalName `soil_temperature`、materialType `Soil`、materialName `soilset`、specularEnabled false、heterogeneityEnabled false、angularEffectStrength 0.5 | 预设别名转规范名；angularEffectStrength 限 0–1（`src/renderer/src/project-schema.js:1238-1253`） |

#### 2.2.3 light、sensor、meteo、atmosphere、fluid 节

light：缺省 zenith 30°、azimuth 135°、direct 0.8、diffuse 0.2、skyTemperature 250 K、shortwaveIrradiance 0、diffuseFractionMode `erbs`（新工程）（`src/renderer/src/project-schema.js:934`）。规范化：`direct` 夹到 0–1 且 `diffuse = 1 − direct`；`shortwaveIrradiance` 夹到 0–2000 W m-2（0 = 模型缺省）；`diffuseFractionMode` 仅 `erbs` 保留，否则 `fixed`——旧工程缺字段按 `fixed` 保持旧结果（`src/renderer/src/project-schema.js:1080`、`src/renderer/src/project-schema.js:1257-1259`）。zenith/azimuth 不校验（`src/renderer/src/project-schema.js:1232`）。

sensor（`src/renderer/src/project-schema.js:938-959`、`src/renderer/src/project-schema.js:1262-1287`）：

| 字段 | 缺省 | 规范化 |
|---|---|---|
| `x`, `y` | 512, 512 | 校验要求 >0（`src/renderer/src/project-schema.js:1507`）；服务端出图时取整为正整数，否则回退 512（`server.mjs:1635-1640`） |
| `bands` | `475 560 668 717 842 10500` | 波长 nm，空白/逗号/分号分隔（`src/renderer/src/project-schema.js:33`、`src/renderer/src/project-schema.js:939`） |
| `continuousBands`、`bandStart/End/Step` | false、400、2500、10 | 步长≥1e-6；连续波段容量 = 800 − 自定义波段数；末端不整除时追加终点；合并去重后升序截取至 800 个（`MAX_SENSOR_BANDS`）（`src/renderer/src/project-schema.js:64`、`src/renderer/src/project-schema.js:305-334`） |
| `projection` | `parallel` | 服务端把 `perspective/central/center` 视为透视，此时观测角固定 1 个（`server.mjs:341`） |
| `vza`, `vaa` | 0, 0 | 观测角；`principalPlane`（−75°…75°每 5°，两个平面）与 `hemisphere`（128 个等面积方向，VZA 0–75°）可叠加；天顶角夹到 [0, 89.999]；所有 VZA=0 方向只保留第一个（`src/renderer/src/project-schema.js:340-378`） |
| `positionX/Y/height`、`fov` | 场景中心、3000 m、60° | fov 夹到 0.1–120（`src/renderer/src/project-schema.js:1273`） |
| `cruise*` | 关闭；route `line`；step 25；count 20 | count 夹到 1–10000，step≥0.01，宽高≥step（`src/renderer/src/project-schema.js:1274-1284`） |
| `image / process / radiationProcess / energyProcess / temperature / albedo` | true / false / false / false / true / false | `radiationProcess`/`energyProcess` 缺字段时由旧 `process` 按模式推断：EB 模式归能量过程，其余归辐射过程（`src/renderer/src/project-schema.js:1285-1286`、`server.mjs:1422-1430`） |

校验：至少一个有效波段、至少一个有效观测角（`src/renderer/src/project-schema.js:1508-1509`）。

meteo（`src/renderer/src/project-schema.js:1035-1041`、`src/renderer/src/project-schema.js:1436-1451`）：

| 字段 | 缺省 | 规范化 |
|---|---|---|
| `path` | `defined/meteo.txt` | `defined/meteo.txt` 与 `HiStream 内置气象数据` 均指引擎目录内置气象（`server.mjs:335-336`、`models/histream/src/base/projectjson.cpp:168-173`） |
| `start` / `end` | 1 / 48 | start 取整且≥0；end 取整且≥start+1（节点区间 [start,end)，缺省共 47 个节点：1…47）（`src/renderer/src/project-schema.js:1082-1083`） |
| `z` | 15 | ≥0 |
| `Tsold` | 300 K | 夹到 150–400（单位 K；材质 `Tsoil` 为 ℃，混用会被静默夹紧，源码标 DEFERRED，`src/renderer/src/project-schema.js:1442-1443`） |
| `SatWater` | 0.45 | 0–1 |
| `dTime` | 1800 s | ≥1 |
| `Ca` | 380 | 夹到 100–2000 |
| `latitude/longitude` | 40 / 116 | ±90 / ±180 |
| `year/timezone` | 2019 / 8 | −2000…6000 取整；−12…14 |

atmosphere：缺省关闭，model `midlatitude-summer`（仅 tropical/midlatitude-summer/midlatitude-winter）、aerosol `rural`（仅 rural/urban）、waterVapor 2（0.5–5）、visibility 23（10–50）、lutFile `assets/atmosphere/simple_modtran_lut.csv`（`src/renderer/src/project-schema.js:1043-1050`、`src/renderer/src/project-schema.js:1388-1404`）。

fluid：缺省关闭；voxelSize 缺省沿用场景体元且≥0.1；timeStep≥0.1 s（缺省 5）；pressureIterations 取整夹 2–100（缺省 20）；windDirection 归一到 [0,360)；maxVelocity≥0.1（缺省 30）；outputHeight 必须>0，否则回退 2（`src/renderer/src/project-schema.js:1012-1027`、`src/renderer/src/project-schema.js:1362-1385`）。

#### 2.2.4 control 节（运行控制）

新工程缺省值与规范化规则（`src/renderer/src/project-schema.js:961-1010`、`src/renderer/src/project-schema.js:1290-1359`）：

| 字段 | 新工程缺省 | 规范化/说明 |
|---|---|---|
| `qualityPreset` | `standard` | 取值 fast/standard/high/custom；缺字段的旧工程记为 `custom`，不改动已有参数（`src/renderer/src/project-schema.js:1295-1296`） |
| `runTimeoutMinutes` | 0 | 0/空 = 服务端默认；否则取整夹 1–1440（`src/renderer/src/project-schema.js:124-127`） |
| `samples` | 32 | 不夹紧 |
| `depth` / `raytracingDepth` | 4 / 16 | `raytracingDepth` 夹 1–64 |
| `voxelCollisionOrders` | 3（旧工程 2） | 夹 2–5 |
| `shortwaveScatteringOrders` | 2（旧工程 1） | 夹 1–3 |
| `radiationMaxSteps` | 按场景对角线体元数×1.5，取 16 的倍数，限 64–4096（旧工程 64） | 夹 1–4096（`src/renderer/src/project-schema.js:103-107`） |
| `couplingIterations` | eFacetEB 60，其余 20 | 规范化不夹；引擎夹 1–100（`models/histream/src/facetrt/facetrtcore.cpp:5970-5972`） |
| `temperatureTolerance` / `temperatureRelaxation` | 0.05 K / 0.8 | 引擎夹 0.001–10 / 0.05–1（`models/histream/src/facetrt/facetrtcore.cpp:5973-5976`）；容差仅记录，收敛按能量残差判据（`models/histream/src/facetrt/facetrtcore.cpp:5984-5986`） |
| `stabilityCorrection` | 仅 eVoxelEB 为 true | 旧工程缺字段按 false（`src/renderer/src/project-schema.js:1004`、`src/renderer/src/project-schema.js:1350`） |
| `soilTemperatureMethod` | 2 | 夹 0–2 |
| `soilInertiaHistory` | `full` | 仅 `full` 保留，否则 `truncated`（旧工程） |
| `vegetationTemperatureMethod` | 0 | 夹 0–1 |
| `radiationSolver` | `accelerated` | 仅 `traditional` 保留 |
| `spectralAccelerationWidth` | 100 | 夹 1–1000 |
| `computeBackend` / `facetBackend` | `gpu` / `gpu` | 仅 `cpu`、`auto` 保留 |
| `cpuThreads` | 32 | 取整，≥1 且 ≤1024，否则 32；引擎再限制不超过逻辑处理器数（`src/renderer/src/project-schema.js:66-70`、`models/histream/src/base/engine.cpp:152-157`） |
| `periodicTraversalCount` | 0 | 夹 0–20；旧工程由 `periodicBoundary`+`periodicNeighborCount` 迁移 |
| `sampler` / `seed` | `sobol` / 0 | seed 夹 0–2147483647 |
| `facetVisibility` | `auto` | auto/raster/rayquery |
| `facetOutputFormat` | `binary` | binary/json；引擎自身缺省仍为 json，由工程字段决定（`src/renderer/src/project-schema.js:966-969`） |
| `voxelEmptySkip` / `voxelDirectionRotation(+Block)` / `voxelProxyGeometry` / `adaptiveSampling` | true / legacy(1) / positionNoIndex / false | 未知值回缺省；Block 仅 1/2/4/8/16 |
| `maxFacetTriangles` | 无 | 仅服务端读取，≥1000 的正整数覆盖面元上限（`server.mjs:94-95`） |

质量档位预设（只写所选模式实际读取的字段）（`src/renderer/src/project-schema.js:80-100`）：

| 档位 | samples | raytracingDepth | voxelCollisionOrders | shortwaveScatteringOrders | radiationMaxSteps | couplingIterations |
|---|---|---|---|---|---|---|
| fast | 16（eVoxelEB 为 32） | 8 | 2 | 1 | 64 | 20 |
| standard | 32 | 16 | 3 | 2 | 自动 | 60 |
| high | 128 | 32 | 5 | 3 | max(512, 因子取 3 的自动值) | 100 |

各模式受管字段：eRaytracing=samples+raytracingDepth；eVoxelRT=samples+voxelCollisionOrders；eVoxelEB=samples+shortwaveScatteringOrders+radiationMaxSteps；eFacetEB=couplingIterations；eFacetRT 无（`src/renderer/src/project-schema.js:93-99`）。

#### 2.2.5 objects 与预设库

`objects.items[]` 每项含 `name`、`type`（Building/Vegetation/Soil/Water/Human/Vehicle/Ship/Fire/Fog 等）、`fileName`（OBJ）、`positionFile`（实例位置）、`materialName/spectralName/thermalName/canopyName`、`meshes[]`（按 OBJ 分组的逐网格绑定）、`distribution`、`movement` 等；规范化时：预设别名转规范名（`water_radiation→water_surface`、`water_temperature_model→water_temperature`、`water_model→water_set`）；分布密度统一换算为每公顷；Fire/Fog 统一为 XYZ 坐标次序并标记仅参与辐射；仅 Human/Vehicle/Ship 保留 `movement`（`src/renderer/src/project-schema.js:237-243`、`src/renderer/src/project-schema.js:1146-1215`）。`count` 与 `names` 总由 `items` 重算（`src/renderer/src/project-schema.js:1406-1410`）。`movement` 仅用于三维预览，引擎不读取（`src/renderer/src/project-schema.js:279-281`）。`spectra/canopies/thermals/materials` 缺省取内置预设（`createMaterialPresets`，`src/renderer/src/project-schema.js:731-883`）；规范化补全开孔率（上限 `MAX_SURFACE_OPENNESS=0.95`）、纹理、镜面项、BSM 参数等（`src/renderer/src/project-schema.js:414`、`src/renderer/src/project-schema.js:1094-1140`）。

### 2.3 规范化与校验的行为约定

`normalizeProject(value)`：深拷贝输入（无 `structuredClone` 时 JSON 往返）；以 `createDefaultProject(input)` 为底；逐节合并并限幅；**不修改入参**；**不检查版本**（`src/renderer/src/project-schema.js:1060-1066`、`src/renderer/src/project-schema.js:1217-1219`）。`parseProjectJson` 则会拒绝非对象根、`format` 不符、`schemaVersion`>6（`src/renderer/src/project-schema.js:1457-1468`）。数值解析统一经 `number()`：`null`、`undefined`、空白串视为缺失取回退值，非有限数同理（`src/renderer/src/project-schema.js:35-40`）。

`validateProject(value)` 返回 `{valid, errors[], project}`，不抛异常（`src/renderer/src/project-schema.js:1475-1479`）。错误项分类：

| 类别 | 条件（错误文本摘要） |
|---|---|
| 根/版本/模式 | 根非对象；`format` 非 `histream-project`；`schemaVersion` 非 1–6 的整数；`mode` 不在五个模式内；`configuration` 非对象（`src/renderer/src/project-schema.js:1481-1493`） |
| 场景/传感器 | 2.2.2、2.2.3 所列条件 |
| 光伏（`energyModel=photovoltaic`） | eta25∈[0,0.5]、gamma∈[−0.02,0]、bifaciality∈[0,1]、heatCapacityPerArea≥0、convectiveScale>0；不能作背景地面；被使用时 `project.mode` 不得为 eVoxelEB（`src/renderer/src/project-schema.js:1512-1520`） |
| 薄膜（`membrane`） | 面热容、对流倍率>0；不能作背景；被使用时不得为 eVoxelEB（`src/renderer/src/project-schema.js:1522-1531`） |
| 墙体（`wallMode=wall`） | 0<厚度≤5 m；室内温度∈(−70,120)；换热系数、λ、ρ、cp>0；`layers` 为 1–6 层且总厚度≤5 m（`src/renderer/src/project-schema.js:1534-1545`） |
| 车辆/船舶/水体 | 部件取值、座舱/内饰/发动机份额之和=1 及各范围；船舶航速 0–100 m/s、水线与水温（−20…60 ℃）、航向/风向 0–360°；水体初始温度模式（`src/renderer/src/project-schema.js:1547-1612`） |
| 光谱 | 开孔率/开孔率纹理不支持 Prospect/BSM；分类纹理需 PNG、至少一个类别光谱、引用存在且不可嵌套（`src/renderer/src/project-schema.js:1614-1632`） |

校验失败时的行为：`/api/project/save` 返回 400，错误以“；”连接（`server.mjs:4917-4918`）；`/api/run` 与 `/api/run/preflight` 返回 400，信息前缀“工程配置不合法：”（`server.mjs:4941-4942`、`server.mjs:4988-4989`）。校验不覆盖的字段（如 `scene.x` 的上界、`light.zenith`、`control.samples`）规范化后原样送给引擎（`src/renderer/src/project-schema.js:1232`）；引擎对其中部分有自己的 clamp（如 2.2.4）。

### 2.4 运行前预检 POST /api/run/preflight

输入：`{mode, inputPath?}`；`inputPath` 缺省用当前工程（`server.mjs:4933-4937`）。流程：`modes.has(mode)` → `resolveProjectFile` → `readProject` → `normalizeStoredProjectPaths` → `validateProject` → 在后台线程执行 `runPreflight`（读 OBJ、遍历 `runs/`）（`server.mjs:4936-4944`）。任何一步失败返回 400 `{error}`；预检**不检查引擎白名单、不创建运行目录、不被 `child` 互斥拦截**。前端预检接口不可用时只写警告并放行（`src/renderer/src/main.js:6341-6345`）。

输出字段（`server.mjs:1182-1283`）：

| 字段 | 含义 |
|---|---|
| `mode, nodes, angles, bands` | 节点数（EB 模式 = end−start，其余 1）；观测角数（透视为 1；否则 `sensorViewAngles` 个数，≥1）；波段数（≥1） |
| `timeoutSeconds, timeoutSource, serverTimeoutSeconds` | 生效上限（`project`/`server`）与服务端默认 |
| `facet`（面元模式） | `triangles`（预估三角面数）、`limit`、`raster`（场景较大边≥1000 m 取 2048，否则 1024）、`backend`、光线查询显存估计（加速结构 53 B/面 + 每侧 48 B/面）或 A-buffer 估计（raster²×64×8 B）、结果文件体积估计（JSON 580 B/面、binary 195 B/面）、`periodicTiles`（周期遍历次数向上取整到 4/8/12/20） |
| `voxel`（体元模式） | 网格 `ceil(尺寸/体元)`、总格数、活动体元上界（5 %）、`radianceMiB`（VoxelRT 每体元 (64+2×波段数)×8 B，VoxelEB 64×12 B） |
| `estimateSeconds, estimateBasis, suggestedTimeoutMinutes` | 取同模式历史运行（`status` 为 completed/non_converged 且有 `finishedAt`）最近 3 次的单位耗时中位数外推；超限时建议上限 = 预计×1.5 向上取整分钟，≤1440 |
| `warnings[]` | 面元超限/超 60 %；结果文件>8 GiB；体元网格>4×10^8 格或辐射场>16 GiB；预计耗时超限/超 80 %；无历史且节点>24 |

预检只给提示，不阻止运行；真正拦截面元超限的是运行阶段的 `assertFacetScale`（2.5.3）。

### 2.5 运行 POST /api/run 全流程

请求：`{mode, inputPath?, executable?}`；成功响应 200 `{pid, inputPath, runId, outputDir}`；任一步失败 400 `{error}`（`server.mjs:5301`、`server.mjs:5322`）。

#### 2.5.1 互斥与输入校验

1. 正在重置 → “模拟环境正在重置”；已有 `child` → “已有模拟任务正在运行”；`runPreparing` 为真 → “已有模拟任务正在准备”（`server.mjs:4967-4970`）。同一时刻最多一个运行。
2. 置 `runPreparing=true`、`runPreparationCanceled=false`；之后每个 `await` 后调用 `assertPreparationActive()`，被 `/api/stop`、`/api/reset` 置位则抛“模拟准备已取消”（`server.mjs:4971-4978`）。
3. 校验 `mode`；`resolveProjectFile`；当前工程切换为该文件；`readProject` + `normalizeStoredProjectPaths` + `validateProject`（`server.mjs:4979-4989`）。
4. `allowedEngine(executable)`：空则取默认引擎；否则只允许内置候选路径或环境变量 `STREAM_STUDIO_ENGINE_ALLOWLIST`/`HISTREAM_STUDIO_ENGINE_ALLOWLIST`（按平台分隔符）列出的路径，Windows 下忽略大小写并比较 realpath；不在其内抛错（`server.mjs:196-211`）。内置候选顺序：`<资源根>/engine/histream.exe`、`models/bin_x64/Release|Debug/histream.exe`、`models/histream/bin/Debug|Release/histream.exe`（`server.mjs:124-130`）。不存在则“找不到 HiStream”（`server.mjs:4992`）。

#### 2.5.2 createIsolatedRun：运行目录与 manifest

`createIsolatedRun(input, project, mode, engine)`（`server.mjs:329-382`）：

- `runId = <UTC ISO 时间戳，':' 与 '.' 换成 '-'>-<8 位 UUID 前缀>`，如 `2026-11-17T00-11-54-160Z-be7bfac9`；运行目录 `<outDir 解析后>/runs/<runId>`，`outDir` 相对工程目录解析，缺省 `output`（`server.mjs:331-333`）。
- 气象：`meteo.path` 为空或内置标记 → `<引擎目录>/defined/meteo.txt`；否则按 `projectAssetPath` 解析（`server.mjs:334-336`）。计算 `{path, bytes, sha256}`；**能量平衡模式找不到气象文件直接抛错**（`server.mjs:337-338`）；其余模式 `meteorology` 记为 `null`。
- 同时计算源工程与引擎的哈希；哈希按 (路径, mtime, size) 缓存，哈希期间文件被改则不缓存（`server.mjs:237-280`）。
- 节点：区间规整与引擎一致，`start=max(0,round(start))`，`end=max(start+1,round(end))`；能量平衡模式展开 `nodes=[start…end−1]`，超过 100000 个抛错；非 EB 模式 `nodes=[]`、`nodeCount=1`（`server.mjs:345-355`）。
- 写入 `build`：引擎目录若有 `build-manifest.json`，记录其身份与 `applicationVersion`、`sourceTreeSha256`；读取失败记 `unreadable:true`（`server.mjs:371-377`）。
- 创建目录，复制源工程为 `source-project.json`，原子写 `run-manifest.json`（先 `.tmp` 再 rename，且解析两端 realpath 以兼容中文目录联接点）（`server.mjs:378-381`、`server.mjs:284-291`）。

run-manifest.json 字段（`server.mjs:356-370`、`server.mjs:5026`、`server.mjs:5082`、`server.mjs:5260-5271`；与 `.test/examples-run/2026-11-17T00-11-54-106Z/projects/crop_gpp_npp/output/runs/2026-11-17T00-11-54-160Z-be7bfac9/run-manifest.json` 实例一致）：

| 字段 | 写入时机 | 含义 |
|---|---|---|
| `kind`=`histream-run`、`schemaVersion`=1 | 创建 | 标识；结果读取/删除据此识别运行目录（`server.mjs:2979-2987`） |
| `runId`、`mode`、`startedAt`、`outputDir` | 创建 | `outputDir` 为 Windows 形式路径 |
| `status` | 全程 | 见 2.9 状态机：preparing/running/completed/non_converged/stopped/timeout/failed |
| `sourceInput`、`engine`、`meteorology`、`build` | 创建 | 各为 `{path,bytes,sha256}`；`meteorology` 后续追加 `snapshot` |
| `requested` | 创建 | `nodes`、`nodeCount`、`viewAngles`（透视取 `[[vza,vaa]]`）、`observation{planeAzimuth, principalPlane, hemisphere, projection}`、`image`、`process` |
| `completed` | 创建（空）→ 逐步更新 | `nodes[]`、`nodeCount`、`products[]`（相对路径） |
| `runtimeInput`、`inputDirectory` | 冻结后 | 运行时 `input/project.json` 的身份与 `input/` 目录 |
| `pid`、`command` | spawn 后 | 引擎 PID 与完整命令行数组 |
| `finishedAt`、`exitCode`、`signal` | 终态 | |
| `error`、`warnings`、`runWarnings`、`computedNodes` | 视情况 | 失败原因；ENVI 转换失败列表；面元质量提示；引擎自写的 `faceteb_run.json.completed` |
| `updatedAt` | 每次写入 | |

`verifyFrozenRunInputs`：出图前复核冻结工程与气象快照的 SHA-256 与 manifest 一致，不一致则拒绝生成影像（`server.mjs:296-306`）。注意被冻结并哈希的只有运行时 `project.json` 与气象快照；未裁切的 OBJ、DEM、光谱文件等以绝对路径引用原文件，不被复制也不被哈希（`tools/runtime-scene.mjs:329-352`）。

随后在内存中克隆工程，把 `outDir` 改为运行目录，并在有气象身份时把 `meteo.path` 改为 `meteoPath`（`server.mjs:4997-4999`），使每次运行输出互不覆盖。

#### 2.5.3 运行时场景与冻结 input/

后台线程调用 `prepareRuntimeSceneProject(input, project, {resolveAssetPath, terrainHeight})`（`server.mjs:5505-5511`、`server.mjs:5541-5554`），其行为如下（`tools/runtime-scene.mjs:287-506`）：

1. 暂存目录 `<工程目录>/.histream-runtime-<pid>-<时间>-<随机>`；`outDir`、DEM、气象（内置标记除外）、大气 LUT、光谱及纹理、对象 OBJ 与位置文件全部转绝对路径，因为运行时 project.json 位于另一个目录（`tools/runtime-scene.mjs:300-352`）。
2. 计算域：X∈[offsetX, offsetX+scene.x]；高度 Y∈[offsetY, offsetY+scene.height]；Z∈[offsetZ, offsetZ+scene.y]——**工程 `scene.y` 对应运行时 Z 轴**（`tools/runtime-scene.mjs:295-299`、`tools/runtime-scene.mjs:15-16`）。
3. 无 `positionFile` 的对象原样保留（计 1 个实例）；有位置文件的：位置文件每行前三列读为 x、z、y（y 为高度），可选缩放（缺省 1）、旋转角（度，缺省 0）；支持 `#`、`//` 注释；格式非法的行计入 `invalid`；找不到位置文件或 OBJ 直接抛错（`tools/runtime-scene.mjs:31-51`、`tools/runtime-scene.mjs:365-378`）。
4. **实例边界筛选**：局部包围盒（OBJ 取顶点包围盒，Y 范围取“最低点贴地”与“原始高程”两种解释的并集，保证跨界对象不被提前丢弃；无 OBJ 的基元取 `dimensions`）经缩放→绕 Y 轴旋转（−rotation）→平移后的轴对齐包围盒，与计算域不相交则丢弃并计入 `excluded`；相交（含边界）则保留（`tools/runtime-scene.mjs:63-142`、`tools/runtime-scene.mjs:397-402`）。
5. **贴地**：车辆（`type=Vehicle`）每个实例的高度按足迹落到地形接触点计算，覆盖位置文件高度；无地形时按 0 m（`tools/runtime-scene.mjs:313-314`、`tools/runtime-scene.mjs:387-396`）。
6. **OBJ 预裁切**：仅当对象恰有 1 个实例且包围盒越出计算域时才裁切（多实例整体保留，不拆分）；对每个三角形做 Sutherland–Hodgman 裁剪（X、Z，无地形时再加高度），容差 1e-8，逆变换回局部坐标，每个子三角形独立 3 个顶点；写入 `object-<索引>.obj`，文件首写一个占位顶点 `v 0 <原最小Y> 0` 以免裁切改变面元求解器的最低点贴地基准；裁切后无三角形则整个实例排除（`tools/runtime-scene.mjs:403-428`、`tools/runtime-scene.mjs:237-280`）。
7. 保留实例的坐标减去 offset，按 x、z、y 顺序写 `position-<索引>.txt`（列：x z y scale rotation），对象 `positionFile` 改为基名；展开表面分类/开孔率纹理（`applySurfaceTextures`，可能改写 OBJ 与光谱，产物名 `object-<n>-textured.obj`）；开孔率光谱展开为有效光谱（必要时写 `spectrum-<n>.txt`）（`tools/runtime-scene.mjs:429-475`）。
8. 用保留对象替换 `objects.items`（`count`、`names` 同步），`offsetX/Y/Z` 归零，以 `stringifyProject` 写出 `project.json`（`histream-project` 格式）（`tools/runtime-scene.mjs:477-487`）。出错则自行清理（`tools/runtime-scene.mjs:502-505`）。
9. DEM：运行时场景只在“车辆贴地”时用 DEM 高度采样器（双线性，行 0 为北缘，缺测按最低值，高度相对 DEM 最小值）；DEM 本身以绝对路径写入运行时工程交引擎读取（`server.mjs:4379-4396`、`tools/runtime-scene.mjs:336-338`）。

服务端随后：校验暂存目录名以 `.histream-runtime-` 开头且不在目标内；`cp -r` 暂存目录 → `runs/<runId>/input/`（异步，避免阻塞事件循环）（`server.mjs:5006-5013`）；有气象身份时把气象复制为 `input/meteo.txt`，改写 `input/project.json` 的 `meteo.path`，计算快照身份，**快照哈希与创建时不一致则取消本轮**（“气象文件在准备运行期间被修改”）（`server.mjs:5015-5025`）；写入 `runtimeInput`、`inputDirectory`（`server.mjs:5026`）。向前端发 stdout 事件：“实例边界筛选：原始 N 个，保留 M 个，完整包围盒在域外跳过 K 个……”（`server.mjs:5028-5031`）。

面元模式（eFacetRT/eFacetEB）在 `input/` 上再做**规模预检** `assertFacetScale`：三角面总数 = 2 + Σ(OBJ 三角形数 × 实例数)（Fire/Fog/介质不计；无 OBJ 的对象计 12），超过上限抛错并推荐改用体元模式。上限优先级：工程 `control.maxFacetTriangles` > 环境变量 `STREAM_STUDIO_MAX_FACET_TRIANGLES` > 缺省 20 000 000（`server.mjs:5035-5040`、`server.mjs:1103-1118`、`server.mjs:88-95`、`server.mjs:1079-1100`）。同时输出植被面元/像元面积比提示（`server.mjs:1046-1063`）。

#### 2.5.4 spawn 引擎

保留三维结果的判据 `keepThreeDimensionalResults`：启用了辐射/能量过程输出（EB 模式二者任一，其余仅辐射过程），或 eFacetEB 中有被引用的光伏材质（`server.mjs:5041-5053`、`server.mjs:1433-1436`）。

命令行（引擎入口 `histream <mode> <project.json> [output.json]`，`models/histream/src/main.cpp:10`）：

| 模式 | 参数 | 工作目录 | stdin |
|---|---|---|---|
| eFacetRT / eFacetEB | `[mode, input/project.json, 结果JSON]`；结果 JSON 为 `<runDir>/facetrt.json` 或 `faceteb.json`（保留结果时），否则 `<runDir>/.histream-transient-<mode>.json`（出图后删除） | 引擎所在目录 | eFacetEB 为 pipe，eFacetRT 为 ignore |
| eVoxelRT / eVoxelEB / eRaytracing | `[mode, input/project.json]` | 工程目录 | ignore |

（`server.mjs:5054-5078`）。路径经 `histreamRuntimePath` 转换（仅 Linux 通过 WSL 运行 .exe 时 `/mnt/x/…` 还原为 `X:\…`，`server.mjs:857-864`）。

环境变量：`{...process.env, HISTREAM_FACET_STEP_SYNC: '1'(仅 eFacetEB) | '0'}`（`server.mjs:5075-5077`）；其余 `HISTREAM_*` 开关（如 `HISTREAM_COMPUTE_BACKEND`、`HISTREAM_FACET_VISIBILITY`、`HISTREAM_RT_SEED` 等）由用户环境透传，引擎读取后覆盖工程字段（`models/histream/src/base/engine.cpp:95-96`、`models/histream/src/facetrt/facetrtcore.cpp:381`）。引擎自身在 `input` 阶段设置 `HISTREAM_CPU_THREADS` 与 `LP_NUM_THREADS`（`models/histream/src/base/engine.cpp:157-165`）。spawn 参数 `windowsHide:true`；stdout/stderr 用 UTF-8（`server.mjs:5076`、`server.mjs:5120-5121`）。

引擎自身的启动顺序（`models/histream/src/main.cpp:99-193`）：解析模式（扩展模式/`--list-devices` 先处理）→ 校验模式名与路径 → `engine.input`（读工程、设线程；Voxel/RT 在此上传场景；失败退出码 4）→ `engine.create`（失败 3）→ `engine.run`（返回码即退出码，异常 1）→ `engine.destroy`（运行成功但清理失败改为 1）。引擎只接受文件名为 `project.json` 的输入（`models/histream/src/base/projectjson.cpp:95-96`）——这就是服务端把运行输入固定命名为 `input/project.json` 的原因。引擎在 `input` 阶段还拒绝：非 eVoxelEB 启用地形环境、非 eRaytracing 启用网格镜面反射（`models/histream/src/base/engine.cpp:139-148`），表现为退出码 4。

spawn 成功后：manifest 写 `status:'running'`、`pid`、`command`；`child.histreamRun={runId,outputDir,startedAt,mode}`；启动超时计时器；发 `started` 事件（`server.mjs:5079-5082`、`server.mjs:5118-5119`）。

### 2.6 stdout / stderr 协议与 SSE 事件

#### 2.6.1 SSE 事件

`GET /api/events` 建立 SSE：先发注释 `: connected`，随即发一条 `status` 事件（`running`、`pid`、`runId`、`outputDir`、`startedAt`、`mode`），之后由 `emit()` 向所有客户端广播 `data: <JSON>\n\n`；无事件 id、不重放（`server.mjs:4625-4634`、`server.mjs:749-752`）。

| `type` | 字段 | 触发 |
|---|---|---|
| `status` | `running, pid, runId?, outputDir?, startedAt?, mode?` | 连接时；前端据此重新接管运行中任务（`src/renderer/src/main.js:6617-6631`） |
| `started` | `pid, runId, outputDir, command` | spawn 后 |
| `stdout` | `text`，引擎原始输出带 `engine:true` | 引擎 stdout 的每个数据块（可能被任意切块，前端按行重组，`src/renderer/src/main.js:6648-6657`）；服务端自己的提示是整行文本 |
| `stderr` | `text` | 引擎 stderr |
| `error` | `text` | 超时、节点出图失败、spawn 错误、ENVI 转换失败等 |
| `closed` | `code, signal, status, runId, outputDir, elapsed, warnings?, runWarnings?` | 终态；`elapsed = Date.now() − spawn 前时刻`，含后处理（`server.mjs:5067`、`server.mjs:5272-5274`） |

#### 2.6.2 引擎输出行格式（制表符分隔）

| 行 | 格式 | 来源/说明 |
|---|---|---|
| `PROGRESS` | `PROGRESS\t<百分比整数>\t<阶段文字>` | 前端以 `^PROGRESS\s+(\d+)\s+(.+)$` 解析并更新进度条（`src/renderer/src/main.js:6480-6486`）。各模式进度：RT 5→10+85·(k+1)/n→100（`models/histream/src/voxelrt/voxelrt.cpp:282`、`models/histream/src/voxelrt/voxelrt.cpp:340`、`models/histream/src/voxelrt/voxelrt.cpp:345`）；VoxelEB 5、10、10+85·完成节点/总节点、100（`models/histream/src/voxeleb/voxeleb.cpp:481-482`、`models/histream/src/voxeleb/voxeleb.cpp:559-562`、`models/histream/src/voxeleb/voxeleb.cpp:614`）；FacetEB 3、5、10…10+86·完成/总节点（≤96）、100（`models/histream/src/facetrt/facetrtcore.cpp:6169-6170`、`models/histream/src/facetrt/facetrtcore.cpp:9178-9185`、`models/histream/src/facetrt/facetrtcore.cpp:9379`） |
| `OBSERVATION` | `OBSERVATION\t    vza_<v>    vaa_<v>    sza_<v>    saa_<v>`，巡航模式为 `OBSERVATION\t航点 i/n X=.. Y=.. H=..` | 前端转“观测角度 · …”日志（`models/histream/src/voxeleb/voxeleb.cpp:592-599`、`src/renderer/src/main.js:6493-6494`） |
| `RESULT` | `RESULT\t<结果JSON路径>` | 仅面元模式结束时输出（`models/histream/src/facetrt/facetrtcore.cpp:5875`、`models/histream/src/facetrt/facetrtcore.cpp:9380`） |
| `PROCESS` | `PROCESS\t<过程元数据路径>` | 体元过程输出（`models/histream/src/voxelrt/voxelrt.cpp:501`） |
| `FACET_CONVERGENCE` | `FACET_CONVERGENCE\t<node>\t<0\|1>\t<迭代数>\t<最大温差K>\t<能量残差W/m²>\t<等效K>` | FacetEB 每节点；服务端在握手模式下记入 manifest（`models/histream/src/facetrt/facetrtcore.cpp:9186-9188`、`server.mjs:5140-5144`） |
| `FACET_TIMING` | `FACET_TIMING\t<node>\t<短波s>\t<耦合s>\t<出图s>` | FacetEB 每节点（`models/histream/src/facetrt/facetrtcore.cpp:9198-9201`） |
| `FACET_STEP` | `FACET_STEP\t<node>\t<DOYddd_hh-mm>\t<儒略时间>\t<太阳天顶角>\t<太阳方位角>` | 握手专用（见 2.7） |
| `ENERGY_BALANCE` | `ENERGY_BALANCE\tnode=..\titerations=..\t…\tstatesOutsideTolerance=..\t…` | VoxelEB 每节点；**VoxelEB 未收敛只体现在该计数，不产生退出码 2**（`models/histream/src/voxeleb/voxeleb.cpp:1210-1214`、`src/renderer/src/main.js:6528-6536`） |
| 其他诊断 | `SHORTWAVE_SPLIT`、`SHORTWAVE_SCATTERING`、`VEHICLE_CABIN`、`FACET_SOLVER_DIAG` | 前端 `recordRunQualityLine` 汇总为“运行质量”（`src/renderer/src/main.js:6513-6559`） |

前端丢弃以 `cube`、`OBJ placement` 开头的行（`src/renderer/src/main.js:6479`）。字段顺序依赖引擎输出，变更会使解析失败（`src/renderer/src/main.js:6510`）。

### 2.7 FacetEB 的 stdin 节点握手

仅 eFacetEB 启用（`HISTREAM_FACET_STEP_SYNC=1`，stdin 为 pipe）。引擎端：每个节点计算完成后输出 `FACET_STEP …` 并**阻塞读取 stdin 一行**，必须恰为 `FACET_ACK\t<node>`；EOF 或内容不符则抛 `FacetEB time-step output was interrupted or failed` 终止（`models/histream/src/facetrt/facetrtcore.cpp:9204-9217`）。握手模式下引擎先把几何占位写入结果文件，供逐节点出图读取（`models/histream/src/facetrt/facetrtcore.cpp:6782-6799`）。

服务端（逐行读取 stdout，兼容分块与 CRLF）（`server.mjs:5135-5179`）：

1. 收到 `FACET_CONVERGENCE`：缓存该节点诊断（node、converged、iterations、temperatureDelta、energyResidual、等效温度残差）。
2. 收到 `FACET_STEP`（已被停止/重置时忽略）：**暂停超时计时** → 用正则 `^FACET_STEP\t(\d+)\t(DOY\d+_\d{2}-\d{2})\t(儒略时间)(?:\t天顶角\t方位角)?$` 解析，不合法或上一节点仍在处理则报“面元节点输出协议异常” → 发 stdout 提示“节点 N 已计算，正在逐方向保存图像” → `verifyFrozenRunInputs` → `exportFacetStep`：面元数≤3 000 000（`STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS` 可调）时复用本次运行常驻的出图线程，更大时每节点新建一次性线程（`server.mjs:2396`、`server.mjs:2430-2459`）；对每个观测角生成 TIFF，全部成功后才删除该节点的 `.facet_steps/energy_T=<token>.bin`（`server.mjs:2362-2377`）。
3. 成功：`completed.nodes` 追加 `{...step, ...诊断}`，`nodeCount` 更新，manifest 的 `completed.products` 刷新，**恢复计时**，向 stdin 写 `FACET_ACK\t<node>\n`。
4. 失败：置 `stepOutputError`，发 `error` 事件“节点影像生成失败（已保留节点文件）”，恢复计时，向 stdin 写 `FACET_ERROR\n` 并结束 stdin，引擎因读到非 ACK 而中止（`server.mjs:5170-5177`）。

非 FacetEB 模式不握手；面元 eFacetRT 在引擎退出后一次性出图（2.8）。

### 2.8 停止、重置、超时与退出码

#### 2.8.1 超时 runTimeoutMinutes

上限 = 工程 `control.runTimeoutMinutes`（1–1440 分钟）> 环境变量 `STREAM_STUDIO_RUN_TIMEOUT_MS`（读取 `process.env`，无 `HISTREAM_STUDIO_` 别名）> 15 分钟（`server.mjs:106-111`）。计时器在 spawn 后立即启动，`unref`；只统计引擎执行时间：FacetEB 节点出图期间暂停，引擎退出时（后处理前）再暂停（`server.mjs:5083-5118`、`server.mjs:5194`）。到点：置 `histreamTimedOut`、发 `error` 事件“运行超过 N 分钟，已终止”，Windows 用 `taskkill /pid <pid> /T /F` 结束进程树，失败或非 Windows 退回 `SIGKILL`（`server.mjs:5090-5105`）。终态 `timeout`，退出码缺失时记为 1（`server.mjs:5204`）。

#### 2.8.2 停止 POST /api/stop

- 准备阶段（无 `child` 但 `runPreparing`）：只置 `runPreparationCanceled`，响应 `{stopped:true, preparing:true}`（`server.mjs:5334-5337`）。由于 catch 块先写 `stopped`、随后又无条件写 `failed`（`server.mjs:5304-5312`、`server.mjs:5321`），**代码实际的终态是 `failed`，`error`=“模拟准备已取消”**；是否为设计意图待核对。
- 运行中：置 `histreamStopped`，终止面元出图线程，Windows `taskkill /T /F`，其他平台 `SIGTERM`；引擎已退出只剩后处理时只终止后处理线程，不再按 PID 杀进程（PID 可能被复用）（`server.mjs:5338-5349`）。终态 `stopped`。
- 无任务：`{stopped:false}`。

#### 2.8.3 重置 POST /api/reset

准备阶段同上只置取消标志；否则 `resetSimulationProcesses`：置 `histreamReset`，终止后处理线程，结束本服务启动的引擎进程树（不波及其他实例），复核 PID 仍存在则回滚状态并报错（`server.mjs:5352-5361`、`server.mjs:402-433`）。重置后 `child` 立即清空；`completeHistreamRun` 见 `histreamReset` 即走 `finishReset`：manifest 写 `stopped`（无 `exitCode`）、清理运行时场景，**不发 `closed` 事件**（`server.mjs:5196-5202`）。

#### 2.8.4 退出码表

| 码 | 引擎含义（`models/histream/src/main.cpp:50-56`） | 引擎出处 | 服务端处理 |
|---|---|---|---|
| 0 | 成功 | `engine.run` 返回 | 结果码，后处理，`completed` |
| 1 | 运行失败（异常、清理失败、FacetEB 内部错误） | `models/histream/src/main.cpp:185-192`、`models/histream/src/facetrt/facetrtcore.cpp:9384-9386` | `failed` |
| 2 | FacetEB 已完成但有节点未收敛，结果保留 | `models/histream/src/base/engine.cpp:296-298`、`models/histream/src/facetrt/facetrtcore.cpp:9383` | 仅 eFacetEB 视为结果码，`non_converged`；其他模式的 2 → `failed` |
| 3 | 资源创建失败（亦被 FacetEB CPU/GPU 影子审计不一致复用） | `models/histream/src/main.cpp:167-175`、`models/histream/src/facetrt/facetrtcore.cpp:9383` | `failed` |
| 4 | 初始化失败（工程读取/校验失败、模式不兼容） | `models/histream/src/main.cpp:159-163` | `failed` |
| 5 | 设备查询失败（仅 `--list-devices`） | `models/histream/src/main.cpp:117-124` | 不在运行路径；`/api/devices` 以缺 `HISTREAM_DEVICES` 行报错（`server.mjs:217-231`） |
| 64 | 用法错误（模式名非法、缺工程路径） | `models/histream/src/main.cpp:56`、`models/histream/src/main.cpp:106`、`models/histream/src/main.cpp:133` | 追加“引擎参数用法错误（退出码 64）”错误并 `failed`（`server.mjs:5208-5211`） |

服务端常量表 `HISTREAM_EXIT` 与上表一致（`server.mjs:119`）；`histreamResultCodes(mode)`：eFacetEB 为 [0,2]，其余 [0]（`server.mjs:120-121`）。

### 2.9 结束阶段 completeHistreamRun 与终态

引擎 `close`（所有 stdio 关闭）后异步执行 `completeHistreamRun`；期间 `child` 仍指向本次运行，界面保持“运行中”，新运行与删除结果被拒绝（`server.mjs:5279-5281`、`server.mjs:5367`）。步骤（`server.mjs:5192-5277`）：

1. `histreamFinished=true`，暂停计时，终止遗留出图线程；若 `histreamReset` 走 `finishReset`（2.8.3）。
2. `finalCode`：有 `stepOutputError` 则 1，否则为退出码；超时且退出码为空则 1。
3. 面元质量警告：退出码属结果码且为面元模式时，在清理 JSON 前读取 `faceteb_run.json` 与结果 JSON 首尾各 256 KiB 中的 `zeroVisibilitySamples`（逐节点取最大），**>5 %** 则生成 `runWarnings`（“约 x % 的接收面没有得到任何可见性样本…”），并逐条发 stdout“提示：…”（`server.mjs:1123-1165`、`server.mjs:5213-5217`）。
4. 面元结果→TIFF：结果码、面元模式、没有逐节点出图且无错误时，在一次性后台线程执行 `facet-tiff`：对每个观测角写多波段 float32 TIFF，必要时写过程量，并随每个 TIFF 追加 `result_statistics_<mode>.csv`（逐波段 count/min/max/mean/总体 stddev，Welford 算法）；失败则 `finalCode=1`、记录 `stepOutputError`；`finally` 中若不保留三维结果则删除结果 JSON（`server.mjs:5218-5237`、`server.mjs:3076-3109`、`server.mjs:3238`）。握手出过图的运行也在结果码下按同条件删除 JSON（`server.mjs:5238`）。
5. ENVI→TIFF：非面元模式且 `finalCode` 属结果码，一次性后台线程 `envi-convert`：能量平衡模式只转文件名带 `T=` 的 `.hdr`，辐射模式只转不带的；仅支持 float32（data type 4）、小端、BSQ、header offset 0；按文件名 `T/VZA/VAA/SZA/SAA` 生成标准 TIFF 名；成功后删除 `.hdr/.img`（eVoxelRT/eVoxelEB/eRaytracing）；单文件失败记入 `warnings`、发 `error` 事件并保留原文件，**全部失败才使运行失败**（`server.mjs:5239-5255`、`server.mjs:3274-3339`）。
6. 节点登记：读 `faceteb_run.json`，有 `completed` 且 `finalCode` 属结果码则覆盖 `completed.nodes`；`computedNodes` 记引擎自报的完成列表；无引擎索引、非握手且 `finalCode===0` 时把请求节点全部记为 `{node, completionSource:'engine-exit'}`；`nodeCount` = 节点数，非 EB 且成功时为 1（`server.mjs:5256-5263`）。
7. 终态：**超时 > 用户停止 > FacetEB 未收敛（`finalCode===2`）> 成功（0）> 失败**，即 `timeout`/`stopped`/`non_converged`/`completed`/`failed`（`server.mjs:5264-5266`）。写 manifest（`finishedAt, exitCode, signal`、可选 `error/warnings/runWarnings`、刷新 `completed.products`），发 `closed`，清理运行时场景（`server.mjs:5267-5276`）。
8. `completeHistreamRun` 抛异常：若 manifest 仍为 `running` 改写 `failed`，发 `error`，并在未处于任何终态时补发 `closed{status:'failed'}`（`server.mjs:5282-5299`）。spawn 错误（`error` 事件）：写 `failed`，无 PID 时释放 `child`，**不发 `closed`**（`server.mjs:5180-5185`）；spawn 同时是否触发 `close` 待核对。

`runProductPaths` 登记产物：运行目录根、`process/`、`.facet_steps/` 下的文件（相对路径、排序），排除 `run-manifest.json(.tmp)`、`source-project.json`、`.histream-transient-*`（`server.mjs:310-320`）。

#### 2.9.1 状态机（文字版）

```
                      POST /api/run
                            │ 互斥通过（无 child / 非 preparing / 非 resetting）
                            ▼
                    ┌──────────────┐   校验/哈希/裁切/复制/气象快照/面元规模 任一步抛错
                    │  preparing   │──────────────────────────────────────────────┐
                    └──────┬───────┘   /api/stop|reset（置取消标志）→ 抛“已取消”     │
                           │ spawn 成功，写 pid/command                              ▼
                    ┌──────▼───────┐                                        ┌────────────┐
                    │   running    │── 超时(runTimeoutMinutes) ──► kill ──► │  timeout   │
                    │ (含节点出图、 │── /api/stop ───────────────► kill ──► │  stopped   │
                    │  引擎计算)    │── /api/reset ──────────────► kill ──► │  stopped*  │
                    └──────┬───────┘                                        └────────────┘
                           │ 引擎退出(close)        (* 无 closed 事件、无 exitCode)
                    ┌──────▼───────┐
                    │ 后处理(仍记   │  面元JSON→TIFF / ENVI→TIFF；出错 → finalCode=1
                    │ running)      │
                    └─┬──┬──┬──┬───┘
        exit 0 ───────┘  │  │  └── 其他退出码 / 后处理失败 ──► failed
        exit 2(FacetEB) ─┘  └── 全部 ENVI 转换失败 ─────────► failed
              │                    exit 0
              ▼                      ▼
        non_converged           completed
```

终态集合：`completed`、`non_converged`、`stopped`、`timeout`、`failed`；中间态：`preparing`、`running`（后处理期间 manifest 仍为 `running`，`server.mjs:5082`、`server.mjs:5267`）。历史耗时估计只采用 `completed` 与 `non_converged`（`server.mjs:1260`）。

#### 2.9.2 一次运行的时间线（以 eFacetEB、2 个节点为例）

| 时刻 | 事件 | manifest / 文件 |
|---|---|---|
| T0 | 点击“运行”→ 前端必要时先保存，再调用 `/api/run/preflight`（有风险弹确认）→ `/api/run`（`src/renderer/src/main.js:6313-6323`） | — |
| T0+ | 互斥、校验、引擎白名单 | — |
| T1 | `createIsolatedRun` | `runs/<id>/` 创建；`source-project.json`、`run-manifest.json`（`preparing`） |
| T2 | 运行时场景（后台线程）→ 复制到 `input/` → 气象快照 → 写 `runtimeInput` | `input/project.json`、`position-N.txt`、`object-N.obj`、`meteo.txt` |
| T3 | 面元规模预检、植被面元/像元比提示 | stdout 事件 |
| T4 | spawn；写 `running`；超时计时开始；发 `started` | `pid`、`command` |
| T5 | 引擎加载（PROGRESS 3/5/10）、建可见性图 | `faceteb_run.json`（`running`）、`.facet_steps/`、结果 JSON 几何占位 |
| T6 | 节点 1：PROGRESS、FACET_CONVERGENCE、FACET_TIMING、FACET_STEP；计时暂停；逐方向出 TIFF；删 `energy_T=*.bin`；`FACET_ACK` | `completed.nodes[0]`；TIFF `T=DOYddd_hh-mm_VZA=_VAA=_f.tif`；`result_statistics_faceteb.csv`（`server.mjs:3084`） |
| T7 | 节点 2 同上；引擎写总结果、`faceteb_run.json`→`completed`/`non_converged`；`PROGRESS 100`、`RESULT` | `faceteb.json`（或 transient） |
| T8 | 引擎退出（0 或 2）；`close` | — |
| T9 | 面元质量警告；已握手出图则删 transient 结果；节点登记 | — |
| T10 | 终态写 manifest；发 `closed`；清理 `.histream-runtime-*`；`child=null` | `status`、`finishedAt`、`exitCode`、`completed.products` |
| T11 | 前端 300 ms 后自动打开结果（成功或未收敛）（`src/renderer/src/main.js:6701-6703`） | — |

### 2.10 运行目录最终结构

`<outDir>/runs/<runId>/`（以下按“确定来源”与“引擎产出”区分）：

```
run-manifest.json              服务端：运行记录（2.5.2）
source-project.json            服务端：运行时刻磁盘 project.json 的副本（统一或旧格式）
input/                         服务端：冻结的运行输入
  project.json                 histream-project 格式，offset 已归零，路径已绝对化，meteo.path 指向 input/meteo.txt
  position-<i>.txt             筛选后的实例（x z y scale rotation）
  object-<i>.obj               预裁切的单实例 OBJ（若发生）
  object-<i>-textured.obj      表面纹理展开后的 OBJ（若启用）
  spectrum-<i>.txt             开孔率展开后的光谱（若有）
  meteo.txt                    气象快照（有气象身份时）
<结果>.tif                      标准命名 float32 TIFF（见下）
result_statistics_<mode>.csv   服务端：每个 TIFF 的逐波段统计
result_statistics.txt          VoxelEB 引擎写（按行追加观测统计）
process/                       过程输出（启用 radiation/energy process 或光伏）
diagnostics/                   VoxelEB 诊断（terrain-environment.json、energy_balance_node=*.json、scattering_node=*.json、physiology_node=*.json/.bin 等）
.facet_steps/                  FacetEB：components.bin、energy_T=<token>.bin（节点出图成功后删除）
facetrt.json | faceteb.json    面元结果（保留三维结果时）；binary 格式还有同名旁路 .bin
faceteb_run.json               FacetEB 引擎自写运行索引（status/requested/completed/products/timing）
photovoltaic_summary.csv, photovoltaic_node_<n>.csv   FacetEB 光伏汇总（引擎写）
```

出处：`server.mjs:379-381`、`server.mjs:5006-5026`、`tools/runtime-scene.mjs:416-474`、`server.mjs:3084`、`models/histream/src/voxeleb/voxeleb.cpp:469`、`models/histream/src/facetrt/facetrtcore.cpp:5979`、`models/histream/src/facetrt/facetrtcore.cpp:6760-6781`、`models/histream/src/facetrt/facetrtcore.cpp:9121-9122`，以及上述历史运行目录实例（VoxelEB：`input/`、`process/`、`diagnostics/`、`T=DOY214_00-50_VZA=0.00_VAA=0.00_v.tif`）。

TIFF 命名 `outputTiffName`：能量平衡模式 `T=<DOYddd_hh-mm>_VZA=<v>_VAA=<v><后缀>.tif`；辐射模式 `SZA=<v>_SAA=<v>_VZA=<v>_VAA=<v><后缀>.tif`；角度保留 2 位小数；后缀：面元 `_f`、体元 `_v`、RayTracing 无；启用大气再加 `_a`（`server.mjs:1409-1413`、`server.mjs:1439-1442`、`server.mjs:1469-1479`）。面元出图与 ENVI 转换均使用该函数（`server.mjs:2298-2299`、`server.mjs:3301-3303`）。

被识别为“结果目录”的条件：目录含有效 `run-manifest.json`（`kind` 合法且 `outputDir` 与目录一致，或无 `outputDir` 时 `runId` 等于目录名），或含 `faceteb.json/facetrt.json/scene_summary.json`；结果读取/删除只允许在工程输出树或这类目录内，运行中的输出目录拒绝删除（`server.mjs:2979-3047`）。

#### 2.10.1 五种模式的运行差异汇总

| 项目 | eRaytracing | eVoxelRT | eVoxelEB | eFacetRT | eFacetEB |
|---|---|---|---|---|---|
| 请求节点 `requested.nodes` | 空，`nodeCount`=1 | 同左 | 展开 [start,end) | 空，1 | 展开 [start,end) |
| 引擎命令行第三参数 | 无 | 无 | 无 | 结果 JSON 路径 | 结果 JSON 路径 |
| 工作目录 | 工程目录 | 工程目录 | 工程目录 | 引擎目录 | 引擎目录 |
| stdin 握手 | 无 | 无 | 无 | 无 | `FACET_STEP`/`FACET_ACK` |
| 引擎原始结果 | ENVI（.hdr/.img） | ENVI | ENVI（文件名含 `T=`） | 面元 JSON（可带 .bin） | 面元 JSON + 逐节点 .bin |
| 服务端出图 | 退出后 ENVI→TIFF | 同左 | 同左 | 退出后一次性面元→TIFF | 每节点握手即时出图 |
| 可能的非 0 结果码 | 无 | 无 | 无（未收敛不改退出码） | 无 | 2 = 未收敛 |
| 质量提示来源 | — | — | `ENERGY_BALANCE` 的 `statesOutsideTolerance` | `zeroVisibilitySamples` | `FACET_CONVERGENCE` 与 `zeroVisibilitySamples` |
| 超过规模上限时 | 不检查 | 不检查（仅预检提示显存） | 同左 | 运行前抛错 | 运行前抛错 |

（依据：`server.mjs:354-355`、`server.mjs:5054-5078`、`server.mjs:5218-5262`、`server.mjs:5035-5040`、`server.mjs:1235-1248`、`models/histream/src/voxeleb/voxeleb.cpp:1210-1214`、`models/histream/src/base/engine.cpp:290-299`。）说明：eFacetEB 若本次运行一个节点都未成功握手出图（`streamedSteps` 为 0）、无出图错误且退出码属结果码，后处理会按 eFacetRT 的路径对结果 JSON 一次性出图（`server.mjs:5218`）；握手出图失败的运行因 `stepOutputError` 不再重试。

模式之间共用的约束：同一时刻只允许一个引擎子进程（`child` 单例），预检与运行都以当前内存工程副本为准而不读取前端未保存的修改（前端在运行前自动保存，`src/renderer/src/main.js:6320`）；运行期间不能更换工程或删除结果（`server.mjs:4645`、`server.mjs:5367`）；运行目录一经创建便不再被本服务自动清理，旧结果保留供历史耗时估计与结果浏览使用（`server.mjs:1253-1267`）。

### 2.11 本章依据的边界与待核对项

已知局限（代码事实）：

- 磁盘工程是统一格式，引擎输入是由 `stringifyProject` 重新生成的 `histream-project`，二者经 `normalizeProject` 往返；统一工程的 `shared.sky.mode='from-time'` 时仍把手动 `zenith/azimuth` 交给 HiStream（`src/renderer/src/project-unified.js:405-415`）。
- 运行输入的“冻结”是部分的：仅运行时 `project.json`、气象、裁切/展开产物被复制并（前两者）哈希，OBJ、DEM、光谱文件仍引用原路径（2.5.2）。
- 引擎的节点区间缺省（`start` 0、`end` 1）与前端缺省（1、48）不同；服务端 manifest 的 `requested.nodes` 按工程值展开（`models/histream/src/base/fileio_json.cpp:878-879`、`src/renderer/src/project-schema.js:1039`）。

**待核对**：

1. `/api/run` 的 `mode` 与 `project.mode` 是否总一致（服务端不比较，前端传 `state.mode`）。
2. 准备阶段被停止时终态为 `failed` 而非 `stopped`（后一次 `writeRunManifest` 覆盖前一次），是否为设计意图（`server.mjs:5304-5321`）。
3. 引擎被 `taskkill` 终止后 `close` 回调的 `code` 具体取值（代码仅处理空值与非 0 情形）。
4. spawn 失败（`error` 事件）后是否同时触发 `close`。
5. `tools/surface-textures.mjs` 在 `input/` 中可能产生的其他文件（本章只列出 `object-<i>-textured.obj`）。
6. 失败/停止/超时（非结果码）退出后 `.histream-transient-*.json` 是否会被清理（`completeHistreamRun` 中未见相应删除代码，`server.mjs:5234`、`server.mjs:5238`）。
7. Voxel/RayTracing 的 ENVI 文件名与 `process/`、`diagnostics/` 的完整清单（由引擎各模块输出，本章仅按实例目录与调用点归纳）。
8. Electron 桌面端入口（`desktop/`）是否对运行流程有附加处理（本章未调查）。
9. `sensor.x/y` 的物理含义（像元数）与 `Tsold`/`Tsoil` 温度单位混用的产品约定（源码标 DEFERRED）。

## 3. HiStream 引擎核心：启动、场景构建与 RayTracing/VoxelRT

本章描述可执行程序 `histream` 从命令行到退出码的完整运行过程，以及 RayTracing、VoxelRT 两种模式的内部流程。物理方程见《理论手册》（坐标与计算域见其 §2.1，辐射方程见 §3，VoxelRT 见 §5，异质性体元见 §6，周期/天空边界见 §5.5，大气顶层观测见 §3.3）；界面操作见《用户手册》。本章只写“程序实际怎么做”，括号内为依据的源码位置（仓库相对路径:行号）；无法由当前代码确认的写“待核对”。

### 3.1 进程入口：命令行、模式分派与退出码

#### 3.1.1 命令行语法

| 形式 | 含义 | 依据 |
|---|---|---|
| `histream <eVoxelEB\|eFacetEB\|eFacetRT\|eVoxelRT\|eRaytracing> <project.json> [output.json]` | 运行一个内置模式；第 3 参数是输出路径（仅 Facet 模式使用，其余模式的输出目录取自工程 `outDir`） | models/histream/src/main.cpp:60-61，models/histream/src/base/engine.cpp:172-178 |
| `histream <eVoxelEB\|eVoxelRT\|eRaytracing> --builtin-example` | 运行内置示例；FacetRT/FacetEB 没有内置示例，用此开关返回用法错误 | models/histream/src/main.cpp:139-144 |
| `histream --list-devices` | 枚举可用设备并退出 | models/histream/src/main.cpp:117-125 |
| 扩展模式 | 由 `ExtensionRegistry` 注册的外部模式，核心构建无扩展 | models/histream/src/main.cpp:100-115 |

分派顺序固定：先判断空参数（打印用法，退出 64）→ 扩展模式 → `--list-devices` → 内置模式名校验（不在五个名字内则退出 64）→ 工程路径校验（缺省且不是 `--builtin-example` 则退出 64）（models/histream/src/main.cpp:99-149）。模式名区分大小写，必须是 `eRaytracing`（r 小写）、`eVoxelRT`、`eVoxelEB`、`eFacetRT`、`eFacetEB`（models/histream/src/base/engine.cpp:118-130）。

进程级设置：Windows 下关闭系统崩溃/中止对话框，失败只通过 stderr 与退出码报告（models/histream/src/main.cpp:76-77）；设置 `GDAL_FILENAME_IS_UTF8=NO`，使 GDAL 与引擎内窄字符路径同样按本地代码页解释（models/histream/src/main.cpp:154）。因此含代码页外字符的命令行路径会被替换为 `?`，源码已将其记为已知缺陷（models/histream/src/main.cpp:126）。

#### 3.1.2 退出码

| 码 | 含义 | 产生位置 |
|---|---|---|
| 0 | 成功 | models/histream/src/main.cpp:193 |
| 1 | 运行失败（`run()` 返回 false 或抛异常）；`run` 成功但 `destroy()` 抛异常也记 1 | models/histream/src/main.cpp:180-192 |
| 2 | 仅 FacetEB：完成但有节点未收敛，结果保留 | models/histream/src/base/engine.cpp:298，models/histream/src/facetrt/facetrtcore.cpp:9383 |
| 3 | 资源创建失败（`create()` 返回 false 或抛异常，含显存规模超限） | models/histream/src/main.cpp:167-176 |
| 3（冲突） | FacetEB 的 CPU/GPU 能量审计不一致也返回 3，与上一行同码不同义 | models/histream/src/facetrt/facetrtcore.cpp:9383，models/histream/src/faceteb/command.cpp:11 |
| 4 | 初始化失败：`Engine::input` 抛异常（工程解析、设备选择、场景构建都发生在这一步） | models/histream/src/main.cpp:159-163 |
| 5 | `--list-devices` 失败 | models/histream/src/main.cpp:121-124 |
| 64 | 用法错误（与 FacetEB 的 2 区分） | models/histream/src/main.cpp:56 |

服务端对应表为 `HISTREAM_EXIT`（server.mjs:119），并仅把 FacetEB 的 2 视为“未收敛但有结果”（server.mjs:5265）。

#### 3.1.3 引擎生命周期（文字流程图）

```
main
 ├─ Engine::input(工程路径, 模式, 输出路径)                    失败 → 退出码 4
 │    1 模式字符串→枚举（未知 → 抛 "Unknown mode"）
 │    2 ProjectJson::load 做一次预加载与兼容检查
 │    3 设置线程环境变量
 │    4 Facet 模式：只记录三个路径，返回（自行读工程）
 │    5 其余模式：FileIO::readJson → Engine::init(选设备、建后端对象)
 │                 → <后端>::setup(取设备/队列/分配器)
 │                 → <后端>::upload(构建场景、几何、光学，纯 CPU)
 ├─ Engine::create()                                            失败 → 退出码 3
 │       RayTracing/VoxelRT/VoxelEB：建缓冲 → 描述符 → 管线 → 命令
 ├─ Engine::run()                                               失败 → 退出码 1
 └─ Engine::destroy()
```
依据：models/histream/src/base/engine.cpp:115-211（input）、215-248（create）、277-311（run）、315-339（destroy）。

`Engine::input` 里的两项预检（在读取任何场景之前）：

- 工程含 `scene.terrainEnvironment` 且其 `airEnabled`、`soilEnabled` 为真或 `co2ValleyExcess≠0`，而模式不是 eVoxelEB，则抛 “TerrainEnvironment currently requires eVoxelEB”（models/histream/src/base/engine.cpp:139-146；`readScene` 内有同样检查：models/histream/src/base/fileio_json.cpp:496-497）。
- 工程存在启用镜面反射的表面（`hasSpecularReflection()`，判定规则见 models/histream/src/base/projectjson.cpp:274-313）而模式不是 eRaytracing，则抛 “Mesh specular reflection requires eRaytracing”（models/histream/src/base/engine.cpp:147-148）。即 VoxelRT/VoxelEB/Facet 均不支持网格镜面反射。

CPU 线程数：取 `control.cpuThreads`（缺省 32；小于 1 回到 32），再限制不超过逻辑处理器数，写入环境变量 `HISTREAM_CPU_THREADS` 与 `LP_NUM_THREADS`（后者供 Mesa lavapipe 使用）（models/histream/src/base/engine.cpp:149-165）。该线程数对 GPU 计算无影响，主要用于 Facet 的 CPU 并行与 CPU 后端。

#### 3.1.4 `--list-devices` 与设备选择

`--list-devices` 以 Auto 后端探测（使 lavapipe 也出现在清单中），打印一行 `HISTREAM_DEVICES<TAB><JSON>` 到 stdout（models/histream/src/base/appsetting.cpp:163-174，models/histream/src/main.cpp:119）。JSON 字段：`logicalProcessors`、`lavapipeThreadLimit`（32）、`gpus[]`（每项 `name`、`type`=discrete/integrated/virtual/other、`voxelIndex`、`facetIndex`，-1 表示该后端不支持此设备）、`cpu.lavapipe`（名称或 null）（models/histream/src/base/appsetting.cpp:126-158）。

Voxel 系与 RayTracing 的 GPU 序号 `voxelIndex`：对满足必需扩展的设备按“独立显卡优先、其余保持 Vulkan 枚举顺序”排序，工程 `control.gpu`（缺省 0，负数取 0）即此排序中的位置（models/histream/src/base/appsetting.cpp:351-358，models/histream/src/base/fileio_json.cpp:259）。Facet 后端使用另一套排序 `facetIndex`（models/histream/src/base/appsetting.cpp:139-144，排序规则在 models/histream/src/facetrt/facetrt_vulkan.h，本章不展开）。

计算后端 `control.computeBackend`（缺省 `gpu`，不区分大小写，未识别的值按 gpu；环境变量 `HISTREAM_COMPUTE_BACKEND` 优先于工程）：

| 值 | 行为 | 失败条件 |
|---|---|---|
| gpu | 仅用 GPU，按 `control.gpu` 取设备 | 没有合格 GPU；序号越界（报错信息含有效范围 `0-(n-1)`） |
| cpu | 仅加载 Mesa lavapipe（以 `VK_DRIVER_FILES` 指向可执行文件旁 `lavapipe/lvp_icd.x86_64.json`），同一套着色器在 CPU 上运行 | Windows 下找不到清单 → 抛错；非 Windows 直接抛错 |
| auto | 追加 lavapipe（`VK_ADD_DRIVER_FILES`）；无合格 GPU 时回退 CPU 并向 stderr 打印一行 | 清单缺失仅打印警告，只能用 GPU |

依据：models/histream/src/base/appsetting.cpp:42-49、68-94、365-384，models/histream/src/base/engine.cpp:78-97。

设备“合格”的条件是满足 Vulkan 1.3 与光追三件套（加速结构、光追管线、ray query）等必需扩展；`shader_atomic_int64`、`shader_atomic_float`（float32 缓冲原子加）、管线可执行属性、片元着色器互锁等为可选扩展（models/histream/src/base/appsetting.cpp:255-309）。设备初始化的其他事实：

- 需要一个隐藏的 GLFW 窗口以创建 `VkSurfaceKHR`，窗口不可见、不抢焦点；GLFW 初始化失败（无桌面会话）、窗口创建失败或 Vulkan 不可用都会抛 “GLFW window initialization failed…”，即退出码 4（models/histream/src/base/appsetting.cpp:178-187、207-242）。
- 设备队列数少于请求数（lavapipe 只有 1 个队列）时降级为只请求 GCT 队列；`m_queues` 顺序为 [GCT, Compute, Transfer]，缺独立队列时回退 GCT（models/histream/src/base/appsetting.cpp:386-398、422-430）。模型只向 GCT 队列提交。
- 成功后向 stdout 打印 `HiStream compute backend (<gpu|cpu|auto>): <设备名> [GPU|CPU]`，CPU 时附线程数（最多 32）（models/histream/src/base/appsetting.cpp:402-415）。
- 校验层的 debugPrintf 特性无条件开启（`ENABLE_GPU_PRINTF`），源码已标注为潜在性能问题（models/histream/src/base/appsetting.cpp:311-325）。
- FacetRT/FacetEB 不经 `AppSetting`，也不调用 `appSetting.destroy()`（models/histream/src/base/engine.cpp:62-71、317-326）。

### 3.2 工程文件读取

#### 3.2.1 加载与宽松取值

`ProjectJson::load` 的校验：路径非空；**文件名必须是 `project.json`**（不区分大小写，否则抛 “HiStream accepts project.json only”）；根必须是 JSON 对象；`format` 缺省 `histream-project`，其他值抛错；`schemaVersion` 缺省 1，范围 1–6（引擎能读取的上限为 6）；必须含 `configuration` 对象（models/histream/src/base/projectjson.cpp:41、86-117）。

取值函数一律“缺失或类型不符回退缺省值，不抛异常”：`number` 接受数值或可解析字符串，非有限值回退；`integer` 对整型值饱和到 int 范围，浮点按四舍五入；`boolean` 接受布尔、整数（非 0 为真）、字符串 `"true"`/`"1"`（models/histream/src/base/projectjson.cpp:229-268）。只有少数字段显式抛错（如 `shortwaveScatteringOrders`、`voxelDirectionRotation`，见 3.2.3）。

路径解析 `resolve`：JSON 字符串按 UTF-8 转路径；绝对路径原样规范化；相对路径相对工程目录；若不存在且以 `assets` 开头，则从可执行文件目录逐级向上查找同名资源，找不到仍返回工程目录下路径（models/histream/src/base/projectjson.cpp:137-154）。输出目录 `configuration.outDir`（缺省 `output`）经同一规则解析（models/histream/src/base/projectjson.cpp:157-159）；服务端在运行前创建目录（server.mjs:5059），引擎的 TIFF 写出本身不创建目录（models/histream/src/base/fileio.cpp:2382 处的注记）。

#### 3.2.2 configuration 段到内部结构

| 工程段 | 读取位置 | 内部结构 |
|---|---|---|
| `control` | models/histream/src/base/fileio_json.cpp:258-320 | `SettingXml`（models/histream/src/base/structs.h:358-406） |
| `sensor`、`light`、`atmosphere` | models/histream/src/base/fileio_json.cpp:322-393、228-242 | `SensorXml`、`LightXml`、`AtmosphereXml` |
| `scene`（含 `background`、`terrainEnvironment`）、`objects.items` | models/histream/src/base/fileio_json.cpp:470-608 | `SceneXml`：背景 + `ObjEntity`（RayTracing）或 `PrimEntity`（体元模式） |
| `spectra`、`thermals`、`canopies`、`materials` | models/histream/src/base/fileio_json.cpp:649-861 | `SpectralXml`、`ThermalXml`、`CanopyXml`、`PropertyXml` |
| `meteo`、`fluid` | models/histream/src/base/fileio_json.cpp:877-912 | 仅 VoxelEB 读取（RayTracing/VoxelRT 只用 `meteo` 的经纬度、年份、时区） |

`FileIO::readJson` 只支持 eRaytracing、eVoxelRT、eVoxelEB，其余返回 false；VoxelEB 工程中若背景或对象使用 `energyModel=photovoltaic` 的材质，抛 “Photovoltaic … requires eFacetEB”（models/histream/src/base/fileio_json.cpp:618-641）。路径为空时走内置示例 `readXml`（models/histream/src/base/fileio_json.cpp:619）。

#### 3.2.3 control 字段、缺省值与范围

| 字段 | 缺省 | 取值处理 | 依据（models/histream/src/base/fileio_json.cpp） |
|---|---|---|---|
| `depth` | 4 | ≥1；VoxelRT/VoxelEB 用它派生射线步数上限 | :258 |
| `raytracingDepth` | 16 | 仅 RayTracing，夹到 1–64，覆盖 `depth` | :411-412 |
| `gpu` | 0 | ≥0 | :259 |
| `computeBackend` | `gpu` | 见 3.1.4 | :260 |
| `samples` | 32 | 夹到 1–1024 | :261 |
| `heterogeneousVoxel` | false | 开启 HEX | :262 |
| `periodicTraversalCount` | 旧字段折算 | 夹到 0–20；旧字段 `periodicBoundary`+`periodicNeighborCount`（缺省 8）折算为缺省值，关闭则 0 | :265-269 |
| `skyboxEnabled` | false | 未命中光线是否取天空盒 | :270 |
| `seed`、`adaptiveSampling`、`sampler` | 0、false、`sobol` | 仅 RayTracing；`sampler` 只认 `sobol`/`legacy`，其他值取引擎缺省（Sobol）；环境变量 `HISTREAM_RT_SEED`/`HISTREAM_RT_SAMPLER` 覆盖 | :273-279；models/histream/src/base/structs.h:354 |
| `radiationSolver` | `traditional` | 值为 `accelerated` 时启用 VoxelRT 加速求解 | :280-281 |
| `shortwaveScatteringOrders` | 1 | **只允许 1–3，越界抛错** | :288-290 |
| `voxelCollisionOrders` | 2 | 夹到 2–5 | :291-292 |
| `voxelDirectionRotation` | `legacy` | 只允许 `legacy`/`rot`，否则抛错 | :298-302 |
| `voxelDirectionRotationBlock` | 1 | 夹到 1–16 后按 2 的幂取整（1/2/4/8/16） | :303-304，models/histream/src/base/voxelflags.h:71-76 |
| `voxelEmptySkip` | true | 空节点跳过 | :305 |
| `voxelProxyGeometry` | `positionNoIndex` | 只允许 `full`/`position`/`positionNoIndex`，否则抛错 | :307-314，models/histream/src/base/voxelflags.h:49 |
| `uavTraverse` / `sensor.cruiseEnabled` | false | 仅在透视投影下才使 `isUAVtrave` 为真 | :317-320 |
| `cpuThreads` | 32 | 见 3.1.3 | models/histream/src/base/engine.cpp:154 |

体元传输开关的优先级是“环境变量 > 工程 control > 引擎缺省”：`HISTREAM_VOXEL_DIRROT`（legacy/rot/rot-all）、`HISTREAM_VOXEL_DIRROT_BLOCK`、`HISTREAM_VOXEL_SKIP`、`HISTREAM_VOXEL_PROXY`（models/histream/src/base/voxelflags.h:80-101），位标志写入推送常量 `voxelFlags`。

#### 3.2.4 传感器、观测角、光源、大气

- **分辨率** `sensor.x/y` 缺省 512，下限 1（:328-331）；**传感器位置** `positionX/Y` 缺省为场景水平中心，高度 `height` 缺省 3000 m、下限 0.01（:332-336）；视场 `fov` 缺省 60°，夹到 0.1–120°（:337）。
- **波段** `sensorBands()`：解析 `sensor.bands`（分隔符为空白/逗号/分号，只保留 >0 的有限数）；`continuousBands` 为真时按 `bandStart/bandEnd/bandStep`（缺省 400/2500/10）生成连续波段，个数 `floor((end-start)/step+1e-9)+1`，步长不能整除时追加 `end`；所有值取 6 位小数去重、升序，**总数上限 800**（超出截断并 stderr 报告，连续段为自定义波段预留名额）；结果为空时回退单波段 10500（models/histream/src/base/projectjson.cpp:326-437）。此规则须与前端 `project-schema.js` 的 `sensorBandValues()` 逐条一致（models/histream/src/base/projectjson.cpp:315-324）。波长大于 50 视为 nm，否则视为 μm；>2.5 μm 为热红外（models/histream/src/raytracing/raytracing.cpp:190）。
- **观测角** `viewAngles()`：VZA 夹到 [0, 89.999]，VAA 归一化到 [0,360)，以 1e-4° 量化去重，天顶（VZA≈0）与方位无关合并为一项；透视投影只取单个 `vza/vaa`；平行投影下 `principalPlane` 生成主平面与垂直主平面各 VZA −75…75 步长 5（负角度对应 VAA+180°），`hemisphere` 生成 128 个方向（cosVZA 在 [cos75°,1] 等分，方位按黄金角递增，即 VZA 0–75°球冠）；两者都关时取单个 `vza/vaa`（models/histream/src/base/projectjson.cpp:445-484）。结果为空时回退天底 {0,0} 并警告，因此 `n_angle≥1` 恒成立（models/histream/src/base/fileio_json.cpp:360-363）。
- **光源**：`zenith` 缺省 30°、`azimuth` 缺省 135°；`direct` 夹到 [0,1]，`diffuse=1−direct`；天空温度缺省 250 K，太阳温度固定 6000 K；`shortwaveIrradiance` 夹到 [0,2000] W m⁻²（RayTracing/VoxelRT 的 2.5–6 μm 绝对太阳项，≤0 用晴空缺省）（models/histream/src/base/fileio_json.cpp:381-393，models/histream/src/base/structs.h:329-331）。
- **大气**：`model` 只认 tropical/midlatitude-summer/midlatitude-winter，否则取 midlatitude-summer；`waterVapor` 夹到 [0.5,5]；`aerosol` 只认 rural/urban；`visibility` 夹到 [10,50]；`lutFile` 缺省 `assets/atmosphere/simple_modtran_lut.csv`（models/histream/src/base/fileio_json.cpp:228-242）。启用与否由 `atmosphere.enabled`（缺省 false）决定，输出 TIFF 的文件名后缀随之变化（见 3.9.5、3.10.4）。

#### 3.2.5 UAV 巡航航线

仅当透视投影且 `cruiseEnabled`/`uavTraverse` 为真时生成。航线点数 `cruiseCount`（缺省 20，夹到 1–10000），点间隔 `cruiseStep`（缺省 25 m，下限 0.01），起点缺省场景水平中心，高度缺省 3000 m；所有水平坐标按场景尺寸周期环绕到 [0,extent)，高度下限 0.01（models/histream/src/base/fileio_json.cpp:130-148）。三种 `cruiseRoute`：

| 值 | 行为 | 依据 |
|---|---|---|
| `line`（缺省） | 起点到目标点（缺省沿 `cruiseHeading`（缺省 90°）前进 `step×(count−1)`）往返，每折返航向转 180°；起终点重合时退化为沿航向的水平线 | :191-222 |
| `rectangle` | 以起点为中心的矩形（边长缺省取场景 1/4 且不超过 500 m），航向依次 0/90/180/270° | :152-170 |
| `random` | 固定种子线性同余发生器（乘数 1664525、增量 1013904223，种子 `cruiseRandomSeed` 缺省 1，最小 1），逐点随机航向，可复现 | :174-189 |

航向约定为 `atan2(Δy,Δx)`，其中 x=北向坐标、y=东向坐标。巡航模式下，工程中的方位角是相机相对角（0 前、90 右、180 后、270 左），换算为地面到传感器方位：`(航向 + 相对方位 + 180) mod 360`；逐航点的视方位存入 `uavViewAzimuths`（models/histream/src/base/fileio_json.cpp:346-370）。

### 3.3 坐标约定与体元步长取整规则

#### 3.3.1 坐标约定

引擎内部（体元、OBJ、渲染场景）统一为右手系：**+X 指北、+Y 向上、+Z 指东**；方位角自北顺时针（方位角 0°=+X，90°=+Z）（models/histream/src/base/scene.cpp:22-25，models/histream/src/base/objloader.h:13，models/histream/src/base/geometry.cpp:16、183-185）。工程与位置文件里的 `(x, y, z)` 含义为 **(北, 东, 高)**，进入引擎后对应世界坐标 `(X, Z, Y)`：

```
工程坐标 (北 x, 东 y, 高 z) ──→ 世界坐标 (X=x, Y=z, Z=y)        // RayTracing 再减去场景半宽平移到中心
RayTracing 实例平移 = (x − sceneX/2,  z,  y − sceneY/2)          // scene.cpp:2216
体元整数格 (i_N, i_H, i_E) = (floor(x/d), floor(z/d)+地形层, floor(y/d))   // scene.cpp:1684-1687
```

说明：

- 场景尺寸 `scene.x`（缺省 60 m）是**南北向长度**，`scene.y`（缺省 60）是东西向宽度，`scene.height`（缺省 15）为高度；`x、y` 下限 0.01，`height` 下限 0（models/histream/src/base/fileio_json.cpp:501-505）。场景原点固定 (0,0,0)（:506）。
- 设计阶段的基元形状（`VoxelDesigner`）使用 XYZ（z 向上）顺序，经 `XYZ2XZY` 交换 y/z 后进入引擎；该函数只交换位置分量，法向原样复制（源码已标注为已知缺陷，体元代理着色器只读位置，法向由叉积得到）（models/histream/src/base/scene.cpp:3900-3937、3911；models/histream/src/base/proxymesh.h:3-5）。**OBJ 路径不得再调用 `XYZ2XZY`**，因为 OBJ 的高度轴已是 Y（models/histream/src/base/scene.cpp:1574-1576）。
- 影像朝向：平行投影相机的 up 取地理北在像面的投影，影像“上北右东”；透视相机 up 取世界 +Y 的投影使地平线水平，天底时退化为正北（models/histream/src/base/geometry.cpp:39-91）。输出 TIFF 元数据记录 `HISTREAM_SCENE_AXES=X_NORTH_Y_UP_Z_EAST`、`HISTREAM_IMAGE_ORIENTATION=NORTH_UP_EAST_RIGHT`，不写地理参考（models/histream/src/base/fileio.cpp:2413-2415）。
- 太阳方向向量 `(r sinθ cosφ, r cosθ, r sinθ sinφ)`，`r=3000`；太阳天顶角恰为 0° 或 45° 时加 `ANGLE_COR=0.1°`，方位角靠近 45° 倍数（差 <0.05°）时偏移 ±0.2°，以避开与体元面对齐的退化方向，用户给定的太阳角因此可能被静默微调（models/histream/src/base/geometry.cpp:165-199，models/histream/src/base/structs.h:61）。

#### 3.3.2 体元步长与取整规则

水平与垂直步长同取 `scene.voxel`（缺省 1 m，下限 0.01），`stepsize_surface = stepsize_height`（models/histream/src/base/fileio_json.cpp:507-508）。体元化代码要求各向同性，竖直方向实际一律使用 `stepsize_surface`（models/histream/src/base/scene.cpp:1655；models/histream/src/voxelrt/voxelrt.cpp:206-211）。各处取整规则不完全一致，使用时应注意：

| 量 | 规则 | 依据 |
|---|---|---|
| 水平格数 `voxelSize_XZY.(x,z)` | `int(sceneSize/step)`，**向零截断** | models/histream/src/base/scene.cpp:2939-2941、3744 |
| 竖直层数 | `max(1, ceil(sceneSize.z/step))` | models/histream/src/base/scene.cpp:63-67；models/histream/src/voxelrt/voxelrt.cpp:232-233 |
| 平地背景网格 | 循环 `i < sceneLength/step`（浮点比较，等效 `ceil`） | models/histream/src/base/voxeldesigner.cpp:1150-1152 |
| DEM 体元背景 | `max(1, floor(sceneSize/step))`，尾部不足一格被舍去 | models/histream/src/base/scene.cpp:206-207 |
| 长方体/椭球基元 | 各向 `ceil(size/step)` | models/histream/src/base/voxeldesigner.cpp:69-71、131-133 |
| 基元放置格 | `int(米/step)`（向零截断；高度为 `(z+DEM 高度)/step`） | models/histream/src/base/scene.cpp:2421-2424 |
| OBJ 放置格 | `floor(米/step)`；DEM 抬升层 `round(DEM 高/step)` | models/histream/src/base/scene.cpp:1681-1687 |
| 场景半宽平移 | `floor(格数/2+0.5)` | models/histream/src/base/scene.cpp:1649-1652 |

《理论手册》§2.1 写作 `Nx = ceil(L/d)`；当前代码的水平格数是截断，当 `scene.x/voxel` 不是整数时，平地背景网格会比计算域格数多出一格，背景体元不做域裁剪（实体基元与 OBJ 则用 `insideVoxelDomain` 裁剪，要求坐标在 `[0,N)` 内）（models/histream/src/base/scene.cpp:63-67、3046-3063）。此处是否造成可见影响：待核对。

背景体元位于**竖直层 −1**（平地背景 `voxelIds=(i,j,−1)` 经 XZY 变为 `(i,−1,j)`），第一层实体体元在层 0；DEM 背景的每格地表体元在 `top−1`（models/histream/src/base/voxeldesigner.cpp:1142、1182；models/histream/src/base/scene.cpp:276、3918-3923）。

### 3.4 材质、光谱与热属性装载

`Compo::createCompOptical` 对每个 `spectra` 条目调用 `resolveSpectrals`，把光谱统一重采样到传感器波段，数组按“材质主序、波段次序”扁平存放（每材质占 `waves.size()` 个元素）（models/histream/src/base/compo.cpp:190-217、222-252）：

- 光谱来源 `model`：`Prospect`（叶片 PROSPECT/Fluspect，参数 Cab/Cw/Cdm/Cs/N，缺省 40/0.01/0.01/0/1.5，N<1 或负参数抛错）、`BSM`（土壤，参数 SMC/Brightness/lat/lon 缺省 25/0.5/25/45）、`file`（导入文本：第 1 行波长、第 2 行反射率、可选第 3 行透射率，波长须唯一）、其余按 `custom`（直接给反射率/透射率）（models/histream/src/base/fileio_json.cpp:653-673；models/histream/src/base/compo.cpp:119-150、295-322）。系数表来自 `defined/optipar.txt`，列固定 2001 点（400–2400 nm 每 1 nm，`N1=2001`），读失败不报错（models/histream/src/base/fileio.cpp:1959-1997，models/histream/src/base/structs.h:67）。
- 缺省值：反射率 0.20、透射率 0、热红外反射 `refTir` 0.05、热红外透射 `tauTir` 0（models/histream/src/base/fileio_json.cpp:654-657）。输出时反射率夹到 [0,1]，透射率夹到 [0,1−反射率]；目标波长超出源光谱范围取端点值，不外推（models/histream/src/base/compo.cpp:155-181、479-483）。
- **>2.5 μm 的波段不做光谱插值**，取平坦的热红外反射/透射（BSM 土壤的热红外反射随含水量变化）（models/histream/src/base/compo.cpp:463-477）。
- 镜面：`control.specularEnabled`（缺省 true）与材质 `specular.enabled` 同时为真才启用；`fraction` 缺省 0.5（0–1）、`roughness` 缺省 0.25（0.05–1）（models/histream/src/base/fileio_json.cpp:660-668）。RayTracing 中网格/背景的局部镜面开关通过复制私有光谱（名称 `__mesh_specular_<n>_<i>`、`__background_specular`）实现，不改共享材质；找不到原光谱抛 “Surface spectrum not found”（models/histream/src/base/fileio_json.cpp:440-462）。
- 物理纹理：启用且给出文件才生效；`strength` 夹到 [0,1]、`relativeStd` 夹到 [0,0.5]、`repeatSize` 下限 0.01（models/histream/src/base/fileio_json.cpp:677-685）。
- 热属性 `thermals`：向阳/背阴温度缺省 305 K/295 K；RayTracing 在传感器 `temperature=false` 时追加名为 `None` 的 300 K 占位；体元缓冲在热属性为空时放 300 K 占位（models/histream/src/base/fileio_json.cpp:691-695；models/histream/src/base/compo.cpp:212-215；models/histream/src/voxelrt/buffer.cpp:92-93）。
- 冠层（VoxelRT/VoxelEB）：缺省 LAI 3、density 1、hc 2、G 0.5、LIDFa −0.35、LIDFb −0.15、hspot 0.2、leafwidth 0.1；`structureType` 编码 0 冠层/1 刚体/2 火/3 雾；`scatteringAlbedo` 夹到 [0,1]、`asymmetry` 夹到 [−0.99,0.99]。叶倾角分布由 `withLeafAngles` 派生 13 个 SAIL 频率与 32 个 `diffuseG`；`LIDFa=LIDFb=0`、`LIDFa>1` 或刚体/介质保持常数 G（models/histream/src/base/fileio_json.cpp:702-716；models/histream/src/base/compo.cpp:59-83）。
- 材质参数 `materials`（VoxelRT/VoxelEB）：按类型分支为植被叶生化、水体、土壤/建筑；木质（`energyModel=wood` 或历史名 `tree_branch_wood`/`tree_trunk_wood`）一律按建筑固体处理（models/histream/src/base/fileio_json.cpp:724-861）。

对象 `type` 到内部类别：Soil→SOIL；Building/Human/Vehicle/Ship→BUILDING（刚性不透光表面）；Water→WATER；Vegetation/Fire/Fog→VEGETATION；其余 OTHER（models/histream/src/base/fileio_json.cpp:37-45）。火、雾（`medium`）对象在 RayTracing 中被跳过，只在 VoxelRT/VoxelEB 保留（:536）。

### 3.5 场景构建

场景构建在 `Engine::input` 的 `upload` 阶段完成，全部在 CPU 上进行，结果写入各模式 IO 对象的 `meshio`（网格与 `MeshLink`）、`instanceio`（`Instance`/`InstanceLink`）、`voxelio`（NanoVDB 句柄与 `VoxelLink`）。

关键数据结构：`Instance{meshId, object2worldMatrix, world2objectMatrix}`（models/histream/src/base/structs_cg.h:307-312）；`MeshLink{type, spectralId, thermalId, canopyId, bioId, angularEffectStrength, vertexAddress, indexAddress}`，GPU 经设备地址读顶点/索引（models/histream/src/base/structs_cg.h:345-356）；`Thermal{sunlit, shaded}`（:82-85）；`Angle{vza,vaa,sza,saa}` 度（models/histream/src/base/structs.h:603-608）；`VoxelLink`（48 字节）含 `voxelId(ivec3)`、`instanceId`、`faceId`、`isValid`、`leafDensity`、`hexId`（−1 表示均匀体元）、`normal`（models/histream/src/base/structs_cg.h:363-376）。

#### 3.5.1 RayTracing：网格 + 实例

`Scene::createObjScene`（models/histream/src/base/scene.cpp:1990-2241）：

1. **背景网格**（网格 0）：有 DEM 时由 `creatBackgroundFromDEM` 生成并导出 `dem.obj` 到输出目录，放置点高度之后插值到地形；否则若背景光谱启用物理纹理，生成网格化平地，否则单个两三角形平面（:2021-2039）。背景 `y=0` 平面以原点为中心，范围 `±sceneSize/2`（models/histream/src/base/objloader.cpp:288-332）。
2. **DEM 网格**：GDAL 读第 1 波段；nodata 与非有限值替换为有效最小值；高程整体减最小值使最低点 y=0；按 `sceneSize` 重采样（每边最多约 2048 格，格距下限 0.01），行向对应北（+X），列向对应东（+Z）；DEM 少于 2×2 像元、打不开、无有效高程抛错（models/histream/src/base/objloader.cpp:988-1129）。DEM 被拉伸到整个场景范围，忽略其地理变换（models/histream/src/base/objloader.cpp:1043-1046）。
3. **OBJ 对象**：每个对象按 `meshes` 绑定逐网格 `loadMesh`（顶点不共享，按面角展开；无三角形的网格跳过并警告；单绑定时名字不匹配会加载整个 OBJ，多绑定时不匹配则跳过）（models/histream/src/base/objloader.cpp:204-284，models/histream/src/base/scene.cpp:2164-2172）。放置点来自 `positionFile`（每行 `x y z [scale [rotation]]`，缺省 1、0；少于 3 列或 `scale≤0` 的行丢弃并警告，无有效行抛错）或缺省一个位于场景中心的实例（models/histream/src/base/utils.cpp:102-125，models/histream/src/base/scene.cpp:2112-2120）。
4. **实例矩阵** `T(shift)·Ry(−rotation°)·S(scale)`，`shift=(x−sceneX/2, z, y−sceneY/2)`（models/histream/src/base/scene.cpp:2215-2224）。光谱绑定必填（缺失抛 “Missing spectral binding”）；热属性名找不到静默取 0 号（:2157-2188）。

#### 3.5.2 体元模式（VoxelRT/VoxelEB 同构）

`Scene::createPrimObjScene` 流程（VoxelRT 版 models/histream/src/base/scene.cpp:3100-3158，VoxelEB 版 :2257-2317）：

```
GridBuilder<int32_t> nanoBuilder(-1)      // 空体元值 = -1
1 createPrimObj_Background        写全局参数(尺寸/步长/经纬度/年/时区)，建背景体元
2 对每个 PrimEntity：
    voxelizeFromObj → createObjFilledVoxels      (OBJ 体元化)
    VEGETATION      → isshapeFromFile ? Crowns : Crown
    BUILDING        → createPrimObj_Building (墙+屋顶)
    WATER           → 仅 VoxelEB 走 Crown；VoxelRT 无此分支
3 heterogeneousVoxel → finalizeHexMixtures
4 nanoHandle = nanoBuilder.getHandle()          // 体元 → VoxelLink 序号 的 NanoVDB 索引网格
```

注意：VoxelRT 版不处理“非 OBJ 的 Water 基元”（SOIL/OTHER 类型的基元在两个模式中都不处理）（models/histream/src/base/scene.cpp:3136-3147）。背景必须最先建，因为它写入后续实体依赖的 `voxelSize_XZY`、`stepsize_surface`（models/histream/src/base/scene.cpp:2922）；背景步长非有限正数抛错（:2936-2938）。

**NanoVDB 写入规则**：每个体元格的值为其首个 `VoxelLink` 的序号；已被占用的格“先到先得”不覆盖（树冠、建筑）。建筑一格占连续 5 个 `VoxelLink`：`faceId` 1–4 为墙面（属墙体实例），5 为屋顶（属实例 +1），`isValid` 取自网格逐体元面有效标志（models/histream/src/base/scene.cpp:2696-2699、2860-2890）。若某放置点没有写入任何新体元，其实例被撤销（:2510-2514）。

**基元形状**：`shape` 为 `cube`（缺省）或 `ellipsoid`，`dimensions` 三个数，不足补 1、非正取 1（models/histream/src/base/fileio_json.cpp:97-103、592-597）。椭球用外接长方体内体元的椭球方程 `((i−c)/c)²…≤1`（水平截面为圆，宽同时作长）判定，长方体取全部 `ceil(size/step)` 个体元（models/histream/src/base/voxeldesigner.cpp:57-159）。形状文件（树冠）行格式 `type height width length x y z`，类型 0/1/2，非数值行、不足 7 列、类型越界、高宽长为负均抛错，不缩放不旋转（models/histream/src/base/scene.cpp:2556-2576）。火焰介质的体元由确定性公式雕刻成向上收窄的火焰形，雾占满整个基元，放置点为水平中心+底部高度（models/histream/src/base/scene.cpp:154-189、2411-2416）。基元树冠/建筑的放置只按平移，**缩放与旋转不影响体元位置**（源码标注为已知不一致）（models/histream/src/base/scene.cpp:2442）。建筑位置文件行为 3–5 列 `x y z [scale [rotation]]`，非法行、`scale≤0`、无实例都抛错（models/histream/src/base/scene.cpp:706-743）。

**OBJ 体元化**（`createObjFilledVoxels`，models/histream/src/base/scene.cpp:1360-1978）：

- 把三角形按体元裁剪，累计每体元内表面积，覆盖比 `min(1, 面积/体元面面积)` 小于 `voxelFillThreshold`（缺省 0.05，夹到 [0,1]）的体元被剔除；输出按 (x,y,z) 排序，保证可复现（models/histream/src/base/scene.cpp:586-606）。每个三角形遍历整个 3D 包围盒，大三角形（地面/屋顶）耗时随之放大（源码标注）（:571）。
- `leafDensitySource=geometry` 时叶面积密度取体元内面积 / 体积（m² m⁻³），否则取冠层密度参数（models/histream/src/base/fileio_json.cpp:603-604，models/histream/src/base/scene.cpp:608-615）。
- 建筑/土壤/水体按不透明处理并生成面积加权外法向，地面法向不允许朝下；建筑法向用封闭实体索引按几何朝向统一（models/histream/src/base/scene.cpp:1417-1430、1879-1883）。
- 逐放置点用**逆映射**填格：把局部包围盒角点变换到世界格范围，取每个世界格中心反变换回局部体元；旋转/缩放过的不透明网格补齐对角缝，保证外壳面连通；旋转或缩放过的放置点改用“世界对齐代理网格”（单实例、纯平移），对应实例 `meshId=-1`，TLAS 跳过但保留 id（models/histream/src/base/scene.cpp:1674-1778、1917-1974）。越出 `[0,N)` 的格计入 `clipped` 并丢弃（:1843-1849）。
- 一个体元被多个对象占用时保留先写入者的类型，只累加叶面积密度（源码标注为已知风险）（:1887-1898）。
- 每次放置点打印 `OBJ placement <i> terrain-height=… inserted=… mesh-overlap=… clipped=…`；全部网格体元化后为 0 抛错 “no voxelizable triangles”（:1439-1442、1910-1914）。
- 放置点高度的地形抬升：`demHeightAboveMinimum`，把场景范围线性拉伸到整幅 DEM 后双线性插值，x（北）映射为行（翻转）、y/z（东）映射为列（models/histream/src/base/scene.cpp:133-152）。

### 3.6 异质性体元（HEX）

开关 `control.heterogeneousVoxel`（缺省 false）；`uploadSetting` 先于场景构建执行，保证开关在体元化前生效（models/histream/src/voxelrt/voxelrt.cpp:160-166）。物理定义见《理论手册》§6。

构建流程（仅对 OBJ 体元化路径有效）：

1. `hexvoxel::loadObj` 读完整 OBJ（只识别以 `v `/`f `/`o `/`g ` 开头的行，面最多 32 个顶点，顶点先转 float 体元坐标，投影坐标大时精度下降）；失败仅向 stderr 报 “Heterogeneous voxel extraction failed”，**退回均质体元**（models/histream/src/base/hexvoxel.cpp:230-304，models/histream/src/base/scene.cpp:1531-1533）。
2. `hexvoxel::compute`（默认 `sampleN=64` 截面采样、`colM=4`、`volK=4`、`includeInteriors=true`）对每个属性网格得到每体元的三轴投影面积、并集覆盖掩码和 `rho`（models/histream/src/base/hexvoxel.h:104-109）。
3. **稠密网格上限**：`gx×gy×gz` 由网格 AABB 在体元单位下推出（各维 `floor(max)+1`），**超过 25,000,000 格抛 “Heterogeneous voxel grid is too large (…); increase the voxel size or disable heterogeneous voxels”**（models/histream/src/base/hexvoxel.cpp:871-886）。该异常发生在 `Engine::input` 内且未被捕获，退出码 4。每个网格一个 `std::vector` 的实现使内存与格数线性相关（源码标注）（:915-916）。
4. `finalizeHexMixtures`：按坐标排序保证确定性；沿三个轴的联合覆盖率 `C`、`rho`、`CI`（`−ln(max(1−C, 0.5/N²))/投影面积`，随后由 `hexneighbors::axisQ` 以位掩码重算）写入 `VoxelHex{model=10}`，并为六个面写邻居间隙相关量 `beta`（相邻体元的 `sampleN` 不同则抛 “Neighbour HEX projection resolutions differ”）；要求立方体元，否则抛 “Heterogeneous HEX requires cubic voxels”（models/histream/src/base/scene.cpp:1015-1107）。
5. **多类别混合**：同一世界体元内按（类型、光谱、热属性）归类，**只保留光学贡献最大的两类**，主类权重量化为 n/1024，相同组合共享一个合成材质；合成光谱只保留反射率/透射率（镜面参数丢失，源码标注），反射率夹到 [0,1]、透射率夹到 [0,1−反射率]；合成热属性按权重线性平均温度（models/histream/src/base/scene.cpp:1229-1320）。
6. **实例旋转**：HEX 投影仅支持 90° 整数倍的旋转，其他角度被取整为最近的 90° 倍数，而几何按真实角度旋转（源码标注为已知不一致）（models/histream/src/base/scene.cpp:818-820）。
7. 可选 LOD 表：环境变量 `HISTREAM_HEX_LOD_TABLE`（格式 `HEX_LOD_V1`，层数 1–6，每边细分数从 1 起逐层翻倍且 ≤32，根体元数 ≤100000 且须等于场景中的 HEX 体元数，总条目 <16,000,000），`HISTREAM_HEX_CONE` 当前仅接受 `lod` 且必须同时给 LOD 表（models/histream/src/base/scene.cpp:944-1013）。`HISTREAM_HEX_CONE=lod` 还会把传感器 `aperture` 置 −4 作标记（models/histream/src/base/geometry.cpp:382-392）。
8. 汇总日志 `Heterogeneous voxel merge: cells=… mixed-categories=… shared-mixtures=… descriptors=…`（models/histream/src/base/scene.cpp:1343-1346）。

### 3.7 加速结构与规模上限

#### 3.7.1 BLAS/TLAS

`AccelStruct::createAccelStruct`（models/histream/src/base/accelstruct.cpp:94-181）：每个网格一个 BLAS（`PREFER_FAST_TRACE | ALLOW_COMPACTION`，顶点格式 `R32G32B32_SFLOAT`，步长取 `MeshBuffer::vertexStride`，缺省 44 字节；无索引网格 `indexType=NONE`，图元数为 `顶点数/3`）；每个实例一个 TLAS 条目：强制不透明、`mask=0xFF`、`instanceCustomIndex=实例序号`（着色器里的 `gl_InstanceCustomIndexEXT`）、同一 hit group（SBT 偏移 0）。`meshId=-1` 的实例被跳过，但其序号仍占用 `instanceCustomIndex`；`meshId` 越界抛错 “Instance i references invalid meshId”（:151-175）。

体元模式的代理网格格式由 `control.voxelProxyGeometry` 决定：`full`（44 字节顶点+索引，逐位复现旧结果）、`position`（12 字节仅位置+索引）、`positionNoIndex`（缺省，12 字节三角形列表、无索引）；后两者经固定 48 MiB 的 staging 分块上传（models/histream/src/base/voxelflags.h:38-49，models/histream/src/base/proxymesh.h:11、29）。

#### 3.7.2 规模上限与失败条件

所有上限检查都发生在 `create` 阶段（缓冲创建/BLAS 构建），异常经 `main` 捕获后退出码 3；信息均带体元数和上限（models/histream/src/main.cpp:172-175）。

| 检查 | 条件（失败即抛 `runtime_error`） | 依据 |
|---|---|---|
| 单个逐体元存储缓冲 | `体元数 × 每体元字节数 > maxStorageBufferRange`（NVIDIA 为 4 GiB−1）。VoxelRT 检查：VoxelLink 48 B、VoxelDir 8 B、VoxelNetRad 16 B、VoxelTempe 12 B、辐射缓冲 `16×n_wave` B | models/histream/src/base/voxellimits.h:26-39，models/histream/src/voxelrt/buffer.cpp:52-58、322-326 |
| 32 位索引 | `体元数 × 每体元条目数 > 4,294,967,295`（辐射缓冲条目 `2×n_wave`） | models/histream/src/base/voxellimits.h:42-48，models/histream/src/voxelrt/buffer.cpp:327 |
| 1D 派发组数 | `ceil(体元数/64) > maxComputeWorkGroupCount[0]`（规范下限 65535，即 ≈4.19 M 体元） | models/histream/src/base/voxellimits.h:51-62，models/histream/src/voxelrt/buffer.cpp:58 |
| 单次显存分配 | BLAS 结构/构建 scratch/代理顶点（含同尺寸 staging）超过 `maxMemoryAllocationSize`（NVIDIA 4,292,870,144 B）。`full` 格式约 16.26 M 个面即超限 | models/histream/src/base/voxellimits.h:64-87，models/histream/src/base/accelstruct.cpp:114-140，models/histream/src/base/proxymesh.h:6、38-48 |
| 输出影像缓冲 | `W×H×波段数×4 B > maxStorageBufferRange`；任一维 ≤0 也抛错 | models/histream/src/base/outputsize.h:26-41 |
| RayTracing 纹理因子 | 因子数组（每实例每三角形一个 float）超过 `maxStorageBufferRange`；实例三角形总数需 <2³²−1 | models/histream/src/raytracing/buffer.cpp:121-129，models/histream/src/raytracing/raytracing.cpp:104-105 |
| RayTracing 空场景 | 无实例或无网格：stderr 提示并 `create` 返回 false → 退出码 3 | models/histream/src/raytracing/raytracing.cpp:339-344 |
| HEX 稠密网格 | >25,000,000 格 → 退出码 4（场景构建阶段） | models/histream/src/base/hexvoxel.cpp:880-886 |
| 着色器缺失 | RayTracing 4 个 `.spv` 任一缺失抛错；VoxelRT 只对 gap 阶段缺失打日志，随后创建失败返回 false | models/histream/src/raytracing/pipeline.cpp:174-178，models/histream/src/voxelrt/pipeline.cpp:81-85、116-121 |

着色器根目录为 `definedDir/shader/<raytracing|voxelrt>/`，`definedDir` 是可执行文件目录（`runtimeDefinedDirectory().parent_path()`，即 `defined/` 的上一级）（models/histream/src/base/fileio_json.cpp:399）。内存预算方面，源码**没有**针对 CPU 内存的统一预算检查；仅有上表各项设备限制，总显存不足时的行为由 Vulkan 分配器决定（待核对）。

### 3.8 五种计算模式总览

| 模式 | 输入 | 核心阶段 | 输出 | 本章详述 |
|---|---|---|---|---|
| RayTracing（eRaytracing） | OBJ 网格 + 背景/DEM + 光谱/热属性，传感器/光源/大气 | 网格→BLAS/TLAS；`raygen` 对每像素做多样本蒙特卡洛路径追踪（含热红外发射），逐观测角/航点出图 | 多波段 float32 TIFF、可选 `albedo.txt`、`result_statistics_raytracing.csv` | 是（§3.9） |
| VoxelRT（eVoxelRT） | 体元场景（基元/OBJ 体元化，可含 HEX）、冠层、光谱/热属性 | 计算管线 `gap`（太阳可见性）→ `diffuse`（逐波段×碰撞阶数的漫射传输）→ `out`（成像） | 多波段 TIFF、可选 `voxelrt.bin/.json`、`result_statistics_voxelrt.csv` | 是（§3.10） |
| VoxelEB（eVoxelEB） | 体元场景 + 气象序列 + 土壤/植被/水体参数，可选流体 | 逐时间节点：气象更新 → 太阳角 → 可选流体 → 能量平衡迭代（直射/散射辐射、生化、空气动力、蒸散、温度更新）→ 观测成像 | 逐节点 TIFF、`result_statistics.txt` 等；`PROCESS` 行指向体元过程文件 | 仅在此概述（models/histream/src/voxeleb/voxeleb.cpp:489-613） |
| FacetRT（eFacetRT） | `project.json`（自读，不经 `FileIO`）、OBJ/DEM | 面元可见性图（GPU/CPU 光栅）→ Jacobi 辐射迭代 | `facetrt.json`，stdout `RESULT` 行 | 仅在此概述（models/histream/src/facetrt/facetrtcore.cpp:5813-5875） |
| FacetEB（eFacetEB） | 同上 + 气象 | 共享可见性图 → 逐节点辐射-能量耦合迭代（残差收敛） | `faceteb.json`、`faceteb_run.json`、逐节点 `.bin`；退出码 0/2/3 | 仅在此概述（models/histream/src/facetrt/facetrtcore.cpp:5884-5979、9379-9383） |

共同点：RayTracing/VoxelRT/VoxelEB 经 `FileIO` + `Scene` + Vulkan 设备（`AppSetting`）；Facet 两个模式由 `facetrtcore.cpp` 自管 Vulkan 与输入解析，`Faceteb/Facetrt` 的 setup/upload 只做路径与标志检查（models/histream/src/facetrt/facetrt.cpp:32-49，models/histream/src/faceteb/faceteb.cpp:34-51）。VoxelRT 与 VoxelEB 共享场景构建、NanoVDB 与描述符布局的绝大部分代码，VoxelRT 没有气象、流体与能量平衡（models/histream/src/base/structs.h:745-774）。

### 3.9 RayTracing 运行流程

#### 3.9.1 阶段与对象

`Raytracing` 的生命周期：`setup → upload → create → run → destroy`，运行状态全部保存在 `RaytracingIO`（models/histream/src/raytracing/raytracing.h:35-85，models/histream/src/raytracing/raytracingio.h:67-235）。

- **setup**：取设备/队列，建命令池、资源分配器、光追属性、`SBTWrapper` 与加速结构构建器（models/histream/src/raytracing/raytracing.cpp:260-308）。
- **upload**（纯 CPU，顺序有依赖）：`readDefined` → `createCompOptical`（光谱/热属性）→ `createObjScene`（网格与实例）→ `createGeometry`（分辨率、角度表、UAV 航点、初始传感器与光源）→ `uploadSetting` → `buildPhysicalTextureFactors`（models/histream/src/raytracing/raytracing.cpp:316-331）。
- **create**：缓冲 → 描述符 → 管线（含 SBT）→ 命令 → `updateSetting`（models/histream/src/raytracing/raytracing.cpp:335-351）。

#### 3.9.2 缓冲与描述符

`Buffer::createBuffer` 的顺序（每个上传独立命令缓冲并同步等待）：光谱材料 → 热材料（空则补一个 300/300 K 元素，因 raygen 总读取）→ 波长集 → 逐网格顶点/索引（`VERTEX|STORAGE|SHADER_DEVICE_ADDRESS|AS_BUILD_INPUT`）→ 回填 `MeshLink` 的顶点/索引设备地址并上传 → 实例链接 → 物理纹理偏移/因子（无纹理时各放 1 个占位）→ BLAS/TLAS → 传感器与光源 uniform（`DEVICE_LOCAL|HOST_COHERENT`）→ 输出存储缓冲（models/histream/src/raytracing/buffer.cpp:25-183）。

输出存储缓冲的元素数为 `W×H×n_wave + 1`（末尾 1 个 float 是 raygen 丢弃的非有限样本计数，读回后非零则 stderr 警告）；开启自适应采样时再加 `16 + 2×W×H` 个 float（覆盖样本数与先导 σ 两个平面）（models/histream/src/raytracing/buffer.cpp:167-174，models/histream/src/raytracing/raytracing.cpp:245-251）。

描述符集共 11 个 binding：`spectral, thermal, modelLink, instanceLink, tlas, sensor(UBO), light(UBO), wave, storage, textureOffset, textureFactor`，后两者仅 raygen 可见；光谱、波长集、实例链接缓冲缺失时抛明确错误（models/histream/src/raytracing/descriptor.cpp:38-64，models/histream/src/raytracing/descriptor.h:27-41）。

#### 3.9.3 管线与命令

管线：1 个描述符集 + 推送常量 `RayRTSetting`（80 字节，raygen/closest-hit/miss 可见）；4 个着色器 `offline.rgen`、`closestHit.rchit`、`rmiss.rmiss`、`shadow.rmiss` 从 `definedDir/shader/raytracing/*.spv` 加载；SBT 布局 raygen(1)|miss(2)|hit(1)（models/histream/src/raytracing/pipeline.cpp:149-178、169-172，models/histream/src/base/structs_cg.h:90-116）。raygen 的特化常量：采样器类型、波段数容量 `RT_MAX_WAVE`（取 {16,32,64,128,800} 中不小于 `n_wave` 的最小值，因此波段数上限 800）、自适应开关（models/histream/src/raytracing/pipeline.cpp:185-199）。

追踪命令 `Command::run`：把整幅图像按行分块，每块设置 `setting.imageOffsetY` 与 `dispatchRowCount` 后 `vkCmdTraceRaysKHR(宽, 块行数, 1)`、提交并等待栅栏；块大小初值约 65536 像素，之后按实测耗时自适应，目标约 120 ms/块，下一块行数 = 本块×clamp(120/耗时, 0.25, 4)，不低于约 32768 像素（避免 Windows TDR）；结果与块大小无关（逐像素种子）（models/histream/src/raytracing/command.cpp:55-99、183-216）。每次提交前插入全局内存屏障（:141-145）。

#### 3.9.4 观测循环

```
run():
  无 UAV 航点且 n_angle<=0 或 UAV 航点为空 → stderr 报错，return false（退出码 1）
  PROGRESS 5  "加载光线追踪场景与观测参数"
  若 isAlbedo：清空 <outDir>\albedo.txt
  ┌ UAV 巡航（isUAVTrave 且 n_pos>0）：for kpos in [0,n_pos)
  │     updateSensorPos(kpos)        // 透视相机置于航点；vaa 取航点航向
  │     OBSERVATION "航点 k/n X= Y= H="
  │     trace()
  │     若 isImage||isOrth：output(kpos)        // 透视输出，文件名带 _P=kpos
  │     PROGRESS 10+85*(kpos+1)/n_pos "中心投影巡航 k/n"
  └ 常规：for kangle in [0,n_angle)
        PROGRESS 10+floor(80*kangle/n_angle) "光线追踪观测 k/n"
        updateAngle(kangle)          // 按 vza/vaa 重建传感器，按 sza/saa 重建光源
        trace()
        若 isImage||isOrth：透视 → output；平行 → outputOrth
        若 isAlbedo：outputAlbedo
        打印 "Success: <kangle>"
        PROGRESS 10+floor(85*(kangle+1)/n_angle) "完成观测 k/n"
  PROGRESS 100 "简单光线追踪计算完成"
```
依据：models/histream/src/raytracing/raytracing.cpp:545-626。

`trace()`：缺省直接 `Command::run`；开启自适应采样（`control.adaptiveSampling` 或 `HISTREAM_RT_ADAPTIVE`，缺省关）且样本数 ≥8、采样器为 Sobol 时执行两遍：侦察遍用约 25%（`HISTREAM_RT_ADAPTIVE_SCOUT`，夹到 0.05–1，至少 4）预算只估计单样本相对标准差 σ；回读 σ 后二分求缩放系数使每像元样本数 `clamp(round(s·σ), max(2,总/4), 4×总)` 的总和达到预算；正式遍按分配样本数重算，侦察样本不进入结果，故无偏，额外开销约 +25% 样本。自适应与 legacy 采样器不兼容（警告并关闭）（models/histream/src/raytracing/raytracing.cpp:371-417、460-540）。

#### 3.9.5 输出写出规则

- **透视/UAV**（`output`）：读回 `[波段][行][列]` float 缓冲，逐像元：值为 0 或非有限且位于视场天空区域的像元写天空辐射值（含 `skyTemperature`），其余做大气校正（`atmosphereCorrectOutputValue`，传感器高度取航点高度或 `sensor.position.z`）（models/histream/src/raytracing/raytracing.cpp:845-863）。
- **平行**（`outputOrth`）：由 `Geometry::orthcorrect` 拟合 4 个角点的双线性映射（`源 = a·c0+b·c1+a·b·c2+c3`，像元中心取 +0.5 偏移），仅在被场景完整覆盖的原始像元范围内**双线性插值**，覆盖区外保持 0，随后大气校正（models/histream/src/raytracing/raytracing.cpp:689-757，models/histream/src/base/geometry.cpp:114-153）。校正缩放固定 1.0，保持真实长宽比（:37）。
- **TIFF**：`FileIO::writeTIFData` 写 `float32`、`INTERLEAVE=BAND`、`BIGTIFF=IF_SAFER` 的多波段 GeoTIFF；文件名 `SZA=xx.xx_SAA=xx.xx_VZA=xx.xx_VAA=xx.xx[_P=<航点>][_a].tif`，角度保留 2 位小数；大气校正启用时后缀 `_a`。波段说明：≤2.5 μm 为 `Reflectance [-] @ λ nm`，更长波段为 `Brightness temperature [K]`（`sensor.temperature` 缺省 true）或 `Spectral radiance [W m-2 sr-1 um-1]`（models/histream/src/base/fileio.cpp:2335-2380、2402-2417，models/histream/src/raytracing/raytracing.cpp:184-201、763-769）。写失败抛 “RayTracing: failed to write output TIFF”，退出码 1。
- **统计文件**：每写一个 TIFF 即向 `<outDir>/result_statistics_raytracing.csv` 追加每波段一行（列 `file,mode,time,vza,vaa,sza,saa,band_index,band_name,count,min,max,mean,stddev`），统计忽略非有限值，标准差为总体标准差，无有效像元时 min/max/mean/stddev 留空；同一进程内**首次写入覆盖旧文件并写表头**，之后追加（models/histream/src/base/fileio.cpp:2455-2525，models/histream/src/base/fileio.h:114-117）。
- **albedo.txt**（`sensor.albedo=true`）：每个观测角每个波段追加一行 `波长 sza saa vza vaa 均值`，均值是**未做大气校正**的原始图像中值 >0 的像元均值，零像元被排除（因此在视场内含真实零值时偏高）（models/histream/src/raytracing/raytracing.cpp:898-939）。该文件路径以 `\\` 拼接（Windows 约定）（:570、928）。
- 标准输出的 `PROGRESS\t<百分比>\t<文字>`、`OBSERVATION\t…` 供服务端解析；其余日志行包括 `GeoTIFF output: <路径>`、`Statistics output: <路径>`。

### 3.10 VoxelRT 运行流程

#### 3.10.1 阶段与对象

`Voxelrt` 与 `Raytracing` 同构（`setup/upload/create/run/destroy`），状态在 `VoxelrtIO`（models/histream/src/voxelrt/voxelrt.h:43-90，models/histream/src/voxelrt/voxelrtio.h:65-232）。

- **upload**：`readDefined` → `createCompOptical`（含冠层）→ **`uploadSetting`（必须先于场景）** → `createPrimObjScene` → `createGeometry`（models/histream/src/voxelrt/voxelrt.cpp:155-175）。VoxelRT 的 `createGeometry` 以体元步长作米/单位换算传感器位置（models/histream/src/base/geometry.cpp:919-920、331-343）。
- **updateSetting**（`create` 末尾）填充推送常量 `VoxelRTSetting`：`scale=stepsize_surface`；`maxStep=clamp(maxDepth×128, 128, 2048)`（缺省 depth=4 时 512，防止稠密植被中数百次有序三角形穿越被截断）；`voxelSize.y` 重算为竖直层数；`collisionOrders=control.voxelCollisionOrders`（2–5）；`periodicNeighborCount`、`skyboxEnabled`、`voxelFlags`（models/histream/src/voxelrt/voxelrt.cpp:212-244）。该结构须 ≤128 字节推送常量范围，但缺少与 GLSL 对应的偏移断言（models/histream/src/base/structs_cg.h:211-240）。

#### 3.10.2 缓冲、描述符、管线

`Buffer::createBuffer`（models/histream/src/voxelrt/buffer.cpp:35-366）顺序：规模检查（见 3.7.2）→ 光谱（空则 1 个占位）→ 热属性（空则 300 K 占位）→ 冠层（空则占位）→ 水体参数（空则占位）→ **逐体元温度（初值 305 K/295 K）** → 逐网格代理顶点/索引（按 `voxelProxyGeometry` 选格式）→ `MeshLink`（回填设备地址）→ 实例链接 → `VoxelLink`、`VoxelHex`、NanoVDB 数据（`nanoHandle` 为空时三者放占位）→ BLAS/TLAS（带设备尺寸检查）→ 传感器/光源 → 波长 → `VoxelDir`（全 0）→ 辐射缓冲 `VoxelRad`（`n_voxel × 2n_wave` 条，每波段一个累计环境项加一对前后碰撞阶乒乓项）→ `VoxelNetRad` → 输出影像 → LAD。

描述符 18 个 binding：`spectral, thermal, canopy, meshLink, instanceLink, voxelLink, nano, tlas, sensor, wave, light, dir, rads, netRad, storage, lad, hex, waterSet`，阶段含 compute 与 raygen/intersection 等（models/histream/src/voxelrt/descriptor.cpp:43-61，models/histream/src/voxelrt/descriptor.h:26-46）。

管线：1 描述符集 + 1 段 compute 推送常量；三条计算管线按 `VoxelRTStage` 创建：`gap`=`voxelrad_solar.comp.spv`、`diffuse`=`voxelrad_diffuse_single.comp.spv`、`out`=`voxelrad_image.comp.spv`（目录 `definedDir/shader/voxelrt/`）；周期边界以特化常量 0（开关）、1（穿越次数上限，夹到 0–20）在管线创建时固化（models/histream/src/voxelrt/pipeline.cpp:62-128）。管线布局的推送常量范围按 `sizeof(VoxelLstSetting)=128` 声明，实际推送 `VoxelRTSetting`，范围不小于实际长度，可用（:60-63）。

#### 3.10.3 派发顺序

`Command::runRT(reuseTransport)`（models/histream/src/voxelrt/command.cpp:96-148）：

```
voxelSize1D = ceil(n_voxel/64);  voxelSize2D = ceil(W/8) × ceil(H/8)
若 reuseTransport：仅 submit(out, 2D)；return
submit(gap, 1D) → 等待
for band in [0, n_wave):                       // 每个波段单独提交，避免单条命令过长触发 TDR
    n_jump = band
    for order in [1, collisionOrders]:         // 每阶读上一阶全部体元，必须串行
        collisionOrder = order
        submit(diffuse, 1D) → 等待
n_jump = 0; collisionOrder = 1
submit(out, 2D) → 等待
```

加速求解器（`control.radiationSolver="accelerated"`）使用 `runRTAccelerated(批=4)`：diffuse 阶段每次提交处理最多 4 个波段（`spectralBatchSize` 夹到 1–4，最后一批可能不足），其余相同（models/histream/src/voxelrt/command.cpp:152-218）。每次派发前插入全局内存屏障，并 `vkWaitForFences` 等待，阶段之间严格串行（:270-297）。`HISTREAM_VOXEL_PASS_TIMING=1` 时输出 `PASS_TIMING` 行（:55-61）。

**gap 与 diffuse 只依赖太阳方向**。`Voxelrt::run` 记录上一次传输的 `(sza,saa)`，相同则复用，只重跑 `out`（models/histream/src/voxelrt/voxelrt.cpp:284-300）。因此多个观测角共享同一太阳角时漫射传输只算一次。

#### 3.10.4 观测循环与输出

```
run():
  k_node = -1
  PROGRESS 5  "加载体元场景与辐射参数"
  ┌ UAV（isUAVTrave 且 n_pos>0）：for kpos
  │     updateSensorPos(kpos)；OBSERVATION "航点 k/n X= Y= H="
  │     runTransport(angles.front())         // 太阳取第一个角度，所有航点复用
  │     若 isRadiationProcess 且 kpos==0 → outputRadiationProcess
  │     若 isImage → outputPos(-1, kpos)     // 文件名带 _P=kpos
  │     （循环内不打印 PROGRESS）
  └ 常规：for kangle
        k_angle=kangle；updateAngle
        OBSERVATION "    vza_… vaa_… sza_… saa_…"
        runTransport(angle)
        若 isRadiationProcess 且 kangle==0 → outputRadiationProcess
        若 isImage：透视 → outputPos(-1,-1)；平行 → output(-1, kangle)
        PROGRESS 10+85*(kangle+1)/n_angle "辐射传输观测 k/n"
  PROGRESS 100 "体元辐射传输计算完成"
```
依据：models/histream/src/voxelrt/voxelrt.cpp:277-347。注意：UAV 分支循环内**不更新** `k_angle`，而 `outputPos` 用 `clamp(k_angle,0,size−1)` 选角度；`k_angle` 在 `VoxelrtIO` 中无初始值，但透视投影下 `viewAngles` 只有 1 项，clamp 后恒为 0（models/histream/src/voxelrt/voxelrtio.h:131，models/histream/src/voxelrt/voxelrt.cpp:635）。

输出差异（相对 RayTracing）：

- **平行投影** `output`：读回 `[波段][行][列]`，用同样的 `orthcorrect` 映射，但**最近邻（截断取整）**取样，不做双线性插值；源值为 0 的像元视为无数据，不做大气校正；映射越界的像元保持 0（models/histream/src/voxelrt/voxelrt.cpp:572-596）。
- **透视/UAV** `outputPos`：不重采样；天空判定与 RayTracing 相同（models/histream/src/voxelrt/voxelrt.cpp:655-672）。
- **文件名后缀**：`_v`（启用大气校正为 `_v_a`），时间标记恒为 −1（VoxelRT 无时间节点），故文件名 `SZA=…_SAA=…_VZA=…_VAA=…_v[_a].tif`；统计写入 `result_statistics_voxelrt.csv`，`mode` 列为 `VoxelRT`（后缀以 `_v` 开头即判为体元输出，有时间为 voxeleb、无时间为 voxelrt）（models/histream/src/voxelrt/voxelrt.cpp:598-602、673-677，models/histream/src/base/fileio.cpp:2457-2476）。
- VoxelRT 的 TIFF 写出**不检查返回值**，写失败不会中止运行（models/histream/src/voxelrt/voxelrt.cpp:599-602）；RayTracing 则抛异常。
- **辐射过程**（`sensor.radiationProcess`）：首个观测后一次性写 `voxelrt.bin`（float32 小端、体元交错；每体元 `x,y,z`（米，`(体元号+0.5)×步长`）、各波段值、`sunlit_fraction`，共 `4+n_wave` 个 float）与 `voxelrt.json`（`schemaVersion 2`，字段偏移、单位、定义），并向 stdout 打印 `PROCESS\t<json路径>`。光学波段为“归一化入射照明”，>2.5 μm 波段为受光/背阴两态按日照比例混合的出射光谱辐亮度；非有限值置 0（models/histream/src/voxelrt/voxelrt.cpp:353-501）。

#### 3.10.5 失败行为小结

| 阶段 | 情形 | 结果 |
|---|---|---|
| `input` | 工程非法、设备选择失败、场景构建异常（HEX 超限、OBJ 打不开、光谱缺失） | 退出码 4 |
| `create` | 规模超限、管线创建失败（`create` 返回 false）、Vulkan 调用失败 | 退出码 3 |
| `run` | 运行期异常、RayTracing 无观测或无航点、RayTracing TIFF 写失败、`vkCheck` 失败 | 退出码 1 |
| 观测数据 | RayTracing/VoxelRT 的 `n_angle≥1` 总成立 | 不会出现“0 退出码但无 TIFF”的情形（RayTracing 对此仍保留防护） |

### 3.11 待核对与已知实现注记

- 平地背景网格的水平格数（`ceil`）与计算域格数（截断）在 `scene.x/voxel` 非整数时不一致，对结果的影响待核对（models/histream/src/base/scene.cpp:2939、models/histream/src/base/voxeldesigner.cpp:1150）。
- 总内存（CPU/显存）预算：源码仅有 3.7.2 所列单项检查，没有总量预算（待核对）。
- VoxelRT 着色器文件缺失时仅 gap 阶段有存在性检查，其余阶段的失败路径依赖 `nvh::loadFile`/`vkCreateComputePipelines`（待核对）。
- FacetEB 退出码 3 与 `main` 的“资源创建失败”同码不同义（models/histream/src/facetrt/facetrtcore.cpp:9383）。
- 路径经 `std::string` 传递，含代码页外字符的路径在 Windows 上可能失败（models/histream/src/main.cpp:126，models/histream/src/base/fileio_json.cpp:398）。

## 4. 面元与体元求解器运行流程（FacetRT / FacetEB / VoxelEB）

本章描述 `histream` 引擎在 FacetRT、FacetEB、VoxelEB 三种模式下“程序实际如何运行”：阶段划分、入口函数、调用的着色器、读写的缓冲与状态量、收敛判据、退出码与输出文件。物理方程与推导不在此重复，请查《理论手册》（`docs/theory-manual.md`）：面元可见性与辐射度见 §4，体元传输见 §5，能量平衡与 RT–EB 迭代见 §7（7.1–7.5），光谱窗口加速见 §8，水体见 §10，火/雾见 §11，简化流体见 §12。界面参数的含义见《用户手册》。

文中括号内为源码依据（仓库相对路径:行号）。`models/histream/src/facetrt/facetrtcore.cpp` 以下简记为 `core.cpp`（FacetRT 与 FacetEB 的实现都在该文件）。

### 4.1 进程入口、生命周期与退出码

#### 4.1.1 命令行与引擎生命周期

命令行为 `histream <eFacetRT|eFacetEB|eVoxelEB|eVoxelRT|eRaytracing> <project.json> [输出路径]`（models/histream/src/main.cpp:60）。`main` 依次执行 `Engine::input → create → run → destroy`（main.cpp:159-188）。

| 阶段 | 失败时的进程退出码 | 说明 |
|---|---|---|
| `input` 抛异常 | 4 | 工程解析/模式字符串非法（main.cpp:160-163） |
| `create` 返回 false 或抛异常 | 3 | GPU 资源创建失败（main.cpp:165-176） |
| `run` 抛异常 | 1 | 运行失败（main.cpp:181-185） |
| `destroy` 抛异常且 run 成功 | 1 | 清理失败也按失败处理（main.cpp:187-192） |
| 参数缺失/模式名非法 | 64 | 用法错误（main.cpp:56,130-148） |
| 成功 | 0 | |
| FacetEB 完成但有未收敛节点 | 2 | 结果保留（main.cpp:12） |

Facet 两种模式不经 `FileIO`：`Engine::input` 只记录工程路径、着色器目录（可执行文件旁 `shader/facetrt` 或 `shader/faceteb`，找不到再找当前目录，最后用编译期宏目录）与输出路径（models/histream/src/base/engine.cpp:168-181,252-273），工程由 `core.cpp` 自己解析。`Facetrt/Faceteb` 的 `setup/upload/create` 只校验输入路径与着色器目录非空并置阶段标志，不分配 GPU 资源（models/histream/src/facetrt/facetrt.cpp:32-62；models/histream/src/faceteb/faceteb.cpp:34-64）；所有 Vulkan 资源都在 `run` 内部由 `FacetrtVulkan` 创建（`core.cpp:6177,6204`）。`Engine::run` 对 FacetRT 只返回 0/1（engine.cpp:291），对 FacetEB 返回 `FacetebIO::exitCode`（engine.cpp:293-298）。

#### 4.1.2 JSON 工程到内部输入的转换

Facet 引擎读取 `.json` 时先把它转成内存中的 XML 视图（`facetInputView`，core.cpp:3787-4170；`readTextFile` 在扩展名为 `.json` 时调用，core.cpp:4173-4179），再用标签读取。因此参数缺省值与限幅实际定义在 XML 读取处。下表列出本章用到的控制项（`control` 段键 → 取值/默认/限幅）：

| JSON 键 | 默认 | 取值、限幅与说明 | 依据 |
|---|---|---|---|
| `facetBackend` | `gpu` | `gpu`/`cpu`/`auto`；环境变量 `HISTREAM_FACET_BACKEND` 优先；其他值按 gpu | core.cpp:3810,5774-5782 |
| `facetVisibility` | `auto` | `auto`/`raster`/`rayquery`；非法值抛错；环境变量 `HISTREAM_FACET_VISIBILITY` 在 `initialize` 中覆盖 | core.cpp:5786-5795；facetrt_vulkan.cpp:303-311 |
| `facetOutputFormat` | 引擎缺省 `json` | `json`/`binary`；环境变量 `HISTREAM_FACET_OUTPUT_FORMAT` 覆盖；其他值抛错（Stream Studio 工程模型自行写入 `binary`，见 docs/facet-binary-output.md:16） | core.cpp:5799-5808 |
| `gpu` | 0 | GPU 序号；超出候选数时静默取最后一块 | core.cpp:3813,6166；facetrt_vulkan.cpp:452 |
| `periodicTraversalCount`/`periodicNeighborCount` | 0 | 周期邻块数，限幅 [0,20]；边界为真且未给时缺省 8；实际取整到 0/4/8/12/20 | core.cpp:3815-3821,5244-5251 |
| `couplingIterations` | 60 | RT–EB 外层耦合最大次数，限幅 [1,100] | core.cpp:3822,5970-5972 |
| `temperatureTolerance` | 0.05 K | 限幅 [0.001,10]；**只写入记录，不参与收敛判断** | core.cpp:5973-5974,5984-5986 |
| `temperatureRelaxation` | 0.8 | 非叶面元的温度松弛因子，限幅 [0.05,1] | core.cpp:5975-5976 |
| `stabilityCorrection` | false | Monin–Obukhov（MOST）稳定度修正开关 | core.cpp:5969 |
| `spectralAccelerationWidth` | 100（nm） | 短波窗口宽度，限幅 [1,1000] | core.cpp:6005-6008；fileio_json.cpp:282-283 |
| `vegetationTemperatureMethod` | 0 | 0 = Ball–Berry/Collatz 经验法；1 = Farquhar/von Caemmerer 机制法；限幅 [0,1] | core.cpp:6009-6011 |
| `soilTemperatureMethod` | 材质 `params.method`，缺省 1 | 土壤热方法 0/1/2（见 4.3.7） | core.cpp:3931,4559-4560 |
| `soilInertiaHistory` | `truncated` | 取 `full` 时方法 1 增加指数记忆模态 | core.cpp:3829,6014 |
| `radiationSolver` | `traditional` | `accelerated` 时 FacetRT 光谱波段使用收敛判据加速求解 | core.cpp:3827,4600-4601 |
| `shortwaveScatteringOrders`（仅 VoxelEB） | 1 | 只能 1/2/3，否则抛错 | fileio_json.cpp:288-290 |
| `radiationMaxSteps`（仅 VoxelEB） | 64 | 限幅 [1,4096] | fileio_json.cpp:293-294 |
| `cpuThreads` | 32 | 小于 1 取 32，且不超过逻辑处理器数；通过 `HISTREAM_CPU_THREADS` 传给后端 | engine.cpp:152-165 |

气象段 `meteo`：`start`（≥0，缺省 0）、`end`（缺省 1，至少 start+1）、`dTime`（秒，缺省 1800）、`latitude`/`longitude`/`year`/`timezone`（缺省 40/116/2019/8）、`z`（参考高度）、`Ca`（CO₂ 浓度，核心中限幅 [100,2000]）、`Tsold`（初始土壤温度）、可选 `timeStepSeconds`（逐节点步长，仅 Facet，个数必须等于 `end`，每个值须在 [1,86400]）（core.cpp:4136-4168；fileio_json.cpp:878-887）。**没有独立的“spin-up 参数”**：节点区间 [start,end) 之前的时段不会被积分，需要预热时由使用者把 `start` 前移；STREAM 的“起转节点数”属于 STREAM 层的节点编排，不在本章范围（models/stream/app/config.h:190-195）。

#### 4.1.3 三种模式对照

| 项 | FacetRT | FacetEB | VoxelEB |
|---|---|---|---|
| 几何 | 三角面元，每面元两侧 | 同左 | 体元（代理面 + NanoVDB 网格） |
| 辐射求解 | 可见性图 + Jacobi 辐射度（GPU，CPU 可选） | 同左，逐节点短波窗口 + 长波耦合 | 光线步进：太阳可见度、直射、1–3 阶散射、热红外漫射 |
| 能量平衡 | 无 | 有（GPU `energy.comp`，CPU 对照） | 有（逐阶段着色器，最多 50 次迭代） |
| 时间循环 | 无（单一太阳方向） | [start,end) 逐节点 | [start,end) 逐节点 |
| 未收敛 | 不适用 | 退出码 2，结果保留 | 退出码仍为 0，只记录未收敛状态数 |
| 主输出 | `facetrt.json`(+`.bin`) | `faceteb.json`(+`.bin`)、`.facet_steps/`、`faceteb_run.json` | TIFF 影像、`result_statistics.txt`、`diagnostics/`、`process/` |

### 4.2 FacetRT 运行流程

#### 4.2.1 阶段总览

入口：`runFacetRTCore`（core.cpp:5816-5880），外层 `try/catch`，任何异常写 stderr 并返回 1（core.cpp:5877-5880）。

```text
读工程/OBJ → loadProjectScene / loadTreeWithSoil           (core.cpp:5826-5828)
→ 读 FacetRTParameters + 光照 configureFacetRTLight        (5834-5837)
→ 选择后端 gpu | cpu | auto                                 (5843-5871)
   GPU: runGpu
     initialize → setGeometry → setGroundFacets → buildVisibilityGraph(40 方向)
     → computeSunlitFraction(太阳方向) → fillUnsampledSunlit
     → 分支：单波段 solve | GPU 光谱 runGpuSpectral | CPU 光学 + GPU solve
→ writeWebResult(facetrt.json) → stdout "RESULT\t<路径>"      (5872-5875)
```

输入是 `project.json`（或单个 `.obj`，OBJ 时只做宽带单波段并使用 700/50 W m⁻² 历史光照，见 core.cpp:4795-4801,5826-5831）。输出路径缺省为工程 `outDir` 下的 `facetrt.json`（core.cpp:5756-5770）。stdout 行 `PROGRESS\t<百分比>\t<阶段>` 与 `RESULT\t<文件>` 由上层服务解析（core.cpp:5839-5841,5875）。

#### 4.2.2 后端选择与回退

- `gpu`（缺省）：GPU 初始化或求解失败即抛错，退出码 1。
- `auto`：先 `runGpu`，失败（含无合格 GPU）后在 stderr 提示并回退到保留的 CPU 光栅路径（core.cpp:5859-5867）。
- `cpu`：全程 CPU 光栅 + CPU Jacobi（`runCpu`，core.cpp:3004-3058）。CPU 光栅遇到“片元链表溢出”时把每像素层数翻倍（上限 128 层，且片元缓冲 ≤ 2 GiB，每片元 12 B）并整体重跑，翻不动才抛错（core.cpp:192-193,441-458,5846-5854）。

#### 4.2.3 GPU 路径逐步（`runGpu`，core.cpp:3260-3449）

| 步骤 | 入口函数 | 着色器（`shader/facetrt/`） | 读写的缓冲/状态 |
|---|---|---|---|
| 1 设备与几何 | `FacetrtVulkan::initialize/setGeometry/setGroundFacets` | — | 顶点缓冲 `m_vertices`；A-buffer `m_fragments`/`m_pixelCounts`；边哈希 `m_edgeKeys/m_edgeCounts`；`m_denominator/m_skyCounts/m_groundCounts`（facetrt_vulkan.h:514-522） |
| 2 可见性图 | `buildVisibilityGraph` → `buildRawGraph` | 光栅：`visibility.vert/frag` + `visibility_resolve.comp`；光线查询：`visibility_rayquery.comp` | 对 40 个半球方向（黄金角螺旋，等立体角）逐方向写入哈希表与逐 side 计数（core.cpp:195,843-857；facetrt_vulkan.cpp:1773-1860） |
| 3 图压缩 | `buildCsrOnGpu`（缺省）或 `buildHostCsr` | `graph_build.comp`（count/compact/前缀和/scatter/sort/rowsums 多阶段） | 哈希表 → CSR：`m_rowOffsets`、`m_edges`（facetrt_vulkan.cpp:1948；shader/facetrt/graph_build.comp 文件头） |
| 4 下边界 | `applyGroundBoundary` | — | 地面朝下侧置为不参与（`kInactiveSide`），向下逃逸射线改看“等效地面”；写入诊断 |
| 5 受光比例 | `computeSunlitFraction` | `direct_resolve.comp` / `direct_rayquery.comp` | 太阳方向正交投影，4×4 = 16 次亚像素抖动累加 `m_directTotal/m_directVisible`；可见比例 = visible/total，无样本为 NaN（facetrt_vulkan.cpp:2504-2647） |
| 6 补全 | `fillUnsampledSunlit` | — | 无样本侧：背向太阳取 0，否则取 1 m 单元均值 → 3×3×3 邻域均值 → 场景均值（core.cpp:2773-2842） |
| 7 求解 | 见 4.2.5 | `radiosity.comp`、`ground_mean.comp`、`incident.comp`、`spectral.comp` | `m_radiosityA/B`（ping-pong）、`m_residual`、`m_incident`、`m_groundMean` |

可见性图的数据含义：每个 oriented side（side = 2×面元 + 0/1，0 为几何法向侧）有分母 `denominator`（射线总数）、`skyCounts`、`groundCounts` 与 CSR 邻接边 `{neighbor,count}`；分母为 0 的接收侧（亚像素面元）按全天空处理，并计入 `zeroSampleSides` 诊断（facetrt_vulkan.cpp:2395-2406）。`maxClosureError` 为各侧（天空+地面+全部边计数）/分母偏离 1 的最大值（facetrt_vulkan.cpp:2411-2433）。

#### 4.2.4 自动恢复与资源上限

`buildVisibilityGraph` 的重试环（facetrt_vulkan.cpp:1685-1713）：

| 异常 | 处理 | 失败条件 |
|---|---|---|
| `EdgeHashOverflow`（边哈希满） | 容量翻倍、重传几何、重建 | 已达 `maxStorageBufferRange/8` 个槽（2 的幂）仍满 → 抛出，退出码 1（facetrt_vulkan.cpp:773-788,1694） |
| `RasterLayerOverflow`（A-buffer 层数不足） | `growRasterLayers`：层数 = max(2×当前，实测所需层数向上取 2 的幂)，封顶 256；每片元 8 B | 超过 `maxStorageBufferRange`、显存预算（缺省最大 device-local 堆的 1/4，或环境变量 `HISTREAM_FACET_RASTER_BUDGET_MB`）或已达 256 层 → 抛错（facetrt_vulkan.cpp:841-874） |
| `RayQueryOverflow`（同一射线 >256 次命中） | 回退到光栅后端并重建 | — |

规模相关初值：可见性栅格缺省 1024×1024；场景水平尺寸 ≥ 1000 m 时 2048×2048 且层数 16（保持片元缓冲总量）；环境变量 `HISTREAM_FACET_RASTER_SIZE` 取 512–8192 的 2 的幂；初始层数按“全场平均片元深度×16 取 2 的幂”自适应，上限 64，`HISTREAM_FACET_RASTER_LAYERS` 只能取 64/128/256 且只升不降（core.cpp:184-194,403-423,5254-5267,5651-5667）。边哈希初始容量 = 2 的幂 ≥ max(65536, 64×side 数)（facetrt_vulkan.cpp:793-810）。`initialize` 要求每像素层数 ∈ [1,256]；几何必须为非空三角形列表、坐标有限、法向为单位向量（0.99–1.01）；设备需图形+计算队列、`shaderBufferInt64Atomics` 与 `fragmentStoresAndAtomics`（facetrt_vulkan.cpp:297-299,445-449,878-896）。若所有 side 都无采样则抛 “Visibility raster has no surface samples”（facetrt_vulkan.cpp:2383-2384）。

#### 4.2.5 辐射度求解与光谱路径

**Jacobi 迭代**（`radiosity.comp`，每线程一个面元的两侧）：对每侧 h = incoming + directIrradiance，其中 incoming = [skyCounts×L_sky + groundCounts×等效地面均值 + Σ count_j×B_j] / denominator；目标 B = emission + ρ·h_本侧 + τ·h_另一侧；`next = mix(old, target, relaxation)`；最大变化量以 `atomicMax` 写入 `m_residual`（shader/facetrt/radiosity.comp:99-157）。有地面时每次扫描前先由 `ground_mean.comp`（单工作组、固定归约顺序）更新等效地面均值（facetrt_vulkan.cpp:2719-2720；shader 文件头）。扫描结束后 `incident.comp` 以双精度累加给出各侧入射辐照度（incident 管线需要 `shaderFloat64`，facetrt_vulkan.cpp:1670-1678）。

三种求解入口：

| 入口 | 迭代规则 | 用于 |
|---|---|---|
| `solve` | 固定 `iterations` 次（FacetRT 为 64，初值 0） | 传统求解、单波段、FacetEB 的 CPU 对照 |
| `solveAccelerated` | 每批 `batchIterations`（8）次扫描后读回 `maxDelta`，≤ tolerance 或达到上限（64）停止；可给初值热启动 | FacetEB 短波窗口、FacetRT 参考路径 |
| `solveSpectralBand` | GPU 端 `spectral.comp` phase 5/6 用间接 dispatch 控制：8 批×8 次，已收敛后续组数置零 | FacetRT GPU 光谱 |

**FacetRT 光谱路径**（JSON 工程缺省走此路，core.cpp:5869-5871,3141-3255）：

1. `prepareGpuSpectral`：每面元 12 个 float（法向、反射率纹理因子、材质索引、物体标志、两侧受光比例、μ₀、Hapke 峰因子等）与每（波段,材料）4 个 float（ρ、τ、受光/阴影黑体辐亮度）一次性上传（core.cpp:2115-2175；`beginSpectral`，facetrt_vulkan.cpp:3226）。
2. 逐波段（`parameters.wavelengths`，缺省 550 nm）：波长 ≤ 2500 nm 为光学波段，否则为热红外（`isOpticalWavelength`，core.cpp:2104-2109）。每波段调用 `solveSpectralBand(band, thermal, sky, scale, accelerated, warm, false, beam)`：phase 0 生成每侧光学参数与发射 → 设初值 → 迭代 → `recordIncident` → phase 2 把辐射度整理为输出并拆出（受光,阴影）两态（facetrt_vulkan.cpp:3326-3387）。
3. **热启动**：仅当 `radiationSolver = accelerated`、非首波段且与上一波段同为光学或同为热红外时，用上一波段的物理辐射度作初值（`warm = acceleratedSolver && band>0 && previousThermal==thermal`，core.cpp:3158）；否则清零（facetrt_vulkan.cpp:3344-3351）。`facetrt.json` 的 `timing.bandSolves[].warmStart` 记录是否热启动。
4. 光学波段输出表观反射率：除以参考辐照 = 波段法向直射×cosθs + 天空漫射；热红外波段输出光谱辐亮度 W m⁻² sr⁻¹ µm⁻¹，scale 取 1（core.cpp:3162-3168）。热红外天空项 = Planck(`skyTemperature`) + 2.5–6 µm 的反射太阳天空部分（core.cpp:3169-3171）。
5. 第 0 个光学波段额外做一次“仅漫射”求解，用于 `lightEnhancement`（直射增强量）（core.cpp:3231-3235）。
6. 光照：JSON 工程取 `light.direct`（`directScatteringRatio`，缺省 0.8）与 `Esun_.dat/Esky_.dat` 的光谱形状，水平总量取 700·cosθs+50 W m⁻²；太阳高度过低或超过 1400 W m⁻² 上限而丢弃的直射并入天空漫射（core.cpp:4802-4846；docs/facetrt-gpu-spectral.md:40-48）。OBJ 或缺少光照设置时保持 700/50 W m⁻²。

环境变量：`HISTREAM_FACET_SPECTRAL_BACKEND=cpu` 让光学组装在 CPU 完成、辐射仍在 GPU；`HISTREAM_FACET_VALIDATE_SPECTRAL=1` 逐波段对照并把结果写入 `spectralVerification`（core.cpp:3149,3323-3326）。

#### 4.2.6 输出 `facetrt.json`

由 `writeWebResult` 写出（core.cpp:3563-3727）。含非有限值的数组直接抛错（core.cpp:3681,3695）。

| 字段 | 含义 |
|---|---|
| `backend` | `Vulkan GPU` 或 `CPU raster` |
| `quantity`/`units` | `reflectance`/`1`、`spectral radiance`/`W m-2 sr-1 um-1` 或混合 |
| `execution`、`quality`、`spectralVerification` | 各阶段实际后端、零采样侧占比、光谱对照状态（未做为 `not_run`） |
| `facetCount`、`leafFacetCount` | 面元总数；前 `leafFacetCount` 个为对象面元，其后为地面 |
| `vertexPositions` | 每面元 3 顶点 xyz（m），二进制时形状 [n,3,3] |
| `graph` | `directedEdges`、`closureError`、`fragmentOverflow`、`hashOverflow`、`lowerBoundary`（下边界逃逸比例等） |
| `timing` | 分阶段毫秒、`bandSolves[]`（含 `warmStart`、`iterations`）、`outputWriteMs`、`wallMs` |
| `iterations`、`maxDelta` | 第 0 波段的迭代数与最后一次扫描最大变化量 |
| `wavelengths` | 波段列表 |
| `sunlit` | [2n] 各侧受光比例 |
| `radiosity` | [2n] 第 0 波段辐射度（光学波段为表观反射率） |
| `lightEnhancement` | [2n] 直射增强量 |
| `bandRadiosity`（多波段） | [bands,2n] |
| `bandStates` | [bands,2n,2]：每波段每侧（受光,阴影）两态 |

`format=binary` 时长度 > 1024 的 float 数组写入同主名 `.bin`（小端 float32、16 字节对齐），JSON 中改为 `{file,offset,length,dtype,shape}` 描述符，并带 `format`、`formatVersion=1`、`binaryFile`、`binaryBytes`（core.cpp:3454-3511；docs/facet-binary-output.md:20-48）。`timing.outputWriteMs/wallMs` 与 `binaryBytes` 在文件写完后原位回填（core.cpp:3532-3555,3719-3724）。

### 4.3 FacetEB 运行流程

#### 4.3.1 入口、控制参数与运行索引

入口：`runFacetEBCore`（core.cpp:5890-9387）。它在一个函数内完成：读参数 → 建共享可见性图 → 选择能量后端 → 初始化状态 → 逐节点循环 → 写总结果。

- 节点区间：`startNode = max(0, round(startTimeNode))`；`endNode = max(startNode+1, round(endTimeNode))`；节点步长 `dTime = max(1, dTime)`（缺省 1800 s），若给出 `nodeTimeStepsSeconds`，节点 k 的步长取其第 k 个值（core.cpp:5942-5958,6849）。
- 过程输出开关：`isRadiationProcess`、`isEnergyProcess`；只给旧开关 `isProcess` 时按能量过程处理（core.cpp:5959-5966）。
- 运行索引 `faceteb_run.json`：启动即以 `status=running` 写出，含 `requested`、`convergence`（判据说明）、`execution`、`quality`、`initialSoilTemperature` 等；每个节点完成后追加 `completed[]` 并重写；结束时 `status` 置 `completed` 或 `non_converged`，写 `finishedAt`、`timing`、`solverDiagnostics`；若运行中途异常，析构函数把仍为 `running` 的状态改写为 `failed`（core.cpp:5979-6000,6309-6325,9142-9155,9335-9366；`FacetRunIndex` 析构，core.cpp:166-172）。**“是否完成”以 `completed[]` 为准，目录里可能残留上次中断的节点文件**（core.cpp:153-155）。

#### 4.3.2 气象驱动

- 文件（`meteo` 的气象文件路径，XML 标签 `filePath`）：**首行为表头（读取后丢弃，内容任意）**，之后每行 7 列：儒略时、气温（°C）、水汽压、气压、风速、短波下行、长波下行；可选第 8、9 列为水平面实测直射/散射（W m⁻²，非负才采用）。列不足、非有限值、文件无数据行都抛异常；`endNode` 超过行数抛 “FacetEB time range exceeds meteorology rows”（core.cpp:6038-6097）。水汽压与气压单位在源码注释中标为“待核对”（core.cpp:5912-5913），从使用处看水汽压与气压按 hPa（core.cpp:7259-7261，energy_math.glsl:67-77）。
- 无气象文件：按 `dTime` 合成时间轴（儒略日从 1 起），其余量取缺省（25 °C、15、839、1 m/s、短波 0、长波 320 W m⁻²）（core.cpp:5914-5926,6087-6094）。
- 太阳位置：每节点用 NREL SPA 按年、日序、秒（四舍五入到整秒并封顶 86399）、时区、经纬度、气压（下限 100）、气温计算天顶角与方位角，海拔固定 100 m（core.cpp:6120-6155）。

#### 4.3.3 一次性准备（节点循环之前）

1. **共享可见性图**：几何不变，图只建一次，所有节点与波段共享；GPU 路径 `initialize → setGeometry → setGroundFacets(leafFacetCount) → buildVisibilityGraph(40 方向)`；`auto` 时失败回退 CPU 光栅（core.cpp:6196-6217；docs/faceteb-gpu-energy.md:15）。不做跨进程缓存。
2. **能量后端选择**（core.cpp:6273-6316）：
   - `HISTREAM_FACET_ENERGY_BACKEND=cpu` 或工程含集总热质量（`lumpedGroup` 材质，且此时 `start` 必须为 0，否则抛错）→ CPU 能量平衡（辐射仍可在 GPU）；
   - `facetBackend=auto` 且设备不支持 GPU 能量内核 → 自动改用 CPU 能量；
   - 否则要求 GPU 能量内核；缺 `shaderFloat64` 或 `energy.comp.spv` 时抛错 “GPU FacetEB energy requires shaderFloat64 and energy.comp.spv”（core.cpp:6307-6308）。
   - `HISTREAM_FACET_VALIDATE_ENERGY=1|2` 在 GPU 能量下并行跑 CPU 影子计算逐项对比（见 4.3.10）。
3. **状态数组**：温度数组长度 2×面元数，初值 NaN；再分“受光态(0)/阴影态(1)”两套 `stateTemperature`；每面元每侧的平均温度 = 受光比例加权（core.cpp:6330-6335,7417-7422）。
4. **冠层柱**：水平方向按边长 = max(场景水平尺寸/256, 1 m)（不超过尺寸）划分柱；每个面元按质心归柱，累计叶面积（植被且非光伏的对象面元）、叶宽加权、冠层顶、地面高度，得每柱 LAI、冠高、叶宽（core.cpp:6356-6403）。每个节点用 `aerodynamics()` 为每柱求叶片/土壤空气动力阻抗（含当前 1/L，core.cpp:7317-7321；energyphysics.h:325-396）。
5. **其他子模型的静态准备**：建筑墙板（材质 `wallMode=wall`，由“黑场景”一次求解得各侧天空视因子，≥0.02 的一侧为室外，core.cpp:6465-6492）、船体吃水线（core.cpp:6493-6560）、内热源（6561-6583）、车辆座舱（6584-6667）、集总热体（6411-6446）、土壤剖面/历史数组（6692-6735）。这些模块按需启用，本章只记录其在流程中的位置。
6. **结果目录**：`.facet_steps/components.bin`（每面元 1 字节组分编号：地面为 1，对象取材质 `outputComponent`）；开启过程输出时建 `process/`；`photovoltaic_summary.csv` 表头（core.cpp:6760-6781）。

#### 4.3.4 节点循环的整体结构

```text
for node in [startNode, endNode):                                  (core.cpp:6845)
  取 dTime、meteo；updateSolarPosition(meteo)                        (6849-6855)
  sunlit = computeSunlit()           // 受光比例，GPU 16 次抖动      (6859)
  if node == startNode: 初始化温度                                   (6861-6903)
  sunlitPart = 各侧受光比例（物体面元两侧取和后限幅；地面各侧单独）   (6907-6921)
  if node != startNode: 受光/阴影状态间面积守恒转移                   (6930-6994)
  保存 previousStateTemperature；滚动土壤表面温度历史                (6999-7019)
  土壤方法2：soilResponse；墙板：wallResponse；车辆舱内温度初始化     (7024-7078)
  ① 短波窗口求解 → absorbed/incident/PAR 累加量                      (7083-7245)
  ② 节点气象量（长波、气温、密度、阻抗）                              (7252-7263)
  ③ RT–EB 耦合迭代（GPU 或 CPU 路径）                               (7779-8226)
  ④ 收敛判定 + 诊断（土壤/物体热量闭合、墙板、车辆座舱、船体反馈）    (8229-8830)
  ⑤ 写诊断 JSON、光伏 CSV                                            (8843-8861)
  ⑥ 成像波段（逐波段独立求解）                                       (8866-8950)
  ⑦ 写 .facet_steps/energy_T=*.bin、process/*                        (8955-9113)
  ⑧ 登记 completed[]、保留最后节点结果、打印进度                      (9121-9202)
  ⑨ 若 HISTREAM_FACET_STEP_SYNC=1：打印 FACET_STEP 并等待 ACK        (9206-9217)
写总结果 faceteb.json；status=completed|non_converged；返回 0|2|3     (9226-9383)
```

**热启动（warm start）在 FacetEB 中的三层含义**：(a) 跨节点：节点 k 的温度、土壤/墙板剖面与历史直接继承节点 k−1 的终态，而不是重新初始化（初始化只在 `node == startNode`，core.cpp:6861-6903）；(b) 节点内：GPU `evaluateEnergy` 对长波 Jacobi 在同一节点的后续耦合迭代中沿用上次辐射度，扫描数从 64（冷启动）降为 16（热启动），且仅当自上次 `evaluateEnergy` 以来没有其他求解触动 ping-pong 缓冲（`m_solveGeneration` 未变）时才热启动，因此每个节点的首次评估总是冷启动（facetrt_vulkan.cpp:2961-2988）；(c) 短波窗口求解 `solveAccelerated` 在 FacetEB 中不使用初值（`nullptr`，core.cpp:6244）。

**初始温度**：首节点所有面侧（含两态）取首节点气温 +273.15 K，限幅 [220,340]；地面侧若 `initialSoilTemperature`（`meteo.Tsold`，XML 转换时仅在 > 0 才写出，core.cpp:4158-4159）落在 [150,400] K 则改用该值；水体材质可按 `initialTemperatureMode`（`dailyMeanAir` 取自首节点起 24 h 内气温均值，或 `fixed`）指定；浸没船体面元取水温（core.cpp:6861-6903）。运行索引 `initialSoilTemperature.source` 记录采用的来源（core.cpp:6353-6355）。

**受光/阴影状态转移**：节点之间受光比例 p 变化时，改变状态的那部分面积携带自己的热历史，按面积加权并入接收态，使 Σ p·C·T 守恒（`sunlitStateTransfer`，energyphysics.h:81-102；应用于表面温度、方法 1 历史、方法 2 剖面、墙板剖面，core.cpp:6930-6994）。环境变量 `HISTREAM_THERMAL_HISTORY` 取 `remap`（缺省）/`legacy`（旧行为，热量不守恒）/`mixed`（诊断用），其他值抛错（core.cpp:6453-6458）。

#### 4.3.5 短波窗口求解（步骤 ①）

当 `max(0, meteo.shortwave) > 0` 且光谱样本 ≥ 2 时（core.cpp:7146）：

1. 光谱：`Esun_.dat`/`Esky_.dat` 只提供形状，直射份额由 `directScatteringRatio`（缺省 0.8，限幅 [0,1]）决定（core.cpp:6021-6027；docs/faceteb-gpu-energy.md:64）。`light.diffuseFractionMode = erbs` 时逐节点按晴空指数/实测直射散射重算直射比例（`shortwave_split::split`，core.cpp:7087-7102），并打印 `SHORTWAVE_SPLIT` 行（core.cpp:7103-7108）。
2. 把 400 nm 起的 1 nm 样本按 `spectralAccelerationWidth` 分成窗口，窗口在样本 300（700 nm）处强制截断以保证 PAR 窗口纯净（core.cpp:7148-7164）；权重近 0 的窗口跳过（7170）。
3. 每个窗口：窗口法向直射 = 总短波×直射份额×法向换算系数；窗口漫射 = 总短波×(漫射份额 + 直射份额×丢弃比例)（core.cpp:7175-7178）。`makeEnergyShortwaveOptics` 取窗口平均 ρ、τ（材质 1 nm `energyReflectance` 做太阳权重平均）、乘纹理因子并限幅，背景面元用 Hapke 半球守恒反射率（core.cpp:1754-1843）。
4. 调 `solveShortwaveWindow`：GPU 时为 `solveAccelerated`（每 8 次扫描检查一次，容差 max(1e-6, 1e-5×窗口总辐照)，最多 64 次）；CPU 时固定 64 次（core.cpp:6237-6246）。随后 `incidentRadiation` 得各侧入射辐照度。仅最后一个节点才额外求“去直射”的漫射基线用于 `lightEnhancement`（core.cpp:7193-7202）。
5. 累加：吸收 = clamp(1−ρ−τ,0,1)×入射；直射分量、PAR 光量子通量（W m⁻²×波长/119.7117 → µmol m⁻² s⁻¹）、车窗透射短波分别累加（core.cpp:7203-7231）。

无短波（夜间）时上述累加量保持 0，短波阶段不做任何 GPU 求解。

#### 4.3.6 RT–EB 耦合迭代（步骤 ③，GPU 路径）

准备（每节点一次，core.cpp:7783-7910）：组装参数 P（每面元 32 float）、输入 I（每“态×侧”12 float）、温度 T（每“态×侧”1 float），调用 `beginEnergyNode(push, P, I, T)` 一次性上传。`EnergyPush`（48 B）含面元数、对象面元数、气孔方法、phase、气温（K）、空气密度（ρ = P/(287.05·T)）、气压、水汽压、`dTime`、CO₂、松弛因子、风速（core.cpp:7898-7909；facetrt_vulkan.h:218-222）。空气动力导度 = 5.8 + 4.1·√max(0.1, u)（W m⁻² K⁻¹）；天空长波 = 气象长波，≤0 时用 σ·T_sky⁴（`skyTemperature` 缺省 250 K，限幅 [150,350]）（core.cpp:7252-7263,6019-6020）。

`energy.comp` 按 `push.phase` 分三种调度（shader/faceteb/energy.comp:1-26；facetrt_vulkan.cpp:2927-3001）：

| phase | 作用 | 读 | 写 |
|---|---|---|---|
| 2 | 由当前两态温度更新长波光学参数：发射 = ε·σ·(w·T亮⁴ + (1−w)·T阴⁴)，ε = 1−ρ−τ | I[9..11]、T | `optics` |
| 0 | 用入射长波 H 计算净辐射、显热及斜率、潜热及斜率、储热/导热、光伏及斜率；叶片联立求叶温步长与气孔阻抗；统计残差 | P、I、T、H（入射） | F（通量，每“态×侧”12 float）、`stats[7]` |
| 1 | 牛顿更新温度 | F、T | T、`stats[4]` |

每个耦合轮次（core.cpp:8093-8137）：

```text
for coupling in 0 .. couplingIterations-1:
  stats = evaluateEnergy(Rli)          // phase2 → 长波 Jacobi(radiosity.comp×64冷/16热)
                                       // → incident/ground_mean → phase0
  if stabilityCorrection: 读回状态，updateStability(advance=false)   // 仅测 |Δ(z/L)|
  if energyConverged(): break
  maximumTemperatureDelta = updateEnergyTemperature()               // phase1，读 stats[4]
  if stabilityCorrection: 读回，updateStability()                    // 1/L 欠松弛 0.5，更新 I[4]、I[5]，updateEnergyInputs
  couplingIterations = coupling + 1
最终再 evaluateEnergy 一次（使报告的通量/温度/统计对应同一状态），readEnergyState(true)
```

主机与 GPU 每轮只交换 7 个归约标量（`stats`）：[0] 最大|残差|、[1] 最大|残差|/响应、[2] 最大气孔对数失配、[3] 最大叶片残差、[4] 最大温度步长、[5] 错误标志（bit0 非有限值、bit1 气孔目标无效，非 0 即抛异常）、[6] 能量不闭合面元数（facetrt_vulkan.cpp:2944-2957；energy.comp:19-26）。因此 `couplingIterations` 记录的是**已执行的温度更新次数**；若第 0 轮即收敛则为 0。

温度更新规则（energy.comp:84-98）：非叶：`T_new = T + relax×(clamp(T + clamp(R/max(1,响应),−5,5), 220, 360) − T)`；叶片：`T_new = T + F[9]`，其中 F[9] = relax×clamp(T_trial − T, −5, 5)。残差 R = 净辐射 − 显热 − 潜热 − 储热 −（物体面元的）光伏功率（+ 非叶热源）；响应 = faces·ε·σ·T³ + 显热斜率 + 潜热斜率 + 储热斜率（+ 光伏斜率），faces = 8（薄壳两面）或 4（墙板外侧/地面）。物体面元两侧共用一个温度；地面面元朝土壤的背侧不参与平衡，直接复制上侧温度（energy.comp:97）。

**CPU 对照路径**（core.cpp:8151-8226）：每轮用当前温度 `makeEnergyLongwaveOptics → solveRadiation(64 次) → incidentRadiation → evaluateEnergyFluxes → measureEnergyResidual`，满足判据则退出，否则按同样的阻尼牛顿公式更新并 `updateMeanTemperature`/`updateStability`。CPU 路径的叶片求解在 `evaluateEnergyFluxes` 内完成（core.cpp:7553-7605）。

#### 4.3.7 通量模块、土壤热方法与植被气孔

**表面能量平衡**（逐“态×侧”，受光态直射短波 = 直射量/受光比例，阴影态仅漫射，core.cpp:7406-7415）：

- 净辐射 = 吸收短波 + ε·长波入射 − ε·σ·T⁴（T 限幅 [1,1000] K）（energy.comp:100-106）；
- 显热 H = 导度×(T − 气温)。叶片导度 = ρ·cp·convectiveScale/叶阻抗 r_b（cp=1005）；薄壳 = 2×(5.8+4.1√u)×convectiveScale；水面/墙板外侧 = 1×；浸没船体用总导度与水温（energy.comp:122-131）；
- 潜热 LE = ρ·λ·0.622·max(0, e_s(T)−e)/p /(r_a + r_s)，λ = (2.501−0.002361·T_C)×10⁶，e_s 取 Tetens 式（energy_math.glsl:66-83；core.cpp:1502-1561）。水面乘 `evaporationCoefficient`；干土（体积含水量 < 0.03）LE=0；
- 地面显热阻力：`groundExchange(土壤阻抗, T−T_air, 模式)`：地表比空气暖时附加自由对流导度 g_free = 0.0025·x·(x²+1)^(−1/3) m/s（x=ΔT 限幅 [0,60]），中性模式并联叠加，MOST 模式取两者较大值（energyphysics.h:397-433；energy_math.glsl:248-263）；
- 光伏：η = η₂₅(1+γ(T−298.15))，功率限幅在 [0, 吸收短波]，入射 = 正面 + 双面系数×背面（energy.comp:133-138）。

**土壤热方法**（`soilTemperatureMethod`，0/1/2，缺省 1）：

| 方法 | 储热/导热 G 的计算 | 状态 |
|---|---|---|
| 0 | G = 0.35×净辐射（经验比例，斜率不含储热项）（core.cpp:7673-7675；energy.comp:195；GST=0.35，shader/global.glsl:28） | 无历史 |
| 1 | 半无限固体热惯量 Duhamel 卷积：G = 斜率×[(T−h₀) + Σ(h_{k−1}−h_k)(√(k+1)−√k)]，斜率 = 2√(c·ρ·λ)/√(π·dt)；保留最近 8 个节点的表面温度历史（受光/阴影各一套）；`soilInertiaHistory=full` 时再用 12 个指数记忆模态补充更早历史（core.cpp:1669-1707,6702-6722,7676-7691） | 每节点更新历史；GPU 上斜率与偏移作为 I[7]、I[8] 每节点上传一次 |
| 2 | 12 层（0、0.01、0.025、0.05、0.1、0.2、0.35、0.55、0.8、1.1、1.5、2、3 m 层界）有限体积后向欧拉，下边界在 3 m 固定为初始温度；每节点把内部消元为 T_i = offset_i + slope_i·Ts，GPU 输入 I[7]=通量斜率、I[8]=flux(0)（energyphysics.h:57-146；core.cpp:7024-7036,7878-7883） | 节点结束时 `profile(Ts)` 推进剖面；闭合残差写诊断 |

**植被气孔与光合**：`vegetationTemperatureMethod=0` 用 Collatz C3/C4 光合 + Ball–Berry 气孔（`collatz`，energy_math.glsl:103-156），`=1` 用 Farquhar/von Caemmerer 光合 + Ball–Berry（`farquhar`，energy_math.glsl:173-214）。叶温与气孔阻抗联立：对 ln r_s ∈ [0, ln 625000] 二分，最多 24 次，每个试探 r_s 先由 `thermal` 求叶温（牛顿 + 括号 [220,360] K，≤32 步，修正量 ≤ 0.01 K 即收敛），再由 `physiology` 求目标 r_s，|ln r_s − ln r_target| ≤ 0.01 即停；24 次用完取区间中点（energy.comp:148-173；energyphysics.h:462-523）。目标无效（NaN 或 ≤0）置错误标志，运行报错。CPU 与 GPU 路径对未收敛的叶温/气孔求根次数分别计数，结束时写入 `solverDiagnostics` 与 stdout `FACET_SOLVER_DIAG`（core.cpp:9353-9365）。

#### 4.3.8 稳定度修正（MOST）

`stabilityCorrection=true` 时（core.cpp:7720-7742）：每个冠层柱的 1/L = −κ·g·H/(ρ·cp·T·u*³)，并把 (z−d)/L 限幅到 [−5,1]（energyphysics.h:438-442）；每次更新按 `1/L ← 1/L + 0.5·(desired − 1/L)` 欠松弛，随后重算该柱叶/土空气动力阻抗，GPU 路径再把新阻抗写回 I[4]、I[5]（core.cpp:7736-7740,8122-8135）。地面模式 P[28] 取 2（MOST，取较大导度）否则 1（core.cpp:7832）。每次更新需读回 GPU 状态做柱汇总，CPU 参与，docs/faceteb-gpu-energy.md:9 已说明。**稳定度变化量只作诊断（`maximumStabilityChange`），不进入 `energyConverged()`**（core.cpp:7374-7379,8237）。

#### 4.3.9 收敛判据、退出码与结果保留

节点收敛（`energyConverged`，core.cpp:7374-7379；energyphysics.h:30-51）同时满足：

1. 能量不闭合面元数 × 1000 < 面元总数（即比例 < 0.1%），“不闭合”指任一态、任一参与平衡的侧满足 |R| ≥ 2 W m⁻² 或 R 非有限（物体面元只检查第 0 侧，地面检查两侧）；
2. 所有侧的 |R|/max(1,响应) ≤ 0.5 K（`facetEnergyMaxEquivalentK`）；
3. 集总热体表面残差 < 2 W m⁻²（GPU 路径下该量恒为 0，因含集总体时强制走 CPU 能量路径）。

`temperatureTolerance` 不参与（core.cpp:5984-5986，docs/faceteb-gpu-energy.md:55）。耦合预算耗尽仍不满足判据的节点标为未收敛：`allNodesConverged &= converged`，**循环继续处理后续节点，不中止**（core.cpp:8229-8231）。

| 返回值 | 条件 | 结果文件 |
|---|---|---|
| 0 | 全部节点收敛 | 全部写出，`status=completed` |
| 2 | 完成全部节点但至少一个节点未收敛 | **全部写出并保留**，`status=non_converged`，stdout 提示“存在未收敛节点，结果已保留”，`converged=false`（core.cpp:9350,9379,9383） |
| 3 | `HISTREAM_FACET_VALIDATE_ENERGY=2`（普查模式）出现 CPU/GPU 不一致 | 同上，`energyVerification` 列出各类别/字段的不一致 |
| 1 | 任何异常（输入非法、GPU 失败、能量状态非有限、写盘失败、握手中断等） | 已写的节点文件保留，`faceteb.json` 视情况不完整；`faceteb_run.json` 状态改为 `failed` |

退出码 3 的优先级最高（`!shadowMismatches.empty() ? 3 : allNodesConverged ? 0 : 2`，core.cpp:9383）。严格模式 `VALIDATE_ENERGY=1` 在首个不一致处抛异常，因此以退出码 1 结束（core.cpp:7981-7985）。`Command::runEB` 的布尔返回值只表示 `exitCode==0`，真正的退出码由 `FacetebIO::exitCode` 经 `Engine::run` 返回（faceteb/command.cpp:34-42；engine.cpp:293-298）。服务端据此只把 FacetEB 的退出码 2 映射为 `non_converged`（server.mjs:119-121,5265-5266）。

#### 4.3.10 步进握手 `HISTREAM_FACET_STEP_SYNC`

引擎侧实现（core.cpp:6782-6812,9204-9217）：

- 仅当环境变量恰为字符串 `"1"` 时启用（其他值或未设置均不启用）。启用时在节点循环之前先写一个只含几何的 `faceteb.json`（`backend`、`componentFile`、`facetCount`、`leafFacetCount`、`vertexPositions`、`wavelengths`、`stepRecordFloats = 11 + 2×波段数`），供第一个节点的预览读取；最终总结果写出时覆盖它（core.cpp:6785-6811）。
- 每个节点的所有输出（`.facet_steps/energy_T=<token>.bin`、`process/*`）写完、该节点的局部数组释放之后，向 stdout 打印一行 `FACET_STEP\t<节点>\t<时间标签>\t<儒略时>\t<天顶角>\t<方位角>`，然后**阻塞**于 `std::getline(std::cin, …)`，必须读到恰为 `FACET_ACK\t<节点号>` 的一行才继续；读不到（EOF/出错）或内容不符则抛异常 “FacetEB time-step output was interrupted or failed”，退出码 1（core.cpp:9206-9217）。
- 目的是背压：避免下一节点的 GPU 求解与父进程对当前节点的 CPU 影像缓冲重叠（core.cpp:9204-9205 的注释）。
- 引擎直接运行（无父进程管道）时必须不设置该变量或设为 `0`；回归与统计脚本显式设 `HISTREAM_FACET_STEP_SYNC=0`（tools/regression/run.mjs:74；statistic/run_scene_examples.mjs:83-85）。服务端 `server.mjs` 在需要逐节点出图时设为 `1` 并在处理完节点后向 stdin 写入 `FACET_ACK\t<节点>\n`，等待期间暂停运行超时（server.mjs:5072-5077,5133-5169；服务端细节见其他章节）。

#### 4.3.11 成像波段（步骤 ⑥）

能量平衡使用宽带短波/长波，传感器产品是独立计算：对 `controlBand` 的每个波段分别求解，不是相邻波段插值（core.cpp:8866-8880）。光学波段的直射/漫射 = 总短波×光谱表在该波长的值×0.001（换算因子含义源码标“待核对”，core.cpp:8883-8891），并计入丢弃的直射份额；热红外波段 < 6 µm 时叠加 2.5–6 µm 反射太阳项（`mwirSolarShape`，core.cpp:219-230,8899-8909）。每波段调 `makeStateSpectralOptics` 生成含两态发射的光学量，再用 `solveShortwaveWindow` 求解，经 `appendSurfaceStates` 得每侧（受光,阴影）两个值（core.cpp:8914-8949）。热红外图像的天空辐射使用固定 `skyTemperature` 的 Planck 辐射而不是气象长波，源码对此有“DEFERRED[低]”标注（core.cpp:8923）。

#### 4.3.12 输出文件与字段

**(1) 节点二进制** `.facet_steps/energy_T=<token>.bin`（token = `DOY<日序>_<HH>-<MM>`，core.cpp:6101-6114）：小端 float32，按面侧交错，每侧记录 `11 + 2×波段数` 个 float（`stepRecordFloats`）：

| 偏移 | 含义 |
|---|---|
| 0 | 平均温度 K |
| 1 | 总辐射度（短波+长波） |
| 2 | 短波辐射度 |
| 3 | 长波辐射度 |
| 4 | 净辐射 W m⁻² |
| 5 | 显热 |
| 6 | 潜热 |
| 7 | 储热/土壤热通量 |
| 8 | 受光比例 |
| 9 | 受光点温度 K |
| 10 | 阴影点温度 K |
| 11+2b, 12+2b | 第 b 个成像波段的（受光点,阴影点）值 |

（core.cpp:8952-8982）。`components.bin` 与几何顺序一致（core.cpp:6763-6771）。

**(2) 过程结果**（`process/`，由 `isRadiationProcess`/`isEnergyProcess` 控制，光伏面元存在时总是写）：`<model>_T=<token>.bin/.json`，每侧小端 float32（core.cpp:8988-9113）。过程结果 kind 与记录：

| `processType` | `kind` | 文件主名 | 每侧 float 数 | 字段（offset） |
|---|---|---|---|---|
| `radiation` | `facet-radiation-process` | `facetrt_T=` | 3 | 短波辐射度(0)、长波辐射度(1)、净辐射(2) |
| `energy` | `facet-energy-process` | `faceteb_T=` | 7 | 潜热(0)、显热(1)、表面热通量(2)、温度(3)、净辐射(4)、受光点温度(5)、阴影点温度(6) |
| `photovoltaic` | `facet-photovoltaic-process` | `faceteb_pv_T=` | 3 | 光伏功率(0)、温度(1)、单面吸收短波(2) |
| `par`（随 radiation 一起） | `facet-par-process` | `facetpar_T=` | 2 | 入射 PAR 光子通量(0)、吸收 PAR 光子通量(1)（µmol m⁻² s⁻¹，400–700 nm，两侧求和为整叶） |

JSON 元数据含 `node`、`julianTime`、`time`、`solarZenith/Azimuth`、`facetCount`、`surfaceCount`、`geometryFile`、`componentFile`、`dataFile`、`dataType=float32-little-endian`、`layout=surface-interleaved`、`recordFloats`、`couplingIterations`、`temperatureDelta`、`converged`、`energyResidual`（W m⁻²）、`energyResidualTemperatureEquivalent`、`temperatureTolerance`、`iterationBudget`、`fields`（core.cpp:9027-9071）。

**(3) 诊断**：`diagnostics/facet_physics_node=<n>.json`（收敛状态、不闭合面元数与比例、稳定度变化、气孔对数失配、土壤剖面与热量闭合、`thermalHistory` 闭合、集总体、`lowerBoundary`）；车辆座舱 `vehicle_cabin_node=<n>.json`；`photovoltaic_node_<n>.csv` 与 `photovoltaic_summary.csv`（core.cpp:8227-8330,8843-8861）。

**(4) 总结果 `faceteb.json`**（`binaryOutput` 时同 4.2.6 的 `.bin` 机制，core.cpp:9226-9315）：`backend`、`componentFile`、`facetCount`、`leafFacetCount`、`time`（最后节点标签）、`vertexPositions`、`graph`、`iterations`/`maxDelta`（最后节点的辐射求解）、`couplingIterations`、`temperatureDelta`（均为**最后一个节点**的值）、`converged`（**所有节点**的与）、`energyResidual`、`energyResidualUnits`、`energyResidualTemperatureEquivalent`、`temperatureTolerance`、`iterationBudget`、`wavelengths`、`stepRecordFloats`，以及 [2n] 数组 `sunlit`、`radiosity`（总辐射度）、`lightEnhancement`、`temperature`、`temperatureSunlit`、`temperatureShaded`、`netRadiation`、`sensibleHeat`、`latentHeat`、`storageHeat`、`photovoltaicPower`（均取最后一个节点；与 FacetRT 不同，FacetEB 写数组时不检查非有限值，核对 core.cpp:9288 的 DEFERRED 说明）。`faceteb_run.json` 的 `products` 列出所有产物，二进制模式追加 `faceteb.bin`。

**(5) 运行索引 `faceteb_run.json`**：`kind=faceteb-run`、`schemaVersion=1`、`status`（`running`/`completed`/`non_converged`/`failed`）、`startedAt/finishedAt`、`requested`、`convergence`（含 `iterationBudget`、`energyResidualThreshold_W_m2=2`、`maximumFailingFacetFractionExclusive=0.001`、`maximumTemperatureEquivalentResidual_K=0.5`、`temperatureToleranceRole` 等）、`execution`（各阶段实际后端与 `iterativeStateDeviceResident`）、`quality`、`completed[]`（每节点：`node`、`token`、`converged`、`iterations`、`energyFailingFacets`、`energyFailingFacetFraction`、`temperatureDelta`、`energyResidual`、`energyResidualTemperatureEquivalent`）、`products`、`timing`（`wallMs`、`setupMs`、`gpuInitMs`、`visibilityMs`、`sunlightMs`、`radiationMs`、逐节点三段耗时等）（core.cpp:5979-6000,9142-9155,9335-9366）。

stdout 协议行：`PROGRESS`（10%→96% 随节点线性推进，最后 100）、`SHORTWAVE_SPLIT`、`FACET_CONVERGENCE\t节点\t收敛(0/1)\t耦合次数\t最大温差\t最大残差\t温度等效残差`、`FACET_TIMING\t节点\t短波s\t耦合s\t输出s`、`FACET_STEP`、`FACET_SOLVER_DIAG`、`RESULT`（core.cpp:9176-9201,9363-9380）。

#### 4.3.13 CPU/GPU 影子审计

`HISTREAM_FACET_VALIDATE_ENERGY=1|2`（需 GPU 能量）：每次 GPU `evaluateEnergy` 之后读回状态，用 CPU 重算入射（容差 1e-4 W m⁻²）、通量（每项 ≤ 0.01 W m⁻²，叶片气孔阻抗先自由求解只作 ln r_s ≤ 0.1 的粗界检查，再把 GPU 的 r_s 强加给 CPU 严格比较，气孔失配容差 5×10⁻⁴）与温度更新（≤ 0.0002 K，叶片 ≤ 0.001 K）；永不用 CPU 状态替换 GPU 状态（core.cpp:7994-8087,8105-8121）。该模式耗时不可作性能结果（docs/faceteb-gpu-energy.md:25）。

#### 4.3.14 FacetEB 资源上限与失败条件（汇总）

| 条件 | 行为 |
|---|---|
| 缺工程路径 | 抛 “FacetEB requires project.json”，退出码 1（core.cpp:5894-5896） |
| 气象行非法/非有限/不足、`endNode` 超行数 | 抛异常，退出码 1（core.cpp:6059-6096） |
| `nodeTimeStepsSeconds` 个数 ≠ `endNode` 或值越界 | 抛异常（core.cpp:5954-5957） |
| 含集总热体且 `start ≠ 0` | 抛 “Lumped thermal runs must restore history from node zero”（core.cpp:6279） |
| 无 `shaderFloat64`/`energy.comp.spv`（后端 gpu） | 抛错；`auto` 则回退 CPU 能量 |
| GPU 能量出现非有限状态或无效生理根 | 抛错，退出码 1（facetrt_vulkan.cpp:2951-2952） |
| 能量残差非有限 | 抛 “FacetEB energy residual is not finite”（core.cpp:7766） |
| 太阳位置计算失败 | 抛错（core.cpp:6150-6152） |
| 可见性/A-buffer/哈希溢出且无法扩容 | 见 4.2.4，退出码 1 |
| 写盘失败 | 抛错（如 core.cpp:8959,9119,9230,9320） |

### 4.4 VoxelEB 运行流程

#### 4.4.1 加载与初始化

`Engine::input` 经 `FileIO::readJson` 把工程转为 VoxelEB 内部配置（fileio_json.cpp:871-919），随后 `Voxeleb::setup`（取 Vulkan 设备/队列、分配器、SBT 与加速结构构建器，voxeleb.cpp:100-144）与 `upload`（顺序：defined 参数 → 成分属性 → 设置 → 场景 → 几何 → 气象 → 气溶胶，voxeleb.cpp:151-170）。`create` 依次建缓冲、描述符、管线、命令对象并同步推送常量（voxeleb.cpp:409-420）。

**气象文件格式（与 FacetEB 不同）**：首行为一个整数“节点数”，其后每行 7 列 `t Ta ea p u Rin Rli`（儒略日、气温 °C、水汽压、气压、风速、水平面入射短波、天空长波），可选第 8、9 列为水平面实测直射/散射；p>0，ea、u、Rin、Rli ≥0 否则抛错；实际行数少于首行声明时抛错，多于声明时全部使用（fileio.cpp:2006-2098）。`meteo.dTime` 为全程固定步长（秒，必须 > 0，缺省 1800）（fileio_json.cpp:884-886）；VoxelEB 没有 `timeStepSeconds` 逐节点变步长。

**初始温度**：所有体元取 `start` 节点的气温 + 273.15 K（气温有限且在 (−100,100) °C 内，否则 298.15 K）；土壤体元若 `meteo.Tsold` 落在 [150,400] K 则取该值；参与介质（冠层结构类型 2/3，火/雾）且 `fixedTemperature>0` 的体元固定为该温度（voxeleb/buffer.cpp:126-205；voxeleb.cpp:262-266）。`Tsold` 缺省为 **300 K**（fileio_json.cpp:882），因此 VoxelEB 在工程未给 `Tsold` 时土壤从 300 K 起算，而 FacetEB 此时取首节点气温——两者初值约定不同（`Tsold` 为 null 时的处理待核实）。温度历史 TLAST（8 层，°C）从表面温度向深层温度线性过渡初始化（buffer.cpp:872-917）。

**体元数规模检查**（`buffer.cpp:68-83`，`voxellimits.h`）：每个逐体元缓冲（`VoxelLink` 48 B、`VoxelDir`、`VoxelTempe` 12 B、`VoxelNetRad`、`VoxelPnet`、`VoxelRaa`、`VoxelRss`、`VoxelAir` 36 B、`VoxelHeatflux` 40 B、`TLAST` 8×8 B）必须不超过 `maxStorageBufferRange`；1D 派发组数 ceil(N/64) 必须不超过 `maxComputeWorkGroupCount[0]`；超限抛出带体元数与上限的明确错误，由 `create` 阶段上报为退出码 3。运行时另外要求 N×64 ≤ 2³²−1（约 6710 万体元），否则 `runEB` 抛错（command.cpp:565-569）。docs/voxel-adaptive-and-direction.md:135-136 给出的 VoxelEB 约 6700 万体元上限与此一致；代理面 BLAS 另受 `maxMemoryAllocationSize` 约束，约 4000 万个面（docs/voxel-proxy-geometry.md:44）。

**阶段枚举与着色器**（`VoxelEBStage`，structs.h:84-101；`pipeline.cpp:88-105`）：

| 阶段 | 着色器（`shader/voxeleb/`） | 工作量 |
|---|---|---|
| `gap` | `voxelrad_solar.comp` | 每线程一体元：沿太阳方向的透过率 → `voxelDirs.solar` |
| `directVNIR` | `voxelrad_direct_VNIR.comp` | 每体元；逐 1 nm 波段累加直射短波吸收 → `voxelNetRads.directVrad`、`voxelPnets.directPnet` |
| `scatteringTrace` / `scatteringVNIR` | `voxelrad_scattering_trace.comp` / `voxelrad_scattering_VNIR.comp`（同一源码，前者以 `-DSCATTER_TRACE_STAGE` 另行编译，pipeline.cpp:84-86） | 多次散射，按 `setting.scatteringStage` 分派 |
| `directTIR` | `voxelrad_direct_TIR.comp` | 当前着色器只清零 `directTrad` 后返回，天空长波由 `diffuseTIR` 一并计入（command.cpp:637-639） |
| `diffuseTIR` | `voxelrad_diffuse_TIR_single.comp` | 每体元沿 64 个方向追踪，得 `diffuseTrad` |
| `bio` | `biochemical.comp`（方法 0）/ `biochemical_m12.comp`（方法 1） | 阳/阴叶光合与气孔阻抗 `rss`、`ci` |
| `aero` | `aeresist.comp`（`stabilityCorrection` 时 `aeresist_most`） | 空气动力阻抗 `raa` |
| `evapo` | `evapo.comp`（或 `evapo_most`） | H、LE、G、叶面 `es`/`cs` |
| `budget` | `budget.comp`（或 `budget_most`） | 阻尼牛顿更新温度，原子累计未收敛状态数 |
| `updateTp` | `updateTp.comp` | 节点末提交温度历史 |
| `fluidLbm`/`fluidCommit` | `fluid_lbm.comp`/`fluid_lbm_commit.comp` | LBM 流体步进（可选） |
| `out` | `voxelrad_image.comp` | 每线程一个像素的传感器成像 |
| `updateL` | `updateL.comp` | 已被管线创建但 `runEB` 不调用（旧版 Obukhov 长度，仅创建管线） |

每次 `submit` 前插入全局内存屏障、绑定管线与描述符集、以 `setting`（`VoxelLstSetting`，128 B）作 push constant、派发后以 fence 同步等待（command.cpp:992-1049,1120-1137）。一维派发组数 = ceil(n_voxel/64)（`GROUP_SIZEX=64`，shader/global.glsl:81）。

#### 4.4.2 节点循环（`Voxeleb::run`，voxeleb.cpp:429-616）

```text
(HISTREAM_OBSERVER_MANIFEST 存在 → runObserver)                  (430)
重写 result_statistics.txt 表头                                   (469-478)
for knode in [startTimeNode, endTimeNode):                         (490)
  updateMeteo(knode)                  // 上传 Meteo；越界抛 out_of_range      (494；278-285)
  updateAngle(0)                      // SPA 重算太阳角                     (497-499)
  applyShortwaveSplit(knode)          // fixed|erbs，打印 SHORTWAVE_SPLIT   (500)
  if fluid.enabled: runFluid(fluidSubsteps)   // 显式交错耦合：先推进风场     (505-507)
  if !runEB(): return false           // 能量平衡                           (510)
  (可选 TEMPERATURE_RANGE 诊断)                                              (514-550)
  if fluid.enabled: outputFluidSlices                                         (552-554)
  PROGRESS 10 + 85×已完成/总数                                               (557-562)
  outputVoxel()                        // 回读并校验全部体元状态，写诊断/过程  (564)
  for kangle in 观测角（UAV 巡航每节点 1 次）:
     updateAngle/updateSensorPos → runRT(out 阶段) → output(TIFF) → outputTxt
PROGRESS 100                                                               (614)
```

VoxelEB 的“RT–EB 耦合”在体元内部由 `runEB` 的迭代完成，成像（`runRT`）只在该节点能量平衡完成后读取其温度/日照/散射场，不反馈能量平衡（command.cpp:839-860）。温度状态在节点之间直接继承（`voxelTempes` 不重置，只清零 `EBState`），这就是 VoxelEB 的跨节点热启动；`clearEnergyState` 只清 `EBState`（command.cpp:134-142）。

#### 4.4.3 单节点能量平衡 `Command::runEB`（command.cpp:506-834）

| 步骤 | 调用 | 说明 |
|---|---|---|
| 1 判定无短波节点 | — | `Rin ≤ 1e-3 W m⁻²` 且太阳天顶角 ≥ 90°（无角度按 180°）→ 跳过 gap/直射/散射三类 pass，改为 `clearShortwaveFields` 清零 `VoxelDir/VoxelNetRad/Pnet/VoxelRad`，打印 `SHORTWAVE_SKIPPED`；数值上与追踪后得到全 0 等价（command.cpp:524-551） |
| 2 太阳可见度 | `gap` | 先 `resetSolarStats`，派发后 `readSolarStats` 取射线统计，换算束辐照度 = Rin×直射比例/cosθs（command.cpp:553-558,341-348）；太阳高度过低（sin<0.01）solar=0 |
| 3 直射 | `directVNIR` | 水平面辐照 Rin×direct 换算到吸收元件（叶片 G/cosθs，固体 cos入射/cosθs），**逐个 1 nm 波段累加**（共 2001 个，不分窗口），400–700 nm 同时累加 PAR；太阳高度 ≤ 0.01 时直接返回，其直射份额并入散射阶段的各向同性天空源（shader/voxeleb/voxelrad_direct_VNIR.comp:98-135） |
| 4 多次散射 | `scatteringTrace`、`scatteringVNIR` | 见下 |
| 5 直射 TIR | `directTIR` | 基本为空操作 |
| 6 气动阻抗 | `aero`（+ 稳定度读回） | 每节点至少一次；MOST 时从 1/L=0 起迭代 |
| 7 迭代 ≤50 次 | `diffuseTIR → bio（前 25 次）→ evapo → clearEnergyState → budget` | 见 4.4.4 |
| 8 收尾 | 再跑 `diffuseTIR`、`bio`（若迭代 < 25 次）、`evapo`，以 `energyFinalize=1` 跑一次 `budget`（只写 G 并统计残差，不改温度） | 读回计数 → `energyFinalUnconvergedStateCount` |
| 9 提交历史 | `updateTp` | 土壤/水体/船体钢板的温度历史前移 |

**多次散射**（command.cpp:564-635）：先 `scatteringStage=0` 清零漫射吸收累加量；然后按光谱分组循环 VNIR 下标 [0,2001)，每组宽度 `n_jump`（= `spectralAccelerationWidth`，缺省 100，≥1），在下标 301 处强制断开使 PAR 落在同一积分域；每组：`stage 1`（首组）追踪未碰撞天空场并缓存天空间隙透过率，其后各组用 `stage 4` 复用缓存只按本组天空辐照缩放；然后对 order=1…`shortwaveScatteringOrders`（1–3）依次 `stage 2`（由上一阶入射场与太阳一次散射构造散射源）与 `stage 3`（追踪本阶入射场并累计吸收到 `diffuseVrad/diffusePnet`）；每组读回 128 B 统计累加到 CPU 的 uint64/double。方向场固定 64 个方向（`DIFFUSENUM=64`，structs.h:64）。结束后写 `diagnostics/scattering_node=<k>.json` 与 stdout `SHORTWAVE_SCATTERING`；统计非有限或含无效值抛错（command.cpp:385-464）。光线步数上限为 `radiationMaxSteps`（缺省 64）；太阳可见度射线沿体元网格直到离开场景（stepLimit 见诊断文件）。

**与理论手册 §8 的差异提示**：理论手册 §8.2 称“VoxelEB 的直射与漫射短波计算均服从相同窗口宽度”；按当前代码，窗口宽度只作用于多次散射分组，`directVNIR` 是逐 1 nm 全波段累加（直射项无射线追踪，代价低）。

#### 4.4.4 迭代、收敛与非收敛行为

迭代主循环（command.cpp:724-802）：

```text
for kiter in 0..49:
  setting.energyIteration = kiter
  diffuseTIR                      // 用当前温度重算长波入射
  if kiter < 25: bio              // 之后冻结生理，仅迭代温度
  evapo                           // H、LE、G（method 0 的 G 留给 budget）
  clearEnergyState; budget        // 阻尼牛顿 + 统计 EBState.count
  energyIterations = kiter + 1
  (船体吃水线：读 waterCount/waterTemperature，更新场景水温)
  if stability && stabilityAvailable:
      outside = EBState.count；sensible = 0.1×Σ sensible[bin] / (nx×nz)
      desired = desiredInverseObukhov(sensible, u*, Ta, z−d)
      变化量 = |desired − 1/L|×(z−d)；收敛 ⇔ 变化量 ≤ 0.02 + 0.02×|desired×(z−d)|
      if kiter ≥ 2 && outside==0 && 收敛: break
      if !收敛: 1/L += 0.5×(desired − 1/L); aero 重算
  elif kiter ≥ 2 && EBState.count == 0: break
```

- **收敛判据**：至少 3 次迭代（kiter≥2），且所有体元的“受光/阴影条件状态”残差均 ≤ `RAD_THRESHOLD = 2 W m⁻²`（残差为 NaN/Inf 也算未收敛）；MOST 开启且可用时还要求 1/L 收敛（command.cpp:786,794；shader/global.glsl:172；budget.comp:427-430）。`EBState.count` 统计的是状态数（每体元最多 2 个），不是体元数。
- **阻尼**（`budget.comp:263-266`）：植被 wc = 0.5（前 8 次）→ 0.3（第 9–20 次）→ 0.2；非植被 0.9 → 0.5 → 0.25。这与理论手册 §7.5 一致；VoxelEB 不读取工程的 `temperatureRelaxation`。温度限幅 [220,373] K（`TMIN/TMAX_THRESHOLD`，global.glsl:173-174）；上一轮温度为 NaN 或越界时回退为气温 +2 K（阳）/+1 K（阴）（budget.comp:127-136）。
- **水体**：受光/阴影两态按 `solar` 混合为单一水温，残差含混合层储热 ρc·h/dt（budget.comp:378-403）。
- **参与介质**（火/雾）不做能量平衡：温度固定为 `fixedTemperature`（>0）否则取气温，通量置 0（budget.comp:94-106）。
- **非收敛的处理**：50 次后无论是否满足判据都继续收尾（步骤 8）并 `updateTp` 提交历史，`runEB` 固定返回 true；未收敛信息只写入 `energyFinalUnconvergedStateCount`（最终状态残差 > 2 W m⁻² 的状态数）、`diagnostics/energy_balance_node=*.json` 的 `budgetStatesOutsideTolerance` 与 `budgetResidualWithinTolerance`、`process/*.json` 的 `energyState`、以及 stdout `ENERGY_BALANCE ... statesOutsideTolerance=N`（voxeleb.cpp:1156-1186,1210-1226）。**VoxelEB 没有退出码 2**：未收敛时进程仍以 0 退出，使用者必须检查上述诊断字段（源码 command.cpp:723 的 DEFERRED 注释也指出这一点）。
- 与 FacetEB 的差别：VoxelEB 的迭代是“逐体元独立牛顿 + 长波场重追踪”，没有单独的 `temperatureTolerance`/`couplingIterations` 参数，上限固定 50（生理固定前 25 次）。

**土壤热方法**（`soilSets.method`，0/1/2，缺省 1，`control.soilTemperatureMethod` 覆盖，fileio_json.cpp:779）：方法 0：G = 0.35×Rn，由 `budget.comp` 在能量平衡内写回（`evapo.comp` 对其输出 0），牛顿分母的辐射项乘 (1−0.35)；方法 1：热惯量 Γ = √(c·ρ·λ)，G = 2Γ/√π·ΣΔT_k/Δt·(√t_k − √(t_k−Δt))，历史为 `TLASTNUM = 8` 个节点，受光比例相对存储变化时用 `sunlitStateTransfer` 先做阳/阴转移，`inertiaHistory=1` 时再加 12 个指数记忆模态，斜率 2Γ/√(π·dt)；方法 2：多层 `Soilheatflux`（深度 0、0.02、0.04、0.10、0.20、0.40、0.60、1.00 m 共 8 层，下边界取材质 `Tsoil` 在 1 m 处），斜率由 `soilHeatFluxSurfaceDerivative` 给出（evapo.comp:215-348；budget.comp:305-342；voxeleb.cpp:1409-1417；`updateTp.comp` 文件头）。水体 Qw = 体积热容×max(混合层深度,0.01)×ΔT/Δt（evapo.comp:218-228）。船体钢板（建筑且 `plateThickness>0`）G 由隐式板模型给出。建筑与干土（含水量 < 0.03）LE=0，水体 LE 乘蒸发系数 ∈[0,1]。

**与 FacetEB 对照**：FacetEB 的方法 2 是 12 层（至 3 m）、下边界固定为初始温度；VoxelEB 的方法 2 是 8 层（至 1 m）、下边界取材质 `Tsoil`。

**植被**：`vegetationTemperatureMethod`（0/1）选择 `biochemical.comp`（Farquhar–Collatz C3/C4 + Ball–Berry，对应 SCOPE biochemical.m，输入温度 < 100 按 °C 处理）或 `biochemical_m12.comp`（Farquhar/von Caemmerer + Ball–Berry）；阳叶按叶倾角分 `SUNLIT_CLASSES` 类各做叶片温度牛顿步再合成，阴叶单状态；`setting.energyIteration==0` 时用环境空气值而非上轮叶面值（pipeline.cpp:99-102；biochemical.comp 文件头）。气孔阻抗上限 625000 s m⁻¹；能量迭代中对新旧阻抗做几何平均阻尼（biochemical.comp 文件头）。

#### 4.4.5 稳定度修正与流体耦合

- `stabilityCorrection=true`：选用带 `_most` 后缀的 `budget/evapo/aeresist` 着色器（同一源码 `-DVOXELEB_MOST=1` 编译，pipeline.cpp:79-81）；开关与场景 1/L 经 `FluidParameters.localExchange.y/z` 传给着色器（command.cpp:180-196）；`EBState` 用定点整数累加显热（0.1 W m⁻²）、u*（10⁻⁴ m s⁻¹）、有效高度（0.01 m）（structs_cg.h:501-517）；每个节点从 1/L=0 开始，保证逐节点可重复（command.cpp:669）。若 `fluid.localExchange=true`（局部风交换闭合）则稳定度修正被禁用并打印 `STABILITY_CORRECTION disabled`（voxeleb.cpp:213-217）；若场景无土壤/水体地面列（`groundCount=0`）则该节点按中性重算（command.cpp:696-703）。
- 流体（`fluid.enabled`，缺省关）：每节点在 `runEB` 之前先 `runFluid`（LBM 步进 `fluidLbm`+`fluidCommit`，每 32 步合并一次提交以避开 Windows GPU 看门狗，command.cpp:866-908），使用上一节点的表面温度推进风场，再用得到的风/气温做本节点能量平衡，更新后的表面温度进入下一节点（“显式交错耦合”，voxeleb.cpp:502-507）。子步数 = max(ceil(间隔/fluid.timeStep), 风穿越一个计算域所需步数（上限 4096）)（voxeleb.cpp:293-308）。

#### 4.4.6 成像与输出文件

**影像**：`runRT` 对每个观测角做一次 `out` 阶段（二维派发，每线程一像素，GROUP_SIZEXY=8）；`isImage` 时 `output()` 回读后写 TIFF，大气启用时后缀 `_v_a` 否则 `_v`（voxeleb.cpp:603-608,751-754）。光学波段（≤2500 nm）为半球–方向反射因子 HDRF，热红外波段为辐亮度，`isTemperature` 时反演亮温（`inversePlanckTemperature`）。透视投影的天空像元在无有效信号时由大气模型填值；光学波段夜间（Rin ≤ 0.5）置 0（voxeleb.cpp:716-737）。`result_statistics.txt`：表头 `t SZA SAA VZA VAA wavelength <Temperature_K|Radiance>`，每观测角每波段追加一行（仅对值 > 0 的像元求平均）（voxeleb.cpp:474-477,761-849）。

**每节点诊断**（每节点都写，即使不导出过程数据，voxeleb.cpp:861）：

| 文件 | 内容 |
|---|---|
| `diagnostics/scattering_node=<k>.json` | `format=histream-shortwave-scattering-v1`，请求/完成散射阶数、`perOrder[]`（`order` 0 为未散射天空项，含 `maxStepTruncations`、`transmittanceStops`、`tracedRays`、`absorbedShortwave`、`absorbedPar`、`transmittanceResidual`、`truncatedResidual`）、`solarVisibility`、`valid` |
| `diagnostics/physiology_node=<k>_T=<token>.json`(+`.bin`) | 10 项生理量（`directPar`、`diffusePar`、`rssSunlit/Shaded`、`gppSunlit/Shaded`、`netAssimilationSunlit/Shaded`、`latentHeatSunlit/Shaded`）的有效计数与最小/最大/均值；能量过程开启时另写每体元 10 个 float 的 `.bin`；**存在非有限/无效体元时抛异常，进程失败**（voxeleb.cpp:1115-1118） |
| `diagnostics/energy_balance_node=<k>_T=<token>.json` | `applicableVoxelCount`、`temperatureIterations`、`budgetResidualToleranceWm2=2`、`budgetStatesOutsideTolerance`、`budgetResidualWithinTolerance`、稳定度（`inverseObukhovLength`、`zOverL`、`frictionVelocity`、`sceneSensibleHeat`、`aerodynamicUpdates`、`lastZOverLChange`）、`solarVisibility`，以及 6 项收支统计（吸收短波、吸收长波、发射长波、净辐射、热通量和、残差；W m⁻²，无裁剪、无强制闭合） |
| `diagnostics/terrain-environment.json` | 启用地形环境时的输入与“未求解”声明 |

**过程结果**（`isRadiationProcess`/`isEnergyProcess`，`process/`，voxeleb.cpp:1233-1502）：

| 文件 | 记录（float32 小端，逐体元交错） |
|---|---|
| `voxelthermal_T=<token>.bin` | 3 个：向阳温度 K、背阴温度 K、向阳比例 |
| `voxel_scene_links.bin`（仅首节点） | 每体元 6 个 int32：`instanceId`、`meshId`、`spectralId`、`canopyId`、`type`、`faceId` |
| `material_sensor_spectra.csv`（仅首节点） | `spectral_id,wavelength_nm,reflectance,transmittance` |
| `voxelrt_T=<token>.bin/.json`（radiation） | 位置 xyz（m，体元中心 = (voxelId+0.5)×体元边长）+ 短波、长波、净辐射（W m⁻²，漫射 + 向阳比例×直射）+ 组分编码 + 温度 K；`recordFloats = 8` |
| `voxeleb_T=<token>.bin/.json`（energy） | 位置 3 + LE、H、G、GPP、NPP（按向阳比例加权，GPP/NPP 单位 µmol CO₂ m⁻² 叶面积 s⁻¹）+ [土壤剖面 8 层温度，仅存在方法 2/船体钢板体元时] + 组分编码 + 温度；`recordFloats = 10`（无剖面）或 18 |
| `voxelfluid_T=<token>.bin/.json` | 流体启用且能量过程开启时：每格 8 个（`airEnabled` 时 10 个）float：位置、风速三分量、空气温度 K、烟雾浓度、[水汽压 hPa、相对湿度 %] |
| `fluid_wind_H=*m_T=*.tif`、`fluid_air_temperature_H=*m_T=*.tif` | 流体切片 GeoTIFF（`fluid.outputWind/outputAirTemperature`） |

组分编码：土壤 1、植被 2、建筑 3、水体 5，其他 0（voxeleb.cpp:902-911）。`.json` 元数据含 `fields`（偏移）、`energyState`（`finalStateRefreshed`、`temperatureIterations`、`budgetStatesOutsideTolerance`）、辐射/碳通量约定（voxeleb.cpp:1354-1426）。

**stdout 协议行**：`PROGRESS`（5、10、10–95、100）、`SHORTWAVE_SPLIT`、`SHORTWAVE_SCATTERING`、`SHORTWAVE_SKIPPED`、`ENERGY_BALANCE`、`OBSERVATION`、`PROCESS\t<元数据路径>`、可选 `TEMPERATURE_RANGE`（`HISTREAM_TEMPERATURE_DIAGNOSTICS`）、`PASS_TIMING`（`HISTREAM_VOXEL_PASS_TIMING=1`）。

#### 4.4.7 体元传输开关

`voxelFlags`（`voxelflags::resolve`，环境变量优先于工程字段）：`control.voxelDirectionRotation`（`legacy` 缺省/`rot`；`HISTREAM_VOXEL_DIRROT`）、`voxelDirectionRotationBlock`（1/2/4/8/16）、`voxelEmptySkip`（缺省 true；`HISTREAM_VOXEL_SKIP`，开启时用 NanoVDB 层级跳过空节点，结果在浮点舍入内相同）、`voxelProxyGeometry`（缺省 `positionNoIndex`，输出与 `full` 逐位相同；`HISTREAM_VOXEL_PROXY`）（voxelflags.h:26-101）。`rot` 只影响 VoxelEB 的热红外漫射，短波多次散射仍用固定 64 方向格点（docs/voxel-adaptive-and-direction.md:47-49,184）。

### 4.5 小结与待核实事项

三种模式的运行要点：

1. FacetRT 是“一次可见性图 + 单一太阳方向 + 每波段 Jacobi”，无时间循环，成功退出码仅 0/1。
2. FacetEB 的可见性图跨节点共享；每节点依次做短波窗口求解（辐射）、GPU 内 RT–EB 耦合迭代（长波 + 能量平衡）、成像波段；节点未收敛不中止运行，最终以退出码 2 报告，结果全部保留。
3. VoxelEB 每节点依次做太阳可见度、直射、1–3 阶散射、再进入最多 50 次的“长波重追踪 + 生理 + 通量 + 牛顿”迭代；未收敛只写诊断，进程退出码仍为 0。
4. 两个 EB 引擎都没有独立的 spin-up 阶段，初值取起始节点气温（VoxelEB 的土壤缺省 `Tsold=300 K`），预热需通过 `meteo.start` 实现。

待核实事项（本章按代码已尽力核对，下列条目仍需确认）：

- VoxelEB `meteo.Tsold` 为 JSON `null` 时的处理：`ProjectJson::number` 对 null 的行为未读，fileio_json.cpp:882 的缺省 300 K 是否在 null 时也生效，待核实。
- FacetEB 气象文件中水汽压、气压的单位：源码注释标“待核对”，本章按 hPa 使用，依据来自公式使用处。
- FacetEB 成像波段光谱换算因子 `0.001`（core.cpp:8883-8891）、太阳位置海拔固定 100 m 的依据，源码标“待核对”。
- VoxelEB 旧阶段 `updateL` 只创建管线、不被 `runEB` 调用的结论来自 command.cpp 的调用点检索，未逐文件核对其他调用方。
- `server.mjs` 中 FacetEB 握手的超时/暂停逻辑只读取了匹配行，细节待核实（属服务端章节）。
- VoxelEB `gap` 阶段 `voxelDirs.solar` 的“向阳比例”语义以 `voxelrad_solar.comp` 文件头为准，未逐行核对其步数上限与 `SolarStepLimit` 的取值。
- VoxelEB 光谱窗口：理论手册 §8.2 与本章 4.4.3 对“直射是否服从窗口宽度”的描述不一致，以代码为准，理论手册措辞待修订。

## 5. STREAM 影像级引擎：配置、强迫数据与运行流程

本章说明 STREAM 影像级引擎“程序实际如何运行”：命令行入口、工程文件与引擎配置的字段和校验、气象强迫数据的登记与读取、引擎内部的计算步骤、反演与同化的触发条件、输出目录与产品、服务端任务队列与 `/api/stream/*` 路由。物理方程与推导见《理论手册》（遥感像元总述见其 0.2 节，辐射方程见 3.1/3.2 节，能量平衡与起转见 7 节，反演与同化见 15 节），界面操作见《用户手册》；本章只指向这些章号，不重复推导。文中括号内为源码依据（仓库相对路径:行号）。

### 5.1 总体结构与数据流

STREAM 影像级引擎与 HiStream 三维引擎相互独立：一次运行对应**一个过境时刻、一景栅格**，输出整景光学方向反射率、热红外亮温与发射率归一化表面温度（`models/stream/README.md:3`）。运行时由三层组成：

```
工作台 scene（界面）
   │  POST /api/stream/run
   ▼
server/stream/index.mjs ── 提交期校验：config.mjs buildEngineConfig（坏参数不入队）
   │  入队 → 串行 pump → start()：再次生成配置 → 建输出目录
   │  写 run-config.json / scene.json / run-manifest.json
   ▼
spawn stream.exe run run-config.json        （同一时刻最多 1 个进程）
   │  stdout：STREAM<TAB>progress<TAB>百分比<TAB>阶段 / [STREAM] 日志行
   │  stderr：错误与 GDAL/PROJ 诊断
   ▼
GeoTIFF 产品 + scene_summary.json ──→ 服务端写 run-manifest.json 终态 ──→ SSE job-closed
```

（`server/stream/index.mjs:287-333`，`server/stream/index.mjs:206-253`，`models/stream/app/image.cpp:189`）

引擎内部按 12 个步骤串行推进（`models/stream/app/image.cpp:3-18`）：读配置与地类表 → 读地表栅格 → 载入气象强迫（仅 EB）→ 土壤水分 → 光谱准备 → 组分温度（RT 读入 / EB 求解）→ 反射天空 L↓ → 成像准备 → 反演（可选）→ 同化（可选）→ 逐像元成像 → 写出结果。像元计算按行用 OpenMP 并行，`threads > 0` 时调用 `omp_set_num_threads`，为 0 则不干预（由 OpenMP 运行库默认决定）（`models/stream/app/image.cpp:194-196`）。

两种温度来源决定数据流的分叉（`models/stream/app/config.h:35-36`，`models/stream/app/config.cpp:353-355`）：

| 项 | RT（`temperature.mode = "rt"`，缺省） | EB（`"eb"`） |
| --- | --- | --- |
| 组分温度 Tss/Tsh/Tcs/Tch（K） | 用户给定：常数、单波段或 24 波段栅格、或 4 波段 `components.tif` | 由逐小时气象强迫驱动的四组分能量平衡，从起转起点推进到过境时刻 |
| 气象强迫 | 不用 | 必需（网格数据包或站点 CSV） |
| 土壤水分 | 用户栅格/常数，缺省 0.25 | `soilMoisture.source = era5`（取强迫的 swvl1）或 `user`（取用户栅格） |
| 额外产品 | 无 | `components.tif/.json`、`eb_forcing.tif`、`eb_fluxes.tif` |
| 进度区间 | 读取 1% → BSM 20% → 成像 25–90% | 读取 1–2% → 能量平衡 10–70% → 成像 70–90% |

### 5.2 命令行入口 stream.exe

入口为 `models/stream/app/main.cpp`。Windows 上用 `wmain` 取 UTF-16 参数并转 UTF-8，全程路径为 UTF-8，并设 `GDAL_FILENAME_IS_UTF8=YES`；标准输出/错误设为无缓冲，使进度行可被服务端实时读取；若可执行文件同目录有 `proj/proj.db` 则覆盖 PROJ 搜索路径（`models/stream/app/main.cpp:52-65`，`:233-250`）。

| 子命令 | 参数与约束 | 行为 | 退出码 |
| --- | --- | --- | --- |
| `run <config.json>` | 恰好 2 个参数 | 调 `runImage`，见 5.6 | 成功 0 |
| `era5-register <pack.json> [--product auto\|era5\|era5-land] <file.nc>... [--files-from <list>]` | ≥3 个参数；`--product` 缺省 `auto` | 扫描 ERA5/ERA5-Land 逐小时 NetCDF，写 `stream-era5-pack` 索引；`auto` 时按纬向步长 <0.15° 判为 era5-land，否则 era5 | 必需变量在公共时段逐小时连续返回 0，否则 3（索引仍写出） |
| `forcing-register <pack.json> --metadata <预设\|元数据.json\|内联JSON> <file.nc>... [--files-from <list>]` | ≥4 个参数；缺 `--metadata` 抛错 | 按元数据登记任意网格来源，写 `stream-forcing-pack` 索引；元数据文件转绝对路径写入索引 | 成功 0；站点形态元数据、网格不一致、变量缺失等抛错 → 1 |
| `forcing-preset [name]` | 无名称：列出预设名；有名称：输出展开后的预设 JSON（去掉 `preset` 键） | 未知名称抛错 → 1 | 0 |
| `forcing-check <预设\|元数据.json\|JSON> [--data pack.json\|station.csv ...] [--json]` | ≥2 个参数；`--data` 之后的非选项参数都视为数据文件 | 先校验元数据；元数据无误且给了 `--data` 时再打开数据（站点 CSV 或网格数据包）核对变量/列、时间、站点 | 通过 0，有错误 3 |

（`models/stream/app/main.cpp:171-227`，`models/stream/forcing/era5.cpp:389-517`，`:436-437`，`:1109-1186`，`models/stream/forcing/forcing_metadata.cpp:696-707`）

退出码约定（`models/stream/app/main.cpp:10`，`:171-179`，`:251-256`）：**0** 成功；**1** 运行期异常（`std::exception` 被捕获，stderr 输出 `[STREAM] 错误：…`）；**2** 用法错误（参数组合不匹配，打印 usage）；**3** 校验失败或登记失败（`forcing-check` 有错误、`era5-register` 必需变量不连续）。`--files-from <list>`：UTF-8 文本，一行一个 NetCDF 路径，忽略 `\r` 与首行 BOM，空文件报错；用于绕开 Windows 命令行约 32767 字符的上限（`models/stream/app/main.cpp:86-106`）。

`forcing-check --json` 的输出为 `{ok, errors[], warnings[], notes[], resolved}`，仅在元数据无误时附 `resolved` 摘要（`models/stream/app/main.cpp:146-160`）。

### 5.3 工程文件与引擎配置

#### 5.3.1 工程文件 stream-image-project v2

STREAM 工程独立于三维工程：只读写 `stream-project.json`，不合并进统一工程 `project.json`（`server/stream/config.mjs:23-24`，`:32`）。读取时目录依次查找 `stream-project.json`、`project.json`；统一工程（`isUnifiedProject`）一律返回 `null`，STREAM 不通过它打开（`server/stream/config.mjs:201-211`；该段上方注释称“统一工程编译成 scene”，与代码行为不符，以代码为准）。

| 字段 | 取值 | 说明 |
| --- | --- | --- |
| `kind` | `stream-image-project`（v2）；兼容旧 `stream-scene-project`（v1） | 其他 kind 返回 null；v2 的 `schemaVersion` 必须为 2，v1 必须为 1，否则抛“不支持的…版本” |
| `schemaVersion` | 2 | 保存时固定写 2（`server/stream/config.mjs:251`） |
| `name` / `description` | 字符串 | `name` 缺省取工程目录名 |
| `scene` | 对象 | 见 5.3.2；v1 的 scene 经 `migrateScene` 迁为 v2 形态 |

（`server/stream/config.mjs:212-221`，`:228-262`）

读写时的路径映射（`mapStreamScenePaths`）：读取把相对工程目录的路径解析为绝对路径；保存把位于工程目录内的路径存为相对路径（`/` 分隔），目录外保持绝对路径。涉及的路径包括所有栅格字段、`temperature.components.file`、`temperature.era5.pack`、`temperature.forcing.pack/files/metadata`（仅当像路径而非预设名）、同化的 `data/optical/uncertainty/covariance/qa/opticalUncertainty/opticalQa` 及各视角文件、`outputRoot`（`server/stream/config.mjs:146-184`，`:237-241`）。保存已存在的目标文件时，要求它本身是 STREAM 工程，否则拒绝覆盖；`overwrite === false` 时一律拒绝；写盘采用“先写 `.tmp` 再 rename”的原子替换（`server/stream/config.mjs:243-261`）。

v1→v2 迁移：本地日期+时钟+时区（缺省 +8）换算为 ISO UTC 过境时刻；v1 没有能量平衡，温度固定为 RT 且 `components.source = 'fields'`（`server/stream/config.mjs:114-142`）。

#### 5.3.2 scene → stream-image-config v2

`buildEngineConfig(scene, {outputDir, dataDir})` 生成引擎配置，**在提交时**就完成校验，使错误发生在入队之前，而不是出队、建目录之后（`server/stream/config.mjs:15-16`，`:425-637`）。引擎侧 `readImageConfig` 再做一遍同类校验（`models/stream/app/config.cpp:89-558`），两者范围保持一致，但并非逐项相同（见 5.3.5）。

顶层字段：

| 字段 | 类型 | 说明 / 校验 | 依据 |
| --- | --- | --- | --- |
| `kind`、`schemaVersion` | `"stream-image-config"`、2 | 引擎严格检查 | `config.cpp:95-96` |
| `outputDir`、`dataDir` | 路径 | 引擎要求两个目录**已存在**，否则报“输出目录不存在”“STREAM 数据目录不存在” | `config.cpp:98-101` |
| `overpass` | UTC 字符串 | 须匹配 `YYYY-MM-DDTHH:MM[:SS]Z`，年份 1900–2200，且为真实存在的日期（拒绝 2019-02-30、T24:00）；无秒时服务端补 `:00` | `config.mjs:35-48`，`:429-431`，`:607`；`config.cpp:102` |
| `surface` | 对象 | `type`（IGBP 地类栅格，必须为文件）、`lai`（必填）、`soilMoisture`、`dem`、`latitude/longitude`、`terrain`、`soilBrightness` | `config.mjs:448-501` |
| `view` | 对象 | `vza`、`vaa`（必填，常数或栅格） | `config.mjs:514-517` |
| `optical` | `{enabled, bands[]}` | 波段中心 400–2400 nm | `config.mjs:440`；`config.cpp:187` |
| `thermal` | `{enabled, bands[]}` | 波段中心 3–20 μm；`fwhm` 只记录，热红外按波段中心单色计算 | `config.mjs:441`；`config.cpp:75-77` |
| `temperature` | 对象 | `mode` = `rt`/`eb`，见 5.3.4 | `config.mjs:529-600` |
| `inversion` | 对象（可选） | 见 5.7 | `config.mjs:614` |
| `assimilation` | 对象（可选） | 见 5.7 | `config.mjs:615` |
| `skyLongwave` | 对象（可选） | `{mode:'none'}`、`{value}`（0–700 W m⁻²）或 `{file, band}`；省略 = 引擎 `auto` | `config.mjs:618-626`；`config.cpp:529-541` |
| `region` | 对象（可选） | `startRow/endRow/startColumn/endColumn`，0 起、终点不含；须为非负整数且终点大于起点 | `config.mjs:628-635` |
| `threads` | 整数 | 服务端限制在 0–256 | `config.mjs:612` |

启用条件：光学、热红外、反演、同化**至少启用一个**，否则报“请至少启用光学、热红外模拟或同化”；EB 并不自动算作一项任务。反演与同化互斥——同化启用时 `inversionTarget` 一律为 `none`（`server/stream/config.mjs:434-439`；引擎侧 `models/stream/app/config.cpp:344`，`:347-349`）。波段数上限 64；波段名中的逗号、花括号、换行替换为空格并截断到 40 字符（服务端）；`center` 须在范围内，`fwhm ∈ [0, high−low]`，热红外另要求 `fwhm ≤ 2·(center − 0.1)`（μm）（`server/stream/config.mjs:291-304`；引擎 `models/stream/app/config.cpp:62-82`，引擎不检查 `fwhm ≤ high−low`）。

#### 5.3.3 栅格输入字段

栅格字段形如 `{file, band}` 或 `{value}`。服务端 `inputField` 规整规则：给了文件（且 `mode !== 'value'`）必须是**存在的绝对路径**；`band` 须为 0–100000 的整数，0 不写出；否则取常数 `value`（`constant=false` 的字段不允许常数）；都没有时，必填字段报“缺少…”，选填字段返回 undefined（`server/stream/config.mjs:267-286`）。引擎 `readField` 要求 `value` 必须是 JSON 数字（字符串、布尔会报错，不再静默当缺失）（`models/stream/scene/raster.cpp:91-105`）。

波段序号：服务端注释称“0 起”，但引擎把 `band > 0` 当作 **1 起**的波段号、0/缺省取第 1 波段（`models/stream/scene/raster.cpp:151`，`models/stream/app/image.cpp:237`）；结果查看器 `/api/stream/raster` 的 `band` 则是 0 起（`server/stream/rasters.mjs:123-124`）。写入引擎配置的 `band` 以引擎的 1 起语义为准。

**网格约定**：地表类型栅格定义参考网格（宽、高、仿射变换、坐标系）。所有其他输入栅格必须与它**同尺寸、同坐标系、同地理变换**（四个角点位移容差 1e-6 个参考像元），不做重投影或重采样，否则报“…与地表类型栅格的…不一致，请先对齐到同一网格”（`models/stream/scene/raster.cpp:36-76`，`:119-124`）。读取时 nodata→NaN，随后若声明了 scale/offset 则再做 `值×scale+offset`（`models/stream/scene/raster.cpp:131-141`）；网格像元总数上限 4×10⁸（`models/stream/app/image.cpp:234`）。

#### 5.3.4 RT 与 EB 的配置差异

| 项 | RT | EB |
| --- | --- | --- |
| 组分温度 | `components.source = 'package'`：4 波段 `file`（Tss、Tsh、Tcs、Tch）；否则 `soilSunlit/soilShaded/leafSunlit/leafShaded` 逐项给出（常数或栅格）。仅当 `thermal.enabled`、LSCT 反演或同化时才要求组分温度 | 不需要 |
| 组分温度常数范围 | 服务端要求 **150–400 K**（防止把 ℃ 当 K 填写） | — |
| 24 波段栅格 | 单文件 24 波段且未指定波段时，按过境时刻 UTC 小时四舍五入（上限 23）取第 `hour+1` 波段；其他多波段文件必须指定波段 | — |
| 气象强迫 | — | `buildForcing` 生成 `era5`/`forcing` 块（见 5.5） |
| `soilMoisture.source` | — | `era5`（缺省，取强迫 swvl1，不要求 `surface.soilMoisture`）或 `user`（**必填**土壤水分栅格） |
| `elevationCorrection` | — | 缺省 true（仅 `=== false` 才关）；需同时有 DEM 且强迫含 `z` 才真正订正 |
| `spinupHours` | — | 缺省不写（引擎取 7）；服务端校验 0–240，向上取整（减 1e-9 容浮点误差） |
| `diffuseFraction` | 选填，须 0–1；**不接受 −1/Erbs**（RT 无短波输入） | 缺省 0.2；−1 表示 Erbs，或 0–1 |
| `energy` 块 | 只透传发射率相关项（`soilEmissivityModel`、`soilEmissivityK`、`surfaceTemperatureNormalisation`、`soilEmissivity`、`soilEmissivityWet`）与 `diffuseFraction` | 完整的 EB 参数（见 5.3.5） |

（`server/stream/config.mjs:450-459`，`:519-524`，`:529-600`；`models/stream/app/image.cpp:163-181`；`models/stream/app/config.cpp:482-525`）

发射率相关项（`soilEmissivityModel`、`surfaceTemperatureNormalisation` 等）**写在 `temperature.energy` 下，RT 与 EB 都读取**（`models/stream/app/config.cpp:493-525`）；服务端对这两个选项总是显式写出，缺省值为 `hapke` 与 `retrieval`（`server/stream/config.mjs:519-524`）。

#### 5.3.5 EB 能量参数、缺省值与校验

服务端对 `temperature.energy` 的数值覆盖项只在界面给出有效数字时才写入；字符串形式的数字会被 `Number()` 规整为 JSON 数字（引擎只接受 JSON 数字）；未知键不报错，仅在 `sceneWarnings` 里警告并被忽略（`server/stream/config.mjs:71-109`，`:547-567`，`:640-644`；`models/stream/app/config.cpp:30-51`）。

| 键 | 引擎缺省 | 范围 / 取值 | 依据 |
| --- | --- | --- | --- |
| `scheme` | `tirt` | 仅 `tirt`（兼容 `tirt-analytic`）；服务端固定写 `tirt` | `config.cpp:412-413` |
| `soilHeatMethod` | `surface` | `surface` / `depth` / `direct` | `config.cpp:414-418` |
| `temperatureMethod` | `robust` | `robust` / `direct` | `config.cpp:419-422` |
| `diffuseFraction` | 0.2 | −1 或 [0,1] | `config.cpp:423-426` |
| `enforceSolarLimit` | true | 布尔 | `config.cpp:427` |
| `soilResistance.method / value` | `constant` / 200 s m⁻¹ | `constant`/`sellers`；value ∈ (0, 10⁵] | `config.cpp:428-444` |
| `stomatalG0` | 0.01 | >0 | `config.cpp:449`，`:476` |
| `minWindSpeed` | 0.05 m s⁻¹ | ≥0 | `config.cpp:450`，`:476` |
| `maxAerodynamicResistance` | 400 s m⁻¹ | ≤0 表示不限幅 | `config.cpp:451`，`:457-458` |
| `referenceHeight` | 15 m | >0 | `config.cpp:452` |
| `heatRoughnessKb` | −1（≤0 按地类） | ≤20 | `config.cpp:453-455` |
| `soilEmissivity` / `soilEmissivityWet` | 0.955 / −1（内置 +0.03，上限 0.995） | (0,1]；Wet 须不小于干端 | `config.cpp:459-460`，`:508`，`:519-524`；`config.h:155-157` |
| `leafEmissivity` | 0.985 | (0,1]；只作用于 EB 的能量平衡 | `config.cpp:461`，`:476` |
| `soilConductivity` / `soilVolumetricCapacity` | 1.55 / 1.298×10⁶ | >0 | `config.cpp:462-463` |
| `soilThermal` | `constant` | `constant`/`moisture`（Johansen） | `config.cpp:464-466` |
| `soilHeatFraction` | 0.25 | [0,1]，仅 `direct` 方法使用 | `config.cpp:467-469` |
| `spectralBinNm` | 50 | 1–100 的整数（nm） | `config.cpp:470-471` |
| `maxIterations` | 50 | ≥1（服务端要求 1–10000 整数） | `config.cpp:472`；`config.mjs:60-64` |
| `maxStabilityIterations` | 60 | 仅为兼容保留，不参与求解 | `config.h:176` |
| `iceMelt` | true | 服务端仅 `false` 时才写出 | `config.mjs:553-554`；`config.cpp:473` |
| `soilEmissivityModel` | `hapke` | `hapke`/`constant` | `config.cpp:509-512` |
| `soilEmissivityK` | 1.0 | >0 | `config.cpp:513-514` |
| `surfaceTemperatureNormalisation` | `retrieval` | `retrieval`/`directional` | `config.cpp:515-518` |

补充：`energy` 之外的 EB 键 `spinupHours` 缺省 7、范围 0–240，须为 JSON 数字（`config.cpp:399-406`）；水体、冰雪等地类在 EB 中的特殊处理见 5.6.4。

#### 5.3.6 其他校验规则与服务端/引擎差异

| 项 | 服务端（提交期） | 引擎（运行期） |
| --- | --- | --- |
| VZA 常数 | [0, 90) | [0, 90) |
| VAA 常数 | [−360, 360] | [−360, 360] |
| 纬度/经度常数 | [−90, 90] / [−180, 360] | 同左（`config.cpp:545-554`） |
| 纬度、经度栅格 | 仅 `locationSource === 'input'` 时必填并写入；否则省略，由引擎用地表类型栅格的地理参考换算 WGS84（像元中心） | 省略时 `geographicFromGrid`；无地理参考则报错；经度 0–360 自动折到 ±180 |
| 土壤亮度 `soilBrightness` | 0.05–1，省略用 classes.json 的 0.25 | 同左（`config.cpp:157-159`） |
| 地形 `terrain.mode` | `none/single/facet(=composite)/hill/ridge`；`ridge.endFraction` ∈ [0,0.5] | 单一坡坡度 [0,90)、坡向 [0,360)；面元法要求 DEM 为文件；各模式缺必需栅格报错 |
| 反演观测 σ | 0.01–10 K（缺省 0.5） | (0, 10] K |
| 组分温度 | 常数 150–400 K | 读入后**不在 100–400 K 的像元置 NaN**（`image.cpp:1175-1178`） |
| 同化 σ、`lstWavelengthUm` | 须为正的 JSON 数字；波长 3–25 μm | 同左（`config.cpp:266-282`） |
| 区域 `region` | 非负整数、终点>起点 | 另检查不超出网格（`image.cpp:267-268`） |

（`server/stream/config.mjs:333-351`，`:460-513`，`:506-517`；`models/stream/app/config.cpp:113-155`，`:215-221`）

### 5.4 地类表、物理数据与输入检查

`dataDir`（服务端取 `stream.exe` 同目录下的 `stream-defined/`）必须含 `classes.json`、`optipar_new.csv`（PROSPECT/BSM 系数）、`Esun_.dat`、`Esky_.dat`（参考太阳光谱，400–2400 nm，1 nm 间隔共 2001 点）；服务端 `status().dataReady` 检查这四个文件（`server/stream/index.mjs:96`，`:127`；`models/stream/physics/constants.h:44-45`；`models/stream/CMakeLists.txt:83-90`）。

`classes.json`（`kind = stream-classes`，`schemaVersion = 1`）为 256 项的地类表，按像元地类码四舍五入到 0–255 查表，表中未定义或非有限的码视为无效（`models/stream/scene/classes.cpp:68-75`，`:85-86`）。内置地类码：0 未分类、1–5 森林、6–7 灌丛、8–9 稀树草原、10 草地、11 湿地、12 农田、13 城市（`ground.surface = impervious`）、14 镶嵌、15 冰雪（`snow`）、16 裸地、17 水体（`surface = water`）、18 与 255 填充值（`surface = fill`）（`models/stream/classes/classes.json:34-692`）。三类 `surface` 的运行语义：`vegetation`（含裸地、城市、冰雪，需求解 EB 与成像）、`water`（不求能量平衡）、`fill`（跳过）；`ground.surface` 非 `soil` 的地类按平坦地面处理，用常数反射率（缺省 0.15）与常数发射率，`snow/ice/glacier` 同时置 `ice` 标记（组分温度上限 273.15 K）（`models/stream/scene/classes.cpp:125`，`:174-186`）。土壤缺省含水量 0.25，BSM 亮度缺省 0.25（`classes.cpp:93-96`）。

输入有效性（植被像元）：LAI 有效范围 [0, 20]（`kMaxLai`），nodata、负值、>20 均按无效输入处理；日志警告并计入 `counts.laiOutOfRangePixels / laiNoDataPixels`；水体与填充类不受 LAI 影响（`models/stream/physics/constants.h:40-41`，`models/stream/app/image.cpp:275-287`，`:1328-1331`）。土壤水分：EB+强迫时取过境时刻 swvl1 并限幅到 0–1，强迫无值的像元取地类缺省 0.25；用户来源时 >1 视为百分数（÷100）并限幅到 **0.05–0.55**（BSM 有效范围），两种来源的限幅不一致，源码已标为遗留问题（`models/stream/app/image.cpp:433-461`）。

### 5.5 气象强迫数据（仅 EB 使用）

#### 5.5.1 概念与预设

EB 的物理核心只用六个量：近地面气温、水汽压、气压、风速、水平面向下短波、向下长波。引擎不再写死 ERA5 变量名，而由**强迫元数据**（JSON，`kind = stream-forcing-metadata`）描述“数据里每个量叫什么、什么单位、测在多高、怎么随时间取值”；标准化层把任意来源转成内部标准量（沿用 ERA5 命名、SI 单位）：`t2m`（K）、`d2m`（K）、`sp`（Pa）、`u10/v10`（m s⁻¹）、`ssrd/strd`（短/长波）为**必需**量；`skt`、`stl1`、`stl2`、`swvl1`、`z` 为可选量（其中 `stl1/stl2` 模型已不使用）（`models/stream/forcing/era5.cpp:367-381`，`docs/forcing-metadata.md:9-18`）。标准化后一律落在**逐小时整点**栅格上；瞬时量在整点取值，通量量存为“截至该整点的前一小时平均通量”。ERA5/ERA5-Land 走“恒等布局”的原读取路径，不经重采样（`models/stream/forcing/era5.h:11`，`:51-52`）。

| 预设名 | 形态 | 步长 | 要点 |
| --- | --- | --- | --- |
| `era5` | gridded | 1 h | `t2m d2m sp u10 v10`；`ssrd/strd` 为 J m⁻² `accumulated`、终点戳；高度 2 m/10 m |
| `era5-land` | gridded | 1 h | 同 era5，`ssrd/strd` 为 `cumulative`（自 00 UTC 累积，逐时差分） |
| `gldas-noah-0.25`、`gldas-noah-1.0` | gridded | 3 h | `Tair_f_inst` 等；比湿、时段平均辐射 W m⁻²；两预设内容相同（**参考高度与时间戳含义按产品文档推断，未经真实数据验证：待核实**） |
| `station-csv` | station | 自动检测 | 列 `time, station_id, lat, lon, elev, temp(°C), rh(%), pressure(hPa), wind_speed, swdown, lwdown`；预设故意不写观测高度，运行时按缺省高度并警告 |

（`models/stream/forcing/forcing_metadata.cpp:57-126`，`:688-692`；服务端预设名清单 `server/stream/forcing.mjs:28`）

预设可被覆盖：`{"preset": "...", "variables": {...}}` 对象逐键递归合并、数组与标量整体替换（`docs/forcing-metadata.md:27`，`models/stream/forcing/forcing_metadata.cpp:51-53`）。元数据主要段落：`time`（`variable`/`column`/`format`/`stepHours`/`utcOffsetHours`(±14 h)/`maxGapHours`）、`grid`（经纬度变量别名）、`station`、`variables`（各通道的 `source/unit/scale/offset/kind/stamp/height_m/missing/range`）、`heightCorrection`（风对数律、气温递减率）、`spatial`（`bilinear`；站点 `nearest`/`idw`、`maxStations` 1–4、`lapseRateKPerKm` 0–20，缺省 6.5）（`models/stream/forcing/forcing_metadata.cpp:423-438`，`:512-529`，`:607-662`）。向下短波必需，缺则报错；向下长波缺失时可写 `fallback: "brutsaert"`（晴空参数化，强制警告“不含云，阴天偏低”）。高度缺失时风缺省 10 m、气温/湿度缺省 2 m，给警告且不订正（`models/stream/forcing/forcing_metadata.cpp:523-529`，`:557-560`）。时间重采样、站点插值、单位表的细节见 `docs/forcing-metadata.md` 第 3–6 节；`forcing_resample.cpp`、`station_source.cpp` 的实现细节本章未逐行核对（**待核实**）。

#### 5.5.2 数据包登记与索引

网格数据（NetCDF；GRIB 须先转换）通过登记生成**数据包索引**，站点 CSV 无需登记。

| 索引 `kind` | 生成命令 | 关键字段 |
| --- | --- | --- |
| `stream-era5-pack` | `era5-register` | `product`（era5/era5-land）、`year`、`period{start,end,hours}`、`fullYear`、`grid{latitude,longitude{first,step,count}}`、`variables`（逐变量 `hours/expected/complete`）、`files[{path,bytes,variables,timeSteps}]`、`complete`、`missingVariables`、`incompleteVariables`、`createdAt` |
| `stream-forcing-pack` | `forcing-register` | `product`（= 元数据名）、`metadata`（预设名或元数据 JSON 路径）、`stepHours`、`year`、`period`、`grid`、`files`、`complete`（恒 true）、`created` |

（`models/stream/forcing/era5.cpp:489-509`，`:1161-1179`）

登记规则：多个文件须共用同一经纬网，否则报错；`files[].path` 在索引同目录内的写相对路径（数据包随工程拷贝仍有效），否则写绝对路径，并记录字节数，`Pack::open` 据此检测登记后文件是否被改动（`models/stream/forcing/era5.cpp:399-430`）。ERA5 登记的公共时段是各必需变量时间范围的交集，`fullYear` 表示覆盖整个日历年，**逐小时连续**才判 `complete`；`forcing-register` 不判连续（原始间隔可大于 1 h），公共时段只取气温、湿度、短波三个通道的交集，覆盖检查推迟到运行时（`models/stream/forcing/era5.cpp:440-488`，`:1103-1108`，`:1146-1158`）。Windows 上 netCDF 经 ANSI 代码页打开文件，含非 ASCII 字符的路径只能走 8.3 短文件名，拿不到纯 ASCII 短名则报错，要求移到英文路径（`models/stream/forcing/era5.cpp:55-76`）；服务端在提交运行时提前做同样的预检查（`server/stream/index.mjs:47-62`，`:303-307`）。

#### 5.5.3 运行配置中的强迫块与优先级

配置形态（`temperature.forcing = {pack, files, metadata}`；`temperature.forcingMetadata` 为 `forcing.metadata` 简写；旧字段 `temperature.era5.pack` 继续有效）：

- **数据包**：`forcing.pack` 非空则用它，否则回退 `era5.pack`；两者并存时 `forcing` 优先，服务端只输出 `forcing` 块（`server/stream/forcing.mjs:164-194`，`models/stream/app/config.cpp:362-365`）。
- **元数据来源优先级**：配置 `forcing.metadata`/`forcingMetadata` > 数据包索引的 `metadata` 键 > 按索引 `product` 的预设（`era5-land` → `era5-land`，其余 → `era5`）（`docs/forcing-metadata.md:241`，`models/stream/forcing/era5.cpp:589-594`）。
- **形态检查**：站点形态须有 `forcing.files`（站点 CSV）或元数据 `station.stations[].file`；网格形态须有数据包，否则报“EB 模式需要气象数据包”（`models/stream/app/config.cpp:376-385`）。形态由元数据 `form` 决定，缺省时预设为 `station-csv` 取 station，其余 gridded（`server/stream/forcing.mjs:141`）。
- **元数据解析**：引擎 `resolveForcingMetadata`：以 `{` 开头 → 内联 JSON；预设名 → 内置预设；否则当文件路径（相对路径以 `forcingBaseDir` 即配置文件所在目录为基）（`models/stream/forcing/forcing_metadata.cpp:820-846`）。服务端另限制：元数据文件须为绝对路径、不含 `..`、位于工程目录/元数据目录/数据包目录内、≤256 KiB 且 `kind` 合法；内联对象 ≤64 KiB；站点文件 ≤64 个、`station.stations` ≤256 项、站点文件路径不得含 `..`；数据包索引 ≤64 MiB 且 `kind` 为两种之一（`server/stream/forcing.mjs:28-34`，`:57-138`）。

#### 5.5.4 校验与登记任务

`forcing-check` 分两步：元数据（结构、枚举、单位、kind/unit 匹配）→ 数据（变量/列是否存在，时间解析，站点位置）；必需量缺失报错，可选量（`skt`、`swvl1`、`z`，及有 Brutsaert 回退的长波）仅警告（`models/stream/forcing/forcing_metadata.cpp:860-883`）。服务端 `POST /api/stream/forcing/check` 把元数据（预设名/对象/路径）、数据包与站点文件交给引擎，限时 60 s，退出码 0 或 3 都解析为报告，其余视为异常（`server/stream/index.mjs:547-570`）。

登记在服务端是一个入队任务（kind = `era5`）：1–4096 个文件且必须存在；`metadata`（缺省取 `product`）不是 `era5/era5-land/auto` 即走 `forcing-register`，内联对象先存成 `<era5-packs>/metadata/pack-<时间戳>.metadata.json`；`station-csv` 与站点形态一律拒绝登记；命令行过长（>30000 字符）时改写 `pack-<时间戳>.files.txt` 并用 `--files-from`；任务**成功退出码只有 0**，因此 `era5-register` 返回 3（必需变量不连续）在服务端等同失败，半成品索引与元数据副本被删除；成功后若给了 `label` 则写回索引（`server/stream/index.mjs:427-486`）。索引目录默认 `~/Documents/Stream Studio/era5-packs/`（`server/stream/index.mjs:84-86`）。

### 5.6 引擎运行流程（`stream run`）

#### 5.6.1 步骤与进度

`runImage` 全程写进度行 `STREAM\tprogress\t<百分比>\t<阶段>`（服务端解析）与以 `[STREAM] ` 开头的日志行（`models/stream/app/image.cpp:84-89`）。主要进度节点：

| 百分比 | 阶段 | 条件 |
| --- | --- | --- |
| 1 | 读取地表输入 | 恒有 |
| 2 | 读取气象数据包 | EB |
| 5 | 计算 PROSPECT 叶片光谱 | 光学启用或 EB |
| 7 / 20 | 计算 BSM 土壤光谱 | EB 为 7，RT 为 20 |
| 10 → 70 | 能量平衡求解 | EB，按行分 40 次上报 |
| 起点 70(EB)/20(RT) | 反演（跨度 EB 10、RT 35）或同化（EB 15、RT 35） | 启用时 |
| 25 或 70 或反演/同化终点 → 90 | 逐像元成像 | 恒有 |
| 92 / 100 | 写出结果影像 / 完成 | 恒有 |

（`models/stream/app/image.cpp:228`，`:318`，`:494`，`:533`，`:1041`，`:1139`，`:1252`，`:1285`，`:1313-1316`，`:1416`，`:1647`）

#### 5.6.2 步骤 1–2：配置与地表输入

读配置与地类表；`soilBrightness > 0` 时覆盖 BSM 亮度；`energy.soilEmissivity` 给出时替换所有土壤地类的干端发射率，使 EB 与成像共用（`image.cpp:192-210`）。读 `surface.type` 作参考网格，再读 LAI、VZA、VAA（均**必需**，常数或栅格）、DEM（缺省 0）；经纬度来自输入或网格地理参考，随后把 0–360 经度折入 ±180。处理区域为整景或 `region`（0 起、终点不含，区域外像元保持 NaN）（`image.cpp:229-269`）。

#### 5.6.3 步骤 3–4：EB 气象强迫与土壤水分

EB 时按元数据打开数据源：站点形态 → `StationSource`（CSV 取自 `forcing.files`，缺省取元数据登记表，相对路径以元数据所在目录或配置目录为基）；否则 `era5::Pack`（`image.cpp:299-318`）。统计处理区域内有效像元的经纬度范围与是否含水体，以只载入覆盖该范围的强迫；需要读取的变量 = 必需量 + `swvl1`（来源为强迫时，缺则报错并提示改用 `user`）+ `skt`（有水体像元；缺则水体改取 2 m 气温并警告）+ `z`（要求高程订正且有 DEM；缺则警告不订正）（`image.cpp:320-364`）。

起转（spin-up）时间栅格（理论依据见《理论手册》0.2 节“STREAM EB 起转”）：

```
N          = temperature.spinupHours（缺省 7，0–240，整点时次个数）
lastNode   = ceil((overpass − 300 s)/3600) − 1        （严格早于“过境前 5 min”的最后整点）
desired    = N>0 ? lastNode − (N−1) : floor(overpass/3600)
start      = min(desired, floor(overpass/3600))；强迫不覆盖时向后顺延到连续覆盖的最早时次（警告）
nodes      = start, start+1h, … 直到 < overpass−300 s，再加上 overpass 本身  （共 N+1 个时次；N=0 只有过境节点）
载入范围   = [start − leadHours, ceil(overpass/3600)+1]
```

（`image.cpp:365-425`）。例：过境 02:55、N=7 → 起点为前一日 20:00，共 8 个时次（`models/stream/README.md:111`）。N>0 但过境前没有任何可用整点时次（且不是整年包的年边界）→ 报错；覆盖不足 → 日志警告并在 `scene_summary.json` 的 `temperature.notes` 记“起转不足”（`image.cpp:389-399`）。

土壤水分：EB 且来源为强迫 → 过境时刻 swvl1（限幅 0–1，无值像元取 0.25）；否则读用户栅格/常数（>1 视为百分数，限幅 0.05–0.55，缺省 0.25）（`image.cpp:433-461`）。

#### 5.6.4 步骤 5–6：光谱与组分温度

光谱准备（光学启用或 EB）：PROSPECT 叶片光谱按地类并行计算；BSM 土壤光谱按含水量量化为 1000 档、只算用到的档（同化时全算，因需相邻档求导）；光学波段按高斯响应加权——`fwhm < 1 nm` 视为单波长，否则 σ = FWHM/(2√(2 ln 2))，取 ±3.5σ，再乘参考太阳辐照度（Esun+Esky）并归一化；落在零辐照度区间则报错（`image.cpp:125-148`，`:467-567`）。

**RT 分支**：读取 `components.file`（≥4 波段，取前 4 个）或逐组分栅格/常数（`image.cpp:1160-1173`）。

**EB 分支**：对每个有效像元求解四组分能量平衡（`image.cpp:1038-1159`）。逐像元流程：

1. 跳过未定义/填充地类、位置无效、植被像元 LAI 无效、强迫无有效值的像元（`ebNoForcing` 计数）。
2. 记录过境时刻的 7 个强迫量到 `eb_forcing.tif`。
3. **水体**不求能量平衡，四组分取过境时刻 `skt`（无 `skt` 时取 2 m 气温）。
4. 其余地类：用 `buildSurface` 组装地表（土壤发射率按含水量在干端与饱和端间插值；冰雪类设上限 273.15 K，多余能量记融化热汇），`initialState` 取起点 2 m 气温 `t2m` 作全部初温（无效时依次退回起点后第一个有效 `t2m`、首个模拟日平均气温，并只告警一次）。
5. 沿 `nodes` 逐节点：SPA 太阳位置 → 该节点自己的日序数 → 像元强迫（空间双线性、时间线性；有 DEM 与 `z` 时按高度差订正气温与气压）→ 水平/坡面短波与吸收 → `solveEnergyNode`；节点 0 的 `dt` 固定 3600 s，其余为与上一节点的间隔；**1/L 括号法回退只用于过境节点**。
6. 写组分温度、18 个通量/诊断波段（含冰雪时加第 19 波段）；无叶（LAI=0）时 Tcs、Tch 置 NaN。统计未收敛、温度被钳在限值（175–380 K）上的像元数。

（`image.cpp:719-920`，`:1056-1136`；`models/stream/physics/energy.h:50-51`）

组分温度读入或求解后，统一做有效性过滤：**不在 100–400 K 内的值置 NaN**（`image.cpp:1175-1178`）。地形（`terrain`）模式下，`surface.terrain` 支持单一坡、DEM 面元（3×3 Sobel；最外一圈坡度保持 0 并警告）、山包与山脊复合坡，对应的方法说明见《理论手册》0.2 节与 `docs/terrain-methods.md`（`image.cpp:598-702`）。

#### 5.6.5 步骤 7–8：反射天空与成像准备

热红外亮温含反射天空项 L = Σw_k B(T_k) + (1−Σw)·B(T_sky)，T_sky = (L↓/σ)^¼（方程见《理论手册》3.2 节）。L↓ 来源由 `skyLongwave` 决定（热红外启用、反演或同化时才计算）：栅格/常数（`configured`）；`auto` 且 EB → 过境时刻强迫 `strd`（`era5-strd`）；`auto` 且 RT → Swinbank 晴空式（取阴影叶温，缺则阴影/光照土壤温度作气温代理）；`{mode:'none'}` → 不计。来源记入 `scene_summary.json` 的 `reflectedSky`；L↓ 由背景状态一次确定，反演/同化之后不再重算（`image.cpp:1186-1217`，`models/stream/README.md:77-83`）。成像准备阶段为每个土壤地类建立干端与饱和端两张 Hapke 方向发射率表（`image.cpp:1226-1246`）。

#### 5.6.6 步骤 11–12：逐像元成像与写出

逐像元（按行并行）检查地类、VZA∈[0,90)、VAA∈[−360,360]、LAI 有效、经纬度有效，否则计入 `invalidInputPixels`；计算 SPA 太阳位置并写 `solar_geometry.tif`（太阳天顶角、方位角）（`image.cpp:1319-1337`）。

- **光学**：水体/填充类不成像（计入 `waterOrFillPixels`）；太阳天顶角 ≥90° 记 NaN 并计入 `nightPixels`；土壤像元须有有效含水量；直/散比 EB 取求解时记录的水平面比例，否则取配置值（越界如 −1 时由 `requestedDiffuseFraction` 反算）；每个波段调用 `canopyReflectance`（`image.cpp:1346-1374`）。
- **热红外**：填充类不成像；`thermalSceneAt`（地形几何）+ `setThermalMaterial` + `thermalPixel`，与反演、同化共用同一观测算子，输出各波段亮温与发射率归一化表面温度；组分温度缺失或面元背向传感器计入 `thermalFailedPixels`（`image.cpp:1376-1405`）。

步骤 12 在输出目录写出产品与 `scene_summary.json`（`kind = stream-image-result`，`schemaVersion = 2`），其主要字段：`reflectedSky`、`overpass`、`grid`（含区域范围）、`counts`（10 项计数）、`temperature`（`mode`、EB 的 `era5`/`forcing`/`nodes`/起转与收敛统计/`notes`、`posterior`）、`solarGeometry`、`optical`/`thermal`/`components`/`fluxes` 的波段统计（有效像元数、最小、均值、最大）、`inversion` 或 `assimilation` 块、`runMode`（`simulation`/`assimilation`）、`canopyStructure`、`products`、`elapsedSeconds`。引擎目前只模拟均质冠层，冠层/垄行结构键被记录并警告，不参与计算（`image.cpp:1415-1649`，`:164-165`）。

### 5.7 反演与同化

#### 5.7.1 触发条件

| 能力 | 触发（服务端） | 引擎侧约束 |
| --- | --- | --- |
| LST/LSCT 反演 | `scene.inversion.target` 为 `lst` 或 `lsct`，**且**未启用同化 | `inversion.target` 须为 `lst/lsct/none`；观测 1–32 个 |
| 同化 | `scene.assimilation.mode === 'assimilate'`；`observation` ∈ `brightness/lst/components`，非法值按 `brightness` | `observation` 须为这三种或 `none`；同化与反演不得同时启用 |
| 直接模拟 | 其余情形 | — |

（`server/stream/config.mjs:358-362`，`:438`，`:614-615`；`models/stream/app/config.cpp:195-206`，`:246-254`，`:344`）。服务端注释称：独立的 LST/LSCT 反演保留在引擎可访问接口中，但工作台不再单独列出（`server/stream/config.mjs:437`，`models/stream/README.md:173`）。

#### 5.7.2 反演（inversion）

观测项：`name`、`kind`（`brightness` 缺省 / `lst`）、`value`（K，栅格或常数，必填）、`wavelength`（3–20 μm，必填）、`sigma`（缺省 0.5 K）、可选 `vza/vaa`（缺省沿用场景几何）。公共参数：`prior.sd`（缺省 5 K，0.1–100）、`prior.correlation`（缺省 0，0–0.95，仅 LSCT）、`emissivitySd`（缺省 0.01，0–0.1，仅 LST）、`window`（缺省 1，奇数 1–9，仅 LSCT）、`maxIterations`（缺省 20，1–100）、`mask`（栅格，非零 = 云/无效，必须是文件）；收敛阈值 `tolerance = 1e-3 K`（`server/stream/config.mjs:317-352`，`models/stream/app/config.cpp:195-240`，`models/stream/app/config.h:57-68`）。

- **LST**：每像元一个辐射温度（四组分取同一温度），1/σ² 加权 Gauss–Newton，发射率误差并入观测方差；初值为有效观测均值，温度搜索域 150–400 K；解在 200–380 K 外时不输出，只写 QA 位 2。**不需要**组分温度或 `thermal.enabled`。产品：`lst.tif`、`lst_uncertainty.tif`、`lst_qa.tif`（4 波段：QA、残差 RMS、使用观测数、有效发射率）（`models/stream/app/inversion.cpp:289-392`）。
- **LSCT**：以本次运行的组分温度（RT 输入或 EB 解）为背景的 MAP Gauss–Newton，B = σ²[(1−r)I + r11ᵀ]，可共享 s×s 同地类窗口；只处理有观测权重的组分，缺背景则失败。产品：`lsct.tif`（4 波段后验温度）、`lsct_uncertainty.tif`、`lsct_qa.tif`（7 波段：QA、残差、DFS、观测数、迭代、J_b、J_a）、`lsct_fractions.tif`、`lsct_radiance_fractions.tif`。后验温度**替换**背景用于本次成像；EB 时 `components.tif` 仍保存 EB 解，`posteriorComponents` 统计后验（`models/stream/app/inversion.cpp:393-642`，`models/stream/app/image.cpp:1274-1277`，`:1499-1511`）。LSCT 与同化在 RT 下即使不启用热红外也要求组分温度作背景（`server/stream/config.mjs:576-578`）。

#### 5.7.3 同化（assimilation）

配置只需 `observation` 与 `data`；其余为引擎内置默认。可选项：`optical`（光学反射率观测，波段数 = 光学波段数，给出时 LAI 加入控制量）、`uncertainty`（1σ，与 `covariance` N×N 行主序波段 K² 二选一）、`qa`（uint16 位掩码栅格）、`opticalUncertainty/opticalQa`、`sigmaBrightnessK`（0.5）、`sigmaLstK`（1.0）、`sigmaComponentK`（2.0）、`sigmaReflectance`（0.02）、`lstWavelengthUm`（10.5，3–25）、`rejectQaBits`（895）、`observationLevel`（亮温须为 `canopy`，其余 `surface`，暂无 TOA 算子）、`sourceIds/priorSourceIds`（不得重叠，否则“不能假设误差独立”）、`views[]`（附加视角，**仅亮温**，≤16 个，需 `data` 与 `vza/vaa`，同一亮温文件不得重复）（`server/stream/config.mjs:369-419`，`models/stream/app/config.cpp:246-346`，`models/stream/app/config.h:84-98`）。数据波段数：亮温 = 热红外波段数、LST = 1、组分温度 = 4；亮温同化须启用热红外，光学观测须启用光学（`models/stream/app/assimilation.cpp:206-210`，`config.cpp:340-343`）。

内置默认（`models/stream/app/assimilation.cpp:55-65`）：背景误差——组分温度 5 K（相互相关 0.5）、土壤水分 0.10（范围 0.02–0.60）、Vcmax25 相对 0.3（下限 10，范围 5–250）、LAI 相对 0.3（下限 0.5，上限 10）；温度搜索域 150–400 K；分析温度合理范围 200–380 K；最大迭代 RT 20、EB 10；收敛判据为各控制量步长 < 背景 σ 的 1e-3；`DFS < 0.2` 置低支持位。

控制量：**RT** 为背景有效的组分温度；**EB** 为土壤水分（外加 C3/C4 且 LAI>0 时的 Vcmax25），温度由过境节点能量平衡的隐式导数 dT/dx = −(∂R/∂T)⁻¹∂R/∂x 给出（`EbSolver`，Dual 数前向微分）；有光学观测时再加 LAI。只分析 `vegetation` 类像元；每像元的观测须满足 150–400 K、误差有限正、QA 被接受，光学观测还须太阳在地平线上且反射率 ∈ [0,1]；`covariance` 只覆盖主视角热红外观测（`models/stream/app/assimilation.cpp:283-424`，`models/stream/app/image.cpp:930-1036`）。每个高斯–牛顿步在候选点求值并仅当 MAP 代价下降才接受，依次回溯 1、1/2、1/4（物理与数学推导见《理论手册》15.6 节）。**分析场整体替换背景组分温度**（及作为控制量的 LAI、土壤水分），本次所有光学/热红外产品是分析场的正向图像；背景单独保存（`models/stream/app/image.cpp:1291-1306`）。

同化产品：`analysis_components.tif`（4）、`background_components.tif`（4）、`analysis_uncertainty.tif`（7：Tss、Tsh、Tcs、Tch、土壤水分、Vcmax25、LAI 的分析 1σ）、`analysis_qa.tif`（9：QA、O−B、O−A、O−A RMS、DFS、观测数、迭代、J_b、J_a）；EB 或有光学观测时加 `analysis_controls.tif`、`background_controls.tif`（土壤水分、Vcmax25、LAI）；EB 再加 `analysis_fluxes.tif`（Rn、H、LE、G）（`models/stream/app/assimilation.cpp:748-784`）。

#### 5.7.4 QA 质量位（反演与同化共用）

| 位 | 值 | 含义 | 默认拒收（`rejectQaBits = 895`） |
| --- | --- | --- | --- |
| 0 | 1 | 输入无效 | 是 |
| 1 | 2 | 云/掩膜 | 是 |
| 2 | 4 | 超出合理范围 | 是 |
| 3 | 8 | 反演失败（不计入分析，场为背景） | 是 |
| 4 | 16 | 病态（法方程奇异、EB 隐式导数无效） | 是 |
| 8 | 256 | 信息量不足（LSCT DFS<1；同化 DFS<0.2） | 是 |
| 9 | 512 | 未收敛（迭代上限/每个试探步算子均失败/同化中法方程奇异） | 是 |
| 10 | 1024 | 线搜索停滞（代价已下降，保留最低代价状态） | 否 |
| 11 | 2048 | 未更新（分析 = 背景） | 否 |
| 12 | 4096 | 无观测或无控制量（仅同化；总与位 0 并存） | 否（位 0 已拒收） |

（`models/stream/app/inversion.h:41-60`，`models/stream/app/assimilation.cpp:837-840`）。位 5–7 为 LS4T 保留，STREAM 不设置。`rejectQaBits` 只作用于**输入观测**的 QA 掩膜；输出 QA 仅标注，带位 8/9 的分析场仍替换背景用于成像，引擎不提供输出侧自动掩膜（`models/stream/README.md:200`）。

### 5.8 输出目录与产品

#### 5.8.1 目录结构

```
<outputRoot>/<name>_<runId>/
  run-config.json       服务端写入的引擎配置（stream-image-config v2）
  scene.json            提交时的 scene 原样
  run-manifest.json     运行清单（服务端写，反复覆盖更新）
  scene_summary.json    引擎写（stream-image-result）
  solar_geometry.tif    恒有（2 波段：太阳天顶角、方位角）
  optical_reflectance.tif / tir_brightness_temperature.tif / tir_surface_temperature.tif
  components.tif + components.json / eb_forcing.tif / eb_fluxes.tif      （EB）
  lst*.tif | lsct*.tif                                                  （反演）
  analysis_*.tif / background_*.tif                                     （同化）
```

目录名 `<name>_<runId>`：`name` 为运行名中字母、数字、下划线、连字符以外的字符替换为 `_`、去首尾下划线、最长 64 字符（空则 `image`）；`runId` 为 UTC ISO 时间戳，`:` 与 `.` 换成 `-`（例 `2026-11-26T11-44-03-123Z`）。输出根取 `scene.outputRoot`，其次 `data.outputRoot`，再其次 `~/Documents/Stream Studio/stream`，须为绝对路径；输出目录在出队后、配置校验通过才创建（`server/stream/index.mjs:293-319`，`:84-97`）。工作台按注释会把 `outputRoot` 设为“工程目录/output”（`server/stream/index.mjs:290-291`；前端实现未读，**待核实**）。

#### 5.8.2 产品一览

| 文件 | 波段 | 出现条件 |
| --- | --- | --- |
| `solar_geometry.tif` | 太阳天顶角、方位角（°） | 恒有 |
| `optical_reflectance.tif` | 每个光学波段一个，名称 `<名> reflectance @ <nm> nm` | 光学启用 |
| `tir_brightness_temperature.tif` | 每个热红外波段亮温（K） | 热红外启用 |
| `tir_surface_temperature.tif` | 每个热红外波段发射率归一化方向表面温度（K，LST 产品约定 Σw_kB(T_k)/ε，不含反射天空） | 热红外启用 |
| `components.tif`、`components.json` | Tss、Tsh、Tcs、Tch（K，EB 解，可直接作 RT 的 `components.file`）；json 登记来源、波段名 | EB |
| `eb_forcing.tif`（7） | 气温 K、水汽压 hPa、气压 hPa、风速 m s⁻¹、水平面向下短波、向下长波 W m⁻²、土壤水分 | EB |
| `eb_fluxes.tif`（18/19） | 1 Rn、2 H、3 LE、4 G、5 迭代数、6 是否收敛、7 Obukhov 长度、8 u*、9 稳定度收敛、10 能量残差、11 稳定度残差、12 稳定度限幅、13 阵风残差、14 空气密度、15 气动粗糙度、16 位移高度、17 冠层权重、18 短波限幅标记；第 19 为冰雪融化热通量（仅含冰雪类且 `iceMelt` 开） | EB |
| `lst*.tif`、`lsct*.tif` | 见 5.7.2 | 反演 |
| `analysis_*`、`background_*` | 见 5.7.3 | 同化 |

（`models/stream/app/image.cpp:1448-1537`）。所有 GeoTIFF 为 float32、DEFLATE 压缩，沿用地表类型栅格的网格与坐标系，nodata = NaN；波段名写入各波段描述与 `TIFFTAG_IMAGEDESCRIPTION`（`band names = {…}`）；关闭文件失败（如磁盘满）视为运行失败（`models/stream/scene/raster.cpp:203-229`）。

#### 5.8.3 run-manifest.json

由服务端维护，每次 `writeManifest` 合并补丁并刷新 `updatedAt`（`server/stream/index.mjs:142-145`）。

| 字段 | 出现时机 | 说明 |
| --- | --- | --- |
| `kind` = `stream-image-run`、`schemaVersion` = 2 | 开始 | 读取时也接受旧 `stream-scene-run` |
| `runId`、`name`、`outputDir`、`mode`、`overpass`、`engine` | 开始 | `mode` 为温度模式 `rt`/`eb`，**不含**同化/反演标记（见 `scene_summary.json` 的 `runMode`） |
| `status` | 全程 | `running` → `completed`/`failed`/`stopped` |
| `startedAt`、`updatedAt` | 开始/每次更新 | ISO UTC |
| `exitCode`、`finishedAt`、`products` | 终态 | `products` 为产品**文件名**数组，仅 `completed` 才列出 |
| `error` | 失败且有消息 | 超时、启动异常等 |

（`server/stream/index.mjs:320-325`，`:185-199`）。`products` 的收集规则：输出目录下扩展名为 `.tif` 或 `.json` 的文件，排除 `run-manifest.json`、`run-config.json`、`scene.json`、`scene_summary.json`；因此 `components.json` 也在列。非 `completed` 运行可能留有写了一半的 `.tif`，所以只有成功的运行才列产品（`server/stream/index.mjs:147-153`）。

#### 5.8.4 栅格检查、查看与分类统计

`rasters.mjs` 提供三项只读能力（`server/stream/rasters.mjs`）：

- `inspectRasters`：批量检查路径（≤32 个），只读 GeoTIFF 头（经典 TIFF，不支持 BigTIFF），返回宽高、波段数、数据类型、CRS（`EPSG:*`，32767 视为未知）、像元大小、左上角原点、NoData、波段名；文件不存在或非绝对路径写入 `error`，不抛异常（`:33-112`）。
- `rasterBand`：取一个波段供显示。步长 `step = ceil(max(宽,高)/2048)`，每 `step` 个像元取一个样本；分位数（1/2/98/99%）与 96 档直方图基于样本；`step > 1` 时 `count/最小/最大/均值/标准差` 改用全部像元重算（`stats.sampled` 标记）；值以 Float32 的 base64 返回（`:120-171`）。
- `classStatistics`：按地表类型栅格逐类统计产品各波段的像元数、均值、标准差；参与统计的产品只有 `optical_reflectance.tif`、`tir_brightness_temperature.tif`、`tir_surface_temperature.tif`、`components.tif`、`eb_forcing.tif`、`eb_fluxes.tif`、`lst.tif`、`lsct.tif`（**不含 `analysis_*`**）；尺寸与类型栅格不一致的产品被跳过（`:177-230`）。
- **面积与单位**：像元面积（km²）——地理坐标（`unit === '°'`）按球面逐行计算 R²·Δλ·Δφ·cos φ（R = 6371.0088 km，φ 取该行中心纬度）；米制（`unit === 'm'` 或空）为 |dx·dy|/10⁶；英尺等未知投影返回 null，前端显示“—”（`:186-197`，`:228-229`）。`unit` 由 `server.mjs` 的 `readTiff` 给出（线性单位 9001 → `m`，9002/9003 → `ft`，地理 CRS → `°`）（`server.mjs:4126`）；`readTiff` 的完整解码细节本章未核对（**待核实**）。

### 5.9 服务端任务队列与进程管理

#### 5.9.1 引擎定位

`stream.exe` 候选路径依次为 `<resourceRoot>/engine/stream.exe`、`<modelRoot>/bin_x64/Release/stream.exe`、`…/Debug/stream.exe`，取第一个存在者；数据目录为其同目录的 `stream-defined`。提交与启动前 `assertEngine` 检查可执行文件与 `classes.json`，缺失时提示“请先运行 npm run build:engine”（`server/stream/index.mjs:79-97`，`:134-139`）。

#### 5.9.2 串行队列

`enqueue` 分配递增整数 `id`，状态 `queued`、进度 0、阶段“排队中”，广播 `job-queued`，并用 `setImmediate(pump)` 在 HTTP 应答之后启动，使页面先拿到 `id` 再收事件。`pump`：当前无运行任务且队列非空时取队首，调用 `job.start()`；`start()` 抛错则按 `failed` 收尾。**同一时刻最多运行一个引擎进程**（含图像运行和数据包登记）；队列不去重，同一工程连点会排入多个相同任务（源码已标 DEFERRED）（`server/stream/index.mjs:155-180`）。

任务对象：`kind`（`image`/`era5`）、`label`、`successCodes`（均为 `[0]`）、`start()`、`collect(status)`、`cleanup()`。

#### 5.9.3 launch、进度解析与看门狗

`launch(job, args)`：若 `job.stopped` 已置位则直接以 `stopped` 收尾；否则 `spawn(stream.exe, args, {cwd: 引擎目录, windowsHide, stdio: ['ignore','pipe','pipe']})`，广播 `job-started`（`server/stream/index.mjs:206-228`）。

- **stdout 逐行**：匹配 `^STREAM\tprogress\t([\d.]+)\t(.*)$` → 更新 `progress`（数值）与 `stage` 并广播 `job-progress`；其余非空行去掉 `[STREAM] ` 前缀后作为 `job-log`（`:37`，`:229-235`）。
- **stderr 逐行**：以 `ERROR n:` 或 `Warning n:` 开头的 GDAL/PROJ 库诊断标为 `diagnostic`，其余标为 `error`（`:236-241`）。
- **看门狗**：`JOB_TIMEOUT_MS` 缺省 2 小时，可用环境变量 `STREAM_STUDIO_STREAM_TIMEOUT_MS`（毫秒，≥0 的有限数）覆盖，**0 表示不限**，无效值回落缺省；引擎本身没有内部看门狗。超时后置 `timedOut`、广播错误日志、结束整个进程树，`close` 事件把任务终态定为 `failed`，错误信息“运行超过 N 分钟，已终止”；定时器 `unref`，不阻止服务退出（`:42-45`，`:215-226`，`:247-250`）。
- **进程关闭**：`stopped`（用户停止）优先；否则退出码在 `successCodes` 中为 `completed`，否则 `failed`。`error` 事件且无 pid（启动失败）直接 `failed`（`:242-252`）。

#### 5.9.4 收尾、停止与关闭

`finish(job, status, code, error)`（幂等）：① 若有 manifest，写 `status/exitCode/finishedAt/products`（仅 `completed` 才列产品），失败时附 `error`；② 调 `collect(status)`——图像任务读取 `scene_summary.json` 并返回 `{products: 完整路径数组, summary}`，登记任务失败时删除索引与元数据副本；③ `cleanup`；④ 有错误则广播 `job-log(level=error)`；⑤ 广播 `job-closed`（含 `status/code/elapsed/result`）；⑥ 释放 `active` 并 `pump` 下一个（`:185-199`，`:326-330`，`:473-484`）。

`stop(id)`：`id` 须为整数；排队中的任务直接移出并广播 `job-closed(stopped)`；已激活但尚未 spawn 的只置 `stopped` 标记，`launch` 随即以 `stopped` 收尾；运行中的置 `stopped` 并杀进程树（Windows `taskkill /pid <pid> /T /F`，失败退回 `child.kill()`；其他平台 SIGTERM）；找不到则返回 `{stopped:false}`（`:258-280`）。

`shutdown()`（服务退出）：清空队列并 `cleanup`；对当前任务，置 `stopped`，**同步**把 manifest 写成 `status: stopped, exitCode: null, products: []`（进程随即退出，等不到 `close` 事件），再杀进程树（`:718-730`）。

#### 5.9.5 SSE 事件

`GET /api/stream/events` 建立 `text/event-stream`，先推一条 `{type:'status', …}`，之后广播以下事件（`data:` 为 JSON）（`server/stream/index.mjs:100-103`，`:702-709`）：

| `type` | 触发 | 载荷要点 |
| --- | --- | --- |
| `status` | 连接建立 | `status()` 内容 |
| `job-queued` / `job-started` | 入队 / spawn | `job` 摘要 |
| `job-progress` | 进度行 | `job.progress`、`job.stage` |
| `job-log` | 日志行 / 警告 / 错误 | `text`，`level`（`warning`、`error`、`diagnostic`，普通日志无 `level`） |
| `job-closed` | 终态 | `status`、`code`、`elapsed`（ms）、`result` |

任务摘要字段：`id, kind, label, status, progress, stage, outputDir, pid, startedAt`（`server/stream/index.mjs:118-119`）。

### 5.10 /api/stream/* 路由

所有 STREAM 路由由 `handle` 分发，成功返回 200 JSON，任何异常统一返回 **400** `{error: 消息}`；路由不匹配返回 `false`，交由 `server.mjs` 继续处理；GET 路由请求体按 `{}` 处理（`server/stream/index.mjs:700-714`，`server.mjs:4491`）。

| 方法 路径 | 请求体要点 | 响应要点 |
| --- | --- | --- |
| `GET /status` | — | `{executable, executableExists, dataReady, defaultOutputRoot, era5Directory, forcingMetadataDirectory, running, queue[]}`（`:123-131`） |
| `GET /events` | — | SSE，见 5.9.5 |
| `POST /run` | `scene`（或请求体本身即 scene）；`outputRoot`、`name` 可置于 scene 或顶层 | 任务摘要（`status: queued`），有未知 `energy` 字段时附 `warnings[]`；提交期校验失败返回 400（`:287-333`） |
| `POST /stop` | `{id}` | `{stopped: true/false}` |
| `POST /inspect` | `{paths[]}`（≤32） | `{rasters[]}` |
| `POST /raster` | `{path, band}`（0 起） | 5.8.4 的 `rasterBand` 结果；大图解码由 `runTask('stream-raster')` 放到后台线程，未注入时同步执行（后台线程实现 `heavyTasks` 在 `server.mjs`，本章未核对：**待核实**） |
| `POST /runs` | `{outputRoot}`（缺省默认输出根） | `{runs[]}`：含 `run-manifest.json` 的子目录，字段 `outputDir/name/status/startedAt/mode`，按开始时间倒序，最多 50 条 |
| `POST /run-info` | `{outputDir}` | `{outputDir, runId, status, startedAt, finishedAt, mode, overpass, typeFile, typeBand, products[]（完整路径，仅 completed）, summary, config}`；缺目录/缺 manifest/损坏给出面向用户的中文错误；兼容旧 `stream-scene-run` 与 `scene-config.json`（`:338-364`） |
| `POST /class-stats` | `{outputDir}` | 5.8.4 的 `classStatistics`，同样经 `runTask('stream-class-stats')` |
| `POST /project/open` | `{path}`（文件或目录） | `{path, name, description, migrated, scene}`；不是 STREAM 工程时 400 |
| `POST /project/save` | `{path\|directory, scene, name, description, overwrite}` | 保存后重新读取的工程 |
| `POST /era5/list` | `{include[]}`（工程指向的其他位置索引） | `{packs[], directory}`；已登记在前（`external=false`），再按起始时间新→旧、标签排序；解析失败的索引被忽略（`:387-418`） |
| `POST /era5/register`、`POST /forcing/register` | `{files[], metadata\|product, label, projectPath}` | 入队任务摘要（kind = `era5`），成功时 `job-closed.result.pack` 为索引摘要 |
| `POST /era5/remove` | `{path}` | `{removed, path}`；只允许移除登记目录内的索引，同时删除其 `metadata/` 下的副本 |
| `GET /forcing/presets` | — | `{presets[], builtin[]}`：调 `stream forcing-preset`（15 s 超时），只保留服务端认识的名字 |
| `POST /forcing/preset` | `{name}` | `{name, metadata}`（进程内缓存） |
| `POST /forcing/check` | `{metadata, pack, files[], projectPath}` | `{ok, errors, warnings, notes, resolved, form}` |
| `POST /forcing/metadata/save` | `{projectPath, metadata, fileName}` | `{path, bytes}`；文件名须形如 `forcing-metadata*.json`，原子写入 |
| `POST /forcing/metadata/read` | `{projectPath, fileName\|path}` 或 `{import:true, path}` | `{exists, path, metadata}`；`import` 可读任意位置文件（仅回显给编辑器），否则只允许工程目录/数据包目录/元数据目录内 |
| `POST /pick` | `{kind}`：`file/folder/project/era5/station/metadata` | `{path, paths[]}` 或 `{cancelled:true}`；仅 Windows（PowerShell `-STA` 对话框），无超时、不限并发（源码已标 DEFERRED） |

（`server/stream/index.mjs:658-696`，`:507-617`，`:622-653`）。

### 5.11 实现差异与使用注意

1. **波段号语义**：服务端注释写“0 起”，引擎按 1 起解释；`/api/stream/raster` 的 `band` 为 0 起。配置里 `band=0` 或缺省都取第 1 波段（`server/stream/config.mjs:272-276`，`models/stream/scene/raster.cpp:151`，`server/stream/rasters.mjs:123`）。
2. **温度范围三层**：服务端对 RT 常数组分要求 150–400 K；引擎读入后把 100–400 K 以外的值置 NaN（栅格不受服务端常数检查约束，误填 ℃ 只会体现为 `thermalFailedPixels` 增多，源码已标 DEFERRED）；EB 求解结果被限幅在 175–380 K，反演/同化产品另有 200–380 K 的合理范围（`image.cpp:1175-1178`，`energy.h:50-51`，`inversion.cpp:46`）。
3. **土壤水分两套限幅**：EB+强迫 0–1，用户来源 0.05–0.55（且 >1 视为百分数），后者同样传入 EB 的 Sellers 土壤阻抗与 Johansen 热参数（源码已标为遗留问题）（`image.cpp:454-460`）。
4. **提交期与运行期校验的先后**：服务端提交时与出队时各调用一次 `buildEngineConfig`；出队时失败不会留下空输出目录（先校验后 `mkdir`）。引擎的网格对齐、区域越界、气象覆盖不足、起转不可用等检查只能在运行期发生，表现为 `failed` 与 stderr 中文错误（`server/stream/index.mjs:298-315`）。
5. **登记退出码**：`era5-register` 必需变量不连续时退出 3 且写出索引，但服务端只认 0，会删除该索引；`packSummary` 对 `missingVariables`/`incompleteVariables` 的宽松处理（排除 `skt/stl1/stl2`）只对外部提供的旧索引有意义（`server/stream/index.mjs:387-405`，`:473-480`）。
6. **产品只在成功时列出**：失败、超时、停止的运行 `products` 为空，输出目录中可能仍有半成品 `.tif`，不应被使用。
7. **GLDAS 预设与 `station-csv` 的高度**：GLDAS 预设未经真实数据验证；`station-csv` 不写观测高度，运行时按缺省高度并警告，应在元数据里补 `height_m`（`docs/forcing-metadata.md:149`，`:147`）。
8. **粗时间分辨率强迫**：`spinupHours` 以小时计，与强迫步长无关；3 h 数据时 7 个节点只跨约 2–3 个强迫时次，建议调到 ≥12（`models/stream/README.md:147`）。

## 6. 结果数据、前端运行机制与质量保障

本章说明三件事：引擎与服务端写出什么结果、如何读回；浏览器端如何驱动一次运行并展示状态；以及如何用测试、回归与检查清单判断一次运行是否可信。物理方程见《理论手册》，界面操作见《用户手册》。除特别说明外，文件路径均相对仓库根目录，`文件:行号` 指当前代码位置；无法在代码中确认者标“待核实”。

### 6.1 结果数据

#### 6.1.1 运行目录与产物清单

每次 `/api/run` 在 `<outDir>/runs/<runId>` 下建立隔离目录，`runId` = ISO 时间戳（`:` `.` 换为 `-`）+ 8 位随机串（`server.mjs:329-333`）。目录内固定文件：

| 文件 | 内容 | 依据 |
|---|---|---|
| `run-manifest.json` | kind=`histream-run`，含请求节点/视角、输入与引擎 SHA-256、状态、`completed.products` | `server.mjs:356-370` |
| `source-project.json` | 源工程副本 | `server.mjs:379` |
| `input/` | 冻结的运行工程、裁切后的 OBJ、`meteo.txt` 快照 | `server.mjs:5006-5026` |
| 产物 | TIFF、统计 CSV、面元/过程 JSON 与 `.bin`、`process/`、`.facet_steps/` | `server.mjs:310-320` |

`run-manifest.json` 每次改动都先写 `.tmp` 再改名，保证读者看到完整文件（`server.mjs:284-291`）。出图前会复核冻结工程与气象快照的 SHA-256，不一致则拒绝生成影像（`server.mjs:296-306`）。`completed.products` 来自 `runProductPaths`：只扫描运行目录、`process/`、`.facet_steps/` 三处的普通文件，排除 manifest、`source-project.json` 和 `.histream-transient-*`（`server.mjs:310-320`）。manifest 同时记录引擎旁 `build-manifest.json` 的文件身份、应用版本与 `sourceTreeSha256`（`server.mjs:371-377`）。

运行终态 `status` 的判定顺序：超时 `timeout` > 用户停止 `stopped` > FacetEB 退出码 2 `non_converged` > 退出码 0 `completed` > 其余 `failed`（`server.mjs:5264-5266`）。退出码表 `{0,1,2,3,4,5,64}`，仅 FacetEB 把 2 视为“结果保留”，其余模式仅 0 保留（`server.mjs:119-121`）。引擎 `main.cpp` 中可见 64、1、0、5、4、3（`models/histream/src/main.cpp:56-175`），退出码 2 的引擎侧来源待核实。

引擎退出后的收尾（`completeHistreamRun`，`server.mjs:5192-5277`）按固定顺序执行，期间 `child` 仍指向本次运行，界面保持“运行中”，并拒绝新的运行与结果删除：

1. 被“重置”终止：只写 `stopped` 并清理，不做后处理。
2. 面元模式且没有逐节点出图：在一次性后台线程把面元 JSON 转 TIFF；结果文件在出图后删除，除非启用了过程输出或 FacetEB 含光伏材质（此时保留 `facetrt.json`/`faceteb.json`，否则写为 `.histream-transient-<mode>.json`，`5047-5061,5234`）。
3. 非面元模式：ENVI 转 TIFF（见 6.1.3）。
4. 汇总节点完成情况：引擎自写的 `faceteb_run.json` 的 `completed` 优先；没有它、退出码为 0 且不是逐节点握手的运行，把请求节点全部记为“由退出码确认完成”（`completionSource: engine-exit`）（`5256-5263`）。
5. 写最终 manifest 与 `closed` 事件，释放运行时场景。后处理抛出的任何异常都会补发 `closed`（`failed`）并把仍为 `running` 的 manifest 改为 `failed`，避免界面永久“运行中”（`5279-5300`）。

FacetEB 使用逐节点握手：引擎每算完一个节点在 stdout 输出 `FACET_STEP`（含节点、`DOYddd_hh-mm` 标记、儒略时间、可选太阳角）并阻塞等待 stdin 的 `FACET_ACK`；服务端暂停超时计时、复核冻结输入、逐观测方向写 TIFF、更新 manifest，再回 ACK；出图失败则写 `FACET_ERROR`，引擎中止，已写出的节点文件保留，运行记为 `failed`（`5072-5178`）。因此“单次运行上限”只统计引擎自身执行时间（缺省 15 分钟，`control.runTimeoutMinutes` 1–1440 覆盖；到点在 Windows 上 `taskkill /T /F`，失败回退 `SIGKILL`，`106-111,5085-5105`）。引擎 stdout 的 `FACET_CONVERGENCE` 行（节点、是否收敛、迭代数、温度变化、能量残差、等效温度残差）被写入 `completed.nodes`（`5140-5143,5164`）。

#### 6.1.2 TIFF 写出（writeFloatTiff）

输入为波段顺序（BSQ）数组 `values[band*W*H+pixel]`，输出经典 TIFF（`II` 小端、魔数 42，无 BigTIFF），32 位 IEEE 浮点、无压缩、单条带、像素交错（`server.mjs:3128-3240`）。要点：

- 校验：宽、高、波段数须为正整数，数据长度不得小于 W×H×bands，否则抛错（`server.mjs:3129-3131`）。文件含数据区不得超过 4 GiB，否则报“TIFF 超过 4 GiB”（`server.mjs:3178`）。
- 标签：256/257 宽高、258 位深=32、259 压缩=1、262 光度=1、270 描述、273/279 条带、277 每像素样本、278 RowsPerStrip=高、284 平面配置=1（交错）、339 样本格式=3；多波段加 338 ExtraSamples（全 0，避免被当作 alpha）；有太阳角时加 42112（`server.mjs:3135,3189-3199`）。
- 波段名：写入 ImageDescription，格式 `HiStream image bands: {名1, 名2, …}`，以 **UTF-8** 编码，避免中文“温度 [K]”被 ASCII 截断（`server.mjs:3161-3166`）。读取端用正则 `(band names|image bands|metrics)\s*[:=]\s*{…}` 还原（`server.mjs:4044-4046`）；缺名时 3 波段 RGB 用“红/绿/蓝”，其余为“波段 n”。
- GDAL_METADATA（tag 42112）：仅当 `solarZenith`、`solarAzimuth` 均为有限数才写，内容为 `HISTREAM_SOLAR_ZENITH_DEG` 与 `HISTREAM_SOLAR_AZIMUTH_DEG` 两个 Item，ASCII 编码（`server.mjs:3133,3167-3172`）。
- 原子性：先写 `<path>.partial`（64 KiB 分块把 BSQ 转交错，非有限值写 NaN），写完再改名发布；失败时删除 `.partial`（`server.mjs:3205-3237`）。随后立即写统计 CSV（`server.mjs:3238`）。

输出文件名：EB 模式 `T=DOYddd_HH-MM_VZA=xx.xx_VAA=xx.xx`，RT 模式 `SZA=…_SAA=…_VZA=…_VAA=…`，再加几何后缀（面元 `_f`、体元 `_v`、其他无）与大气标记 `_a`，角度保留 2 位小数、非有限写 `0.00`（`server.mjs:1409-1413,1439-1442,1469-1479`）。面元影像的波段名：EB 且勾选“输出温度”时只有 1 个波段“温度 [K]”，否则每个波长一个波段，波长 ≤2500 nm（≤2.5 视为 µm）命名 `Reflectance [-] @ λ nm`，其余命名 `Spectral radiance [W m-2 sr-1 um-1] @ λ nm`（`server.mjs:1654-1668`）。

#### 6.1.3 ENVI 转 TIFF

体元（VoxelRT/VoxelEB）与 RayTracing 的引擎输出是 ENVI（`.hdr`+`.img`），运行结束后在一次性线程中转换（`server.mjs:5241-5254`）。面元模式不经 ENVI，直接由面元 JSON 出图（`server.mjs:3268-3270`）。转换器 `convertEnviToTiff` 的限制，任一不满足即抛带原因的错误（`server.mjs:3274-3290`）：

| 项 | 要求 |
|---|---|
| samples/lines/bands | 正整数 |
| data type | 仅 4（float32，缺省 4） |
| byte order | 仅 0（小端） |
| interleave | 仅 bsq（缺省 bsq） |
| header offset | 仅 0 |
| `.img` 大小 | 不小于 W×H×bands×4 |

`.hdr` 解析只取 `key = value` 的同一行，`band names={a,b}` 无空白写法也能匹配；多行花括号只取首行（`server.mjs:3639-3643`，测试 `tools/test-server-fixes-20261125.mjs:32-56`）。目录筛选：EB 模式只转文件名含 `T=` 的头文件，辐射模式只转不含者（`server.mjs:3323-3333`）。成功后仅 `eVoxelRT/eVoxelEB/eRaytracing` 删除 ENVI 原文件，失败则原文件保留（`server.mjs:3312-3315`）。单个文件失败记入 manifest `warnings` 与 `closed.warnings`；**全部**失败才把运行记为失败（`server.mjs:5247-5253`）。预览读取器 `readEnvi` 支持 bsq/bil/bip 及类型 1/2/3/4/5/12/13（`server.mjs:3645-3661,4423-4476`）。

#### 6.1.4 TIFF 读取（readTiff）

读取器是前端“结果/地形”预览的核心（`server.mjs:3859-4157`）：

| 能力 | 范围 |
|---|---|
| 容器 | 经典 TIFF，`II`/`MM`；BigTIFF 报“暂不支持”（`3863-3868`）；只解析第一个 IFD |
| 压缩 | 1 无压缩、5 LZW、8/32946 Deflate、32773 PackBits，其他报错（`3940`） |
| 分块 | 条带或瓦片；planar 1/2；Orientation 1–8（`3939-3947,4027-4039`） |
| 位深/格式 | 位深 1/8/16/32/64，样本格式 1/2/3；每像素样本 ≤1024（`3951-3954`） |
| predictor | 2（8/16/32 位整型）、3（浮点 32/64）（`3715-3759`） |
| NoData | 标签 42113；float32 影像用 `Math.fround` 比较（`3919-3923`） |

解压防护：LZW、PackBits 与 Deflate 的输出长度均不得超过单分块上限 `maxChunkBytes = 分块宽×高×样本数×最大字节数`，Deflate 通过 `maxOutputLength`，LZW/PackBits 在累计超限时抛错（`3961-3982`）。**写出端永远不压缩**，上述压缩只服务于外部输入（DEM、STREAM 产品等）。

返回对象：尺寸、波段名、min/max/mean（全部有效像元，排除 NaN 与 NoData）、预览 `pixels`（最长边 ≤768，最近邻，base64 灰度）、拉伸范围、地理参考（像元大小、左上角原点 = 连接点 − 像素坐标×像元大小）、CRS 与单位、≤96 像素的粗地形网格 `terrainValues`（缺测为 null）（`4069-4156`）。拉伸为 2%–98% 分位；热红外波段（名称含 temperature/热红外/thermal，或 @ 波长 ≥3000 nm）把 ≤0 视为无效，若零值占比 ≥1% 且有效样本 ≥64，则在排序后 30%–92% 位置找最大间隙分离“地表/天空平台”，间隙须 ≥ max(2, 0.12×P10–P90)，并生成高温热点叠加（`3791-3823,4087-4117`；间隙阈值的单位按 K 设计，待核实）。拉伸只影响显示，不改变 TIFF 数值。风场 TIFF（文件名 `fluid_wind_` 开头）要求 4 波段：0 风速、1 X、2 垂直、3 Y，约每行 28 支箭头（`4162-4194`）。

#### 6.1.5 统计 CSV

`writeRasterStatistics` 在每次 `writeFloatTiff` 之后执行，写同目录 `result_statistics_<mode小写>.csv`（`server.mjs:3076-3118`）。模式标签：优先取引擎模式（FacetRT/FacetEB/VoxelRT/VoxelEB/Raytracing），否则按文件名 `_f/_v`（含 `_a`）与是否 `T=` 开头推断（`3065-3083`）。

- 表头固定：`file,mode,time,vza,vaa,sza,saa,band_index,band_name,count,min,max,mean,stddev`；所有文本单元格强制加双引号并按 RFC 4180 转义（`3061,3110`）。
- 每个波段一行；`band_index` 自 0 起；`count` 为有限值个数，NaN/Inf 不计入；均值与方差用 Welford 在线算法；`stddev` 为总体标准差（除以 count）；count=0 时 min/max/mean/stddev 为空（`3088-3105`）。
- 同名文件重新导出时，旧行被替换而不是追加；表头与当前不一致（旧格式或损坏）则丢弃旧内容重建；先写 `.tmp` 再改名（`3107-3116`）。
- 时间、VZA、VAA 取自文件名标记（`filenameToken`，不区分大小写，遇 `_H= _T= _SZA= …` 或 `_F/_V/_A` 截止）（`3245-3259`）。
- 读取时 `/api/results/read` 对该 CSV 额外返回 `observation`：优先 manifest 的 `requested.observation`，否则从 `input/project.json` 或 `source-project.json` 推导主平面方位等（`3521-3542,5422-5424`）。

#### 6.1.6 面元、体元与过程结果

**面元结果**（`facetrt.json`/`faceteb.json`，`server/facet-result.mjs`）有两种格式，两者返回完全相同的结果形状：

| 格式 | 判定 | 要点 |
|---|---|---|
| JSON | 头部 64 KiB 含内联 `vertexPositions` 数组 | 手工在 Buffer 上逐数字解析，不转成 JS 字符串（避开 V8 字符串长度上限）；数组须按固定顺序出现 |
| binary | 头部含 `"format":"binary"`，或元数据中描述符 | 元数据 JSON（≤64 MiB）+ 小端 float32 旁路 `.bin`，起点 16 字节对齐 |

（`facet-result.mjs:52-190,319-350`；格式规范见 `docs/facet-binary-output.md`。）Stream Studio 的工程缺省写 `binary`，引擎自身缺省仍为 `json`（`docs/facet-binary-output.md:16`）。

二进制读取的约束：`formatVersion` 大于 1 报“请升级”；描述符 `dtype` 只支持 `float32`；本机必须小端；`shape` 乘积须等于 `length`；`offset+length×4` 不得超出旁路文件；旁路文件名必须是与 JSON 同目录的纯文件名（拒绝路径分隔符、`:`、`.`/`..`）（`facet-result.mjs:354-373,450-462`）。单文件（JSON，或二进制中的单个数组）超过 `MAX_RESULT_BYTES`（缺省 8 GiB，环境变量 `STREAM_STUDIO_MAX_RESULT_BYTES`）拒绝读取（`16,23-30,490`）。指标集合：`radiosity/lightEnhancement/sunlit` 必需，`temperature/netRadiation/sensibleHeat/latentHeat/storageHeat/photovoltaicPower` 仅在存在时输出；每面元有正反两面，指标长度为 2×面元数（`145-176`）。三维显示抽样上限 1 000 000 个面元，抽样序号 `floor(i·N/n)`，磁盘结果保留全部面元（`18,197-209,593-601`）。旁路文件管理：`facetSidecarFiles` 列出、`facetResultBytes` 计总大小、`removeFacetResult` 先删旁路后删 JSON（`376-419`）。

**过程结果**（`*-process` JSON + 二进制）由 `processResultMetadata` 识别：JSON ≤1 MiB 且 `kind` 以 `-process` 结尾（`server.mjs:1483-1491`）。元数据给出 `dataFile`、`recordFloats`、`fields[{id,label,offset}]`（偏移须 <recordFloats）、`geometry`（facet|voxel）与元素数。面元：每面元 2 条记录（正反面），几何取自 `geometryFile`（缺省 `../faceteb.json`），数据不足 `facetCount×2×recordBytes` 报“不完整”；体元：每体元 1 条记录，`positionOffsets` 缺省 `[0,1,2]`，可选 `soilProfile`（`layerCount/offsets/depths`，单位缺省 degC 与 m）；体元显示上限同为 1 000 000（`1502-1613`）。

#### 6.1.7 结果列表、读取、变化分析与删除

| 接口 | 行为 | 失败 |
|---|---|---|
| `POST /api/results/list` `{path}` | 先 `resultDirectoryAllowed`，再在后台线程 `listResults`：收集本目录与 `process/` 下 `.tif/.tiff/.json/.csv`，分类 `tiff/text/facet/process`（`file` 丢弃），并递归 `runs/<runId>`（不跟随符号链接），按修改时间降序（`server.mjs:5377-5387,3551-3634`） | 400 `只能列出当前工程输出目录或已识别运行目录中的结果` |
| `POST /api/results/read` `{path,band}` | 文件须存在且通过 `resultFilePathAllowed`；按扩展名分派：`.hdr`→ENVI，`.tif(f)`→TIFF（`fluid_wind_`→风场），面元 `.json`→facet，过程 `.json`→process，图像（≤32 MB）→base64，`.txt/.csv/.dat/.log/.json`（≤4 MB）→文本，其余只回路径（`5394-5430`） | 400 + `error` |
| `POST /api/results/change` `{firstPath,secondPath,band,maxScatterPoints}` | 两个路径均须存在、通过路径检查且为 `.tif/.tiff`；在后台线程比较，以第一幅路径作串行键（`5432-5447`） | 400 |
| `POST /api/results/delete-all` `{path,kind}` | 运行中（`child` 非空）拒绝；先清空结果缓存并广播 `clear`；`kind=3d` 删三维结果及其 `dataFile`/旁路，否则仅删 `.tif/.tiff` 与 `result_statistics*.csv`（`5365-5375`） | 400 |

注意：删除接口路径是 `/api/results/delete-all`，不是 `/delete`。

**允许范围。** `resultDirectoryAllowed(dir)`（`3013-3029`）按顺序判断：①目录与任一输入文件相交，或工程输入目录（`models/positions/spectral/meteorology/terrain/images/textures`）位于目录内 → 拒绝；②正在运行的输出目录 → 拒绝；③等于工程输出根 → 允许；④含有效 `run-manifest.json` → 允许（kind 须为 `histream-run/stream-image-run/stream-scene-run`，且 `outputDir` 解析后等于该目录，或无 `outputDir` 时 `runId` 等于目录名，`2979-2987`）；⑤含 `faceteb.json/facetrt.json/scene_summary.json` → 允许；⑥`runs/` 下含有效运行目录（`runs` 为符号链接则拒绝）→ 允许。`resultFilePathAllowed(file)`（`3036-3047`）：位于输出根内（含符号链接真实路径检查），或其目录、或其父目录含有效 manifest，或其目录含上述三个产物文件之一。注意文件级检查**不排除**正在运行的目录，且不看工程输入。测试覆盖：任意绝对路径读取返回 400（`tools/test-review-fixes-20261126.mjs:59-77`）。删除只作用于 manifest 的 `products` 列表内、位于本运行目录、且不与任何输入快照重合的文件；找不到任何输入快照时整次运行都不删，快照损坏则抛错（`3406-3462`）。

**变化分析**（`compareTiffResults`，`4204-4304`）：B−A；尺寸须一致，两幅均带地理参考时 CRS、原点、像元大小须一致（容差 1e-8）；波段号不得超过两者共同波段数；任一方非有限的像元不参与，无共同有效像元则报错。统计量：均值差、MAE、RMSE、最值、2%/98% 分位、相关系数（方差为 0 时为 null）、散点（点数夹在 100–10000，缺省 4000）、差值预览（蓝负、白零、红正，按分位限幅线性着色）。

#### 6.1.8 结果缓存键与二进制响应

`/api/results/read` 的响应由 `sendCachedResult` 缓存，键为 `result:<variant>|<binary|json>|<各依赖文件的 fileStamp.key>`，`variant` 如 `tiff:<band>`、`envi:<band>`、`wind`、`facet`、`process`（`server.mjs:480-515`）。`fileStamp.key = 绝对路径|mtimeMs|size|ctimeMs`，其中 `ctimeMs` 是 2026-11-24 加入的，防止保留 mtime 与大小的原地改写（`result-cache.mjs:45-55`）。依赖文件：ENVI = `.hdr`+`.img`；面元 = JSON+旁路；过程 = 元数据+数据文件，面元过程再加几何文件及其旁路（`518-526`）。任一依赖不存在则不缓存；读取期间文件身份变化也不缓存；同键并发请求合并（`484-503`）。

缓存为按字节计费的 LRU：缺省 512 MB，`STREAM_STUDIO_RESULT_CACHE_MB` 调整，≤0 或非数值关闭（`result-cache.mjs:37-41`）。同一路径出现新身份时，依赖旧身份的条目一并作废；单条超过上限不存；`resize` 超限时淘汰最旧项，最旧项就是自身且为唯一条目时自删（`80-113`，测试 `tools/test-review-fixes-20261126.mjs:26-33`）。服务线程缓存已编码响应，每个后台线程缓存原始文件与已解码分块，预算为总量÷线程数；一次性后处理线程不缓存（`server.mjs:149-167,5637-5641`）。请求头 `Accept` 含 `application/vnd.stream-studio.result-v1` 时，`vertexPositions/voxelPositions/values` 三个键下长度 ≥256 的数值数组被抽成 Float32（所有值可精确表示）或 Float64 块；格式为魔数 `SSR1`、uint32 头长、头 JSON、8 字节对齐数据区；响应带 `X-Cache: hit|miss` 与 `Cache-Control: no-store`（`result-cache.mjs:132-207`，`web-api.js:23-42`）。

#### 6.1.9 结果读取的失败行为汇总

| 场景 | 行为 |
|---|---|
| 路径不存在或不是文件 | 400 `找不到模拟结果` |
| 路径不在允许范围 | 400 `只能读取…` / `只能列出…` / `只能比较…` |
| TIFF 头损坏、BigTIFF、不支持的压缩/位深/predictor | 400，消息含具体原因（如 `暂不支持 TIFF compression n`） |
| 分块偏移越界、解压超过分块上限 | 400 `TIFF 图像数据分块越界` / `…解压结果超过分块上限` |
| ENVI 缺 `.img` 或数据不足 | 400 `ENVI 数据不完整：需要 … 字节` |
| 面元 JSON/旁路缺失、数组长度不符 | 400 `缺少面元结果旁路文件` / `… 数据不完整` |
| 单个结果文件超过 `MAX_RESULT_BYTES` | 400，提示减少面元数量或简化 OBJ |
| 图像 >32 MB、文本 >4 MB | 400，提示改用资源管理器/外部程序 |
| 读取期间文件被改写 | 响应照常返回，但不进入缓存，下次重读 |

所有读取接口的错误统一为 `{ error }`，前端把它显示在结果状态栏与预览区（`main.js:8431-8435`）。

#### 6.1.10 runWarnings 质量提示的产生规则

当前唯一的规则（`server.mjs:1123-1165,5213-5217`）：

1. 触发条件：本次为面元模式（有 `radiosityJsonPath`），且引擎退出码属于“保留结果”集合（`resultCodes`）。必须在面元 JSON 被清理之前读取。
2. 取值：`faceteb_run.json` 中递归（深度 ≤6）查找键 `zeroVisibilitySamples` 的最大值；同时在结果 JSON 的首、尾各 256 KiB（文件 ≤512 KiB 时整读）用正则匹配该键，取全部来源的最大值。
3. 判定：值 **严格大于 0.05**（接收面中没有任何可见性样本的占比）才产生一条提示，文字含百分比并建议提高可见性栅格边长或改用体元模式；字段缺失（旧引擎）返回空数组。
4. 传播：写 stdout `提示：…`、manifest `runWarnings`、`closed` 事件 `runWarnings`；前端写“结果质量提示”日志，成功时再弹 `warning` 提示。不改变 `status`（`main.js:6688-6689`）。

其他质量信号与此独立：ENVI 转换失败进 `warnings`；运行前预检 `warnings`（见 6.3.5）；前端对引擎诊断行的汇总（见 6.2.4）。引擎写入 `zeroVisibilitySamples` 的位置待核实（引擎侧未读）。

### 6.2 前端运行机制

#### 6.2.1 状态与脏标记

渲染进程是原生 JS + Three.js 的单页应用，核心为 `main.js` 中的单例 `state`（`main.js:111-152`）：

| 字段 | 含义 |
|---|---|
| `mode` / `config` / `project` | 计算模式；工程配置（`project.configuration` 的引用）；工程对象 |
| `inputPath` / `projectPath` / `xmlText` | 工程文件路径；序列化文本（字段名沿用 XML 时代，现为 `project.json`） |
| `xmlDirty` / `editRevision` | 未保存标记；编辑版本号，每次标脏加 1 |
| `projectGeneration` | 工程代数，换工程时加 1，用于丢弃过期异步结果 |
| `running` / `runPending` / `progress` / `progressStage` | 运行状态与进度 |
| `resultFiles` / `selectedResult` / `resultView` / `resultOpenRequest` | 结果浏览状态；请求序号用于丢弃过期读取 |

数据流：界面输入 → 修改 `state.config` → `syncProjectText()`（规范化并重新序列化，`normalizeProject`+`stringifyProject`，标脏，显示“有未保存的修改”）→ `refreshFromState` 重建三维预览（`main.js:609-628`）。连续输入用 `scheduleProjectTextSync` 防抖 250 ms（`633-638`）。JSON 文本框手改时只置 `xmlManualEdit` 与标脏；保存时才重新解析并替换 `state.config`（`9231,3714-3716`）。

#### 6.2.2 工程打开、保存与另存为

- **打开** `loadXml(result, confirm=true)`：先 `confirmDiscardChanges`（`xmlDirty` 为真则 `window.confirm`），STREAM 工程转交 STREAM 工作台；运行中拒绝；解析规范化，按文件名/名称恢复对象的 `distribution` 与 `instanceCount`，`projectGeneration` 加 1，清 `xmlDirty`，重建预览（`main.js:3490-3532`）。新建工程同样先确认，再以 `confirm=false` 载入（`8854-8863`）。
- **保存** `saveXml()`：先让当前数字框 blur 提交并冲刷待同步文本；记录 `generation/revision/路径/文本/模式` 快照；对配置做深拷贝后提交 `/api/project/save`。返回后：若工程已切换，只写日志并返回 `null`；若保存期间 `editRevision` 变化，磁盘是提交时快照，界面保持“未保存”；否则清 `xmlDirty`（`3695-3760`）。返回值 `true/false/null` 中，`null` 表示被取代，调用方须跳过刷新与回滚。
- **服务端** `/api/project/save`：保存目标必须在当前工程目录内（否则提示改用“另存为”），经 `validateProject` 校验，写盘后同步导出 `project.usd`（失败只在响应 `usd` 字段报告）（`server.mjs:4905-4931`）。
- **另存为** `saveProjectAs`：同样记录快照，复制到新目录后以 `loadXml(result)` 载入；若期间工程或版本已变化，只记日志，保持当前工程（`main.js:8822-8851`）。该路径调用 `loadXml` 时使用缺省 `confirm=true`，而此前未清 `xmlDirty`，因此脏工程另存后可能再弹一次放弃确认（由代码推断，待核实实际交互）。

#### 6.2.3 一次运行的前端流程

`runSimulation`（`F5`）：无工程拒绝；`state.running` 或 `state.runPending` 直接返回（防重入）；检查镜面设置；`xmlDirty` 则先 `saveXml`；`runPreflightCheck`；再 `startSimulation`（`main.js:6313-6324`）。预检失败只写警告并放行；有风险提示时弹确认，预计耗时超限且有建议上限时三选一（调高并运行/按当前运行/取消）（`6337-6383`）。`startSimulation` 置进度 1%、`setRunning(true)`，调用 `/api/run`，失败则复位并区分“引擎不在白名单”（`6406-6420`）。运行中禁用运行按钮、模式切换、绘制/编辑与设备选项（`751-778`）；进度条只增不减（`743-747`），运行中每 250 ms 刷新耗时（`9240`）。停止调用 `/api/stop`；准备阶段（尚无子进程）只置取消标志，下一个 `await` 检查点抛“模拟准备已取消”（`server.mjs:5329-5341,4976-4978`）。

#### 6.2.4 SSE 事件处理

`webApi.onSimulationEvent` 用 `EventSource('/api/events')`，忽略无法解析的消息，只在 `init()` 订阅一次（`web-api.js:125-133`，`main.js:9278`）。服务端 `emit` 向所有连接广播 `data: <JSON>`；新连接立即收到一条 `status`（运行与否、PID、runId、outputDir、startedAt），用于重连后同步（`server.mjs:749-752,4625-4634`）。`handleSimulationEvent` 分支（`main.js:6616-6705`）：

| 事件 | 处理 |
|---|---|
| `status` | 服务端在跑而本地未跑 → 接管并 `setRunning(true)`；服务端已结束而本地仍在跑 → 提示“连接中断期间已结束”，要求刷新结果 |
| `started` | 记录 outputDir/runId，进度 1%，清空 `runQuality` 与 `stdoutCarry`，打印模式、温度方法、输出选项 |
| `stdout` | `engine:true` 的原始输出可能被任意切块，拼上 `stdoutCarry` 后只解析完整行；服务端自己的消息永远是整行 |
| `stderr`/`error` | 写错误日志 |
| `closed` | 先冲刷残余；按 `code===0` 判成功，`status==='non_converged'` 为“保留”，`stopped`/`timeout` 分别提示；输出 `warnings`、`runWarnings`；`setRunning(false)`；汇总运行质量；成功或未收敛时 300 ms 后自动打开结果 |

`normalizedEngineLog` 把 `PROGRESS n 阶段` 转为进度更新，`PROCESS/RESULT/OBSERVATION/Time Info` 转中文前缀，丢弃 `cube`/`OBJ placement` 行（`6467-6499`）。`recordRunQualityLine` 解析 `FACET_CONVERGENCE`、`FACET_TIMING`、`ENERGY_BALANCE`（含 MOST 诊断）、`SHORTWAVE_SCATTERING`（含太阳可见性截断）、`SHORTWAVE_SPLIT`、`VEHICLE_CABIN` 行；字段顺序依赖引擎输出，顺序变化会使解析静默失败（`6513-6561`）。`runQualitySummary` 在结束时给出：FacetEB 未收敛节点与建议、耗时构成、VoxelEB“x/50 迭代”与未达容差状态、射线截断提示（`6566-6607`）。其中 VoxelEB 迭代上限 50 在前端为字面量（`6589`），与引擎常量的一致性待核实。

STREAM 工作台使用独立通道 `/api/stream/events`，事件类型 `status/job-queued/job-started/job-progress/job-log/job-closed`；任务 id 返回前的事件缓存在 `pendingEvents`，随后回放；非当前任务事件忽略；`job-closed` 后读 `runInfo`，失败则用事件携带的结果兜底（`stream/api.js:40-48`，`stream/workspace.js:1412-1462,2328-2377`）。准备被取消显示“已取消”而非“未能启动”（`1453-1457`）。`EventSource` 无 `onerror` 处理，断线重连由浏览器默认完成，状态重同步依赖服务端补发的 `status`（`stream/api.js:41`）。

#### 6.2.5 结果浏览与接口层

结果对话框分“图像结果”与“分析”两视图（角度/波段/时间/变化/三维）。`openResultFile` 用递增的 `resultOpenRequest` 与 `stale()` 丢弃过期读取；`result.kind` 为 `envi/wind/tiff/facet/voxel/image/text` 分别绘制，其余提示用外部程序打开（`main.js:8393-8436`）。筛选：图像视图=`tiff/envi/image`；变化分析=TIFF；三维=`facet/process`（须启用过程输出）；其余分析=`result_statistics*.csv`（`8449-8454`）。

接口层：`http.js` 的 `request/post` 统一把网络失败、非 JSON 响应与 HTTP 错误转成中文 `Error`（服务端 `error` 字段优先）；成功响应的 `Content-Type` 以二进制类型开头时交给 `decodeBinaryResult`（`http.js:14-36`）。`webApi` 绝大多数为 `POST`+JSON，只有 `readResult` 带 `Accept` 请求二进制（`web-api.js:73-134`）。`streamApi` 对应 `/api/stream/*`（`stream/api.js:17-38`）。服务端对 `/api/*` 的请求安全：Host 须为环回地址；有 `Origin` 须环回；`Sec-Fetch-Site` 仅允许 `same-origin/none`；非 GET/HEAD 须 `application/json`；请求体上限 160 MB，超过 4 MB 的在后台线程解析（`server.mjs:757-805`）。**仍没有会话令牌**，同机其他进程可调用（`server.mjs:774-775`）。

#### 6.2.6 三维预览、i18n 与一致性约束

**scene-loader.js**：把 `project.json` 的对象加载成 Three.js 预览，只用于所见即所得，不参与计算（`scene-loader.js:1-4`）。约定：位置文件 x=北、y=东、z=高；Three.js 的 X←x、Z←y、Y 向上；整体按 `min(1, 45/max(scene.x, scene.y))` 缩放（`797`）。每个实例化物体预览最多绘制 5000 个实例，超出时等间隔抽样（`24,41-46`）。DEM 高度采样用双线性插值，未启用地形或栅格尺寸与数据长度不符时为 0，结果不小于 0（`721-741`）。随机游走（Human/Vehicle/Ship）只是预览动画，引擎不读取 `movement`（`763-765`）。

**i18n.js**：源文案为中文，切到 `en-US` 时就地翻译文本节点与 `title/placeholder/aria-label`，切回还原；`t()` 先查整句词典，再动态正则，再短语替换，无词条原样保留不报错；语言存于 `appStorage` 键 `streamStudioLanguage`，用 `MutationObserver` 翻译新增节点（`i18n.js:1-20,27-29,1054-1060,1129-1134,1156-1170`）。

**客户端限幅与服务端校验的关系**。三层依次是：①输入层 `bindValue` 的 converter（空串/非数值恢复上一次值，再限幅），②`normalizeProject` 的规范化限幅（如气象 `Tsold` 夹到 150–400、`year`、`timezone`），③`validateProject` 的拒绝性校验，在 `/api/run`、`/api/run/preflight`、`/api/project/save` 与 `resolveProjectFile`（旧格式）上调用（`server.mjs:946-948,4917,4941,4988`）。客户端限幅是 UX 层，不能代替服务端：

| 量 | 客户端 | 服务端 `validateProject` |
|---|---|---|
| 场景 x、y | ≥0.01（`main.js:3115`） | 须 >0（`project-schema.js:1497`） |
| 场景高度 | ≥0 | ≥0 |
| 体元尺寸 | ≥0.0001（`3117`） | >0（`1499`） |
| 传感器分辨率 | 取整，≥1（`3313`） | >0（`1507`） |
| 耦合迭代 | 1–100 取整（`3167`） | 在已读校验段未见（待核实） |
| 温度容差/松弛 | 0.001–100 / 0.05–1（`3182-3183`） | 在已读校验段未见（待核实） |
| 天空温度 | 0–1000 K（`3310`） | 在已读校验段未见（待核实） |
| 波段与观测角 | — | 至少一个有效波段与观测角（`1508-1509`） |

另外：高于当前 `PROJECT_SCHEMA_VERSION`（6）的工程，`parseProjectJson` 与 `validateProject` 均拒绝，避免保存时静默降级（`project-schema.js:27,1466,1486-1489`）；服务端对 `meteo.end−start` 的节点数另设上限 100000（`server.mjs:350-353`）。

### 6.3 质量保障与运维

#### 6.3.1 自动测试

`npm test` 只运行四个 `node:test` 文件，不需要引擎、GPU 或浏览器（`package.json:16`）：

| 文件 | 覆盖 |
|---|---|
| `tools/test-review-fixes-20261126.mjs` | `validateProject` 拒绝非正场景尺寸；`ByteLru.resize` 不超限；STREAM RT 转发 `diffuseFraction` 并拒绝 ℃ 组分温度；任意路径读取 `/api/results/read` 返回 400 |
| `tools/test-server-fixes-20261125.mjs` | ENVI 头解析与转 TIFF（波段名、Voxel 删除原件）；输出时间/角度标记与旧实现逐值一致；DEM 导入 64 MB 限制 |
| `tools/test-facet-binary-reader-20261126.mjs` | JSON 与二进制面元读取/抽样一致（含稀疏抽样）、旁路文件管理、损坏输入报错、`/api/results/read` 缓存因旁路改变而失效 |
| `tools/test-usd-roundtrip-20261125.mjs` | USD 导出→导入往返保持车辆与对象高度 |

`docs/` 中记录该集合为 21 项（`review_now/streamstudio-fixes-20261126/FIXES_2026-11-26.md` §2）。不在 `npm test` 内、需手动运行的脚本：STREAM 服务端（`test-stream-forcing-server.mjs`、`test-stream-server-{bugs,review,tools}-20261124.mjs`、`test-assimilation-config.mjs`）；调用真实 `stream.exe` 的 pytest（`test-stream-fixes-20261121.py`、`test-stream-{bugs,review}-20261124.py`、`test-assimilation-contract.py`、`test-forcing-metadata.py`、`test-stream-jacobian.py`）；面元/车辆（`test-vehicle-parts.mjs`、`test-vehicle-cabin.py`）；需编译的 C++ 自检（`test-facet-energy-convergence.cpp`、`test-facet-lumped-thermal.cpp`、`test-facet-nested-budget.cpp`、`test-facet-sparse-aerodynamics.cpp`、`test-lidf-gamma.cpp`、`test-voxel-limits.cpp` 需 Vulkan 设备、`test-diffuse-directions.cpp`、`test-facet-collatz.py`）。仓库中未见统一入口或 CI 配置运行这些脚本，是否在发布流程中执行待核实。

#### 6.3.2 回归测试（tools/regression）

流程：`prepare.mjs` 按 `/api/run` 流程冻结输入 → `run.mjs <workDir> <variant> [--engine <binDir>] [算例…]` 复制输入、改写 `outDir`/气象路径、应用 `REGRESSION_SET` 覆盖、启动引擎（`HISTREAM_FACET_STEP_SYNC=0`）、记录 `result.json`；超时（缺省 900 s，`REGRESSION_TIMEOUT`）杀进程，退出码记为 `'timeout'`（`run.mjs:57-85`）→ `compare.py <workDir> A B [--json] [--tolerance x]`。`run.mjs` 自身不判定通过与否（始终正常结束，缺参数以 64 退出）。

`compare.py` 判定规则（`compare.py:176-256`）：

- 仅比较两个变体**都存在**的算例；只在一边出现的算例使门禁失败。
- 参与门禁的量：TIFF 各波段均值；VoxelEB 各体元类型的 `T` 与 `Rn`；FacetRT 各波段均值。度量为 `|b−a|/max(|a|,1e-12)`，任一方缺失按 ∞（`relative_change`）。
- 仅打印不判定：FacetEB 收敛标志/迭代数/残差、VoxelEB 迭代数与未达容差状态、图边数、耗时、p99、`gt1` 计数；变化者标 `[CHANGED, not gated]` 并在末尾汇总（最多列 40 条）。
- 退出码：**未给 `--tolerance` 时恒为 0**（只报告）；给出 `--tolerance` 后，若有缺失算例、任一算例两侧引擎退出码不同、或最大相对变化 > 容差，打印 `FAIL` 并以 1 退出，否则 0。纯性能改动建议 `--tolerance 1e-4`。

使用注意：示例或 `prepare.mjs` 规范化变化后旧基线不可比；RayTracing 默认 Owen 置乱 Sobol 采样、VoxelRT/EB 默认跳过空节点，与旧基线比较需设 `HISTREAM_RT_SAMPLER=legacy`、`HISTREAM_VOXEL_SKIP=off` 才逐位相同（`tools/regression/README.md:36-38`）。

#### 6.3.3 check-* 与验收脚本

- `tools/check-facet-par-output.py <运行输出目录>`：校验 `process/facetpar_T=*.json` 的 PAR 过程文件：每记录 2 个 float、`surfaceCount = 2×facetCount`、字段 `incidentParPhotons/absorbedParPhotons`、`.bin` 大小 = `surfaceCount×8` 字节、数值有限且非明显负（容差 1e-5）、吸收不超过入射、夜间入射峰值接近 0；失败抛 `AssertionError`。
- `tools/check-user-release-cases.mjs <case-root> <新记录目录>`：对 4 个发行版 STREAM 示例各选 4×4 区域真实运行 `stream.exe`（超时 60 s），核对输入引用为案例内相对路径、记录 SHA-256、像元数 = 16、失败计数 = 0、EB 全部收敛、至少 2 个非空 TIFF；有失败退出 1。
- `tools/validate-obj-library.mjs`（`npm run validate:assets`）校验 OBJ 资产库。

#### 6.3.4 构建清单（build-manifest）

`tools/write-build-manifest.mjs [运行时目录]` 由 `build-engine.ps1` 在构建末尾调用，写 `models/bin_x64/<配置>/build-manifest.json`（`build-engine.ps1:90`）。内容：应用版本、`createdAt`、git `commit` 与 `dirty`（git 不可用时 `commit` 为空串、`dirty` 为 null，仅告警）、`sourceTreeSha256`（对源码清单求哈希，覆盖 histream/stream 源码与着色器、`src/renderer`、`server.mjs`、`server/`、若干 `tools/` 脚本与 `package.json`）、引擎与运行时二进制哈希，以及已部署 `.spv` 的 `shaderBinaries`、非本次构建产物 `staleShaderBinaries`、缺失的 `missingShaderBinaries`（`write-build-manifest.mjs:41-114`）。“清单一致”的含义是：源码哈希一致、`stale/missing` 为空——即部署的引擎与着色器出自当前源码。运行时 `createIsolatedRun` 只把清单的文件身份、版本与 `sourceTreeSha256` 记入每次运行的 manifest（`server.mjs:371-377`）；未见运行期自动比对清单与当前源码的代码，核对需人工比较，待核实。Release 构建要求 USD 支持（`HISTREAM_REQUIRE_USD=ON`，`build-engine.ps1:54`）。

#### 6.3.5 运行前/运行后检查清单

用户手册第 17 章要求的要点：场景单位为米且坐标/高度方向一致；OBJ 法向、三角化与实例数正确；光谱覆盖全部波段；反射率+透射率满足能量约束；气象温度、气压、风速、辐射单位正确；模拟与观测的分辨率、波段响应、观测角、时间一致；大气前后结果以 `_a` 区分；变化分析只比较同物理量同单位；GPP 白天随光照增强、夜间近 0；关键实验同时保存 `project.json`、统计 CSV 与运行日志（`docs/user-manual.md:1072-1083`）。据此给出可勾选清单。

**运行前**

- [ ] 工程已保存（`xmlDirty` 为假），保存的是当前版本的 `project.json`。
- [ ] 场景尺寸 x、y > 0，高度 ≥ 0，体元尺寸 > 0，传感器像素数 ≥ 1，至少一个波段与观测角。
- [ ] 场景单位为米，位置文件 x=北、y=东、z=高；OBJ 非空、法向与三角化正确（面元规模按三角面数×实例数估算）。
- [ ] 光谱范围覆盖全部传感器波段；ρ+τ ≤ 1，发射率合法；导入波谱文件满足 2–3 行且 ρ+τ ≤ 1。
- [ ] EB 模式：气象文件存在，`meteo.start/end` 节点范围合理（至少 1 个节点，≤100000）；温度、气压、风速、辐射单位正确；材质 `Tsoil`（℃）与 `Tsold`（K）单位勿混。
- [ ] 查看预检日志：面元数不超过上限（缺省 2000 万，`STREAM_STUDIO_MAX_FACET_TRIANGLES`/`control.maxFacetTriangles`），超 60% 时核对显存；体元网格 ≤4 亿格、辐射场显存估计合理；预计耗时不超过“单次运行上限”（缺省 15 分钟，`control.runTimeoutMinutes` 1–1440）。
- [ ] 面元输出格式确认（缺省 binary；复制结果须带同名 `.bin`）；预计单文件 >8 GiB 将被预检警告。
- [ ] 输出目录未与任何输入目录重叠；运行期间不要修改工程、气象文件（会被哈希复核拒绝出图）。
- [ ] 自定义引擎路径已加入 `STREAM_STUDIO_ENGINE_ALLOWLIST` 并重启。
- [ ] 改过引擎/着色器时：已重建，且 `build-manifest.json` 的 `staleShaderBinaries`、`missingShaderBinaries` 为空，`dirty` 状态已知。

**运行后**

- [ ] 日志出现“模拟完成”，退出码 0；FacetEB 状态 `non_converged`（退出码 2）仅表示结果保留，不能当作收敛。
- [ ] `run-manifest.json`：`status`、`exitCode`、`completed.nodeCount` 与 `requested.nodeCount` 一致；`error`、`warnings`、`runWarnings` 逐条阅读。
- [ ] 日志“运行质量”：FacetEB 未收敛节点数、最大温度等效残差（容差 0.5 K 等效）；VoxelEB 未达容差状态数；射线步数截断与太阳射线截断数为 0 或已评估。
- [ ] `runWarnings` 若提示无可见性样本占比 >5%，提高可见性栅格边长或改用体元。
- [ ] TIFF 数量与观测方向×节点数匹配；`result_statistics_*.csv` 中 `count` 等于有效像元数，无非预期的 NaN（count=0 的波段 min/max 为空）；反射率波段不应大量 >1（`compare.py` 的 `gt1` 即此检查）。
- [ ] 热红外温度在合理范围；NoData 与 QA（STREAM：`scene_summary.json` 的 `nonConvergedPixels`、`temperatureLimitedPixels`、`temperature.warning`）已检查，失败像元不参与精度统计。
- [ ] 变化分析前确认两幅图的波段、单位、投影方向、空间范围与大气标识一致。
- [ ] 归档：`project.json`、`run-manifest.json`、统计 CSV、运行日志一并保存。
- [ ] 改动引擎后：用 `tools/regression` 跑基线与新版，`compare.py --tolerance` 通过，并人工查看“CHANGED, not gated”汇总。

### 6.4 已知限制与未完成项

#### 6.4.1 `docs/model-remaining-issues-20261005.md` 中仍未解决的主要项

该文件按时间倒序累积，大量条目已标“已修正”。下列为文中仍明确标注“尚存/未做/未验证”的主要项（摘要）：

1. **A5 联合隐函数求导未实现**：STREAM 同化 EB 雅可比对 1/L 与阵风冻结，属准牛顿方向近似；Vcmax 增量偏差约 +55%，`lineSearchStalled` 不应视为失败（`:102-108`）。
2. **GLDAS 预设未经真实数据验证**，且强迫面板未在打包程序内实际点击验证（`:166`）。
3. **VoxelEB 单一场景 L**：稳定度迭代整个场景只有一个 Monin–Obukhov 长度，低风速正午常在 ζ=−5 限幅附近；土壤 G/Rn 仍约 0.6（VoxelEB）/0.78（FacetEB 修正前）（`:132,142,144`）。
4. **周期边界限制**：伸出计算域的几何不会在对侧出现；开周期 VoxelEB/VoxelRT 仍比不开周期慢约 19%/17%（`:126,156`）。
5. **Facet A-buffer**：已达 256 层或超出显存预算时仍报错，需清理重叠面或改用体元；稠密冠层开周期时 64 层 A-buffer 会溢出（`:196,222`）。
6. **土壤方法 2 深层固定温度**：VoxelEB 超过约 14 天或有升/降温趋势的算例，固定 1 m 边界引入约 1 W m⁻²/K 的偏差；无季节性深层温度（`:153`）。
7. **Hapke 观测方向固定为天顶**，引擎不接收传感器方向，斜视/透视仅为近似（`:146`）。
8. **预算残差保留**：auto/forest 白天节点最大混合残差约 21–23 W m⁻²；按要求未闭合继续下一时刻（`:254`）。
9. **未验证项**：三阶后的收敛误差、64 方向积分、全域能量闭合、实测精度与北京完整 5 km 场景成本；无 SCOPE/DART 对比（`:252`）。
10. **车辆与船舶**：人体适宜性评估整体推迟；无车内气流/分层；船舶无六自由度与波浪；水体为单层混合层（`:31-34,163-165`）。

#### 6.4.2 2026-11-26 审阅修复的遗留项

依据 `review_now/streamstudio-fixes-20261126/FIXES_2026-11-26.md` §3：

| 项 | 现状 |
|---|---|
| VoxelEB 流体 LBM 的 τ 限幅 | 决定等效粘度，需网格/风速标定或改亚格子模型，未改 |
| HEX 稠密存储 | 仅以 25 000 000 格上限阻止 OOM，稀疏/CSR 改造未做 |
| FacetEB 审计退出码 3 与资源失败 3 冲突 | 服务端仍把 3 当作 failed；需要独立退出码（引擎侧未核对） |
| 本地 API 无会话令牌 | 已加 `Sec-Fetch-Site` 与结果路径限制，同机原生进程仍可调用 |
| 其他体元基元（椭球/水体等）仍用截断循环 | 建筑墙体顶层侧面闭合、内部格 VoxelLink 语义未重构 |
| STREAM GeoTIFF/JSON 写出非原子 | `4e8` 像元上限与整幅缓冲的内存预算未统一 |
| HiStream 路径编码 | 窄字符 ifstream/GDAL，未做全链路宽字符改造 |
| `project-unified` 的 STREAM 转换无生产调用点 | 若接通，`assimilation` 丢失问题升为 P0 |
| TIFF 头解析重复 | `server.mjs` 与 `server/stream/rasters.mjs` 未抽公共实现 |
| 同步 IO 与渲染层缓存按条数 | P2 未处理 |

与本章直接相关的补充：`readTiff` 只读第一个 IFD、不支持 BigTIFF；`resultFilePathAllowed` 不排除运行中的目录；`EventSource` 无错误回调；`compare.py` 在无 `--tolerance` 时不会失败。

## 7. 运行判据、文档与代码差异、已知缺陷

本章把前六章中分散的“怎样才算正常运行”“哪里与其他文档不一致”“哪些是已确认的缺陷”集中登记，供运行结果判定、排错和后续修订时直接查阅。

### 7.1 退出码与运行状态的统一对照

#### 7.1.1 HiStream（`histream.exe`）

| 退出码 | 引擎含义 | 服务端判定 | 说明 |
| --- | --- | --- | --- |
| 0 | 成功 | completed | 所有模式 |
| 1 | 运行失败 | failed | 一般错误 |
| 2 | FacetEB 已完成但有节点未收敛，结果保留 | FacetEB：non_converged（结果可用）；其他模式不应出现 | 来自 `engine.cpp` 对 `facetebio->exitCode` 的透传 |
| 3 | `main` 中为资源创建失败；FacetEB 中为 CPU/GPU 能量审计不一致 | failed | **同码两义**，见 7.3 |
| 4 | 初始化失败（含工程读取、HEX 超限等 `Engine::input` 内异常） | failed | |
| 5 | 设备查询失败（`--list-devices`） | failed | |
| 64 | 用法错误（参数、模式名） | failed | 与“未收敛”区分 |

成功码集合：FacetEB 为 {0, 2}，其他模式为 {0}（server.mjs 中的 `histreamResultCodes`）。VoxelEB 的能量平衡未收敛**不会**返回 2，只在 `ENERGY_BALANCE` 行与结果诊断字段（如 `statesOutsideTolerance`、`budgetStatesOutsideTolerance`）体现，进程仍以 0 退出（第 4 章）。

#### 7.1.2 STREAM（`stream.exe`）

| 子命令 | 退出码 | 含义 |
| --- | --- | --- |
| `run` | 0 / 非 0 | 成功 / 失败，服务端按任务类型的 `successCodes` 判定 |
| `era5-register` / `forcing-register` | 0 / 3 / 1 | 登记成功 / 登记校验失败 / 异常；服务端注册任务只认 0 |
| `forcing-check` | 0 / 3 | 通过 / 有错误 |
| 无参数 | 2 | 打印用法 |

#### 7.1.3 运行记录终态（run-manifest.json 的 status）

| status | 含义 | 结果是否可用 |
| --- | --- | --- |
| running | 引擎进行中或后处理中 | 否 |
| completed | 成功码退出且后处理完成 | 是 |
| non_converged | FacetEB 退出码 2 | 是，但必须核对收敛诊断 |
| stopped | 用户停止或重置 | 否（可能留有半成品） |
| timeout | 超过单次运行上限（`runTimeoutMinutes`） | 否 |
| failed | 其他失败 | 否 |

STREAM 任务的 status 取值为 queued / running / completed / failed / stopped；**只有 completed 才列出 products**，失败、停止的运行即使目录里有 `.tif` 也不得使用（第 5 章）。

### 7.2 运行判据（规范性规则）

下列规则由前六章的代码事实归纳而成，作为判断“一次运行是否可信、能否作为依据”的最低要求。

1. **状态与退出码同时检查**：只看界面“完成”不够，必须在 run-manifest 中确认 `status` 与 `exitCode` 一致（completed ↔ 0；non_converged ↔ 2）。
2. **FacetEB 的 non_converged 不是失败，也不是通过**：结果被保留，但必须读 `facetrt.json`/`faceteb.json` 的收敛诊断，统计未达容差的节点数，再决定能否使用。
3. **VoxelEB 的收敛只能从诊断字段判断**：退出码恒为 0，必须检查 `statesOutsideTolerance`、`budgetStatesOutsideTolerance` 以及能量闭合残差。
4. **不要把编译通过或单元测试通过当作数值正确的证明**：几何取整、观测角、占位热属性等修改会改变数值，发布前必须运行 `tools/regression` 并对比 G/Rn、反射率、亮温、迭代次数与耗时（第 6 章 6.3）。
5. **热红外结果必须确认温度来源**：VoxelRT/RayTracing 没有热属性时使用 300 K 占位（无提示）；STREAM 组分温度必须以 K 给出（150–400 K），误填 ℃ 在栅格输入时只表现为 `thermalFailedPixels` 增多。
6. **输入路径使用全英文可写路径**：引擎侧经窄字符接口读取，中文或代码页外字符可能打开失败；STREAM 读取 ERA5 NetCDF 时还依赖 8.3 短文件名。
7. **运行目录不可复用，也不可手工修改**：每次运行的输入被冻结在 `input/` 下，但 OBJ、DEM、光谱文件仍按原路径引用（第 2 章 2.5.2），运行期间不得改动这些原文件。
8. **同一时刻只运行一个引擎进程**：HiStream 与 STREAM 各自串行；STREAM 的反演与同化互斥（第 5 章）。
9. **更改计算域或体元尺寸后要核对取整**：计算域格数与背景网格格数的取整方式不同，`scene.x/voxel` 非整数时对结果的影响尚未评估（7.4）。
10. **结果文件只从允许范围内读取**：工程输出根、带 `run-manifest.json` 的运行目录，或含 `faceteb.json`/`facetrt.json`/`scene_summary.json` 的产物目录；复制到其他位置的结果需带上这些标志文件才能在界面中打开。
11. **本地 API 不具备防本机其他进程的能力**：无会话令牌，只靠 Host/Origin/Sec-Fetch-Site/Content-Type 校验；不要在多用户共用的机器上暴露端口。
12. **版本守卫**：高于当前 schema 的工程会被拒绝；旧版本工程由 `normalizeProject` 迁移，首次保存后磁盘上即为 `stream-studio-project` 统一格式，之后不保证能在更低版本软件中打开。

### 7.3 已确认的实现缺陷与待修项

下列条目已用源码核对，或由其他章节的代码证据支持。修复前，运行时请按“应对”一列处理。

| 编号 | 位置 | 现象 | 应对 |
| --- | --- | --- | --- |
| D1 | server.mjs:5302-5322（`/api/run` 的 catch） | 准备阶段被用户取消时先写入 `stopped`（5306），随后在 5321 无条件覆盖为 `failed`，且接口返回 400 | 取消后以界面“已取消”提示为准；manifest 状态为 failed 并不代表引擎出错 |
| D2 | server.mjs:3013-3029（`resultDirectoryAllowed`） | 2026-11-26 修复在 3019 行加入“目录含 faceteb/facetrt/scene_summary 即允许”，该函数同时用于**删除**，使删除保护范围变宽 | 删除结果前确认目录；修复应只保留在读取判定 `resultFilePathAllowed` |
| D3 | src/renderer/src/main.js：`loadXml(result, confirm = true)` | 另存为（成功后调用）、打开示例、选择工程等调用点在服务端已切换工程后才弹“放弃未保存修改”确认；取消后界面与服务端工程不一致，随后保存会被“只能保存到当前工程目录”拒绝 | 取消后重新打开工程；修复应把确认前移到调用 API 之前，内部调用传 `confirm=false` |
| D4 | models/histream/src/facetrt/facetrtcore.cpp:9383 | FacetEB 退出码 3 同时表示“CPU/GPU 能量审计不一致”和资源失败，服务端一律判 failed | 出现 3 时先看 stderr 区分原因 |
| D5 | models/histream/src/voxelrt（`VoxelrtIO::k_angle`） | `k_angle` 无初始值，UAV 分支循环中不更新，`outputPos` 用它选观测角（第 3 章） | 透视投影只有 1 个观测角时恒为 0，实际无影响；多角度 UAV 输出需复核 |
| D6 | server/stream/index.mjs:387-405 | `era5-register` 必需变量不连续时引擎退出 3 且写出索引，服务端注册任务只认 0，会删除该索引 | 登记失败后检查日志，补齐时段后重试 |
| D7 | 引擎侧 | 路径经 `std::string` 传递，代码页外字符路径可能失败（main.cpp:126、fileio_json.cpp:398） | 使用全英文路径 |
| D8 | 本轮审阅修复遗留 | VoxelEB LBM τ 限幅决定等效粘度；HEX 仅有 25 000 000 格上限；STREAM 输出写入非原子；本地 API 无令牌（第 6 章 6.4.2） | 见各项说明 |

### 7.4 文档与代码的差异登记

| 编号 | 文档或常识 | 代码事实 | 处理 |
| --- | --- | --- | --- |
| C1 | 理论手册 §8.2：VoxelEB 直射与漫射短波均按窗口宽度计算 | 窗口宽度只作用于多次散射分组；`directVNIR` 逐 1 nm 全波段累加（voxelrad_direct_VNIR.comp:135） | 修订理论手册措辞，运行以代码为准 |
| C2 | 理论手册 §2.1：Nx = ceil(L/d) | 平地背景网格按 ceil，计算域按截断取整（scene.cpp:2939、voxeldesigner.cpp:1150）；背景体元不做域裁剪 | 非整数倍时影响待评估（第 3 章 3.11） |
| C3 | 通常认为两种 EB 引擎初值一致 | VoxelEB 缺省 `Tsold=300 K`（fileio_json.cpp:882）；FacetEB 缺省取首节点气温 | 以代码为准，运行时显式设置 |
| C4 | 气象文件格式通用 | FacetEB 首行为被丢弃的表头；VoxelEB 首行为节点数 | 不可混用气象文件 |
| C5 | 通常认为存在 spin-up 参数 | HiStream 的 FacetEB/VoxelEB 都没有；预热靠前移 `meteo.start`。STREAM 层的 `spinupHours` 与强迫步长无关 | 见第 4、5 章 |
| C6 | STREAM 配置注释：波段“0 起” | 引擎按 1 起解释；`/api/stream/raster` 的 `band` 为 0 起；配置中 `band=0` 或缺省都取第 1 波段 | 第 5 章 5.11 |
| C7 | 服务端组分温度要求 150–400 K | 引擎读入后把 100–400 K 之外置 NaN；EB 解被限幅到 175–380 K；反演/同化产物另限 200–380 K | 三层范围并存，见第 5 章 |
| C8 | 土壤水分限幅 | 强迫来源 0–1；用户来源 0.05–0.55 且大于 1 视为百分数 | 第 5 章 5.11 |
| C9 | `desktop/settings.mjs` 注释“10 个键” | 白名单实际 9 个（settings.mjs:20-24,61） | 以代码为准 |
| C10 | `--api-only` 启动日志打印 5173 | 实际监听 `STREAM_STUDIO_PORT`（缺省 4173） | 以代码为准（server.mjs:5611） |
| C11 | “冻结输入”的说法 | 只覆盖运行时 `project.json` 与气象快照（有哈希）；OBJ、DEM、光谱文件仍引用原路径 | 第 2 章 2.5.2 |
| C12 | `examples/` 中 `.img/.hdr` 示例 | 当前引擎输出为 GeoTIFF 与 CSV，统计文件 `result_statistics_raytracing.csv`、`result_statistics_voxelrt.csv` | 以当前输出为准 |
| C13 | `/api/results/delete` | 实际路径为 `/api/results/delete-all` | 第 6 章 |
| C14 | `compare.py` 一般用作门禁 | 未给 `--tolerance` 时退出码恒为 0；给出后缺失算例、退出码不同或最大相对变化超限才返回 1 | 作为门禁时必须传 `--tolerance` |

### 7.5 待核实项汇总

下列条目是各章撰写时明确标注“待核实”的内容，**不得当作结论引用**。核实后应回到对应章节改写，并从此表删除。

| 章 | 待核实内容 |
| --- | --- |
| 1 | `HISTREAM_ROOT` 在源码中无读取点，实际作用未知；打包安装版中后台线程能否启动未实测；`STREAM_STUDIO_DATA_ROOT` 回退目录是否被 `userOpenPathAllowed` 放行；服务进程崩溃重启后界面是否自动恢复工程 |
| 2 | `mode` 与 `project.mode` 是否总一致；`taskkill` 终止后 `close` 的 `code` 取值；spawn 失败是否同时触发 `close`；`.histream-transient-*.json` 在失败路径是否被清理；Voxel/RayTracing 各模式的 ENVI 文件名与 `process/`、`diagnostics/` 清单；`sensor.x/y` 与 `Tsold/Tsoil` 单位约定 |
| 3 | 总内存/显存预算缺失时的行为；VoxelRT 除 gap 阶段外着色器缺失时的失败路径；设备排序细节（`facetrt_vulkan.h`） |
| 4 | VoxelEB `Tsold` 为 null 时的取值；FacetEB 气象文件中水汽压与气压单位（按 hPa 使用）；成像波段 0.001 换算因子与太阳位置海拔固定 100 m 的依据；VoxelEB `updateL` 是否有其他调用方；`SolarStepLimit` 取值 |
| 5 | GLDAS 预设的变量与参考高度；强迫重采样与站点插值的细节；工作台是否把 `scene.outputRoot` 设为工程目录下的 output；`readTiff` 的 nodata 与 scale 处理；`/api/stream/raster` 后台线程实现 |
| 6 | `thermalSurfaceStretch` 间隙阈值单位；引擎写入 `zeroVisibilitySamples` 的位置；前端 VoxelEB“x/50 迭代”的 50 与引擎常量是否一致；耦合迭代、温度容差/松弛、天空温度是否有服务端校验；build-manifest 运行期与源码的比对；tools 下 STREAM、pytest、C++ 测试是否纳入发布流程 |

### 7.6 修改本文档的规则

1. 修改代码行为时，同步修改对应章节与本章 7.1–7.4 的相关表项。
2. 新发现的缺陷先登记在 7.3，修复并经验证后再移除，不要直接改写历史描述。
3. 数值性改动（几何取整、角度、热属性、求解器）必须在 7.2 第 4 条的回归之后才能写“已验证”。
4. 每次重新生成 PDF 前先运行 `npm test`，并在 0.2 节更新所对应的代码提交号。

## 附录 A. 代码模块索引

本附录按目录列出各模块的职责与对应章节。职责依据各章对源码的核对结果与文件命名归纳；需要细节时进入对应章节，再以源码为准。路径均为仓库相对路径。

### A.1 应用层（Node / 前端）

| 路径 | 职责 | 章节 |
| --- | --- | --- |
| desktop/main.mjs、preload.cjs、server-process.mjs、local-server.mjs、settings.mjs | Electron 外壳：窗口、本地服务子进程的启动与崩溃恢复、用户设置白名单 | 1 |
| server.mjs | 本地 HTTP/SSE 服务入口：路由、校验、工程读写、运行编排、TIFF/ENVI/CSV 读写、结果列表与删除 | 1、2、6 |
| server/task-pool.mjs | 后台线程池（常驻池、一次性线程、专用线程与回退） | 1 |
| server/result-cache.mjs | 结果缓存 `ByteLru`（按字节计量） | 1、6 |
| server/facet-result.mjs | 面元/体元/过程结果 JSON 与二进制 sidecar 的读取与采样 | 6 |
| server/stream/index.mjs | STREAM 服务：任务队列、spawn、看门狗、manifest、登记任务、SSE | 5 |
| server/stream/config.mjs | STREAM 工程与引擎配置的构建与校验 | 5 |
| server/stream/forcing.mjs、rasters.mjs | 强迫元数据规则；栅格头解析、面积与单位 | 5 |
| src/renderer/src/main.js | 三维工作台主界面：状态、检查器、运行与结果面板 | 6 |
| src/renderer/src/project-schema.js、project-unified.js | 三维工程模型：默认值、归一化、校验；统一工程转换 | 2 |
| src/renderer/src/scene-loader.js | 场景装载与三维预览 | 6 |
| src/renderer/src/web-api.js、stream/api.js | 与服务端的接口层 | 6 |
| src/renderer/src/stream/workspace.js、forcing-panel.js | STREAM 工作台与强迫面板 | 5、6 |
| src/renderer/src/i18n.js、storage.js、http.js、menubar.js、material-specular.js | 本地化、持久化、请求封装、菜单、镜面材质辅助 | 6 |
| tools/runtime-scene.mjs | 运行时场景生成：实例边界筛选、OBJ 预裁切、DEM 高程 | 2 |
| tools/surface-textures.mjs、tarp-cover.mjs | 纹理展开与篷布生成等运行前资产准备 | 2 |
| tools/write-build-manifest.mjs、build-engine.ps1、build-all.ps1 | 构建与 build-manifest 生成 | 1 |
| tools/build-core-manuals-pdf.py | 生成使用手册、理论手册与运行原理说明 PDF | 本文 0.2 |

### A.2 HiStream 引擎（models/histream）

| 路径 | 职责 | 章节 |
| --- | --- | --- |
| src/main.cpp | 命令行、模式分派、退出码 | 3 |
| src/base/engine.cpp、engine.h | 引擎生命周期：输入、初始化、运行、销毁；退出码透传 | 3、4 |
| src/base/appsetting.cpp | 设备枚举与选择、全局设置 | 3 |
| src/base/projectjson.cpp、fileio_json.cpp | 工程 JSON 的解析与各模式参数装载 | 3 |
| src/base/scene.cpp、voxeldesigner.cpp、hexvoxel.cpp、geometry.cpp | 场景构建：对象与实例、体元基元与建筑、异质体元、几何工具 | 3 |
| src/base/objloader.cpp、meshio.cpp、instanceio.cpp、surfio.cpp、gridio.cpp、voxelio.cpp | OBJ 装载及网格、实例、表面、栅格、体元数据容器 | 3 |
| src/base/accelstruct.cpp | 光线追踪加速结构（BLAS/TLAS） | 3 |
| src/base/structs.h、structs_cg.h | 引擎与 GPU 共享的数据结构（Instance、Thermal、Angle 等） | 3 |
| src/raytracing/ | RayTracing 模式：缓冲、描述符、管线、命令、运行与输出 | 3 |
| src/voxelrt/ | VoxelRT 模式：体元辐射传输成像 | 3 |
| src/facetrt/（facetrtcore.cpp、facetrt_vulkan.cpp、energyphysics.h、leafbiochemistry.h、lumpedthermal.h） | FacetRT 与 FacetEB 共用的面元核心：可见性、求解、能量平衡物理 | 4 |
| src/faceteb/ | FacetEB 模式的 Vulkan 资源与运行封装 | 4 |
| src/voxeleb/ | VoxelEB 模式：体元能量平衡与耦合迭代 | 4 |
| shader/RayTracing、voxelrt、voxeleb、facetrt、faceteb | GLSL 着色器（光线追踪、辐射传输、能量平衡、可见性图） | 3、4 |
| shader/settings.h、bindingLayout.h | CPU 与着色器共享的常量与绑定布局 | 3、4 |

### A.3 STREAM 引擎（models/stream）

| 路径 | 职责 | 章节 |
| --- | --- | --- |
| app/main.cpp | 子命令：run、era5-register、forcing-register、forcing-preset、forcing-check，及 `--files-from` | 5 |
| app/config.cpp、image.cpp | 引擎配置读取与影像级运行主流程 | 5 |
| app/inversion.cpp、assimilation.cpp、thermal_view.cpp | 反演、同化与热红外观测视图 | 5 |
| scene/raster.cpp、classes.cpp | 栅格读写与地表类型表 | 5 |
| forcing/ | ERA5 与通用强迫数据：登记、元数据、重采样、站点、像元强迫、时间 | 5 |
| physics/ | 冠层、光学、短波、辐射度、能量平衡、土壤热传导、有效 LAI、Hapke、亮温 | 5（物理见理论手册） |
| classes/、defined/ | 地表分类与预置系数表 | 5 |

### A.4 测试、回归与统计

| 路径 | 职责 | 章节 |
| --- | --- | --- |
| tools/test-*.mjs、test-*.py、test-*.cpp | 单元与回归测试（`npm test` 运行其中一部分） | 6 |
| tools/regression/ | 数值回归与 `compare.py` 比较 | 6 |
| tools/check-*.mjs、check-*.py | 发布案例与输出检查 | 6 |
| statistic/ | 统计报告与场景示例脚本 | 6 |

