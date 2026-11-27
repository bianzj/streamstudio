# Stream Studio 代码模块说明

## 1. 总体架构与代码组织

### 1.1 本文定位

《代码模块说明》是面向开发者的代码参考：它回答“代码在哪、谁依赖谁、改什么要动哪里”。三本技术文档的分工如下：

| 文档 | 回答的问题 | 对应源文件 |
| --- | --- | --- |
| 《理论与方法》 | 模型算什么：方程、假设、适用边界 | docs/theory-manual.md |
| 《使用手册》 | 怎么操作：界面、参数含义、推荐流程 | docs/user-manual.md |
| 《运行原理说明》 | 程序怎么跑：进程、数据流、状态、退出码、限制 | docs/runtime-principles.md |
| 《代码模块说明》（本文） | 代码怎么组织：文件职责、接口、依赖、修改指南 | docs/code-modules.md |

本文不重复运行流程，凡涉及流程的内容只指向《运行原理说明》的章号。各章括号内的 `文件:行号` 是证据位置；多数章为节省篇幅省略了路径前缀，省略规则在各章开头说明（第 4、5 章省略 `models/histream/`，第 7 章省略 `models/stream/`，第 6 章用别名表）。行号会随代码演进漂移，核对时以函数名为准。“待核实”表示撰写时没有读完相关代码，不得当作结论引用，汇总见第 9 章。

### 1.2 仓库目录总览

| 目录 | 内容 | 详见 |
| --- | --- | --- |
| desktop/ | Electron 主进程、预加载脚本、本地服务进程管理、用户设置 | 第 2 章 |
| server.mjs、server/ | 本地 HTTP/SSE 服务：路由、工程读写、运行编排、结果读写、后台线程、STREAM 服务 | 第 2 章 |
| src/renderer/ | 前端：三维工作台、STREAM 工作台、工程模型、接口层、本地化 | 第 3 章 |
| models/histream/ | HiStream 三维引擎（C++ / Vulkan / GLSL） | 第 4、5、6 章 |
| models/stream/ | STREAM 影像引擎（C++） | 第 7 章 |
| models/nvpro_core/ | 第三方 Vulkan 辅助库（构建依赖，非本项目代码） | 第 4 章 |
| models/bin_x64/Release/ | 构建产物与运行时文件（`histream.exe`、`stream.exe`、SPV 着色器、`build-manifest.json`） | 第 8 章 |
| tools/ | 构建、资产生成、运行时辅助、检查、测试、文档生成脚本 | 第 8 章 |
| statistic/ | 统计与场景示例报告脚本 | 第 8 章 |
| assets/ | 内置资源库：示例工程、OBJ、DEM、光谱、纹理、大气查找表、RAMI | 第 8 章 |
| docs/ | 技术文档、手册 PDF、案例报告、发布说明 | 第 8 章 |
| gui/、build/、release/ | 前端构建输出、C++ 构建中间文件、发布暂存（均为生成物） | 第 8 章 |
| .packaging/、design/、review/、.test/、tests/ | 打包暂存、设计记录、审阅记录、测试运行目录 | 第 8 章 |

仓库根的 `package.json` 声明包名 `stream-studio`、版本 2.0.1、`type: module`、入口 `desktop/main.mjs`。关键脚本：

| 脚本 | 命令 | 作用 |
| --- | --- | --- |
| dev | `node dev.mjs` | 开发模式（Vite 与服务端） |
| build / build:gui | `vite build` | 构建前端到 gui/ |
| build:engine | `tools/build-engine.ps1` | 构建 HiStream 与 STREAM 引擎 |
| build:all | `tools/build-all.ps1` | 引擎与前端全量构建 |
| start | `node server.mjs` | 仅启动本地服务（Web 模式） |
| test | `node --test` 四个测试文件 | 回归测试，覆盖范围见第 8 章 |
| pack:win | `build:all` → `stage:package` → `electron-builder --win nsis` | 生成 Windows 安装包 |

### 1.3 分层与调用方向

```
┌──────────────────────────────────────────────────────────────┐
│ 前端 src/renderer（浏览器或 Electron 窗口）                    │
│   main.js ─ webApi(web-api.js) ─ stream/workspace.js          │
└───────────────┬──────────────────────────────────────────────┘
                │ HTTP JSON / SSE（127.0.0.1）
┌───────────────▼──────────────────────────────────────────────┐
│ 应用层 server.mjs + server/*（Node）                           │
│   工程读写 · 运行编排 · 结果读写 · 后台线程池 · STREAM 服务    │
│   引用：src/renderer/src/project-schema.js、project-unified.js │
│         tools/runtime-scene.mjs、usd-export/import、tarp-cover │
└───────────────┬──────────────────────────────────────────────┘
                │ spawn 子进程（命令行 + stdout 协议 + 文件）
┌───────────────▼───────────────┐   ┌─────────────────────────┐
│ histream.exe（C++/Vulkan）     │   │ stream.exe（C++）        │
│  models/histream               │   │  models/stream           │
└───────────────────────────────┘   └─────────────────────────┘
```

依赖方向的几条规则，是读代码和改代码时的前提：

1. **前端不直接接触引擎**：一切经 `/api/*`；前端到引擎的唯一数据通道是服务端写出的运行时工程与文件。
2. **服务端引用前端目录下的纯逻辑模块**：`server.mjs` 直接 `import` 了 `src/renderer/src/project-schema.js` 与 `project-unified.js`（工程默认值、归一化、校验、统一工程转换）。这两个文件必须保持“无 DOM、无浏览器依赖”，否则服务端无法加载。修改它们等于同时修改前后端行为。
3. **引擎之间互不依赖**：`models/histream` 与 `models/stream` 是两个独立的 CMake 工程，各有自己的配置格式和命令行；Node 侧通过两个独立的服务入口驱动（HiStream 在 server.mjs，STREAM 在 server/stream/）。
4. **HiStream 内部**：`src/base` 是公共基础层（工程解析、场景构建、数据容器、加速结构）；五种模式模块（raytracing、voxelrt、voxeleb、facetrt、faceteb）依赖 base；按 `#include` 检索，模块之间唯一的跨目录引用是 faceteb 包含 facetrt 的头文件（`faceteb/command.cpp:16` 引 `facetrtcore.h`，`faceteb/facetebio.h:20` 引 `facetrt_vulkan.h`），即 FacetEB 复用 FacetRT 的核心；着色器按模式分目录，公共头（`settings.h`、`bindingLayout.h`）由 CPU 与 GPU 两侧共同包含。
5. **服务端与引擎只通过四种方式耦合**：命令行参数与环境变量；运行目录中的输入文件；stdout 的 `PROGRESS`/`OBSERVATION`/`RESULT`/`PROCESS` 等行协议；退出码。引擎侧改动这四项之一，必须同步修改服务端（第 2 章与《运行原理说明》第 2 章）。

### 1.4 一次修改会波及哪里：耦合点清单

| 耦合点 | 一侧 | 另一侧 | 同步规则 |
| --- | --- | --- | --- |
| 工程字段 | `project-schema.js`（默认值与校验） | `models/histream/src/base/fileio_json.cpp`、`projectjson.cpp`（解析与缺省） | 新增字段要两侧都加，缺省值可能不同（《运行原理说明》第 7 章 C 类差异） |
| 退出码 | `server.mjs` 的 `HISTREAM_EXIT` | `models/histream/src/main.cpp`、`engine.cpp`、`facetrtcore.cpp` | 码表需与 main.cpp 一致；FacetEB 的 3 与资源失败同码，是已知缺陷 |
| 进度行协议 | 前端 `main.js` 正则解析 | 各模式 `std::cout << "PROGRESS\t…"` | 格式为制表符分隔，前端按空白切分 |
| 面元结果格式 | `server/facet-result.mjs`、`tools/facet_result.py` | `facetrtcore.cpp` 的二进制写入端 | 读取端并不一致：JS 读取器不读 `temperatureSunlit/Shaded`，Python 读取器会读（第 6 章） |
| CPU/GPU 共享结构 | `src/base/structs_cg.h`、`shader/settings.h` | 各模式着色器 | 字段顺序与对齐必须一致（第 4 章） |
| 着色器目录名 | CMake 与 `pipeline.cpp` 用小写 `shader/raytracing` | 磁盘目录 `shader/RayTracing` | Windows 不区分大小写，其他平台会失败（第 4、5 章） |
| STREAM 配置 | `server/stream/config.mjs`（构建与校验） | `models/stream/app/config.cpp`（读取与校验） | 范围校验两侧不同，服务端更严（《运行原理说明》第 5 章） |
| 统一工程 | `project-unified.js` | `server.mjs` 的 `readProject`、USD 同步 | 磁盘 `project.json` 为 `stream-studio-project`，运行时重新生成 `histream-project` |
| 构建清单 | `tools/write-build-manifest.mjs` | `models/bin_x64/Release/build-manifest.json`、run-manifest | 记录源码哈希与引擎哈希，用于追溯 |

### 1.5 代码约定

1. **文件头注释**：绝大多数源文件以“文件 / 功能 / 主要模块 / 数据流 / 约定 / 注意”的中文块注释开头，是进入文件的第一入口；函数上方有“作用 / 输入 / 输出 / 说明”注释。头注释可能滞后（本文各章已逐项记录与代码不一致处）。
2. **标记词**：`DEFERRED[级别]` 表示已知但暂不处理的问题（附位置、原因与建议）；`FIX(日期 编号)` 表示某次审阅的修复记录；“待核对”表示作者自己也未确认。修复后旧 DEFERRED 注释应改写或删除（本轮审阅已清理一批）。
3. **坐标与单位**：引擎坐标为 +X 北、+Y 上、+Z 东；工程里的 (x, y, z) 为（北、东、高）；温度对外为 K，体元土壤历史缓冲以 ℃ 存储。细节见《运行原理说明》第 0 章与第 3、4 章。
4. **命名**：产品 Stream Studio；影像引擎 STREAM（`stream.exe`，`stream-*` 格式标识）；三维扩展 HiStream（`histream.exe`，`histream-*` 格式标识，`HISTREAM_*` 引擎环境变量）；应用环境变量前缀 `STREAM_STUDIO_*`，旧前缀 `HISTREAM_STUDIO_*` 仅部分变量保留别名（第 2 章）。
5. **路径**：服务端内部用 Node 路径，写给引擎时转 Windows 路径（`windowsPath`）；引擎侧窄字符路径对非 ANSI 字符不安全，运行用例请使用全英文路径。
6. **文本编码**：源码与文档为 UTF-8；个别 PowerShell 脚本以默认编码读写会出现乱码，读写时显式指定 UTF-8。

### 1.6 如何使用本文

- **想找某个功能的实现位置**：先在 1.2 找目录，再进入对应章的“文件清单表”。
- **想改某个行为**：先读对应章末尾的“修改指南与注意事项”，再看 1.4 的耦合点，确认是否需要两侧同步。
- **想确认运行时发生了什么**：转《运行原理说明》，按其第 2 章的时间线定位阶段，再回到本文查对应文件。
- **想判断一个结论是否可靠**：看是否带 `文件:行号`；带“待核实”的不能当作事实使用。

## 2. 应用层：Electron 外壳与本地服务

本章是应用层的代码模块参考，覆盖三部分源码：桌面外壳 `desktop/`（5 个文件，约 620 行）、本地 HTTP 服务主体 `server.mjs`（5672 行）、服务端支撑模块 `server/`（含 `server/stream/`，共 7 个文件，约 3000 行）。运行流程（一次模拟如何准备、启动、收尾）在《运行原理说明》第 1 章（进程、HTTP、线程池、缓存）与第 2 章（运行生命周期）、第 5 章（STREAM 服务）中叙述，本章只给出代码位置、接口与修改点，不重复流程。物理方程见《理论与方法》，本章不涉及。

阅读约定：括号中的“文件:行号”均为仓库相对路径；“待核实”表示本次调查没有读到足够代码，不作断言。

### 2.1 模块总览与依赖方向

应用层的运行形态有两种：桌面版由 Electron 主进程拉起一个 `utilityProcess` 子进程承载 HTTP 服务；开发/Web 版直接 `node server.mjs`，`dev.mjs` 会以 `--api-only` 拉起后端并与 Vite 并行（dev.mjs:25；package.json:10,15，`main` 指向 `desktop/main.mjs`，package.json:8）。

依赖方向（箭头表示“导入或启动”）：

```
desktop/main.mjs ──utilityProcess.fork──▶ desktop/server-process.mjs ──动态 import──▶ server.mjs
desktop/main.mjs ──import──▶ desktop/settings.mjs
desktop/server-process.mjs ──import──▶ desktop/local-server.mjs
server.mjs ─┬─▶ server/facet-result.mjs        （面元结果读取，无上游依赖）
            ├─▶ server/result-cache.mjs        （LRU 与二进制编码，无上游依赖）
            ├─▶ server/task-pool.mjs           （线程池协议，无上游依赖）
            ├─▶ server/stream/index.mjs ─▶ config.mjs ─▶ forcing.mjs
            │                          └─▶ rasters.mjs
            ├─▶ src/renderer/src/project-schema.js、project-unified.js（工程模型，与前端共用）
            └─▶ tools/runtime-scene.mjs、tarp-cover.mjs、usd-export.mjs、usd-import.mjs
```

三条规则需要记住：

1. `server/` 下的模块不得反向导入 `server.mjs`。`server/stream/index.mjs` 需要的 `json`、`body`、`readTiff`、`normalizeHostPath`、`windowsPath`、`runTask` 全部由 `server.mjs` 在创建服务时注入（server.mjs:184-185；server/stream/index.mjs:77）。
2. `server.mjs` 同时是主线程入口和 worker 线程脚本：后台线程用 `new Worker(new URL(import.meta.url))` 重新加载整个文件，靠 `workerData.kind` 分流（server.mjs:163,2410,5602-5643）。因此文件顶层的任何副作用都会在每个 worker 中重复执行一次。
3. 桌面版的页面与接口同源（`http://127.0.0.1:<随机端口>`），端口由 `startDesktopServer` 在系统分配后回报（desktop/local-server.mjs:31-34）；`server.mjs` 在桌面模式下不自行监听（`STREAM_STUDIO_SERVER_AUTOSTART=0`，desktop/main.mjs:195；server.mjs:5603）。

### 2.2 desktop/：Electron 外壳

#### 2.2.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| desktop/main.mjs | 422 | Electron 主进程入口：用户数据目录与旧版迁移、单实例锁、服务子进程的启动/崩溃恢复/退出、窗口与外链策略、设置 IPC | 无导出；`migrateLegacyUserData`、`killProcessTree`、`terminateServer`、`killOrphanedChildren`、`stopServerProcess`、`serverEnvironment`、`chooseProjectFile`、`startServerProcess`、`handleServerExit`、`waitForServer`、`createWindow` |
| desktop/server-process.mjs | 79 | utilityProcess 子进程入口：动态加载 `server.mjs`，注册文件选择代理，监听端口并回报 URL，处理 shutdown | 无导出；`shutdown`、`pendingChoices`、消息分发 |
| desktop/local-server.mjs | 35 | 让 `http.Server` 监听 `127.0.0.1:0` 并返回基础 URL | `startDesktopServer(server)` |
| desktop/settings.mjs | 71 | 渲染进程偏好与草稿的落盘存储（`renderer-settings.json`） | `createDesktopSettings(directory)` |
| desktop/preload.cjs | 13 | 向页面暴露 `window.streamStudioSettings`（同步 IPC 封装） | `read`、`setItem`、`removeItem` |

#### 2.2.2 main.mjs

职责：它是桌面版唯一的主进程，本身不做模拟、不读结果；窗口、对话框和 IPC 在这里，所有重活都在服务子进程里（desktop/main.mjs:34-36）。

启动阶段的关键点：

- 身份与目录：`app.setName('Stream Studio')`，用户数据目录固定为 `%APPDATA%/Stream Studio`，必须在 app ready 之前设置（desktop/main.mjs:42-51）。`migrateLegacyUserData` 只在新目录没有 `Local State` 且旧目录“HiStream Studio”有时才迁移，先复制到 `<userData>.migrating` 暂存目录，再拷入新目录，跳过缓存与 `Singleton*` 锁文件（`MIGRATION_SKIPPED`，desktop/main.mjs:54-80）。
- 单实例：`requestSingleInstanceLock` 失败直接 `app.quit()`；第二实例通过 `second-instance` 事件唤起已有窗口（desktop/main.mjs:83-94）。
- 数据根：`createWindow` 取 `%LOCALAPPDATA%/StreamStudio`（缺省回退 `userData`）作为 `STREAM_STUDIO_DATA_ROOT`，资源根 `resourceRoot()` 在打包后为 `process.resourcesPath`，开发时为源码根（desktop/main.mjs:313-315,339-343）。

服务子进程管理的调用关系：

```
app.whenReady → createWindow (338)
  serverEnvironment(root, dataRoot) (184)        ← 组装 PATH、STREAM_STUDIO_*、HISTREAM_ROOT
  startServerProcess(env) (221)                  ← utilityProcess.fork(desktop/server-process.mjs)
      message: listening → resolve(record)；startup-error → reject；choose-project-file → chooseProjectFile (202)
      exit: 就绪前 → reject(附 stderr 尾部)；就绪后 → handleServerExit (277)
  waitForServer(url) (319)                       ← 轮询 GET /api/defaults，150 ms 一次，15 s 超时
  createDesktopSettings → ipcMain.on('stream-studio:settings') → new BrowserWindow → loadURL
退出：before-quit → stopServerProcess (167) → 超时 5 s → terminateServer → killProcessTree (125)
崩溃：handleServerExit → killOrphanedChildren (148) → 60 s 内最多重启 2 次，再崩溃弹窗并退出
```

接口约束：

- `serverEnvironment` 删除 `ELECTRON_RUN_AS_NODE`，把 `<root>/engine` 放到 PATH 最前，并设置 `STREAM_STUDIO_RESOURCE_ROOT`、`STREAM_STUDIO_DATA_ROOT`、`STREAM_STUDIO_SERVER_AUTOSTART=0`、`HISTREAM_ROOT`（desktop/main.mjs:184-198）。PATH 分隔符硬编码为分号，只适用于 Windows（desktop/main.mjs:190）。
- 进程树结束：Windows 用 `taskkill /T /F`，其他平台只能 `SIGKILL` 该 pid 本身（desktop/main.mjs:125-132）；pid 尚为 0（`spawn` 事件未触发）时退回 `child.kill()`（desktop/main.mjs:138-141）。
- 窗口：取主显示器工作区内最大的 16:9 窗口并居中（desktop/main.mjs:365-374）；`contextIsolation: true`、`sandbox: true`、`nodeIntegration: false`，仅靠 preload 暴露接口（desktop/main.mjs:393-398）。
- 外链：`setWindowOpenHandler` 一律 `deny`，http(s) 交系统浏览器；`will-navigate` 对非同源目标阻止并同样转交（desktop/main.mjs:402-411）。
- 冒烟测试：命令行带 `--smoke-test` 时不显示窗口，页面载入后输出 `STREAM_STUDIO_SMOKE_TEST_OK <url>` 并退出；此时服务崩溃不重启（desktop/main.mjs:32,285,418-421）。

被谁调用：Electron 运行时（package.json 的 `main`）。依赖：`settings.mjs`、Electron、`server-process.mjs`（以路径字符串 fork，非 import）。`utilityProcess.fork` 与图标、preload 的路径都基于 `sourceRoot`（desktop/main.mjs:223,394），打包后该路径是否位于 asar 内待核实。

#### 2.2.3 server-process.mjs 与 local-server.mjs

`server-process.mjs` 只有一个职责：在子进程里把 `server.mjs` 跑起来并与主进程对话。消息协议（process.parentPort，desktop/server-process.mjs:15-20）：

| 方向 | 消息 | 字段 | 触发/处理 |
| --- | --- | --- | --- |
| 子→主 | listening | url, pid | 监听成功后发送（desktop/server-process.mjs:73）；主进程据此 resolve（desktop/main.mjs:241-246） |
| 子→主 | startup-error | message | 动态 import 或监听失败，100 ms 后以码 1 退出（desktop/server-process.mjs:74-78） |
| 子→主 | choose-project-file | id, currentProjectFile | 由 `setProjectFileChooser` 注册的函数发出（desktop/server-process.mjs:66-70）；对话框必须在主进程弹出 |
| 主→子 | choose-project-file-result | id, path, error | 按 id 完成 `pendingChoices` 中的 Promise（desktop/server-process.mjs:50-56） |
| 主→子 | shutdown | 无 | 调用 `shutdown(0)`：先 `stopEngines()`，再拒绝挂起的选择，`server.close` 并 `closeAllConnections`，1.5 s 兜底强退（desktop/server-process.mjs:35-45） |

`startDesktopServer(server)` 通过 `once('error')` 与 `once('listening')` 互相注销的写法把监听结果转成 Promise，端口固定为 0，返回 `http://127.0.0.1:<port>`；没有端口探测与重绑（desktop/local-server.mjs:18-35）。`server.mjs` 导出的 `server`、`stopEngines`、`setProjectFileChooser` 是这条链路的契约（server.mjs:2601,5646）；若改名必须同步修改 `server-process.mjs:62-65`。

#### 2.2.4 preload.cjs 与 settings.mjs

页面侧只能看到三个同步方法：`streamStudioSettings.read(key)`、`setItem(key, value)`、`removeItem(key)`，它们都是 `ipcRenderer.sendSync('stream-studio:settings', operation, key, value)` 的封装，失败即抛错（desktop/preload.cjs:3-13）。前端消费者是 `src/renderer/src/storage.js:14`。

主进程处理器在 desktop/main.mjs:353-362，只接受来自主窗口、主框架且同源的请求，结果放进 `event.returnValue`，形如 `{ ok: true, known, value }` 或 `{ ok: false, error }`。

`createDesktopSettings(directory)` 返回 `(operation, key, value)` 闭包（desktop/settings.mjs:32-71）：

- 键白名单 `KEYS` 共 9 个：语言（2 个）、示例/工程/可执行文件目录（3 个）与 4 个草稿键（desktop/settings.mjs:20-24）；不在表内的键一律抛“不支持的桌面设置”。该文件内一处 DEFERRED 注释写“10 个键”，与白名单实际 9 个不符（desktop/settings.mjs:61），以代码为准。
- 值只能是字符串或 `null`，单值上限 5 MB（UTF-8 字节）；`remove` 写入 `null` 墓碑，防止旧 origin 的 localStorage 值被再次迁移（desktop/settings.mjs:58-62）。
- 落盘：整体 JSON 先写 `renderer-settings.json.tmp` 再 `rename`；读到损坏文件时改名为 `.corrupt` 并以空设置继续（desktop/settings.mjs:36-49,66-67）。
- 性能注意：每次写都同步重写整个文件，且页面是 `sendSync`，会同时阻塞页面与主进程（desktop/settings.mjs:61）。

### 2.3 server.mjs：本地服务主体

`server.mjs` 是单文件服务，按功能自上而下分区。下表是分区地图，后面小节只展开需要解释的部分。

#### 2.3.1 分区地图与函数清单

| 分区 | 行号区间 | 主要函数/常量 |
| --- | --- | --- |
| 导入、路径根、环境变量与限值 | 30-132 | `envSetting`(64)、`parseTriangleLimit`(89)、`projectFacetLimit`(94)、`projectRunTimeoutMs`(108)、`HISTREAM_EXIT`(119)、`histreamResultCodes`(120)、`histreamCandidates`(124) |
| 全局运行状态与线程池 | 134-185 | `child`/`runPreparing`/`projectFile`/`projectDir`/`clients`(136-148)、`resultCache`(152)、`TASK_WORKERS`(154)、`heavyTasks`(161)、`runPostprocessTask`(169)、`streamService`(184) |
| 引擎定位、设备、文件哈希 | 188-280 | `executable`(188)、`allowedEngine`(196)、`listComputeDevices`(217)、`fileIdentity`(245)、`fileIdentityAsync`(264) |
| 运行目录与 manifest | 284-433 | `writeRunManifest`(284)、`verifyFrozenRunInputs`(296)、`runProductPaths`(310)、`createIsolatedRun`(329)、`processExists`(385)、`taskkill`(392)、`resetSimulationProcesses`(402) |
| JSON 响应与缓存读取 | 437-526 | `json`(445)、`JsonBytes`(440)、`encodeResultPayload`(451)、`produceResult`(459)、`sendCachedResult`(480)、`processResultDependencies`(518) |
| 路径安全 | 529-577 | `pathWithin`(529)、`realPathWithin`(538)、`sameResolvedPath`(556)、`sha256File`(569) |
| histream-proxy 部署检查 | 585-746 | `histreamProxyDeployment`(585)、`histreamProxyHealth`(727) |
| SSE 与请求安全 | 749-805 | `emit`(749)、`isLoopbackHostname`(765)、`foreignRequestReason`(776)、`body`(791) |
| 导入内容清洗 | 808-843 | `sanitizeObjContent`(808)、`normalizeSpectrumContent`(820) |
| 路径换算与工程路径 | 847-974 | `normalizeHostPath`(847)、`histreamRuntimePath`(857)、`windowsPath`(867)、`projectStoredPath`(877)、`normalizeStoredProjectPaths`(890)、`mapHistreamInputPaths`(912)、`resolveProjectFile`(930)、`projectFromSource`(953)、`projectAssetPath`(967) |
| 面元规模预检与预检接口 | 978-1283 | `objTriangleCount`(978)、`objMedianTriangleArea`(995)、`medianInstanceScale`(1026)、`facetPixelRatioNote`(1046)、`positionInstanceCount`(1066)、`estimateFacetTriangles`(1079)、`assertFacetScale`(1103)、`readZeroVisibilitySamples`(1136)、`facetResultWarnings`(1160)、`runPreflight`(1182) |
| 大气查找表与普朗克函数 | 1290-1405 | `loadAtmosphereLut`(1290)、`sampleAtmosphereLut`(1365)、`planckRadiance`(1396)、`inversePlanckTemperature`(1402) |
| 输出命名与过程结果读取 | 1409-1614 | `outputGeometrySuffix`(1409)、`isEnergyBalanceMode`(1416)、`processOutputSelection`(1422)、`outputTiffName`(1469)、`processResultMetadata`(1483)、`readProcessResult`(1502) |
| 面元结果→影像 TIFF | 1628-2459 | `writeRadiosityTiffForAngle`(1628-2357)、`writeRadiosityTiff`(2362)、`facetStepWorkerOptions`(2399)、`exportFacetStepOnce`(2408)、`exportFacetStep`(2430) |
| 工程文件读写与迁移 | 2462-2630 | `writeProject`(2502)、`readProject`(2517)、`migrateProject`(2556)、`openProject`(2583)、`setProjectFileChooser`(2601)、`chooseProjectFile`(2608) |
| USD 同步与导入导出 | 2634-2800 | `recordUsdSync`(2653)、`usdChangedExternally`(2666)、`usdNeedsBackup`(2675)、`exportProjectUsdFiles`(2689)、`openUsdProject`(2719)、`runUsdReader`(2787) |
| 新建/另存/示例 | 2806-2975 | `createProject`(2806)、`saveProjectAs`(2842)、`listExamples`(2917)、`openExample`(2937)、`resolveAsset`(2971) |
| 结果访问范围 | 2979-3058 | `resultRunManifest`(2979)、`resultDeletionScope`(2991)、`resultDirectoryAllowed`(3013)、`resultFilePathAllowed`(3036)、`userOpenPathAllowed`(3051) |
| TIFF 写出、ENVI 转换 | 3061-3369 | `writeRasterStatistics`(3076)、`writeFloatTiff`(3128)、`filenameToken`(3245)、`convertEnviToTiff`(3274)、`convertEnviResults`(3323)、`tiffObservation`(3344) |
| 结果列表与删除 | 3376-3634 | `resultDirectories`(3376)、`resultProductsForDeletion`(3406)、`deleteTiffResults`(3467)、`deleteThreeDimensionalResults`(3488)、`resultObservationContext`(3521)、`listResults`(3551) |
| ENVI/TIFF/DEM 读取 | 3639-4476 | `enviMetadata`(3639)、`unpackTiffLzw`(3670)、`undoTiffPredictor`(3715)、`twoPercentStretch`(3764)、`thermalSurfaceStretch`(3791)、`cachedResultSource`(3837)、`readTiff`(3859)、`readWindTiff`(4162)、`compareTiffResults`(4204)、`inspectAsciiGrid`(4312)、`demHeightSampler`(4379)、`inspectDemFile`(4402)、`readEnvi`(4423) |
| 路由分发 | 4490-5463 | `api`(4490)，路由表见 2.4 |
| 后台任务表 | 5468-5565 | `jsonPayload`(5468)、`toJsonBytes`(5472)、`HEAVY_TASKS`(5484)、`prepareRuntimeSceneInWorker`(5541)、`exportProjectUsdInWorker`(5557)、`runHeavyTaskInline`(5562) |
| 服务器、线程角色、退出 | 5573-5672 | `server`(5573)、角色分流(5602-5643)、导出(5646,5648)、`stopEngines`(5652)、`shutdown`(5665) |

导出：`server`、`stopEngines`、`setProjectFileChooser`（桌面进程使用）与 `convertEnviToTiff`、`enviMetadata`、`outputAngleToken`、`outputTimeToken`、`outputTiffName`（测试直接调用）（server.mjs:2601,5646,5648）。

#### 2.3.2 常量、环境变量与全局状态

- 路径根：`PROJECT_ROOT` 为 `server.mjs` 所在目录；`RESOURCE_ROOT` 取 `STREAM_STUDIO_RESOURCE_ROOT`（或旧名 `HISTREAM_STUDIO_RESOURCE_ROOT`），缺省等于 `PROJECT_ROOT`；`GUI_OUTPUT_DIR = <RESOURCE_ROOT>/gui` 是静态页面根（server.mjs:60-72）。
- 引擎候选顺序：`<RESOURCE_ROOT>/engine/histream.exe`，再依次 `models/bin_x64/Release|Debug`、`models/histream/bin/Debug|Release`；`executable()` 取第一个存在者，全不存在时返回首选路径供报错（server.mjs:124-130,188-190）。
- 限值：面元数量上限缺省 2000 万，优先级为工程 `control.maxFacetTriangles` > 环境变量 `STREAM_STUDIO_MAX_FACET_TRIANGLES` > 缺省，下限 1000（server.mjs:88-95）；单次运行超时缺省 15 分钟，可被工程 `control.runTimeoutMinutes`（1-1440）覆盖（server.mjs:106-111）。
- 退出码：`HISTREAM_EXIT = { ok:0, failed:1, facetNonConverged:2, resources:3, initialization:4, deviceQuery:5, usage:64 }`，必须与 `models/histream/src/main.cpp` 一致；只有 FacetEB 把码 2 视为“结果有效”（server.mjs:119-121）。
- 全局状态（模块级变量，非线程共享）：`child`（唯一的引擎子进程，同一时刻最多一个）、`runPreparing`、`runPreparationCanceled`、`resettingProcesses`、`histreamProxyRequestInFlight`、`projectFile`、`projectDir`、`projectFileChooser`、`clients`（SSE 连接）（server.mjs:136-148）。这些变量只存在于主线程；worker 中同名变量是各自独立的副本，不能依赖。
- 挂在 `ChildProcess` 对象上的运行标志：`histreamRun`、`histreamStopped`、`histreamReset`、`histreamTimedOut`、`histreamFinished`、`facetWorker`、`facetStepService`，由 `/api/run`、`/api/stop`、`/api/reset`、`stopEngines` 共同读写（server.mjs:5081,5092,5339,5195,2433）。
- 环境变量别名：`envSetting` 同时接受 `STREAM_STUDIO_*` 与旧前缀 `HISTREAM_STUDIO_*`（server.mjs:64-70），但 `STREAM_STUDIO_MAX_FACET_TRIANGLES`、`STREAM_STUDIO_RUN_TIMEOUT_MS`、`STREAM_STUDIO_TASK_WORKERS`、`STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS`、`STREAM_STUDIO_RESULT_CACHE_DEBUG` 直接读 `process.env`，没有旧前缀别名（server.mjs:93,106,155,2396,506）。

#### 2.3.3 请求入口、响应与安全

入口 `server = createServer(...)`（server.mjs:5573-5594）：路径以 `/api/` 开头先过 `foreignRequestReason`（Host 必须为 127.0.0.1、localhost 或 [::1]；有 Origin 必须是环回；`Sec-Fetch-Site` 仅允许 same-origin 或 none；非 GET/HEAD 必须是 `application/json`，server.mjs:765-787），再进 `api()`；`api()` 返回 `false` 表示无匹配，回 404 `API 不存在`。其余路径在非 `--api-only` 时按 `GUI_OUTPUT_DIR` 托管静态文件，不存在回退 `index.html`。详细的校验表与路径限制表见《运行原理说明》§1.4.2、§1.4.5，此处不再复述。

公共工具：

- `json(response, status, value)`：统一 JSON 响应并附 `X-Frame-Options: DENY`；`value` 为 `JsonBytes` 时直接发送后台线程已序列化的字节（server.mjs:437-448）。错误统一为 `{ error }`。
- `body(request)`：读取并解析 JSON 请求体，总量上限 160 MB；超过 4 MB 交 `heavyTasks.run('parse-json', …, { transfer: true })` 在后台线程解析；结果必须是 JSON 对象（server.mjs:757,763,791-805）。
- `emit(payload)`：向 `clients` 中所有 SSE 连接写 `data: <JSON>\n\n`（server.mjs:749-752）。STREAM 服务有自己独立的 `emit` 与客户端集合（server/stream/index.mjs:99-103），两条事件流互不相通。

#### 2.3.4 路径换算与路径安全

函数分工：

| 函数 | 作用 | 位置 |
| --- | --- | --- |
| `normalizeHostPath` | Linux 下 `X:\…` 转 `/mnt/x/…`，Windows 下 `/mnt/x/…` 转 `X:\…`，其余原样 | server.mjs:847 |
| `windowsPath` | `/mnt/x/…` 转盘符路径，用于返回前端与日志 | server.mjs:867 |
| `histreamRuntimePath` | 传给引擎的路径形式（Linux 下运行 .exe 时还原盘符） | server.mjs:857 |
| `projectStoredPath` | 写入工程文件的形式：工程目录内存相对路径（正斜杠），否则存 Windows 绝对路径 | server.mjs:877 |
| `projectAssetPath` | 读取时的解析：相对路径按工程目录；以 `assets/` 开头且工程内不存在时回落 `<RESOURCE_ROOT>/assets` | server.mjs:967 |
| `normalizeStoredProjectPaths` / `mapHistreamInputPaths` | 对工程中所有外部文件引用字段批量应用转换（前者含输出目录，后者不含输出目录与内置气象） | server.mjs:890,912 |
| `pathWithin` / `realPathWithin` | 字符串级与 realpath 级“在目录内”判断，后者可对不存在的目标检查最近祖先 | server.mjs:529,538 |

注意两者覆盖面不同：`mapHistreamInputPaths` 还处理 `atmosphere.lutFile`（server.mjs:917），而 `normalizeStoredProjectPaths` 没有该字段（server.mjs:890-906），因此大气查找表路径不会在保存时被改写为相对路径。新增任何“工程里引用外部文件”的字段时，必须同时加入 `normalizeStoredProjectPaths`（写入）与 `mapHistreamInputPaths`（另存为、结果删除范围）两处，否则会出现路径不随工程搬迁，或删除结果时误删输入的问题（server.mjs:890-926；`resultDeletionScope` 与 `resultProductsForDeletion` 都依赖 `mapHistreamInputPaths`，server.mjs:3000,3437）。

#### 2.3.5 工程读写、迁移与 USD 同步

工程读写调用链：

```
POST /api/project/open → openProjectOrUsd (2778)
    ├─ USD 扩展名 → openUsdProject (2719) → runUsdReader (2787) → buildProjectFromUsd → writeProject → openProject
    └─ 其他 → openProject (2583)
          resolveProjectFile (930) → readProject (2517) → migrateProject (2556) → 记入 projectFile/projectDir

POST /api/project/save → resolveProjectFile → validateProject → ensureDefaultSceneAsset (2541)
    → writeProject (2502) → [isUnifiedProject? applyHistreamToUnified : backupLegacyProject + histreamProjectToUnified]
    → writeUnifiedProject (2482，tmp + rename) → exportProjectUsdInWorker (5557) → exportProjectUsdFiles (2689)
```

要点：

- 工程文件统一为“统一工程”格式（`project-unified.js`），HiStream 段与 STREAM 段共存；`writeProject` 对已有统一工程只替换 HiStream 段，保留 STREAM 段；旧格式首次保存前备份为 `project.v<schemaVersion>.json`（server.mjs:2468-2479,2502-2513）。`readProject` 对 schemaVersion 高于 `PROJECT_SCHEMA_VERSION` 的旧格式直接报错，对 STREAM 独立工程（`stream-image-project`、`stream-scene-project`）提示改用遥感影像工作台（server.mjs:2526-2535）。
- `migrateProject` 只有在发生实际变化（补默认房屋、修复被误写成“…/C:…”的气象路径、路径规范化）时才回写，避免无谓改动文件（server.mjs:2556-2579）。`/api/run` 路径中刻意不调用它：运行不得改写源工程（server.mjs:4984-4986）。
- 文件选择：`chooseProjectFile` 优先使用桌面版注入的 `projectFileChooser`，否则回退到 PowerShell `OpenFileDialog`（server.mjs:2608-2630）。
- 另存为 `saveProjectAs`：拒绝保存到当前工程目录内部；复制 `models/positions/spectral/meteorology/terrain/images`（不复制 `output`）；`rebasePath` 把复制目录内的路径改相对，其余保持指向原文件的绝对路径，并同步改写 STREAM 场景路径与共享 DEM 路径（server.mjs:2842-2906）。
- 新建 `createProject`：目录须为绝对路径，名称禁止 `<>:"/\|?*` 与控制字符；`outDir` 写为绝对路径 `<工程>/output`（server.mjs:2806-2833）。
- 示例 `openExample`：示例只读，复制到 `~/Documents/Stream Studio/examples/<文件夹名>`，已存在则覆盖示例自带文件、保留 `output/`；若示例是 STREAM 工程则交 `streamService.openProjectFile`（server.mjs:2937-2968）。

USD 同步：每次保存都把工程镜像为同目录 `project.usd`。`usd/sync.json` 记录“本软件最后写出或导入的 USD 文件的 SHA-256”，格式为 `{ files: { <小写文件名>: <sha256> }, imported: { <小写文件名>: <sha256> } }`（server.mjs:2643-2662）。`usdChangedExternally` 用它判断是否被外部工具修改；`usdNeedsBackup` 在“无记录、哈希不符、或记录来自导入”三种情形之一成立时返回真，`exportProjectUsdFiles` 据此在覆盖前复制为 `project.external-<时间戳>.usd`（server.mjs:2666-2681,2689-2697）。打开 USD 时，若同目录已有 `project.json` 且 USD 哈希与同步记录一致，则直接打开工程，不重复导入；否则调用 `histream-usd.exe read` 产出 OBJ 与 `usd-import.json`，在写入工程前出错会删除本次创建的 `models/usd-import-*` 与 `positions/usd-import-*`（server.mjs:2719-2775）。

#### 2.3.6 运行编排：预检、启动、停止、重置

只列函数边界与调用链，运行时序见《运行原理说明》§2.4-§2.9。

- `runPreflight(input, project, mode)`（server.mjs:1182-1283）：由 `/api/run/preflight` 通过 `heavyTasks.run('run-preflight')` 调用，输出 `{ mode, nodes, angles, bands, timeoutSeconds, facet 或 voxel, estimateSeconds, estimateBasis, warnings }`。面元估算用 `estimateFacetTriangles`（地面初值 2 加各对象“三角形数乘实例数”，server.mjs:1079-1100）；显存系数 `RAYQUERY_BYTES_PER_TRIANGLE=53`、`RAYQUERY_SIDE_BYTES_PER_TRIANGLE=48` 与输出体积系数 `FACET_OUTPUT_BYTES_PER_TRIANGLE={json:580, binary:195}` 为粗估值（server.mjs:98-103）；A-buffer 与体元显存公式中的系数含义待核实（server.mjs:1210,1245）。
- `assertFacetScale`（server.mjs:1103-1118）是 `/api/run` 的硬拦截，`runPreflight` 只给警告；两者共用 `projectFacetLimit`。
- `createIsolatedRun`（server.mjs:329-382）：建立 `<outDir>/runs/<runId>`，`runId` 为 ISO 时间（冒号与点换连字符）加 8 位 UUID 片段；写 `source-project.json` 与初始 `run-manifest.json`；能量平衡模式的节点数上限 100000（server.mjs:350-353）。
- `/api/run`（server.mjs:4960-5326）的调用链：

```
互斥检查(resettingProcesses/child/runPreparing) → resolveProjectFile → readProject → normalizeStoredProjectPaths → validateProject
→ allowedEngine (196) → createIsolatedRun (329) → structuredClone 得 runProject（outDir 与气象路径改指向运行目录）
→ prepareRuntimeSceneInWorker (5541) → cpAsync(暂存目录 → <run>/input) → 冻结气象并写 runtimeInput 哈希
→ [面元] heavyTasks.run('facet-scale') → spawn(engine, args)
→ stdout/stderr → emit；[eFacetEB] readline 解析 FACET_CONVERGENCE / FACET_STEP → exportFacetStep (2430) → stdin 写 FACET_ACK
→ close → completeHistreamRun (5192) → runPostprocessTask('facet-tiff' 或 'envi-convert') → writeRunManifest → emit('closed')
```

- 引擎命令行：面元模式为 `[mode, 冻结工程, 结果 JSON]`，其余模式 `[mode, 冻结工程]`；FacetEB 以管道 stdin 与引擎握手，并设置环境变量 `HISTREAM_FACET_STEP_SYNC=1`（server.mjs:5054-5078）。
- 状态优先级：超时 > 用户停止 > FacetEB 未收敛 > 成功 > 失败，对应 manifest 的 `timeout`、`stopped`、`non_converged`、`completed`、`failed`（server.mjs:5264-5266）；清单还会出现 `preparing`、`running` 两个中间状态（server.mjs:357,5082）。
- 停止与重置：`/api/stop` 在准备阶段只置 `runPreparationCanceled`（`/api/run` 在每个 await 后检查），引擎已退出只剩后处理时不再按 PID 结束进程（PID 可能被复用）（server.mjs:5329-5350）。`resetSimulationProcesses` 只结束本服务自己的引擎进程树，杀后复核，失败则恢复 `child` 并抛错（server.mjs:402-433）。
- 超时计时器 `pauseRunTimer`/`resumeRunTimer` 在 FacetEB 逐节点出图期间暂停，使超时只统计引擎自身时间（server.mjs:5083-5118,5149,5168）。
- 失败清理：`/api/run` 外层 `catch` 清理运行时场景、把清单标为 `failed` 或 `stopped`，已启动的引擎被 `taskkill`；`finally` 释放 `runPreparing`（server.mjs:5302-5325）。

`run-manifest.json` 字段（server.mjs:356-370,5267-5271）：`kind:'histream-run'`、`schemaVersion:1`、`runId`、`mode`、`status`、`startedAt`、`finishedAt`、`outputDir`、`sourceInput`/`engine`/`meteorology`（均为 `{ path, bytes, sha256 }`，气象另含 `snapshot`）、`requested`（`nodes`、`nodeCount`、`viewAngles`、`observation`、`image`、`process`）、`completed`（`nodes`、`nodeCount`、`products`）、`build`、`runtimeInput`、`inputDirectory`、`pid`、`command`、`exitCode`、`signal`、`error`、`warnings`、`runWarnings`、`computedNodes`。写入一律经 `writeRunManifest`（先写 `.tmp` 再解析 realpath 后 `rename`，server.mjs:284-291）。

#### 2.3.7 面元结果出图与 ENVI 转换

- `writeRadiosityTiffForAngle`（server.mjs:1628-2357，约 730 行）：纯 CPU 光栅化，把面元结果投影到传感器像面并写多波段 float32 TIFF，附带统计 CSV 与过程量文件。头部注释给出五个步骤：读取结果与工程、建立相机与大气函数、三角形光栅化、沿太阳方向的阴影光栅、取值叠加与写盘（server.mjs:1616-1627）。该函数内部细节本次只核对了头部与收尾部分，其余逐行逻辑待核实。
- `writeRadiosityTiff`（server.mjs:2362-2387）：遍历所有观测视角（透视投影只有一个），只有第一个视角写过程量，最后一个视角后清理逐步文件；逐步导出（`step` 非空）时同一节点的各视角共享几何与太阳阴影光栅，全部视角成功后才删除 `.facet_steps/energy_T=*.bin`。
- 节点出图 worker：面元数不超过 `FACET_STEP_RESIDENT_FACETS`（缺省 300 万）时使用常驻的 `facet-step-service` 线程，几何只解析一次；更大时每节点新建一次性 `facet-step` 线程，等其退出后才让引擎继续（server.mjs:2396-2459,5613-5635）。
- 运行结束后的出图/转换用 `runPostprocessTask` 开一次性线程（任务 `facet-tiff`、`envi-convert`），`running.facetWorker` 保存线程句柄，供“停止”终止（server.mjs:169-181）。
- ENVI 转换：`convertEnviToTiff` 只接受 float32、小端、BSQ、`header offset=0`；输出名由文件名中的 T、SZA、SAA、VZA、VAA 经 `outputTiffName` 生成；仅 `eVoxelRT`、`eVoxelEB`、`eRaytracing` 转换成功后会删除原 `.hdr`/`.img`（server.mjs:3274-3317）。`convertEnviResults` 逐文件收集失败，只有全部失败才使运行失败（server.mjs:3323-3339,5249-5251）。
- TIFF 写出 `writeFloatTiff`（server.mjs:3128-3240）：经典 TIFF、小端、单条带、像素交错、float32；多波段多写 ExtraSamples(338)，有太阳角时写 GDAL_METADATA(42112)；先写 `.partial` 再 `rename`；偏移 32 位，文件不得超过 4 GiB；写完后调用 `writeRasterStatistics` 更新同目录 `result_statistics_<mode>.csv`（server.mjs:3076-3118）。波段名保存在 ImageDescription（`HiStream image bands: {…}`，UTF-8），`readTiff` 依赖它恢复波段名（server.mjs:3165,4045）。
- 文件名约定：EB 模式 `T=<DOYddd_HH-MM>_VZA=_VAA=`，RT 模式 `SZA=_SAA=_VZA=_VAA=`，后缀 `_f`（面元）、`_v`（体元）、`_a`（大气校正）；`filenameToken` 与 `tiffObservation` 反向解析这些字段，改命名必须同时改三处（server.mjs:1469-1479,3245-3259,3344-3369）。

#### 2.3.8 结果列表、读取与删除

- 访问范围：`resultDirectoryAllowed`（列表/删除）与 `resultFilePathAllowed`（读取/变化分析）由 `resultDeletionScope` 计算的工程输入集合与输出根决定，输入保护优先（server.mjs:2991-3047）。`resultRunManifest` 只认 `kind` 为 `histream-run`、`stream-image-run`、`stream-scene-run` 且 `outputDir`（或 `runId` 与目录名）与目录一致的清单（server.mjs:2979-2987）。
- 列表 `listResults`（server.mjs:3551-3634）：收集目录与 `process/` 下的 `.tif/.tiff/.json/.csv`，分类为 `tiff`、`text`、`facet`、`process`，丢弃 `file`；`fluid_wind_` 开头的 TIFF 标记为风场 `resultType:'wind'`；存在面元过程时间序列时不再单独列 `faceteb.json`；递归汇总 `runs/<runId>`（不跟随符号链接）。
- 读取：`/api/results/read` 按扩展名分派到 `sendCachedResult`（envi/tiff/wind/facet/process）、图像 base64（≤ 32 MB）、文本（≤ 4 MB）或仅返回路径（server.mjs:5394-5430）；缓存机制见 2.5.2。
- 删除：`deleteTiffResults` 与 `deleteThreeDimensionalResults` 只删除 `run-manifest` 的 `completed.products`（或 `products`）中列出、位于结果目录内且不与任何输入文件重合的产物；找不到输入快照时宁可整个运行都不删；快照解析失败直接抛错（server.mjs:3406-3513）。删除前先清空主线程 `resultCache` 并向各 worker 广播 `{type:'clear'}`（server.mjs:5368-5369）。
- 变化分析：`compareTiffResults`（server.mjs:4204-4304）要求尺寸一致，两幅都带地理参考时 CRS、原点、像元大小也要一致（容差 1e-8）；用 Welford 在线公式求均值、MAE、RMSE、相关系数，抽取最多 100-10000 个散点，差值预览为蓝-白-红。

#### 2.3.9 TIFF、ENVI、DEM 读取

`readTiff(path, band, includeValues)`（server.mjs:3859-4157）是结果查看、DEM 检查、STREAM 栅格检查与变化分析共用的解码器：

- 支持：经典 TIFF（II/MM）、条带或瓦片、压缩 1/5(LZW)/8/32946(Deflate)/32773(PackBits)、predictor 2/3、planar 1/2、位深 1/8/16/32/64、Orientation 1-8；不支持 BigTIFF（server.mjs:3852-3853,3868）。Deflate、LZW、PackBits 都带最大输出限制以防解压炸弹（server.mjs:3970-3980）。
- 读盘缓存：`cachedResultSource` 以 `fileStamp` 为键把整文件字节与已解码分块放入 `resultCache`（键前缀 `source:`），读取前后文件身份不一致则不缓存（server.mjs:3837-3846）；分块缓存增长后用 `resultCache.resize` 重新计费（server.mjs:3996-3999）。
- 预览：最长边不超过 768 的最近邻降采样，默认按 2%-98% 分位拉伸；波段被判为热红外（名称含 temperature/热红外/thermal，或名称中 @ 波长 ≥ 3000 nm）时，把 ≤ 0 的值计为无效，改用 `thermalSurfaceStretch`，并计算高温热点叠加（server.mjs:4043-4117）。拉伸间隙阈值的单位含义待核实（server.mjs:3787）。
- 返回字段：`kind:'tiff'`、尺寸、`bands`、`bandNames`、`pixels`（base64 灰度）、`minimum`/`maximum`、拉伸范围与模式、`crs`、`unit`、`pixelSizeX/Y`、`originX/Y`、`georeferenced`、`terrainValues`（最长边 ≤ 96 的粗网格）；`includeValues` 为真时附完整 `values`（Float64Array）（server.mjs:4138-4156）。
- 相关：`readEnvi`（server.mjs:4423-4476）支持 bsq/bil/bip 与 7 种数据类型但不返回数值；`readWindTiff` 要求 4 波段，抽样约每行 28 支箭头（server.mjs:4162-4194）；`inspectAsciiGrid` 解析 ESRI ASCII Grid，像元数上限 2500 万（server.mjs:4312-4368）；`demHeightSampler` 与引擎 `demHeightAboveMinimum` 保持同样的双线性与轴约定，供 USD 导出和运行时场景使用（server.mjs:4379-4398）。

STREAM 的栅格头读取 `readTiffHeader`（server/stream/rasters.mjs:33）与上述解码器是两份独立实现，对 SSHORT、SLONG、RATIONAL 类型字段的处理不一致，源码已标注为待收敛（server/stream/rasters.mjs:62）。

#### 2.3.10 histream-proxy 与 STREAM 的挂接

- `histreamProxyDeployment()`（server.mjs:585-723）读取部署清单，并做六类校验：清单格式与 `native_engine_invoked===false`；所有模型/体数据路径在 `artifact_root` 内；清单中的 runner 与 python 必须等于环境变量配置的受信任路径；固定 4 个热状态体数据；源输入 17 个与抓取输入的 SHA 清单必须与规范文件集合完全一致；逐文件核对 SHA-256。`ready` 为真才允许运行。`histreamProxyHealth` 把结果整理给前端（server.mjs:727-746）。三个环境变量 `STREAM_STUDIO_PROXY_MANIFEST/RUNNER/PYTHON` 缺省均为空串，未配置即视为未启用（server.mjs:80-82,586-589）。
- STREAM 服务：`createStreamService`（server.mjs:184）在主线程与每个 worker 中都会被调用，但只有主线程的实例对外收请求；`api()` 第一行先交给 `streamService.handle`，返回 `false` 才继续匹配 HiStream 路由（server.mjs:4491-4492）。关闭时 `stopEngines` 调用 `streamService.shutdown()`（server.mjs:5661）。

#### 2.3.11 后台任务表与线程角色

`HEAVY_TASKS`（server.mjs:5484-5536）共 18 个任务，调用方式是 `heavyTasks.run(名称, 参数, { key, transfer })`；线程不可用时 `runHeavyTaskInline` 同步执行，且只认自有属性（server.mjs:5562-5565）。

| 任务名 | 作用 | 典型调用者 |
| --- | --- | --- |
| parse-json | 解析超过 4 MB 的请求体 | `body` (801) |
| result-read | 读取并编码结果（TIFF/ENVI/面元/过程） | `sendCachedResult` (491) |
| result-image | 图像结果转 base64 | `/api/results/read` (5418) |
| results-change | TIFF 变化分析 | `/api/results/change` (5442) |
| project-read | 读取场景资源文本（≤ 80 MB） | `/api/project/read` (4898) |
| dem-info / dem-inspect | DEM 元数据（前者返回 JSON 字节，后者返回对象） | `/api/project/dem-info`、`import-dem` |
| tarp-cover | 生成篷布 OBJ | `/api/project/tarp-cover` (4704) |
| facet-tiff | 面元结果转 TIFF | `completeHistreamRun` (5222) |
| envi-convert | ENVI 转 TIFF | `completeHistreamRun` (5246) |
| stream-raster / stream-class-stats | STREAM 栅格取样与分类统计 | `server/stream/index.mjs:665,673` |
| run-preflight | 运行前预检 | `/api/run/preflight` (4944) |
| results-list | 结果列表 | `/api/results/list` (5383) |
| runtime-scene | 运行时场景准备 | `prepareRuntimeSceneInWorker` (5542) |
| facet-scale | 面元规模与像元比提示 | `/api/run` (5037) |
| usd-export | 导出 project.usd | `exportProjectUsdInWorker` (5558) |
| import-obj | 导入 OBJ | `/api/project/import-obj` (4717) |

线程角色由 `workerData.kind` 决定（server.mjs:5602-5643）：`facet-step`（一次性节点出图）、`facet-step-service`（常驻节点出图）、`task-service`（线程池成员，缓存上限为总缓存除以线程数，server.mjs:5637）、`task-once`（一次性任务，缓存上限 0，server.mjs:5641）。主线程且 `STREAM_STUDIO_SERVER_AUTOSTART` 不为 `0` 时才监听端口（缺省 4173，server.mjs:5602-5611）。

任务约定：参数与返回值必须可结构化克隆；大结果用 `jsonPayload` 预序列化为 `{ bytes }`，主线程用 `toJsonBytes` 包装后由 `json()` 直接发送，避免二次 `JSON.stringify`（server.mjs:5468-5474）。带 `key` 的任务按 key 固定到同一线程，使该线程内的 TIFF 解码缓存命中。

### 2.4 /api 路由总表

分发顺序：`streamService.handle` → 下表自上而下的 `if (method && pathname)`。所有 HiStream 路由的错误体均为 `{ error }`，状态码以 400 为主；表中“处理”列给出 `api()` 内的分支起始行（均在 server.mjs）。

| 方法 | 路径 | 处理 | 请求/响应要点 |
| --- | --- | --- | --- |
| GET | /api/histream-proxy/info | 4494 | 返回 `histreamProxyHealth(true)`（含支持帧列表） |
| GET | /api/histream-proxy/result | 4499 | `file` 参数须匹配 `<名>_frame-NNNN.npz` 且位于 `artifactRoot` 内；附件下载，不缓存 |
| POST | /api/histream-proxy/run | 4527 | 只接受 `captureId`、`episodeId`、`frameIndex`；拒绝 `inputPath/projectPath/mode/executable`；同时只允许一个（409）；runner 超时 120 s；失败不回退原生引擎 |
| GET | /api/defaults | 4598 | 引擎路径与存在性、当前工程、`runTimeoutSeconds`、`appVersion`、`streamExecutable`；桌面版用它探测就绪 |
| POST | /api/help/open | 4603 | `document` 仅 `user` 或 `theory`；Windows 下 `explorer` 打开 PDF |
| GET | /api/devices | 4615 | 查询 `executable`、`refresh=1`；经 `allowedEngine` 校验；缓存设备列表 |
| GET | /api/events | 4625 | SSE；先发 `: connected` 与一条 `status`；事件类型 `status/started/stdout/stderr/error/closed` |
| GET | /api/examples | 4636 | `{ examples, directory }` |
| POST | /api/examples/open | 4643 | 运行中拒绝；复制示例并打开 |
| POST | /api/project/open | 4652 | `{ path, mode }`；`.usd*` 走导入 |
| POST | /api/project/pick | 4660 | 弹文件对话框，取消返回 `{ cancelled: true }` |
| POST | /api/project/create | 4671 | 返回 201，并切换当前工程 |
| POST | /api/project/save-as | 4682 | 返回 201，切换当前工程，附 `usd` 导出结果 |
| POST | /api/project/tarp-cover | 4695 | 在 `models/<name>.obj` 生成篷布，名称字符受限 |
| POST | /api/project/import-obj | 4709 | 仅 `.obj`，≤ 80 MB，按工程目录为 key 串行 |
| POST | /api/project/import-meteo | 4722 | TXT/METEO/DAT/CSV，≤ 20 MB，写 `meteorology/` |
| POST | /api/project/import-dem | 4746 | `contentBase64`，≤ 64 MB，写 `terrain/`，附 `metadata`、`warnings` |
| POST | /api/project/dem-info | 4783 | `.tif/.tiff/.asc` 元数据 |
| POST | /api/project/import-spectrum | 4802 | 经 `normalizeSpectrumContent` 校验后写 `spectral/` |
| GET | /api/project/image | 4823 | 只读 PNG/JPG（≤ 32 MB），限当前工程或 assets |
| POST | /api/project/import-texture | 4842 | PNG/JPG/TIF，校验魔数，写 `textures/`，同名覆盖 |
| POST | /api/project/distribution | 4866 | 写 `positions/*.txt`，每行 `x y z scale rotation`，≤ 100000 个实例 |
| POST | /api/project/read | 4889 | 读场景资源（≤ 80 MB），限当前工程或 assets |
| POST | /api/project/save | 4905 | 须在当前工程目录内；返回 `content`、`project`、`usd` |
| POST | /api/run/preflight | 4933 | `{ mode, inputPath }` → `runPreflight` |
| POST | /api/run | 4960 | `{ mode, inputPath, executable }` → `{ pid, inputPath, runId, outputDir }` |
| POST | /api/stop | 5329 | `{ stopped, preparing? }` |
| POST | /api/reset | 5352 | `{ activePid, terminated }`，失败 500 |
| POST | /api/results/delete-all | 5365 | `kind==='3d'` 删三维结果，否则删 TIFF 与统计表；运行中拒绝 |
| POST | /api/results/list | 5377 | `{ directory, files[] }` |
| POST | /api/results/read | 5394 | `{ path, band }`；`Accept` 含 `application/vnd.stream-studio.result-v1` 时可返回二进制块；响应头 `X-Cache: hit 或 miss` |
| POST | /api/results/change | 5432 | `{ firstPath, secondPath, band, maxScatterPoints }`，仅 TIFF |
| POST | /api/open | 5449 | 在资源管理器中打开（文件则选中）；拒绝 UNC |

STREAM 路由表在 `server/stream/index.mjs` 的 `routes` 对象内，键为“方法 路径”，成功 200，异常 400 `{ error }`（server/stream/index.mjs:658-714）：

| 方法 | 路径 | 处理 | 要点 |
| --- | --- | --- | --- |
| GET | /api/stream/status | 659 | 引擎路径与就绪、默认输出根、当前与排队任务 |
| GET | /api/stream/events | 702 | SSE，先推一条 `status`；事件 `job-queued/started/progress/log/closed` |
| POST | /api/stream/run | 660 | `submitRun`：提交期校验，入队，返回任务摘要与 `warnings` |
| POST | /api/stream/stop | 661 | `{ id }` |
| POST | /api/stream/inspect | 662 | `{ paths }`，最多 32 个栅格 |
| POST | /api/stream/raster | 664 | `{ path, band }`，后台线程任务 `stream-raster` |
| POST | /api/stream/runs | 667 | `{ outputRoot }`，最近 50 条 |
| POST | /api/stream/run-info | 668 | `{ outputDir }` → 清单、摘要、配置、产品 |
| POST | /api/stream/class-stats | 669 | 按地表类型分类统计，后台线程任务 |
| POST | /api/stream/project/open | 676 | 只打开 STREAM 独立工程 |
| POST | /api/stream/project/save | 681 | 写 `stream-project.json` |
| POST | /api/stream/era5/list、register、remove | 682-684 | 气象数据包列出、登记、移除 |
| GET | /api/stream/forcing/presets | 685 | 引擎预设名与内置列表 |
| POST | /api/stream/forcing/preset、check、register | 686-688 | 预设内容、元数据校验、登记（与 era5/register 同一函数） |
| POST | /api/stream/forcing/metadata/save、read | 689-690 | 工程元数据文件读写 |
| POST | /api/stream/pick | 691 | Windows 浏览对话框，`kind` 为 file/folder/project/era5/station/metadata |

### 2.5 server/：服务端支撑模块

#### 2.5.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| server/task-pool.mjs | 286 | worker 线程池协议：常驻池、一次性线程、字节转移 | `createTaskPool`、`runTaskOnce`、`serveTasks`、`serveTaskOnce`、`ownedBytes`、`receivedBytes` |
| server/result-cache.mjs | 230 | 按字节计费的 LRU 与二进制结果编码/解码 | `ByteLru`、`fileStamp`、`resultCacheLimitBytes`、`encodeBinaryResult`、`decodeBinaryResult`、`BINARY_RESULT_TYPE` |
| server/facet-result.mjs | 677 | 面元结果（JSON 与二进制两种格式）读取与旁路文件管理 | `readFacetResult`、`sampleFacetResult`、`isFacetResultFile`、`facetResultFormat`、`facetResultCount`、`facetSidecarFiles`、`facetResultBytes`、`copyFacetSidecars`、`removeFacetResult`、`readLargeFileSync`、`MAX_RESULT_BYTES`、`THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT` |
| server/stream/index.mjs | 734 | STREAM 服务入口：路由、串行队列、SSE、引擎进程、气象数据包 | `createStreamService`、`unreachableNonAsciiPackFile` |
| server/stream/config.mjs | 647 | STREAM 工程读写与引擎配置生成/校验 | `createProjectStore`、`mapStreamScenePaths`、`migrateScene`、`localToUtc`、`validateEnergy`、`unknownEnergyFields`、`ENERGY_KEYS`、`PROJECT_FILE` |
| server/stream/forcing.mjs | 211 | 气象强迫元数据规则与引擎配置块生成 | `createForcingRules`、`FORCING_PRESETS`、`FORCING_METADATA_FILE`、`isPresetName`、`looksLikeMetadataPath`、`isPlainObject`、`isInside` |
| server/stream/rasters.mjs | 234 | STREAM 栅格头检查、波段取样、分类统计 | `createRasterTools`（`inspectRasters`、`rasterBand`、`classStatistics`） |

#### 2.5.2 task-pool.mjs 与 result-cache.mjs

线程池协议（server/task-pool.mjs:11-12）：主→线程 `{ id, task, args }` 与 `{ type:'clear' }`；线程→主 `{ type:'ready' }` 与 `{ id, ok, result 或 error }`，一次性线程的回包不带 `id`。

- `createTaskPool({ createWorker, runInline, size=2, idleMs=300000 })`（server/task-pool.mjs:86）：线程懒创建，带 `key` 的任务用 FNV-1a 哈希固定线程（`keyIndex`，server/task-pool.mjs:74），无 key 选负载最小的槽位（`pick`）；空闲 5 分钟回收（`armIdle`）；线程在 `ready` 之前就失败累计 2 次（`STARTUP_RETRIES`）后永久退回主线程内联执行并只打印一次日志（`fallback`）；运行期崩溃只让该任务以 `crashError` 失败，下次任务重建线程。对外：`run`、`broadcast`、`inline`、`size`、`terminate`。
- `runTaskOnce`：线程退出后才 resolve，确保堆与 ArrayBuffer 已释放；未 ready 就退出则回退内联执行（server/task-pool.mjs:225-254）。
- 字节转移：`result.bytes` 为独占整块 ArrayBuffer 的 `Uint8Array` 时才转移，否则结构化复制；`ownedBytes` 保证可转移，`receivedBytes` 在主线程包成共享内存的 `Buffer`（server/task-pool.mjs:36-60）。转移后发送方不得再使用该缓冲。
- 线程端：`serveTasks`、`serveTaskOnce` 用 `Object.hasOwn` 查任务表，防止原型链名称被调用（server/task-pool.mjs:258-286）。

结果缓存：

- `fileStamp(path)` 返回 `{ path, mtimeMs, size, key }`，`key` 为 `<绝对路径>|mtime|size|ctime`，ctime 是为了让“保持 mtime 与 size 的原地改写”也失效（server/result-cache.mjs:45-55）。
- `ByteLru`：`Map` 保持插入顺序，`get` 命中后重新插入；`set(key, value, bytes, stamps)` 在同一路径出现新身份时淘汰依赖旧身份的其他条目，单条超过上限则拒收，其后从最旧端淘汰；`resize` 供条目增长后重新计费（server/result-cache.mjs:60-127）。总上限由 `STREAM_STUDIO_RESULT_CACHE_MB` 决定，缺省 512 MB，非正数关闭缓存（server/result-cache.mjs:37-41）。
- 两层缓存：主线程 `resultCache` 存已编码的接口响应（键 `result:<variant>|<binary 或 json>|<各依赖文件 key>`，server.mjs:484）；worker 内的 `resultCache` 存原始 TIFF/ENVI 字节与已解码分块（键 `source:<key>`，server.mjs:3839），上限为总设置按线程数均分。`sendCachedResult` 对同键并发请求用 `resultInFlight` 合并（server.mjs:475-503）。
- 二进制结果格式 SSR1（小端）：`'SSR1'` 魔数 4 字节；uint32 头部 JSON 长度 L；`{ value, buffers }` 头部 JSON；数据区起点为 `ceil((8+L)/8)*8`，各数组按 8 字节对齐。被抽走的数组在 `value` 中换成 `{ $ssBuffer: 序号 }`，`buffers[i] = { type: 'f32' 或 'f64', offset, length }`。只有键名为 `vertexPositions`、`voxelPositions`、`values` 且长度不小于 256 的纯数值数组才会被抽出；全部值可用 float32 精确表示时用 f32，否则 f64；null 记为 NaN（server/result-cache.mjs:132-207）。前端解码在 `src/renderer/src/web-api.js`，Node 侧解码为 `decodeBinaryResult`（server/result-cache.mjs:211）。

#### 2.5.3 facet-result.mjs

对外只有一套接口，内部按格式分派（server/facet-result.mjs:650-677）：`readFacetResult(jsonPath, geometryOnly)` 返回类型化数组形式的完整结果；`sampleFacetResult(jsonPath, maximumFacets, geometryOnly)` 返回等间隔抽样的普通数组，用于三维显示；`isFacetResultFile` 判断 `.json` 是否为面元结果。两种格式返回完全相同的结果形状。

- JSON 格式（`readJsonFacetResult`，server/facet-result.mjs:52）：不把整个文件转为 JS 字符串（受 V8 字符串长度上限限制），而在 Buffer 上手工扫描键并逐数字解析；数组必须按引擎写出顺序读取（`searchFrom` 单向前进），指标顺序为 `sunlit, radiosity, lightEnhancement, temperature, netRadiation, sensibleHeat, latentHeat, storageHeat, photovoltaicPower`（server/facet-result.mjs:134-149）。每个面元有正反两个面，指标数组长度为 `2×facetCount`。
- 二进制格式（`control.facetOutputFormat="binary"`）：元数据 JSON 中 `format:'binary'`，大数组是描述符 `{ file, offset, length, dtype:'float32', shape }`，数据在同目录 `.bin` 旁路文件（小端 float32）；`formatVersion` 大于 1 报错；旁路文件名必须是同目录纯文件名（`sidecarName` 拒绝路径分隔符）；只读调用方需要的数组，抽样时对密集记录一次读入最多 8 MB 窗口（server/facet-result.mjs:319-373,433-494,569-591）。元数据解析结果按 (size, mtime) 缓存最多 8 个（server/facet-result.mjs:353-366）。格式规范见 `docs/facet-binary-output.md`。
- 旁路文件管理：`facetSidecarFiles`、`facetResultBytes`、`copyFacetSidecars`、`removeFacetResult`；`removeFacetResult` 先删旁路再删 JSON，并清理同主名 `.bin`（server/facet-result.mjs:376-419）。
- 大文件：`readLargeFileSync` 分块读入自分配的 Buffer（`readFileSync` 对 2 GiB 以上文件会抛错），超过 `MAX_RESULT_BYTES`（缺省 8 GiB，`STREAM_STUDIO_MAX_RESULT_BYTES` 可调）时拒绝并提示减少面元（server/facet-result.mjs:16,23-44）。
- 三维显示抽样上限 `THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT = 1000000`，磁盘上的结果保留全部面元（server/facet-result.mjs:18）。
- 两处指标清单不完全相同：完整读取含 `photovoltaicPower`，JSON 抽样路径没有（server/facet-result.mjs:145,262-271），改指标时须同时检查四处清单。

调用方：`server.mjs` 的 `readRadiosityResult`、`sampleRadiosityResult`、`isRadiosityResultFile`、`facetResultCount` 等；`tools/export-faceteb-frames.mjs` 与测试也直接导入（tools/test-facet-binary-reader-20261126.mjs:26）。

### 2.6 server/stream/：STREAM 影像模拟服务

#### 2.6.1 index.mjs

`createStreamService({ resourceRoot, modelRoot, json, body, readTiff, normalizeHostPath, windowsPath, runTask, documentsRoot })`（server/stream/index.mjs:77）返回 `{ handle, shutdown, executable, openProjectFile }`（server/stream/index.mjs:733）。内部状态为 `clients`、`queue`、`active`、`nextId`，同一时刻最多一个引擎进程。

核心函数调用关系：

```
POST /api/stream/run → submitRun (287)
    assertEngine → buildEngineConfig(提交期校验) → sceneWarnings → unreachableNonAsciiPackFile → enqueue (160)
enqueue → setImmediate(pump) → job.start()
    start: 建输出目录 <输出根>/<名称>_<时间戳> → buildEngineConfig(正式) → 写 run-config.json、scene.json、run-manifest.json → launch (206)
launch: spawn(stream.exe run <config>) → stdout 进度行 STREAM<TAB>progress<TAB>百分比<TAB>阶段 → emit
    close → finish (185) → writeManifest → job.collect → emit('job-closed') → pump
POST /api/stream/era5/register 或 forcing/register → registerPack (427) → enqueue(job.kind='era5') → launch(era5-register 或 forcing-register)
```

要点：

- 引擎候选：`<resourceRoot>/engine/stream.exe`、`models/bin_x64/Release|Debug/stream.exe`；参数目录 `stream-defined` 位于引擎同目录，`status()` 检查 `classes.json`、`optipar_new.csv`、`Esun_.dat`、`Esky_.dat` 四个文件（server/stream/index.mjs:79-95,123-131）。
- 目录：`documents` 缺省 `~/Documents/Stream Studio`，下设 `era5-packs/`（气象数据包索引与 `metadata/` 副本）、`forcing-metadata/`、默认输出根 `stream/`（server/stream/index.mjs:84-97）。
- 任务摘要对象只暴露 `id、kind、label、status、progress、stage、outputDir、pid、startedAt`（server/stream/index.mjs:118-119）。任务对象约定：`kind`、`label`、`successCodes`、`start()`、`collect(status)`、可选 `cleanup()`。
- 看门狗：`STREAM_STUDIO_STREAM_TIMEOUT_MS` 缺省 2 小时，0 关闭；超时用 `killChildTree` 终止后以 failed 收尾（server/stream/index.mjs:43-45,217-226）。
- 只有 `completed` 状态的运行才列出产品文件，避免被杀进程留下的半个 `.tif`（server/stream/index.mjs:147-149）。
- `registerPack` 的命令行超过 30000 字符时改用 `--files-from <清单文件>`，规避 Windows 命令行长度限制；通用来源（元数据不是 era5、era5-land、auto）走 `forcing-register`，内联对象先存为 `era5-packs/metadata/pack-<时间戳>.metadata.json`（server/stream/index.mjs:427-486）。
- 提交期检查 ERA5 数据文件路径的非 ASCII 可达性：路径含非 ASCII 且无 8.3 短名时拒绝，因为引擎只能通过短名打开（server/stream/index.mjs:50-62）。
- `checkForcing` 调用 `stream forcing-check --json`，退出码 0 与 3 都视为得到报告，码 3 的含义待核实（server/stream/index.mjs:565）。
- 已登记的 DEFERRED 项：队列不去重、输出根不限制到工程目录、浏览对话框无超时与并发限制、`listRuns` 对每个目录同步全量读取（server/stream/index.mjs:159,292,373,645）。

#### 2.6.2 config.mjs

- `createProjectStore({ normalizeHostPath, windowsPath, forcingDirectories })` 返回 `{ readProjectFile, saveProjectFile, buildEngineConfig, sceneWarnings }`（server/stream/config.mjs:189,646）。
- 工程文件：`stream-project.json`，`kind:'stream-image-project'`、`schemaVersion:2`、`name`、`description`、`scene`；旧版 `stream-scene-project`（v1）经 `migrateScene` 迁移，统一工程（`project.json`）不作为 STREAM 工程打开（server/stream/config.mjs:201-222,251）。保存时 `mapStreamScenePaths` 把工程目录内路径改相对、目录外保持绝对；覆盖已有文件前必须确认它本身是 STREAM 工程；先写 `.<pid>.tmp` 再 `rename`（server/stream/config.mjs:228-262）。
- `mapStreamScenePaths(scene, convert)`（server/stream/config.mjs:161）是路径改写的唯一入口，覆盖栅格字段、组分文件、ERA5/强迫数据包、站点文件、元数据路径、同化数据与各视角文件，以及 `outputRoot`（第二参数为 `'output'`）；`server.mjs` 的另存为与结果删除范围也依赖它（server.mjs:2898,3001,3438）。
- `buildEngineConfig(scene, { outputDir, dataDir })`（server/stream/config.mjs:425-637）：先校验过境时刻（严格 UTC，年份 1900-2200，`validUtcParts` 拒绝 2019-02-30 与 T24:00），再依次处理模拟类型与波段（光学 400-2400 nm、热红外 3-20 μm，≤ 64 个）、地表输入、地形模式、观测几何、温度来源（EB 或 RT）、反演、同化、天空长波、区域，输出 `kind:'stream-image-config'`、`schemaVersion:2`。EB 分支调用 `forcingRules.buildForcing`；RT 分支要求四组分温度（150-400 K 常数检查，防止把摄氏度当开尔文）。校验范围需与 `models/stream/app/config.cpp` 保持一致，且全部在提交期完成，使错误不在出队、建目录之后才暴露（server/stream/config.mjs:11-12）。
- `validateEnergy` 只拒绝引擎本身会拒绝的取值；`ENERGY_KEYS` 之外的 `temperature.energy` 字段不报错，只在 `sceneWarnings` 中提示（server/stream/config.mjs:71-109,639-644）。

#### 2.6.3 forcing.mjs 与 rasters.mjs

`createForcingRules` 返回 `{ buildForcing, resolveMetadata, readMetadataFile, importMetadataFile, checkDocument, existingFile, packFile, stationFiles, absolutePath, assertInlineSize, formOf }`（server/stream/forcing.mjs:210）。

- 元数据来源三种：预设名（`FORCING_PRESETS` 五个）、`.json` 文件路径、内联对象；内联对象 ≤ 64 KiB，元数据文件 ≤ 256 KiB，站点文件 ≤ 64 个，路径禁止 `..` 与 NUL（server/stream/forcing.mjs:28-33,57-63）。元数据文件只允许位于工程目录、数据包目录或登记的元数据目录内（`metadataFile`，server/stream/forcing.mjs:101-113）；`importMetadataFile` 只用于把任意位置的文件读回编辑器，不交给引擎（server/stream/forcing.mjs:198-208）。
- `buildForcing` 的取舍：`forcing.pack` 与旧字段 `era5.pack` 同时存在时以 forcing 为准且不再输出 era5 块，与引擎取值顺序一致；站点形态需要站点 CSV 或元数据中登记的 `station.stations[].file`，网格形态必须有数据包（server/stream/forcing.mjs:169-194）。
- `rasters.mjs`：`inspectRasters`（最多 32 个，头部探测不解码像元）、`rasterBand`（步长 `ceil(max(宽,高)/2048)` 块取样，返回 Float32 的 base64、统计量与 96 档直方图，步长大于 1 时 count、最值、均值、标准差改用全部像元重算，分位数仍基于样本）、`classStatistics`（按 IGBP 类型栅格逐类统计各产品各波段的均值、标准差与面积，面积单位 km2，经纬度栅格逐行按球面计算）（server/stream/rasters.mjs:100-230）。`classStatistics` 对每个波段各调用一次 `readTiff`，多波段产品会重复解码，源码已记为性能待改进（server/stream/rasters.mjs:203）。

### 2.7 数据结构与文件格式汇总

| 文件 | 位置 | 格式要点 | 写入者 |
| --- | --- | --- | --- |
| project.json | 工程目录 | 统一工程（HiStream 段 + STREAM 段 + 共享段）；旧格式首次保存前备份为 `project.v<n>.json` | `writeProject` (server.mjs:2502) |
| project.usd、usd/sync.json | 工程目录 | USD 镜像与哈希同步记录 | `exportProjectUsdFiles`、`recordUsdSync` (server.mjs:2653,2689) |
| run-manifest.json | `<outDir>/runs/<runId>/` | `kind:'histream-run'`，见 2.3.6 字段表 | `writeRunManifest` (server.mjs:284) |
| source-project.json、input/ | 同上 | 源工程副本；运行用冻结工程、气象快照与裁切后的 OBJ | `createIsolatedRun`、`/api/run` |
| facetrt.json、faceteb.json（+ .bin） | 运行目录 | 面元结果，JSON 或二进制两种格式 | 引擎 |
| process/*.meta.json、数据文件 | 运行目录 | `kind` 以 `-process` 结尾；`dataFile`、`recordFloats`、`fields[{id,label,offset}]`、`geometry`（facet 或 voxel）、`facetCount` 或 `voxelCount` | 引擎与 `writeRadiosityTiffForAngle` |
| *.tif、result_statistics_<mode>.csv | 运行目录 | float32 TIFF；统计表表头 `file,mode,time,vza,vaa,sza,saa,band_index,band_name,count,min,max,mean,stddev` | `writeFloatTiff`、`writeRasterStatistics` (server.mjs:3076,3128) |
| renderer-settings.json | Electron userData | `{ <键>: 字符串 或 null }`，仅 9 个白名单键 | `createDesktopSettings` |
| stream-project.json | STREAM 工程目录 | `kind:'stream-image-project'`、`schemaVersion:2` | `saveProjectFile` (server/stream/config.mjs:228) |
| run-config.json、scene.json、run-manifest.json | STREAM 输出目录 | 引擎配置 `kind:'stream-image-config'`；清单 `kind:'stream-image-run'`、`schemaVersion:2` | `submitRun` (server/stream/index.mjs:315-322) |
| era5-packs/pack-*.json | `~/Documents/Stream Studio/` | 数据包索引，`kind` 为 `stream-era5-pack` 或 `stream-forcing-pack`；元数据文档 `kind:'stream-forcing-metadata'`、`schemaVersion:1` | 引擎（登记），`removePack` 删除 |

### 2.8 环境变量速查

| 变量 | 作用 | 位置 |
| --- | --- | --- |
| STREAM_STUDIO_RESOURCE_ROOT | 资源根（引擎、assets、gui、docs） | server.mjs:71 |
| STREAM_STUDIO_DATA_ROOT | 数据根；仅用作 `/api/open` 的缺省路径 | server.mjs:5451 |
| STREAM_STUDIO_SERVER_AUTOSTART | 为 0 时不自行监听 | server.mjs:5603 |
| STREAM_STUDIO_PORT | Web 模式端口，缺省 4173 | server.mjs:5602 |
| STREAM_STUDIO_ENGINE_ALLOWLIST | 额外允许的引擎路径，按系统分隔符分隔 | server.mjs:201 |
| STREAM_STUDIO_MAX_FACET_TRIANGLES | 面元数量上限（最小 1000） | server.mjs:93 |
| STREAM_STUDIO_RUN_TIMEOUT_MS | 单次运行超时 | server.mjs:106 |
| STREAM_STUDIO_TASK_WORKERS | 后台线程数，0 到 8，缺省 2 | server.mjs:154-158 |
| STREAM_STUDIO_RESULT_CACHE_MB | 结果缓存上限，缺省 512，0 关闭 | server/result-cache.mjs:38 |
| STREAM_STUDIO_RESULT_CACHE_DEBUG | 为 1 时打印命中情况 | server.mjs:506 |
| STREAM_STUDIO_FACET_STEP_RESIDENT_FACETS | 常驻节点出图线程的面元数阈值 | server.mjs:2396 |
| STREAM_STUDIO_MAX_RESULT_BYTES | 单个结果文件字节上限 | server/facet-result.mjs:16 |
| STREAM_STUDIO_STREAM_TIMEOUT_MS | STREAM 单任务超时，缺省 2 小时 | server/stream/index.mjs:43 |
| STREAM_STUDIO_PROXY_MANIFEST、PROXY_RUNNER、PROXY_PYTHON | histream-proxy 部署 | server.mjs:586,621,622 |
| HISTREAM_ROOT | 由桌面主进程设置为 `<root>/engine` | desktop/main.mjs:196 |

### 2.9 修改指南与注意事项

1. 新增一个 HTTP 接口：在 `api()` 内按“方法加路径”增加分支（server.mjs:4490-5463）；通过 `body()` 读 JSON，用 `json()` 回包，错误统一 `{ error }`；会读写大文件的工作放进 `HEAVY_TASKS` 并用 `heavyTasks.run`，不要在事件循环里同步处理。STREAM 相关接口则在 `routes` 对象中加键（server/stream/index.mjs:658）。同时在《运行原理说明》§1.4.4 的路由分类中更新。
2. 新增后台任务：在 `HEAVY_TASKS` 加函数（server.mjs:5484），参数与返回值必须可结构化克隆，大结果用 `jsonPayload` 返回 `{ bytes }`，主线程用 `toJsonBytes` 包装；需要命中线程内缓存的任务给 `key`；运行结束后的大内存任务用 `runPostprocessTask`（一次性线程）而不是常驻池。
3. 往 `server.mjs` 顶层加代码：它会在每个 worker 中重复执行，不得在顶层启动监听、定时器或有副作用的 I/O；全局状态变量（`child`、`projectFile` 等）在 worker 中是独立副本，不能用来跨线程通信（server.mjs:5602-5643）。
4. 新增工程字段若引用外部文件：同时加入 `normalizeStoredProjectPaths` 与 `mapHistreamInputPaths`（server.mjs:890,912），再检查 `saveProjectAs` 的 `rebasePath` 与 `resultDeletionScope`，STREAM 场景字段则改 `mapStreamScenePaths`（server/stream/config.mjs:161）。字段本身的规范化与校验在 `src/renderer/src/project-schema.js`，由另一章说明。
5. 修改结果文件命名：同步改 `outputTiffName`、`filenameToken`、`tiffObservation`、`convertEnviToTiff` 以及引擎命名（server.mjs:1469,3245,3344,3274），并核对测试 `tools/test-server-fixes-20261125.mjs`（package.json:16 的 `test` 脚本）。
6. 修改面元结果字段：JSON 解析、二进制解析、JSON 抽样、二进制抽样四处指标清单各自独立，需一起改（server/facet-result.mjs:145,262,528,623）；若新增二进制数组类型，需同时确认 `facetSidecarFiles` 能列出其旁路文件，否则删除与复制会漏文件。
7. 新增返回给前端的大数值数组：确认键名是否属于 `BINARY_KEYS`（`vertexPositions`、`voxelPositions`、`values`），否则仍走 JSON；改二进制格式时需同时改 `web-api.js` 的解码并提升魔数版本（server/result-cache.mjs:132）。
8. 修改引擎退出码语义：先改 `models/histream/src/main.cpp`，再同步 `HISTREAM_EXIT` 与 `histreamResultCodes`（server.mjs:119-121）；`completeHistreamRun` 的状态优先级在 server.mjs:5264-5266，改动要同时更新 `listResults` 与前端对 `runStatus` 的处理。
9. 修改 FacetEB 逐节点握手：协议行 `FACET_CONVERGENCE`、`FACET_STEP`、`FACET_ACK`、`FACET_ERROR` 在 server.mjs:5130-5178 解析，必须与引擎侧同步；握手期间要暂停超时计时（`pauseRunTimer`），且出图失败必须向 stdin 写 `FACET_ERROR` 让引擎中止，否则引擎会一直等待。
10. 修改“可删除/可读取的结果范围”：`resultDirectoryAllowed`、`resultFilePathAllowed`、`resultProductsForDeletion` 的原则是输入保护优先、来源不明则不删（server.mjs:3013,3036,3406）；放宽时不得让工程输入文件或输入目录落入可删范围，并保留对符号链接 `runs/` 的拒绝。
11. 允许新的引擎可执行文件：在 `histreamCandidates` 加内置候选，或经 `STREAM_STUDIO_ENGINE_ALLOWLIST` 配置；不要放宽 `allowedEngine`，`/api/run` 与 `/api/devices` 都靠它防止启动任意程序（server.mjs:196,4618,4991）。
12. 桌面版新增 IPC 或设置键：设置键加入 `settings.mjs` 的 `KEYS`（desktop/settings.mjs:20）；新增的 IPC 处理器必须像设置 IPC 一样校验 `event.sender`、主框架与同源（desktop/main.mjs:353-357）；主进程与服务子进程之间的新消息要同时改 `startServerProcess` 的 `message` 分发与 `server-process.mjs` 的 `parentPort.on('message')`，并保证 `shutdown` 能清理挂起的 Promise。
13. 修改服务退出流程：桌面版的停止顺序是主进程 `shutdown` 消息 → `stopEngines()` → `server.close` → 5 s 内未退出则 `taskkill` 进程树；`stopEngines` 必须同时终止面元出图线程、线程池、引擎进程与 STREAM 服务（server.mjs:5652-5662）。新增长期运行的子进程或线程时，要在这里登记，否则退出会遗留孤儿进程。
14. 调整线程数或缓存：`STREAM_STUDIO_TASK_WORKERS` 为 0 时所有重任务回到服务线程同步执行，只用于排错；缓存总量按线程数均分，线程越多单线程缓存越小，大 TIFF 多波段连读的命中率会下降（server.mjs:152-165,5637）。
15. 移植到非 Windows：当前多处硬编码 Windows 行为——`taskkill`、PowerShell 文件对话框、`explorer.exe`、`;` 作为 PATH 分隔符、`.exe` 引擎名，以及 STREAM 的 8.3 短名检查（desktop/main.mjs:128,190；server.mjs:2623,5458；server/stream/index.mjs:68）；非 Windows 平台只部分有回退分支，改动前需逐处确认。

## 3. 前端：src/renderer

本章描述渲染进程（浏览器页面）的全部源码。前端是 Vite 构建的原生 JavaScript 单页应用，不使用 React 或 Vue 等框架：界面由 `index.html` 的静态骨架加上脚本中的 `innerHTML` 模板拼接生成，事件靠 `addEventListener` 与事件委托绑定（src/renderer/src/main.js:2531-2534，src/renderer/src/stream/workspace.js:548）。运行流程（打开、运行、结果读取的时序）见《运行原理说明》，本章只给出函数级的调用关系与修改入口。

### 3.1 模块划分与依赖方向

前端文件按职责分为三层。依赖只允许自上而下，下层不得引用上层：

```
界面层   main.js ── scene-loader.js, i18n.js, menubar.js, material-specular.js
            └──── stream/workspace.js ── stream/forcing-panel.js
通信层   web-api.js ─┐
         stream/api.js ─┼─ http.js          storage.js（被 web-api、i18n、workspace、main 使用）
数据层   project-unified.js ── project-schema.js ── material-specular.js
```

- 数据层（`material-specular.js`、`project-schema.js`、`project-unified.js`）是纯函数，不碰 DOM、不读写文件，浏览器与 Node 共用。`server.mjs` 直接导入 `project-schema.js` 与 `project-unified.js`（server.mjs:46-47），`server/stream/config.mjs` 导入 `isUnifiedProject`（server/stream/config.mjs:27），`tools/` 下的示例生成、USD 导入导出、回归脚本也导入它们。因此这三个文件不得引入 `window`、`document` 或 Node 专有模块（src/renderer/src/project-unified.js:35）。
- 通信层只负责 HTTP、SSE 与本地存储：`http.js` 是 `web-api.js` 和 `stream/api.js` 共用的 fetch 封装（src/renderer/src/http.js:3-9）。
- 界面层按功能分工：`main.js` 是三维工程工作台，`stream/workspace.js` 是遥感影像（STREAM）工作台，两者互不直接引用状态，只通过 `createStreamWorkspace({ toast })` 返回的 `open/openProject/close/command/isOpen` 联系（src/renderer/src/stream/workspace.js:2402，src/renderer/src/main.js:9043）。
- 统一工程（`stream-studio-project`）的格式转换发生在服务端读写边界；`main.js` 中没有任何对统一格式的引用（grep 未命中 `unified`），它只看到旧的 HiStream `project.json` 结构，或在 `result.streamProject` 存在时转交 STREAM 工作台（src/renderer/src/main.js:3500-3504，src/renderer/src/project-unified.js:26-33）。

### 3.2 构建与运行配置

| 项目 | 内容 | 出处 |
| --- | --- | --- |
| Vite 根目录 | `src/renderer`，`index.html` 与 `src/` 均相对该目录解析 | vite.config.js:24 |
| 开发服务器 | 127.0.0.1:5173，`/api` 代理到 `http://127.0.0.1:4173` | vite.config.js:25-32 |
| 构建输出 | 项目根下的 `gui/`，`emptyOutDir: true`，每次构建清空该目录 | vite.config.js:35-37 |
| 依赖版本 | `three` 固定 0.185.1，`vite` 固定 7.3.6，`electron` ^38.0.0，均在 devDependencies | package.json:24-27 |
| 模块类型 | `"type": "module"`，桌面入口 `desktop/main.mjs` | package.json:7-8 |
| 静态资源 | `src/renderer/public/references/*.png` 为“理论与参考”对话框的参考图（共 6 张） | src/renderer/index.html:292 |

package.json 脚本中与前端相关的有：`dev`（`node dev.mjs`，同时拉起 `server.mjs --api-only` 与 Vite，两个子进程同生共死）、`build` 与 `build:gui`（均为 `vite build`）、`start`（`node server.mjs`，提供 `/api` 与 `gui/` 静态页，默认端口 4173）、`desktop`（`electron .`）、`test`（运行 4 个 `tools/test-*.mjs`，从文件名看不含界面测试，具体覆盖范围待核实）（package.json:10-19，dev.mjs:25-27，server.mjs:5602）。

发布方式：`server.mjs` 在非 `--api-only` 模式下用 `GUI_OUTPUT_DIR`（资源根下的 `gui`）托管静态文件，找不到文件时回退 `index.html`（server.mjs:72，server.mjs:5583-5589）；Electron 打包把 `gui/` 作为 `extraResources` 原样复制（electron-builder.yml:25-27）。

`index.html` 的 CSP 元标签规定：`default-src 'self'`，脚本与样式允许内联，`connect-src` 仅允许本机 127.0.0.1 的 http 与 ws，禁止 `object` 与 `base`（src/renderer/index.html:10）。新增外部 CDN 脚本或跨域请求会被浏览器拦截。

### 3.3 文件清单

#### 3.3.1 src/renderer 根目录

| 文件 | 行数(约) | 职责 | 关键内容 |
| --- | --- | --- | --- |
| index.html | 622 | 页面骨架：SVG 图标精灵、菜单栏、顶栏、三栏主区、各对话框与抽屉、STREAM 工作台挂载点 | `#app`、`#menubar`、`#viewport`、`#inspectorContent`、`#streamWorkspace`、`<script type="module" src="/src/main.js">` |
| public/references/ | 6 张 PNG | 参考图例（角度效应、作物温度、森林流程、光伏、船舶、城市场景） | 由 `.reference-shot` 按钮打开 |

#### 3.3.2 src/renderer/src

| 文件 | 行数(约) | 职责 | 关键导出或入口 |
| --- | --- | --- | --- |
| main.js | 9302 | 三维工作台全部逻辑：state、Three.js 预览、属性面板、对话框、保存与运行、结果查看、事件绑定、`init()` | 无导出；模块加载即执行（src/renderer/src/main.js:9302） |
| project-schema.js | 1957 | HiStream 工程数据模型：常量、默认值、规范化、校验、XML 导出 | `createDefaultProject`、`normalizeProject`、`parseProjectJson`、`stringifyProject`、`validateProject`、`projectToXml`、`QUALITY_PRESETS` 等 |
| project-unified.js | 652 | 统一工程与两种旧工程的互转 | `normalizeUnified`、`histreamProjectToUnified`、`unifiedToHistreamProject`、`streamProjectToUnified`、`unifiedToStreamScene` |
| scene-loader.js | 1014 | 把工程对象载入 Three.js 预览场景并驱动动画 | `loadXmlScene`、`updateSceneDynamics`、`invalidateImageTextures` |
| web-api.js | 134 | 三维工作台访问 `/api/*` 的客户端 | `webApi`（含 SSE 订阅） |
| http.js | 36 | fetch 封装，统一错误转换 | `request`、`post` |
| storage.js | 42 | 键值持久化，桌面版与网页版同接口 | `appStorage` |
| i18n.js | 1176 | 中英文切换（DOM 就地翻译） | `t`、`getLocale`、`setLanguage`、`localizeElement`、`startLocalization` |
| menubar.js | 105 | 菜单栏展开、键盘导航与 Ctrl+N/O | `createMenubar` |
| material-specular.js | 89 | 镜面反射设置的规范化、迁移与表单片段 | `normalizeSpecular`、`meshSpecular`、`meshSpecularRunError` 等 6 个 |
| styles.css | 1013 | 全局样式（深色主题变量见 :root） | 由 main.js 末尾 import（src/renderer/src/main.js:44） |

#### 3.3.3 src/renderer/src/stream

| 文件 | 行数(约) | 职责 | 关键导出或入口 |
| --- | --- | --- | --- |
| workspace.js | 2403 | STREAM 遥感影像工作台：表单、校验、工程、运行、结果查看器、气象数据包 | `createStreamWorkspace` |
| forcing-panel.js | 621 | 气象强迫元数据面板（数据源、变量名、单位、校验） | `createForcingPanel`、`summaryLines` |
| api.js | 50 | `/api/stream/*` 客户端与 SSE 通道 | `streamApi` |

### 3.4 index.html 页面结构

页面是单文档、全部节点在加载时存在，对话框用 `hidden` 属性显示或隐藏（styles.css 中 `[hidden]` 强制 `display:none`，src/renderer/src/styles.css:21）。

| 区域 | id 或类 | 说明 |
| --- | --- | --- |
| 菜单栏 | `#menubar`，5 个 `.menu` | 项目、解决方案、结果、分析、帮助；带 `data-scene-command` 的项可被 STREAM 工作台接管，带 `data-solution-mode` 的项切换计算模式，带 `data-stream-command` 的项驱动 STREAM（src/renderer/index.html:39-93） |
| 顶栏 | `.topbar` | `#projectName`、`#unsavedMark`、`#languageToggle`、`#runBtn`、`#stopBtn`、`#resetBtn`（index.html:94-114） |
| 左侧栏 | `.sidebar` | `#modeGrid`（5 个 `.mode-card`）、`#computeDevice`、`#cpuThreads`、`#sceneTree`（`.tree-item[data-panel]` 共 11 项）、引擎状态 `#engineState`（index.html:117-169） |
| 视口 | `#viewport` | Three.js 画布，加工具栏 `#viewStyle`、`#gridBtn`、`#voxelBtn`、`#fitBtn` 与对象编辑菜单（index.html:171-215） |
| 属性检查器 | `#inspectorContent` | 由 `renderInspector()` 整体重写（index.html:217-223） |
| 控制台 | `#consoleOutput`、`#runProgress`、`#elapsed` | 运行日志与进度条（index.html:225-238） |
| 对话框 | `.drawer-backdrop`，id 为 `referenceDialog`、`xmlDrawer`、`exampleDialog`、`projectDialog`、`saveAsDialog`、`geometryDialog`、`materialDialog`、`objectAttributeDialog`、`objectDialog`、`presetDialog`、`resultDialog`、`histreamProxyDialog`、`helpDialog` | 共同使用 `.drawer-backdrop` 遮罩（index.html:247-613） |
| STREAM 挂载点 | `#streamWorkspace` | 初始 `hidden`，内容由 `createStreamWorkspace` 首次打开时渲染（index.html:611，workspace.js:548） |

### 3.5 main.js

`main.js` 约 9300 行，按“出现顺序”组织，没有拆成模块。下表给出功能分区的行号区间，函数名之后括号内为起始行。

#### 3.5.1 功能分区

| 区间 | 内容 | 主要函数或常量 |
| --- | --- | --- |
| 1-152 | 头注释、导入、`$`/`$$`/`api` 简写、4 组纹理预设清单、`state` 单例 | `PHYSICAL_TEXTURE_PRESETS`(58)、`state`(111) |
| 154-346 | 预设合并与旧 XML 材质节点 | `mergeNamedPresets`(157)、`ensureMaterialPresets`(179)、`defaultConfig`(210)、`projectModeFromXml`(239)、`parseMaterialNode`(254) |
| 348-606 | 旧 XML 工程解析（JSON 工程直接走 `parseProjectJson`） | `parseXml`(354) |
| 608-779 | 脏标记与工程文本同步、toast、日志、进度条、运行状态切换 | `markXmlDirty`(609)、`syncProjectText`(616)、`scheduleProjectTextSync`(633)、`toast`(671)、`addLog`(682)、`renderRunProgress`(718)、`setRunning`(751) |
| 780-1132 | Three.js 场景初始化，视口内“绘制区域/立方体”交互 | `scene/camera/renderer/controls`(785-821)、`geometryDraw`(835)、`startGeometryDraw`(981)、`finishGeometryDraw`(1037) |
| 1134-1378 | 对象选取与 TransformControls 变换编辑 | `objectEdit`(852)、`setObjectEditEnabled`(1167)、`editedPlacement`(1267)、`saveEditedObjectTransform`(1311) |
| 1380-1615 | 天空盒、清场、场景边界尺寸引导 | `updateSceneSkybox`(1414)、`clearWorld`(1438)、`updateSceneBoundsGuide`(1511) |
| 1617-2147 | 传感器与巡航可视化（卫星、飞机、视场足迹） | `createParallelObservationVisualization`(1770)、`updateCruiseVisualization`(2006)、`updateCruiseAnimation`(2105) |
| 2149-2526 | 预览世界生成、太阳、体元预览、相机、渲染循环 | `generateWorld`(2172)、`updateSun`(2245)、`buildVoxelPreview`(2336)、`applyViewStyle`(2451)、`fitCamera`(2472)、`resize`(2492)、`animate`(2513) |
| 2528-2715 | 面板 HTML 小工具、材质库卡片、计算设备选择 | `panelTitles`(2530)、`field`(2532)、`libraryCard`(2538)、`renderComputeDevices`(2644)、`loadComputeDevices`(2689)、`remapGpuIndex`(2703) |
| 2717-3098 | 属性面板生成、质量档位控件、通用绑定器 | `renderInspector`(2724)、`restoreActualScene`(2967)、`applyQualityPreset`(3007)、`bindValue`(3070)、`bindSwitch`(3091) |
| 3104-3421 | 属性面板控件绑定（场景、控制、温度方法、流体、DEM、大气、背景、太阳、传感器、输出） | `bindInspector`(3109) |
| 3423-3770 | 导航徽标、刷新、DEM、工程读写、示例、气象与 DEM 导入 | `renderNavBadges`(3424)、`refreshFromState`(3439)、`loadActualScene`(3471)、`loadXml`(3495)、`chooseXml`(3535)、`showExampleDialog`(3584)、`importMeteo`(3625)、`importDem`(3662)、`saveXml`(3695) |
| 3772-4100 | 名称与 OBJ 清理、目标分布与运动的表单与采样 | `distributionFromForm`(3860)、`generateDistribution`(4056)、`movementFromForm`(3972)、`seededRandom`(3928) |
| 4101-5006 | 物体/材质对话框的字段生成、车辆参数、纹理字段、材质与几何对话框 | `vehicleFields`(4171)、`renderPhysicalParameters`(4677)、`showMaterialDialog`(4805)、`showGeometryDialog`(4938) |
| 5008-5331 | OBJ 文本生成与导入 | `surfaceGeometryObj`(5018)、`waterSurfaceObj`(5050)、`chaosGeometryObj`(5126)、`generateGeometryObject`(5189)、`importObj`(5318) |
| 5333-5802 | 预设（光谱/热/物化）对话框 | `physicalFromForm`(5346)、`showPresetDialog`(5653)、`savePreset`(5757) |
| 5803-6310 | 导入对象保存、对象属性对话框、对象分布对话框、篷布罩、删除对象 | `saveImportedObject`(5803)、`saveObjectAttributes`(6007)、`saveSceneObject`(6176)、`addTarpCover`(6230)、`deleteSceneObject`(6283) |
| 6311-6706 | 运行：预检、启动、事件处理、运行质量统计 | `runSimulation`(6313)、`runPreflightCheck`(6337)、`startSimulation`(6406)、`normalizedEngineLog`(6467)、`recordRunQualityLine`(6513)、`runQualitySummary`(6566)、`handleSimulationEvent`(6616) |
| 6708-7733 | 结果预览与三维视图 | `drawEnviPreview`(6936)、`drawTiffPreview`(7020)、`drawWindPreview`(7037)、`drawImagePreview`(7115)、`showResult3D`(7176)、`showFacet3D`(7332)、`showVoxel3D`(7452)、`showSoilProfile3D`(7585) |
| 7735-8391 | 统计 CSV 分析图表与变化分析 | `parseCsvRows`(7735)、`lineAnalysisSvg`(7887)、`polarAnalysisSvg`(7958)、`scatterAnalysisSvg`(8141)、`renderChangeAnalysis`(8208)、`drawStatisticsAnalysis`(8378) |
| 8393-8612 | 结果对话框控制 | `openResultFile`(8393)、`resultFilesForView`(8449)、`setResultView`(8477)、`refreshResults`(8550)、`showResults`(8588) |
| 8614-8869 | 模式切换、工程对话框、histream-proxy 对话框、另存为与新建 | `selectMode`(8619)、`openHistreamProxyDialog`(8723)、`saveProjectAs`(8822)、`createProject`(8854) |
| 8871-9302 | 事件绑定（5 组）、STREAM 接入、帮助、快捷键、引擎路径、`init()` | `streamWorkspace`(9032)、`SHORTCUTS`(9059)、`showHelp`(9070)、`chooseEngine`(9249)、`init`(9276) |

#### 3.5.2 state 对象

`state` 是模块级单例（src/renderer/src/main.js:111-152）。字面量里声明的字段：

| 字段 | 含义 |
| --- | --- |
| `mode` | 当前计算模式，取值 `eRaytracing`、`eVoxelRT`、`eVoxelEB`、`eFacetRT`、`eFacetEB`，缺省 `eVoxelEB` |
| `executable`、`executableExists`、`defaultExecutable`、`defaultExecutableExists` | 引擎路径及其存在性，来自 `GET /api/defaults`（main.js:9282-9283） |
| `inputPath`、`projectPath`、`outputDir` | 当前 `project.json` 路径、工程目录、结果目录 |
| `project`、`config` | 规范化后的工程对象与其 `configuration` 节；`state.config` 是界面各处直接修改的对象，保存时才深拷贝（main.js:3718-3719） |
| `xmlText`、`xmlDirty`、`xmlManualEdit` | `project.json` 文本、未保存标记、文本是否被用户在 JSON 抽屉里手改 |
| `editRevision`、`projectGeneration` | 编辑版本号与工程代次；保存、另存为用它们判断异步期间工程是否被切换或再编辑（main.js:3703-3708） |
| `runPending`、`running`、`startedAt`、`progress`、`progressStage`、`logCount` | 运行状态与进度；`progress` 只增不减（main.js:744） |
| `panel` | 当前属性面板 id，对应 `panelTitles` 的键 |
| `pendingObject`、`drawPlacement`、`editingObjectIndex`、`selectedObjectIndex`、`editingAttributeIndex`、`editingPreset`、`presetPhysicalType` | 各对话框的编辑上下文 |
| `resultFiles`、`selectedResult`、`resultRaster`、`resultView`、`analysisType`、`analysisSelection`、`resultDisplay` | 结果查看器状态；`resultView` 取 `image` 或 `analysis`，`analysisType` 取 `angle`、`band`、`time`、`change`、`3d`（main.js:8497） |
| `changeAnalysisRequest`、`resultPreviewRequest`、`resultOpenRequest` | 异步请求计数，用于丢弃过期结果 |

脚本其他位置动态挂上的字段（字面量中没有声明）：`pid`、`runId`（main.js:6413，6412）、`runQuality`、`stdoutCarry`（6514，6640）、`devices`、`devicesError`、`devicesLoading`（2697）、`examples`（3589）、`serverRunTimeoutSeconds`（9284）。`state.project.voxelization` 在由面元类模式切到体元模式时写入（main.js:8631）。

#### 3.5.3 核心调用链

编辑到保存（属性面板路径）：

```
控件 change 事件 → bindValue / bindSwitch（main.js:3070，3091）
  → target(value) 写入 state.config.*
  → syncProjectText()（616）：state.project.configuration = state.config
        → normalizeProject → stringifyProject → state.xmlText，markXmlDirty()
  → refreshFromState(rebuild)（3439）：renderNavBadges、generateWorld、updateSceneSkybox、updateSun
  → rebuildScene 为真时 restoreActualScene()（2967）→ loadActualScene（3471）
保存：saveXml(fromProject)（3695）
  → active.blur() 提交数字框；flushProjectTextSync() 冲刷 250 ms 防抖（633-642）
  → structuredClone(config, project) 快照 → api.saveXml({path, content, project})
  → 按 projectGeneration / editRevision 判断：已切换工程返回 null；有新编辑则保持“未保存”
```

要点：`input` 事件下的实时预览只用于偏移输入框（先 `scheduleProjectTextSync` 防抖，再 280 ms 后 `restoreActualScene`，main.js:3120-3133）；其余字段只在 `change` 时写回。`saveXml` 返回 `true`、`false`、`null` 三态，`null` 表示被更新的工程或编辑取代，调用方必须跳过刷新与回滚（main.js:3694，3722）。

打开工程：

```
chooseXml()（3535）→ api.chooseXml(state.mode) → POST /api/project/pick
  → loadXml(result)（3495）
      result.streamProject 存在 → streamWorkspace.openProject(...)，返回
      state.running → 拒绝
      normalizeProject(result.project 或 parseProjectJson(result.content))
      → ensureMaterialPresets(configuration)（179，合并内置预设）
      → 按文件名或名称恢复对象的 distribution / instanceCount（3516-3519）
      → invalidateTextureCaches；Object.assign(state, {...})；projectGeneration += 1
      → selectMode → refreshFromState → restoreActualScene → renderInspector → reportUsdOpen
```

`loadXml` 的函数名与 `xmlText`、`xmlDirty` 等字段名沿用 XML 时代，内容现在是 `project.json`（main.js:3486-3488）。

运行：

```
runSimulation()（6313）
  → meshSpecularRunError(mode, config)       镜面反射仅 eRaytracing 允许
  → xmlDirty 时 saveXml()
  → runPreflightCheck()（6337）→ POST /api/run/preflight；有风险提示则 window.confirm 或 chooseDialog 三选一
  → startSimulation()（6406）→ POST /api/run；setRunning(true)
SSE /api/events → webApi.onSimulationEvent → handleSimulationEvent（6616）
  type = status / started / stdout / stderr / error / closed
  stdout → normalizedEngineLog（6467）→ setSimulationProgress、recordRunQualityLine（6513）
  closed → runQualitySummary（6566）→ 成功或未收敛时 300 ms 后 showResults()
```

引擎 stdout 可能被任意切块，因此只解析完整行，残余缓存在 `state.stdoutCarry`（main.js:6648-6657）。`recordRunQualityLine` 依赖引擎输出的固定行格式（`FACET_CONVERGENCE`、`FACET_TIMING`、`ENERGY_BALANCE`、`SHORTWAVE_SCATTERING`、`SHORTWAVE_SPLIT`、`VEHICLE_CABIN`），引擎调整字段顺序会使解析静默失败（main.js:6505-6510）。

结果查看：

```
showResults(view, analysisType)（8588）→ refreshResults（8550）→ POST /api/results/list
  → openResultFile(file, band)（8393）→ api.readResult → POST /api/results/read（二进制 SSR1 或 JSON）
  → 按 result.kind 分派：envi → drawEnviPreview、wind → drawWindPreview、tiff → drawTiffPreview、
    facet → showFacet3D、voxel → showVoxel3D、image → drawImagePreview、text → 统计图表或原文
```

`openResultFile` 用 `state.resultOpenRequest` 序号与 `stale()` 丢弃过期读取（main.js:8395-8396）；旧版面元结果没有 `kind`，在此被包装成 `kind='facet'`（8406-8416）。

#### 3.5.4 Three.js 预览与坐标约定

- 预览世界以场景中心为原点，Three 的 +X 为北、+Z 为东、+Y 向上；`world.userData.displayScale = min(1, 45 / max(x, y))`，使大场景缩到约 45 个预览单位（main.js:782-784，2176）。
- DEM 栅格列向东增大、第 0 行为北边缘（main.js:2211-2215，src/renderer/src/scene-loader.js:729）。
- `animate`（main.js:2513）每帧顺序为：`controls.update`、`updateSceneDynamics`、`updateCruiseAnimation`、`updateObjectSelectionBox`、主场景渲染、结果查看器渲染；引擎运行且视口 1.5 s 内无输入时降到约每 100 ms 一帧（RUNNING_FRAME_INTERVAL_MS=100、VIEWPORT_INTERACTION_GRACE_MS=1500，main.js:2503-2504）。
- 结果三维查看器是独立的第二个 Three 场景，由 `showResult3D` 等创建，`disposeResultViewer`（7134）释放；它的渲染调用在 `animate` 内（`renderResultViewer`，7164）。

#### 3.5.5 事件绑定与快捷键

事件绑定在模块加载时一次性执行（main.js:8871 起），分 5 组：模式与设备与导航树、场景对象工具菜单、顶部按钮与示例与新建与另存为、各对话框的关闭与提交、结果与分析入口。快捷键：

| 键 | 行为 | 位置 |
| --- | --- | --- |
| Ctrl/Cmd+N、Ctrl/Cmd+O | 点击 `#newProjectBtn` 或 `#openXmlBtn` | menubar.js:89-102 |
| Ctrl/Cmd+S、Ctrl/Cmd+Shift+S | 保存、另存为 | main.js:9238 |
| F5 | 运行三维模拟；STREAM 工作台打开时由其捕获阶段监听抢先处理并 `stopImmediatePropagation` | main.js:9238，workspace.js:2221-2226 |
| F7 | 打开 STREAM 工作台 | main.js:9238 |
| Esc | 按固定优先级只关闭最上层的一个对话框 | main.js:9238 |

菜单栏必须先于 STREAM 工作台创建：二者都在捕获阶段监听 keydown，只有这个顺序才能让 Esc 先关菜单；因此 `streamWorkspace` 先声明为 `null`，`route` 回调里显式判空（main.js:9028-9043）。

### 3.6 project-schema.js

#### 3.6.1 职责与导出

HiStream 工程（`project.json`，`format: 'histream-project'`，当前 `schemaVersion` 为 6）的数据模型：常量、默认值、规范化与旧工程迁移、校验、XML 导出（src/renderer/src/project-schema.js:27-28）。

| 导出 | 签名与含义 | 行号 |
| --- | --- | --- |
| `PROJECT_SCHEMA_VERSION`、`PROJECT_FORMAT`、`PROJECT_MODES` | 版本 6、格式名、5 种计算模式 | 27-31 |
| `createDefaultProject(options)` | 新工程完整缺省对象；`options` 含 `mode`、`name`、`sceneX/Y/Height`、`voxelSize` 等；缺省场景 60×60×15 m、体元 1 m | 889 |
| `normalizeProject(value)` | 任意版本工程 → 当前 schema 的完整工程，不修改入参；缺字段按“旧工程保持旧行为、新工程用新缺省”处理 | 1060 |
| `parseProjectJson(text)` | 解析并规范化；非对象根、非本格式 `format`、`schemaVersion` 高于 6 均抛错 | 1457 |
| `stringifyProject(value)` | 规范化后输出 2 空格缩进、末尾换行的 JSON | 1471 |
| `validateProject(value)` | 返回 `{ valid, errors, project }`，只收集错误文本不抛异常 | 1479 |
| `projectToXml(value, paths)` | 生成 `<HiStreamProject>` XML；`paths` 可含 `outputDir`、`definedDir`、`resolveProjectPath`、`atmosphereLutPath`、`meteoPath` | 1794 |
| `createMaterialPresets()` | 内置光谱、冠层、热、物化预设 `{ spectra, canopies, thermals, materials }` | 731 |
| `sensorBandValues(sensor)` | 传感器波段列表（连续波段加自定义波段，去重、升序、上限 800） | 305 |
| `sensorViewAngles(sensor, sunAzimuth)` | 观测方向 `[[VZA, VAA], ...]`，支持单方向、主平面扫描（±75° 每 5°）、128 方向半球 | 340 |
| `sensorCruisePositions(sensor, scene)` | 巡航航点，路线 `line`、`rectangle`、`random`，随机路线用固定种子 LCG | 137 |
| `QUALITY_PRESETS`、`qualityPresetFields`、`qualityPresetValues`、`qualityPresetMatches`、`autoRadiationMaxSteps` | 快速/标准/高精度/自定义 4 档质量预设 | 80-121 |
| `groundedPlacementHeight`、`isGroundedObject` | 贴地对象（目前只有 `Vehicle`）按 DEM 求放置高度 | 214-233 |
| `splitVehicleObj`、`vehicleMeshBinding`、`createVehicleBodyMaterial` 与 `VEHICLE_*` 常量 | 车辆 OBJ 组名自动绑定、前挡/侧窗/后窗/引擎盖拆分、座舱与发动机缺省 | 514-713 |
| `surfaceOpenness`、`porousOptics`、`effectivePorousSpectrum` | 穿孔板开孔率光学，开孔率上限 `MAX_SURFACE_OPENNESS = 0.95` | 414-486 |
| `normalizeTerrainEnvironment`、`TERRAIN_ENVIRONMENT_DEFAULTS` | 地形水热经验场参数 | 47-63 |
| `normalizeCpuThreads`、`normalizeRunTimeoutMinutes`、`DEFAULT_CPU_THREADS` | CPU 线程（缺省 32，限 1-1024）与单次运行上限（0 或 1-1440 分钟） | 66-127 |
| `simplifySolarSpectra` | 剔除旧版内置且未被引用也未改动的光伏光谱 | 384 |

依赖 `material-specular.js`（`normalizeSpecular`、`meshSpecular`）（project-schema.js:23）。被 `main.js`、`project-unified.js`、`server.mjs`、`scene-loader.js` 与 `tools/` 脚本引用。

#### 3.6.2 工程文件结构（project.json）

顶层字段：`schemaVersion`、`format`、`name`、`mode`、`createdAt`、`updatedAt`、`folders`、`configuration`（project-schema.js:893-908）。`folders` 为 6 个相对子目录名：`models`、`positions`、`spectral`、`meteorology`、`terrain`、`output`。`configuration` 的节与要点：

| 节 | 主要字段 | 说明 |
| --- | --- | --- |
| `outDir` | 字符串 | 输出目录，缺省 `output` |
| `scene` | `x`、`y`、`height`（m）、`offsetX/Y/Z`、`voxel`、`voxelFillThreshold`、`terrain`、`demFile`、`demInfo`、`terrainEnvironment`、`background` | `x` 向北、`y` 向东、`height` 向上；偏移三轴分别为北、高度、东（main.js:2774 的界面标签） |
| `light` | `zenith`、`azimuth`、`direct`、`diffuse`、`skyTemperature`、`shortwaveIrradiance`、`diffuseFractionMode` | 新工程 `diffuseFractionMode` 缺省 `erbs`，旧工程缺字段按 `fixed`（project-schema.js:932-934） |
| `sensor` | `x`、`y`（像元数）、`bands`（空格分隔波长 nm）、`continuousBands`、`projection`（`parallel` 或 `perspective`）、`vza`、`vaa`、`principalPlane`、`hemisphere`、`position*`、`fov`、`cruise*`、输出开关 `image`、`radiationProcess`、`energyProcess`、`temperature`、`albedo` | 旧版单一 `process` 开关在规范化时拆给两个过程开关（project-schema.js:1085-1086） |
| `control` | `samples`、`raytracingDepth`、`gpu`、`computeBackend`、`cpuThreads`、`qualityPreset`、`runTimeoutMinutes`、`shortwaveScatteringOrders`、`voxelCollisionOrders`、`radiationMaxSteps`、`couplingIterations`、`temperatureTolerance`、`temperatureRelaxation`、`stabilityCorrection`、`soilTemperatureMethod`、`soilInertiaHistory`、`vegetationTemperatureMethod`、`facetOutputFormat` 等 | 部分新引擎字段暂无界面控件，只能改 JSON（project-schema.js:962-969） |
| `fluid` | `enabled`、`voxelSize`、`timeStep`、`windDirection`、`outputWind` 等 | 默认关闭，仅 VoxelEB 使用 |
| `objects` | `count`、`names`、`items[]` | 每项含 `name`、`type`、`fileName`、`positionFile`、`meshes[]`、`distribution`、`movement`、`instanceCount` |
| `spectra`、`canopies`、`thermals`、`materials` | 命名预设数组 | 对象按名称引用 |
| `meteo` | `path`、`start`、`end`、`z`、`Tsold`（K）、`SatWater`、`dTime`（s）、`Ca`、`latitude`、`longitude`、`year`、`timezone` | `Tsold` 被限幅到 150-400 K |
| `atmosphere` | `enabled`、`model`、`waterVapor`、`aerosol`、`visibility`、`lutFile` | 默认关闭 |

物理含义与方程见《理论与方法》，不在此展开。

#### 3.6.3 normalizeProject 与 validateProject

`normalizeProject` 的处理顺序（project-schema.js:1057-1058）：深拷贝输入（无 `structuredClone` 时用 JSON 往返，以兼容旧 Node，1062-1065）→ 预算传感器、光照、气象派生量 → 规范化光谱（合并水体别名 `water_radiation` → `water_surface` 等，237-241；开孔率、纹理、镜面项）→ 规范化对象（别名、分布密度、火/雾介质、移动参数）→ 逐节与缺省值合并并限幅。

`validateProject` 在规范化结果上检查（project-schema.js:1476-1477）：根节点与 `format`、`schemaVersion`（1 到 6 的整数）、`mode`；场景尺寸与体元；地形水热场仅 VoxelEB；传感器至少一个波段与一个观测角；光伏与薄膜材质的参数与模式限制（光伏热电耦合须 `eFacetEB`）；多层墙体（1-6 层、总厚 ≤ 5 m）；车辆座舱与发动机（内饰份额之和为 1，发动机三份额之和为 1）；船舶水线与航速；水体初始水温模式；光谱开孔率与分类纹理。校验通过与否由 `errors` 数组决定，调用方必须检查 `valid`。

#### 3.6.4 projectToXml 的现状

`projectToXml` 拼接 `<Control>`、`<Geometry>`、`<Atmosphere>`、`<Scene>`、`<Attribute>`、`<Meteorology>` 六段（project-schema.js:1849-1955）。仓库内除 `tools/test-vehicle-parts.mjs` 外没有调用者（grep 仅命中该测试与本文件），而服务端运行前经 `prepareRuntimeSceneProject` 准备项目（server.mjs:48，5505），文件头注释所写“→ projectToXml → 引擎”（project-schema.js:15）与当前主运行链路是否一致，待核实，见服务端章。

### 3.7 project-unified.js

统一工程 `format: 'stream-studio-project'`、`schemaVersion: 1`，一个 `project.json` 同时承载两个引擎的输入（project-unified.js:40-41）：

```
{ format, schemaVersion, name, description, createdAt, updatedAt, folders,
  shared:  { location{latitude, longitude, timezone, dem{file, band}, demInfo}, time{overpass, year},
             sky{mode, zenith, azimuth, direct, diffuse, ...}, atmosphere, observation{vza, vaa},
             output{root}, compute{cpuThreads, device, backend}, bands{optical, thermal} },
  engines: { histream: { enabled, mode, scene, sensor, control, fluid, objects, spectra, ...meteo },
             stream:   { enabled, name, outputRoot, threads, surface, view, region, optical, thermal, temperature } } }
```

设计约定：共享层只放两个引擎语义一致的字段；光谱波段刻意留在各引擎段，因为 HiStream 的 `sensor.bands` 是渲染波长列表（nm，含 10500 的热红外），而 STREAM 的波段是带半高宽的响应函数（光学 nm、热红外 μm），合并会在一侧保存时覆盖另一侧（project-unified.js:31-33）。`outputRoot` 与 `threads` 也留在 STREAM 段，避免静默改写三维工程的输出位置与线程数（454-456）。

| 函数 | 作用 |
| --- | --- |
| `normalizeUnified(value)`(149) | 补全 `shared` 与两个引擎段；`mergeHistreamEngine`(224) 对 4 个预设数组整体替换，`mergeStreamEngine`(242) 缺省 `enabled=false` |
| `histreamProjectToUnified(legacy, previous)`(353) | 旧 HiStream 工程 → 统一工程；`previous` 用来保留另一引擎的内容；`applyHistreamToUnified`(373) 是别名 |
| `unifiedToHistreamProject(unified)`(379) | 统一工程 → 旧 HiStream 工程（`normalizeProject` 补全）；共享层字段写回 `outDir`、`scene.demFile`、`light`、`sensor.vza/vaa`、`control`、`meteo`、`atmosphere` |
| `streamProjectToUnified(scene, previous)`(554) | STREAM 影像工程 → 统一工程；只有常数经纬度、常数观测角、DEM 文件、过境时刻、显示时区进共享层（`sharedFromStream`，514） |
| `unifiedToStreamScene(unified)`(580) | 统一工程 → STREAM scene；引擎段中的栅格文件字段优先于共享层常数 |
| `projectFormatOf(value)`(68) | 返回 `unified`、`histream`、`stream-image`、`stream-scene` 或空串 |
| `createUnifiedProject`、`stringifyUnifiedProject`、`parseUnifiedProject`、`defaultStreamScene`、`normalizeStreamScene`、`streamSceneToEngine` | 便捷入口与缺省 |

代码中已标注的限制（DEFERRED 注释，未修复）：一侧清空 DEM 后共享层仍回退到旧 DEM，无法删除（project-unified.js:317，521）；`normalizeStreamScene` 总会补齐默认时区、过境时刻与观测角，使 `sharedFromStream` 中的 `previous` 回退分支实际不生效，旧 STREAM 工程会覆盖统一工程里 HiStream 侧设置的时区与观测角（530）；`parseUnifiedProject` 不校验 `schemaVersion`，更高版本会被当作 v1 并在保存时降级（650）。

### 3.8 scene-loader.js

职责：把 `config.objects.items` 载入 Three.js 预览场景，并驱动水面、火焰与随机游走动画。预览只用于所见即所得，不参与计算（src/renderer/src/scene-loader.js:1-4）。

| 导出 | 说明 |
| --- | --- |
| `loadXmlScene({ api, world, config, log, isCurrent })`(854) | 返回实际创建的实例数；若被更新的一次加载取代（`isCurrent()` 为 false）返回 `null` |
| `updateSceneDynamics(world, deltaSeconds, config)`(794) | 每帧推进着色器时间与随机游走；`deltaSeconds` 限幅到 0-0.1 s |
| `invalidateImageTextures(fileName)`(262) | 清除图片纹理缓存；缺参数时清全部 |

调用链：

```
loadXmlScene → 清除 world 中旧的 projectObject / vegetation / building
  → 对每个 item：有 positionFile → api.readText → positionsFromText（30）→ samplePlacements（41，上限 5000）
  → 有 fileName 且非参与介质：api.readText(OBJ) → OBJLoader.parse → 按文件缓存 → clone
        → 材质：类别纹理（loadImageTexture，272）或程序化预览纹理（createPreviewTexture，103）或顶点色
        → 按放置点实例化：位置 = 放置点 × sceneScale − 世界半宽，高度 = 放置 z + terrainHeight（721）
  → 无 OBJ 或 Fire/Fog/medium：primitive（680）基本体
  → attachRandomMovement（766）
```

坐标约定：位置文件每行 `x y z [scale [rotation]]`，x 北向、y 东向、z 高度，旋转为度；载入时放置点减去偏移 `(x - offsetX, y - offsetZ, z - offsetY)`，即北向对 `offsetX`、东向对 `offsetZ`、高度对 `offsetY`（scene-loader.js:15，886-891）。`sceneScale = min(1, 45 / max(x, y, 1))`（864）。图片纹理通过 `GET /api/project/image?path=` 加载（275），使用 `flipY = false` 与最近邻采样，以便与求解器投影一致（270-283）。单个对象失败只写日志，不中断其余对象（1002-1005）。

### 3.9 通信层与前后端接口对照

#### 3.9.1 http.js、storage.js

- `request(path, options, binary)`：`fetch` 后统一错误转换——网络失败为“无法连接本地服务”，响应非 JSON 为“请求失败：状态码”，HTTP 错误优先取服务端 `error` 字段（src/renderer/src/http.js:14-31）。`binary` 为 `{ type, decode }`，响应 `Content-Type` 以 `type` 开头时用 `decode(ArrayBuffer)`。`post(path, value, binary)` 是 JSON 请求体的 POST（34-36）。
- `appStorage`：桌面版通过 `window.streamStudioSettings`（`desktop/preload.cjs:9-13` 暴露的同步 IPC：`read`、`setItem`、`removeItem`）读写主进程设置，网页版用 `localStorage`；桌面版首次读到未记录的键时，把 `localStorage` 旧值迁移过去；`setItem` 失败只告警并返回 `false`（src/renderer/src/storage.js:14-41）。

#### 3.9.2 web-api.js 与服务端路由

`webApi` 为三维工作台的全部后端调用。非 GET 请求必须带 `application/json`，否则服务端拒绝（server.mjs:5568-5569）。`readResult` 通过 `Accept` 头请求二进制结果格式 `application/vnd.stream-studio.result-v1`，响应体为 `SSR1` 魔数、uint32 头长度、头 JSON、按 8 字节对齐的 Float32/Float64 数据块，头 JSON 中的 `{ $ssBuffer: i }` 被替换为对应类型化数组视图（web-api.js:19-42）。

| webApi 方法 | 请求 | server.mjs 行 |
| --- | --- | --- |
| `defaults()` | GET `/api/defaults` | 4598 |
| `devices(executable, refresh)` | GET `/api/devices?executable=&refresh=` | 4615 |
| `onSimulationEvent(cb)` | SSE GET `/api/events`，返回关闭函数 | 4625 |
| `listExamples()` / `openExample(id, directory)` | GET `/api/examples` / POST `/api/examples/open` | 4636 / 4643 |
| `openProject(path, mode)` | POST `/api/project/open` | 4652 |
| `chooseXml(mode)` | POST `/api/project/pick`，取消返回 `null` | 4660 |
| `createProject(project)` | POST `/api/project/create` | 4671 |
| `saveProjectAs(project)` | POST `/api/project/save-as` | 4682 |
| `generateTarpCover` / `importObj` | POST `/api/project/tarp-cover` / `import-obj` | 4695 / 4709 |
| `importMeteo` / `importDem` / `inspectDem` | POST `/api/project/import-meteo` / `import-dem` / `dem-info` | 4722 / 4746 / 4783 |
| `importSpectrum` / `importTexture` | POST `/api/project/import-spectrum` / `import-texture` | 4802 / 4842 |
| `saveDistribution(data)` | POST `/api/project/distribution` | 4866 |
| `readText(path)` | POST `/api/project/read` | 4889 |
| `saveXml({path, content, project})` | POST `/api/project/save` | 4905 |
| `preflight(data)` / `run(data)` | POST `/api/run/preflight` / `/api/run` | 4933 / 4960 |
| `stop()` / `reset()` | POST `/api/stop` / `/api/reset` | 5329 / 5352 |
| `deleteResults` / `listResults` / `readResult` / `compareResults` | POST `/api/results/delete-all` / `list` / `read` / `change` | 5365 / 5377 / 5394 / 5432 |
| `openPath(path)` | POST `/api/open` | 5449 |
| `openHelpDocument(document)` | POST `/api/help/open` | 4603 |
| `histreamProxyInfo()` / `runHistreamProxy(data)` | GET `/api/histream-proxy/info` / POST `/api/histream-proxy/run` | 4494 / 4527 |
| `chooseObj()` | 浏览器文件框，不访问服务端 | web-api.js:52-70 |
| `chooseExecutable(message)` | `window.prompt` 输入引擎路径并存入 `histreamExecutable` | web-api.js:80-89 |

`webApi.chooseExecutable` 写入的自定义引擎路径必须由管理员加入服务端环境变量 `STREAM_STUDIO_ENGINE_ALLOWLIST` 并重启，否则运行时被拒，界面据错误文本中的 `ENGINE_ALLOWLIST` 给出专门提示（main.js:9244-9270）。

#### 3.9.3 stream/api.js 与服务端路由

`streamApi` 对应 `server/stream/index.mjs` 的路由表（server/stream/index.mjs:659-691），事件通道为 `GET /api/stream/events`（702）。

| streamApi 方法 | 路由 |
| --- | --- |
| `status()` | GET `/api/stream/status` |
| `run(scene)` / `stop(id)` | POST `/api/stream/run`（请求体 `{ scene }`） / `/api/stream/stop` |
| `pick(kind)` / `inspect(paths)` / `raster(path, band)` | POST `/api/stream/pick` / `inspect` / `raster` |
| `runs(outputRoot)` / `runInfo(outputDir)` / `classStats(outputDir)` | POST `/api/stream/runs` / `run-info` / `class-stats` |
| `openProject(path)` / `saveProject(data)` | POST `/api/stream/project/open` / `project/save` |
| `era5Packs` / `registerEra5` / `removeEra5` | POST `/api/stream/era5/list` / `register` / `remove` |
| `forcingPresets()` / `forcingPreset(name)` | GET `/api/stream/forcing/presets` / POST `forcing/preset` |
| `checkForcing` / `registerForcing` / `saveForcingMetadata` / `readForcingMetadata` | POST `forcing/check` / `register` / `metadata/save` / `metadata/read` |
| `openPath(path)` | POST `/api/open`（与主界面共用） |
| `onEvent(cb)` | SSE，返回取消订阅函数；无 `onerror` 处理，断线重连对调用方不可见（api.js:41） |

#### 3.9.4 本地存储键

| 键 | 用途 | 位置 |
| --- | --- | --- |
| `streamStudioLanguage`（旧键 `histreamStudioLanguage` 仅读取） | 界面语言 | i18n.js:27-28 |
| `histreamExecutable` | 自定义引擎路径 | web-api.js:82-87 |
| `histreamExampleDirectory`、`histreamProjectDirectory` | 示例与新建/另存为的目录记忆 | main.js:3598，8650，8841 |
| `streamStudioImage`（旧键 `streamStudioScene`、`histreamStreamScene`） | STREAM 表单草稿 | workspace.js:36-37 |
| `streamStudioImageProject` | STREAM 当前工程 | workspace.js:38 |

### 3.10 i18n.js、menubar.js、material-specular.js

#### 3.10.1 i18n.js

源文案是中文，缺省语言 `zh-CN`；切到 `en-US` 时就地翻译 DOM 文本节点与 `title`、`placeholder`、`aria-label` 属性，切回中文时用缓存的原文还原（src/renderer/src/i18n.js:1-20）。

| 部分 | 行号 | 说明 |
| --- | --- | --- |
| `ENGLISH_MESSAGES` | 34-889 | 整句词典，键必须与界面中文完全一致（去首尾空白） |
| `skipSelector` | 904-909 | 不翻译的元素：`textarea`、`pre`、`code`、`.console-output`、`.path-input`、`#resultFiles`、`#projectName`、`[data-i18n-ignore]` 等；排除区内文本若恰为词典整句仍会翻译 |
| `PHRASE_REPLACEMENTS` | 914 | 子串替换表，按长度降序排序（1020） |
| `DYNAMIC_TRANSLATIONS` | 965 | 带数字或名称的动态文案，正则加模板 |
| `t(source)` | 1054 | 中文模式原样返回；英文模式先查词典，再走 `translateDynamic`（1035：动态正则 → ` · ` 分段 → 短语替换） |
| `startLocalization({ onChange })` | 1156 | 绑定 `#languageToggle`，首次整页翻译，并用 `MutationObserver` 自动翻译之后新增的节点 |

语言切换派发 `window` 事件 `streamstudio:languagechange`（1135）。`main.js` 在 `startLocalization` 的 `onChange` 中重绘检查器、徽标、边界引导、巡航与太阳提示（main.js:9297-9301）。没有词条的文案原样保留，不报错。

#### 3.10.2 menubar.js

`createMenubar({ root, route })` 返回 `{ close }`。`route(item)` 返回真表示菜单项已被接管，此时在捕获阶段 `stopPropagation`，菜单项自身的 click 处理器不再执行；随后总是关闭菜单（src/renderer/src/menubar.js:57-62）。键盘：上下键循环、左右键切换相邻菜单、Tab 关闭；有 `dialog[open]` 或 `.drawer-backdrop:not([hidden])` 打开、或焦点在输入控件时不响应 Ctrl+N/O（92-95）。

#### 3.10.3 material-specular.js

| 函数 | 说明 |
| --- | --- |
| `normalizeSpecular(value)`(23) | 规整为 `{ enabled, fraction: 0-1（缺省 0.5）, roughness: 0.05-1（缺省 0.25） }` |
| `meshSpecular(mesh, spectra, control, fallbackSpectrum)`(36) | `fraction/roughness` 取自光谱，`enabled` 优先级为 `mesh.specularEnabled` > `mesh.specular` > 旧共享开关 `control.specularEnabled` |
| `meshSpecularRunError(mode, configuration)`(46) | 非 `eRaytracing` 且有启用镜面反射的表面时返回提示文案，否则空串；`runSimulation` 在运行前调用 |
| `meshSpecularFields(setting)`(63)、`meshSpecularFromRow(row)`(69) | 逐 Mesh 的“使用镜面反射”下拉框及其读取 |
| `specularParametersFromValues`(75)、`specularParameterFields`(83) | 光谱对话框中的比例与粗糙度输入 |

### 3.11 stream/workspace.js

STREAM 遥感影像工作台，一个工程对应一个过境时刻的影像模拟：RT 模式直接给定四组分温度，EB 模式由逐小时气象强迫求解能量平衡，同化模式用观测更新背景场。本文件只负责界面、表单规范化与校验、服务端交互和结果查看，物理计算在 `models/stream`（src/renderer/src/stream/workspace.js:3-6）。

#### 3.11.1 模块级常量与纯函数（1-503 行）

| 内容 | 行号 |
| --- | --- |
| 传感器预设 `OPTICAL_PRESETS`（sentinel2、landsat、modis、rgbnir；光学条目为名称、中心波长 nm、半高宽 nm）、`THERMAL_PRESETS`（单位 μm） | 42-53 |
| `IGBP` 地类、`PRODUCTS`（输出 GeoTIFF 文件名 → 显示名、类别、单位、小数位、缺省色带）、`BAND_LABELS`（引擎波段描述正则 → 中文）、`COMPOSITES`、`COLORMAPS`、`STRETCHES` | 65-140 |
| 输入字段声明表 `SURFACE_FIELDS`、`LOCATION_FIELDS`、`GEOMETRY_FIELDS`、`COMPONENT_FIELDS`、`PACKAGE_FIELD`，合并为 `ALL_FIELDS` | 146-167 |
| 表单规范化：`migrateLegacy`(213)、`defaults`(229)、`normalizeScene`(253)、`normalizeAssimilation`(288)、`exportField`(308)、`exportScene`(315) | 190-324 |
| EB 起转规则：`spinupNodes`(335)、`ebPlan`(361)、`packProblems`(387)、`packWarnings`(403) | 326-413 |
| 坐标与绘图：`utmToGeographic`(418)、`mercatorGroundScale`(450)、`buildLut`(466)、`decodeFloat32`(485)、`loadDraft`(494) | 415-499 |

`ebPlan` 的规则与引擎 `models/stream/app/image.cpp` 一致（workspace.js:326-358）：起转节点数缺省 7，范围 0-240；起算点是过境前最后一个整点（须早于过境时刻 5 分钟以上）往回数的第 N 个整点；数据包覆盖不足时起点逐小时后移，ERA5-Land 需再早一小时。修改这套规则必须与引擎同步。

字段对象统一为 `{ mode: 'file' 或 'value', file, value, band }`（`normalizeField`，192）；`exportField` 导出为 `{ file[, band] }`、`{ value }` 或 `{}`（308-311）。

#### 3.11.2 createStreamWorkspace 闭包

入口 `createStreamWorkspace({ toast, onClose })` 返回 `{ open, openProject, close, command, isOpen }`（workspace.js:505，2402）。工作台状态都封装在闭包内：`form`（当前表单，初值为本地草稿）、`project`、`dirty`、`editRevision`、`projectGeneration`、`jobGeneration`、`running`、`starting`、`currentJob`（`'pending'` 表示任务 id 尚未返回）、`pendingEvents`、`packs`、`inspections`、`rasterCache`（上限 24 条）、`view`（查看器状态）（516-542，1692-1695）。

| 区段 | 行号 | 主要函数 |
| --- | --- | --- |
| 布局与参数面板 | 544-1000 | `render`(548)、`renderParams`(884)、`temperatureGroup`(851)、`assimilationGroup`(693) |
| 输入、栅格探测与校验 | 1002-1288 | `markDirty`(1005)、`inspectInputs`(1021)、`onParamInput`(1071)、`fieldsInUse`(1218)、`validate`(1228)、`runPayload`(1273) |
| 工程新建、保存、打开 | 1290-1404 | `newProject`(1319)、`saveProject`(1339)、`openProject`(1376)、`pickProject`(1398) |
| 运行控制 | 1406-1597 | `startRun`(1412)、`refreshStatus`(1484)、`showRun`(1537)、`renderCounts`(1564) |
| 结果查看器 | 1599-2079 | `selectProduct`(1661)、`raster`(1701)、`showView`(1719)、`buildImage`(1766)、`draw`(1828)、`drawHistogram`(1897)、`readout`(2051) |
| 日志 | 2081-2115 | `addLog`(2085)，普通行最多 500 条，GDAL/PROJ 提示合并为可折叠块 |
| 事件绑定 | 2117-2229 | `bind`(2123) |
| 显示与关闭 | 2231-2257 | `show`(2237)、`close`(2253) |
| 气象数据包管理 | 2259-2319 | `renderEra5List`(2263)、`registerEra5`(2290)、`removeEra5`(2312) |
| 服务端事件 | 2321-2377 | `handleEvent`(2328) |
| 对外命令 | 2379-2400 | `command`(2381) |

`command(name)` 接受：`new`、`new-rt`、`new-eb`、`mode-rt`、`mode-eb`、`open`、`save`、`save-as`、`era5`、`run`（2379-2399）。

提交运行：

```
startRun()（1412）
  → 占位 starting；工程未保存则 saveProject(false)（输出目录依赖工程路径）
  → inspectInputs() → validate()（1228）→ 有问题则 toast 并终止
  → api.run(runPayload())（1273）→ currentJob = job.id；回放 pendingEvents
SSE /api/stream/events → handleEvent（2328）
  era5 登记任务事件 → 更新 #swEra5Status
  影像任务：job-queued / job-started / job-progress / job-log / job-closed
  job-closed → api.runInfo(outputDir) → showRun → loadClassStats
```

异步安全依赖三个“代”：`projectGeneration`、`jobGeneration`、`inspectToken`；`invalidateProject`（1305）在切换工程时一并递增，各异步步骤结束后用 `isCurrent()` 判断结果是否过期。`refreshStatus` 还兜底处理事件流中断：若本任务已不在服务端队列但没收到结束事件，就读取输出目录的运行信息（1481-1517）。

已知限制（DEFERRED 注释）：`runPayload` 把 `projectPath` 放进 `scene`，服务端读取的是 `data.projectPath`，因此服务端按工程推导输出目录的分支不会执行，实际依赖前端拼出的 `outputRoot`（1277）；保存工程靠匹配中文错误文本 `/已存在$/` 判断文件已存在（1351）；栅格缓存按条目数而非字节数限制，键不含文件时间戳（1691）。

### 3.12 stream/forcing-panel.js

`createForcingPanel({ api, getForm, getProject, getPack, markDirty, root })` 管理气象强迫元数据（字段含义见 `docs/forcing-metadata.md`）。场景里只保存 `temperature.forcing = { metadata, files }`，面板内部持有完整副本 `state.working`，每次编辑后写回最小等价值（src/renderer/src/stream/forcing-panel.js:17-20）。

`metadata` 的存储形态由 `currentValue`（124）决定：未加载则保持原值；有未改动的工程元数据文件路径则存路径；与基础预设等价则存预设名（ERA5 缺省存 `undefined`，保持旧工程行为）；否则存 `working` 的完整副本。数据源类型为 `era5`、`era5-land`、`gldas-noah-0.25`、`gldas-noah-1.0`、`station-csv`、`custom`（23-26）。

| 函数 | 行号 | 作用 |
| --- | --- | --- |
| `syncFromForm` | 147 | 场景变化（如打开工程）后同步面板状态；`committedKey` 相同则不动 |
| `load` / `chooseType` | 166 / 196 | 载入预设或工程元数据文件；用 `token` 丢弃过期响应 |
| `check` | 322 | `api.checkForcing`，网格形态附 `pack`，站点形态附 `files` |
| `save` | 339 | `api.saveForcingMetadata` 写工程元数据文件，要求工程已保存 |
| `importFile`、`reset`、`addStationFiles` | 354、370、377 | 载入外部元数据、还原预设、追加站点 CSV |
| `onClick`、`onChange`、`onInput`、`onToggle` | 392-440 | 事件入口，返回是否已处理；由 `workspace.js` 的 `onParamInput` 等先转发给本面板（1068-1070） |
| `summaryLines(resolved)`（导出） | 79 | 把引擎检查结果转成摘要行 |

外部接口：`html()`、`afterRender()`、`isStation()`、`currentDocument()`、`problems()`（阻止运行的问题）、`label()`（604-619）。站点形态下外层的 ERA5 数据包行被隐藏，运行校验不再要求数据包（workspace.js:1262）。

### 3.13 修改指南与注意事项

1. 新增或修改三维工程字段（如 `control` 下新参数）：先改 `createDefaultProject`（project-schema.js:889）给新工程缺省，再改 `normalizeProject`（1060）补默认、限幅与旧工程兼容，必要时在 `validateProject`（1479）加校验，再在 `renderInspector`（main.js:2724）与 `bindInspector`（3109）加控件与绑定，最后同步服务端与引擎对该字段的读取。字段会进入统一工程时还要改 `histreamEngineSection` 或 `sharedFromHistream`（project-unified.js:266，299）与 `unifiedToHistreamProject`（379），否则统一工程往返会丢字段。若 schema 不兼容，递增 `PROJECT_SCHEMA_VERSION`（project-schema.js:27），旧版本程序会拒绝更高版本（1466）。
2. 新增属性面板：在 `index.html` 的 `#sceneTree` 增加 `.tree-item[data-panel]`，在 `panelTitles`（main.js:2530）加标题，在 `renderInspector` 增加分支，在 `bindInspector` 增加绑定，需要徽标时在 `renderNavBadges`（3424）与 `index.html` 加 `<em id>`。
3. 新增数值输入：用 `bindValue(id, setter, converter, rebuildScene)`（main.js:3070），限幅写在 `converter` 里并与 `validateProject` 的下限一致；空串或非数值会恢复上一次有效值，不要自己写 `Number('')`。
4. 新增后端接口：在 `web-api.js` 加方法（`post`/`request`），在 `server.mjs` 加路由；非 GET 请求必须是 `application/json`（server.mjs:5568-5569）。STREAM 侧在 `stream/api.js` 与 `server/stream/index.mjs` 路由表同时加（server/stream/index.mjs:659-691）。需要二进制大数组时服务端走 SSR1 格式，前端无需改解码器（web-api.js:19-42）。
5. 新增结果类型：服务端 `/api/results/read` 返回新的 `kind`，在 `openResultFile`（main.js:8419-8427）加分支，并在 `resultFilesForView`（8449）与 `renderResultFiles` 决定它出现在哪个视图；离开时调用 `disposeResultViewer`（7134）释放三维资源。
6. 新增界面文案：中文直接写在代码中，英文词条补到 `ENGLISH_MESSAGES`（i18n.js:34-889）；带数字或名称的句子加到 `DYNAMIC_TRANSLATIONS`（965）；不应翻译的区域加 `data-i18n-ignore` 或加入 `skipSelector`（904）。通过 `innerHTML` 生成的内容靠 `MutationObserver` 自动翻译，但在代码里拼接并用 `toast`、`addLog` 显示的文本需要自己调用 `t()`。
7. 预览外观：新增预览纹理预设时，同时改 `PREVIEW_TEXTURE_PRESETS`（main.js:84）与 `createPreviewTexture`（scene-loader.js:103）；预览不影响计算，计算用的 `physicalTexture`、`classTexture` 走服务端与引擎。位置文件坐标与偏移的对应关系见 3.8，不要把 `offsetY`（高度）当作东向偏移。
8. 修改质量档位数值：改 `QUALITY_PRESET_VALUES` 与 `QUALITY_PRESET_MODE_VALUES`（project-schema.js:83-91），数值由回归算例标定并写入用户手册，改动前对照该处注释（72-79）；界面状态由 `qualityPresetMatches` 推断，`markQualityCustom`（main.js:3019）把手改参数记为 `custom`。
9. `project-schema.js` 与 `project-unified.js` 不得引入 DOM、`window` 或 Node 专有 API，否则 `server.mjs` 或浏览器之一会加载失败；`sensorBandValues` 的取整、末点追加、容量与排序规则与引擎 `projectjson.cpp` 一致，改动必须两端同时改（project-schema.js:301-304）。
10. 修改保存或另存流程时保持“快照加代次”模式：先 `flushProjectTextSync`，记录 `projectGeneration` 与 `editRevision`，异步完成后判断是否仍是当前工程，被取代时返回 `null` 而不是 `false`（main.js:3695-3760，8822-8851）。直接给 `state.config` 赋新对象会使已绑定的检查器与对话框处理器仍引用旧对象，只有 JSON 抽屉手改才允许替换（main.js:3712-3716）。
11. 解析引擎 stdout 的新诊断行：在 `recordRunQualityLine`（main.js:6513）加正则，返回日志文本、空串（隐藏）或 `null`（不是诊断行），并在 `runQualitySummary`（6566）补汇总；行格式须与引擎打印一致。
12. STREAM 新增输入字段：改字段声明表（workspace.js:146-167）、`defaults`（229）与 `normalizeScene`（253）、`exportScene`（315）；若属于共享语义还要改 `project-unified.js` 的 `streamSceneToEngine`（459）与 `unifiedToStreamScene`（580），并同步 `server/stream/config.mjs` 的配置生成；EB 起转规则改动须与引擎 `image.cpp` 同步（workspace.js:326-358）。
13. 构建与部署：`gui/` 是纯构建产物目录，`vite build` 会清空它，不要手放文件（vite.config.js:36-37）；开发时用 `npm run dev`（前端 5173，API 4173），页面直接访问 4173 只会得到提示（server.mjs:5583）。`three` 与 `vite` 版本固定，升级前需回归三维预览与结果查看器。
14. 快捷键与对话框：新增对话框要加入 Esc 的优先级链（main.js:9238）；新增全局 keydown 监听注意捕获顺序，菜单栏先于 STREAM 工作台注册（main.js:9028-9031），STREAM 工作台打开时 F5 与 Ctrl+S 由其捕获阶段抢先处理。
15. 注意区分两套“工程”：三维工程是 `project.json`（`histream-project`），STREAM 影像工程是 `stream-project.json`，统一工程把二者放进同一个 `project.json`；前端 `loadXml` 只处理三维结构，`streamProject` 一律转交 `streamWorkspace.openProject`（main.js:3500）。

### 3.14 待核实事项

- `projectToXml` 与服务端运行链路的关系：仓库内只有 `tools/test-vehicle-parts.mjs` 调用它，主运行链路是否改用 `project.json` 直接交给引擎，待核实（见服务端章）。
- 各 `/api/*` 路由的请求体与响应字段细节（如 `results/list` 的 `kind` 取值全集、`project/open` 返回的 `usdImport` 结构）以服务端章为准，本章仅依据前端调用侧归纳。
- `main.js` 4101-5006 与 5333-5802 区段（材质、预设对话框）只读了函数清单与关键入口，字段级细节待核实。

## 4. HiStream 基础层：models/histream/src/base 与公共构建

本章是代码模块参考，只回答“有哪些文件、各管什么、接口是什么、改哪里”。运行流程（进程如何启动、一次运行如何推进）见《运行原理说明》第 3 章，本章不重复；物理方程见《理论与方法》，本章不推导。

引用约定：括号内 `文件:行号` 均相对 `models/histream/`，为节省篇幅省略该前缀，例如 `src/base/engine.cpp:137` 即仓库内 `models/histream/src/base/engine.cpp` 第 137 行。行号会随提交漂移，核对时以函数名为准。无法从代码确认的内容标注“待核实”。

### 4.1 层次与依赖方向

基础层位于 histream 可执行程序的最底部，但它不是严格的“纯下层”。下面是实际的调用与包含关系：

```
histream.exe  (src/main.cpp)
  └─ Engine (base/engine.*)  按 Mode 选择后端：raytracing / voxeleb / voxelrt / facetrt / faceteb
       ├─ AppSetting (base/appsetting.*)       Vulkan 实例、设备、队列、隐藏窗口
       ├─ FileIO (base/fileio_json.cpp)  ──►  ProjectJson (base/projectjson.*)
       │      └─ 填充 RaytracingXml / VoxelRTXml / VoxelEBXml (base/structs.h)
       └─ 后端 setup → upload → create → run → destroy
            upload 内部：FileIO::readDefined → Compo → Scene(→ObjLoader/VoxelDesigner/hexvoxel) → Geometry
            create 内部：各后端 buffer.cpp 上传缓冲 → AccelStruct 建 BLAS/TLAS
            run 内部：VirtualScreen 回读 → FileIO::writeTIFData 写 GeoTIFF
```

依赖方向的几条事实：

1. 数据结构自底向上：`structs_cg.h`（CPU/GPU 共用）被 `structs.h` 包含（`src/base/structs.h:37`），`structs.h` 被几乎所有 base 文件包含。
2. base 反向依赖各后端：`scene.h`、`geometry.h`、`compo.h`、`virtualscreen.h` 直接包含 `src/raytracing/raytracingio.h`、`src/voxeleb/voxelebio.h`、`src/voxelrt/voxelrtio.h`（`src/base/scene.h:27-29`、`src/base/geometry.h:22-24`、`src/base/compo.h:21-23`、`src/base/virtualscreen.h:18-20`）；而三个 `*io.h` 又包含 `base/accelstruct.h` 与 `meshio.h` 等（`src/voxelrt/voxelrtio.h:35`）。因此修改任一 `*io.h` 会触发 base 的大范围重编。
3. `engine.h` 包含全部后端头（`src/base/engine.h:23-38`）；Facet 后端受 `HISTREAM_ENABLE_FACET` 宏保护。
4. Facet 后端不经 FileIO：`Engine::input` 对 FacetRT/FacetEB 只记录路径后返回（`src/base/engine.cpp:168-182`）。它们只复用 base 中的头文件级模块：`reflectance_texture.h`、`soil_tir_emissivity.h`、`shortwave_split.h`（`src/facetrt/facetrtcore.cpp:56-59`），以及 `appsetting.cpp` 使用其 `facetvk::rankDevices`（`src/base/appsetting.cpp:18`）。
5. 同一个 `project.json` 被解析两次：`Engine::input` 先 `ProjectJson::load`（`src/base/engine.cpp:137`）做模式兼容检查与线程数设置，`FileIO::readJson` 又 `load` 一次（`src/base/fileio_json.cpp:622`）。

### 4.2 入口与调度

#### 4.2.1 src/main.cpp

| 文件 | 行数(约) | 职责 | 关键导出或函数 |
| --- | --- | --- | --- |
| `src/main.cpp` | 194 | 命令行解析、模式分派、驱动 Engine 生命周期 | `main`、`printUsage`、常量 `kExitUsage=64` |

命令行（`src/main.cpp:59-65`）：

```
histream <eVoxelEB|eFacetEB|eFacetRT|eVoxelRT|eRaytracing> <project.json> [output.json]
histream <eVoxelEB|eVoxelRT|eRaytracing> --builtin-example
histream --list-devices
```

处理顺序：Windows 下关闭崩溃与断言对话框（`src/main.cpp:76-77`）；构造 `ExtensionRegistry` 并调用 `registerExtensions`（`src/main.cpp:101-102`）；无参数打印用法并返回 64（`src/main.cpp:103-107`）；命中扩展模式则直接 `extension->run` 并返回其退出码（`src/main.cpp:108-115`）；`--list-devices` 打印一行 `HISTREAM_DEVICES<TAB>JSON`（`src/main.cpp:117-125`）；内置五种模式校验，Facet 模式不允许 `--builtin-example`，空工程路径返回 64（`src/main.cpp:130-149`）；设置 `GDAL_FILENAME_IS_UTF8=NO`（`src/main.cpp:154`）；然后 `input → create → run → destroy`（`src/main.cpp:159-192`）。

退出码（与 `server.mjs` 的 `HISTREAM_EXIT` 一一对应，`server.mjs:119`）：

| 退出码 | 含义 | 产生位置 |
| --- | --- | --- |
| 0 | 成功 | `Engine::run` 返回 0 |
| 1 | 运行失败，或运行成功但清理失败 | `src/main.cpp:185,191` |
| 2 | 仅 FacetEB：完成但有节点未收敛，结果保留 | `src/base/engine.cpp:298` |
| 3 | 资源创建失败（`create` 返回 false 或抛异常） | `src/main.cpp:167-175` |
| 4 | 初始化失败（`input` 抛异常） | `src/main.cpp:160-163` |
| 5 | 设备查询失败（`--list-devices`） | `src/main.cpp:121-124` |
| 64 | 用法错误 | `src/main.cpp:56,106,133,142,147` |

已知限制：Windows 下 `argv` 是 ANSI 代码页字符串，含代码页外字符的路径会被替换为 `?`，源码内已有 DEFERRED 标注（`src/main.cpp:126`）。

#### 4.2.2 Engine（engine.h / engine.cpp）

公共接口（`src/base/engine.h:47-61`）：

```
void Engine::input(std::string path, std::string V, std::string outputPath = {});  // 解析模式+工程，建对象并 setup/upload
void Engine::init(Mode mode);        // 建后端对象与 IO 对象；非 Facet 模式还初始化 AppSetting
bool Engine::create();               // 创建运行资源；Facet 模式在此才 setup/upload/create
int  Engine::run();                  // 返回进程退出码
void Engine::destroy();              // 释放后端；非 Facet 模式再 appSetting.destroy()
```

`input` 的步骤（`src/base/engine.cpp:115-211`）：字符串转 `Mode`（未知抛 `runtime_error`，118-130）；`ProjectJson::load` 后检查两条模式约束——`scene.terrainEnvironment` 启用空气、土壤或 CO2 超额时只允许 eVoxelEB（139-146），含镜面反射的工程只允许 eRaytracing（147-148）；线程数取 `control.cpuThreads`（缺省 32，小于 1 回到 32，不超过逻辑处理器数），写入环境变量 `HISTREAM_CPU_THREADS` 与 `LP_NUM_THREADS`（152-165）；Facet 模式记录输入、输出路径与着色器目录后返回（168-182）；其余模式 `FileIO::readJson` → `init` → 后端 `setup(appSetting, io)` → `upload(fileio, io)`，并把 `definedDir`、`projectDir` 回填到 IO 对象（186-209）。

`init` 的 GPU 与后端选择（`src/base/engine.cpp:78-97`）：默认 GPU 0、后端 `"gpu"`；已读入工程时取该模式 `settingxml.theGPU` 与 `computeBackend`；环境变量 `HISTREAM_COMPUTE_BACKEND` 非空时覆盖；最后 `appSetting.init(gpuIndex, parseComputeBackend(backend))`。

`facetShaderDirectory(name)` 的查找顺序（`src/base/engine.cpp:252-274`）：可执行文件旁 `shader/<name>`、当前目录 `shader/<name>`、编译宏 `HISTREAM_FACETRT_SHADER_DIR` 或 `HISTREAM_FACETEB_SHADER_DIR`、最后回退当前目录路径。

`destroy` 对 Facet 模式提前返回，不调用 `appSetting.destroy()`（`src/base/engine.cpp:317-326`），因为 Facet 模式从未初始化 AppSetting。

#### 4.2.3 扩展机制（extension.h / extension.cpp）

“树外扩展”让外部目录在不修改核心的前提下新增命令行模式。接口：`ExtensionMode{name, usage, run}`、`ExtensionRegistry::add/find/modes`、`registerExtensions(ExtensionRegistry&)`、`executablePath(argv0)`（`src/base/extension.h:33-62`）。`registerExtensions` 不手写，由 CMake 用 `cmake/extensions.cpp.in` 生成 `build/generated/histream_extensions.cpp`（`CMakeLists.txt:474-476`）；每个扩展目录须提供 `histream-extension.cmake`，向 `HISTREAM_EXTENSION_REGISTRARS` 追加注册函数名（`CMakeLists.txt:451-473`）。`executablePath` 在 Windows 用 `GetModuleFileNameW`，其他平台读 `/proc/self/exe`（`src/base/extension.cpp:29-41`）。核心求解器不依赖扩展代码，没有扩展时注册表为空。

### 4.3 src/base 文件清单

目录共 69 个文件。为便于查找按职责分为六张表。7 个仅 5 行的 `.cpp`（`meshio.cpp`、`instanceio.cpp`、`voxelio.cpp`、`surfio.cpp`、`virtualio.cpp`、`gridio.cpp`、`pixelio.cpp`）加上 `defined.cpp` 只包含自己的头文件，无逻辑，与头文件合并为一行。

#### 4.3.1 设备、队列与基础工具

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| `engine.h` / `engine.cpp` | 92 / 339 | 顶层调度器 | `Engine::input/init/create/run/destroy` |
| `appsetting.h` / `appsetting.cpp` | 88 / 466 | Vulkan 实例、设备、队列、隐藏 GLFW 窗口、计算后端选择、设备清单 | `AppSetting::init/destroy/describeDevices`、`ComputeBackend`、`parseComputeBackend` |
| `extension.h` / `extension.cpp` | 64 / 43 | 树外扩展注册 | `ExtensionRegistry`、`registerExtensions`、`executablePath` |
| `queue.h` | 47 | 队列句柄结构，NVIDIA Apache-2.0 原作者文件 | `nvvk::Queue{queue, familyIndex, queueIndex}` |
| `vkcheck.h` | 24 | 非 `VK_SUCCESS` 转 `runtime_error`，使设备丢失（如 TDR）让整次运行失败 | `vkCheck(result, operation)` |
| `outputsize.h` | 41 | 输出影像元素数检查，超过 `maxStorageBufferRange` 抛异常 | `checkedOutputImageElements` |

#### 4.3.2 工程输入与通用工具

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| `projectjson.h` / `.cpp` | 76 / 484 | `project.json` 的只读封装、路径解析、波段与观测角生成 | `ProjectJson::load/scene/.../resolve/sensorBands/viewAngles`、宽松取值 `number/integer/boolean/string` |
| `fileio.h` | 123 | `FileIO` 声明：解析结果成员、气象与光谱读取、影像输出 | `FileIO::readJson/readXml/readDefined/readMeteo/writeTIFData/writeENVIdata` |
| `fileio_json.cpp` | 921 | `FileIO::readJson`：JSON 工程转 `*Xml` 结构 | `readJson`，匿名函数 `configureCommon/readScene/readAtmosphere/cruiseRoute` |
| `fileio.cpp` | 2596 | 旧 XML 工程解析（仅空路径内置示例仍在用）、`readDefined`、`readMeteo`、影像输出 | `readXml`、`readXxxXML`、`readDefined`、`readMeteo`、`writeTIFData` |
| `xmlexamples.h` / `.cpp` | 340 / 2209 | 内置最小示例工程（`--builtin-example`）；`.cpp` 后半为旧版示例的整段注释备份 | `XmlExamples::raytracing/voxelrt/voxeleb/watereb`、`rootDirectory` |
| `utils.h` / `.cpp` | 163 / 797 | 数值表与位置文件读取、GDAL 栅格读写、字符串与时间换算 | `Utils::readNumericRows/readPositionFile/readascfileinout/readImage*/saveImage*/expint/calculateDeltaT/calculateMonthAndDay` |
| `myfunction.h` / `.cpp` | 43 / 107 | 字符串切分与随机数小工具 | `myFunction::mySplitStr/Int/Float`、`generateRandomFloat` |
| `defined.h` / `.cpp` | 51 / 5 | `DefinedIO`：程序基目录、PROSPECT 与 BSM 系数表 | `DefinedIO::definedDir`、`m_fluspectCoeff` |

#### 4.3.3 数据结构与 IO 容器

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| `structs.h` | 780 | CPU 侧配置结构、枚举、全局宏 | `RaytracingXml/VoxelEBXml/VoxelRTXml`、`SettingXml`、`SceneXml`、`Mode`、`Type`、`PrimMesh/ObjMesh` |
| `structs_cg.h` | 660 | CPU/GPU 共用结构与 push constant | `RayRTSetting`、`VoxelLstSetting`、`VoxelRTSetting`、`Canopy`、`VoxelLink`、`SoilSet` 等 |
| `terrain_environment.h` | 98 | 地形起伏下的空气温湿、土壤含水量、CO2 预设经验场 | `TerrainEnvironment::validate/temperatureC/vaporHpa/soilMoisture/co2ExcessPpm` |
| `meshio.h` / `.cpp` | 84 / 5 | 网格与材质级数据容器（CPU 数组、名称表、GPU 缓冲句柄） | `MeshIO` |
| `instanceio.h` / `.cpp` | 47 / 5 | 实例级数据容器 | `InstanceIO::instances/instanceLinks` |
| `voxelio.h` / `.cpp` | 89 / 5 | 体元模式数据容器（NanoVDB 句柄、VoxelLink、各状态缓冲） | `VoxelIO` |
| `surfio.h` / `.cpp` | 36 / 5 | 叶面积密度 LAD 容器 | `SurfIO::lads` |
| `virtualio.h` / `.cpp` | 32 / 5 | 通用存储缓冲容器 | `VirtualIO::m_pBufferStorage` |
| `gridio.h` / `.cpp`、`pixelio.h` / `.cpp` | 27 / 5、30 / 5 | 空壳类，工程内未见使用 | `GridIO`、`Pixelio` |

#### 4.3.4 场景、几何与加速结构

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| `compo.h` / `.cpp` | 57 / 655 | 光谱、热属性、冠层与叶/土/水参数整理；PROSPECT 与 BSM | `Compo::createCompOptical/createCompProperty/fluspect/bsm/resolveSpectrals` |
| `scene.h` / `.cpp` | 74 / 3985 | 场景构建：RayTracing 网格实例，或体元 NanoVDB 加 VoxelLink | `Scene::createObjScene/createPrimObjScene/createPrimObj_*`、`XYZ2XZY` |
| `objloader.h` / `.cpp` | 104 / 1221 | OBJ、背景平面、DEM 地形转 `ObjMesh` | `ObjLoader::loadModel/loadMesh/createBackground/creatBackgroundFromDEM/interpolateZValues` |
| `voxeldesigner.h` / `.cpp` | 111 / 2056 | 参数化形状与高度栅格离散成体元及三角网 | `VoxelDesigner::createTriEntity/createTriBackground/createTriEntitiesFromTif*` |
| `voxel_boundary.h` | 68 | 体元集合提取外表面四边形 | `voxel_boundary::emit` |
| `hexvoxel.h` / `.cpp` | 173 / 1279 | 异质性体元核心：面元投影得聚集指数与叶面积密度，自包含 | `hexvoxel::loadObj/compute/toTsv/toJson`、`HexVoxelCg` |
| `hexneighbors.h` | 108 | 位掩码路径下的沿轴消光系数与邻体元间隙相关量 | `hexneighbors::axisQ/beta` |
| `hexprojection.h` | 81 | LOD 表路径下同类量 | `hexprojection::overlapLogRatio` |
| `geometry.h` / `.cpp` | 100 / 1123 | 相机与光源构造、角度与位姿切换、正射校正系数 | `Geometry::createGeometry/updateAngle/updateSensorPos/orthcorrect/updateSolarAngle` |
| `accelstruct.h` / `.cpp` | 53 / 185 | BLAS 与 TLAS 构建 | `AccelStruct::createAccelStruct/getTlas` |
| `virtualscreen.h` / `.cpp` | 42 / 100 | 设备缓冲到主机可见缓冲的回读 | `VirtualScreen::bufferToBuffer`（三个重载） |

#### 4.3.5 物理与气象头文件（header-only）

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| `atmosphere_lut.h` | 474 | 大气 LUT 读取（按路径缓存）、四轴多线性插值、透过率加程辐射校正、Planck 与其反函数、天空像元输出值 | `sampleAtmosphereLut`、`atmosphereCorrectRadiance`、`atmosphereSkyOutputValue` |
| `energy_output.h` | 135 | 能量平衡输出汇总：Rn、热通量和、闭合残差 | `histream::energy_output::calculate/Input/Balance` |
| `radiation_spectrum.h` | 79 | 入射光谱读取与短波归一化系数 | `readIncidentSpectrum`、`normalizedShortwaveCoefficient` |
| `shortwave_split.h` | 127 | 气象节点总短波拆分直射与散射（fixed 或 Erbs） | `shortwave_split::parseMode/split` |
| `soil_tir_emissivity.h` | 92 | 由土壤含水量修正热红外反射率 | `soiloptics::tirReflectance` |
| `urban_aerodynamics.h` | 78 | 城市街谷显热（Masson 2000 中性子集）：屋顶/道路墙面/街谷出口交换系数与街谷空气温度闭合；FacetEB 与 VoxelEB 共用，由 `control.urbanAerodynamics` 开启 | `urban::Canyon`、`urban::neutralExchange`、`urban::convectiveVelocity`、`urban::surfaceCoefficient`、`urban::canyonAirTemperature` |
| `reflectance_texture.h` / `.cpp` | 61 / 312 | 反射率扰动纹理加载、三平面采样、强度换算 | `loadReflectanceVarianceTexture`、`sampleReflectanceVarianceTriplanar`、`physicalTextureScale` |

#### 4.3.6 体元运行选项与规模限制

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
| --- | --- | --- | --- |
| `voxelflags.h` | 126 | 体元传输选项位，环境变量优先于工程字段 | `voxelflags::resolve/fromEnvironment/proxyMode` |
| `voxellimits.h` | 89 | 体元规模与单次分配上限检查，超限抛明确错误 | `requireStorageRange/requireIndex32/requireDispatch/requireAllocation` |
| `proxymesh.h` | 124 | 体元代理网格顶点格式与 48 MiB 分块上传 | `proxymesh::uploadPositionMesh/uploadChunked/requireFullMeshFits` |
| `voxelmerge_stats.h` | 195 | 两级体元合并潜力统计，只读，不改变行为 | `voxelmerge::report` |

### 4.4 关键文件详解

#### 4.4.1 AppSetting：Vulkan 环境

职责：把隐藏 GLFW 窗口、Vulkan 1.3 实例与设备、队列和表面一次建好，并选择计算后端。窗口仅为获得 `VkSurfaceKHR`，设置了 `GLFW_VISIBLE=FALSE` 与 `GLFW_NO_API`（`src/base/appsetting.cpp:217-221`）。

公共接口（`src/base/appsetting.h:58-88`）：

```
enum class ComputeBackend { Gpu, Cpu, Auto };
ComputeBackend parseComputeBackend(std::string);        // 不区分大小写，未识别按 Gpu（appsetting.cpp:42-49）
void AppSetting::init(int gpuIndex = 0, ComputeBackend = Gpu);   // 注册驱动 → initWindow → initVulkan → initSurface
void AppSetting::destroy();
static std::string AppSetting::describeDevices();       // JSON 设备报告，--list-devices 使用
nvvk::Context m_context;  std::vector<nvvk::Queue> m_queues;     // 供后端使用
```

关键步骤与约束：

1. `registerLavapipe`（`src/base/appsetting.cpp:68-94`）在触碰 Vulkan 前通过环境变量注册 Mesa lavapipe：Cpu 后端设置 `VK_DRIVER_FILES` 与 `VK_ICD_FILENAMES` 只加载 lavapipe；Auto 后端设置 `VK_ADD_DRIVER_FILES` 追加。清单位于可执行文件旁 `lavapipe/lvp_icd.x86_64.json`；Cpu 后端找不到则抛异常，非 Windows 平台选 Cpu 直接抛异常。
2. `initVulkan`（`src/base/appsetting.cpp:245-443`）声明必需与可选扩展：加速结构、光追管线、ray query 为必需（255-270）；`shader atomic float`、`atomic int64`、片段着色器 interlock 等为可选（274、296、309）。必需扩展缺失的设备会被 `getCompatibleDevices` 排除。
3. 设备分组与 GPU 序号（`src/base/appsetting.cpp:344-358`）：合格设备按类型分为 GPU 与 CPU 两组；GPU 组独立显卡在前，其余保持 Vulkan 枚举顺序。`gpuIndex` 是此顺序中的下标，越界抛异常并给出有效范围（379-382）。
4. 队列（`src/base/appsetting.cpp:422-430`）：`m_queues[0]=GCT`，`[1]=Compute`，`[2]=Transfer`；设备没有独立 C 或 T 队列时回退到 GCT 队列。lavapipe 只有 1 个队列时，把额外请求的队列降级为只请求 GCT（385-398）。
5. 无条件定义 `ENABLE_GPU_PRINTF` 并启用校验层的 debugPrintf（`src/base/appsetting.cpp:312-325`），源码已标注潜在性能代价（311）；发布版是否实际加载校验层取决于部署，待核实。
6. `describeDevices` 以 Auto 后端探测：临时建窗口与实例，生成 JSON 后立即销毁（`src/base/appsetting.cpp:163-174`）。JSON 含 `logicalProcessors`、`lavapipeThreadLimit`（32）、`gpus[]`（`name`、`type`、`voxelIndex`、`facetIndex`，-1 表示该后端不支持）与 `cpu.lavapipe`（126-158）。`voxelIndex` 用于 Raytracing、VoxelRT、VoxelEB，`facetIndex` 用于 Facet 后端，二者下标体系不同，不可互换。

#### 4.4.2 ProjectJson：工程文件的只读入口

接口见 `src/base/projectjson.h:23-76`。要点：

1. `load(path)`（`src/base/projectjson.cpp:86-117`）要求文件名必须是 `project.json`（不区分大小写）；`format` 缺省 `histream-project`；`schemaVersion` 缺省 1，允许 1 到 `kLatestProjectSchemaVersion=6`（41 行，与前端 `PROJECT_SCHEMA_VERSION=6` 一致，`src/renderer/src/project-schema.js:27`）；必须有非空 `configuration` 对象。失败一律抛异常。
2. 段访问器返回 `configuration` 下的段，缺失时返回空对象或空数组的静态哨兵引用，永不悬空（`src/base/projectjson.cpp:43-59,121-132`）。注意 `objects()` 实际取 `configuration.objects.items`，`meteorology()` 对应键名 `meteo`。
3. `resolve(value)`（137-154）：相对路径先按工程目录解析；不存在且以 `assets` 开头时，从可执行文件目录逐级向上查找。使用 `u8path`，把 JSON 的 UTF-8 字符串正确解释为路径。
4. 宽松取值 `string/number/integer/boolean`（221-268）：缺失或类型不符回退缺省值，不抛异常；`integer` 对整型值饱和到 `int` 范围；`number` 把 NaN 与 Inf 替换为缺省值。
5. `sensorBands()`（326-438）必须与前端 `sensorBandValues()` 逐条一致，因为服务端按前端波段表解析 Facet 结果。规则：6 位小数去重并升序；连续波段数 `floor((end-start)/step+1e-9)+1`，步长不能整除时追加终点；自定义波段预留名额，总数上限 800（`kMaxSensorBands`，328）；空列表回退 10500 nm（436）。缺省范围为 400 到 2500，步长 10（400-402）。
6. `viewAngles()`（445-484）：透视投影只取单个 `vza/vaa`；否则 `principalPlane` 生成主平面与垂直主平面各 31 个角（VZA 为 -75 到 75，步长 5，负值对应对侧）；`hemisphere` 生成 128 个黄金角采样（VZA 0 到 75 度）。VZA 限幅 [0, 89.999]，VAA 归一到 [0, 360)，天顶不论 VAA 合并为一项。
7. `hasSpecularReflection()`（274-313）同时兼容新写法（绑定自带 `specularEnabled` 或 `specular.enabled`）与旧写法（`control.specularEnabled` 加光谱项 `specular`）。
8. 内部 `executableDirectory()` 用 `GetModuleFileNameA`（`src/base/projectjson.cpp:63-73`），与 `engine.cpp` 的宽字符版本不一致，安装路径含代码页外字符时 `defined/` 与 `assets/` 查找会失败（源码 DEFERRED 标注，65 行）。

#### 4.4.3 FileIO：工程转配置、气象与影像输出

读入主路径是 `FileIO::readJson`（`src/base/fileio_json.cpp:618-921`），只支持 eRaytracing、eVoxelRT、eVoxelEB 三种模式，其他返回 false。路径为空时转 `readXml("")`，使用 `XmlExamples` 内置示例（619 行）。非空路径的 XML 解析（`readXml` 余下部分与 `readXxxXML`）在当前引擎中没有调用点：`readXml` 唯一的调用来自 `readJson` 的空路径分支（grep 全 `src` 确认）。

`readJson` 的调用链：

```
readJson(path, mode)
  ├─ ProjectJson::load
  ├─ 创建 m_pRaytracingXml / m_pVoxelrtXml / m_pVoxelebXml 之一（623-626）
  ├─ VoxelEB 光伏材质检查：背景或对象绑定光伏材质则抛异常，要求 eFacetEB（629-641）
  ├─ configureCommon：SettingXml、SensorXml、LightXml、AtmosphereXml（250-433）
  ├─ spectra → SpectralXml（648-687）；thermals → ThermalXml（690-695）
  ├─ canopies → CanopyXml（701-716）；materials → PropertyXml（723-861）
  ├─ readScene → SceneXml（866）
  └─ 仅 VoxelEB：meteo → MeteoXml、fluid → FluidXml、aerocond（871-919）
```

工程字段到配置结构的主要映射与限幅（均在 `src/base/fileio_json.cpp`）：

| JSON 字段 | 目标 | 规则 |
| --- | --- | --- |
| `control.samples` | `settings.n_sample` | 限幅 1 到 1024（261） |
| `control.depth` | `settings.maxDepth` | 至少 1（258）；RayTracing 改用 `control.raytracingDepth`，缺省 16，限幅 1 到 64（409-412） |
| `control.periodicTraversalCount` | `periodicNeighborCount` | 缺省由旧字段 `periodicBoundary` 与 `periodicNeighborCount` 折算，限幅 0 到 20（263-269） |
| `control.shortwaveScatteringOrders` | 同名 | 只允许 1 到 3，越界抛异常（288-290） |
| `control.voxelDirectionRotation` | `voxelDirectionRotation` | 只允许 `legacy` 或 `rot`（297-302） |
| `control.voxelProxyGeometry` | `voxelProxyGeometry` | `full`、`position`、`positionNoIndex`（306-314） |
| `sensor.cruise*` | `uavPoses`、`uavViewAzimuths` | `cruiseRoute` 生成直线往返、矩形、随机三种航线，点数限幅 1 到 10000，水平坐标按场景周期环绕（130-223） |
| `light.direct` | `lightxml.direct/diffuse` | 限幅 [0,1]，散射为 1 减直射（385-386） |
| `light.shortwaveIrradiance` | 同名 | 限幅 [0, 2000]（389） |
| `atmosphere.*` | `AtmosphereXml` | 非法模型与气溶胶回退，水汽限幅 [0.5, 5]，能见度限幅 [10, 50]（228-242） |
| `meteo.dTime` | `meta.dTime` | 必须为正，否则抛异常（884-886） |

材质 `materials[]` 按类型分支（723-861）：木质（`energyModel=="wood"` 或名为 `tree_branch_wood`、`tree_trunk_wood`）强制按 BUILDING 处理并使用木材热参数；植被取叶片生化参数（`Tparam` 不足 5 个时用缺省补齐并告警）；水体取热容、混合深度与反射模型；其余按土壤，船体（`Ship`）可选航速、水线、钢板等参数，越界一律抛异常。

读取辅助：

1. `readDefined(definedio)`（`src/base/fileio.cpp:1963-1997`）按列 18 次读取 `<definedDir>\defined\optipar.txt` 填 `m_fluspectCoeff`；路径用硬编码反斜杠拼接，且 `readascfileinout` 打不开文件时不报错（1976 行的 DEFERRED 标注），文件缺失会表现为系数向量为空，随后 `resolveSpectrals` 因系数不足抛异常。调用点：`src/raytracing/raytracing.cpp:322`、`src/voxelrt/voxelrt.cpp:253`、`src/voxeleb/voxeleb.cpp:402`。
2. `readMeteo(...)`（`src/base/fileio.cpp:2006-2191`，调用点 `src/voxeleb/voxeleb.cpp:267`）。气象文件格式：首行为节点数，其后每行至少 7 个数 `t Ta ea p u Rin Rli`，可选第 8、9 列为水平面直射与散射短波（W m-2，缺失记 NaN，仅 `diffuseFractionMode=erbs` 使用）。校验：文件为空、头非法、节点数不为正、字段非有限、`p<=0` 或 `ea/u/Rin/Rli<0` 均抛异常（2020-2067）。随后建立 2162 个波长点的网格：400 到 2400 nm 步长 1（2001 点），2500 到 15000 nm 步长 100（126 点），16000 到 50000 nm 步长 1000（35 点）（2108-2121）；读入 `Esun_.dat` 与 `Esky_.dat` 并用梯形积分归一，短波系数缩放因子为 1000（2127-2189）。
3. `writeTIFData(...)`（`src/base/fileio.cpp:2335-2529`）：写 Float32 多波段 GeoTIFF（`INTERLEAVE=BAND`、`BIGTIFF=IF_SAFER`，2390-2394），不写地理参考，只写元数据 `HISTREAM_IMAGE_ORIENTATION=NORTH_UP_EAST_RIGHT` 与太阳角（2413-2417）。文件名：有时间则 `T=DOY<d>_<hh>-<mm>_VZA=_VAA=[_P=]<suffix>.tif`，否则 `SZA=_SAA=_VZA=_VAA=...`（2344-2380）。同时追加统计 CSV `result_statistics_{voxeleb,voxelrt,raytracing}.csv`，同一 FileIO 实例首次写入覆盖并写表头（2455-2526）。调用点：`src/raytracing/raytracing.cpp:763,866`、`src/voxelrt/voxelrt.cpp:599,674`、`src/voxeleb/voxeleb.cpp:751`。`writeENVIdata` 目前没有调用点，且 `t>=0` 分支的文件名不含序号，同一时刻多个位置会互相覆盖（`src/base/fileio.cpp:2570` 的 DEFERRED 标注）。
4. 原 `readXml` 在加载失败时直接 `exit(1)`（`src/base/fileio.cpp:125`），跳过析构。

#### 4.4.4 structs.h 与 structs_cg.h：配置结构与 GPU 共享布局

`structs.h` 是纯 CPU 侧。三个工程根结构（`src/base/structs.h:664,712,745`）：

| 结构 | 主要成员 | 备注 |
| --- | --- | --- |
| `RaytracingXml` | `settingxml/lightxml/sensorxml/atmospherexml/scenexml/spectralxmls/thermalxmls` | 无冠层、材质、气象 |
| `VoxelRTXml` | 上述加 `propxmls/canopyxmls/meteoxml/aerocondxml/atomcondxml` | `readJson` 只填前半部分，气象与气动字段不填（`fileio_json.cpp:869-870`） |
| `VoxelEBXml` | 在 VoxelRT 基础上加 `fluidxml` | 气象、流体、气动由 `readJson` 填写（871-919） |

其他要点：全局宏 `PI` 与 `DEG2RAD` 只有约 7 位有效数字（`structs.h:48,62`）；`N1=2001`（短波点数）与 `N2=161`（热红外点数）固定了光谱表长度（67-68）；`TLASTNUM=8`、`SOIL_MEMORY_MODES=12`、`DIFFUSENUM=64` 必须与着色器一致（53-64）。枚举 `Mode` 的取值顺序为 eRaytracing、eVoxelEB、eVoxelRT、eFacetRT、eFacetEB（129-135）。许多字段（如 `FluspectParam`、`Shape`、`MeteoMeta` 的部分成员）没有默认值，依赖解析代码赋值，源码以 DEFERRED 标注。

`structs_cg.h` 的结构以缓冲或 push constant 形式上传，布局由 `static_assert` 与着色器对应：

| C++ 结构 | GLSL 对应 | 保护 |
| --- | --- | --- |
| `RayRTSetting`（80 字节） | `settings.h` 的 `RayRTSetting` | `offsetof(sceneSize)==64`、`sceneTop==72`、`sizeof==80`（`structs_cg.h:114-116`） |
| `VoxelLstSetting`（128 字节） | `VoxellstSetting` | 7 个 offsetof 加 `sizeof==128`（162-170） |
| `VoxelRTSetting` | `VoxelrtSetting` | 只断言 `<=128`（240）；字段增删无法被编译期发现 |
| `VoxelLink`（48 字节）、`VoxelHex`（160 字节）、`VoxelTempe`（12 字节）、`VoxelAir`（36 字节） | `parameters.h` 同名 | `sizeof` 断言（394-395,424,472） |
| `EBState`、`FluidParameters`（128 字节） | 同名 | 断言（517,544-546） |
| `Canopy`、`SoilSet`、`LeafBio`、`WaterSet`、`Meteo`、`SensorMatrix`、`LightSet`、`MeshLink` | 同名 | 无任何断言，须人工对照 `shader/parameters.h` |

注意命名差异：C++ 的 `VoxelLstSetting`、`VoxelRTSetting` 在 GLSL 中为 `VoxellstSetting`、`VoxelrtSetting`；C++ 的 `VoxelHeatflux` 在 GLSL 中为 `VoxelHeatFlux`；GLSL 的 `VoxelLink` 把法线拆成 `normalX/Y/Z` 三个 float，与 C++ 的 `glm::vec3 normal` 字节等价（`shader/parameters.h:217-230`）。`MeshBuffer` 增加了 `vertexStride`（缺省 44）与 `indexed` 两个字段，供位置专用代理网格使用（`structs_cg.h:329-341`）。

#### 4.4.5 IO 容器类（meshio、instanceio、voxelio、surfio、virtualio、defined）

这些类只持有数据，没有逻辑，成员全部公开。三个后端 IO 通过 `shared_ptr` 持有它们：`RaytracingIO` 持有 `m_meshio/m_instanceio/m_virtualio/m_defined`；`VoxelrtIO` 与 `VoxelebIO` 另持有 `m_voxelio/m_surfio`（`src/voxeleb/voxelebio.h:192-197`、`src/voxelrt/voxelrtio.h:155-160`）。`MeshIO` 的 `*Names` 映射（光谱、热、冠层、叶、土、水）由 `Compo` 填，`Scene` 按名称解析为 `MeshLink` 的各类 id。不同模式只使用部分缓冲指针，其余为空，销毁前须判空（`src/base/meshio.h` 头注释）。

#### 4.4.6 Compo：材质光谱与参数

接口见 `src/base/compo.h:28-54`。调用链：

```
Raytracing::upload   → Compo::createCompOptical(RaytracingIO)       (raytracing.cpp:323)
Voxelrt::upload      → Compo::createCompOptical(VoxelrtIO)          (voxelrt.cpp:161)
Voxeleb::upload      → Compo::createCompProperty(VoxelebIO)         (voxeleb.cpp:157)
  └─ resolveSpectrals(material, outputWavelengths, customWavelengths, coefficients)
       ├─ PROSPECT → fluspect(...) ；BSM → bsm(...)；OTHER → readSpectrumFile；CUSTOM → 面板数值
       └─ 对每个目标波长：> 2.5 µm 取平坦热红外值（BSM 经 soiloptics::tirReflectance），否则线性插值并限幅
```

约定与易错点：

1. 光谱数组按“材质主序、波长次序”扁平存放，每个材质占 `sensorxml.waves.size()` 个元素（`src/base/compo.cpp:19-20,198-201`）。VoxelEB 另存 `fixedSpectrals`，波长为 400 到 2400 nm 逐 1 nm 加 10500 nm（`src/base/compo.cpp:522-536`）。
2. 缺省反射率 0.2、透射率 0；透射率限制不超过 `1-反射率`（`src/base/compo.cpp:479-483`）。波长单位判断：大于 50 视为 nm，否则为 µm（92-101）。
3. 导入光谱文本 `readSpectrumFile`：`#` 为注释，第 1 行波长，第 2 行反射率，可选第 3 行透射率，各行等长至少 2 点，波长须唯一（119-150）。
4. `withLeafAngles` 由 `LIDFa/LIDFb` 填 13 个 SAIL 倾角频率 `lidf` 与 32 个 `diffuseG`；`LIDFa=LIDFb=0`、`LIDFa>1`、刚体或介质时保持常数 G（59-83）。
5. VoxelEB 的 `createCompProperty` 在各类参数为空时追加占位项（默认热属性 300 K、`__default_canopy`、`__default_leaf`、`__default_soil`、水体名 `water`），因为描述符布局固定，不能有空缓冲（513-590）。但冠层与三类参数集合在追加前未清空，重复调用会产生重复条目（539 行 DEFERRED）。
6. 头文件注释与实现不完全一致：`compo.h` 称 `bsm` 输出“追加”、`createCompProperty` 成功返回 false；实际 `bsm` 先 `clear()`（`compo.cpp:605`），`createCompProperty` 返回 true（593）。以实现为准。

#### 4.4.7 Scene：场景构建

`Scene` 无成员变量，所有结果写入传入的 IO 对象（`src/base/scene.h:38-71`）。

```
Scene::createObjScene(fileio, RaytracingIO)                    scene.cpp:1990
  背景（DEM → ObjLoader::creatBackgroundFromDEM；物理纹理 → createGridBackground；否则 createBackground）
  → 逐 OBJ 实体：读放置点（位置文件或场景中心）→ loadMesh → MeshLink → Instance/InstanceLink

Scene::createPrimObjScene(fileio, VoxelebIO 或 VoxelrtIO)      scene.cpp:2257 / 3100
  createPrimObj_Background        必须最先调用，写入场景尺寸与体元步长（2923）
  逐 PrimEntity：
    voxelizeFromObj → createObjFilledVoxels（模板，1359）
    VEGETATION → createPrimObj_Crown（单形状）或 createPrimObj_Crowns（形状文件）
    BUILDING   → createPrimObj_Building（墙与屋顶两个网格）
    WATER      → createPrimObj_Crown
  异质体元模式：finalizeHexMixtures（912）
  nanoHandle = nanoBuilder.getHandle()
```

VoxelEB 与 VoxelRT 两套 `createPrimObj_*` 逻辑相同，仅 IO 类型不同，因此改一处通常要同步改另一处。

要点：

1. 坐标约定：体元、OBJ、Three.js 世界坐标为 +X 北、+Y 上、+Z 东；位置文件与配置中的 (x, y, z) 为（北、东、高）；基元网格由 `VoxelDesigner` 以 Z 向上设计，经 `XYZ2XZY` 交换 y 与 z（`src/base/scene.cpp:3896-3940`）。
2. 名称解析辅助：`requiredId`（缺失抛异常，用于光谱）、`mappedId`（可选绑定，名称列表比实例短时沿用最后一项）、`idOrZero/idOrZeroWarn`（`src/base/scene.cpp:632-700`）。背景的热属性名找不到时静默取 0 号，与实体路径不一致（2057、2985 行 DEFERRED 标注）。
3. `createPrimObj_Background` 要求 `stepsize_surface` 为有限正数（2936），体元数用整数截断，高度方向体元数置 0 由实体决定（2939-2944）。
4. OBJ 体元化（`createObjFilledVoxels`）：先 `voxelizeObjSurface` 按三角形与体元的裁剪面积判定占据，建筑法线朝外由 `SolidClosureIndex` 判定；异质体元模式下调用 `hexvoxel::compute` 取得每体元聚集指数与叶面积密度，旋转实例时按 90 度整数倍旋转贡献（808-866），文件头注释列出各步骤。
5. `finalizeHexMixtures`（912-1347）：体元按坐标排序保证可复现；可选读 `HISTREAM_HEX_LOD_TABLE`（文件格式 `HEX_LOD_V1`：头含魔数、父体元边长、层数 1 到 6、根体元数；每层细分数从 1 开始逐层翻倍且不超过 32；每个根体元给坐标与 Σn³ 个单元 `{ax,ay,az,rho}`；总条目须少于 1600 万，951-1003）；多类别体元取贡献最大的两类，权重按 1/1024 量化并缓存混合材质。体元边长必须各向同性。
6. DEM 背景走 `createVoxelDemBackground`（阶梯状柱体顶面，196 行），与 RayTracing 的连续地形网格不同。

#### 4.4.8 ObjLoader、VoxelDesigner、hexvoxel

| 类 | 作用 | 要点 |
| --- | --- | --- |
| `ObjLoader`（`objloader.h:51-104`） | 结果放在公开成员 `m_objmesh` | `loadModel` 读全部 shape，`loadMesh(file, name, exactMatch)` 按名称读；`creatBackgroundFromDEM` 经 GDAL 读 DEM 并保存 `m_heightmap`，之后 `getHeightAt/interpolateZValues` 把离地高度换算为 y；顶点索引越界抛异常（tinyobj 仅告警，`objloader.cpp:36-45`） |
| `VoxelDesigner`（`voxeldesigner.h:48-111`） | 形状、背景、高度栅格转 `PrimMesh` | 体元 id 为最小角坐标，AABB 为 `[id, id+1]`，网格顶点单位为体元而非米；`createTriVoxels` 对介质生成三张中心正交面，对实体生成外表面（`voxeldesigner.cpp:6-9`）；`minElevation` 与 `m_scale` 在现役代码中从不赋值（`voxeldesigner.h:77-104`） |
| `hexvoxel`（`hexvoxel.h`） | 面元投影求 `CI=-ln(1-C)/P` 与 `rho` | 自包含，仅依赖标准库；`compute` 的稠密网格上限 2500 万格，超限抛异常并建议增大体元（`hexvoxel.cpp:880-886`）；用 OpenMP，故 `hexvoxel.cpp` 在 CMake 中单独加 OpenMP 选项（`CMakeLists.txt:406`） |

`voxeldesigner.cpp:24` 直接 `#include <nvh/gltfscene.cpp>`（包含 .cpp 文件），会把该实现编入本翻译单元，链接时要避免再次编译同一文件。

#### 4.4.9 Geometry：相机、光源与角度

接口见 `src/base/geometry.h:33-97`，三个后端各一套同名重载。

```
createGeometry(fileio, modelio)
  ├─ configureSensor(sensorxml, sceneSize, metresPerUnit)    换算到场景单位，缓存投影、位置、视场
  ├─ 角度表：每个 viewAngle 配同一组太阳角 → modelio->angles
  └─ createConfiguredSensor(...) + createLight(...)          初始传感器与光源（取第 0 个角度）
updateAngle(modelio, k)    重建传感器与光源并同步写 GPU 缓冲
updateSensorPos(modelio,k) UAV 逐位姿：透视传感器；可覆盖第 0 个角度的 vaa
orthcorrect(...)           求正射输出的像素到场景坐标双线性系数 cx、cy
```

要点：

1. `createConfiguredSensor` 三条分支（`src/base/geometry.cpp:349-405`）：环境变量 `HISTREAM_SENSOR_CAMERA` 指向 JSON 时直接使用其中 `viewInverse/projInverse`（各 16 个列主序数），忽略工程角度；透视投影；平行（正射）投影，平行传感器视场固定 `SENSOR_FOV=0.5` 并将相机拟合到场景范围。`HISTREAM_HEX_CONE=lod` 把 `aperture` 置为 -4 作为着色器标记，其他取值抛异常。
2. `createLight` 在 `sza` 恰为 0 或 45 度、`saa` 贴近 45 度倍数时做 `ANGLE_COR=0.1` 度偏移，避免与体元面对齐的退化方向（`geometry.cpp:165-176`）；这会静默改变用户给定的太阳角。方位角自北顺时针，+X 北、+Z 东（183-185）。
3. VoxelEB 的 `updateAngle` 先调用 `updateSolarAngle`，用 SPA 算法由气象时间、经纬度、年份、时区重算 `sza/saa`，并覆盖角度表中的值（`geometry.cpp:714-724,781-824`）；观测点海拔硬编码 100 m，天顶角大于 90 度时没有处理（812 行 DEFERRED 标注）。
4. 三个后端的 `updateSensor/updateLight` 用 `vkCmdUpdateBuffer` 同步写入；RayTracing 与 VoxelEB 加 transfer 到 compute 屏障，VoxelRT 版本不加（`geometry.cpp:534-561,1021-1049`）。
5. 正射校正基于 `solveNadirMapping`，缩放系数固定 1.0（`geometry.cpp:37,110-155`）。

#### 4.4.10 AccelStruct 与 VirtualScreen

`AccelStruct::createAccelStruct(device, meshio, instanceio)`（`src/base/accelstruct.cpp:94-181`）：每个网格一个 BLAS（下标即 meshId，快速追踪加允许压缩），每个实例一个 TLAS 条目，`instanceCustomIndex` 为实例在 `instanceio->instances` 中的下标，所有实例强制不透明、mask 为 0xFF、共用同一 hit group（161-173）。`meshId==-1` 的实例仅携带体元链接，不入 TLAS，但下标仍被占用（157）；其他越界 meshId 抛异常。设置 `m_physicalDevice` 后会在构建前逐 BLAS 查询大小并调用 `voxellimits::requireAllocation`（体元模式使用），`HISTREAM_VOXEL_PROXY_STATS=1` 打印尺寸。局部 `Vertex` 结构固定 44 字节步长参与计算，源码已标注与上传端结构须一致的风险（35 行 DEFERRED）。

`VirtualScreen::bufferToBuffer`（`src/base/virtualscreen.cpp:26-78`）：容量检查、源缓冲屏障（shader 或 transfer 写到 transfer 读）、拷贝、目标缓冲屏障（transfer 写到 host 读）、在生产者队列提交并等待。size 超过任一缓冲内存需求时返回 false 且不提交命令；调用方须检查返回值。

#### 4.4.11 头文件级物理与限制模块

1. `voxelflags.h`（`src/base/voxelflags.h:28-101`）：`voxelFlags` 位定义 `kDirRot=1`、`kDirRotAll=2`、`kHdda=4`、`kValid=1<<20`，代理网格模式占第 12 到 13 位，旋转块边长以 log2 存放在第 8 位起；`resolve()` 优先级为环境变量，其次工程字段，最后引擎缺省（`kDefaultDirectionRotation=0`、`kDefaultEmptySkip=true`、`kDefaultProxyGeometry=positionNoIndex`）。位定义必须与 `shader/voxel_flags.glsl:18-25` 一致；着色器在没有 `VALID` 位时按旧行为处理（`voxel_flags.glsl:28-37`）。
2. `voxellimits.h`：`requireStorageRange`（单个存储缓冲不超过 `maxStorageBufferRange`）、`requireIndex32`（32 位下标不溢出）、`requireDispatch`（一维派发组数不超过 `maxComputeWorkGroupCount[0]`）、`requireAllocation`（单次分配不超过 `maxMemoryAllocationSize`）。过去超限是静默的，现统一抛带体元数与上限的错误。
3. `proxymesh.h`：`full` 为 44 字节顶点，`position` 为 12 字节位置顶点加索引，`positionNoIndex` 为无索引三角形列表；后两种经固定 48 MiB 的 host-visible staging 分块上传，不再需要与缓冲等大的 staging。
4. `shortwave_split.h`：`parseMode("fixed"或"erbs")`；`split` 在 fixed 模式用配置的直射比例，erbs 模式优先用气象文件第 8、9 列实测值，否则用 Erbs 1982 晴空指数公式；太阳高度低于 1 度或 `Rin<=0` 时全部视为散射（头注释）。
5. `soil_tir_emissivity.h`：湿度权重 `1-exp(-mu)`，`mu=max(SMC-0.05,0)/0.25`，发射率增量上限 0.03、发射率上限 0.995；同时兼容 SMC 的百分数与体积分数写法。`models/stream/physics/soil_emissivity.h` 保留等价拷贝，修改时需同步（头注释）。
6. `atmosphere_lut.h`：LUT 有效范围 350 到 14000 nm，传感器高度 LUT 内为 km，传入 m 时内部除以 1000；LUT 无法打开或为空时返回“无校正”（T=1、程辐射 0）。LUT 文本 v2 表头 `atmosphere_model`，列为 `model aerosol waterVapor visibility altitude zenith wavelength transmittance pathRadiance`，v1 无该表头且省略前三列（头注释）。
7. `radiation_spectrum.h`：非法样本（非有限、为负、比例超出 [0,1]、文件缺失或样本不足）一律抛异常，不做静默修正。
8. `terrain_environment.h`：`validate` 检查有限值、标高为正、`0<=soilDry<=soilValley<=1`；高度基准是场景最低点（DEM 最小值），不是海拔；仅 eVoxelEB 支持（`terrain_environment.h:39-49`）。

### 4.5 公共构建：CMakeLists.txt 与 cmake/

`CMakeLists.txt`（551 行）定义主目标 `histream` 与若干辅助目标。

| 内容 | 位置 | 说明 |
| --- | --- | --- |
| 主目标 | `CMakeLists.txt:22-24` | C++17，入口 `src/main.cpp`；MSVC 加 `/utf-8` |
| nvpro_core | `CMakeLists.txt:42-89` | 在上一级目录查找 `nvpro_core/cmake/setup.cmake`；加载 VulkanSDK、ImGUI、NVML 包；定义 `USE_NEW_NVPRO_CORE`；内存分配用 `ALLOC_DMA`（96） |
| 源文件 | `CMakeLists.txt:113-129` | `file(GLOB ...)` 取 `src/*.cpp`、`src/base`、`src/thirdparty`、`src/raytracing`、`src/voxeleb`、`src/voxelrt` |
| Facet 开关 | `CMakeLists.txt:13-16,131-151` | 选项 `HISTREAM_ENABLE_FACET`，缺省 ON；OFF 时仍编入 `facetrt_vulkan.cpp`，因为 `appsetting.cpp` 依赖 `rankDevices`；同时向目标定义宏 `HISTREAM_ENABLE_FACET=0/1`，与 `engine.h:32-34` 对应 |
| 着色器编译 | `CMakeLists.txt:171-365` | 用 `glslc --target-env=vulkan1.2` 把 `shader/raytracing`（4 个）、`shader/facetrt` 与 `shader/faceteb`、`shader/voxeleb/*.comp`、`shader/voxelrt/*.comp` 编为 `.spv` 到构建目录；任何共享 `.glsl/.h` 变化触发全部重编（178-180） |
| VoxelEB 变体 | `CMakeLists.txt:290-333` | `aeresist`、`budget`、`evapo` 另编一份 `*_most.comp.spv`（`-DVOXELEB_MOST=1`，对应稳定度修正）；`voxelrad_scattering_VNIR.comp` 另编 `voxelrad_scattering_trace.comp.spv`（`-DSCATTER_TRACE_STAGE=1`） |
| 链接库 | `CMakeLists.txt:390-408` | nvpro_core、GDAL、Eigen3、OpenCV、OpenMP；OpenMP 仅对 `hexvoxel.cpp` 与 `facetrtcore.cpp` 通过 `COMPILE_OPTIONS` 启用 |
| OpenUSD | `CMakeLists.txt:410-428` | 找到 `pxr` 才生成 `histream-usd`；选项 `HISTREAM_REQUIRE_USD` 使缺失时配置失败 |
| 扩展 | `CMakeLists.txt:451-476` | 见 4.2.3 |
| 运行时部署 | `CMakeLists.txt:499-529` | 目标 `histream_runtime_resources`：删除废弃的 `fluid_*` 旧着色器，复制 `shader/` 源、`defined/`、各 `.spv` 目录、vcpkg 运行库（`../cmake/copy-runtime-dependencies.cmake` 排除 pdb、lib 等）与 `thirdparty/lavapipe/` 到可执行文件旁 |
| 测试 | `CMakeLists.txt:534-551` | 4 个 `EXCLUDE_FROM_ALL` 可执行文件，源码在仓库根 `tools/test-*.cpp`：`histream_lumped_thermal_tests`、`histream_lidf_gamma_tests`、`histream_diffuse_directions_tests`、`histream_voxel_limits_tests`（后者需 Vulkan 读取设备限制） |

构建的几点事实：

1. 源文件 glob 没有 `CONFIGURE_DEPENDS`（`CMakeLists.txt:113`），新增 `.cpp` 后须重新运行 CMake 配置；着色器 glob 则带有 `CONFIGURE_DEPENDS`（178、260、339）。
2. `src/cases/` 与 `src/usdtool/` 不在源文件 glob 内；`src/thirdparty/nanoutil/` 为纯头文件。
3. `cmake/copy-core-shaders.cmake` 对 `shader/` 做递归 `configure_file COPYONLY`，不会删除已部署文件（`cmake/copy-core-shaders.cmake:1-7`）；据代码推得它也会把 `shader/.idea` 等非着色器文件一并复制。
4. CMake 引用 `shader/raytracing/` 小写目录，而磁盘目录为 `shader/RayTracing/`，`shade_state.glsl` 以 `bindinglayout.h` 小写包含 `bindingLayout.h`（`shader/shade_state.glsl:21`）；Windows 大小写不敏感可通过，非 Windows 构建会失败，是否有 Linux 构建需求待核实。
5. nlohmann/json 头文件被 `projectjson.h` 包含，但 `CMakeLists.txt` 没有对应 `find_package`，其来源（vcpkg 全局包含路径或 nvpro_core）待核实。
6. 主构建的 vcpkg 路径硬编码 `x64-windows`（`CMakeLists.txt:399,520`），构建平台实际仅针对 Windows。

### 4.6 shader 公共头与 CPU/GPU 对应

`shader/` 顶层的公共文件（不含各后端子目录）：

| 文件 | 行数(约) | 职责 | 关键内容 |
| --- | --- | --- | --- |
| `settings.h` | 165 | push constant、光线负载、着色状态 | `RayRTSetting`、`VoxellstSetting`、`VoxelrtSetting`、`PtPayload`、`ShadowHitPayload`、`ShadeState` |
| `bindingLayout.h` | 259 | 各管线描述符绑定号与 buffer 声明 | 宏 `RAYTRACING`、`VOXELLST`、`VOXELRT` 各一段 |
| `parameters.h` | 503 | GPU 侧材料、冠层、体元状态结构体与介质小函数 | `SpectralMaterial`、`Canopy`、`VoxelLink`、`VoxelHex`、`SoilSet`、`EBState` |
| `global.glsl` | 283 | 全局常量、物理常数、64 方向漫射求积方向表 | `TLASTNUM`、`DIFFUSE_DIRECTIONS[64]`、`compensatedAdd` |
| `functions.glsl` | 1937 | 共用函数库：冠层投影 G、体元寻址、湍流介质光学厚度、周期边界、散射相函数、土壤热传导 | `CanopyProjection`、`ResolveSpatialInteraction`、`Soilheatflux` |
| `lidf_common.h` | 270 | 叶倾角分布与 Gamma，C++ 与 GLSL 共用同一份源 | `lidfProjection`、`sailLeafAngleFrequencies`（仅 C++） |
| `periodic_walk.glsl` | 156 | 周期边界逐副本 DDA 步进 | `PeriodicWalk` |
| `voxel_flags.glsl` / `voxel_dirrot.glsl` / `voxel_radindex.glsl` | 39 / 76 / 18 | 体元选项位、按体元哈希旋转漫射方向、VoxelEB 辐射缓冲 64 位下标 | `VoxelFlag`、`VoxelProxyMode` |
| `shade_state.glsl` / `shade_state_voxel.glsl` | 94 / 176 | 把命中负载还原成 `ShadeState`（面元与体元两种） | 前者依赖 `bindinglayout.h` |
| `traceray_comp.glsl` | 312 | 计算着色器中 `rayQuery` 封装 | `ClosestHit`、`AnyHit` |
| `compress.glsl`、`random.glsl`、`sampling.glsl`、`bsdf.glsl` | 221、136、295、106 | 八面体压缩、随机数、采样、朗伯 BSDF | `compress_unit_vec`、`tea/pcg` |
| `surface_brdf.glsl`、`background_hapke.glsl`、`water_brdf.glsl`、`specular_brdf.glsl` | 100、150、72、37 | 地表 Hapke、水面 Cox-Munk、镜面 | `surfaceReflectanceWeight` |
| `mwir_solar.glsl`、`solar_steps.glsl`、`nanovdb_info.glsl`、`PNanoVDB.h` | 131、55、78、2503 | 中波红外太阳源项、太阳可见度步数预算、NanoVDB 读取封装 | `thermalSolarGlobal`、`SolarStepLimit` |
| `shadersCompile.py` | 57 | 开发期手工编译脚本，不参与正式构建，面向 Windows | 待核实是否仍被使用 |

#### 4.6.1 绑定布局速查

`bindingLayout.h` 通过宏选择管线，编号只在所属段内有意义，同名宏编号可不同（文件头注释）。

| 段 | 编号范围 | 要点 |
| --- | --- | --- |
| `RAYTRACING`（33-69） | B_SPECTRAL 0 到 B_TEXFACTOR 10 | 光谱、热、网格链接、实例链接、TLAS、传感器、光源、波长、输出图像、纹理偏移与因子；push constant 为 `RayRTSetting` |
| `VOXELLST`（77-192） | 0 到 41 | 体元温度、冠层、体元链接、NanoVDB、辐射、气象、气动、土壤、水体、通量、Tlast、EBState、LAD、流体场、`B_HEX=40`、`B_SOILMEMORY=41`；push constant 为 `VoxellstSetting` |
| `VOXELRT`（199-255） | 0 到 17 | 无气象与流体；`voxelRads` 为普通 SSBO；push constant 为 `VoxelrtSetting` |

特别注意：VOXELLST 段的 `B_RADS=15` 只是占位编号，没有对应缓冲声明；`voxelRads` 通过 push constant 的 `radianceAddress` 物理地址访问，以突破 4 GiB 限制（`shader/bindingLayout.h:143-150`）。

#### 4.6.2 CPU 与 GPU 结构对应

CPU 侧定义在 `src/base/structs_cg.h`，GPU 侧在 `shader/parameters.h` 与 `shader/settings.h`，须逐字段一致。新增或删除字段时的同步清单：

1. 在 `structs_cg.h` 与 `parameters.h`（或 `settings.h`）同时修改，顺序与类型一一对应。
2. 对带 `static_assert` 的结构更新断言的偏移与大小；对没有断言的结构（见 4.4.4 表）建议补充断言。
3. 对 push constant 结构，核对总大小不超过 128 字节。
4. `Canopy` 的 `lidf[13]`、`diffuseG[32]` 与 `DIFFUSE_DIRECTIONS` 的排列绑定：相邻两个方向互为反向且 `|y|=(2k+1)/64`（`shader/global.glsl:96-105`）。
5. 常量三处同步：`TLASTNUM`（`structs.h:53`、`global.glsl:89`）、`SOIL_MEMORY_MODES`（`structs.h:56`、`global.glsl:92`）、`DIFFUSENUM` 与 `DIFFUSE_ANGLE_NUM`（均为 64）。`BANDNUM=2162` 等于 `N1+N2`，`NBAND=2002` 为 2001 个短波点加 1 个热红外点（`global.glsl:94,127`）。
6. 修改共享头后，CMake 会重编所有 `.spv`（`CMakeLists.txt:178-180`），但已部署的旧 `.spv` 与新 C++ 混用会布局错位，部署时须整体更新 `shader/*`。

### 4.7 thirdparty 与 usdtool

| 位置 | 内容 | 说明 |
| --- | --- | --- |
| `src/thirdparty/tinyxml.*`、`tinystr.*`、`tinyxmlerror.cpp`、`tinyxmlparser.cpp` | TinyXML | 仅旧 XML 路径使用；4 个 `.cpp` 被 glob 编入 |
| `src/thirdparty/spa.h` / `spa.cpp` | NREL 太阳位置算法 `SPACalc`、`spa_data` | `Geometry::updateSolarAngle` 使用 |
| `src/thirdparty/NanoVDB.h`、`CNanoVDB.h`、`nanoutil/` | NanoVDB 与 GridBuilder 等 | `structs.h:41-43` 包含；`scene.cpp` 用 `GridBuilder<int32_t>(-1)` 建体元索引网格 |
| `src/thirdparty/magic_enum.hpp`、`tiny_obj_loader.h` | 枚举反射、OBJ 解析 | `objloader.cpp` 定义 `TINYOBJLOADER_IMPLEMENTATION` |
| `thirdparty/lavapipe/` | Mesa lavapipe 26.2.4：`vulkan_lvp.dll`、`lvp_icd.x86_64.json`、`README.md` | 构建时复制到可执行文件旁 `lavapipe/`，CPU 或 Auto 后端使用 |
| `src/usdtool/histream_usd.cpp`（700 行） | 独立命令行 `histream-usd read <scene> <输出目录>` | 可选目标，依赖 OpenUSD；输出 OBJ 与 `usd-import.json`；成功时标准输出一行 `HISTREAM_USD<TAB>清单路径`；退出码 0 成功、2 参数错误、1 异常；由 `server.mjs:2787-2788` 调用；Z-up 舞台按 `(x,y,z)→(x,z,-y)` 转换，不处理动画，只取最早时间码（文件头注释） |

### 4.8 环境变量与文件格式速查

| 变量 | 作用 | 位置 |
| --- | --- | --- |
| `HISTREAM_COMPUTE_BACKEND` | 覆盖工程的 `computeBackend` | `src/base/engine.cpp:95` |
| `HISTREAM_CPU_THREADS`、`LP_NUM_THREADS` | 由 `Engine::input` 写入，供后端与 lavapipe 使用 | `src/base/engine.cpp:159-164` |
| `HISTREAM_VOXEL_DIRROT`、`_DIRROT_BLOCK`、`_SKIP`、`_PROXY` | 覆盖体元选项 | `src/base/voxelflags.h:84-98` |
| `HISTREAM_VOXEL_PROXY_STATS`、`_MERGE_STATS`、`_PASS_TIMING`、`_TEST_RADIANCE_VOXELS` | 诊断输出与测试 | `accelstruct.cpp:115`、`voxelmerge_stats.h:62`、`voxelflags.h:112-123` |
| `HISTREAM_SENSOR_CAMERA`、`HISTREAM_HEX_CONE`、`HISTREAM_HEX_LOD_TABLE` | 外部相机矩阵、HEX 锥追踪、HEX LOD 表 | `src/base/geometry.cpp:357,386`、`src/base/scene.cpp:951,1005` |
| `HEXDBG_BASE/ROW/CELL` | `hexvoxel` 调试输出 | `src/base/hexvoxel.cpp:677,961,971` |
| `HISTREAM_RT_SEED`、`HISTREAM_RT_SAMPLER`、`HISTREAM_RT_ADAPTIVE` | RayTracing 采样覆盖，在后端读取，不在 base | 见 `fileio_json.cpp:271-272` 注释，实现待核实 |

文件格式：`project.json`（`format=histream-project`，`schemaVersion` 1 到 6，主体在 `configuration`）；气象文本（4.4.3）；`Esun_.dat`、`Esky_.dat`（每行一个样本，需 2162 个）；位置文件每行 `x y z [scale] [rotation]`（`Utils::readPositionFile`）；导入光谱文本（4.4.6）；`HEX_LOD_V1`（4.4.7）；大气 LUT（4.4.11）。

### 4.9 修改指南与注意事项

1. 新增命令行模式：写树外扩展，提供 `histream-extension.cmake` 与注册函数，通过 `-DHISTREAM_EXTENSION_DIRS` 传入（`CMakeLists.txt:451-476`）；不要改 `main.cpp` 的内置五种模式白名单（`src/main.cpp:130-131`）。若需新增内置模式，还要同步 `engine.cpp` 的字符串解析、`structs.h` 的 `Mode` 枚举、`server.mjs` 的白名单。
2. 新增或修改 `control` 字段：改 `fileio_json.cpp` 的 `configureCommon`（缺省值与限幅）、`structs.h` 的 `SettingXml`，再由后端 `uploadSetting` 复制到 IO；同时改前端 `project-schema.js` 的缺省与校验。工程格式不兼容时升 `kLatestProjectSchemaVersion`（`projectjson.cpp:41`）并与前端 `PROJECT_SCHEMA_VERSION` 同步。
3. 修改传感器波段规则：必须同时改 `projectjson.cpp:sensorBands` 与前端 `sensorBandValues`，否则服务端解析 Facet 结果的偏移错位。
4. 增删 GPU 结构字段：同步 `structs_cg.h` 与 `shader/parameters.h` 或 `settings.h`，更新 `static_assert`，必要时补上缺失的断言（`VoxelRTSetting`、`Canopy` 等）；部署时整体更新 `.spv`。
5. 新增描述符绑定：在 `bindingLayout.h` 对应管线段加 `B_*` 宏与声明，宿主端 `descriptor.cpp` 使用同一编号；VOXELLST 段新增编号接在 41 之后，并注意 `B_RADS` 是占位。
6. 新增着色器文件：放入 `shader/<mode>/`，`voxeleb` 与 `voxelrt` 的 `*.comp` 自动被 glob 编译；`raytracing` 与 `facet*` 的文件名在 `CMakeLists.txt:182-186,216-226` 显式列出，须手工添加。需要 MOST 变体时修改 `CMakeLists.txt:290` 的正则。
7. 新增 base 源文件：放入 `src/base/` 后重新配置 CMake（glob 无 `CONFIGURE_DEPENDS`）；若依赖新的第三方库，在 `CMakeLists.txt:390-408` 附近 `target_link_libraries`。
8. 新增材质类型或光谱来源：改 `structs.h` 的 `spectralType`、`fileio_json.cpp` 的光谱解析（653 行附近）、`compo.cpp:resolveSpectrals` 的分支，并保证热红外分支（`> 2.5 µm`）有合理处理。
9. 新增场景实体种类：在 `scene.cpp` 的 VoxelEB 与 VoxelRT 两套 `createPrimObj_*` 中同步实现；`createPrimObj_Background` 必须仍是第一个被调用的函数（它写入全局体元步长）；RayTracing 需另外在 `createObjScene` 中实现。
10. 改路径处理：当前路径全链路是窄字符串，`fileio_json.cpp`、`engine.cpp`、`scene.cpp`、`main.cpp` 的 DEFERRED 标注均指向同一问题；要支持非 ASCII 路径，须统一改为 `std::filesystem::path` 与宽字符 API，不能只改单点。已有点位不一致：`resolve` 用 `u8path`，`executableDirectory` 一处用 A 版 API，一处用 W 版 API。
11. 修改计算后端或设备选择：`appsetting.cpp:initVulkan` 的扩展清单与 `describeDevices` 的 JSON 字段是前端设备列表的数据源；新增字段需同步 `server.mjs` 解析。`voxelIndex` 与 `facetIndex` 含义不同，不可混用。
12. 修改规模上限：集中在 `voxellimits.h`，调用点在后端 `buffer.cpp` 与 `command.cpp`；新增大缓冲时务必调用对应 `require*`，否则超限仍是静默错误。输出影像缓冲必须经 `checkedOutputImageElements`。
13. 修改输出命名：`writeTIFData` 的文件名规则被服务端解析和后处理使用（`src/base/fileio.cpp:2344-2380`），改名前须检查 `server.mjs` 对 `T=DOY…`、`SZA=…` 的匹配；统计 CSV 列定义在 2472 行。
14. 改异质体元：`hexvoxel.*` 保持自包含（不引入 Vulkan 与 glm），否则无法被独立工具复用；`scene.cpp` 中的 `HexMixAccumulator` 与 `finalizeHexMixtures` 同时服务 VoxelRT 与 VoxelEB，改动要两个模式都回归。
15. 易错点汇总：头文件注释可能落后于实现（`compo.h`）；`gridio` 与 `pixelio` 为空壳；`writeENVIdata` 与旧 XML 解析无调用点，改它们不会影响引擎行为；`src/main.cpp` 在 `.cpp` 中带有 `#pragma once`，无害；`hexvoxel.h` 提到的 `src/tools/hexvoxel` 命令行目录在 `src` 下不存在，去向待核实；`src/cases/` 的用途待核实。

## 5. HiStream 成像与体元模块：RayTracing / VoxelRT / VoxelEB

本章说明 HiStream 可执行程序中三个基于 Vulkan 的求解后端：RayTracing（光线追踪成像）、VoxelRT（体元辐射传输成像）、VoxelEB（体元能量平衡加成像）。三者的源码目录分别为 `src/raytracing`、`src/voxelrt`、`src/voxeleb`，着色器目录分别为 `shader/RayTracing`、`shader/voxelrt`、`shader/voxeleb`。运行流程（时间节点循环、迭代收敛判据、物理含义）见《运行原理说明》，物理方程见《理论与方法》，本章只给出文件分工、接口、数据布局与修改点。

引用约定：括号内路径为仓库相对路径，省略前缀 `models/histream/`，例如 `src/raytracing/pipeline.cpp:134` 即 `models/histream/src/raytracing/pipeline.cpp:134`；`shader/...` 同理。表格中的行数为物理行数（含注释与空行）的约数。三者的基础设施（Scene、Geometry、Compo、FileIO、AccelStruct、VirtualScreen 等）属于 `src/base`，不在本章展开。

### 5.1 总览与依赖方向

#### 5.1.1 三个模块的共同骨架

三个目录采用同一套分层：一个主控类（驱动 setup、upload、create、run、destroy 五步）、一个 IO 类（纯数据容器，成员全部 public，没有业务逻辑）和四个几乎无状态的阶段类 Buffer、Descriptor、Pipeline、Command。阶段类只通过 `std::shared_ptr<XxxIO>&` 读写 IO 中的 Vulkan 句柄，彼此不直接调用。

| 项目 | RayTracing | VoxelRT | VoxelEB |
| --- | --- | --- | --- |
| 主控类 | Raytracing（src/raytracing/raytracing.h:37） | Voxelrt（src/voxelrt/voxelrt.h:43） | Voxeleb（src/voxeleb/voxeleb.h:40） |
| IO 类 | RaytracingIO（raytracingio.h:69） | VoxelrtIO（voxelrtio.h:65） | VoxelebIO（voxelebio.h:59） |
| GPU 管线 | 1 条光追管线：raygen、2 个 miss、1 个 closest-hit | 3 条计算管线 | 15 条计算管线（每个 VoxelEBStage 一条） |
| 阶段枚举 | RaytracingStageIndices（src/base/structs.h:137） | VoxelRTStage：gap、diffuse、out（structs.h:104） | VoxelEBStage，15 项（structs.h:84） |
| 推送常量 | RayRTSetting，80 字节 | VoxelRTSetting，字段推算 96 字节 | VoxelLstSetting，128 字节 |
| 描述符绑定 | 11 个 | 18 个 | 42 个编号，写入 41 个（rads 无描述符） |
| 场景构建 | Scene::createObjScene | Scene::createPrimObjScene | Scene::createPrimObjScene |
| 光学参数 | Compo::createCompOptical | Compo::createCompOptical | Compo::createCompProperty |
| 运行时着色器目录 | definedDir/shader/raytracing | definedDir/shader/voxelrt | definedDir/shader/voxeleb |

场景构建与光学参数的函数名取自各 upload 函数（src/raytracing/raytracing.cpp:322-328、src/voxelrt/voxelrt.cpp:160-169、src/voxeleb/voxeleb.cpp:156-164）。

#### 5.1.2 生命周期调用链

Engine 按模式创建主控对象与 IO 对象（src/base/engine.cpp:100-107），再依次调用下列函数（engine.cpp:191-208、241-245、302-308、328-334）：

```
Engine::init   → make_shared<Raytracing 或 Voxeleb 或 Voxelrt> + make_shared<对应 IO>
Engine::input  → X->setup(appSetting, io)      取设备/队列、命令池、分配器、加速结构构建器
               → X->upload(fileio, io)         读 defined、光学参数、场景、几何、设置（纯 CPU）
               → io->definedDir / projectDir   在 upload 之后才赋值，来自 FileIO 的 XML 对象
Engine::create → X->create(io)                 Buffer → Descriptor → Pipeline → Command → updateSetting
Engine::run    → X->run(io, fileio)            返回 true 则进程退出码为 0，否则为 1
Engine::destroy→ X->destroy(io)                vkDeviceWaitIdle 后逆序释放
```

注意 definedDir 在 upload 之后赋值，因此 upload 阶段的代码不能使用它；它只在 Pipeline::createPipeline 加载 SPIR-V 时使用。

#### 5.1.3 模块依赖方向

```
Engine ──► Raytracing / Voxelrt / Voxeleb
              ├─► src/base：Scene、Geometry、Compo、VirtualScreen、AccelStruct、FileIO、Utils 等
              └─► Buffer、Descriptor、Pipeline、Command ──► XxxIO（共享状态）
XxxIO ──► src/base：MeshIO、InstanceIO、VirtualIO、VoxelIO、SurfIO、DefinedIO、structs*.h
shader ◄── src/base/structs_cg.h（字节布局镜像）、shader/settings.h、shader/bindingLayout.h
```

依赖只向下：阶段类不依赖主控类，主控类不依赖其他求解模块；三个求解模块之间没有相互引用。C++ 结构体与 GLSL 结构体靠“同序同类型”人工对齐，只有部分结构带 static_assert（见 5.2.5、5.3.5、5.4.5）。

#### 5.1.4 同名类与头文件保护的陷阱

三个目录各有一份 buffer.h、descriptor.h、pipeline.h、command.h，类名都叫 Buffer、Descriptor、Pipeline、Command，且头文件保护宏相同：`FIELD_BUFFER_H`（src/raytracing/buffer.h:17、src/voxelrt/buffer.h:16、src/voxeleb/buffer.h:17）、`FIELD_DESCRIPTOR_H`（raytracing/descriptor.h:16、voxelrt/descriptor.h:16、voxeleb/descriptor.h:20）、`FIELD_PIPELINE_H`、`FIELD_COMMAND_H` 同理。含义如下：

- 同一个编译单元里只有先被包含的那一套生效。src/base/engine.h:23-27 先包含 raytracing 再包含 voxeleb 和 voxelrt，因此 engine.cpp 里看到的 Buffer 等类是 RayTracing 版本，只在指针层面使用，不调用阶段类。
- 各 .cpp 用 `#include "buffer.h"` 的同目录相对写法（如 src/voxeleb/buffer.cpp:23），所以每个实现文件拿到的是自己目录的头文件。
- 链接能通过，依赖于这些成员函数以不同的 IO 参数类型重载，符号互不冲突；`Command::Command()` 只在 src/raytracing/command.cpp:26 定义一次，voxelrt 与 voxeleb 的 command.h 只声明（src/voxelrt/command.h:26、src/voxeleb/command.h:26），靠同一份定义链接。该做法属于违反单一定义规则的隐患（待核对：各编译器下的实际行为）。
- 因此新增求解模块不要沿用 FIELD_xxx_H 宏名，最好改用 `#pragma once` 并放入命名空间（facetrt 与 faceteb 即是这样做的，如 src/facetrt/buffer.h:11-17）。

#### 5.1.5 着色器的构建与部署

CMake 用 glslc（`--target-env=vulkan1.2`）把着色器编成 `.spv`（CMakeLists.txt:171-211、260-365）：RayTracing 固定编译 4 个文件（offline.rgen、closestHit.rchit、rmiss.rmiss、shadow.rmiss）；VoxelEB 用 glob 编译 shader/voxeleb 下全部 `.comp`，并额外生成 3 个 `_most` 变体（aeresist、budget、evapo，加 `-DVOXELEB_MOST=1`，CMakeLists.txt:290-310）和 1 个 `voxelrad_scattering_trace.comp.spv`（同一源文件加 `-DSCATTER_TRACE_STAGE=1`，CMakeLists.txt:313-333），合计 19 个 SPIR-V；VoxelRT 同样 glob 编译 shader/voxelrt 下全部 `.comp`（3 个）。`.glsl` 与 `.h` 公共头变化时，所有依赖它们的着色器都会重编（CMakeLists.txt:176-180）。构建后的 spv 目录由 `histream_runtime_resources` 目标复制到可执行文件旁的 `shader/voxeleb`、`shader/voxelrt`、`shader/raytracing`（CMakeLists.txt:499-517）。

CMake 与 pipeline.cpp 都使用小写目录 `shader/raytracing`（CMakeLists.txt:198；src/raytracing/pipeline.cpp:169-172），而源码目录实际叫 `shader/RayTracing`。Windows 文件系统不区分大小写所以没有问题，换到区分大小写的系统需要统一（待核对：本仓库是否有 Linux 构建）。

### 5.2 RayTracing 模块（src/raytracing）

#### 5.2.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出、类型、函数 |
| --- | --- | --- | --- |
| raytracing.h | 88 | 主控类声明，持有 8 个子对象 | Raytracing：setup、upload、create、run、destroy、trace、output、outputOrth、outputAlbedo、uploadSetting、updateSetting |
| raytracing.cpp | 973 | 主流程实现；物理纹理因子；自适应采样；读回与写 TIFF | buildPhysicalTextureFactors、raytracingBandNames、sceneTopHeight（匿名命名空间） |
| raytracingio.h | 238 | IO 容器，含全部 Vulkan 句柄与设置 | RaytracingIO、kNoTextureOffset、Allocator 类型别名 |
| raytracingio.cpp | 54 | 占位，旧函数均已注释 | 无 |
| buffer.h、buffer.cpp | 34、229 | 上传材质、网格、实例链接、纹理因子、传感器、光源、输出缓冲，并构建加速结构 | Buffer::createBuffer、destroy |
| descriptor.h、descriptor.cpp | 50、120 | 11 个绑定的布局、池与描述符集 | Descriptor::RaytracingbindingInd、createDescriptor、destroy |
| pipeline.h、pipeline.cpp | 46、343 | 光追管线、SBT、特化常量 | Pipeline::createPipeline、createShaderBindingTable、destroy |
| command.h、command.cpp | 55、252 | 按行分块的 vkCmdTraceRaysKHR 提交与同步 | Command::create、run、submit、recordCommandBuffer、waitFence、destroy |

#### 5.2.2 Raytracing 主控类

公共接口（src/raytracing/raytracing.h:54-72）：

```
bool setup(AppSetting&, shared_ptr<RaytracingIO>&);                 // 设备、队列、命令池、分配器、SBT、BLAS 构建器
bool upload(shared_ptr<FileIO>&, shared_ptr<RaytracingIO>&);        // 纯 CPU：读场景与设置，算物理纹理因子
bool create(shared_ptr<RaytracingIO>&);                             // GPU 资源；场景无实例时返回 false
bool run(shared_ptr<RaytracingIO>&, shared_ptr<FileIO>&);           // 观测循环
void trace(shared_ptr<RaytracingIO>&);                              // 一次完整追踪，可含自适应两遍
void output(io, fileio, int kangle, int kpos = -1);                 // 透视或 UAV 航点输出
void outputOrth(io, fileio, int kangle, int kpos = -1);             // 正射输出
void outputAlbedo(io, fileio, int kangle);                          // albedo.txt 追加
```

核心调用链：

```
upload  : readDefined → Compo::createCompOptical → Scene::createObjScene → Geometry::createGeometry
          → uploadSetting → buildPhysicalTextureFactors                          (raytracing.cpp:316-331)
create  : Buffer::createBuffer → Descriptor::createDescriptor → Pipeline::createPipeline
          → Command::create → updateSetting                                      (raytracing.cpp:335-351)
run     : 清空 albedo.txt → [UAV 航点：updateSensorPos → trace → output]
          或 [逐角度：updateAngle → trace → output 或 outputOrth → outputAlbedo]   (raytracing.cpp:545-626)
trace   : 常规 = Command::run；自适应 = 侦察遍 → 读回 sigma → 二分求缩放 → 正式遍    (raytracing.cpp:460-540)
```

要点：

- uploadSetting（raytracing.cpp:371）复制传感器与设置，并处理环境变量覆盖：HISTREAM_RT_ADAPTIVE、HISTREAM_RT_ADAPTIVE_SCOUT（限幅 0.05-1）、HISTREAM_RT_ADAPTIVE_TILE、HISTREAM_RT_SEED、HISTREAM_RT_SAMPLER（取值 sobol 或 legacy，否则抛异常）（raytracing.cpp:392-406）。自适应采样要求 Sobol 采样器，否则被忽略；样本数小于 8 时同样关闭（raytracing.cpp:409-414）。
- updateSetting（raytracing.cpp:422）把 IO 字段写入 `io->setting`。sceneSize 的 x、y 下限取 0.01，sceneTop 由 sceneTopHeight 求得（遍历所有实例的局部包围盒 8 个角点，取世界最大 y 并加余量），无几何时为 1e30（raytracing.cpp:203-241）。
- Command::run 每次提交后 `setting.imageOffsetY` 与 `dispatchRowCount` 会被改写，结束后复位为 0，因此 trace 的自适应部分必须在 run 之外修改 `adaptivePass`、`n_sample` 等字段（raytracing.cpp:473-536）。
- output 与 outputOrth 的读回步骤相同：创建主机可见缓冲，经 `VirtualScreen::bufferToBuffer` 拷出，复制到 `std::vector<float>` 后立即销毁 Vulkan 缓冲（raytracing.cpp:642-675、780-815）。读取长度比图像多 1 个 float，末位是着色器丢弃的非有限样本计数，由 reportDroppedSamples 报警（raytracing.cpp:245-251）。
- 大气校正与天空填充使用 `src/base/atmosphere_lut.h` 的 atmosphereCorrectOutputValue、atmosphereSkyPixelZenith、atmosphereSkyOutputValue。output 中，值为 0 或非有限且属于天空像元的像素填天空值，其余像素做大气校正（raytracing.cpp:844-863）。
- 写文件调用 `FileIO::writeTIFData(projectDir, data, w, h, n_wave, angle, -1, 后缀, kpos, false, 波段名)`；启用大气层时后缀为 `_a`（raytracing.cpp:763-766、866-869）。

#### 5.2.3 RaytracingIO 字段分组

| 分组 | 主要字段（raytracingio.h 行号） |
| --- | --- |
| 路径 | projectDir、definedDir（94-95） |
| 通用 IO 子对象 | m_meshio、m_instanceio、m_virtualio、m_defined、m_pAccelStruct（105-108、182） |
| 场景 | n_modelmesh、n_instance、sceneSize、sceneOrigin、voxelSize、voxelOrigin（117-127） |
| 观测几何 | sensor（SensorMatrix）、light（LightSet）、angles、waves、isUAVTrave、uavposes、uavViewAzimuths、n_pos、k_pos、kangle（112、133-141） |
| 设置 | datestr、timestr、n_wave、n_angle、imageSize、n_sample、maxDepth、periodicNeighborCount、skyboxEnabled、dispatchRowCount、rtSeed、rtSampler、rtAdaptive、adaptiveTile、adaptiveScoutFraction、输出开关 isTemperature、isDisplay、isImage、isAlbedo、isOrth（146-175） |
| 推送常量与小缓冲 | setting（RayRTSetting）、m_pBufferWave、m_pBufferSensor、m_pBufferLight（178-181） |
| 物理纹理 | instanceTextureOffsets、textureFactors、m_pBufferTextureOffset、m_pBufferTextureFactor（187-190） |
| Vulkan 基础 | m_device、m_physicalDevice、m_instance、m_pAlloc、m_debug、m_queues、m_queue、m_fence、m_semaphores（195-206） |
| 描述符与管线 | m_genCmdBuf、m_bindings、m_descPool、m_descSet、m_descSetLayout、m_pipelineLayout、m_pipeline（209-216） |
| SBT | useSBTWrapper（缺省 true）、useDeferred（缺省 false）、m_sbtWrapper、missCount=2、hitCount=1、m_rtSBTBuffer、各区域 Region、m_rtProperties（223-233） |

`kNoTextureOffset = 0xFFFFFFFF`，必须与 `offline.rgen` 的 NO_TEXTURE_OFFSET 一致（raytracingio.h:65；shader/RayTracing/offline.rgen:91）。Allocator 类型由编译宏 ALLOC_DMA、ALLOC_VMA 选择，缺省为 Dedicated（raytracingio.h:50-58）；voxelrtio.h 与 voxelebio.h 各有一份同样的别名。

#### 5.2.4 Buffer、Descriptor、Pipeline、Command

**Buffer::createBuffer**（src/raytracing/buffer.cpp:25）按下列顺序创建，每项用独立命令缓冲 `submitAndWait`：光谱材质（有数据才建）、热材质（为空则补一个 300 K 的占位元素，因为着色器总读绑定 1，buffer.cpp:54-60）、波长、逐网格顶点与索引缓冲（带 STORAGE、DEVICE_ADDRESS 与 BLAS 构建输入标志，buffer.cpp:76-95）、写入顶点与索引设备地址后的 meshLinks、实例链接、物理纹理偏移与因子（无纹理时放占位元素；字节数超过 maxStorageBufferRange 则抛异常，buffer.cpp:118-133）、加速结构（`createAccelStruct`）、传感器与光源 UBO、输出图像存储缓冲、最后 `finalizeAndReleaseStaging`。输出缓冲元素数为 `checkedOutputImageElements(...)+1`，自适应时再加 `16 + 2*宽*高`（buffer.cpp:167-173）。Buffer::destroy（buffer.cpp:189）对有条件创建的缓冲先判空。

**Descriptor**：枚举 RaytracingbindingInd 自上而下为 0..10（descriptor.h:27-41），必须与 `shader/bindingLayout.h` 的 RAYTRACING 段 B_* 一致（bindingLayout.h:33-59）。

| 绑定号 | 名称 | 类型 | 可见阶段 | 内容 |
| --- | --- | --- | --- | --- |
| 0 | spectral | 存储缓冲 | 计算、raygen | Spectral 数组（反射率、透射率、镜面分数、粗糙度） |
| 1 | thermal | 存储缓冲 | 计算、raygen | Thermal 数组（向阳、背阴温度） |
| 2 | modelLink | 存储缓冲 | 片元、计算、raygen | MeshLink（含顶点、索引设备地址） |
| 3 | instanceLink | 存储缓冲 | 同上 | InstanceLink |
| 4 | tlas | 加速结构 | 同上 | TLAS |
| 5 | sensor | UBO | 同上 | SensorMatrix |
| 6 | light | UBO | 同上 | LightSet |
| 7 | wave | 存储缓冲 | 同上 | 波长数组 |
| 8 | storage | 存储缓冲 | 同上 | 输出图像（float） |
| 9 | textureOffset | 存储缓冲 | 仅 raygen | 每实例的因子起点 |
| 10 | textureFactor | 存储缓冲 | 仅 raygen | 每实例三角形因子 |

createDescriptor（descriptor.cpp:22）在写入前检查光谱、波长、实例链接三个缓冲存在，缺失即抛 runtime_error（descriptor.cpp:59-64）。顶点与索引缓冲不占绑定，着色器通过 buffer_reference 以设备地址访问（bindingLayout.h:60-62）。

**Pipeline::createPipeline**（pipeline.cpp:134）：重建管线布局，推送常量范围为 raygen、closest-hit、miss 三个阶段，大小 `sizeof(RayRTSetting)`（pipeline.cpp:149-150）。读取 4 个 spv 并检查存在性，缺失抛异常（pipeline.cpp:168-178）。着色器组下标依 `RaytracingStageIndices`：eRaygen、eMiss、eMiss2、eClosestHit（src/base/structs.h:137-144）；组类型为 3 个 GENERAL 与 1 个 TRIANGLES_HIT_GROUP，`maxPipelineRayRecursionDepth = 2`（pipeline.cpp:234-269）。SBT 默认由 `nvvk::SBTWrapper` 生成，`useSBTWrapper=false` 时走手工的 createShaderBindingTable，布局为 raygen(1)、miss(2)、hit(1)，无 callable（pipeline.cpp:41-126、308-315）。创建完成后立即销毁着色器模块。

**特化常量**（仅 offline.rgen 使用，pipeline.cpp:190-206）：

| constant_id | 着色器内名称 | 取值来源 |
| --- | --- | --- |
| 8 | RT_SAMPLER_SOBOL | rtSampler 非 0 则为 1（sampler_qmc.glsl:28） |
| 9 | RT_MAX_WAVE | 取 16、32、64、128、800 中不小于 n_wave 的最小值；环境变量 HISTREAM_RT_MAX_WAVE 可强制（不小于 n_wave，不大于 800） |
| 10 | RT_ADAPTIVE | rtAdaptive 为真则为 1 |

offline.rgen 头部注释写的候选集合是 8、16、32、64、128、800（offline.rgen:75-76），与 pipeline.cpp:192 的代码不一致，以代码为准。

**Command**：`run`（command.cpp:55）按行分块提交整幅图像：初始每块约 65536 像素，下限约 32768 像素，之后按实测耗时把每块行数乘以 `clamp(120ms/实测, 0.25, 4)`，目的是避开 Windows 的 GPU 超时恢复（command.cpp:68-93）。`submit`（131）先记录一次全局内存屏障（ALL_COMMANDS 到光追阶段），再调用 `recordCommandBuffer`（183：绑定管线与描述符集、推送 RayRTSetting、vkCmdTraceRaysKHR，高度取 dispatchRowCount 或整幅）。信号量参数未使用，同步靠 fence；`waitFence`（227）等待、复位并回收命令缓冲。

#### 5.2.5 着色器与管线对应、推送常量

| 着色器文件 | 行数(约) | 管线角色 | 说明 |
| --- | --- | --- | --- |
| shader/RayTracing/offline.rgen | 947 | raygen | 每线程一个像素：多样本路径追踪，输出各波段 HDRF 或亮温；含周期边界遍历（ClosestHitPeriodic、AnyHit）、AdaptivePass、PathTrace、SamplePixel、main |
| shader/RayTracing/closestHit.rchit | 55 | closest-hit | 只把 hitT、图元与实例号、重心坐标、变换矩阵写入 PtPayload |
| shader/RayTracing/rmiss.rmiss | 32 | miss（主光线） | 空函数，未命中时 hitT 保持 raygen 预设的 INFINITY |
| shader/RayTracing/shadow.rmiss | 52 | miss（阴影） | 把 ShadowHitPayload.isHit 置 false |
| shader/RayTracing/sampler_qmc.glsl | 123 | 被 include | Owen 置乱 Sobol 采样器，维度分配见文件头 |

offline.rgen 通过 `#define RAYTRACING 1` 选择 bindingLayout.h 的对应段（offline.rgen:42）。主光线 payload 在 location 0，阴影光线 payload 在 location 1（offline.rgen:70-71）。阴影查询优先用 `gl_RayFlagsTerminateOnFirstHitEXT`；周期边界启用时改用内联 rayQuery 逐副本查询（offline.rgen:160-278）。

推送常量 RayRTSetting 的 C++ 与 GLSL 定义同序（src/base/structs_cg.h:90-113；shader/settings.h:26-49），偏移由 static_assert 固定（structs_cg.h:114-116）：

| 偏移 | 字段 | 偏移 | 字段 |
| --- | --- | --- | --- |
| 0 | frame（随机种子） | 36 | isDisplay |
| 4 | maxDepth | 40 | isTemperature |
| 8 | n_sample | 44 | adaptiveScale |
| 12 | n_wave | 48 | adaptivePass |
| 16 | imageSize（ivec2） | 52 | periodicNeighborCount |
| 24 | adaptiveMaxExtra | 56 | skyboxEnabled |
| 28 | adaptiveTile | 60 | imageOffsetY |
| 32 | adaptiveMinCount | 64、72、80 | sceneSize、sceneTop、总大小 |

#### 5.2.6 输出数据格式

- 输出存储缓冲布局：按波段分平面，`outImage[宽*高*波段 + 行*宽 + 列]`（offline.rgen:941）；其后紧跟 1 个丢弃计数；自适应时在 `宽*高*n_wave + 16` 之后另有两个平面，平面 1 为侦察 sigma（offline.rgen:679-681、735；raytracing.cpp:471-484）。
- TIFF：多波段，波段名由 raytracingBandNames 生成：波长不大于 2.5 微米为 “Reflectance [-]”，更长波段按 isTemperature 取亮温或光谱辐亮度（raytracing.cpp:184-201）。波长大于 50 视为 nm。
- albedo.txt：run 开头清空，outputAlbedo 逐波段追加一行 `波长 sza saa vza vaa 均值`，均值只对值大于 0 的像元求取且不做大气校正（raytracing.cpp:565-577、898-939）。
- 标准输出：PROGRESS 行（5、10 至 95、100）与 OBSERVATION 行（UAV 航点）供前端解析（raytracing.cpp:563、588、602、624）。

### 5.3 VoxelRT 模块（src/voxelrt）

#### 5.3.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出、类型、函数 |
| --- | --- | --- | --- |
| voxelrt.h | 93 | 主控类声明 | Voxelrt：setup、upload、create、run、destroy、uploadSetting、updateSetting、uploadDefined、output、outputPos、outputRadiationProcess、outputVoxel |
| voxelrt.cpp | 742 | 主流程；回读与写 TIFF；辐射过程导出 | planckRadiance、writeVoxelrtProcessField、voxelrtBandNames（匿名命名空间） |
| voxelrtio.h | 235 | IO 容器 | VoxelrtIO |
| voxelrtio.cpp | 5 | 仅包含头文件 | 无 |
| buffer.h、buffer.cpp | 35、448 | 全部缓冲，空数据时上传 1 元素占位 | Buffer::createBuffer、destroy |
| descriptor.h、descriptor.cpp | 56、167 | 18 个绑定 | Descriptor::VoxelrtbindingInd、createDescriptor、destroy |
| pipeline.h、pipeline.cpp | 50、412 | 3 条计算管线；其余 createXxxPipeline 已注释 | Pipeline::createPipeline、destroy、createShaderBindingTable（空） |
| command.h、command.cpp | 73、316 | 三阶段派发 | Command::create、runRT、runRTAccelerated、submit、recordCommandBuffer、waitFence、destroy |

#### 5.3.2 Voxelrt 主控类与调用关系

```
upload : uploadDefined → Compo::createCompOptical → uploadSetting（必须先于场景）
         → Scene::createPrimObjScene → voxelmerge::report → Geometry::createGeometry   (voxelrt.cpp:155-175)
create : Buffer → Descriptor → Pipeline（失败返回 false）→ Command::create → updateSetting  (voxelrt.cpp:260-271)
run    : runTransport(sun) = runRT 或 runRTAccelerated(…, 4)，gap 与 diffuse 在太阳角不变时复用
         UAV：updateSensorPos → runTransport(angles.front()) → outputRadiationProcess（kpos==0）→ outputPos
         常规：updateAngle → runTransport → outputRadiationProcess（kangle==0）→ outputPos 或 output   (voxelrt.cpp:277-347)
```

要点：

- uploadSetting 必须在 createPrimObjScene 之前，因为体元化时要用到“非均质体元”开关（voxelrt.cpp:162-165）。voxelFlags 由 `voxelflags::resolve` 综合工程设置与环境变量 HISTREAM_VOXEL_PROXY、HISTREAM_VOXEL_DIRROT_BLOCK 得到（voxelrt.cpp:199-201；src/base/voxelflags.h:80-93）。
- updateSetting（voxelrt.cpp:212）：`scale = stepsize_surface`；`maxStep = clamp(maxDepth*128, 128, 2048)`；`voxelSize.y` 改写为 `ceil(场景高度/体元边长)`（至少 1）；spectralBatchSize 恒为 1，collisionOrder 初值 1。maxIteration 在主机端从未赋值，沿用 VoxelRTSetting 缺省值 3（structs_cg.h:216），而着色器旧路径用 `maxIteration > 1` 作开关（voxelrad_diffuse_single.comp:1229），不要把它改成 1。
- runTransport 的复用判据是太阳 sza、saa 与上一次完全相同（voxelrt.cpp:290-300）；复用时只重跑 out 阶段。
- outputRadiationProcess 只在第一个观测后调用一次，因为 VoxelRT 只有一个静态辐射场（voxelrt.cpp:313、331）。输出格式见 5.3.6。
- output 与 outputPos 的差别：output 经 `Geometry::orthcorrect` 求 4 个系数的双线性映射（源像元 = i*c0 + j*c1 + i*j*c2 + c3，截断取整），仅对源值非零的像元做大气校正；outputPos 不重采样，对空像元且位于天空视场内的像素填天空值（voxelrt.cpp:540-678）。TIFF 文件名后缀 `_v` 或 `_v_a`。
- 风险：VoxelrtIO 的 `k_angle` 无初值（voxelrtio.h:131），UAV 分支未赋值（voxelrt.cpp:305-319），而 outputPos 用它选角度（voxelrt.cpp:635），UAV 模式下取值不确定（待核对：实际运行结果）。`isAlbedo` 只被读取，没有对应输出（voxelrt.cpp:186）。

#### 5.3.3 VoxelrtIO 字段分组

| 分组 | 主要字段（voxelrtio.h 行号） |
| --- | --- |
| 路径 | projectDir、definedDir（82-83） |
| 场景 | sceneSize_XYZ、voxelSize_XZY（分量顺序为 x、竖直、y）、sceneOrigin_XYZ、voxelOrigin_XZY、stepsize_surface、stepsize_height、stepsize_atmosphere、n_voxel、n_instance、n_modelmesh（86-103） |
| 观测几何 | isUAVTrave、uavposes、uavViewAzimuths、meteo、sensor、light、angles、waves（105-120） |
| 运行设置 | n_wave、n_angle、n_pos、n_node、k_angle、k_node（初值 0，run 中置 -1）、k_pos、imageSize、n_sample、maxDepth、输出开关 isTemperature、isDisplay、isImage、isRadiationProcess、isAlbedo、heterogeneousVoxel、acceleratedRadiationSolver、collisionOrders（缺省 2）、voxelFlags、periodicNeighborCount、skyboxEnabled（126-152） |
| 子 IO | m_meshio、m_instanceio、m_virtualio、m_voxelio、m_surfio、m_defined（155-160） |
| 推送常量与小缓冲 | setting（VoxelRTSetting）、m_pBufferWave、m_pBufferSensor、m_pBufferLight（163-168） |
| Vulkan 与同步 | 与 RaytracingIO 同（175-193） |
| 管线 | 实际使用 m_pipelines（std::map，键为 VoxelRTStage）与 m_pipelineLayout（209-210）；其余 _rt、_et、_eb、_aero、_bio 与 SBT 成员为沿用遗留，VoxelRT 不使用（199-206、212-227） |

#### 5.3.4 Buffer、Descriptor、Pipeline、Command

**Buffer::createBuffer**（src/voxelrt/buffer.cpp:35）：先做规模检查（`voxellimits::requireStorageRange`、`requireDispatch`，buffer.cpp:52-59）；随后依次创建光谱（空则占位）、热材质（空则 300 K 占位，buffer.cpp:92-99）、冠层（空则占位）、水体参数（空则补一个默认元素，buffer.cpp:123-128）、体元温度（初值向阳 305 K、背阴 295 K，buffer.cpp:134）、每个 primMesh 的顶点与索引缓冲（代理几何模式不是 full 时改走 `proxymesh::uploadPositionMesh`，buffer.cpp:148-182）、meshLinks（写入设备地址，索引缓冲不存在则地址为 0，buffer.cpp:186-194）、实例链接、体元链接与 hex 与 NanoVDB（nanoHandle 为空时三者都放占位，buffer.cpp:219-261）、加速结构、传感器与光源 UBO、波长、VoxelDir（全 0）、辐射场 VoxelRad、VoxelNetRad、输出图像、LAD。

辐射场缓冲每体元占 `2*n_wave` 个 VoxelRad（8 字节）：前 n_wave 个槽为各波段累计的环境辐射（cumulated 字段），后 n_wave 个槽按碰撞阶数交替存放入射辐射（奇数阶用 cumulated，偶数阶用 induced）（buffer.cpp:315-332；shader/voxelrt/voxelrad_diffuse_single.comp:92-128）。

**Descriptor**：枚举 VoxelrtbindingInd 共 18 项（descriptor.h:26-46），与 bindingLayout.h 的 VOXELRT 段 B_* 一一对应（bindingLayout.h:201-218）。所有绑定使用同一组阶段：片元、计算、raygen、intersection；TLAS 额外加 closest-hit（descriptor.cpp:43-61）。

| 绑定号 | 名称 | 类型 | 内容 |
| --- | --- | --- | --- |
| 0、1、2 | spectral、thermal、canopy | 存储缓冲 | 光谱、热、冠层参数 |
| 3、4、5 | meshLink、instanceLink、voxelLink | 存储缓冲 | 几何到材质、体元的链接表 |
| 6 | nano | 存储缓冲 | NanoVDB 网格数据（uint 数组） |
| 7 | tlas | 加速结构 | TLAS |
| 8 | sensor | UBO | SensorMatrix |
| 9 | wave | 存储缓冲 | 波长 |
| 10 | light | UBO | LightSet |
| 11、12、13 | dir、rads、netRad | 存储缓冲 | VoxelDir、VoxelRad、VoxelNetRad |
| 14 | storage | 存储缓冲 | 输出图像 |
| 15、16、17 | lad、hex、waterSet | 存储缓冲 | 叶面积密度、异质体元参数、水体参数 |

thermal 绑定只在 `m_pBufferThermal` 非空时写入，而 Buffer 实际总会创建它（descriptor.cpp:79-84）。

**Pipeline::createPipeline**（src/voxelrt/pipeline.cpp:43）：管线布局的推送常量范围按 `sizeof(VoxelLstSetting)`（128 字节）声明，而 command.cpp 实际推送 `sizeof(VoxelRTSetting)`（pipeline.cpp:62-63；command.cpp:284）；范围大于等于实际长度，因此可用，但类型不一致。三个阶段的 spv 路径见下表；周期边界通过特化常量 0（PERIODIC_BOUNDARY_ENABLED）与 1（PERIODIC_CROSSING_LIMIT，限幅 0-20）固化（pipeline.cpp:94-101）。创建失败返回 false，已建管线由 destroy 回收。

**Command**（src/voxelrt/command.cpp）：每次 submit 前录制 `ALL_COMMANDS -> COMPUTE_SHADER` 的全局内存屏障（command.cpp:275-279），每次提交后立即 waitFence，阶段间严格串行。`runRT`（96）：gap 按体元 1D 派发，组数 `ceil(n_voxel/64)`；diffuse 按 波段 × 碰撞阶数 逐个提交（`n_jump` 当作当前波段，`collisionOrder` 当作阶数，130-137）；out 按图像 2D 派发，组 8×8。`runRTAccelerated`（152）以 `spectralBatchSize`（限幅 1-4）合并波段，`n_jump` 为批内首个波段。已知风险：1D 派发组数超过 `maxComputeWorkGroupCount[0]` 时属未定义行为（源码 DEFERRED 注释，command.cpp:103）。

#### 5.3.5 着色器与推送常量

| 阶段 | 着色器 | 行数(约) | 派发 | 作用 |
| --- | --- | --- | --- | --- |
| gap | shader/voxelrt/voxelrad_solar.comp | 225 | 1D，每体元一线程 | 沿太阳方向逐步追踪，写 voxelDirs[].solar；建筑按法线或顶面、侧面取多个起点（solar.comp:146-209） |
| diffuse | shader/voxelrt/voxelrad_diffuse_single.comp | 1290 | 1D，逐波段、逐阶数 | 64 个漫射方向求环境入射辐射，写 voxelRads；main 按 collisionOrder、spectralBatchSize 选择三条路径（diffuse_single.comp:1120-1288） |
| out | shader/voxelrt/voxelrad_image.comp | 790 | 2D，每像素一线程 | 视线追踪，输出 HDRF 或辐亮度、亮温，覆盖加权平均，未覆盖像元 4×4 补采样（image.comp:696-789） |

三个着色器都以 `#define VOXELRT 1` 选择绑定段。VoxelRTSetting（structs_cg.h:212-239；shader/settings.h:89-112）字段同序；其中 ivec3 voxelSize 位于偏移 48，恰好对齐 16 字节，所以 C++ 与 GLSL 偏移一致。该结构只有 `sizeof <= 128` 的断言（structs_cg.h:240），缺少逐字段偏移断言（源码已有 DEFERRED 注释），增删字段时需人工核对。

#### 5.3.6 输出文件格式

- TIFF：波段名由 voxelrtBandNames 生成，规则同 RayTracing（voxelrt.cpp:79-95）。
- `voxelrt.bin` 与 `voxelrt.json`：写在工程目录；小端 float32，体元交错，每体元记录 `x y z（体元中心，米）+ n_wave 个波段值 + sunlit_fraction`，共 `4+n_wave` 个 float。波段值为向阳与背阴状态按日照比例的加权平均；热红外波段取 `(R+T)*环境辐亮度 + 发射率*Planck`，光学波段取 `环境照明 + 直射分量`。JSON 记录 recordFloats、字段偏移（3+波段号）、单位与定义；标准输出打印 `PROCESS<TAB>路径`（voxelrt.cpp:353-501）。

### 5.4 VoxelEB 模块（src/voxeleb）

#### 5.4.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出、类型、函数 |
| --- | --- | --- | --- |
| voxeleb.h | 102 | 主控类声明 | Voxeleb：setup、upload、create、run、runObserver、destroy、uploadSetting、updateSetting、uploadAero、uploadMeteo、updateMeteo、applyShortwaveSplit、output、outputTxt、outputVoxel、outputFluidSlices |
| voxeleb.cpp | 1703 | 逐时间节点主流程，输出与诊断 | isOpticalWavelength、inversePlanckTemperature、voxelebBandNames（匿名命名空间） |
| observer_capture.inl | 151 | 观测器模式，被 voxeleb.cpp 末尾 include | runObserver、observerRead、observerUpload |
| voxelebio.h | 274 | IO 容器 | VoxelebIO |
| voxelebio.cpp | 5 | 仅包含头文件 | 无 |
| buffer.h、buffer.cpp | 35、1101 | 全部缓冲，含稠密流体网格与大辐射缓冲 | Buffer::createBuffer、destroy |
| descriptor.h、descriptor.cpp | 96、287 | 42 个编号的绑定 | Descriptor::VoxelebbindingInd、createDescriptor、destroy |
| pipeline.h、pipeline.cpp | 54、439 | 15 条计算管线 | Pipeline::createPipeline、destroy、createShaderBindingTable（空） |
| command.h、command.cpp | 73、1156 | 能量平衡全流程调度与诊断读回 | Command::runEB、runRT、runFluid、submit、recordCommandBuffer、waitFence |

#### 5.4.2 Voxeleb 主控类

```
upload   : uploadDefined → Compo::createCompProperty → uploadSetting → Scene::createPrimObjScene
           → voxelmerge::report → Geometry::createGeometry → uploadMeteo → uploadAero    (voxeleb.cpp:151-170)
create   : Buffer → Descriptor → Pipeline（失败返回 false）→ Command::create → updateSetting   (voxeleb.cpp:409-420)
run      : 设置了 HISTREAM_OBSERVER_MANIFEST 则转 runObserver
           否则 对 knode in [startTimeNode, endTimeNode)：
             updateMeteo → Geometry::updateAngle(0) → applyShortwaveSplit
             → [流体启用] Command::runFluid → Command::runEB → outputFluidSlices → outputVoxel
             → 观测循环：updateAngle 或 updateSensorPos → Command::runRT → output → outputTxt    (voxeleb.cpp:429-616)
```

要点：

- uploadSetting 必须先于场景构建，因为 hex 提取开关要在体元化时使用（voxeleb.cpp:158-160）。stabilityCorrection 与流体 localExchange 互斥，同时打开时稳定度修正被关闭并打印 STABILITY_CORRECTION 行（voxeleb.cpp:213-217）。
- updateSetting（voxeleb.cpp:225）同 VoxelRT 的竖直层数改写，`voxelCount` 经 `voxelflags::testRadianceVoxels` 取值（测试旋钮，正常等于 n_voxel）。`maxStep` 来自 `radiationMaxSteps`，`n_jump` 来自 `spectralAccelerationWidth`（voxeleb.cpp:197-202）。
- updateMeteo（278）以 `vkCmdUpdateBuffer` 把当前节点的 Meteo 写入 UBO；启用流体时重算流体子步数与环境风，并同步更新 FluidParameters。气象温度 Ta 为摄氏度，送入流体参数时加 273.15。
- applyShortwaveSplit（347）：固定模式只打印日志；erbs 模式按节点重算直射比例，更新 LightSet 与 VNIR 的 atomconds 并分块（每块 65536 字节）重传 GPU。
- run 中每个节点都会调用 outputVoxel（564）做状态校验：任一体元出现非有限温度、通量或日照比例超出 0 至 1 即抛异常，哪怕没有开启过程输出。
- output（654）：正射投影用 orthcorrect 映射，透视投影直接取像元；天空像元且源值为 0 或非有限时填大气天空值，光学波段夜间（Rin 不大于 0.5）填 0；其余像元做大气校正，热红外且 isTemperature 时用 inversePlanckTemperature 反演亮温（voxeleb.cpp:716-735）。后缀 `_v` 或 `_v_a`。
- outputTxt（761）每个角度向 `result_statistics.txt` 追加每波段一行 `t sza saa vza vaa 波长 均值`，表头在 run 开头写入（voxeleb.cpp:469-476）。
- uploadAero（1685）只处理 AeroType::ONE，其余分支是空占位；函数固定返回 false。

#### 5.4.3 VoxelebIO 字段分组

| 分组 | 主要字段（voxelebio.h 行号） |
| --- | --- |
| 路径与地形 | projectDir、definedDir、terrainEnvironment、terrainAirHeightRange（73-76） |
| 场景 | sceneSize_XYZ、voxelSize_XZY、stepsize_surface、stepsize_height、stepsize_atmosphere、n_voxel、initialSoilTemperature（NaN 表示未配置）（79-96） |
| 气象 | meteos、meteo、startTimeNode、endTimeNode、lat、lon（100-105） |
| 观测几何与大气 | sensor、light、angles、waves、isUAVTrave、uavposes、atomconds、aeroconds（110-120） |
| 运行设置 | voxelFlags、n_pipeline=15、n_wave、n_angle、n_node、k_node、k_angle、imageSize、n_sample、maxDepth、输出开关 isImage、isProcess、isRadiationProcess、isEnergyProcess、isAlbedo、heterogeneousVoxel、periodicNeighborCount、skyboxEnabled、vegetationTemperatureMethod、shortwaveScatteringOrders（125-147） |
| 能量迭代诊断 | energyIterations、energyFinalUnconvergedStateCount、energyFinalized（150-152） |
| 稳定度与船体 | stabilityCorrection、stabilityAvailable、inverseObukhov、frictionVelocity、stabilityHeight、stabilitySensibleHeat、stabilityAeroUpdates、stabilityChange、shipWaterline、shipWaterK、shipWaterVoxels（157-170） |
| 散射与短波直散分离 | m_pScatteringStats、solarStats、solarBeamIrradiance、diffuseFractionMode、fixedDirectFraction、nodeDirectFraction、meteoMeasuredSplit、unitShortwaveShapes（171-184） |
| 流体 | fluid（FluidXml）、fluidGridSize、fluidCellCount、fluidSubsteps、fluidParameters（185-189） |
| 子 IO 与推送常量 | m_meshio、m_instanceio、m_virtualio、m_voxelio、m_surfio、m_defined、setting（VoxelLstSetting）、m_radianceAllocator（192-201） |
| GPU 小缓冲 | m_pBufferWave、m_pBufferAtomcond、m_pBufferSensor、m_pBufferLight、m_pMeteoBuffer、m_pBufferAero、m_pFluid* 共 8 个（203-218） |
| Vulkan 与管线 | 同 RaytracingIO；实际使用 m_pipelines（键为 VoxelEBStage）与 m_pipelineLayout（254-255），其余 _rt、_et、_eb 等为遗留 |

#### 5.4.4 Buffer、Descriptor、Pipeline、Command

**Buffer::createBuffer**（src/voxeleb/buffer.cpp:46）按下列顺序：规模检查（voxellimits，68-83）；光谱、固定光谱（2002 波段的材质表）、热材质、冠层（均只在非空时创建，66-121）；体元温度（缺省 298.15 K，有气象则取起始节点 Ta 加 273.15；土壤用 Tsold；冠层结构类型为 2 或 3 且 fixedTemperature 大于 0 时固定，123-212）；稠密流体网格及其元数据、D3Q19 分布函数、A/B 双缓冲与 FluidParameters UBO（214-537）；场景网格、实例与体元链接、NanoVDB、加速结构（540-632）；传感器、光源、波长、大气条件、VoxelDir（初值 0、1，634-674）；辐射场与统计缓冲（676-745）；VoxelNetRad、VoxelPnet、气动参数、气象 UBO、Raa、叶片生化与土壤与水体参数、VoxelAir、Rss（746-857）；热通量、TLAST、土壤记忆、EBState（859-968）；输出影像与 LAD（976-988）。

辐射场缓冲（voxelRads）特殊：用独立的 `nvvk::ResourceAllocatorDedicated`（VoxelebIO::m_radianceAllocator）分配，带 DEVICE_ADDRESS 用途，**不进描述符**，其设备地址写入推送常量 `setting.radianceAddress`；总字节数为 `体元数*64*8 + 体元数*64*4 + (体元数*n_wave+1)/2*8`，依次为方向辐射场、天空间隙透射率缓存、光学图像的多次散射环境（buffer.cpp:689-725）。短波诊断缓冲同样靠 `setting.scatteringStatsAddress` 访问（buffer.cpp:727-745）。

**Descriptor**：枚举 VoxelebbindingInd 编号 0 至 41（descriptor.h:31-83），与 bindingLayout.h 的 VOXELLST 段 B_* 一一对应（bindingLayout.h:79-123）；`rads`（15）仅占位，`createDescriptor` 不添加该绑定（descriptor.cpp:50-52）。同一组阶段标志，TLAS 另加 closest-hit。条件创建的缓冲（光谱、固定光谱、冠层、实例链接、体元链接、NanoVDB、波长、大气条件）缺失时抛明确异常（descriptor.cpp:110-123）。

| 绑定号 | 名称 | 类型 | 内容 |
| --- | --- | --- | --- |
| 0、1、2 | spectral、fixedSpectral、thermal | 存储缓冲 | 传感器波段光谱；2002 波段固定光谱；热材质 |
| 3、4 | tempe、canopy | 存储缓冲 | VoxelTempe、Canopy |
| 5、6、7、8 | meshLink、instanceLink、voxelLink、nano | 存储缓冲 | 链接表与 NanoVDB |
| 9 | tlas | 加速结构 | TLAS |
| 10、12 | sensor、light | UBO | SensorMatrix、LightSet |
| 11、13 | wave、atomcond | 存储缓冲 | 波长；大气直射与散射系数 |
| 14、16、17 | dir、netRad、pnet | 存储缓冲 | VoxelDir、VoxelNetRad、VoxelPnet |
| 15 | rads | 无描述符 | 经 radianceAddress 访问 |
| 18 | storage | 存储缓冲 | 输出图像 |
| 19 | meteo | UBO | Meteo |
| 20 至 26 | aerocond、raa、leafBio、soilSet、rss、air、flux | 存储缓冲 | 气动条件、Raa、叶片生化、土壤、Rss、VoxelAir、VoxelHeatflux |
| 27、28、29、30 | tLast、state、lad、waterSet | 存储缓冲 | 温度历史、EBState、LAD、水体 |
| 31 至 38 | fluidVelocityA/B、fluidScalarA/B、fluidLbmA/B、fluidDensity、fluidMeta | 存储缓冲 | 流体 A/B 双缓冲与元数据 |
| 39 | fluidParameters | UBO | FluidParameters（128 字节） |
| 40、41 | hex、soilMemory | 存储缓冲 | 异质体元参数；土壤方法 1 的长历史模态 |

**Pipeline::createPipeline**（src/voxeleb/pipeline.cpp:45）：推送常量范围 `sizeof(VoxelLstSetting)`（65-66）；stabilityCorrection 为真时 aero、evapo、budget 取带 `_most` 后缀的 spv（81、94-98）；bio 阶段由 `vegetationTemperatureMethod` 选择：0 取 biochemical.comp，其余取 biochemical_m12.comp（100-102）。周期边界特化常量与 VoxelRT 相同。任何阶段创建失败返回 false。

**Command**（src/voxeleb/command.cpp）：`submit`（992）为每个阶段录制全局内存屏障、绑定、推送 VoxelLstSetting 并提交，随后必须 waitFence；推送常量按值拷贝，所以改 `setting` 后需重新 submit 才生效。`runRT`（839）只提交 out 阶段，按图像 2D 派发。`runFluid`（866）每 32 步合并为一个命令缓冲（fluidLbm、屏障、fluidCommit、屏障）；每批首个派发前没有屏障，源码已标注此风险（command.cpp:885）。匿名命名空间里的辅助函数（46-466）负责清零短波缓冲、清零与读回 EBState、上传稳定度与船体水温（借 FluidParameters.localExchange 的 y、z、w 分量）、读回散射与太阳可见度统计、写 `diagnostics/scattering_node=<k>.json`。

`runEB`（506-834）调用链：

```
判定 noShortwaveNode = (Rin <= 1e-3 且 sza >= 90°)
  真：clearShortwaveFields → writeScatteringStats → 打印 SHORTWAVE_SKIPPED
  假：resetSolarStats → gap → readSolarStats → directVNIR
      → scatteringTrace(stage 0)                                       清零漫射吸收
      → 按光谱组循环 first=0..2000（宽度 n_jump，在 301 处强制断开）：
          scatteringTrace(stage 1 首组或 stage 4)                       天空场及间隙透射率缓存
          → 对 order=1..shortwaveScatteringOrders：
              scatteringVNIR(stage 2) → scatteringTrace(stage 3)        构造源与追踪入射场
          → readScatteringStats 累加到 CPU
      → writeScatteringStats
directTIR（当前着色器清零后返回）
evaluateAerodynamics()                                                 aero；MOST 开启时先上传 1/L
for kiter in 0..49:
    diffuseTIR → [kiter < 25] bio → evapo → clearEnergyState → budget
    → [船体水线] 更新场景水温 → [MOST] 更新 1/L 并重算 aero
    → 收敛判据：kiter >= 2 且 EBState.count == 0（MOST 还需 1/L 收敛）
收尾：diffuseTIR → [bio] → evapo → clearEnergyState → budget(energyFinalize=1) → 读回最终未收敛数
updateTp                                                               提交温度历史
```

updateL 管线会被创建，但源码中没有任何 `submit(VoxelEBStage::updateL...)`，该阶段已弃用（pipeline.cpp:97；shader/voxeleb/updateL.comp:3-6）。迭代上限 50 与生理迭代 25 为硬编码（command.cpp:655、724），未收敛时只记录 `energyFinalUnconvergedStateCount`，不会中断。

#### 5.4.5 着色器与管线对应、推送常量

| 阶段 | 着色器（shader/voxeleb） | 行数(约) | 作用 |
| --- | --- | --- | --- |
| gap | voxelrad_solar.comp | 257 | 太阳可见度，写 voxelDirs[].solar 与统计 |
| directVNIR | voxelrad_direct_VNIR.comp | 148 | 太阳直射短波吸收，写 directVrad、directPnet |
| directTIR | voxelrad_direct_TIR.comp | 149 | 已禁用，仅清零 directTrad 后返回 |
| scatteringVNIR | voxelrad_scattering_VNIR.comp | 709 | 多次散射，只用 stage 2（构造散射源，含 16 KiB 共享内存核） |
| scatteringTrace | voxelrad_scattering_trace.comp.spv，由同一源文件加 -DSCATTER_TRACE_STAGE 编译 | 同上 | 用 stage 0、1、3、4，省寄存器与共享内存 |
| diffuseTIR | voxelrad_diffuse_TIR_single.comp | 665 | 热红外漫射入射长波，写 diffuseTrad |
| bio | biochemical.comp 或 biochemical_m12.comp | 427、562 | 光合与气孔阻力，写 GPP、NPP、rss、ci |
| aero | aeresist.comp 或 aeresist_most | 701 | 空气动力阻抗 raa；MOST 版另累加 u* 与有效高度到 EBState |
| evapo | evapo.comp 或 evapo_most | 370 | 显热、潜热、热储，更新 es、cs |
| budget | budget.comp 或 budget_most | 498 | 阻尼牛顿步更新温度，统计未收敛数 |
| updateTp | updateTp.comp | 250 | 节点结束提交 TLAST 与土壤记忆 |
| updateL | updateL.comp | 150 | 已弃用，从未派发 |
| fluidLbm、fluidCommit | fluid_lbm.comp、fluid_lbm_commit.comp | 302、39 | D3Q19 BGK LBM 一步；B 缓冲提交回 A |
| out | voxelrad_image.comp | 922 | 传感器成像，写 outImage |

公共头：fluid_common.glsl（网格索引与边界）、fluid_coupling.glsl（局部气象、船体、自由对流与 MOST 函数，仅被 include）、image_environment.glsl（图像环境系数存取）、scatter_stats.glsl（散射与太阳可见度统计，仅被 include）。

VoxelLstSetting 为 128 字节，偏移由 static_assert 固定：radianceAddress 80、scatteringBandStart 88、scatteringStatsAddress 104、energyFinalize 112、scatteringOrders 116、energyIteration 120、imageEnvironment 124（structs_cg.h:162-170）。着色器侧结构名为 VoxellstSetting，字段在 VOXELLST 宏下含地址类字段（shader/settings.h:53-86）。统计记录 ScatteringOrderStats 为 32 字节，SolarVisibilityStats 为 64 字节，紧随 4 条散射记录之后（structs_cg.h:174-207）；FluidParameters 为 128 字节，其中 localExchange 的 x 为局部交换开关，y 为 MOST 开关，z 为场景 1/L，w 为船体水温（structs_cg.h:530-546；command.cpp:180-218）。

#### 5.4.6 输出文件与标准输出标记

| 路径（相对工程目录） | 产生者 | 内容 |
| --- | --- | --- |
| 影像 TIFF，后缀 `_v` 或 `_v_a` | output | 多波段，时间标签取 meteo.t |
| result_statistics.txt | run、outputTxt | 逐角度每波段均值 |
| diagnostics/terrain-environment.json | run | 地形环境输入与未求解过程声明（voxeleb.cpp:436-461） |
| diagnostics/scattering_node=k.json | runEB | 短波散射与太阳可见度统计 |
| diagnostics/physiology_node=k_T=*.json（及 .bin） | outputVoxel | 10 项生理指标统计；能量过程开启时另存逐体元二进制 |
| diagnostics/energy_balance_node=k_T=*.json | outputVoxel | 6 项闭合诊断、稳定度、太阳可见度（voxeleb.cpp:1119-1227） |
| process/voxelthermal_T=*.bin | outputVoxel | 每体元 3 个 float：向阳温度、背阴温度、向阳比例 |
| process/voxel_scene_links.bin、material_sensor_spectra.csv | outputVoxel | 首个节点一次；每体元 6 个 int32 链接 |
| process/voxelrt_T=*.bin 与 .json | outputVoxel | 辐射过程：位置 3 个、值 3 个（短波、长波、净辐射）、状态 2 个（组分编码、温度） |
| process/voxeleb_T=*.bin 与 .json | outputVoxel | 能量过程：位置 3 个、值 5 个（LE、H、G、GPP、NPP）、可选土壤剖面 TLASTNUM 个、状态 2 个 |
| process/voxelfluid_T=*.bin 与 .json | outputVoxel | 流体格：位置、风速、气温、烟、可选水汽压与相对湿度 |
| fluid_wind_H=*.tif、fluid_air_temperature_H=*.tif | outputFluidSlices | 指定高度的水平切片 GeoTIFF，无效值 NaN |
| capture_records.jsonl、surface_mask_P=*.bin | runObserver、output | 观测器模式的相机记录与热红外地表命中掩膜 |

过程文件的记录布局：radiation 的 `recordFloats = 8`，energy 的 `recordFloats = 10 + TLASTNUM（有土壤剖面时）`，由 `componentOffset + 2` 计算（voxeleb.cpp:1297、1371）；元数据 JSON 写明偏移，读取方应以 JSON 为准。元数据里土壤剖面固定写 8 层深度（voxeleb.cpp:1413-1414）。

标准输出标记（供前端或脚本解析）：PROGRESS、OBSERVATION、PROCESS、FLUID_OUTPUT、SHORTWAVE_SPLIT、SHORTWAVE_SCATTERING、SHORTWAVE_SKIPPED、ENERGY_BALANCE、RADIANCE_BUFFER；仅在对应环境变量打开时输出的有 TEMPERATURE_RANGE（HISTREAM_TEMPERATURE_DIAGNOSTICS）、PASS_TIMING（HISTREAM_VOXEL_PASS_TIMING）、GPU_STAGE（HISTREAM_GPU_DIAGNOSTICS）、PROXY_GEOMETRY（HISTREAM_VOXEL_PROXY_STATS）。

#### 5.4.7 观测器模式

HISTREAM_OBSERVER_MANIFEST 指向一个清单 JSON（格式标识 `stream3d-native-observer-v1`），run 直接转入 runObserver（observer_capture.inl:38）：校验体元数、体元边长、材质与位置行序，把保存的向阳与背阴温度及日照比例上传 GPU，只重跑 directTIR 与 diffuseTIR 刷新长波入射，不推进能量平衡；随后对每个相机记录构造透视传感器（`Geometry::createSensor` 与 `updateSensor`）、调用 runRT 与 output，并写 capture_records.jsonl。要求传感器为透视投影。

### 5.5 Vulkan 资源生命周期

| 阶段 | RayTracing | VoxelRT | VoxelEB |
| --- | --- | --- | --- |
| setup | 取设备与 eGCT 队列；`m_genCmdBuf.init`；创建 Allocator 并 init；查询光追属性；SBTWrapper.setup；BLAS 构建器 setup（raytracing.cpp:260-306） | 同左（voxelrt.cpp:104-146） | 同左（voxeleb.cpp:100-144） |
| create | 缓冲（临时 CommandPool 逐项 submitAndWait）→ 描述符池（容量 1）→ 管线、SBT → fence 与 2 个信号量 | 同左，管线为 3 条计算管线，信号量 4 个 | 同左，管线 15 条，信号量 16 个；另建独立辐射分配器 |
| run 期间 | 每块提交产生一个命令缓冲，登记 m_inFlight，waitFence 回收 | 同左 | 同左；辅助函数用临时 CommandPool；读回用临时主机可见缓冲，用完立即销毁 |
| destroy 顺序 | vkDeviceWaitIdle → Pipeline → Descriptor → Buffer → Command → SBTWrapper → BLAS 构建器 → m_genCmdBuf.deinit → Allocator.deinit（raytracing.cpp:950-973） | vkDeviceWaitIdle → Buffer → SBT → BLAS 构建器 → Descriptor → Pipeline → Command → deinit（voxelrt.cpp:509-531） | 同 VoxelRT；Buffer 内另销毁辐射分配器（voxeleb.cpp:622-642；buffer.cpp:1066-1067） |

补充说明：

- 描述符与管线布局的释放顺序：RayTracing 特意先销毁 Pipeline 再销毁 Descriptor，注释说明管线布局引用描述符集布局（raytracing.cpp:953-960）。VoxelRT 与 VoxelEB 的顺序相反（先 Descriptor 后 Pipeline），在 vkDeviceWaitIdle 之后实际无害，但与 RayTracing 不一致。
- 着色器模块在管线创建后立即销毁，成功与失败路径均如此（raytracing/pipeline.cpp:319-322；voxelrt/pipeline.cpp:119、124）。
- 三个 destroy 的返回值不可靠：Voxelrt::destroy 与 Voxeleb::destroy 在成功路径也返回 false（voxelrt.cpp:529；voxeleb.cpp:640），Engine 并不依赖这些返回值（engine.cpp:328-334）。三个 `Buffer::createBuffer` 的成功返回值同样不可靠，调用方忽略它。
- 已知的释放遗漏：VoxelRT 的 Buffer::destroy 仅在 `thermals.size() > 0` 时销毁热材质缓冲，但 createBuffer 总会创建它（含占位），热材质为空时该缓冲不会被销毁（voxelrt/buffer.cpp:396-399、92-99）。RayTracing 的 Pipeline::destroy 没有释放手工 SBT 缓冲（pipeline.cpp:331-343），默认路径由 SBTWrapper 释放，不受影响。
- 缓冲上传的同步模型：每个缓冲用独立命令缓冲 `submitAndWait`，简单但串行，网格数很多时创建 CommandPool 的开销显著（源码 DEFERRED 注释：voxelrt/buffer.cpp:163）。

### 5.6 修改指南与注意事项

1. **新增或调整 RayTracing 描述符绑定**：同时改 `src/raytracing/descriptor.h` 的枚举、`descriptor.cpp` 的 addBinding 与 makeWrite、`shader/bindingLayout.h` 的 RAYTRACING 段 B_* 与缓冲声明，并在 `buffer.cpp` 创建、销毁对应缓冲；枚举顺序即绑定号，三处必须一致。
2. **新增 VoxelRT 或 VoxelEB 描述符绑定**：同上，对应 `bindingLayout.h` 的 VOXELRT 或 VOXELLST 段；VoxelEB 的绑定号 15 被 rads 占位，不要复用；增加缓冲还需在 `voxelebio.h` 或 `voxelio.h` 加成员，并检查 `Buffer::destroy` 对该缓冲是否判空（VoxelEB 的 destroy 对多数缓冲无条件解引用）。
3. **修改推送常量字段**：RayRTSetting 同时改 `src/base/structs_cg.h:90`、`shader/settings.h:26` 和 static_assert，并在 `raytracing.cpp:updateSetting` 赋值；VoxelLstSetting 需保持 128 字节并重核 5.4.5 的偏移；VoxelRTSetting 目前没有逐字段断言，建议先补断言再改，并注意 `voxelrt/pipeline.cpp:63` 的范围按 VoxelLstSetting 声明。
4. **新增 VoxelEB 阶段**：在 `structs.h` 的 VoxelEBStage 加枚举，在 `pipeline.cpp` 的路径表加 spv；`.comp` 会被 CMake glob 自动编译；在 `command.cpp:runEB` 中调度；同步检查 PassTiming 的 24 槽数组与名称表（command.cpp:52、62-64）。VoxelRT 新增阶段需同步扩展 `command.cpp` 的 3 项计时数组（voxelrt/command.cpp:34、55）。
5. **调整短波散射阶数或光谱分组**：阶数范围 1 至 3 在读取处与 runEB 中双重检查（command.cpp:537），`orderMilliseconds` 只有 3 项、统计只有 4 条记录（command.cpp:539-540；structs_cg.h:174）；分组宽度由 spectralAccelerationWidth 控制，且在波段下标 301 处强制断开以保证 PAR 同域。
6. **修改能量平衡迭代**：上限 50、生理迭代 25、收敛下限 3 次均写死在 `command.cpp:655、724、786、794`；稳定度松弛系数 0.5 与容差公式在 `command.cpp:718、789`。修改后检查 `energyFinalUnconvergedStateCount` 的报告是否仍然合理。
7. **启用或调整 MOST 稳定度修正**：涉及 `pipeline.cpp:81`（_most 后缀）、CMake 中 `^(aeresist|budget|evapo)\.comp$` 的变体规则（CMakeLists.txt:290）、`aeresist.comp` 等着色器的 `VOXELEB_MOST` 分支，以及借用 FluidParameters.localExchange 的 y、z 分量（command.cpp:180）；它与流体 localExchange 互斥（voxeleb.cpp:213）。
8. **修改输出波段、单位或后处理**：波段名函数分别在 `raytracing.cpp:184`、`voxelrt.cpp:79`、`voxeleb.cpp:77`，各自独立实现；光学与热红外的分界各处写法不同（RayTracing 与 VoxelRT 以 2.5 微米为界并兼容 nm，VoxelEB 的 isOpticalWavelength 以 2500 nm 为界），改动时三处及着色器内的判据（如 `voxelrad_diffuse_single.comp:135`）要一并核对。
9. **调整 RayTracing 采样与自适应**：环境变量解析在 `raytracing.cpp:392-414`，特化常量在 `pipeline.cpp:190-206`，缓冲附加平面在 `buffer.cpp:167-173`，着色器侧在 `offline.rgen` 的 AdaptivePass；这四处的平面偏移（`宽*高*n_wave + 16`）必须保持一致。
10. **增加 VoxelEB 输出或过程量**：在 `outputVoxel` 内追加，注意该函数每个节点都运行且发现无效体元就抛异常；过程文件的记录布局由 `componentOffset` 推导，改变值的个数时要同步改 JSON 中的 offset 与 `recordFloats`；新增 JSON 字段尽量保持向后兼容，前端按 JSON 偏移读取。
11. **大场景与资源上限**：体元数的存储范围、派发组数检查在各 `Buffer::createBuffer` 开头（`voxellimits`），输出影像元素数受 `checkedOutputImageElements` 限制；1D 派发组数超过 `maxComputeWorkGroupCount[0]` 的问题在 VoxelRT 与 VoxelEB 都存在（源码 DEFERRED 注释），超大场景需改为 2D 派发或分批。
12. **新增求解模块或复制已有模块**：不要沿用 FIELD_BUFFER_H 等头文件保护宏与全局类名（见 5.1.4）；`Command::Command()` 的单一定义也要自带，否则会依赖 RayTracing 的定义链接。
13. **改 `.glsl` 公共头或 C++ 共享结构**：所有依赖该头的着色器会重编，但运行目录 `shader/<模块>/` 里的旧 spv 不会自动替换，应重新执行 `histream_runtime_resources` 目标，避免 C++ 新布局配旧着色器（CMakeLists.txt:176-178）。
14. **着色器目录大小写**：代码与 CMake 使用 `shader/raytracing`，源目录是 `shader/RayTracing`，在区分大小写的平台需统一。
15. **VoxelRT UAV 模式角度索引**：修改 UAV 流程时先给 `k_angle` 赋初值，否则 outputPos 的角度选择不确定（voxelrt.cpp:305-319、635）。

### 待核对条目

- 三个模块头文件保护宏与类名相同时，engine.cpp 编译与链接的实际行为（5.1.4）。
- CMake 与 pipeline.cpp 使用小写 `shader/raytracing`，在区分大小写平台的构建行为（5.1.5）。
- VoxelRT UAV 模式下未初始化的 `k_angle` 的实际取值与影响（5.3.2）。
- VoxelRTSetting 的 96 字节为按字段推算值，缺少 sizeof 的精确断言（5.3.5）。
- VoxelEB 描述符 descriptor.cpp 第 141 至 287 行（具体 makeWrite 顺序与 VK_WHOLE_SIZE 例外）仅依据文件头注释与枚举确认，未逐行核对。
- VoxelEB 的 `updateL` 管线被创建但从未派发，是否可从管线表移除（5.4.4）。

## 6. HiStream 面元模块：FacetRT / FacetEB

本章覆盖 `models/histream/src/facetrt`、`models/histream/src/faceteb`、`models/histream/shader/facetrt`、`models/histream/shader/faceteb`，以及面元结果文件（FacetBinary 格式）的写入端与读取端（`server/facet-result.mjs`、`tools/facet_result.py`、`tools/facet_document.py`）。运行流程（进程握手、退出码状态映射、逐节点计算顺序）见《运行原理说明》第 2.6–2.8 节与第 4.1–4.3 节，本章不再复述，只给出“代码在哪里、接口是什么、改哪里”。物理方程见《理论与方法》，本章不推导。

引用约定：括号内 `文件:行号` 为仓库相对路径，下列前缀为路径别名：

| 别名 | 实际路径 |
|---|---|
| FC | models/histream/src/facetrt/facetrtcore.cpp |
| FV | models/histream/src/facetrt/facetrt_vulkan.cpp |
| FVH | models/histream/src/facetrt/facetrt_vulkan.h |
| EP | models/histream/src/facetrt/energyphysics.h |
| SRT | models/histream/shader/facetrt/ |
| SEB | models/histream/shader/faceteb/ |
| FR | server/facet-result.mjs |

### 6.1 模块定位与依赖方向

FacetRT 以三角面元为单位求辐射传输；FacetEB 在其上逐时间节点迭代“辐射—能量平衡”耦合并求表面温度。两个模式共用同一个求解核心文件和同一个 Vulkan 后端类，目录 `facetrt/` 与 `faceteb/` 的壳层文件几乎对称，真正的计算全部在 `facetrtcore.cpp`。

依赖方向（上层依赖下层，不允许反向）：

```
main.cpp ──> base/engine.cpp (Engine)
               ├─ facetrt/facetrt.cpp  ─> facetrt/command.cpp ─┐
               └─ faceteb/faceteb.cpp  ─> faceteb/command.cpp ─┤
                                                               v
                         runFacetRTCore / runFacetEBCore  (facetrt/facetrtcore.cpp)
                              ├─ facetrt_vulkan.h/.cpp  (facetvk::FacetrtVulkan，GPU)
                              ├─ energyphysics.h / leafbiochemistry.h / lumpedthermal.h (纯头文件物理)
                              ├─ base/projectjson, compo, soil_tir_emissivity,
                              │  reflectance_texture, shortwave_split, utils; thirdparty/spa.h
                              └─ shader/facetrt 或 shader/faceteb 下的 *.spv（运行时按文件名加载）
```

要点：

- 壳层只做阶段状态机，不创建任何 Vulkan 资源；GPU 资源由 `runFacet*Core` 内的 `FacetrtVulkan` 局部对象创建并随函数结束析构（FC:3281，FC:6177，FV:273-276）。
- 编译开关 `HISTREAM_ENABLE_FACET`（缺省 ON）决定两个目录的 `*.cpp/*.h` 是否进入构建；关闭时仍保留 `facetrt_vulkan.cpp`，因为 `base/appsetting.cpp` 的 `--list-devices` 需要 `facetvk::rankDevices`（models/histream/CMakeLists.txt:13,131-150；models/histream/src/base/appsetting.cpp:18,362）。
- `facetrtcore.cpp` 单独加 OpenMP 编译选项（models/histream/CMakeLists.txt:406），CPU 并行统一经 `parallelRanges`（FC:117-136），线程数取环境变量 `HISTREAM_CPU_THREADS`，缺省为逻辑核数并限 32（FC:106-113）。

阅读顺序建议：先读 `facetrtcore.h` 的两个入口，再读 `facetrtcore.cpp` 的 `runFacetRTCore`（短，FC:5816-5880），把它当作 `runFacetEBCore` 的缩略版；`runFacetEBCore` 才是改动最频繁、最容易引入回归的函数。两条路径共享的稳定接口只有三个：`Scene`（输入几何与光源）、`facetvk::FacetrtVulkan`（GPU）与 `CpuGraph` 加 `solveCpu`（CPU）。改 `loadProjectScene`、`makeHemisphereDirections`、`sunDirection`、可见性图或光学组装函数会同时影响两个模式；改 RT 专属的 `writeWebResult`、`runGpuSpectral` 不影响 EB 的能量平衡。`facetrt/` 与 `faceteb/` 壳层的对称文件（buffer、descriptor、pipeline、command）差别仅在命名空间与 IO 类型，批量修改时应同时检查两侧。

### 6.2 壳层目录：src/facetrt 与 src/faceteb

#### 6.2.1 文件清单（src/facetrt）

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
|---|---|---|---|
| facetrt.h / facetrt.cpp | 42 / 82 | 模式门面，五个生命周期接口 | `class Facetrt`：`setup/upload/create/run/destroy`（facetrt.h:27-35） |
| facetrtio.h / facetrtio.cpp | 36 / 1 | 阶段共享状态 | `FacetrtIO`：`inputPath/shaderDirectory/outputPath/setting/*Ready`（facetrtio.h:22-35） |
| buffer.h/.cpp、descriptor.h/.cpp、pipeline.h/.cpp、command.h/.cpp | 各 25–48 | 阶段对象，仅置标志 | `Buffer::createBuffer`、`Descriptor::createDescriptor`、`Pipeline::createPipeline`、`Command::create/runRT/destroy` |
| facetrtcore.h | 27 | 核心入口声明 | `runFacetRTCore`、`runFacetEBCore`（facetrtcore.h:19-27） |
| facetrtcore.cpp | 9387 | 全部场景读取、可见性、辐射、能量平衡、输出 | 见 6.4 节 |
| facetrt_vulkan.h / .cpp | 628 / 3583 | Vulkan 后端 | `facetvk::FacetrtVulkan` 等，见 6.3 节 |
| energyphysics.h | 545 | EB 物理公式（无 Vulkan 依赖） | 命名空间 `facetphysics`，见 6.7 节 |
| leafbiochemistry.h | 184 | Farquhar/von Caemmerer 叶片光合 | `facetbio::LeafBiochemistry`、`farquhar`（leafbiochemistry.h:30-62） |
| lumpedthermal.h | 77 | 集总热体（内部热库） | `LumpedThermalParameters`、`solveLumpedCore`（lumpedthermal.h:27-61） |

#### 6.2.2 文件清单（src/faceteb）

| 文件 | 行数(约) | 职责 | 关键导出/类型/函数 |
|---|---|---|---|
| faceteb.h / faceteb.cpp | 43 / 85 | 门面 | `class Faceteb`，同 Facetrt 的五个接口 |
| facetebio.h / facetebio.cpp | 41 / 1 | 状态载体 | `FacetebIO`，比 FacetrtIO 多 `int exitCode{1}`（facetebio.h:39） |
| buffer/descriptor/pipeline/command | 25–52 | 阶段对象 | `Command::runEB` 保存退出码（faceteb/command.cpp:34-42） |

`faceteb/` 没有自己的物理或求解代码，`#include "../facetrt/facetrtcore.h"` 直接复用核心（faceteb/command.cpp:16）。

#### 6.2.3 生命周期与前置条件链

调用方是 `Engine`：`input()` 对 Facet 模式只记录路径并用 `facetShaderDirectory` 填入着色器目录，不读工程（models/histream/src/base/engine.cpp:168-182）；`create()` 才依次执行 `setup → upload → create`（engine.cpp:228-239）；`run()` 调 `run`（engine.cpp:290-298）；`destroy()` 逆序清标志（engine.cpp:318-323）。

```
Facetrt::setup     : inputPath、shaderDirectory 非空 -> setupReady        (facetrt.cpp:32-39)
Facetrt::upload    : setupReady -> uploadReady                            (facetrt.cpp:42-49)
Facetrt::create    : uploadReady; Buffer(upload) && Descriptor(buffer)
                     && Pipeline(descriptor 且 shaderDirectory 非空) && Command(pipeline)
                                                                          (facetrt.cpp:53-62)
Facetrt::run       : Command::runRT -> runFacetRTCore(...)==0             (command.cpp:31-38)
Faceteb::run       : Command::runEB -> exitCode=runFacetEBCore(...)       (faceteb/command.cpp:34-42)
```

约束与易错点：

- 阶段标志不代表真实资源；`FacetrtIO::setting`（`facetvk::Config`）在 `models/histream/src` 内无任何读取或写入（全仓库检索仅见声明，facetrtio.h:28），属遗留字段，真实 `Config` 在核心内由 `Scene` 重新构造（FC:3270-3278，FC:6158-6167）。
- FacetRT 的 `run` 只返回布尔，`Engine::run` 因而只会得到 0 或 1（engine.cpp:290-291）；FacetEB 返回 `exitCode`，0/1/2/3 语义见 6.6.5。
- 着色器目录查找顺序：可执行文件旁 `shader/<name>` → 当前目录 `shader/<name>` → 编译期宏 `HISTREAM_FACETRT_SHADER_DIR` / `HISTREAM_FACETEB_SHADER_DIR` → 当前目录路径兜底（engine.cpp:252-274）；Windows 上目录经 `path::string()` 转 ANSI，非 ASCII 安装路径有已登记的缺陷（engine.cpp:255，DEFERRED 注释）。
- 命令行为 `histream <eFacetRT|eFacetEB> <project.json> [output.json]`，Facet 模式不支持 `--builtin-example`（main.cpp:60-62，main.cpp:140）；退出码 2 保留给“FacetEB 有未收敛节点”，用法错误为 64（main.cpp:50-56）。

### 6.3 Vulkan 后端：facetrt_vulkan.h / facetrt_vulkan.cpp

命名空间 `facetvk`。设计：以栅格化 A-buffer（或硬件光线查询）为每个方向的每个像素生成一条沿视线的射线，射线数即面侧间交换权重；压缩成 CSR 后在 GPU 上做 Jacobi 辐射度迭代（FVH:1-19，FVH:268-279）。每面元两个 oriented side：`2f` 为几何法向侧，`2f+1` 为反面（FVH:16-17）。

#### 6.3.1 公共数据结构（FVH）

| 类型 | 含义与要点 |
|---|---|
| `Vertex{position[3], normal[3]}` | 非索引三角形列表，同一面元 3 个顶点相邻且法向相同，法向须归一化（长度 0.99–1.01，FV:878-896） |
| `Direction{x,y,z}` | 指向虚拟正交相机；各方向必须等立体角权重（FVH:88-95） |
| `SurfaceOptics{reflectance,transmittance,emission,directIrradiance}` | 逐 side 光学量，16 字节，与 `radiosity.comp` 的结构一致（FVH:99-105）；`validateOptics` 要求有限、非负、ρ+τ ≤ 1.0001（FV:2651-2671） |
| `Config` | `rasterWidth/Height`（缺省 512，核心传入 1024 或 2048）、`maxFragmentsPerPixel`（1–256）、`edgeHashCapacity`（0 = 自动）、`gpuIndex`、`periodicNeighborCount`（0–20）、`periodicSizeX/Z`、`visibilityBackend`（FVH:112-124） |
| `VisibilityBackend` | `Raster / RayQuery / Auto`；环境变量 `HISTREAM_FACET_VISIBILITY`（取值 raster、rayquery 或 auto）覆盖（FVH:73-78，FV:302-311） |
| `GraphDiagnostics` | 面元/边数、溢出计数、闭合误差 `maxClosureError`、下边界诊断、各阶段耗时、校验结果（FVH:129-175） |
| `GroundBoundary{sides,weights}` + `kInactiveSide` | 等效地面：朝上地面侧按面积加权（权重和为 1）；朝下侧 `groundCounts=0xffffffff` 表示不参与（FVH:179-188） |
| `SolveResult` | `radiosity/iterations/maxDelta/incident/groundRadiosity`（FVH:202-211） |
| `EnergyPush`（48 B） / `EnergySnapshot` / `EnergyStatistics` | GPU 能量平衡的推送常量、读回快照、统计（FVH:218-237，`static_assert` 在 FV:2808） |
| `SpectralPush`（56 B） / `SpectralAudit` | GPU 光谱求解的推送常量与审计数据（FVH:242-255，`static_assert` 在 FV:3239） |
| 异常 | `EdgeHashOverflow`（不可恢复）、`RasterLayerOverflow`（可扩层恢复）、`RayQueryOverflow`（回退光栅）（FVH:37-67） |

#### 6.3.2 类 FacetrtVulkan 的公共接口

```
initialize(Config, shaderDir)            // 建实例/设备/命令池；读环境变量（FV:291-329）
setGeometry(vector<Vertex>)              // 校验、上传、建可见性资源/管线；使旧图失效（FV:902-930）
setGroundFacets(first)                   // 索引 >= first 的面元为地面（FVH:293）
buildVisibilityGraph(directions)         // 可见性图 + 自动恢复（FV:1685-1713）
computeSunlitFraction(sunDir)            // 每 side 直射可见比例，无样本为 NaN（FV:2446-2473）
solve(optics, sky, iterations, relax)    // 固定次数 Jacobi，初值 0（FV:3034-3069）
solveAccelerated(optics, sky, init*, maxIt, batch, tol, relax)   // 分批回读 maxDelta 判敛（FV:3074-3160）
incidentIrradiance(solution, optics, sky)// GPU 已算则直接返回，否则 CPU 遍历 CSR（FV:3167-3216）
beginEnergyNode / updateEnergyInputs / evaluateEnergy / updateEnergyTemperature / readEnergyState
beginSpectral / solveSpectralBand / readSpectralOutput / readSpectralAudit / restoreSpectralOptics
supportsGpuEnergy / supportsGpuIncident / visibilityBackendName / dumpGraph / destroy
rankDevices(VkInstance)                  // 自由函数：设备筛选与排序（FV:137-182）
```

设备筛选条件：非 CPU 类型，支持 `shaderInt64`、`shaderBufferInt64Atomics`、`fragmentStoresAndAtomics`，且有图形+计算队列族；独显 1000 分、其他 100 分，加 `maxImageDimension2D/1024`，稳定降序（FV:137-182）。`gpuIndex` 越界会静默钳到最后一块 GPU（FV:450-452，DEFERRED 注释）。`shaderFloat64` 可选，缺失时不创建 `incident` 管线，也就不创建能量资源（FV:470-471，FV:1665-1678）。

#### 6.3.3 核心函数调用链

```
buildVisibilityGraph                                   (FV:1685)
 └ 循环 buildVisibilityGraphOnce                       (FV:2201)
     ├ buildRawGraph(directions, rayQuery)             (FV:1719) 清零累积缓冲 -> 每方向光栅/光线查询 -> 回读计数
     │    光栅: visibility.vert/frag -> visibility_resolve.comp
     │    光线查询: visibility_rayquery.comp (+ BLAS/TLAS, createAccelerationStructure FV:1033)
     ├ buildCsrOnGpu (graph_build.comp, FV:1948) 失败回退 buildHostCsr (FV:1916)
     ├ applyGroundBoundary (FV:191) -> 下边界与诊断
     └ createSolveResources / createSolveDescriptors / createSolvePipeline (FV:1530/1573/1628)
 捕获 EdgeHashOverflow -> 容量翻倍并 setGeometry 重建
 捕获 RasterLayerOverflow -> growRasterLayers() 后重建（不能扩则重新抛出）
 捕获 RayQueryOverflow -> fallBackToRaster 后重建
```

`growRasterLayers` 的新层数取 `max(2×当前, 上次溢出实测层数向上取 2 的幂)`，封顶 256，且片元缓冲（8 字节/片元）不得超过 `maxStorageBufferRange` 或显存预算（环境变量 `HISTREAM_FACET_RASTER_BUDGET_MB`，缺省最大 device-local 堆的 1/4）（FV:815-874）。边哈希容量：`edgeHashCapacity` 非 0 时向上取 2 的幂，自动模式为 `max(65536, 64×side 数)` 取 2 的幂，上限为 `maxStorageBufferRange/8` 内的最大 2 的幂（FV:773-810）。

`makeProjection` 构造每个方向的正交视图投影：x/y 窗口取中心块外接球，使每方向像素面积（计数权重）相同，深度范围覆盖周期副本，深度映射到 [0,1]（FV:958-1031）；`rayTieDepth` 给光线查询的深度并列容差，缺省 1e-6，外接球很大时收紧（FV:935-951）。直射可见比例沿太阳方向做 4×4=16 次亚像素抖动累加，visible/total 为比例（FV:2504-2543）。

`evaluateEnergy` 的长波 Jacobi 在没有其他求解动过乒乓缓冲时热启动 16 次扫描，否则冷启动 64 次（靠 `m_solveGeneration` 判定，FV:2961-2992）。`recordIterations` 每次扫描前清 `residual`，因此回读的 `maxDelta` 只代表一批中最后一次扫描（FV:2699-2702）。

#### 6.3.4 描述符绑定与管线对应

| 管线成员 | 着色器 | 创建位置 | 描述符集 |
|---|---|---|---|
| `m_rasterPipeline` | visibility.vert + visibility.frag | FV:1433-1435 | 可见性集：绑定 0–7 为 storage buffer（fragments、pixelCounts、edgeKeys、edgeCounts、denominator、skyCounts、stats、groundCounts），光线查询再加 8=TLAS、9=顶点缓冲（FV:1265-1293） |
| `m_resolvePipeline` | visibility_resolve.comp | FV:1506 | 同上 |
| `m_directResolvePipeline` | direct_resolve.comp | FV:1518 | 直射集：同布局，denominator/skyCounts 换成 directTotal/directVisible |
| `m_rqVisibilityPipeline` / `m_rqDirectPipeline` | visibility_rayquery.comp / direct_rayquery.comp | FV:1411-1412 | 同可见性集（含 8、9） |
| `m_graphPipeline` | graph_build.comp | FV:1980 | 12 个 storage buffer（FV:1960） |
| `m_solvePipeline` | radiosity.comp | FV:1639 | 求解集 13 个绑定：0 rowOffsets、1 edges、2 denominator、3 skyCounts、4 optics、5/6 radiosityA/B、7 residual、8 incident、9 groundCounts、10 groundSides、11 groundWeights、12 groundMean（FV:1569-1578） |
| `m_groundPipeline` | ground_mean.comp | FV:1653 | 求解集 |
| `m_incidentPipeline` | incident.comp | FV:1667 | 求解集，需 `shaderFloat64` |
| `m_energyPipeline` | energy.comp | FV:2862 | 能量集 7 个绑定：P、I、T、F、optics、incident、stats（FV:2801-2804，FV:2839-2840） |
| `m_spectralPipeline` | spectral.comp | FV:3294 | 光谱集 10 个绑定（FV:3224） |

#### 6.3.5 设备端缓冲与尺寸

全部缓冲是 device-local 的 storage buffer（带 transfer 源/目的标志），由 `createBuffer` 创建，`uploadBuffer/downloadBuffer` 经临时命令缓冲同步搬运，`endCommands` 提交后等待完成，所以所有公共接口都是同步调用（FV:556-720，FV:22）。

| 缓冲 | 尺寸 | 创建处 |
|---|---|---|
| A-buffer `m_fragments` | 像素数 × 每像素层数 × 8 B；光线查询后端只留 1 元素占位 | FV:1208-1246 |
| `m_edgeKeys` / `m_edgeCounts` | 哈希容量 × 8 B / × 4 B；空槽键为全 1（`kEmptyEdgeKey`，FV:47） | FV:1248-1251 |
| `m_denominator/m_skyCounts/m_groundCounts` | 每 side 一个 uint32 | FV:1252-1257 |
| `m_visibilityStats` | 4 个 uint32：[0] 片元溢出、[1] 哈希溢出、[2] 最深列表 | FV:1258，SRT visibility.frag:11-12 |
| `m_optics`、`m_radiosityA/B`、`m_incident` | 每 side 16 B / 4 B | FV:1542-1546 |
| `m_residual` | 1 个 uint32，存最大变化量的 float 位模式 | FV:1545 |
| `m_groundSides/Weights/Mean` | 地面朝上侧索引、归一化面积权重、4 float 均值槽 | FV:1547-1556 |
| 能量缓冲 | P：面元数×32 float；I、F：（状态,侧）×12 float；T：（状态,侧）×1 float；stats：7 个 uint32（主机可见） | FV:2812-2818 |

每个缓冲创建前检查不超过 `maxStorageBufferRange`，超限抛错（FV:1223-1236）；`DeviceCandidate` 的队列族必须同时支持图形与计算，所有命令提交到同一个队列（FV:508，FV:164-177）。

### 6.4 facetrtcore.cpp 区段地图

该文件头部注释写“约 8100 行”（FC:3-4），实际为 9387 行，以代码为准。全部内部函数在匿名命名空间，对外只有 `runFacetRTCore`（FC:5816）和 `runFacetEBCore`（FC:5890）。

| 行号区间 | 功能 | 主要函数/类型 |
|---|---|---|
| 1–231 | 头注释、`parallelRanges`、`FacetRunIndex`、常量、2.5–6 μm 太阳光谱形状 | `cpuThreadCount`、`utcTimestamp`、`mwirSolarShape`（FC:219）；常量 `kRasterSize=1024`、`kRasterLayers=64`、`kSkyDirectionCount=40`、`kIterations=64`（FC:183-200） |
| 232–540 | 数据结构 | `SurfaceParameters`（FC:240）、`FacetRTParameters`（FC:337）、`Scene`（FC:354）、`CpuGraph`（FC:488）、`RunResult`（FC:501） |
| 541–840 | 向量工具、OBJ 三角形读取与按域裁剪、独立 OBJ 补地面 | `appendTriangle`（FC:628）、`clipPolygonToPlane`（FC:651）、`loadTreeWithSoil`（FC:723） |
| 843–970 | 半球方向、太阳方向、热红外太阳项、宽带光学、Planck | `makeHemisphereDirections`（FC:843）、`sunDirection`（FC:860）、`facetRtThermalSolar`（FC:877）、`makeOptics`（FC:896）、`planckRadiance`（FC:923） |
| 972–1340 | Hapke 背景双向反射/红外发射率 | `HapkeAlbedoTable`（FC:1084）、`backgroundHapkeSpectral`（FC:1260） |
| 1343–1500 | 反射率纹理方差因子 | `finalizeReflectanceTextureFactors`（FC:1404） |
| 1502–1710 | EB 辅助物理：饱和水汽压、潜热、气孔阻抗、土壤储热记忆 | `latentHeatFlux`（FC:1520）、`leafStomatalResistance`（FC:1584）、`soilMemoryModes`（FC:1691） |
| 1712–2290 | 光学参数组装 | `makeEnergyShortwaveOptics`（FC:1754）、`makeEnergyLongwaveOptics`（FC:1849）、`makeSpectralOptics`（FC:1914）、`appendSurfaceStates`（FC:2046）、`prepareGpuSpectral`（FC:2115）、`makeStateSpectralOptics`（FC:2182） |
| 2294–3070 | CPU 保留后端 | `CpuRasterizer`（FC:2313）、`buildCpuGraph`（FC:2532）、`computeCpuSunlit`（FC:2704）、`fillUnsampledSunlit`（FC:2775）、`solveCpu`（FC:2843）、`incidentIrradianceCpu`（FC:2924）、`runCpu`（FC:3004） |
| 3071–3449 | GPU 后端（RT） | `runGpuSpectral`（FC:3141）、`runGpu`（FC:3260） |
| 3454–3729 | FacetBinary 写出 | `BinarySidecar`（FC:3454）、`NumberSlot`（FC:3534）、`writeWebResult`（FC:3563） |
| 3731–4170 | project.json → 内存 XML 适配 | `facetInputView`（FC:3787） |
| 4173–4850 | XML 工具、材质/土壤/光源读取 | `readTextFile`（FC:4173）、`firstXmlTag`（FC:4207）、`loadSurfaceParameters`（FC:4424）、`facetRTParametersFromInput`（FC:4581）、`loadShortwaveSpectrum`（FC:4741）、`configureFacetRTLight`（FC:4802） |
| 4851–5230 | OBJ、DEM、放置变换、地面三角网 | `readObjGeometry`（FC:4859）、`readDemHeightField`（FC:4955）、`readPlacements`（FC:5005）、`appendProjectGround`（FC:5160） |
| 5231–5690 | 工程场景装配 | `loadProjectScene`（FC:5231） |
| 5692–5810 | 入口辅助 | `sceneFileFromInput`（FC:5735）、`outputFileFromInput`（FC:5756）、`facetBackendFromInput`（FC:5774）、`visibilityBackendFromInput`（FC:5786）、`binaryOutputFromInput`（FC:5799） |
| 5816–5880 | `runFacetRTCore` | — |
| 5890–9385 | `runFacetEBCore`（单个约 3500 行的函数，大量 lambda） | 见 6.6 节 |

#### 6.4.1 输入链：project.json 不直接解析

核心读取工程时，`.json` 扩展名的文件经 `facetInputView` 在内存中转成旧式标签 XML，再用 `firstXmlTag`/`xmlFloat` 取值（FC:3777-3787，FC:4173-4179）。该适配层依次写 Control、Light、Sensor、Scene/Object、Optical/Thermal/Biochemistry（含 `soilSet`、`surfaceEnergy`、`canopyAero`、`nodeTimeStepsSeconds`）、Meteorology（FC:3789-4168）。`firstXmlTag` 做标签名边界检查，`ref_TIR` 不会误匹配 `ref_TIR_wet`，但不支持嵌套同名标签（FC:4204-4219）。

`facetInputView` 的步骤（FC 注释编号）：1 读取工程各节并拒绝“背景地面使用光伏材料”；2 写 Control；3 对象列表（Fire、Fog 与带 `medium` 字段的对象不是面元场景，跳过，FC:3859）；4 光学材料；5 热状态（缺省受光 305 K、背光 295 K）；6 土壤 `soilSet`；7 逐材料 `surfaceEnergy`（按材料类别分支，约 200 行，FC:3938-4130）；8 冠层空气动力叶宽（缺省 0.1）；9 可选逐节点步长，个数须等于 `meteo.end`，每个值须为 [1, 86400] 内的有限数；10 Meteorology（FC:3789-4168）。新增面元相关工程字段时，几乎总是要同时改这一函数与对应的读取函数。

#### 6.4.2 核心数据结构

- `Scene`（FC:354-385）：`vertices`（每 3 个为一面元）、`materialIndices`、`materials`、`reflectanceTextureFactors`、`leafFacetCount`（对象面元数，其后为地面，历史命名沿用“叶”）、`facetInstance/vehicleNames`、太阳角 `sunZenith/sunAzimuth`（度，缺省 30/135）、`beamNormalIrradiance`（缺省 700）与 `skyDiffuseIrradiance`（缺省 50 W m-2）、`visibilityRasterSize/Layers`、`periodic*`、`visibilityBackend`。
- `SurfaceParameters`（FC:240-331）：光学（逐波段 `reflectance/transmittance`、热红外 `tir*`、能量窗口 `energyReflectance/Transmittance`）、热学与土壤（`heatCapacityPerArea`、`soilTemperatureMethod` 0/1/2）、植被（`leaf`、`surfaceResistance`）、光伏（`pvEta25/pvGamma/pvBifaciality`）、水体、建筑墙板 `wallSlab`、船体 `ship*/waterline*`、车辆 `vehicle*/cabin*/engine*`、集总热体 `lumped*`、输出分量 `outputComponent`。
- `RunResult`（FC:501-539）：`backend`（字符串 `"Vulkan GPU"` 或 CPU 名，`writeWebResult` 据此写 `execution`，FC:3585）、`graph`、`solution`、`sunlit`、`bandRadiosity/bandStates`、分阶段耗时、光谱校验误差。
- `CpuGraph`（FC:488-496）：与 GPU 的 CSR 同构（`rowOffsets/edges/denominators/skyCounts/groundCounts/ground/diagnostics`）。对每个有效 side，`(sky + ground + 全部邻边 count) / denominator` 理论上为 1，偏差记入 `maxClosureError`（FC:2666，FVH:129-136）。

### 6.5 FacetRT 运行链

```
runFacetRTCore(inputPath, shaderDir, outputFile)                 (FC:5816)
 ├ outputFileFromInput -> 缺省 <outDir>/facetrt.json               (FC:5756-5770)
 ├ loadProjectScene 或 loadTreeWithSoil(.obj)                      (FC:5826-5828)
 ├ visibilityBackendFromInput / binaryOutputFromInput              (FC:5832-5833)
 ├ facetRTParametersFromInput + configureFacetRTLight              (FC:5834-5837)
 ├ 后端 cpu | gpu | auto (auto: GPU 异常则回退 CPU)               (FC:5843-5871)
 │    runGpu (FC:3260):
 │      initialize -> setGeometry -> setGroundFacets(leafFacetCount)
 │      -> buildVisibilityGraph(40 方向) -> computeSunlitFraction -> fillUnsampledSunlit
 │      -> 分支 a 单波段 solve + 去直射基线 solve -> lightEnhancement
 │         分支 b runGpuSpectral (GPU 生成光学量，spectral.comp)
 │         分支 c HISTREAM_FACET_SPECTRAL_BACKEND=cpu：CPU 逐波段 makeSpectralOptics + GPU solve
 │    runCpu (FC:3004) 经 runCpuWithLayerRetry 在 CPU A-buffer 溢出时扩层重跑 (FC:5846-5854)
 └ writeWebResult(resultFile, scene, result, binary)               (FC:5874)
```

输出标准输出协议：`PROGRESS<TAB>百分比<TAB>阶段`、结束时 `RESULT<TAB>文件`（FC:5839-5841，FC:5875）；异常打印 `facetrt: ...` 并返回 1（FC:5877-5880）。多波段光学量输出为表观反射率（除以 `beam·cosθs + sky`），热红外波段为光谱辐亮度 W m-2 sr-1 um-1，`quantity/units` 随波段类型变化（FC:3138-3139，FC:3433-3442）。`kMwirSolarShape` 须与 `shader/mwir_solar.glsl` 保持一致（FC:202-206）。

CPU 保留后端的调用链为 `buildCpuGraph`（FC:2532）→ `computeCpuSunlit`（FC:2704）→ `fillUnsampledSunlit`（FC:2775）→ `solveCpu`（FC:2843）/ `incidentIrradianceCpu`（FC:2924）。`buildCpuGraph` 的步骤：创建 `CpuRasterizer` 与原子计数器；逐方向光栅化后并行处理每像素的片元链并写入分片边表（`edgeShard`）；原子计数拷入 `graph` 并施加下边界；分片表摊平后按 key 排序，使图构造与线程数无关；展开为对称 CSR；做闭合性检验（FC:2534-2666 的步骤注释）。`CpuRasterizer` 用定长每像素片元列表，满即抛 `"CPU A-buffer overflow"`，由 `isCpuRasterOverflow` 识别后扩层重试（FC:434-458）。`fillUnsampledSunlit` 对无样本的 side：背向太阳取 0，否则依次取同单元均值、3×3×3 邻域均值、全局均值（FC:2775-2835）。GPU 与 CPU 两条路径的输出文件形状必须相同，核心靠同一份 `writeWebResult` 保证。

场景装配 `loadProjectScene` 要点：域尺寸 `sceneSizeX/sceneSizeY/Height`；周期邻域个数取整到完整环（4/8/12/20）（FC:5244-5251）；水平范围 ≥ 1000 m 时栅格改 2048、层数 16（FC:5254-5260）；`HISTREAM_FACET_RASTER_SIZE` 可在 512–8192 的 2 的幂内覆盖（FC:5263-5267）；对象三角形先入、`leafFacetCount` 记录对象面元数，其后追加地面（FC:5639-5648）；层数自适应并允许 `HISTREAM_FACET_RASTER_LAYERS`（64/128/256，只升不降）（FC:5651-5667）。注意 FC:5654 的 DEFERRED 注释称溢出不会自动扩层，与现行实现不符：GPU 侧有 `growRasterLayers`，CPU 侧有 `growCpuRasterLayers`（FC:441-458）。

### 6.6 FacetEB 运行链

#### 6.6.1 runFacetEBCore 区段（FC:5890–9385）

| 行号区间 | 内容 |
|---|---|
| 5897–6100 | 计时器、`MeteoStep`（FC:5914）、节点区间、`couplingIterations`（1–100，缺省 60）、`temperatureTolerance`、`temperatureRelaxation`（0.05–1，缺省 0.8）、`faceteb_run.json` 初值（FC:5979）、气象文件逐行读取（FC:6041-6079） |
| 6101–6155 | `timeToken`（FC:6101，生成 `DOYddd_hh-mm`）、`updateSolarPosition`（FC:6120，SPA） |
| 6157–6330 | Vulkan 配置、可见性图（GPU 或 CPU）、`solveRadiation/solveShortwaveWindow/incidentRadiation/computeSunlit` 四个统一 CPU/GPU 的 lambda（FC:6222-6271）、能量后端选择与影子审计开关（FC:6273-6308） |
| 6328–6840 | 状态初始化：温度数组、冠层柱 `CanopyColumn`（FC:6339）、集总体 `LumpedBody`（FC:6411）、墙板、车辆 `VehicleRuntime`（FC:6588）、土壤记忆与剖面（FC:6716-6820）、`.facet_steps/components.bin` 与 `photovoltaic_summary.csv`（FC:6758-6781） |
| 6845–7078 | 节点主循环开头：太阳位置、`computeSunlit`、首节点初始温度、受光/阴影状态面积守恒转移（`HISTREAM_THERMAL_HISTORY`，FC:6447-6458，FC:6922-6991） |
| 7080–7320 | 短波分窗口求解：按 `spectralAccelerationWidth` 分窗，窗口平均光学量求解一次，累加吸收/入射/PAR（FC:7126-7200） |
| 7323–7780 | 能量通量容器 `StateFluxes`（FC:7340）、`energyConverged`（FC:7374）、`evaluateEnergyFluxes`（FC:7427）、`updateStability`（FC:7720）、`measureEnergyResidual`（FC:7745） |
| 7780–8090 | GPU 能量路径准备：`gpuParameters`（32 float/面元）、`gpuInputs`（12 float/（状态,侧））、`beginEnergyNode`（FC:7783-7910）、`importGpuState`（FC:7913）、`verifyGpuEvaluation` 影子审计（FC:7994） |
| 8093–8230 | 耦合迭代：GPU 路径（FC:8093-8150）与 CPU 路径（FC:8156-8226） |
| 8227–8330 | 节点诊断与土壤热量闭合检查 |
| 8331–8860 | 车辆舱室（FC:8331-8780）、墙板推进（FC:8556）、船体向水体反馈（FC:8779）、光伏 CSV（FC:8846） |
| 8866–8950 | 成像波段（传感器产品）逐波段求解 |
| 8952–9130 | 节点二进制、过程产品 `writeProcessMetadata`（FC:8988） |
| 9140–9218 | `faceteb_run.json` 的 `completed`、`PROGRESS/FACET_CONVERGENCE/FACET_TIMING`、`FACET_STEP`/`FACET_ACK` 握手 |
| 9220–9385 | 总结果 `faceteb.json` 写出、`timing`、`solverDiagnostics`、退出码 |

#### 6.6.2 耦合迭代（以代码为准）

GPU 路径每轮：`model.evaluateEnergy(skyLongwave)`（phase 2 更新长波 optics → 长波 Jacobi → phase 0 算通量与残差）→ `importGpuStatistics` → 可选审计 → `energyConverged()` 成立则退出 → `updateEnergyTemperature()`（phase 1 牛顿更新）→ 启用稳定度修正时回传 `I[4]`、`I[5]` 阻抗（FC:8093-8137）。退出后再评估一次，使报告的通量与温度属于同一状态（FC:8138-8149）。CPU 路径逐面侧阻尼牛顿：`ΔT = 残差 / max(1, 斜率)` 限幅 ±5 K，温度夹在 [220 K, 360 K]（集总体用其 `lumpedMaxSurfaceK`），叶片直接用联立求解的 `temperatureStep`（FC:8174-8213）。

收敛判据：失败面元数占比严格小于 0.1 %、最大等效温度残差 ≤ 0.5 K（EP:30-51），并且集总体表面最大残差 < 2 W m-2（FC:7374-7379）。`temperatureTolerance` 只写入运行索引，不参与判据（FC:5984-5986）。

#### 6.6.3 能量后端选择

`HISTREAM_FACET_ENERGY_BACKEND=cpu`、含集总热体、或 `auto` 且设备不支持 GPU 能量内核时走 CPU 能量路径（辐射仍可用 GPU）；否则使用 GPU 能量内核，需要 `shaderFloat64` 与 `energy.comp.spv`，缺失且后端非 auto 时直接抛错（FC:6273-6308）。集总热体要求从节点 0 开始（FC:6279）。`HISTREAM_FACET_VALIDATE_ENERGY=1`（严格，遇首个不一致即停）或 `=2`（普查）开启 CPU 影子对比（FC:6291-6296）。

#### 6.6.4 FacetEB 输出产物

| 产物 | 位置/格式 | 写入处 |
|---|---|---|
| `faceteb_run.json` | 运行索引：`status`（running/completed/non_converged/failed）、`requested`、`convergence`、`completed[]`、`products[]`、`execution`、`quality`、`timing`、`solverDiagnostics`、`thermalHistory` | FC:5979，FC:9142-9155，FC:9350-9366；析构时仍为 running 则改 failed（FC:166-172） |
| `.facet_steps/components.bin` | 每面元 1 字节输出分量编号（地面固定 1） | FC:6760-6771 |
| `.facet_steps/energy_T=<token>.bin` | 每侧 `11 + 2×波段数` 个 float32：平均温度、总/短波/长波辐射度、净辐射、显热、潜热、储热、受光比例、受光点温度、阴影点温度，再接各波段（受光,阴影） | FC:8952-8982 |
| `process/faceteb_T=<token>.bin/.json`、`facetrt_T=…`、`faceteb_pv_T=…` | 过程产品；energy 型每侧 7 个 float，radiation/photovoltaic 型每侧 3 个 float；JSON 含 `recordFloats`、`dataFile`、收敛信息 | FC:8984-9054 |
| `process/facetpar_T=<token>.bin/.json` | 仅 `isRadiationProcess=1` 时写；每侧 2 个 float：入射、吸收 PAR 光子通量（umol photons m-2 s-1，400–700 nm，两侧相加得整叶值） | FC:9076-9111；检查脚本 tools/check-facet-par-output.py |
| `photovoltaic_summary.csv`、`photovoltaic_node_<n>.csv` | 光伏汇总与逐节点 | FC:6777-6778，FC:8849 |
| `diagnostics/vehicle_cabin_node=<n>.json` | 每车一行 | FC:8752 |
| `faceteb.json`（+`faceteb.bin`） | 末节点逐侧场与图诊断，格式见 6.9 | FC:9220-9313 |

#### 6.6.5 退出码

`runFacetEBCore` 返回：影子审计有不一致为 3；全部节点收敛为 0；有未收敛节点为 2（结果保留）；异常为 1（FC:9383-9385）。`FacetebIO::exitCode` 初值 1（facetebio.h:39）。服务端状态映射见《运行原理说明》第 7.1 节。

#### 6.6.6 逐节点握手

设置 `HISTREAM_FACET_STEP_SYNC=1` 时，引擎先把共享几何写入结果 JSON 位置（含 `stepRecordFloats`，FC:6782-6812），每节点结束打印 `FACET_STEP<TAB>节点<TAB>token<TAB>儒略时<TAB>天顶角<TAB>方位角`，并阻塞读取 stdin 的一行 `FACET_ACK<TAB>节点`，否则抛错（FC:9206-9217）。服务端对应实现位于 server.mjs:5130-5169；流程说明见《运行原理说明》第 2.7 节。

#### 6.6.7 节点循环内的 lambda 依赖

`runFacetEBCore` 没有拆成成员函数，节点内逻辑靠闭包共享状态，阅读与修改时按下列依赖顺序理解（调用方向自上而下）：

```
computeSunlit()  -> sunlit -> sunlitPart（对象面元两侧共用一个受光比例，地面朝下侧镜像朝上侧，FC:6905-6921）
状态转移         -> sunlitStateTransfer / applySunlitStateTransfer（温度、土壤历史、土壤剖面、墙板剖面同权移动，FC:6930-6991）
短波窗口循环     -> makeEnergyShortwaveOptics -> solveShortwaveWindow -> incidentRadiation
                    -> absorbedShortwave / absorbedDirect / incidentShortwave / PAR 累加器（FC:7126-7220）
耦合迭代         -> (GPU) beginEnergyNode -> evaluateEnergy / updateEnergyTemperature / readEnergyState
                    (CPU) makeEnergyLongwaveOptics -> solveRadiation -> incidentRadiation
                          -> evaluateEnergyFluxes -> measureEnergyResidual -> 牛顿更新 -> updateStability
节点后处理       -> 土壤闭合 -> 车辆舱室 -> 墙板 -> 船体反馈 -> 光伏 -> 成像波段 -> 节点文件 -> run index -> FACET_STEP
```

几个容易忽略的约定：

- 物体面元（`facet < leafFacetCount`）两侧共用一个温度，只检查第一侧残差；地面面元两侧各有温度，朝下（土壤侧）一侧每步复制朝上侧温度（FC:7388-7405，FC:8209-8212，SEB energy.comp:97）。
- 去直射的漫射基线解只在最后一个节点计算，仅用于 `lightEnhancement`（FC:7193-7197）。
- 受光点收到的直射通量等于该侧直射通量除以该侧受光比例，阴影点只收漫射部分（`stateValue`，FC:7406-7415）。
- 节点步长 `dTime` 优先取 `nodeTimeStepsSeconds`，其个数须恰为 `endNode`（FC:5949-5958，FC:6849）；变步长会影响集总热体累计秒数（FC:6850-6851）。

### 6.7 物理头文件

#### 6.7.1 energyphysics.h（命名空间 facetphysics）

| 符号 | 行号 | 作用 |
|---|---|---|
| `facetEnergyResidualThreshold=2.0`、`facetEnergyMaxEquivalentK=0.5`、`facetEnergyViolation`、`facetEnergyConverged`（两重载） | EP:32-51 | 收敛判据 |
| `soilEdges`（13 个层界，3 m）、`SoilResponse`、`soilResponse` | EP:57-109 | 12 层土壤向后欧拉，得 `T_i = offset_i + slope_i·Ts` |
| `sunlitStateTransfer`、`applySunlitStateTransfer` | EP:81-102 | 受光/阴影两态面积守恒转移 |
| `WallLayer/WallSlab/WallResponse/wallResponse` | EP:165-260 | 墙/屋面多层（最多 6 层×10 单元） |
| `psiM/psiH/phiH`、`Aerodynamics`、`aerodynamics` | EP:299-325 | Monin–Obukhov 相似函数与冠层阻抗 |
| `groundFreeConductance`、`groundExchange`、`inverseObukhov` | EP:409-438 | 地面自由对流修正与 1/L |
| `SolverDiagnostics`、`solverDiagnostics()` | EP:447-457 | 叶片求根未收敛原子计数（运行开始 reset，FC:6002） |
| `temperatureRoot`、`coupledLeaf`、`stomatalRoot`；`leafStomatalMaxIterations=24`、`leafStomatalLogTolerance=0.01`、`leafThermalToleranceK=0.01` | EP:462-545 | 叶温—气孔阻抗耦合求根 |

#### 6.7.2 其余两个头文件

`leafbiochemistry.h`：`LeafBiochemistry`（缺省 `vcmax25=60`、Ball–Berry 斜率 8、`type=3`，leafbiochemistry.h:30-44）和 `farquhar(...)`，输入非法抛 `std::runtime_error`；仅在 `vegetationTemperatureMethod != 0` 时由核心调用（leafbiochemistry.h:9-10，:62-80）。`lumpedthermal.h`：`LumpedThermalParameters::validate`、`averageLumpedPower`（加热窗重叠比例）、`solveLumpedCore`（向后欧拉，返回含闭合残差 `residualW` 的 `LumpedThermalStep`）（lumpedthermal.h:27-76）。

GPU 与 CPU 公式须保持一致：`energy_math.glsl` 首行注明与 `energyphysics.h` 对齐，分支与容差“刻意与 CPU 一致，改动需同步”（SEB energy_math.glsl:5,18-20）。`models/histream/src/base/energy_output.h` 仅被 VoxelEB 引用（models/histream/src/voxeleb/voxeleb.cpp:36），Facet 路径不使用。

### 6.8 着色器目录

编译规则：`HISTREAM_FACET_SHADER_FILES` 十个通用文件，faceteb 追加 `energy.comp`，facetrt 追加 `spectral.comp`；用 `HISTREAM_GLSLC --target-env=vulkan1.2` 生成同名 `.spv` 到构建目录 `shader/<kind>/`，再复制到可执行文件旁（models/histream/CMakeLists.txt:216-257，:490-508）。开发脚本 `shadersCompile.py` 使用 `glslangValidator ... --target-env vulkan1.3`，需手改 `versionName`，面向 voxeleb/voxelrt/RayTracing，不属于 Facet 正式构建（models/histream/shader/shadersCompile.py:19-44）。

| 着色器 | 行数(约) | 阶段/调度粒度 | 主要缓冲 | 对应 FV 管线 |
|---|---|---|---|---|
| visibility.vert | 66 | 顶点：按法向与视线定朝向侧，周期实例平移；最高位标记副本 | 顶点缓冲；推送 `VisibilityPush` | `m_rasterPipeline` |
| visibility.frag | 71 | 片元：`atomicAdd(pixelCounts)` 取层号，写 `{depthBits,cameraSide}`；溢出记 stats[0]、最深层记 stats[2] | A-buffer、pixelCounts | `m_rasterPipeline` |
| visibility_resolve.comp | 182 | 每线程一像素：按（深度,侧）插入排序；累加分母/天空/地面；相邻片元建哈希边（探测上限 4096，超限记 stats[1]） | edgeKeys(uint64)、edgeCounts、denominators、skyCounts、groundCounts | `m_resolvePipeline` |
| direct_resolve.comp | 86 | 每像素：中心瓦片片元累加 total，最近片元累加 visible | totalCounts、visibleCounts | `m_directResolvePipeline` |
| visibility_rayquery.comp / .glsl | 101 / 150 | 光线查询后端，枚举全部交点、去重、排序，规则与 resolve 相同；交点超过 256 记 stats | TLAS(8)、顶点(9) | `m_rqVisibilityPipeline` |
| direct_rayquery.comp | 40 | 光线查询版直射可见比例 | 同上 | `m_rqDirectPipeline` |
| graph_build.comp | 169 | 哈希表→CSR：phase 0 count、1 compact、6/7/8 前缀和、5 stats、2 scatter、3 sort、4 rowsums；256 线程/组 | 12 个绑定 | `m_graphPipeline` |
| ground_mean.comp | 60 | 单工作组归约等效地面均值，固定归约顺序 | groundSides/Weights/Mean | `m_groundPipeline` |
| radiosity.comp | 157 | 一次 Jacobi/松弛扫描，一线程一面元的两侧；`atomicMax` 记最大变化 | 求解集 | `m_solvePipeline` |
| incident.comp | 50 | 入射辐照度，double 累加（`GL_ARB_gpu_shader_fp64`） | 求解集 | `m_incidentPipeline` |
| spectral.comp（仅 facetrt） | 287 | 按 `pc.phase`：0 生成 optics/发射，4 漫射清直射，5/6 间接调度，2/3 输出 | Facets F、Materials M 等 10 个 | `m_spectralPipeline` |
| energy.comp（仅 faceteb） | 211 | 按 `pc.phase`：2 长波 optics，0 通量与残差，1 牛顿更新温度；一线程一面元 | P、I、T、F、optics、H、stats | `m_energyPipeline` |
| energy_math.glsl（仅 faceteb） | 263 | `#include` 到 energy.comp：FP64 `dexp/dlog/dpow`、饱和水汽压、Collatz、Farquhar、`thermal`、`groundExchange` | — | — |

要点：

- `shader/facetrt` 与 `shader/faceteb` 的十个通用着色器逐行比较，除文件头注释、`MAX_FRAGMENTS` 行尾注释与 `direct_resolve.comp` 一处注释外完全相同；功能改动必须在两个目录同步修改。
- 常量 `MAX_FRAGMENTS=256` 是着色器硬上限，主机 `Config::maxFragmentsPerPixel` 不得超过它（FV:50，FV:297-299）。
- `energy.comp` 的 `P[32]`、`I[12]`、`F[12]`、`stats[7]` 布局写在其文件头（SEB energy.comp:14-25）；改动需同步 `FacetrtVulkan::createEnergyResources`（FV:2806-2872）与 `facetrtcore.cpp` 的 `gpuParameters/gpuInputs` 打包（FC:7783-7894）、`importGpuState`（FC:7913-7948）。
- `spectral.comp` 的面元记录 12 float 与材料 vec4 布局写在头注释（SRT spectral.comp:15-21），改动需同步 `prepareGpuSpectral`（FC:2115-2175）。
- `visibility.vert` 与 `FacetrtVulkan::periodicTileOffset` 的 20 个瓦片偏移表须相同（SRT visibility.vert:38-47，FV:117-128）。

### 6.9 FacetBinary 输出格式与写入端

控制：`control.facetOutputFormat`（`json` 缺省或 `binary`），环境变量 `HISTREAM_FACET_OUTPUT_FORMAT` 优先；其他值报错（FC:5799-5808）。Stream Studio 的工程模型缺省写 `binary`，引擎自身缺省仍是 `json`（docs/facet-binary-output.md:16）。完整规范见该文档。

写入端：

- `BinarySidecar`（FC:3454-3511）：`.bin` 与 JSON 同目录同主名；长度大于 `kMinimumElements=1024` 的 float 数组进入旁路（`takes`，FC:3456，FC:3467）；`begin` 先补零到 16 字节对齐，再返回描述符文本 `{"file","offset","length","dtype":"float32","shape"}`；`shape` 乘积与元素数不符时退化为一维（FC:3478-3494）。
- 元数据：`format:"binary"`、`formatVersion:1`、`binaryFile`、`binaryBytes`；`binaryBytes`、`outputWriteMs`、`wallMs` 先用 18 字符宽的空白占位，写完后原位回填（`NumberSlot`、`patchNumber`，FC:3534-3555）。
- FacetRT 的 `writeWebResult`（FC:3563-3719）：字段顺序为 `backend, quantity, units, execution, quality, spectralVerification, facetCount, leafFacetCount, vertexPositions, graph, timing, iterations, maxDelta, wavelengths, sunlit, radiosity, lightEnhancement, bandRadiosity, bandStates`；`bandRadiosity` 形状 `[bands, 2n]`，`bandStates` 形状 `[bands, 2n, 2]`（FC:3705-3717）；数组中出现非有限值抛错（FC:3695）。
- FacetEB 的总结果（FC:9220-9313）：`backend, componentFile, facetCount, leafFacetCount, time, vertexPositions, graph, iterations, maxDelta, couplingIterations, temperatureDelta, converged, energyResidual…, wavelengths, stepRecordFloats, sunlit, radiosity, lightEnhancement, temperature, temperatureSunlit, temperatureShaded, netRadiation, sensibleHeat, latentHeat, storageHeat, photovoltaicPower`；EB 的 `writeArray` 不检查非有限值，NaN 会写成非法 JSON（FC:9288，DEFERRED 注释），而 FacetRT 检查。
- `vertexPositions` 形状为 `[n,3,3]`；其余逐侧数组 `[2n]`；`side = 2×面元 + 0/1`（FC:3614，FC:3708-3711）。

JSON 模式没有 `format` 字段；读取者以“字段值是含 `dtype` 的对象”判定旁路数组（docs/facet-binary-output.md:27，:48）。

### 6.10 读取端：server/facet-result.mjs 与 Python 工具

#### 6.10.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出 |
|---|---|---|---|
| server/facet-result.mjs | 677 | 双格式读取器 | `readFacetResult`（FR:651）、`sampleFacetResult`（FR:658）、`isFacetResultFile`（FR:665）、`facetResultFormat`（FR:340）、`facetResultCount`（FR:422）、`facetSidecarFiles`（FR:376）、`facetResultBytes`（FR:389）、`copyFacetSidecars`（FR:396）、`removeFacetResult`（FR:410）、`readLargeFileSync`（FR:23）、`MAX_RESULT_BYTES`、`THREE_DIMENSIONAL_FACET_SAMPLE_LIMIT` |
| tools/facet_result.py | 77 | numpy 读取 | `load(path, mmap=False)`、`_read_descriptor`、`_is_descriptor`（facet_result.py:23-66） |
| tools/facet_document.py | 32 | 文档级读取，保持 `json.load` 兼容 | `read_document(path, only=None, mmap=False)`（facet_document.py:22-32） |

#### 6.10.2 facet-result.mjs 的内部结构

```
readFacetResult(path, geometryOnly)
 ├ facetResultFormat(path): 读前 64 KB 看 "format":"binary" 或内联 vertexPositions 数组 (FR:340-350)
 ├ binary -> readBinaryFacetResult (FR:513) -> readFloatField/readSmallArray -> assembleFacetResult (FR:540)
 └ json   -> readJsonFacetResult (FR:52)：在 Buffer 上手工扫描键与数字，不转成 JS 字符串
sampleFacetResult -> sampleBinaryFacetResult (FR:593) | sampleJsonFacetResult (FR:197)
```

要点与约束：

- JSON 读取器依赖键的**写入顺序**：`searchFrom` 单向前进，数组必须按 `vertexPositions → wavelengths → sunlit → radiosity → lightEnhancement → temperature → netRadiation → sensibleHeat → latentHeat → storageHeat → photovoltaicPower → bandRadiosity → bandStates` 出现（FR:84-95，FR:143-149）。二进制读取器按键名取字段，没有该限制。
- 指标集合：`radiosity/lightEnhancement/sunlit` 必需，其余仅在存在时输出（FR:145-176）。读取器的键表不含 `temperatureSunlit/temperatureShaded`，这两个数组只能由 Python 端读到（FR:145，facet_result.py:19-20）。
- 二进制校验：`formatVersion > 1` 报错；描述符 `dtype` 只支持 `float32`；本机必须小端；`shape` 乘积须等于 `length`；`offset + 4×length` 不得越过旁路文件；旁路文件名必须是纯文件名（拒绝路径分隔符、冒号、`.`、`..`）（FR:354-373，FR:450-462）。
- 内存与体量：单文件或单数组超过 `MAX_RESULT_BYTES`（缺省 8 GiB，`STREAM_STUDIO_MAX_RESULT_BYTES`）拒绝读取（FR:16，FR:26-30，FR:490）；三维抽样最多 1 000 000 面元，抽样序号 `floor(i·N/n)`（FR:18，FR:197-209，FR:593-601）。
- 二进制元数据按 `(size, mtimeMs)` 缓存最近 8 个（FR:353-366）。
- 被调用：server.mjs 以别名导入（server.mjs:41-45）；`tools/export-faceteb-frames.mjs`、`tools/case005/export_case005.mjs` 调用 `copyFacetSidecars`；测试为 `tools/test-facet-binary-reader-20261126.mjs`。

#### 6.10.3 Python 读取器

`load` 返回 dict：描述符字段转 numpy（`mmap=True` 时为 `np.memmap`），JSON 模式下长度大于 1024 的数字列表转 `float32` 数组，并把 `vertexPositions` 重塑为 `(n,3,3)`、`bandRadiosity` 为 `(bands,s)`、`bandStates` 为 `(bands,s,2)`，使两种格式结果可直接比较（facet_result.py:36-66）。描述符 dtype 支持 `float32/int32/float64`（facet_result.py:29），比 Node 读取器宽。常量 `_PER_SIDE` 仅在文件内定义，`load` 未引用（facet_result.py:19，待核实是否有外部使用）。`read_document` 对 JSON 格式等同 `json.load`，对二进制格式把描述符替换为 numpy 数组，`only` 可限定只读部分字段（facet_document.py:22-32），被 `tools/plot-faceteb-day.py`、`tools/compare-facet-outer-stomata.py`、`tools/regression/compare.py`、`tools/case005/*` 使用。

#### 6.10.4 字段对应关系（写入端 ↔ 两个读取端）

| JSON 字段 | 写入处 | 形状 | facet-result.mjs | facet_result.py |
|---|---|---|---|---|
| vertexPositions | FC:3614，FC:9248 | [n,3,3] | 必需，`facetCount×9` 个 float（FR:135，FR:519） | 重塑为 (n,3,3)（facet_result.py:58-59） |
| wavelengths | FC:3707，FC:9299 | 小数组，内联 | `readDynamic` / `readSmallArray`，过滤非正值（FR:140，FR:151） | 保持列表 |
| sunlit、radiosity、lightEnhancement | FC:3708-3711，FC:9302-9304 | [2n] | 三者必需（FR:160-170） | 数组 |
| temperature、netRadiation、sensibleHeat、latentHeat、storageHeat、photovoltaicPower | FC:9305-9312 | [2n]（仅 EB） | 存在才输出 | 数组 |
| temperatureSunlit、temperatureShaded | FC:9306-9307 | [2n]（仅 EB） | 不读取 | 数组（facet_result.py:19-20） |
| bandRadiosity | FC:3713 | [bands,2n]（仅 RT 多波段） | `surfaceCount×max(1,bands)` | 重塑 (bands,s) |
| bandStates | FC:3716 | [bands,2n,2]（仅 RT） | `facetCount×4×max(1,bands)`（FR:149，FR:532） | 重塑 (bands,s,2) |
| graph、timing、execution、quality、spectralVerification | FC:3582-3668，FC:9261-9266 | 对象，内联 | 不读取 | 进入 `meta` |

两个读取端对 `bandStates` 的元素总数约定一致（`facetCount×4×bands`），但 Python 端的重塑假设最后一维是（受光,阴影）两态，与写入端 `{bands, sides, 2}` 一致（FC:3716）。

#### 6.10.5 Spectral 求解 phase 对照（GPU 光谱）

`FacetrtVulkan::solveSpectralBand` 通过 `SpectralPush::phase` 驱动 `spectral.comp`（FV:3319-3377）：

| phase | 含义 | 调用位置 |
|---|---|---|
| 0 | 由材料光谱与背景模型生成 optics 与发射 | 每波段开始（FV:3339-3342） |
| 4 | 漫射求解：清直射项与光学波段光束项（仅 band 0） | `diffuse=true` 时代替 0（FV:3339） |
| 5 / 6 | 加速模式：初始化 / 更新间接调度与收敛状态，共 8 批，每批 8 次扫描 | FV:3357-3366 |
| 2 | 输出物理辐射度，并拆出亮/阴状态 | 求解后（FV:3376） |
| 3 | 漫射输出：直射分量 = 总输出 − 漫射输出，写入输出缓冲第 3 段 | `diffuse=true`（FV:3376） |

输出缓冲布局（`n = 2×面元数`）：`[band·n + s]` 辐射度；`[bands·n + 2(band·n+s) + {0,1}]` 亮/阴状态；`[3·bands·n + s]` 漫射模式直射分量，总长 `(3·bands+1)·n` 个 float（FV:3222-3223）。

### 6.11 环境变量与控制字段速查

| 名称 | 取值/缺省 | 作用位置 |
|---|---|---|
| `HISTREAM_FACET_BACKEND` / `facetBackend` | cpu、auto，其余为 gpu | FC:5774-5782 |
| `HISTREAM_FACET_VISIBILITY` / `control.facetVisibility` | raster、rayquery、auto | FV:302-311，FC:5786-5795 |
| `HISTREAM_FACET_VALIDATE_VISIBILITY` | 非 0 即双后端对比 | FV:312-314 |
| `HISTREAM_FACET_GRAPH_BUILD`、`HISTREAM_FACET_VALIDATE_GRAPH` | gpu/cpu；1 校验 CSR 逐位一致 | FV:315-325 |
| `HISTREAM_FACET_FORCE_NO_RAYQUERY` | 测试钩子 | FV:337-339 |
| `HISTREAM_FACET_RASTER_SIZE`、`HISTREAM_FACET_RASTER_LAYERS`、`HISTREAM_FACET_RASTER_BUDGET_MB` | 512–8192；64/128/256；MB | FC:5263，FC:5658，FV:817 |
| `HISTREAM_FACET_SPECTRAL_BACKEND`、`HISTREAM_FACET_VALIDATE_SPECTRAL` | cpu；1 | FC:3323，FC:3149 |
| `HISTREAM_FACET_ENERGY_BACKEND`、`HISTREAM_FACET_VALIDATE_ENERGY` | cpu；1/2 | FC:6276，FC:6291 |
| `HISTREAM_FACET_STEP_SYNC` | 1 开启逐节点握手 | FC:6782 |
| `HISTREAM_FACET_OUTPUT_FORMAT` | json/binary | FC:5801 |
| `HISTREAM_THERMAL_HISTORY` | remap（缺省）、legacy、mixed | FC:6453-6457 |
| `HISTREAM_FACET_DUMP_GRAPH`、`HISTREAM_FACET_HAPKE_AUDIT` | 诊断 | FC:3016，FC:3074 |
| `HISTREAM_CPU_THREADS` | 由 Engine 从 `control.cpuThreads` 设置 | FC:104-113，engine.cpp:160-164 |

### 6.12 测试与辅助脚本

| 文件 | 类型 | 覆盖内容 |
|---|---|---|
| tools/test-facet-lumped-thermal.cpp | C++，CMake 目标 `histream_lumped_thermal_tests`（EXCLUDE_FROM_ALL，仅依赖头文件） | `solveLumpedCore/averageLumpedPower` 的解析解、能量闭合、输入校验（models/histream/CMakeLists.txt:534-536） |
| tools/test-facet-energy-convergence.cpp | C++，独立可执行 | `facetEnergyViolation/facetEnergyConverged` 的阈值边界与溢出 |
| tools/test-facet-nested-budget.cpp | C++ | `coupledLeaf/temperatureRoot` 迭代预算与早停 |
| tools/test-facet-sparse-aerodynamics.cpp | C++ | `aerodynamics()` 稀疏冠层阻抗回归 |
| tools/test-facet-collatz.py | Python | 按函数签名从 `facetrtcore.cpp` 截取 `saturationVaporPressure/quadraticRoot/leafStomatalResistance` 拼成测试程序，需 `cl` |
| tools/check-facet-par-output.py | Python | PAR 过程产品格式与物理边界 |
| tools/test-facet-binary-reader-20261126.mjs | Node | 两种格式的读取器一致性 |
| tools/export-faceteb-frames.mjs、tools/plot-faceteb-day.py、tools/compare-facet-outer-stomata.py | 辅助 | 导出节点温度 TIFF、整日温度图、外层气孔比较 |

注意 `test-facet-collatz.py` 依赖函数文本边界（按花括号配平截取），改动这些函数的签名或把它们移出 `facetrtcore.cpp` 会使测试失效。

### 6.13 待核实清单（本章）

- `energyphysics.h` 与 `energy.comp` 公式逐项一致性只见两方头注释声明（EP:17，SEB energy_math.glsl:5,20），未逐式核对。
- `CpuRasterizer`（FC:2313-2515）、`buildCpuGraph`（FC:2532-2700）、`runCpu`（FC:3004）仅读到头部与步骤注释，内部细节待核实。
- `loadProjectScene` 对象块（FC:5295-5640）、车辆舱室（FC:8331-8780）、船体与墙板（FC:6459-6700，FC:8556-8846）仅按注释概述，未逐行核对。
- `shadersCompile.py` 是否曾用于 facet 着色器待核实（其 `versionName` 缺省为 voxeleb）。
- `Config::edgeHashCapacity` 等字段由 `Scene` 构造的完整路径在 `runCpu` 分支下的行为未核对。

### 6.14 修改指南与注意事项

1. 新增或修改工程字段（如新的材质参数）：先在 `facetInputView` 中输出对应标签（FC:3787-4170），再在 `loadSurfaceParameters`/`facetRTParametersFromInput` 或 EB 的参数读取处用 `firstXmlTag/xmlFloat` 取值（FC:4424，FC:4581），最后加入 `SurfaceParameters`（FC:240）；缺省值要与旧工程一致。
2. 新增 FacetEB 过程输出或节点产物：在节点循环的“步骤 N”写出（FC:8952），并向 `runIndex.value["products"]` 登记（FC:9002）；若改变 `energy_T=*.bin` 记录宽度，同步 `stepRecordFloats`（FC:6808，FC:9300）与服务端出图读取（server.mjs，见《运行原理说明》第 2.7 节）。
3. 改动能量平衡通量公式：CPU 在 `evaluateEnergyFluxes`（FC:7427）与 `energyphysics.h`，GPU 在 `energy.comp` 与 `energy_math.glsl`；两侧必须同步，并用 `HISTREAM_FACET_VALIDATE_ENERGY=1` 审计（FC:6291，退出码 3）。
4. 修改 `P/I/F/stats` 布局：同步 `energy.comp` 头注释、`createEnergyResources` 的缓冲尺寸（FV:2813-2818）、`beginEnergyNode` 的长度校验（FV:2904-2910）、核心打包与导入（FC:7783，FC:7913）。
5. 新增或修改着色器：在 `shader/facetrt` 与 `shader/faceteb` 两处同步；新文件要加入 `HISTREAM_FACET_SHADER_FILES`（models/histream/CMakeLists.txt:216-226）并在 `FacetrtVulkan` 中创建管线；`.spv` 缺失时光线查询、GPU 图构建、能量会按各自规则回退或报错（FV:371-372，FV:1957-1958，FC:6307-6308）。
6. 修改推送常量：`VisibilityPush`、`SolvePush`、`EnergyPush`（48 B）、`SpectralPush`（56 B）与 GLSL 中的 `push_constant` 块逐字段一致；`EnergyPush` 的 `reserved` 在着色器里实际存风速（FVH:216-217，SEB energy.comp:47）。
7. 改变可见性方向数或栅格尺寸：方向数 40 来自 `kSkyDirectionCount`（FC:195），GPU 计数器是 uint32，`FV:1212-1216` 对 `像素数 × 2 × 40` 做溢出保护；放大栅格需复核该保护与 `maxStorageBufferRange`。
8. 调整 A-buffer 层数策略：GPU 在 `growRasterLayers`（FV:841），CPU 在 `growCpuRasterLayers`（FC:441，上限 128 层且 12 B/片元不超过 2 GiB）；两者上限不同（256 与 128），着色器上限 256。
9. 增加结果数组：在写入端用 `writeArray`（FC:3674，FC:9279）并确认形状；在 `server/facet-result.mjs` 的 `metricKeys`/`metricDefinitions`（FR:145，FR:160）与 `readBinaryFacetResult` 的键表（FR:528）补充；JSON 读取器要求键顺序，新数组必须写在已有数组之后；同步 `tools/facet_result.py` 的 `_PER_SIDE` 和 `docs/facet-binary-output.md`。
10. 增加 `FacetRT` 的新退出状态：`Command::runRT` 只返回布尔（command.cpp:31-38），需仿照 `FacetebIO::exitCode` 增加字段，并同步 `Engine::run`（engine.cpp:290-298）、`main.cpp` 与 `server.mjs` 的 `HISTREAM_EXIT` 映射。
11. 新增 Facet 目录壳层文件：`CMakeLists.txt` 用 `file(GLOB src/facetrt/*.cpp src/faceteb/*.cpp)`，需重新配置 CMake 才会被收录（models/histream/CMakeLists.txt:132-137）。
12. 修改多波段输出语义：光学波段为表观反射率、热红外波段为辐亮度，`quantity/units` 由是否含热红外波段决定（FC:3433-3442），前端标签依赖 `quantity == "spectral radiance"`（FR:159，FR:257）。
13. 修改地面与下边界：`applyGroundBoundary`（FV:191-260）、`radiosity.comp/incident.comp` 的 `incoming` 公式、`ground_mean.comp`、CPU 的 `solveCpu/incidentIrradianceCpu`（FC:2843，FC:2924）四处公式要一致，`kInactiveSide` 哨兵不要当作计数。
14. 易错：`firstXmlTag` 不处理嵌套同名标签（FC:4204-4219）；`FacetrtIO::setting` 是死字段，不要指望改它影响求解；`gpuIndex` 越界静默钳位；EB 的 JSON 数组不检查 NaN；`facetrtcore.cpp` 头注释行数与部分 DEFERRED 注释已过时，改代码前以实际函数为准。
15. 新增第三种面元模式：复制 `facetrt/` 壳层并改命名空间与 IO 类，在 `engine.h/.cpp` 增加 `Mode`、成员与 `input/create/run/destroy` 分支及 `facetShaderDirectory` 的目录名（engine.cpp:63-75，engine.cpp:168-182，engine.cpp:228-239，engine.cpp:290-298），在 `main.cpp` 的模式白名单与用法串登记（main.cpp:60，main.cpp:130），在 CMake 的 `FACET_KIND` 循环与运行时复制列表加入新目录（models/histream/CMakeLists.txt:227，:492），并在 `server.mjs` 的模式表与退出码映射补充；求解函数应复用 `facetrtcore.cpp` 的入口，不要再复制一份核心。

## 7. STREAM 影像引擎：models/stream

本章是 `models/stream` 的代码模块参考。运行流程（配置到产品的时序、服务端队列）见《运行原理说明》第 5 章（5.2 命令行、5.5 强迫数据、5.6 运行流程、5.7 反演与同化），公式推导见《理论与方法》（§0.2、§2.2、§3、§7、§9.2、§15），本章只说明“代码在哪里、接口是什么、改哪里”。

引用约定：括号内 `文件:行号` 均相对 `models/stream/`，行号为 2026-11-26 读取的实际行号，随后续修改会漂移。未读到或未核对的内容标“待核实”。

### 7.1 模块总览与依赖方向

STREAM 是一个独立可执行文件 `stream`（Windows 为 `stream.exe`），一次运行对应一个过境时刻，输出整景光学反射率、热红外亮温与方向温度（README.md:3）。它与 HiStream 三维引擎共用 vcpkg 与运行库目录，但源码互不引用（CMakeLists.txt:7-9）。

| 目录 | 职责 | 对外形态 |
| --- | --- | --- |
| `physics/` | 纯计算：光学、冠层闭合、节点辐射度、短波、能量平衡、土壤热、太阳位置 | 头文件声明加模板；不读写文件（读系数表的 `readOptCoeff` 除外） |
| `scene/` | 栅格读写（GDAL）与地类参数表 | `Grid`、`Field`、`ClassTable` |
| `forcing/` | 气象强迫：元数据、数据源抽象、ERA5 网格包、站点 CSV、时间重采样、像元插值 | `ForcingSource` 接口、`PixelForcing` |
| `app/` | 配置解析、主流程、热红外观测算子、反演、同化、命令行 | `runImage`、`readImageConfig`、`runInversion`、`runAssimilation` |
| `classes/` `defined/` | 地类表 `classes.json`、PROSPECT/BSM 系数与参考太阳谱 | 构建时复制为运行目录的 `stream-defined/` |
| `tests/` | 梯度回归测试 `jacobian_check.cpp` | 独立目标，不进默认构建 |

依赖方向（只列实际 `#include` 与调用得到的边，括号为证据）：

```
app  ──►  forcing  ──►  physics   (pixel_forcing.h:19 包含 physics/energy.h；pixel_forcing.cpp:24-25)
 │          │
 │          └────────►  scene/raster.h  (era5.cpp:26，station_source.cpp:26，forcing_metadata.cpp:25)
 ├────────►  scene    ──►  physics   (scene/classes.h:14-15)
 └────────►  physics                (app/image.cpp:41-50)
physics 不包含 forcing、scene、app 的任何头文件（grep "#include \"../" 在 physics/ 下无命中）
```

由此得到三条结构性事实：其一，`physics/` 是叶子层，`tests/jacobian_check.cpp` 只链接其中五个 `.cpp` 即可独立运行（CMakeLists.txt:48-54）；其二，`forcing` 只通过 `physics/energy.h` 的 `Forcing` 结构向能量平衡交付数据（forcing/pixel_forcing.h:19），能量平衡对数据源一无所知；其三，热红外观测算子放在 `app/thermal_view.*` 而不是 `physics/`，因为它同时依赖地类表（`ClassParameters`）与配置（`ImageConfig`）（app/thermal_view.h:23-28）。

自动微分（`Dual<N>`，别名 `Ad`，8 维）的使用位置是本引擎最重要的横切约束，汇总如下，细节分见各节：

| 位置 | 做法 | 依据 |
| --- | --- | --- |
| `physics/dual.h` | 定义 `Dual<N>`、`ad::` 数学函数，`kAdSize = 8` | physics/dual.h:29-75 |
| `canopy`、`radiosity`、`shortwave`、`energy`、`heat_conduction` | 核心写成标量类型模板，显式实例化 `double` 与 `Ad` | canopy.cpp:263-265；radiosity.cpp:534-549；shortwave.cpp:359-365；energy.cpp:680-686；heat_conduction.cpp:166-167 |
| `soil_emissivity.h`、`brightness.h`、`hapke.h` 的表封装 | 提供 `T` 版本函数 | soil_emissivity.h:88-101；brightness.h:46-57；thermal_view.h:64-70 |
| `app/thermal_view.cpp` | `thermalPixel<Ad>`、`thermalWeightsT<Ad>` | thermal_view.cpp:386-393 |
| `app/assimilation.cpp` | 在 `evaluate` 中以 `Ad::seed` 播种控制量并调用上述算子 | assimilation.cpp:436-516 |
| `app/image.cpp` 的 `ebSolver` | 用 `energyResiduals<Ad>` 取 ∂R/∂T、∂R/∂x，做隐式微分 | image.cpp:930-1036 |
| 不用 `Ad` | `solveEnergyNode`（只有 `double`）、`optics`（`float`）、`spa`、`inversion.cpp`（解析导数 `planckDerivative`） | energy.cpp:698；inversion.cpp:76 |

### 7.2 根目录文件、构建与数据

| 文件 | 行数(约) | 职责 | 关键内容 |
| --- | --- | --- | --- |
| `CMakeLists.txt` | 117 | 构建 `stream` 与可选测试目标，部署数据与运行库 | 目标 `stream`（CMakeLists.txt:11-34）、`stream_jacobian_check`（:48-62）、POST_BUILD 复制 `defined/` 与 `classes.json` 到 `stream-defined/`（:84-91） |
| `README.md` | 222 | 命令行、`stream-image-config v2`、能量平衡参数、QA 位表、输出清单 | 以用户视角为主；与代码不一致时以代码为准 |
| `PORTING.md` | 77 | 物理模块来源（TiRT/TIRTEB）对照与有意差异 | 模块对照表（PORTING.md:7-18）、与 TIRTEB 的有意差异（:42-51） |
| `LICENSE`、`physics/LICENSE-MWRTM` | 73、21 | 前者为 PolyForm Noncommercial；后者为 MIT 声明 | `physics/LICENSE-MWRTM` 与 `stream_core.hpp` 微波部分的关系待核实 |
| `classes/classes.json` | 695 | IGBP 地类参数表 | `kind = stream-classes`、`schemaVersion = 1`；地类码 0–17、18（填充）、255（填充） |
| `defined/optipar_new.csv` | 2002 | PROSPECT/BSM 系数，表头加 2001 行 | 18 列：wl、nr、Kab、Kca、Ks、Kw、Kdm、phiI、phiII、KcaV、KcaZ、Kant、KcaV2、phi、GSV1-3、nw |
| `defined/Esun_.dat`、`Esky_.dat` | 2162 | 参考太阳直射/天空谱，单列文本 | 只取前 2001 个值（image.cpp:107-119） |

构建要点：C++20；MSVC 下开启 `/openmp /utf-8 /bigobj` 并屏蔽数个警告，其他平台 `find_package(OpenMP REQUIRED)`（CMakeLists.txt:38-44）；GDAL 与 netCDF 先走 CMake 包配置，失败再退到模块或 pkg-config（:67-78）；运行库与 PROJ 库仅在 `WIN32` 且定义了 `VCPKG_INSTALLED_DIR` 时复制（:92-116）。新增 `.cpp` 必须手工加入 `add_executable(stream …)` 列表（:11-34），否则链接失败；`stream_jacobian_check` 的源文件列表是独立的（:48-54）。

### 7.3 physics/：纯计算核心

#### 7.3.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出、类型、函数 |
| --- | --- | --- | --- |
| `types.h` | 66 | 共用简单结构 | `Canopy`、`CropRows`、`LeafOptics`、`SoilOptics`、`OptCoeff`（types.h:27-62） |
| `constants.h` | 47 | 常数与 LAI 有效性 | `kStefanBoltzmann = 5.670374419e-8`（:30）、`kMaxLai = 20`、`validLai`（:40-41）、`kSpectralCount = 2001`（:44） |
| `dual.h` | 165 | 前向自动微分 | `Dual<N>`、`Ad`、`kAdSize`、`ad::exp/log/sqrt/pow/min/max/clamp`（:29-162） |
| `optics.h/.cpp` | 31 / 217 | PROSPECT 叶片、BSM 土壤 | `readOptCoeff`、`prospect`、`bsm`、`calctav`、`expint`（optics.h:20-29） |
| `canopy.h/.cpp` | 44 / 267 | 均匀冠层四组分比例与方向反射率 | `canopyFractionsT`、`sphericalCoefficients`、`canopyReflectanceT`（canopy.h:26-42） |
| `radiosity.h/.cpp` | 104 / 551 | hom3 节点辐射度 | `HomCanopyT<T>`、`homAbsorbed`、`homThermalScatter`、`homBeamMultiple`、`homSkyMultiple`、`homLongwave` |
| `shortwave.h/.cpp` | 96 / 369 | 太阳谱分组、直散分配、各组分短波吸收 | `SolarBins`、`makeSolarBins`、`absorbShortwave`、`shortwaveComponents<T>`、`requestedDiffuseFraction`、`admissibleDiffuseFraction` |
| `energy.h/.cpp` | 171 / 895 | 四组分能量平衡 | `solveEnergyNode`、`energyInputs`、`energyResiduals<T>`、`johansenSoilThermal<T>`、结构 `EnergySurface`、`Forcing`、`EnergyState`、`EnergyDiagnostics` |
| `heat_conduction.h/.cpp` | 56 / 232 | 土壤热通量 G：7 h 历史卷积、2 m 土柱 | `surfaceHeatFluxT`、`depthHeatFlux`、`initializeColumn`、`advanceThermalState`、`SoilThermalState` |
| `soil_emissivity.h` | 103 | 土壤发射率随含水量 | `soilWetWeight(T)`、`soilSaturatedEmissivity`、`soilEmissivityAtMoisture(T)`（:51-101） |
| `hapke.h` | 103 | 粗糙土壤方向发射率 | `hapkeEmissivity`、`hapkeAlbedoForHemispherical`、`SoilEmissivityTable`（:31-101） |
| `brightness.h` | 59 | Planck 与逆 Planck | `kPlanckC1/C2`、`planck`、`inversePlanck`、`planckT`、`inversePlanckT` |
| `observation_errors.h` | 58 | 观测协方差求精度矩阵 | `observationPrecision`（Cholesky，:25），仅被 `assimilation.cpp` 使用 |
| `spa.h/.cpp` | 148 / 1097 | NREL SPA 太阳位置 | `spa_data`、`SPACalc::spa_calculate`（spa.h:27-146） |
| `effective_lai.h/.cpp` | 37 / 205 | 垄行与树冠有效 LAI | 已编译、无调用者（effective_lai.cpp:4-5、27） |
| `stream_core.hpp` | 199 | StreamEarth 观测核心的孤立副本 | 命名空间 `stream_core`，无任何包含者（stream_core.hpp:17-18） |

#### 7.3.2 公共类型、常数与自动微分

`types.h` 中 `Canopy` 保存 LAI、冠层高度、叶片投影 G、热点、叶宽与 IGBP 码；`treeStand`、`canopyHeight`、`crownShape` 与 `CropRows` 目前不参与计算（types.h:19-47）。`LeafOptics` 含 PROSPECT 的 cab/cw/cdm/cs/n 与热红外反射 `reflTir`、透射 `transTir`，发射率为 `1 − reflTir − transTir`（types.h:49-54）。PROSPECT 参数的物理单位待核实。

`constants.h` 的两个“故意保留”的点：`kRad = 0.01745329` 比 π/180 略短，为与原内核逐位一致而保留，不要改精确（constants.h:19-22）；`kStefanBoltzmann` 全引擎统一一个值，EB、辐射度、热红外成像与强迫长波共用（:30）。`validLai` 规定有效 LAI 为有限且在 [0, 20]，nodata、负值与超限值都按无效输入处理（:40-41）。

`dual.h` 的约束：`Dual<N>` 只带一阶导数；分支与限幅按“值”选择，导数取被选中分支（限幅处导数为 0）；`sqrt/cbrt/pow` 在 0 处导数取 0 以避免 inf/NaN；`ad::` 函数对 `double` 的行为与 `std::` 完全一致，因此 `double` 实例化逐位复现原正向代码（dual.h:9-13、136-138）。`kAdSize = 8` 是同化控制量的上限，`assimilation.cpp` 用 `static_assert(kMaxControls <= kAdSize)` 保护，当前实际使用 7 个（assimilation.cpp:66-67）。

#### 7.3.3 光学与冠层链：optics、canopy、radiosity、shortwave

调用关系（文字版）：

```
optics::prospect(coeff, LeafOptics)  -> 叶片 R/T（1 nm，float）         (optics.cpp:113)
optics::bsm(coeff, SoilOptics, SMC)  -> 土壤反射率（k=0..6 水膜混合）    (optics.cpp:176)
canopyReflectanceT(lai, hotspot, 几何, Rleaf, Tleaf, Rsoil, fdiff, sunlit)  (canopy.cpp:218)
 ├─ canopyFractionsT        四组分可见比例（Beer 间隙率 + 热点 + 两层光照叶） (canopy.cpp:159)
 ├─ sphericalCoefficients   64 点 Gauss 的球面叶片一次散射 ks/ko/sob/sof     (canopy.cpp:193)
 └─ HomCanopyT(lai) → homBeamMultiple / homSkyMultiple   多次散射           (canopy.cpp:240-250)
```

`radiosity.h` 的 hom3 系统固定为 3 个节点（土壤、下层冠层、上层冠层）与 5 个状态（3 个出射加 2 个冠层向下半部分），常量 `kHomNodes = 3`、`kHomStates = 5`（radiosity.h:32-33）。`HomCanopyT<T>` 的构造函数由 LAI 求 τ = G·LAI/2、节点容量（土壤 1，冠层各 4τ）、漫射截获 X(τ) = 1 − 2E3(τ) 的交换因子、逃逸概率与“到达矩阵” `arrive[状态][2×节点个数]`（radiosity.cpp:211-265）；`split` 给出垂直光束到达各节点的份额（:272-281）。该接口的使用者有四类：短波吸收（`homAbsorbed/homAbsorbedPair`）、能量平衡的长波（`homLongwave`，radiosity.cpp:463-530，被 `energy.cpp:466` 调用）、热红外有效发射率的多次散射项（`homThermalScatter`，被 `thermalWeightsT` 调用）、光学方向多次散射（`homBeamMultiple/homSkyMultiple`）。`SameAs<T>`（radiosity.h:36）是非推导上下文别名，让 `T` 只由 `HomCanopyT<T>` 推导，调用时标量实参可隐式转换，写新调用时若遇模板推导失败先检查这一点。

`shortwave.h` 把“请求的漫射份额”与“允许的漫射份额”分成两个函数：`requestedDiffuseFraction` 在配置值落在 [0,1] 时直接采用，否则按 Erbs 公式由晴空指数反算，太阳在地平线以下取 1（shortwave.cpp:112-129）；`admissibleDiffuseFraction` 再叠加晴空 DNI 上限，超出的直射转为漫射（:134-147）。`absorbShortwave` 是 `double` 封装并附带诊断量，内部调用 `shortwaveComponents<double, std::vector<float>>`（:154-）；同化与 `ebSolver` 直接调用 `shortwaveComponents<Ad, std::vector<Ad>>`，并把土壤光谱逐 bin 变成带含水量斜率的 `Ad` 向量（image.cpp:978-988）。

#### 7.3.4 能量平衡 energy

公共接口（energy.h）：

```
EnergyDiagnostics solveEnergyNode(const EnergySurface&, const Forcing&, const ShortwaveAbsorption&,
                                  double szaDeg, bool sunlit, double dtSeconds,
                                  EnergyState& state, bool bracketFallback = false);   // :138
EnergyInputsT<double> energyInputs(const EnergySurface&, const ShortwaveAbsorption&, const EnergyState&);  // :155
template <class T> std::array<T,4> energyResiduals(surface, EnergyInputsT<T>, forcing, szaDeg,
        sunlit, dtSeconds, const EnergyState& history, double inverseObukhov, double sceneSensible);      // :160
```

`solveEnergyNode` 的内部调用链（energy.cpp:698-891）：

```
solveEnergyNode
 ├─ 步骤1 选 dG/dT（surface: 2I/sqrt(pi dt)；depth: 土柱线性化斜率；direct: -f·4εσT³）
 ├─ 步骤2 联合迭代（至多 maxIterations）
 │    fluxes()  ── aerodynamicResistances<double>()  Raupach 阻抗网络        (:217)
 │              └─ balances<double>()  四组分 Rn、H、LE、G、残差            (:452)
 │                    ├─ homLongwave / 无叶时 ε·L↓
 │                    ├─ stomatalResistanceT   Farquhar + Ball-Berry          (:343)
 │                    ├─ soilSurfaceResistanceT                              (:409)
 │                    └─ surfaceHeatFluxT / depthHeatFlux / 比例 G
 │    updateTemperature()  阻尼牛顿，单步 ≤5 K，限幅 [175,380] K               (:596)
 │    1/L 自适应松弛更新（±5 m-1 限幅）；bracketFallback 时进入括号法        (:744-816)
 ├─ 步骤4 finalEvaluation：用较宽的验收阈值判 converged                        (:822-845)
 └─ 步骤5 advanceThermalState 提交土壤热历史并汇总场景加权诊断量               (:851-890)
```

`fluxes()` 还负责冰雪温度上限：当 `surface.temperatureCap > 0` 且组分在上限上、残差为正时，残差记为融化热汇 `melt` 并置零（energy.cpp:549-564）。迭代内的判据与最终验收阈值**故意不同**：迭代要求组分残差 ≤ 0.5 W m⁻²、阵风残差 ≤ 1 W m⁻²、稳定度残差 ≤ 1e-4 + 0.02|target|（energy.cpp:52-55）；返回前重算后分别用 5、2 与两倍稳定度容差（:827-828、`kResidualThreshold`，:48）。`stabilityIterations` 恒等于 `iterations`，仅为保持输出布局（energy.h:132）。

同化专用入口 `energyResiduals<T>` 与正向共用 `aerodynamicResistances<T>` 和 `balances<T>`；`aerodynamicResistances` 对 LAI 的标量类型模板化，于是 Ad 运行时位移高度、粗糙度、u*、rac、叶边界层、冠层权重对 LAI 的偏导进入雅可比，而 1/L 与对流阵风（`sceneSensible`）作为冻结的 `double` 参数传入（energy.cpp:190-194、649-659）。这决定了 EB 同化是“准牛顿”：其梯度不含稳定度反馈。

`Ground`、`EnergySurface` 的缺省值与 `classes.json`、`config` 的缺省值要成套核对：`Ground::emissivity` 0.955、`conductivity` 1.55、`volumetricCapacity` 1.298e6（energy.h:56-72）；`EnergySurface::referenceHeight` 缺省 15 m，且冠层更高时取 `max(referenceHeight, hc + 0.1)`（energy.h:84；energy.cpp:233）。

#### 7.3.5 土壤热、发射率与亮度

`heat_conduction.cpp` 的 `kHistorySeconds = 7 h` 是 `surface` 方法的历史窗口，与 TIRTEB 相同（heat_conduction.cpp:25-29）；`depth` 方法用 1 cm 起按 1.3 倍递增到 2 m 的土柱，向后欧拉，且对地表温度精确线性化为 `G = depthIntercept + depthSlope × T` 并缓存于 `SoilThermalState`（heat_conduction.h:21-30；`depthHeatFlux` :174）。`SoilThermalState::history` 为最新在前的“上一步”温度，`advanceThermalState` 在节点提交时移位历史并推进土柱（:203）。

土壤发射率有两层：`soil_emissivity.h` 给出宽带半球值随含水量由干端向饱和端插值（权重 `1 − exp(−max(SMC−0.05,0)/0.25)`，缺省饱和值为干端加 0.03 且不超过 0.995，soil_emissivity.h:42-83），EB 与成像共用；`hapke.h` 给出方向发射率，先二分反求单次散射反照率使半球均值等于该宽带值，再按观测天顶角取值，`SoilEmissivityTable` 以 0.1° 间隔建表（hapke.h:57-101）。`app/thermal_view.h` 的 `MoistureSoilEmissivity` 把干端与饱和端两张表按同一含水量权重混合（thermal_view.h:51-71）。

`brightness.h` 的 `kPlanckC2 = 14388.291040407` 与 CODATA 值有 3.63e-5 的相对差；因正反演共用同一常数，内部自洽，文件内给出了不改的理由与量级（brightness.h:25-29）。改此常数会使全部亮温产品移动约 0.01 K。

#### 7.3.6 未接入与孤立文件

`effective_lai.*` 已编译但无调用者，所有冠层按均质处理；地类表中写了树冠或垄行键时，运行日志警告并写入 `scene_summary.json` 的 `canopyStructure`（effective_lai.cpp:27；scene/classes.cpp:134-145；image.cpp:473-485、1618-1634）。`stream_core.hpp` 是 StreamEarth 观测核心的复制品，算法与 TiRT 标准闭合不同（两层指数、阶梯式 alpha 表、无反射天空），不得当作 Studio 的观测算子（stream_core.hpp:2-12、17-24）。`observation_errors.h` 是被使用的：仅 `assimilation.cpp:403` 调用。

#### 7.3.7 物理模块与理论手册章节对应

对应关系依据 `docs/theory-manual.md` 的章节标题与正文关键词（不含逐段核对）：

| 代码模块 | 理论手册位置 |
| --- | --- |
| `optics`（PROSPECT、BSM）、`canopy`、`radiosity`、`shortwave` | §0.2 遥感像元的计算方法；§3.1 光学波段；§3.2 热红外波段 |
| `energy`（阻抗、气孔、稳定度）、`heat_conduction` | §7.1–§7.3；rar 的稳定度项位置见 §7.2 |
| `soil_emissivity`、`hapke` | §2.2（土壤含水量与热红外发射率）；§9.2（背景 Hapke 角度效应） |
| `thermal_view` 的权重 w_k、`inversion` | §15.1–§15.5；w_jk 的定义见 §15.4 |
| `assimilation`、`dual`、`energyResiduals<Ad>` | §15.6 STREAM 内建同化（可微算子与 MAP 分析） |
| `spa`、`brightness` | 手册中的具体位置待核实 |

### 7.4 scene/：栅格与地类表

| 文件 | 行数(约) | 职责 | 关键导出 |
| --- | --- | --- | --- |
| `raster.h/.cpp` | 67 / 254 | GDAL 栅格读写、网格对齐检查、像元经纬度、统计 | `Grid`、`Field`、`kNaN`、`readField`、`openRaster`、`readBand`、`fieldRaster`、`geographicFromGrid`、`writeTiff`、`bandStatistics`（raster.h:28-65） |
| `classes.h/.cpp` | 59 / 195 | 地类参数表加载与查询 | `SurfaceKind`、`GroundKind`、`ClassParameters`、`ClassTable::find`、`loadClassTable`（classes.h:25-57） |

`Field` 表示一个栅格输入：`{file, band}` 或 `{value}`，`present()` 判定二者之一存在（raster.h:44-50）；`fieldRaster(field, grid, label, fallback, required)` 把它展开为整幅 `float` 数组，文件来源读取指定波段，常数来源铺满全网格（raster.cpp:145-147）。所有输入必须与地表类型栅格同网格，`requireAlignedGrid` 检查尺寸、坐标系与地理变换，不一致直接报错，不做重投影或重采样（raster.cpp:30-36）。`writeTiff` 写 float32 GeoTIFF，DEFLATE 压缩，nodata 为 NaN，波段名写入描述（raster.cpp:201）。`geographicFromGrid` 由地理变换与 CRS 求每个像元中心的 WGS84 经纬度，失败为 NaN（raster.cpp:170）。`pathFromUtf8` 与 `utf8FromPath` 是全引擎处理路径编码的唯一入口，`main.cpp` 设置 `GDAL_FILENAME_IS_UTF8`（main.cpp:247）。

`ClassTable` 持有 256 个 `ClassParameters`，`find(code)` 对浮点地类码四舍五入，超出 0–255 或表项为 `Missing` 时返回空指针（classes.cpp:68-75），调用方据此把像元记为未知地类。`loadClassTable` 要求 `kind = stream-classes`、`schemaVersion = 1`，所有错误一律抛带字段路径的中文 `std::runtime_error`（classes.cpp:80-86）。读取规则：所有地类先继承土壤地面参数，再被各自的 `ground` 块覆盖；`ground.surface` 不是 `soil` 时按平面地面（`GroundKind::Flat`），`snow/ice/glacier` 另置 `ice = true`（classes.cpp:172-187）；光合途径 `C3/C4` 以外一律为 0（无气孔，叶片阻抗 625000 s m⁻¹，classes.cpp:157-161；energy.cpp:346）；叶片结构参数 N 必须 ≥ 1（classes.cpp:156）。`ClassTable::referenceHeight` 虽被读入（classes.cpp:105），但在 `models/stream` 内没有其他引用，能量平衡实际使用 `config.referenceHeight`（image.cpp:770）；要改参考高度，改配置而不是 `classes.json`。

### 7.5 forcing/：气象强迫

#### 7.5.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出 |
| --- | --- | --- | --- |
| `forcing_source.h/.cpp` | 80 / 25 | 数据源抽象接口与运行报告 | `ForcingSource`、`ForcingWeights`、`ForcingReport`（去重的 `warn/info`） |
| `forcing_metadata.h/.cpp` | 165 / 939 | 元数据结构、预设、解析校验、单位与物理换算 | `ForcingMetadata`、`ForcingChannel`、`ForcingIssues`、`resolveForcingMetadata`、`forcingPresetNames/Json`、`forcingMetadataSummaryJson`、`forcingUnit`、`forcingBrutsaertLongwave` |
| `era5.h/.cpp` | 107 / 1188 | 网格数据包登记与读取（ERA5、ERA5-Land、元数据驱动的其他网格） | `registerPack`、`registerForcingPack`、`Pack`、`requiredVariables`、`optionalVariables` |
| `station_source.h/.cpp` | 85 / 557 | 站点 CSV 数据源 | `StationSource`（最近站或 IDW，最多 4 站） |
| `forcing_resample.h/.cpp` | 89 / 366 | 时间标准化：瞬时插值、通量分配、标准量合成、覆盖检查 | `resampleInstant`、`fluxRecordsFrom`、`distributeFlux`、`standardiseQuantities`、`forcingChannelKeys`、`checkRecordsCover` |
| `pixel_forcing.h/.cpp` | 92 / 290 | 单像元插值、高程订正、接收面短波 | `PixelForcing`、`SurfaceShortwave`、`approximateCosZenith/SunAzimuth` |
| `utc.h/.cpp` | 36 / 132 | UTC 与 Unix 秒换算 | `UtcTime`、`utcFromSeconds`、`parseUtc`、`formatUtc`、`daysFromCivil` |

#### 7.5.2 数据源抽象 ForcingSource

`ForcingSource` 是纯虚接口，`era5::Pack` 与 `StationSource` 各自实现（forcing_source.h:49-78）。标准量沿用 ERA5 命名与 SI 单位：`t2m d2m sp u10 v10 ssrd strd`，可选 `skt swvl1 z`；时间一律为自 1970-01-01 UTC 起的整小时数（forcing_source.h:45-48）。接口的使用顺序固定：

```
metadata() / standardised() / product() / fullYear() / leadHours() / available(var)
covers(variables, firstHour, lastHour, gap)     检查覆盖，失败给出首个缺口
load(variables, 纬度窗, 经度窗, firstHour, lastHour)   读入内存（点源忽略经纬窗）
weights(lat, lon) -> ForcingWeights{index[4], weight[4]}   像元权重（网格：双线性四角；站点：IDW 最多 4 站）
at(var, hour, weights)          整点值，hour 夹到已加载范围，NaN 感知加权
hourlyFlux(var, hour, weights)  截至 hour 的前一小时平均通量（W m-2）
lapseRate()                     气温递减率（缺省 0.0065 K m-1，元数据可改）
```

`ForcingWeights` 在网格源中的 `index` 是已加载窗口内“行×列数+列”的一维偏移（forcing_source.h:24-31），因此权重只对产生它的那个源、那次 `load` 有效；`PixelForcing` 在构造时一次取好权重并缓存（pixel_forcing.cpp:90-100）。`image.cpp` 在 RT 模式下放一个未打开的 `era5::Pack` 作占位，以便后面统一用 `ForcingSource&`（image.cpp:291、319）。

新增一种数据源（例如另一种站点格式）的最小改动是：实现全部纯虚函数；在 `image.cpp` 的 `makeSource` 中增加分支（image.cpp:299-317）；在 `config.cpp` 的步骤 8a 增加配置键与校验（config.cpp:362-385）。

#### 7.5.3 元数据与标准化

`ForcingMetadata` 按文档章节分组：time、grid、station、variables（每个量一个 `ForcingChannel`）、heightCorrection、spatial（forcing_metadata.h:80-127）。`ForcingChannel` 的换算顺序固定：`SI = (raw × scale + offset) × unitFactor + unitOffset`，`missing/range` 用声明单位在换算之前判断；`kind`（instant、previous、interval_mean、accumulated、cumulative）与 `stamp` 决定时间含义（forcing_metadata.h:45-68）。内置预设为 `era5`、`era5-land`、`gldas-noah-0.25`、`gldas-noah-1.0`、`station-csv`（forcing_metadata.cpp:130-132、690），GLDAS 预设未经真实数据验证（PORTING.md:55）。

`ForcingMetadata::era5Layout()` 判断元数据是否与内置 ERA5 预设恒等；为真时 `Pack` 走原读取路径（`generic_ = false`），结果与引入元数据层之前逐位一致，ERA5-Land 在该路径下由“短波为 cumulative”识别（forcing_metadata.cpp:777-794；era5.cpp:600-601）。这是修改读取逻辑时最容易破坏的回归点：改动元数据默认值或预设可能使 `era5Layout()` 变假，从而让 ERA5 数据改走标准化路径。

`Pack::open` 的元数据优先级为：调用方传入、索引文件的 `metadata` 键、产品名对应预设（era5.cpp:589-596）；索引文件 `kind` 为 `stream-era5-pack` 或 `stream-forcing-pack`，`schemaVersion = 1`（:586-588）。`registerPack` 写出 `stream-era5-pack`，记录产品、年份、公共时段、`fullYear`、网格、逐变量完整性与文件清单（含字节数，打开时校验，被改动则要求重新登记，era5.cpp:496-508、632-633）；`registerForcingPack` 不判断逐小时连续，覆盖检查推迟到运行时（era5.cpp:1103-1108）。`Pack::load` 在经纬窗口两侧各多取 2 格以保证双线性邻点，全球经度包窗口越界时取全部经度（era5.cpp:817-833）。数据缺小时记录时，ERA5 原路径在 ±72 h 内取最近记录代替并写入 `notes_`，同时 `exact_` 标 0（era5.cpp:870-875，之后的细节待核实）。

`Pack::hourlyFlux` 统一把通量化为 W m⁻²：元数据路径本身就是 W m⁻² 并截断为非负；ERA5 逐小时累积除以 3600；ERA5-Land 自 00 UTC 累积，与前一小时差分（01 UTC 取累积本身），遇到补全小时时改用最近可差分的小时（era5.cpp:1065-1101）。

时间标准化的共用函数在 `forcing_resample.cpp`：瞬时量线性插值，缺口超过 `max(6 h, 2×步长)` 视为缺测（forcing_resample.cpp:45-47）；通量分配到整点时保证每个时段能量守恒，短波按太阳天顶角余弦分配（`approximateCosZenith` 在 `pixel_forcing.cpp` 定义，`forcing_resample.cpp:37` 以外部声明引用）。`StationSource` 构造时读取并校验全部 CSV，`load` 时重采样并交 `standardiseQuantities` 合成标准量，其内部逐站细节待核实。

#### 7.5.4 PixelForcing 与接收面短波

`PixelForcing::at(unixSeconds)` 组装 EB 所需的 `Forcing`（pixel_forcing.cpp:276-288）：气温按递减率与高程差订正（`temperature`，:115-118）；水汽压由露点（不订正）经 Tetens 换算并封顶到订正后气温的饱和值；气压按 `exp(−g·Δh/(Rd·Ta))` 折到像元高程；风速为 10 m 分量合成；长波与短波的小时通量放在小时中心（k−0.5）后线性插值，短波按晴空指数 F/cos z 插值以保持日变化（pixel_forcing.h:13-15；pixel_forcing.cpp:124-165）。`surfaceShortwave` 有两个入口：只给天顶角的 NOAA 版本，以及给定 SPA 天顶/方位角的版本（FIX C10，pixel_forcing.h:63-69）；EB 步进使用后者，使“太阳是否在地平线上”与冠层直射几何一致（image.cpp:880-883）。水平像元的旧算法保持逐位不变（`tilted = false`）；坡面、山包与山脊在内部把直射按入射余弦、漫射按天空视角因子投影，并把总量限制在 1400 W m⁻² 以内（pixel_forcing.cpp:261-269）。

### 7.6 app/：主流程与应用层

#### 7.6.1 文件清单

| 文件 | 行数(约) | 职责 | 关键导出 |
| --- | --- | --- | --- |
| `main.cpp` | 257 | 命令行入口与子命令分发 | `wmain/main`、`dispatch`、`forcingCheck`、`usePackagedProj` |
| `config.h/.cpp` | 203 / 562 | 运行配置结构与解析 | `ImageConfig`、`readImageConfig`、`InversionConfig`、`AssimilationConfig`、`TerrainRadiation` |
| `image.h/.cpp` | 19 / 1653 | 主流程：强迫、光谱、EB、反射天空、成像、写出 | `runImage`（image.cpp:189-1649） |
| `thermal_view.h/.cpp` | 162 / 395 | 热红外观测算子，成像、反演、同化共用 | `thermalPixel<T>`、`thermalWeightsT<T>`、`thermalSceneWeights`、`ThermalScene`、`TerrainFields`、`solarPosition` |
| `inversion.h/.cpp` | 101 / 702 | LST 与 LSCT 反演，QA 位常量 | `runInversion`、`InversionInputs`、`kQa*` |
| `assimilation.h/.cpp` | 101 / 875 | MAP 高斯-牛顿同化 | `runAssimilation`、`AssimilationInputs`、`EbSolver` |
| `parallel_error.h` | 58 | OpenMP 并行区异常收集 | `ParallelError` |

#### 7.6.2 main.cpp：命令行

退出码：0 成功，1 运行时异常，2 用法错误，3 校验或登记失败（main.cpp:11、171-179、254-255）。五个子命令（`dispatch`，main.cpp:186-227）：

| 子命令 | 作用 | 落到的函数 |
| --- | --- | --- |
| `run <config.json>` | 一次成像、反演或同化 | `runImage`（:188） |
| `era5-register <pack.json> [--product …] <file.nc>… [--files-from <list>]` | 登记 ERA5 数据包 | `era5::registerPack`（:214-224） |
| `forcing-register <pack.json> --metadata <预设或文件> <file.nc>…` | 登记其他网格数据包 | `era5::registerForcingPack`（:195-213） |
| `forcing-preset [名称]` | 列预设或输出预设 JSON | `forcingPresetNames/Json`（:189-193） |
| `forcing-check <预设或文件或 JSON> [--data …] [--json]` | 校验元数据，可再打开数据 | `forcingCheck`（:112-168） |

`--files-from` 从文本文件按行读路径，以绕开 Windows 命令行长度上限，并剥离 BOM（main.cpp:86-106）。`forcing-register` 把文件形态的元数据转成绝对路径再写入索引，使数据包移动后仍可找到元数据（:206-210）。`main` 把 stdout/stderr 设为无缓冲，使服务端能实时解析进度行（:243-244）。

#### 7.6.3 config：ImageConfig 与解析

`ImageConfig`（config.h:110-196）按“场景栅格、土壤亮度、区域、波段、温度来源、EB 强迫与地形、TIRTEB 对齐的能量平衡选项、发射率与土壤热参数、迭代与起转”分组；关键枚举为 `TemperatureMode { Rt, Eb }`、`TerrainRadiation { None, Single, Facet, Hill, Ridge }`、`ObservationKind`、`InversionTarget`、`AssimilationObservation`。

`readImageConfig(path)`（config.cpp:89-558）的解析顺序即其代码顺序：基本路径与过境时刻（`kind = stream-image-config`、`schemaVersion = 2`）→ `surface`（类型、LAI、土壤水分、DEM、`terrain`、`soilBrightness`、经纬度）→ `view` → `region` → `optical/thermal` 波段 → `inversion` → `assimilation` → `temperature`（`eb` 或 `rt`）→ 发射率共用块 → `skyLongwave` → 常数几何范围检查 → `threads`。设计约束有三条：数值字段必须是 JSON 数字，使用 `numberOr/integerOr` 严格读取，字符串、布尔或对象报错（config.cpp:30-51）；GDAL 的 `GetDouble` 会把非数值静默当作 0，所以新增数值字段不要直接用它；`temperature.energy` 下的发射率与 Hapke 项在 EB 与 RT 两种模式下都读取（config.cpp:495-525），而 `diffuseFraction` 在 RT 模式不接受 −1（:497-505）。同化与反演互斥（config.cpp:344），至少要启用光学、热红外、反演或同化之一（:348）。`threads` 缺省 0 表示不调用 `omp_set_num_threads`（image.cpp:194-196）。

#### 7.6.4 image.cpp：runImage 的结构

`runImage` 是一个约 1500 行的单一函数，步骤以注释“步骤 n”标注（image.cpp:6-18）；真正可复用的逻辑以 lambda 形式写在函数内部，复用靠闭包捕获。

| 步骤 | 内容 | 行号 |
| --- | --- | --- |
| 1 | 读配置与地类表；覆盖 BSM 亮度与土壤干端发射率 | image.cpp:191-222 |
| 2 | 读地表栅格，经度折到 ±180，确定区域，统计无效 LAI | :224-287 |
| 3 | 建强迫源，确定起转起点与节点序列，载入强迫（仅 EB） | :289-426 |
| 4 | 土壤水分（强迫的 swvl1 或用户栅格） | :428-461 |
| 5 | PROSPECT 叶片光谱、BSM 土壤光谱（按含水量分 1000 档） | :463-567 |
| 6 | 地形输入；EB 闭包；RT 读入或 EB 逐像元求解 | :591-1173 |
| 7 | 反射天空下行长波 L↓ | :1180-1217 |
| 8 | 成像缓冲与 Hapke 表 | :1219-1246 |
| 9–10 | 反演或同化 | :1267-1306 |
| 11 | 逐像元成像 | :1308-1411 |
| 12 | 写出 GeoTIFF 与 `scene_summary.json` | :1413-1647 |

步骤 6 内的核心闭包与调用关系：

```
pixelForcing(pixel)  -> PixelForcing（DEM 与强迫网格海拔之差作高程订正）      (:719)
buildSurface(entry, LAI, 含水量, Vcmax) -> EnergySurface                      (:739)
initialState(forcing) -> EnergyState（全部初温取起转起点 2 m 气温）            (:796)
marchNodes(pixel, surface, spectra, LAI, forcing, state, EbRun&)              (:849)
  └─ 对 nodes 逐节点：solarPosition → forcing.at → surfaceShortwave
       → absorbShortwave → solveEnergyNode（仅过境节点 bracketFallback = true） (:866-918)
EB 主循环（逐行 OpenMP）：marchNodes → 写 components / forcingOut / fluxOut      (:1058-1141)
ebSolver(pixel, EbControls, EbOutcome&)   同化重解 + 隐式导数                   (:930-1036)
```

起转节点序列：起点是“过境前最后一个整点往前数 N 个整点”，N 即 `temperature.spinupHours`（缺省 7）；数据包不覆盖起点时向后移到可连续覆盖的最早整点并写警告和 `notes`，`nodes` 为整点序列加过境时刻本身（image.cpp:377-426、419-422）。节点 0 的时间步固定为 3600 s，之后为与上一节点的时间差（image.cpp:906）。该逻辑的行为细节见《运行原理说明》5.6.3。

`ebSolver` 的数学步骤：以控制量重建地表并再次起转推进，得到收敛温度；以 `Ad::seed` 把四组分温度（种子 0–3）与土壤水分（4）、Vcmax（5）、LAI（6）播种；有短波时用 `shortwaveComponents<Ad>` 重算吸收分量；`energyResiduals<Ad>` 返回 R 与其导数；最后对 4×4（无叶 2×2）矩阵做部分选主元的 Gauss-Jordan，得 dT/dx = −(∂R/∂T)⁻¹ ∂R/∂x（image.cpp:957-1034）。返回 `true` 但 `gradientValid = false` 表示温度有效而导数被拒（融化钳制、主元 ≤ 1e-9 或非有限，:955、1016、1031）。

两个“全局”约定要记住：土壤含水量按 `kMoistureSteps = 1000` 量化，BSM 光谱按档缓存（image.cpp:81、158）；结果栅格行优先，像元序号 = row × width + column，`region` 行列从 0 起、终点不含（image.cpp:25、263-269）。

#### 7.6.5 thermal_view：热红外观测算子

`thermal_view` 是成像、同化、反演共用的单一实现，保证三者看到相同的四组分权重（thermal_view.h:19-22）。组分顺序固定为 `[土壤光照, 土壤阴影, 叶片光照, 叶片阴影]`。公共接口：

```
Sun solarPosition(unixSeconds, lat, lon, elevation)                           // SPA，气压置 0 不做折射，delta_t = 69
std::array<T,4> thermalWeightsT<T>(HomCanopyT<T>, lai, hotspot, sza, vza, 相对方位, 叶发射率, 土壤发射率)
bool thermalSceneAt(TerrainFields, pixel, sza, saa, vza, vaa, ThermalScene&)  // 几何部分；面元背向传感器返回 false
void setThermalMaterial(ThermalScene&, ImageConfig, ClassParameters, tables, code)   // 材料部分
bool thermalPixel<T>(scene, 四组分温度, lai, 含水量, 波长[], 亮温[], 表面温度[])
bool thermalSceneWeights(scene, lai, 含水量, w, fixed, reflectivity)          // 线性形式，供反演
```

关键算法（thermal_view.cpp）：权重 w_k = 四组分比例 × 发射率 + hom3 多次散射项，各项下限 0，太阳在（局地）地平线以下时把光照权重并入对应阴影组分，Σw 不变（:60-86）；亮温的辐亮度为 `Σ w_k B(T_k) + (1 − Σw)·B(T_sky)`，亮温由逆 Planck 求得，“表面温度”为发射辐亮度除以有效发射率再逆 Planck，不含反射天空（:282-298）；发射率归一化缺省使用固定的地类干端土壤发射率（`reference = true`，对应 `surfaceTemperatureNormalisation = retrieval`，:253-258）。山包与山脊由 `forEachThermalView` 枚举子视角：hill 为 12 个锥面方位各 1/12，ridge 为两个侧坡加两个端面，并叠加平地背景，投影权重含局地观测余弦（:187-224）；`thermalPixel` 与 `thermalSceneWeights` 共用该枚举，保证反演与成像一致（:317、354）。叶片发射率取地类的 `1 − reflTir − transTir`，而不是 EB 的宽带 `leafEmissivity`（:147）。

`ThermalScene::skyTemperature` 为 NaN 表示不计反射天空；它由 `skyTemperatureFromLongwave(L↓) = (L↓/σ)^¼` 得到，RT 缺省的 L↓ 由 `swinbankLongwave`（5.31e-13·T⁶）估计（thermal_view.cpp:334-338、380-383）。

#### 7.6.6 inversion：LST 与 LSCT

`runInversion(InversionInputs, background)` 返回 `InversionResult`（inversion.h:97-99）。`InversionInputs` 的成员全部是对整景栅格的引用；只处理 `[startRow,endRow) × [startColumn,endColumn)`，`progressStart/progressSpan` 为本阶段占总进度条的区间（inversion.h:62-87）。流程（inversion.cpp:150-）：读观测栅格与掩膜，逐像元用 `thermalSceneAt`、`setThermalMaterial`、`thermalSceneWeights` 准备 `Sample`（含权重 `weights[4]`、LST 归一化因子 `fixedEmissivity`、反射天空项 `reflected`，:112-121、242-262）；LST 分支为多观测加权 Gauss-Newton，把发射率误差并入观测方差（:289-330）；LSCT 分支为以 RT 输入或 EB 解为背景的 MAP Gauss-Newton，可共享 s×s 同地类窗口，背景协方差 B = σ²[(1−r)I + r11ᵀ] 并限于被观测的组分（:393-461）。观测模型 `model()` 与其对温度的解析梯度只依赖 `planck/inversePlanck` 与 `planckDerivative`，**不使用 `Ad`**（inversion.cpp:76、127-141）。产品为 `lst.tif`、`lst_uncertainty.tif`、`lst_qa.tif` 与 `lsct*.tif` 五件（:383-387、618-628）。

QA 位常量集中在 `inversion.h:41-60`，同化直接引用，位定义与默认拒收掩膜 895 见 README.md:182-196；新增质量位必须同时改 `inversion.h`、README 表与 `config.h` 的 `rejectQaBits` 说明（config.h:92-94）。

#### 7.6.7 assimilation：MAP 同化

接口（assimilation.h）：`EbControls {moisture, vcmax, lai}` 为 EB 重解的输入，`EbOutcome` 含四组分温度、通量与 `gradient[k][c] = dT_k/d[土壤水分, Vcmax, LAI]`，`gradientValid` 为假时分析必须拒绝线性化（:45-51）；`EbSolver` 是 `std::function` 回调，由 `image.cpp` 注入（:53、1296）。`AssimilationInputs` 同样全为引用，另有可空的 `diffuse`、`shortwave`、`skyLongwave` 指针（:60-87）。

`runAssimilation`（assimilation.cpp:192-）逐像元独立，按行并行，步骤如下：

```
读观测（亮温 / LST / 组分温度，可选不确定度、协方差、QA、附加视角、光学反射率）   (:203-237)
逐像元：
  输入检查 → solarPosition → thermalSceneAt / setThermalMaterial             (:283-320)
  构造观测列表 obs（温度 150–400 K、σ 有限为正、QA 与 rejectQaBits 与运算为 0）  (:321-355)
  确定控制量：RT 为有效背景的四组分温度；EB 为土壤水分加 Vcmax（C3/C4 且 LAI>0）；
              有光学观测时加 LAI                                                 (:362-387)
  背景协方差：对角 σ²，组分温度间相关 0.5，求逆得 B^-1                          (:418-425)
  evaluate(x)：Ad 播种 → [RT: 温度即控制量；EB: ebSolver] → thermalPixel<Ad> 或
              canopyReflectanceT<Ad> → h[i] 及对控制量的导数                     (:436-516)
  MAP 高斯-牛顿：增量形式，回溯步长 1、1/2、1/4，仅在代价 J 下降时接受            (:524-628)
  停止原因分类 → QA 位 9/10/11；后验协方差与 DFS；写不确定度与 QA 波段            (:629-731)
写 analysis_* / background_* 产品                                               (:748-783)
```

内置缺省全部是 `assimilation.cpp` 顶部的 `constexpr`：背景温度 σ 5 K 且相关 0.5、土壤水分 σ 0.10、Vcmax 相对 σ 0.3、LAI 相对 σ 0.3；迭代 RT 20 次、EB 10 次；收敛判据为各控制量步长小于其先验 σ 的 1e-3（assimilation.cpp:55-66）。`analysis_qa.tif` 9 个波段依次为 QA、O−B、O−A、O−A RMS、DFS、观测数、迭代次数、J_b、J_a（:258）。同化只分析 `SurfaceKind::Vegetation` 的像元（:285）。输出质量位只是标注，不会阻止分析场替换背景用于成像（README.md:200）。

#### 7.6.8 parallel_error.h

OpenMP 并行区内抛出的异常会触发 `std::terminate`。本引擎的统一做法是：并行体用 `try { … } catch (...) { errors.capture(); }`，循环体开头 `if (errors.failed()) continue;`，并行区结束后串行调用 `errors.rethrow("阶段名")` 重抛为带阶段名的 `std::runtime_error`（parallel_error.h:20-52）。`image.cpp`、`inversion.cpp`、`assimilation.cpp` 的所有并行循环都遵循该模式；新增并行循环必须照做，否则任何一个像元异常都会让进程无消息崩溃。需要在并行区累计浮点统计量时，本引擎按行存部分和、再按行序相加，使最后一位数字与线程调度无关（inversion.cpp:283-285；assimilation.cpp:268-269）。

### 7.7 tests/：雅可比回归测试

`tests/jacobian_check.cpp`（354 行）构成独立目标 `stream_jacobian_check`（`EXCLUDE_FROM_ALL`），只链接 `canopy`、`energy`、`heat_conduction`、`radiosity`、`shortwave` 五个物理源文件，不需要 GDAL（CMakeLists.txt:48-62）。它用四个合成场景（作物 LAI 3 与 1、森林 LAI 4、C4 草地）各跑 1 个节点与 7 个节点的 march，对比 `Ad` 残差对 LAI 的偏导与“阻抗重算”的中心有限差分（容差 1e-3 + 2e-4·|FD|），并检查隐式 dT/dLAI 与差分隐式值一致（容差 1e-4 + 2e-3·|值|）；冻结阻抗的差分与全非线性差分只打印不判定，因 1/L 与阵风冻结的剩余差异是已知的（jacobian_check.cpp:262-319、336-351）。退出码等于失败检查数；运行方式 `stream_jacobian_check <stream-defined 目录>`；由 `tools/test-stream-jacobian.py` 构建并运行（jacobian_check.cpp:28-29；tools 脚本头部）。物理核改动后应运行该测试，特别是动了 `aerodynamicResistances`、`balances`、`shortwave`、`radiosity` 之一时。

### 7.8 数据结构与文件格式

| 文件 | 格式要点 | 生产者 | 消费者 |
| --- | --- | --- | --- |
| 运行配置 | `kind = stream-image-config`、`schemaVersion = 2` | `server/stream/config.mjs`（README.md:42） | `readImageConfig` |
| 地类表 | `kind = stream-classes`、`schemaVersion = 1`，含 `soil/atmosphere/aerodynamics/classes` | 人工维护 | `loadClassTable` |
| ERA5 数据包索引 | `kind = stream-era5-pack`，含 product、year、period、fullYear、grid、variables、files（路径与字节数）、complete | `stream era5-register` | `Pack::open` |
| 元数据数据包索引 | `kind = stream-forcing-pack`，另存 `metadata` 键 | `stream forcing-register` | `Pack::open` |
| `components.tif`、`components.json` | 4 波段 Tss/Tsh/Tcs/Tch（K）；JSON 为 `stream-components` v1 | EB 的 `runImage` | RT 模式的 `components.file`（image.cpp:1531-1543） |
| `eb_forcing.tif` | 7 波段：气温、水汽压、气压、风速、水平面短波、长波、土壤水分 | `runImage` | 前端查看 |
| `eb_fluxes.tif` | 18 波段，含冰雪且开启融化时为 19 波段 | `runImage` | 前端查看（image.cpp:578、1110-1125） |
| `scene_summary.json` | `kind = stream-image-result`、`schemaVersion = 2`，含 counts、temperature、forcing、canopyStructure 等 | `runImage` | 服务端与前端 |
| `analysis_*`、`background_*`、`lst*`、`lsct*` | 见 7.6.6、7.6.7 | 对应模块 | `runImage` 登记到 `products` |

`eb_fluxes.tif` 的 18 个基础波段顺序为 Rn、H、LE、G、迭代次数、是否收敛、Obukhov 长度、u*、稳定度是否收敛、能量残差、稳定度残差、稳定度限幅、阵风残差、空气密度、气动粗糙度、位移高度、冠层权重、短波限幅标记，第 19 波段为融化热通量（image.cpp:1110-1112、1517-1523）。这些顺序被前端与服务端按位置读取，改顺序属于破坏性变更。

### 7.9 修改指南与注意事项

1. 新增一个 EB 能量平衡参数（例如新阻抗系数）：在 `config.h` 的 `ImageConfig` 加字段、`config.cpp` 的步骤 8c 用 `numberOr` 读取并校验、`image.cpp` 的 `buildSurface` 写入 `EnergySurface`（image.cpp:739-783）；若参数影响残差，还要确认 `energyInputs` 与 `energyResiduals<Ad>` 是否需要把它作为 `Ad` 量传入，并更新 `README.md` 参数表。同时检查 `server/stream/config.mjs` 是否需要透传（待核实该文件的透传逻辑）。
2. 改能量平衡的物理公式：改 `physics/energy.cpp` 的模板函数（`balances`、`aerodynamicResistances`、`stomatalResistanceT`），不要在 `double` 与 `Ad` 之间写两份；只改 `fluxes()` 或 `solveEnergyNode` 的非模板部分不会影响同化梯度，反之改模板部分一定会，改完运行 `stream_jacobian_check`（见 7.7）。
3. 改收敛判据或松弛：判据常数在 `energy.cpp:43-74`，迭代内与最终验收阈值不同是有意设计，不要合并；改动后同步 `README.md`、`PORTING.md` 的“稳定度”一节与 `scene_summary.json` 中的 `stability` 字符串（image.cpp:1603）。
4. 新增地类或改地类参数：改 `classes/classes.json`，不必改 C++；字段读取规则在 `scene/classes.cpp:80-192`。新增地面种类（非 soil）要在 `readGround` 之后设置 `groundKind`，并确认 `image.cpp:562-566` 的平地面光谱表、`thermal_view` 的 `soil` 标志与 Hapke 表构建（image.cpp:1228-1236）都覆盖到。地类 18 与 255 一律视为填充。
5. 新增气象数据源或变量：实现 `ForcingSource` 全部纯虚函数，在 `makeSource` 注册（image.cpp:299-317），把新标准量加入 `requiredVariables/optionalVariables`（era5.cpp:367-381）与 `image.cpp` 的变量清单（:345-362）；若只是新的网格产品，优先写元数据预设，不改 C++（`forcing_metadata.cpp` 预设表，:130-132）。
6. 改 ERA5 读取：必须保持 `era5Layout()` 路径逐位不变（era5.cpp:600-601），修改后用 ERA5 与 ERA5-Land 小包各做一次前后对比；`hourlyFlux` 的三种分支（元数据、ERA5 累积、ERA5-Land 差分）要分别验证（era5.cpp:1065-1101）。
7. 新增输出波段或产品：在 `image.cpp` 步骤 12 增加 `writeTiff` 与 `products.Add`，更新 `scene_summary.json` 的统计；扩展 `eb_fluxes` 时只能向后追加波段，不要插入中间（image.cpp:1517-1528）。含冰雪才追加第 19 波段的做法是“有条件追加”的先例。
8. 新增热红外观测几何（新的地形模式）：改 `config.cpp` 地形解析（:113-155）、`image.cpp` 的地形输入（:591-702）、`thermal_view` 的 `thermalSceneAt` 与 `forEachThermalView`（thermal_view.cpp:100-224），并让 `PixelForcing` 能产生对应的接收面短波（pixel_forcing.cpp:192-269）。`thermalPixel` 与 `thermalSceneWeights` 共用 `forEachThermalView`，只改其一会使反演与成像不一致。
9. 改反演或同化的观测误差、质量位：QA 位常量在 `inversion.h:41-60`，同化的缺省误差与先验在 `assimilation.cpp:55-66`；同化与反演共用同一套位定义，新增位要同时更新 README.md:182-196 的表、`config.h` 的 `rejectQaBits` 注释与 `config.cpp` 的位掩码范围检查（config.cpp:283-286）。
10. 新增同化控制量：枚举 `ControlKind` 与 `kMaxControls`（assimilation.cpp:66-71）、`evaluate` 中的播种与算子分支、`EbOutcome` 的梯度列数；总数不得超过 `kAdSize = 8`，超过要改 `dual.h` 并重新评估全部 `Ad` 运算的开销。EB 控制量还要在 `ebSolver` 的播种与 `b` 矩阵列数（image.cpp:960-1008）中同步。
11. 新增并行循环：套用 `ParallelError` 模式（7.6.8），统计量用 `reduction` 或按行部分和；进度行只允许主线程输出（`isMainThread`，image.cpp:98-105）。输出行格式 `STREAM\tprogress\t<百分比>\t<阶段>` 与日志前缀 `[STREAM]` 被服务端解析，不得改格式。
12. 改常数：`kStefanBoltzmann` 全引擎共用，改它会同时移动 EB、辐射度与成像；`kPlanckC1/C2` 与 `kRad` 是为与旧结果或 TiRT 逐位一致而保留的，改前先读各处注释（constants.h:19-22；brightness.h:25-29）；`kMaxLai = 20` 同时影响配置校验、成像、反演与同化（`validLai` 的所有调用点）。
13. 接入 `effective_lai`（垄行或树冠）：需要在 `canopy.cpp` 的 `canopyFractionsT` 之前把 LAI 换成方向有效 LAI，并保持 `Ad` 可微；`effective_lai.cpp` 当前使用 `float` 与旧的 π 近似（effective_lai.cpp:26-28），接入前要统一为 `kPi` 并重新验证。接入后删除 `classes.cpp` 与 `image.cpp` 中的“忽略结构”警告。
14. 新增源文件：加入 `CMakeLists.txt` 的 `add_executable(stream …)`；若属于梯度测试链路，也要加入 `stream_jacobian_check` 的列表（CMakeLists.txt:11-34、48-54）。源文件按 UTF-8 保存，MSVC 已加 `/utf-8`，源码内中文日志与错误信息依赖于此。
15. 易错点汇总：RT 模式组分温度不在 100–400 K 会被静默置 NaN（image.cpp:1174-1178），常见原因是误给了摄氏度；用户土壤水分被截断到 0.05–0.55，而 ERA5 路径截断到 0–1，且“大于 1 视为百分数”（image.cpp:453-460）；`ClassTable::referenceHeight` 未被使用（见 7.4）；`stream_core.hpp` 与 `effective_lai.cpp` 都不在计算路径上，修改它们不会改变任何输出；`config.cpp` 的 `region` 终点不包含（image.cpp:263-269）；面元地形需要 8 邻域，栅格最外一圈坡度恒为 0（image.cpp:693-700）。

## 8. 工具、测试、构建与发布脚本

本章覆盖仓库中“不属于运行时主链路、但开发与发布离不开”的部分：`tools/`（含 `regression/`、`case005/`、`stream-inverse/`）、`statistic/`、`package.json` 脚本、`.packaging/` 与 `electron-builder.yml`、`assets/` 目录组织、`docs/` 文档索引。运行流程本身（工程生命周期、结果缓存、质量保障）见《运行原理说明》第 2 章与第 6 章，本章只讲脚本的职责、接口与相互依赖。以下行号均指仓库根 `C:\work\streamstudio` 下的相对路径；本章调查期间未实际运行任何测试或构建，测试“覆盖什么”均来自文件头注释与用例代码。

### 8.1 总览：目录分工与依赖方向

| 目录或文件 | 用途 | 是否随安装包 | 是否入库 |
| --- | --- | --- | --- |
| tools 顶层（约 65 个文件） | 构建、运行时辅助、资产/示例生成、检查、测试、文档生成 | 仅 6 个运行时辅助脚本（见下） | 入库 |
| tools/regression | HiStream 引擎冻结输入回归 | 否 | 入库 |
| tools/case005 | 案例 005（光伏草场）的运行、发布、校验脚本 | 否 | 入库 |
| tools/stream-inverse | STREAM 单像元可微反演原型，含 vendor 的 autograd 1.9.1 | 否 | 入库 |
| tools/histream-ml | HiStream FacetEB 神经代理（PyTorch MLP 集成，导出 ONNX）与基于代理梯度的参数反演原型；设计与验证见 docs/differentiable-inversion-and-surrogate.md | 否 | 入库（数据与模型不入库） |
| statistic | 独立结果统计工具（Python） | 是，复制到 resources/statistic | 入库 |
| assets | 内置示例与各类资源库 | 经 stage 精简后复制 | 入库（output 与 `__pycache__` 忽略） |
| .packaging | assets 的打包暂存副本 | 是 | 忽略（.gitignore:14） |
| build、release、gui、models/bin_x64 | CMake 构建树、安装包输出、前端产物、引擎产物 | release 为产出物 | 均忽略（.gitignore:2-5、32） |
| docs | 手册、设计与历史记录、案例库 | 仅两本 PDF 手册 | 入库 |
| review | 示例端到端验证脚本与历史审阅记录 | 否 | 入库 |

依赖方向（单向）：`server.mjs` 与 `server/` 只依赖 `tools/` 中的 4 个运行时模块（`runtime-scene.mjs`、`tarp-cover.mjs`、`usd-export.mjs`、`usd-import.mjs`，server.mjs:48-51），`surface-textures.mjs` 与 `png.mjs` 经 `runtime-scene.mjs` 间接引用（runtime-scene.mjs:27），其余 `tools/` 脚本不被 `server.mjs` 引用；`tools/` 脚本反过来依赖 `src/renderer/src/project-schema.js`（工程默认值、规范化、校验）与 `server/` 模块（测试和验收脚本）。随安装包的 `tools/` 文件由 `electron-builder.yml:15-20` 列出：`runtime-scene.mjs`、`surface-textures.mjs`、`png.mjs`、`tarp-cover.mjs`、`usd-export.mjs`、`usd-import.mjs`，另有 `src/renderer/src/project-schema.js`、`project-unified.js`、`material-specular.js`（electron-builder.yml:21-23）。这与 `write-build-manifest.mjs:52-53` 的源码指纹清单一致，新增服务端依赖的 `tools/` 文件时两处必须同步（见 8.12）。

### 8.2 package.json 脚本与构建发布链

#### 8.2.1 脚本表

| 脚本 | 实际命令 | 作用 |
| --- | --- | --- |
| dev | node dev.mjs | 同时启动 API 后端与 Vite 开发服务器（package.json:10） |
| build、build:gui | vite build | 构建前端到 gui/（package.json:11-12） |
| build:engine | powershell 调用 tools/build-engine.ps1 | 构建两个原生引擎并写构建清单（package.json:13） |
| build:all | powershell 调用 tools/build-all.ps1 | 前端加引擎一键构建（package.json:14） |
| start | node server.mjs | 以浏览器方式运行服务（package.json:15） |
| test | node --test 加 4 个文件 | 默认回归集合，见 8.6（package.json:16） |
| validate:assets | node tools/validate-obj-library.mjs | 校验 OBJ 库目录（package.json:17） |
| lut:modtran | node tools/modtran-lut.mjs | 生成大气 LUT（package.json:18） |
| desktop | electron . | 运行桌面壳，入口 desktop/main.mjs（package.json:8、19） |
| stage:package | node tools/stage-package-assets.mjs | 生成 .packaging/assets（package.json:20） |
| pack:win | build:all，再 stage:package，再 electron-builder --win nsis | 产出 NSIS 安装包（package.json:21） |

没有 lint、没有 pytest 或 CTest 入口；Python 与 C++ 测试均需手工运行。

#### 8.2.2 完整发布调用链

```
npm run pack:win
  ├─ npm run build:all                       (tools/build-all.ps1:23-27)
  │    ├─ npm.cmd run build:gui  →  vite build  → gui/            (vite.config.js:35-37)
  │    └─ build-engine.ps1 -Configuration Release
  │         ├─ cmake 配置/构建 models/histream → build/histream     (build-engine.ps1:51-68)
  │         ├─ cmake 配置/构建 models/stream   → build/stream       (build-engine.ps1:72-79)
  │         └─ node tools/write-build-manifest.mjs models/bin_x64/Release  (build-engine.ps1:90)
  │              └─ 写 models/bin_x64/Release/build-manifest.json
  ├─ npm run stage:package → tools/stage-package-assets.mjs → .packaging/assets
  └─ electron-builder --win nsis → release/Stream-Studio-Setup-<ver>-x64-slim.exe
                                                   (electron-builder.yml:77)
```

引擎产物目录由 CMake 变量决定：histream 用 `OUTPUT_PATH`（models/histream/CMakeLists.txt:420），stream 用 `STREAM_RUNTIME_DIR`（models/stream/CMakeLists.txt:9、36），两者都被脚本指向 `models/bin_x64`，因此两个 exe 落在同一个 `models/bin_x64/<Configuration>/`，随后整体复制为安装包的 `resources/engine`（electron-builder.yml:30-31）。

#### 8.2.3 build-engine.ps1 与 build-all.ps1

`tools/build-engine.ps1`（91 行）参数为 `-Configuration`（Debug 或 Release，缺省 Release）与 `-VcpkgRoot`（缺省取环境变量 `VCPKG_ROOT`，再取仓库同级的 `vcpkg` 目录）（build-engine.ps1:17-31）。关键行为：

- cmake 优先取 PATH，找不到则在 `Program Files\Microsoft Visual Studio` 下搜索 CommonExtensions 内的 cmake.exe（build-engine.ps1:34-43）。
- 要求 `models/nvpro_core/cmake/setup.cmake` 存在，否则抛错（build-engine.ps1:48-50）。
- Release 追加 `-DHISTREAM_REQUIRE_USD=ON`，缺 OpenUSD 时配置失败（build-engine.ps1:54；CMake 侧 models/histream/CMakeLists.txt:411-414）；每次都传 `-DHISTREAM_EXTENSION_DIRS=` 清空扩展目录缓存，保证产品引擎不含外部扩展（build-engine.ps1:56；CMakeLists.txt:451）。
- STREAM 是独立 CMake 工程，沿用同一 vcpkg 工具链（build-engine.ps1:72-79）。
- 写清单需要 git，PATH 与 `GIT_EXECUTABLE` 都没有时回退到 Visual Studio 自带的 git（build-engine.ps1:83-88）。

`tools/build-all.ps1`（30 行）先执行 `npm.cmd run build:gui`，失败即以其退出码退出，再调用 build-engine.ps1（build-all.ps1:23-27）。

#### 8.2.4 write-build-manifest.mjs

用法：`node tools/write-build-manifest.mjs [运行时目录]`，缺省 `models/bin_x64/Release`（write-build-manifest.mjs:27）。输出 `build-manifest.json`，结构为 `kind = stream-studio-build`、`schemaVersion = 1`，字段包括 `applicationVersion`（取自 package.json）、`createdAt`、`commit`、`dirty`、`sourceTreeSha256`、`engine`、`runtimeBinaries`、`sources`、`shaderBinaries`、`staleShaderBinaries`、`missingShaderBinaries`（write-build-manifest.mjs:103-111）。要点：

- 源码清单 `sources`（write-build-manifest.mjs:41-57）包含 `models/histream/src` 的 cpp、h、inl，`models/histream/shader` 的各类着色器源，`models/stream` 的 cpp、h、hpp 与 `classes/classes.json`，`src/renderer` 的 js、html、css，`server.mjs`、`server/*.mjs`、6 个 tools 脚本和 package.json；`sourceTreeSha256` 是对该数组 JSON 求的哈希（write-build-manifest.mjs:107）。
- `builtShaderModules` 手写了 CMake 实际生成的 SPIR-V 清单：raytracing 4 个、facetrt 与 faceteb 的公共 7 个加各自一个，voxeleb 与 voxelrt 目录下全部 comp，外加 3 个 `_most` 变体和 `voxelrad_scattering_trace`（write-build-manifest.mjs:69-79）。注释要求与 `models/histream/CMakeLists.txt` 同步；运行目录里不在该清单内的 `.spv` 被记入 `staleShaderBinaries` 并告警，清单内缺失的记入 `missingShaderBinaries`（write-build-manifest.mjs:80-88）。
- git 不可用时 `commit` 为空串、`dirty` 为 null，只告警不失败（write-build-manifest.mjs:90-95）。
- 运行目录缺少 `shader` 目录或 `histream.exe` 时，`readdirSync` 与 `readFileSync` 会直接抛错（write-build-manifest.mjs:80、108），脚本没有容错。
- 消费方：服务端提交运行时读取 `build-manifest.json`，把 `applicationVersion` 与 `sourceTreeSha256` 写进运行清单（server.mjs:371-376）。

#### 8.2.5 前端与开发服务器

`vite.config.js`（39 行）以 `src/renderer` 为 root，输出到仓库根 `gui/` 且 `emptyOutDir: true`，开发服务监听 127.0.0.1:5173 并把 `/api` 代理到 4173（vite.config.js:19-37）。`dev.mjs`（63 行）用当前 Node 同时拉起 `server.mjs --api-only` 与 `node_modules/vite/bin/vite.js`，任一子进程退出则杀掉另一个（dev.mjs:25-27、30-36）。`gui/` 是纯构建产物，禁止手放文件。

### 8.3 打包与发布

#### 8.3.1 stage-package-assets.mjs

把 `assets/` 精简复制为 `.packaging/assets/`，并重写两个目录 JSON。核心函数调用链：

```
主流程 (stage-package-assets.mjs:139-144)
  scan(assets)                 记录 >15 MiB 的 OBJ、examples 下的 output 目录、_*_source 目录   (48-67)
  findIncompleteRamiScenes()   RAMI 场景的 manifest 中任一 OBJ 超限 → 整个场景目录排除          (70-80)
  rm(.packaging/assets); copyTree(assets → .packaging/assets)   用 excluded() 过滤                (93-109)
  rewriteObjCatalog()          从 obj-library/catalog.json 去掉被排除模型并清理 defaultSets       (112-126)
  rewriteRamiCatalog()         从 rami-library/catalog.json 去掉被排除场景                       (129-136)
```

排除规则 `excluded()`（stage-package-assets.mjs:83-91）：体积大于 `MAX_OBJ_BYTES = 15 MiB` 的 OBJ（行 20）；`obj-library/crop` 下不在白名单的 OBJ，白名单只有 `maize-growth/maize_stage_04.obj`、`wheat.obj`、`rice.obj`、`sunflower.obj`（行 27-32）；`log_nvprosample.txt`；`examples` 下名为 `output` 的目录；形如 `_*_source` 的目录；含超限 OBJ 的 RAMI 场景。本机检出中超限的有 5 个植被 OBJ（jacaranda 约 33.7 MiB，另 4 个 16 至 17 MiB）与 RAMI 场景的 `PISY10.obj`（约 18.6 MiB），所以 HET07_JPS_SUM 场景不会进入安装包；森林示例为此使用 `tools/compact-obj.mjs` 生成的蓝花楹紧凑副本（tools/generate-forest-example.mjs:49）。

注意：脚本先 `rm` 整个目标目录再复制（行 141-142），因此 `.packaging/assets` 是可随时重建的派生物。本机现存副本的 `examples/catalog.json` 不含 `beijing_multisensor`，应是该案例加入前生成的过期暂存；`pack:win` 会自动重新 stage，单独运行 `electron-builder` 则不会（见 8.12）。

#### 8.3.2 electron-builder.yml

关键配置（均在 electron-builder.yml）：`appId: cn.stream.studio`、`asar: true`、仅保留 zh-CN 与 en-US 语言包、输出目录 `release`（行 3-10）。`files` 只收 `desktop/**`、`server.mjs`、`server/**/*.mjs`、6 个 tools 脚本、3 个前端共享模块与 package.json（行 11-24）。`extraResources` 把以下内容复制到安装目录 `resources/`（行 25-70）：

| 来源 | 目标 | 过滤 |
| --- | --- | --- |
| gui | gui | 无 |
| .packaging/assets | assets | 无 |
| models/bin_x64/Release | engine | 排除 pdb、lib、exp、ilk、旧审计 exe、`shader/cases/**/*.spv`、`*.cmake`、`*.xlsx`、usd 代码生成模板、`histream_*_tests.exe` 等（行 32-50） |
| docs | docs | 仅 Stream_Studio_User_Manual_CN.pdf 与 Stream_Studio_Theory_Manual_CN.pdf（行 51-55） |
| statistic | statistic | py、mjs、md、txt，排除 `test_*.py` 与 `__pycache__`（行 56-64） |
| src/renderer/src 下 project-schema.js 与 material-specular.js | 同路径 | 在 resources 下另存一份（行 65-68），用途待核实 |
| design/branding/stream-studio-logo.ico | branding/stream-studio.ico | 无 |

目标为 `win` 的 NSIS x64，产物名 `Stream-Studio-Setup-${version}-x64-slim.${ext}`，非一键安装、可选安装目录、不在卸载时删除用户数据（行 71-89）。安装器未签名（docs/release-2.0.1.md:17）。运行时 `server.mjs` 通过环境变量 `STREAM_STUDIO_RESOURCE_ROOT` 找到资源根，桌面壳在打包后传入 `process.resourcesPath`（server.mjs:71；desktop/main.mjs:193、312-314）。

#### 8.3.3 用户交付脚本

`tools/prepare_user_release.py`（91 行，尚未实际使用，docs/development-and-release.md:11）按 `--version`、`--previous`、`--installer`、`--out`、`--start-text`、可重复的 `--add-example NAME=DIR=CASE_JSON` 生成交付目录。步骤：输出目录已存在则拒绝；安装包文件名必须含版本号；复制安装包、LICENSE、两本 PDF 手册；整体带入上一版 `examples/`；追加新案例并记录 `projectSha256`；写 `案例目录.json`、`交付清单.json`（kind 为 `streamstudio-user-delivery`）与 `SHA256SUMS.txt`（prepare_user_release.py:47-87）。LICENSE 取仓库根 `LICENSE.txt`，不存在则取上一版（行 54）；而仓库根现有文件名是 `LICENSE`，这条分支实际会落到上一版副本（待核实是否有意）。

`tools/check-user-release-cases.mjs`（87 行）是显式调用的验收脚本，不被构建脚本调用：对 MODIS-ERA5、Landsat-ERA5、MODIS-LSCT、Landsat-LSCT 四个案例，要求工程明确选择 4×4 区域，递归检查所有输入引用均为案例内相对路径并记录 SHA-256，用 `createProjectStore().buildEngineConfig` 生成配置，以 `release/win-unpacked/resources/engine/stream.exe` 运行（超时 60 s，PATH 仅含引擎目录与系统目录），再核对 `scene_summary.json` 中像元数为 16、各失败计数为 0、EB 全部收敛（check-user-release-cases.mjs:35、41-78），有失败时退出码为 1（行 87）。它声明自己不含整幅影像、GUI、全新安装或科学精度验证（行 42）。

发布与日常开发的流程约定见 `docs/development-and-release.md`：日常小改动不升版本、不构建安装包；用户发布放在 D:/releases/streamstudio/<版本>；长任务应进 `HEAVY_TASKS`（docs/development-and-release.md:5、6、15）。

### 8.4 tools/ 运行时辅助模块

| 文件 | 行数 | 职责 | 关键导出 |
| --- | --- | --- | --- |
| runtime-scene.mjs | 506 | 运行前整理“运行时场景”：实例筛选、单对象裁剪、贴地、纹理展开 | `prepareRuntimeSceneProject(inputPath, sourceProject, options)` |
| surface-textures.mjs | 231 | 把分类纹理与开孔率纹理展开为普通 OBJ 分组加光谱 | `hasSurfaceTexture`、`applySurfaceTextures` |
| png.mjs | 84 | 最小 PNG 解码器，仅 8 位、非隔行，不校验 CRC | `decodePng(buffer, label)` 返回宽、高、紧凑 RGB |
| tarp-cover.mjs | 134 | 为任意 OBJ 生成下垂篷布网格 | `generateTarpCover(objText, options)` |
| usd-export.mjs | 592 | 工程导出 ASCII OpenUSD（.usda） | `exportProjectUsd`、`USD_EXPORT_FORMAT = histream-usd-v1` |
| usd-import.mjs | 182 | USD 清单转回工程对象列表 | `buildProjectFromUsd` |
| modtran-lut.mjs | 191 | 调用本机 MODTRAN 5.2.1 生成稀疏大气 LUT CSV | 无导出，命令行脚本 |

#### 8.4.1 runtime-scene.mjs

唯一入口 `prepareRuntimeSceneProject`（runtime-scene.mjs:287）由 `server.mjs` 的 `runtime-scene` 任务调用（server.mjs:5506），也被回归测试的 `prepare.mjs:95` 调用。`options.resolveAssetPath` 缺省按工程目录解析，`options.terrainHeight` 缺省平地 0 m。返回对象含 `inputPath`（运行时 project.json）、`project`、`textureSummary`、`total/kept/excluded/invalid`、`clippedObjects`、`sourceTriangles/retainedTriangles`、`grounded` 与 `cleanup()`（runtime-scene.mjs:488-501）。调用链：

```
prepareRuntimeSceneProject
  ├─ normalizeProject；建 .histream-runtime-<token> 临时目录                      (287-331)
  ├─ 路径绝对化（outDir、DEM、气象、LUT、光谱及纹理、对象 OBJ 与位置文件）        (332-352)
  ├─ 逐对象（355-450）
  │    ├─ 无位置文件：原样保留
  │    ├─ positionRecords 读位置 → primitiveBounds / objBounds 求局部包围盒
  │    ├─ isGroundedObject → groundedPlacementHeight 覆盖高度（车辆贴地）          (389-396)
  │    ├─ transformedBounds + intersectsDomain 筛选实例，余者计入 excluded         (398-402)
  │    ├─ 仅“单对象单实例且越界”时 clippedObjContent 裁剪 OBJ → object-N.obj       (405-428)
  │    └─ 位置减去 offset，写 position-N.txt                                     (430-449)
  ├─ applySurfaceTextures（surface-textures.mjs）                                 (453-465)
  ├─ effectivePorousSpectrum 展开开孔率光谱，必要时写 spectrum-N.txt              (467-475)
  └─ 对象列表替换、offset 归零、写 project.json                                    (478-487)
```

数据约定：OBJ 为 Y 向上；计算域 X 属于 offsetX 到 offsetX 加 scene.x，高度属于 offsetY 到 offsetY 加 scene.height，Z 属于 offsetZ 到 offsetZ 加 scene.y，即工程的 `scene.y` 对应运行时 Z（runtime-scene.mjs:13-21、299）。位置文件每行前三列读成 x、z、y 后又按同序写回，与 `usd-export.mjs` 的 `readPlacements` 列顺序一致（runtime-scene.mjs:17-20、440-446）。出错时函数自行 `cleanup()`，成功时由调用方负责清理（行 285-286、502-505）。

#### 8.4.2 surface-textures.mjs 与 png.mjs

`applySurfaceTextures({ items, spectra, readAsset, objectPath, readObject, writeObject })` 通过回调读写文件，因此与具体路径解耦（surface-textures.mjs:115）。分类纹理：对每个纹素在各类别颜色中取 RGB 欧氏距离最近者，预算成类别图；被引用的类别光谱不得再启用分类纹理（行 127-146）。开孔率纹理：纹素亮度取 RGB 平均，量化为 `levels` 档（限制 2 到 32，缺省 8），线性映射到 `minOpenness` 与 `maxOpenness`（上限 0.95），派生光谱命名为 `<原名>__open<档号>`（行 150-181）；Prospect 与 BSM 模型光谱不支持开孔率纹理（行 167）。面重新归入 `<原分组>__<光谱名>` 组并改写对象 `meshes` 绑定（行 205、212-226）。采样用 OBJ 局部坐标与面法线主导轴，周期平铺（行 44-55）。

#### 8.4.3 tarp-cover.mjs

`generateTarpCover(objText, { cell, lift = 0.05, drape = 6, margin = 0.15, maxNodes = 400000 })`（tarp-cover.mjs:19）：网格间距缺省取最大水平尺寸除以 80 并限幅到 0.05 至 0.5 m（行 35）；网格点数超过 `maxNodes` 抛错（行 42）；把目标三角形按约半个网格间距重心采样并盖到四个邻点作为高度场（行 45-64）；再用两遍倒锥形最大值传播加两遍只升不降的平滑模拟布料下垂（行 70-93）；只输出被覆盖格及其 8 邻域并三角化（行 95-118）。返回 `obj`、顶点数、三角形数、`cell`、`targetSize`、`targetFootprint`、`dimensions`、`topCells`（行 126-133）。调用方：`server.mjs:5493` 的篷布覆盖接口、命令行 `tools/generate-tarp-cover.mjs`。

#### 8.4.4 usd-export.mjs 与 usd-import.mjs

`exportProjectUsd(project, options)` 的 `options` 含 `outputPath`、`assetsDirectory`、`resolveAssetPath`、可选 `heightAt` 与 `sourceProjectFile`，返回 `{ path, prototypes, meshes, faces, instances, skippedObjects, materials }`（usd-export.mjs:337-342）。先写 `<输出>.tmp` 再改名（行 359-361）；根层 `customLayerData` 内嵌完整工程 JSON（行 364-378）；每个工程对象一个 `class` 原型，无 OBJ 的对象用 Cube 或 Sphere（行 398-403）；轴约定为 Y 向上、X 北、Z 东。`buildProjectFromUsd(baseProject, manifest, options)` 是逆过程：先用内嵌工程补全缺失的光谱、热、材料、冠层记录（usd-import.mjs:62-68），再为每个网格组按“USD 属性、同名模板、同光谱兄弟网格、缺省”的优先级选绑定，单对象最多保留 100000 个实例（行 30、85-86）。两者在服务端由 `usd-export` 任务与导入接口使用（server.mjs:2689、2747、5516）。往返一致性由 `tools/test-usd-roundtrip-20261125.mjs` 守护。

#### 8.4.5 modtran-lut.mjs

命令行 `node tools/modtran-lut.mjs [--root MODTRAN 目录] [--output csv]`，MODTRAN 目录缺省取环境变量 `MODTRAN_ROOT`，再取 `D:\work\MODTRAN5.2.1`（modtran-lut.mjs:30）。采样轴为 3 种大气、2 种气溶胶、3 档水汽、3 档能见度、3 档传感器高度、3 档观测天顶角，共 486 次运行（行 37-49、144-145）；每次写 tp5、改写 `mod5root.in`、调用 `Mod5.2.1cons.exe`、解析 tp7 并重采样到 350 至 2500 nm（5 nm 步长）与 2525 至 14000 nm（25 nm 步长）（行 112-115），输出 9 列 CSV。`finally` 中恢复原 `mod5root.in`（行 188-191），运行期间不可并行。默认输出 `assets/atmosphere/simple_modtran_lut.csv`（行 33）。

### 8.5 资产与示例生成脚本

| 文件 | 行数 | 职责与输出 |
| --- | --- | --- |
| build-typical-obj-library.mjs | 434 | 导入 Quaternius 车船素材并程序化生成公交、货轮等，写 obj-library 下 typical 目录及同名 json；会覆盖同名文件，素材取自仓库外 `.tmp-asset-import` |
| import-realistic-models.py | 361 | 下载 Poly Haven 与 Khronos 的 glTF，简化为带顶点色的 OBJ，写 SOURCE.txt；需联网与 numpy、trimesh |
| import-rami-het07.mjs | 89 | 把 RAMI-V HET07_JPS_SUM 解压目录转为 Y 向上 OBJ、位置文件与 manifest，写入 assets/rami-library |
| generate-dem-library.mjs | 71 | 生成 4 个 65×65 的 ESRI ASCII 合成 DEM（平地、山丘、盆地、丘陵） |
| generate-forest-scene.mjs | 115 | 固定随机种子生成 100 m 森林整景 OBJ（forest_demo_100m.obj） |
| generate-forest-example.mjs | 256 | 生成 assets/examples/forest（VoxelEB），加 `--vehicles` 生成含 11 辆车的 RayTracing 变体 |
| generate-crop-example.mjs | 220 | 生成 assets/examples/crop_gpp_npp（12×12 株玉米，VoxelEB） |
| generate-tarp-example.mjs | 346 | 生成自包含的篷布、遮阳棚、车辆 RayTracing 示例 |
| generate-tarp-assets.py | 245 | 生成篷布、遮阳网、沙地的合成光谱与周期纹理，并更新两处 catalog.json |
| generate-tarp-cover.mjs | 27 | tarp-cover.mjs 的命令行包装 |
| vehicle-lineup.mjs | 236 | 荒漠与森林示例共用的 11 车阵列：`addVehicleLineup`、`shadeCanopyObj` |
| compact-obj.mjs | 55 | 去法线与纹理坐标、焊接同位置同颜色顶点，减小 OBJ 体积 |
| normalize-obj-scale.mjs | 34 | 就地均匀缩放并把底部对齐到 Y=0 |
| export-example.mjs | 50 | 把示例导出为可独立运行的目录并补齐引用的 assets 文件 |
| export-faceteb-frames.mjs | 64 | 用 server.mjs 的 facet-step 工作线程把 FacetEB 节点渲染为 512×512 温度 TIFF |
| render-logo-preview.mjs | 32 | 用 Electron 离屏窗口把 SVG 标志渲染为 PNG |

调用关系：`generate-forest-example.mjs` 依赖 `createDefaultProject`、`stringifyProject`、`validateProject`，`generate-crop-example.mjs` 至少依赖前两者（generate-crop-example.mjs:22）；森林脚本再依赖 `vehicle-lineup.mjs` 与 `compact-obj.mjs`（generate-forest-example.mjs:30-31、49）。`generate-forest-example.mjs` 缺省输出到 `assets/examples/forest`，`--vehicles` 缺省写本机路径 `D:/cases/streamstudio/vehicle-tarp/forest`，可用命令行目录或环境变量 `STREAM_STUDIO_VEHICLE_FOREST_DIR` 覆盖（generate-forest-example.mjs:37-40）。`generate-crop-example.mjs` 需要已构建的 `models/bin_x64/Release/defined/meteo.txt`（文件头 14-15 行）。

`vehicle-lineup.mjs` 的契约：`spots` 必须正好 11 个，顺序为 1 个裸车、5 个棚下车位、5 个篷布车位（vehicle-lineup.mjs:174、178）；车辆网格组名 Windows、Black、Grey、Headlights、TailLights 与 `part_N__covered_car` 必须与 `project-schema.js` 的车辆绑定规则一致（文件头 18-20 行）；`energyBalance` 为真时棚布与篷布使用 `tarp_fabric` 材料，其余模式用 `vehicle_material`（行 172-173、197-198）。调用前 `exampleRoot/models` 与 `positions` 目录须已存在（行 170-171）。

子目录与结果读取器：

| 路径 | 内容 | 性质 |
| --- | --- | --- |
| tools/case005（11 个文件） | pv_case005.py 准备与运行两个 eFacetEB 场景；pv_media005.py、pv_layout005.py、refine_spatial005.py 出图与 0.25 m 补算；export_case005.mjs 补渲染热红外 TIFF；publish、compact、verify、finalize 四个发布与校验脚本；case005_resources.py 与 case005.* 为网页资源 | 一次性案例脚本，数据根为仓库外 `D:/cases/StreamStudio/case005`，不可在干净检出上直接运行 |
| tools/stream-inverse | stream_inverse.py（465 行）：autograd 可微单像元热红外正向链与 Gauss–Newton 反演；命令行 `--demo`、`--export-sample`、`--sample`；样本 JSON 结构见其 README | 研究原型，尚未并入 stream.exe |
| tools/histream-ml（`hsml/` 四个模块 + 六个入口脚本） | `params.py` 参数空间（v1/v2）与写入工程/气象文件；`engine.py` 运行 `histream.exe eFacetEB`、压成类别均值观测向量、读引擎内切线敏感度（`control.facetSensitivity`）为雅可比；`surrogate.py` MLP 集成训练/评估、交叉验证残差与收缩协方差、ONNX 导出；`inversion.py` 白化 MAP、Laplace 后验、信赖域引擎校正、只用引擎的 Levenberg–Marquardt；入口 `gen_dataset.py`、`train_surrogate.py`、`check_gradients.py`、`invert.py`、`osse_stats.py`、`osse_engine.py` | 研究原型，只覆盖 `feb_crop`；不接服务端；需要独立 venv 安装 PyTorch，见其 README |
| tools/facet_result.py、facet_document.py | FacetRT 与 FacetEB 结果读取：`facet_result.load(path, mmap)` 读 JSON 或二进制，`facet_document.read_document(path, only, mmap)` 读结果文档，格式见 docs/facet-binary-output.md | 被 regression/compare.py 引用（compare.py:36-37） |

`validate-obj-library.mjs`（53 行）与 `check-facet-par-output.py`（45 行）属检查类：前者逐个 catalog 模型核对文件存在、顶点与三角形非零、三角形数（每个 f 行按边数减 2 累计）与 catalog 一致（validate-obj-library.mjs:22-36），并检查 `defaultSets` 引用的模型存在且 `recommended` 为真（行 39-46），失败时退出码为 1（行 48-51）；后者检查 `process/facetpar_T=*.json` 的格式与物理边界。`compare-facet-outer-stomata.py`（121 行）对比同一 144 株作物场景的多次 FacetEB 运行，场景常量硬编码（958 面元每株、中心 9 株、面元 62764、正午 token）。

### 8.6 测试

#### 8.6.1 npm test 包含什么

`npm test` 只运行 4 个 Node 文件（package.json:16），均不需要引擎、浏览器或 GPU：

| 文件 | 覆盖内容 | 主要被测模块 |
| --- | --- | --- |
| test-review-fixes-20261126.mjs | validateProject 拒绝非正场景尺寸；ByteLru.resize 不超限；STREAM RT 转发 diffuseFraction 并拒绝摄氏度组分温度；/api/results/read 拒绝任意绝对路径（返回 400） | project-schema.js、server/result-cache.mjs、server/stream/config.mjs、server.mjs |
| test-server-fixes-20261125.mjs | ENVI 头解析 `key={...}`；convertEnviToTiff 与 Voxel 模式删除 ENVI 原文件；输出文件名时间与角度标记对 5000 个随机值与旧实现逐值一致；import-dem 的 64MB 上限 | server.mjs 导出的 enviMetadata、convertEnviToTiff、outputTimeToken、outputAngleToken、outputTiffName |
| test-facet-binary-reader-20261126.mjs | 合成同一份面元结果，分别按 JSON 与二进制写盘，核对 readFacetResult、sampleFacetResult、isFacetResultFile、facetResultCount 一致；旁路文件列出、计大小、复制、删除；损坏输入报错；/api/results/read 两种格式一致且旁路文件改动使缓存失效 | server/facet-result.mjs、server.mjs |
| test-usd-roundtrip-20261125.mjs | offsetY 为 0 与 7、有地形时，贴地车辆与普通对象高度经 USD 导出再导入不漂移，再次导出的实例平移与首次相同 | usd-export.mjs、usd-import.mjs |

这些文件在导入 `server.mjs` 前设置 `STREAM_STUDIO_SERVER_AUTOSTART=0`，部分还设置 `STREAM_STUDIO_DATA_ROOT` 指向临时目录（test-server-fixes-20261125.mjs:20-21；test-facet-binary-reader-20261126.mjs:24-25；test-review-fixes-20261126.mjs:60）。需要监听端口的用例在进程内监听 127.0.0.1 的随机端口。

#### 8.6.2 npm test 之外的测试

Node 类，运行方式均为 `node --test <文件>`，不含引擎：

| 文件 | 覆盖内容 |
| --- | --- |
| test-assimilation-config.mjs | buildEngineConfig 对同化选项的转发与校验、mapStreamScenePaths 改写路径、RT 土壤发射率与反射天空选项 |
| test-stream-server-bugs-20261124.mjs | EB 能量参数校验与未知字段只告警、iceMelt 仅显式 false 下发、假引擎下 stop 使排队与运行任务以 stopped 收尾、缓存键含 ctimeMs、rasterBand 统计覆盖全部像元 |
| test-stream-server-review-20261124.mjs | 过境年份 1900 至 2200、soilBrightness 不得静默丢弃、maxIterations 须为正整数、非法运行请求不留输出目录、非 ASCII ERA5 路径检查 |
| test-stream-server-tools-20261124.mjs | 服务端发出的每个 energy 键须被引擎 config.cpp 读取且缺省值一致；rasters.inspectRasters、rasterBand、classStatistics；路由在无引擎时的行为 |
| test-stream-forcing-server.mjs | 气象强迫元数据 API、路径穿越与超大输入拒绝、工程字段透传、元数据保存读取往返；需要真实 stream.exe 的用例在缺引擎时自动跳过（test-stream-forcing-server.mjs:35、71、82、196），另含用假引擎模拟崩溃与成功的 S6 用例 |
| test-vehicle-parts.mjs | 车辆网格绑定表、splitVehicleObj 拆组、车体材料、validateProject 与 projectToXml 部件标签；直接 `node tools/test-vehicle-parts.mjs` 运行 |
| stream-server-test-helpers.mjs | 非测试文件，提供 `makeService`、`fakeEngine`、`waitFor`，被两个 stream-server 测试使用（stream-server-test-helpers.mjs:17、36、46） |

Python 类（pytest，调用真实 `models/bin_x64/Release/stream.exe`，需要 GDAL、numpy；环境变量 `STREAM_RUNTIME` 指引擎目录，`STREAM_FIX_CASE` 指算例目录，缺省 `assets/examples/modis_beijing_eb`，test-stream-fixes-20261121.py:37-38）：

| 文件 | 覆盖内容 |
| --- | --- |
| test-stream-fixes-20261121.py | 地形下 LST 与 LSCT 反演闭合、LST 不确定度、冠层参数报告、土壤阻力校验、spin-up 行为、同化代价与停止原因、ERA5 缺变量；同时是其他 stream 测试按路径导入的辅助库（`run`、`read`、`rt_config`、`eb_config`） |
| test-stream-review-20261124.py | LAI 填充码、极冷场景温度下限、配置校验、收敛性回退；提供 `REGION` 与 `clone` |
| test-stream-bugs-20261124.py | C4、C5、A2、I1 与 I2、L5、冰雪融化；用 importlib 按路径加载上两个文件 |
| test-assimilation-contract.py | 同化与热红外正演契约，1×1 像元合成栅格；跨仓库 LS4T 用例在缺 `LS4T_ROOT` 时跳过 |
| test-stream-jacobian.py | 构建并运行 `stream_jacobian_check`，对比自动微分残差对 LAI 的导数与有限差分；`build/stream` 不存在时跳过，找不到 cmake 时失败（test-stream-jacobian.py:72-76） |
| test-forcing-metadata.py | 元数据驱动气象强迫端到端，缺依赖或引擎时输出 SKIP 并以 0 退出，用 `python -I` 运行 |
| test-vehicle-cabin.py | 车辆座舱热平衡离散方程的纯 Python 复核，失败退出码 1 |
| test-facet-collatz.py | 从 facetrtcore.cpp 抽取 Collatz 气孔阻抗函数拼成程序并用 `cl` 编译运行，需 VS x64 开发者命令行 |

C++ 类（单文件可执行程序，输出 PASS 与 FAIL 并以退出码报告）：

| 文件 | 覆盖内容 | 构建方式 |
| --- | --- | --- |
| test-facet-lumped-thermal.cpp | 集总热体解析解、能量闭合、输入校验 | CMake 目标 `histream_lumped_thermal_tests`（models/histream/CMakeLists.txt:534-537） |
| test-lidf-gamma.cpp | 叶倾角分布、G 函数与 Gamma 函数数值自检 | 目标 `histream_lidf_gamma_tests`（CMakeLists.txt:539） |
| test-urban-aerodynamics.cpp | 城市街谷风、屋顶交换系数与显热闭合的数值自检（仅依赖 `urban_aerodynamics.h`） | 目标 `histream_urban_aerodynamics_tests` |
| test-diffuse-directions.cpp | 漫射 64 方向格点的射线效应评估，不需 GPU | 目标 `histream_diffuse_directions_tests`（CMakeLists.txt:544） |
| test-voxel-limits.cpp | 体元规模上限检查，需 Vulkan 设备 | 目标 `histream_voxel_limits_tests`，链接 Vulkan（CMakeLists.txt:548-551） |
| test-facet-energy-convergence.cpp、test-facet-nested-budget.cpp、test-facet-sparse-aerodynamics.cpp | 能量收敛判据边界、耦合求解器工作量预算、稀疏冠层空气动力学 | 仅依赖 energyphysics.h；CMake 中未发现对应目标，构建命令待核实 |

上述 CMake 目标均为 `EXCLUDE_FROM_ALL`，默认构建不会编译它们；`stream_jacobian_check` 同理（models/stream/CMakeLists.txt:48-49）。示例构建命令：`cmake --build build\histream --config Release --target histream_voxel_limits_tests`（test-voxel-limits.cpp:7）。

#### 8.6.3 运行速查

```
npm test                                                  # 默认 4 个 Node 文件，无需引擎
node --test tools/test-stream-server-bugs-20261124.mjs    # 其余 Node 文件同理，单文件运行
node tools/test-vehicle-parts.mjs                         # 非 node:test，直接执行
python -m pytest tools/test-stream-fixes-20261121.py      # 需 GDAL、numpy 与 Release 版 stream.exe
python -I tools/test-forcing-metadata.py [--exe ...] [--keep]   # 缺依赖时 SKIP 并以 0 退出
python tools/test-vehicle-cabin.py                        # 纯 Python
cmake --build build\histream --config Release --target histream_lidf_gamma_tests
npm run validate:assets                                   # OBJ 库目录校验
```

Python 解释器路径在文件头多写作 `C:/work/miniconda/python.exe`（如 test-assimilation-contract.py:5），属本机约定，换环境时自行替换。引擎侧测试结果对 `models/bin_x64/Release` 的构建状态敏感：先按 8.2 构建，再运行 pytest。

#### 8.6.4 端到端验证脚本

`review/run-examples.mjs`（223 行）与 `review/run-one-project.mjs`（155 行）只通过 HTTP API 驱动已启动的服务，分别运行全部内置示例或单个工程，结果写入忽略目录 `.test/examples-run/<时间戳>`，超时由 `RUN_TIMEOUT_MS` 控制；`review/compare-summary.mjs`（76 行）递归比较两份 JSON 的数值差异，只打印、不设退出码。

### 8.7 tools/regression：HiStream 引擎回归

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| cases.mjs | 406 | 算例表 `CASES`（94 项）与 `FACET_OUTPUT`；字段 example、mode、meteo、positions、files、set、addSpectra、addMaterials、addObjects |
| prepare.mjs | 111 | 生成冻结输入：复制示例、规范化、套用算例、调用 `prepareRuntimeSceneProject`、冻结气象 |
| run.mjs | 85 | 用指定引擎目录逐算例运行，记录退出码与耗时 |
| compare.py | 260 | 对比两个变体：TIFF 波段统计、VoxelEB 分类型能量项、FacetEB 收敛、FacetRT 波段统计 |
| rasterio_lite.py | 166 | 仅依赖 numpy 的 GeoTIFF 与 ENVI 读取器 |
| README.md | 38 | 用法与已知基线差异说明 |

算例按名称前缀对应五种模式：rt 11 个、vrt 15 个、veb 26 个、frt 6 个、feb 36 个，共 94 个（对 cases.mjs 的实际枚举结果）；STREAM 不在此范围（cases.mjs:17）。用法三步（tools/regression/README.md:15-27）：

```
node tools/regression/prepare.mjs <workDir> [算例名 ...]            # 生成 <workDir>/cases/<算例>/{project,input,case.json}
node tools/regression/run.mjs <workDir> <variant> [--engine <binDir>] [算例名 ...]
python -I tools/regression/compare.py <workDir> <A> <B> [--json out.json] [--tolerance 1e-4]
```

调用链：`prepare.mjs` 对统一格式工程先经 `unifiedToHistreamProject` 再 `normalizeProject`（prepare.mjs:64-67），套用 `spec.set` 的点分路径覆盖（行 38-43、74），用 `terrainHeight: () => 0` 调用 `prepareRuntimeSceneProject` 以排除 DEM 影响（行 95），把运行时目录复制到 `input/` 并把气象文件冻结为 `input/meteo.txt`（行 96-108）。`run.mjs` 把 input 复制到 `runs/<variant>/<算例>`，改写 `outDir` 与气象路径，应用环境变量 `REGRESSION_SET`（形如 `a.b=1;c.d=x`，值先按 JSON 解析）后启动 `histream.exe <模式> <project.json>`，FacetEB 与 FacetRT 额外传结果索引文件名 `faceteb.json` 或 `facetrt.json`；超时由 `REGRESSION_TIMEOUT` 控制，缺省 900 s；并固定 `HISTREAM_FACET_STEP_SYNC=0`（run.mjs:50-80）。`compare.py` 的 `--tolerance` 只门禁 TIFF 波段均值、VoxelEB 的 T 与 Rn、FacetRT 波段均值，FacetEB 收敛与迭代数差异标注 `[CHANGED, not gated]` 只报告（compare.py:15-17）。

约束：示例或 `prepare.mjs` 规范化逻辑变化后旧基线不可比；RayTracing 默认 Owen 置乱 Sobol 采样，与旧基线逐位比较须设 `HISTREAM_RT_SAMPLER=legacy`；体元默认跳空节点，逐位比较须设 `HISTREAM_VOXEL_SKIP=off`（tools/regression/README.md:36、38）。

### 8.8 statistic：独立统计工具

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| histream_statistics.py | 1022 | 命令行统计与查看：TIFF 逐波段统计、体元与面元过程结果的字段统计、抽样点 CSV、热力图、直方图、三维结构图、按土壤、植被、建筑分组统计 |
| scene_example_report.py | 171 | 复用上者的 `colorize`、`draw_colorbar`、`load_font`、`read_structure`，生成建筑与森林对照图、汇总 CSV 与 TXT |
| run_scene_examples.mjs | 99 | 端到端演示：造两个 eFacetEB 单时刻场景、运行 histream.exe、调用统计与报告脚本 |
| inputs/scene-example-meteo.txt | - | 演示用气象 |
| requirements.txt | 2 | numpy>=1.24、Pillow>=10.0 |

命令行：`python statistic\histream_statistics.py <输入> [-o 输出目录] [--fields] [--bands] [--facet-side mean 或 front 或 back] [--max-points 50000] [--components soil,vegetation,building 或 none]`（histream_statistics.py:970-982）。输入可为 TIFF、过程 JSON 或整个目录；单个文件失败不阻断批处理。TIFF 读取按 rasterio、Pillow、内置未压缩条带读取器三级回退（histream_statistics.py:444-517）。每个输入产生独立子目录，含 `statistics.csv`、`summary.txt`、`sample_points.csv` 与三类 PNG；CSV 为 UTF-8 BOM。统计使用全部有效值，抽样只影响点表与三维图；能量字段为通量，均值不按面积加权（statistic/README.md:76、91）。`run_scene_examples.mjs` 优先使用 `engine/histream.exe`，其次 `models/bin_x64/Release/histream.exe`（run_scene_examples.mjs:29-32），用环境变量 `STREAM_STUDIO_PYTHON` 指定 Python。服务端不引用该目录，它仅作为独立工具随包分发（electron-builder.yml:56-64）。

### 8.9 文档与 PDF 生成脚本

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| build-core-manuals-pdf.py | 756 | 把 docs/user-manual.md 与 theory-manual.md 排版为带封面、目录、书签的 PDF，写 docs/Stream_Studio_User_Manual_CN.pdf 与 Stream_Studio_Theory_Manual_CN.pdf |
| build-example-manual-pdf.py | 686 | 手写版面的《功能与案例手册》，从 Word 文档提取截图拼面板；源文档路径可由首个参数或 `STREAM_STUDIO_MANUAL_SOURCE_DOCX` 覆盖 |
| build-beijing-multisensor-report.py | 740 | 生成 docs/cases/beijing-multisensor/report.pdf，数值全部由案例 results 与 tables 计算 |
| plot-beijing-multisensor.py | 657 | 生成该报告的配图，`--source reference`（缺省，仅用仓库内精简结果）或 `run`；每张图尺寸断言小于 3000 px |
| plot-faceteb-day.py | 245 | 玉米场景 48 节点 FacetEB 结果的俯视温度图、时间序列、GIF 与 CSV |

`build-core-manuals-pdf.py` 的解析约定：第 1 行总标题被跳过，`##` 为目录一级，`###` 为二级，`####` 只排版不入目录，表格为竖线分隔且列等宽；`$$` 公式若存在 `docs/figures/equations/<sha256>.svg` 则用矢量图，否则退化为纯文本（文件头 15-16 行）。依赖 reportlab，字体依赖 Windows 的 Deng 与 Dengb。因此编写手册 Markdown 时，单元格内不得出现未转义竖线。该脚本默认生成使用手册与理论手册两本 PDF；带参数 `runtime` 生成《运行原理说明》（源文件 docs/runtime-principles.md），带参数 `modules` 生成《代码模块说明》（源文件 docs/code-modules.md）。后两者开启按内容长度分配表格列宽。表格单元格中的 `\|` 与反引号内的竖线不会被当作列分隔符。

### 8.10 assets 目录结构与索引文件

assets 共约 290 个文件、311.6 MiB（本机统计），子目录与索引如下：

| 子目录 | 内容 | 索引与约定 |
| --- | --- | --- |
| examples | 6 个内置示例：crop_gpp_npp、forest、urban_block、landsat_beijing_eb、modis_beijing_eb、beijing_multisensor | `catalog.json`（version 1，examples 数组，字段 id、label、project、recommendedMode、focus、category、tier、note、categoryLabel）；每个示例含 project.json、README.md，HiStream 示例另有 meteorology、positions、models，遥感示例另有 era5、inputs、reference |
| obj-library | 按类别的 OBJ 模型：aircraft、building、crop、large_scene、ship、solar、special_vehicle、terrain、vegetation、vehicle | `catalog.json`（version 5，Y 向上，单位 m，`categoryOrder`、`qualityTiers`、`defaultSets`、61 个 models）；另有 README、ASSET_AUDIT、REALISTIC_MODELS、SOURCES 与 entity_0/1_position.txt |
| spectral-library | 23 个光谱 txt | `catalog.json` 列 20 项（目录与文件数不一致，部分 txt 未登记，待核实），`usage` 为 quick-test-only，波长单位 nm、值为无量纲分数；篷布与遮阳网类 txt 为三行（波长、反射率、透射率），由 `generate-tarp-assets.py` 生成，其余文件格式待核实 |
| texture-library | 方差纹理 jpg、分类纹理与开孔率纹理 png | `catalog.json`（schemaVersion 1，17 条，含 sha256、suggestedRepeatSize）；`defaultEnabled` 为 false，纹理均值在运行时被去除，不替代材料均值光谱 |
| dem-library | basin、dune、flat、hill、rolling、slope 6 个子目录 | 无 catalog，仅 README；四个 65×65 的 asc 由 `generate-dem-library.mjs` 生成，dune 与 slope 的文件另有来源 |
| rami-library | RAMI-V HET07_JPS_SUM 场景 | `catalog.json` 的 scenes 指向各场景 manifest.json；由 `import-rami-het07.mjs` 导入 |
| atmosphere | simple_modtran_lut.csv | 由 `modtran-lut.mjs` 生成，被工程 `atmosphere.lutFile` 缺省引用（modtran-lut.mjs:3-4） |

obj-library 目录条目 `id、label、category、objectType、file、dimensions、triangles、sourcePack、license` 为基本字段，推荐模型另有 `qualityTier、recommended、usage`，部分有 `spectralPreset、thermalPreset、materialPreset`（assets/obj-library/catalog.json:21-37）。`defaultSets` 定义太阳能设施、植被、车辆、船舶的 typical 列表与 simple 单项（同文件 11-16 行）。stage 阶段会据排除结果改写 `defaultSets`（stage-package-assets.mjs:119-124），因此改动 `defaultSets` 时 id 必须真实存在，`validate-obj-library.mjs` 会校验。

示例目录里的 `output/` 属生成物，被 `.gitignore:28` 与 stage 规则同时排除；工程内相对路径以示例目录为基准，引用共享库时写成以 `assets/` 开头的路径，由服务端解析到资源根（generate-forest-example.mjs:48；regression/prepare.mjs:85-92 的同类逻辑）。仓库根 `tests/research-cases` 存放研究案例工程，输入按内容去重、路径均为相对路径、必须整体复制到纯英文可写路径使用（tests/research-cases/README.md:5），与被忽略的 `.test/` 不是同一位置（docs/development-and-release.md:8）。

### 8.11 docs 目录文档索引

| 类别 | 文件 | 用途 |
| --- | --- | --- |
| 核心手册 | user-manual.md、theory-manual.md 及对应 PDF | 用户手册与理论手册，PDF 随安装包 |
| 运行说明 | runtime-principles.md 及 PDF | 运行原理，章节 0 至 7 与附录 A 代码模块索引 |
| 发布与开发约定 | development-and-release.md、release-2.0.1.md、release-next-draft.md、example-library.md、testing/ | 开发与发布边界、2.0.1 说明、下一版草稿、案例库目录、验收记录 |
| 引擎专题 | facet-binary-output.md、facetrt-rayquery-visibility.md、raytracing-variance-reduction.md、voxel-adaptive-and-direction.md、voxel-proxy-geometry.md、histream-terrain-environment.md、terrain-methods.md、vehicle-thermal.md、photovoltaic.md | 面元二进制格式、可见性后端、降方差、体元跳空与方向旋转、代理网格、地形与车辆、光伏的当前设计 |
| 历史记录 | faceteb-gpu-energy.md、facetrt-gpu-spectral.md、model-remaining-issues-20261005.md | 文首标注为历史实现记录，其中命令不再作为操作步骤 |
| STREAM 专题 | forcing-metadata.md 与 .schema.json、lst-lsct-inversion.md、stream-lst-lsct-requirements.md、stream-assimilation-ui.md、stream-examples.md、stream-scope.md、stream-simple-modes.md、stream-unified-workflow.md、stream-workflow-presets.md、ls4t-interop-20261106.md | 气象强迫元数据规格、反演与同化方法、工作流设计与职责边界 |
| 子目录 | cases、figures、superpowers/plans、testing | 图文案例库（catalog.json 记录案例与适用范围，manifest.json 记录各工程文件的 SHA-256）、手册配图与公式矢量图、历史计划、验收报告 |

`docs/cases/005-photovoltaic` 由 `tools/case005` 的脚本发布，`verify_promotion005.py` 按 `promotion-manifest.json` 复核哈希。

### 8.12 修改指南与注意事项

1. 想新增服务端依赖的 tools 脚本：同时修改 `electron-builder.yml:15-23` 的 `files` 与 `write-build-manifest.mjs:52-53` 的指纹清单；`server/**` 下只有 `.mjs` 会被打包（electron-builder.yml:13），新文件用 `.js` 会漏包。
2. 想新增或改名着色器：同步改 `write-build-manifest.mjs:69-79` 的 `builtShaderModules` 与 `models/histream/CMakeLists.txt`，否则清单出现 stale 或 missing 告警。
3. 想改引擎构建参数：改 `tools/build-engine.ps1`；Debug 产物落 `models/bin_x64/Debug`，不被打包，清单也写到该目录。
4. 想减小安装包或调整收录范围：改 `stage-package-assets.mjs:20、27-32、83-91`（体积上限、作物白名单、排除规则）与 `electron-builder.yml` 的 `extraResources` 过滤；超限 OBJ 要同时提供紧凑副本（如 `compact-obj.mjs`），并确认目录 JSON 重写后 `defaultSets` 仍成立。
5. 想只重新打包：`pack:win` 已包含 stage 与 build:all；单独执行 `electron-builder` 不会刷新 `.packaging/assets`，本机现存副本已过期。
6. 想增加默认回归测试：把文件追加到 `package.json:16` 的 `node --test` 列表；需要引擎、GPU、GDAL 的测试不应加入，并要像 `test-stream-forcing-server.mjs:35` 那样在缺引擎时跳过；导入 `server.mjs` 前必须先设 `STREAM_STUDIO_SERVER_AUTOSTART=0`。
7. 想新增 pytest 测试：复用 `test-stream-fixes-20261121.py` 的 `run`、`eb_config`、`rt_config`，用 `STREAM_RUNTIME` 指向引擎目录；文件名含连字符，导入需按路径（importlib）。
8. 想新增引擎回归算例：在 `tools/regression/cases.mjs` 的 `CASES` 增加条目（名称前缀 rt、vrt、veb、frt、feb 决定模式归类），Facet 模式结果文件名由 `FACET_OUTPUT` 给出；改完需重跑 prepare，并重做基线。
9. 想验证性能改动不改结果：用 `compare.py --tolerance 1e-4`，但 FacetEB 收敛与迭代数变化不会触发失败，需人工看 `[CHANGED, not gated]` 汇总。
10. 想更新内置示例：改对应 `generate-*.mjs` 后重新生成，确认 `assets/examples/catalog.json` 与示例 README 同步；示例变化会使回归基线失效。
11. 想新增 OBJ 模型：放入 `assets/obj-library/<类别>/`，在 `catalog.json` 补条目并写准确的 `triangles`，运行 `npm run validate:assets`；该校验只检查目录到文件方向，不会发现未登记的 OBJ。
12. 想重新生成大气 LUT：确认 MODTRAN 目录无并行任务，运行 `npm run lut:modtran`（脚本会临时改写 `mod5root.in`，结束时自动恢复，modtran-lut.mjs:188-191）；采样轴改动会改变运行次数与 CSV 行数，需同步确认引擎读取该 CSV 的列约定。
13. 想新增纹理或光谱类资产：写入对应库并更新 `catalog.json`，纹理 `sha256` 须与文件一致（generate-tarp-assets.py 会自动计算）。
14. 想改手册或理论文档：修改 `docs/user-manual.md` 或 `theory-manual.md` 后运行 `build-core-manuals-pdf.py` 重新生成 PDF，Markdown 表格单元格不得含未转义竖线；公式矢量图缺失会退化为纯文本。
15. 想发布用户交付目录：只有用户明确要求时才执行；日常修改不升版本、不构建安装包（AGENTS.md:3-4）。用 `prepare_user_release.py` 前先确认 LICENSE 文件名分支，并在交付目录之外保存安装包原始哈希。
16. 平台约束：构建脚本依赖 Windows PowerShell、Visual Studio 与 vcpkg；多个脚本含本机硬编码路径（D:/cases、C:/work/miniconda、`D:\work\MODTRAN5.2.1`），迁移环境时需改环境变量或参数；`tools/case005` 脚本依赖仓库外数据根，不能在干净检出上直接运行。

## 9. 待核实项汇总

本章汇总各章撰写时标注“待核实/待核对”的条目。**待核实项不得当作结论引用**；核实后应回到对应章节改写，并从本章删除。条目摘自各章正文（过长的已截断，完整内容见对应章节）。

### 9.1 各章待核实项

#### 第 2 应用层：Electron 外壳与本地服务

| 序号 | 位置与内容 |
| --- | --- |
| 1 | 被谁调用：Electron 运行时（package.json 的 `main`）。依赖：`settings.mjs`、Electron、`server-process.mjs`（以路径字符串 fork，非 import）。`utilityProcess.fork` 与图标、preload 的路径都基于 `sourceRoot`（desktop/main.mjs:223,394），打包后该路径是否位于 asar 内待核实。 |
| 2 | `runPreflight(input, project, mode)`（server.mjs:1182-1283）：由 `/api/run/preflight` 通过 `heavyTasks.run('run-preflight')` 调用，输出 `{ mode, nodes, angles, bands, timeoutSeconds, facet 或 voxel, estimateSeconds, estimateBasis, warnings }`。面元估算用 `estimateFacetTriangles`（地面初值 2 加各对象“三角形数乘实例数”，server.mjs:1079-… |
| 3 | `writeRadiosityTiffForAngle`（server.mjs:1628-2357，约 730 行）：纯 CPU 光栅化，把面元结果投影到传感器像面并写多波段 float32 TIFF，附带统计 CSV 与过程量文件。头部注释给出五个步骤：读取结果与工程、建立相机与大气函数、三角形光栅化、沿太阳方向的阴影光栅、取值叠加与写盘（server.mjs:1616-1627）。该函数内部细节本次只核对了头部与收尾部分，其余逐行逻辑待核实。 |
| 4 | 预览：最长边不超过 768 的最近邻降采样，默认按 2%-98% 分位拉伸；波段被判为热红外（名称含 temperature/热红外/thermal，或名称中 @ 波长 ≥ 3000 nm）时，把 ≤ 0 的值计为无效，改用 `thermalSurfaceStretch`，并计算高温热点叠加（server.mjs:4043-4117）。拉伸间隙阈值的单位含义待核实（server.mjs:3787）。 |
| 5 | `checkForcing` 调用 `stream forcing-check --json`，退出码 0 与 3 都视为得到报告，码 3 的含义待核实（server/stream/index.mjs:565）。 |

#### 第 3 前端

| 序号 | 位置与内容 |
| --- | --- |
| 1 | package.json 脚本中与前端相关的有：`dev`（`node dev.mjs`，同时拉起 `server.mjs --api-only` 与 Vite，两个子进程同生共死）、`build` 与 `build:gui`（均为 `vite build`）、`start`（`node server.mjs`，提供 `/api` 与 `gui/` 静态页，默认端口 4173）、`desktop`（`electron .`）、`test`（运行 4 个 `tools/test-*.mjs`，从文件名看不含界面测试，具体覆盖范围待核实）（package.json:10-19，dev.mjs:25… |
| 2 | `projectToXml` 拼接 `<Control>`、`<Geometry>`、`<Atmosphere>`、`<Scene>`、`<Attribute>`、`<Meteorology>` 六段（project-schema.js:1849-1955）。仓库内除 `tools/test-vehicle-parts.mjs` 外没有调用者（grep 仅命中该测试与本文件），而服务端运行前经 `prepareRuntimeSceneProject` 准备项目（server.mjs:48，5505），文件头注释所写“→ projectToXml → 引擎”（project-schema.js:… |
| 3 | `projectToXml` 与服务端运行链路的关系：仓库内只有 `tools/test-vehicle-parts.mjs` 调用它，主运行链路是否改用 `project.json` 直接交给引擎，待核实（见服务端章）。 |
| 4 | `main.js` 4101-5006 与 5333-5802 区段（材质、预设对话框）只读了函数清单与关键入口，字段级细节待核实。 |

#### 第 4 HiStream 基础层

| 序号 | 位置与内容 |
| --- | --- |
| 1 | 无条件定义 `ENABLE_GPU_PRINTF` 并启用校验层的 debugPrintf（`src/base/appsetting.cpp:312-325`），源码已标注潜在性能代价（311）；发布版是否实际加载校验层取决于部署，待核实。 |
| 2 | CMake 引用 `shader/raytracing/` 小写目录，而磁盘目录为 `shader/RayTracing/`，`shade_state.glsl` 以 `bindinglayout.h` 小写包含 `bindingLayout.h`（`shader/shade_state.glsl:21`）；Windows 大小写不敏感可通过，非 Windows 构建会失败，是否有 Linux 构建需求待核实。 |
| 3 | nlohmann/json 头文件被 `projectjson.h` 包含，但 `CMakeLists.txt` 没有对应 `find_package`，其来源（vcpkg 全局包含路径或 nvpro_core）待核实。 |
| 4 | `shadersCompile.py`；57；开发期手工编译脚本，不参与正式构建，面向 Windows；待核实是否仍被使用 |
| 5 | `HISTREAM_RT_SEED`、`HISTREAM_RT_SAMPLER`、`HISTREAM_RT_ADAPTIVE`；RayTracing 采样覆盖，在后端读取，不在 base；见 `fileio_json.cpp:271-272` 注释，实现待核实 |
| 6 | 易错点汇总：头文件注释可能落后于实现（`compo.h`）；`gridio` 与 `pixelio` 为空壳；`writeENVIdata` 与旧 XML 解析无调用点，改它们不会影响引擎行为；`src/main.cpp` 在 `.cpp` 中带有 `#pragma once`，无害；`hexvoxel.h` 提到的 `src/tools/hexvoxel` 命令行目录在 `src` 下不存在，去向待核实；`src/cases/` 的用途待核实。 |

#### 第 5 成像与体元模块

| 序号 | 位置与内容 |
| --- | --- |
| 1 | 链接能通过，依赖于这些成员函数以不同的 IO 参数类型重载，符号互不冲突；`Command::Command()` 只在 src/raytracing/command.cpp:26 定义一次，voxelrt 与 voxeleb 的 command.h 只声明（src/voxelrt/command.h:26、src/voxeleb/command.h:26），靠同一份定义链接。该做法属于违反单一定义规则的隐患（待核对：各编译器下的实际行为）。 |
| 2 | CMake 与 pipeline.cpp 都使用小写目录 `shader/raytracing`（CMakeLists.txt:198；src/raytracing/pipeline.cpp:169-172），而源码目录实际叫 `shader/RayTracing`。Windows 文件系统不区分大小写所以没有问题，换到区分大小写的系统需要统一（待核对：本仓库是否有 Linux 构建）。 |
| 3 | 风险：VoxelrtIO 的 `k_angle` 无初值（voxelrtio.h:131），UAV 分支未赋值（voxelrt.cpp:305-319），而 outputPos 用它选角度（voxelrt.cpp:635），UAV 模式下取值不确定（待核对：实际运行结果）。`isAlbedo` 只被读取，没有对应输出（voxelrt.cpp:186）。 |

#### 第 6 面元模块

| 序号 | 位置与内容 |
| --- | --- |
| 1 | `load` 返回 dict：描述符字段转 numpy（`mmap=True` 时为 `np.memmap`），JSON 模式下长度大于 1024 的数字列表转 `float32` 数组，并把 `vertexPositions` 重塑为 `(n,3,3)`、`bandRadiosity` 为 `(bands,s)`、`bandStates` 为 `(bands,s,2)`，使两种格式结果可直接比较（facet_result.py:36-66）。描述符 dtype 支持 `float32/int32/float64`（facet_result.py:29），比 Node 读取器宽。常量 `_PE… |
| 2 | `CpuRasterizer`（FC:2313-2515）、`buildCpuGraph`（FC:2532-2700）、`runCpu`（FC:3004）仅读到头部与步骤注释，内部细节待核实。 |
| 3 | `shadersCompile.py` 是否曾用于 facet 着色器待核实（其 `versionName` 缺省为 voxeleb）。 |

#### 第 7 STREAM 影像引擎

| 序号 | 位置与内容 |
| --- | --- |
| 1 | `LICENSE`、`physics/LICENSE-MWRTM`；73、21；前者为 PolyForm Noncommercial；后者为 MIT 声明；`physics/LICENSE-MWRTM` 与 `stream_core.hpp` 微波部分的关系待核实 |
| 2 | `types.h` 中 `Canopy` 保存 LAI、冠层高度、叶片投影 G、热点、叶宽与 IGBP 码；`treeStand`、`canopyHeight`、`crownShape` 与 `CropRows` 目前不参与计算（types.h:19-47）。`LeafOptics` 含 PROSPECT 的 cab/cw/cdm/cs/n 与热红外反射 `reflTir`、透射 `transTir`，发射率为 `1 − reflTir − transTir`（types.h:49-54）。PROSPECT 参数的物理单位待核实。 |
| 3 | `spa`、`brightness`；手册中的具体位置待核实 |
| 4 | `Pack::open` 的元数据优先级为：调用方传入、索引文件的 `metadata` 键、产品名对应预设（era5.cpp:589-596）；索引文件 `kind` 为 `stream-era5-pack` 或 `stream-forcing-pack`，`schemaVersion = 1`（:586-588）。`registerPack` 写出 `stream-era5-pack`，记录产品、年份、公共时段、`fullYear`、网格、逐变量完整性与文件清单（含字节数，打开时校验，被改动则要求重新登记，era5.cpp:496-508、632-633）；`registerForcing… |
| 5 | 时间标准化的共用函数在 `forcing_resample.cpp`：瞬时量线性插值，缺口超过 `max(6 h, 2×步长)` 视为缺测（forcing_resample.cpp:45-47）；通量分配到整点时保证每个时段能量守恒，短波按太阳天顶角余弦分配（`approximateCosZenith` 在 `pixel_forcing.cpp` 定义，`forcing_resample.cpp:37` 以外部声明引用）。`StationSource` 构造时读取并校验全部 CSV，`load` 时重采样并交 `standardiseQuantities` 合成标准量，其内部逐站细节待核实。 |
| 6 | 新增一个 EB 能量平衡参数（例如新阻抗系数）：在 `config.h` 的 `ImageConfig` 加字段、`config.cpp` 的步骤 8c 用 `numberOr` 读取并校验、`image.cpp` 的 `buildSurface` 写入 `EnergySurface`（image.cpp:739-783）；若参数影响残差，还要确认 `energyInputs` 与 `energyResiduals<Ad>` 是否需要把它作为 `Ad` 量传入，并更新 `README.md` 参数表。同时检查 `server/stream/config.mjs` 是否需要透传（待核实该文件的透… |

#### 第 8 工具、测试与构建

| 序号 | 位置与内容 |
| --- | --- |
| 1 | src/renderer/src 下 project-schema.js 与 material-specular.js；同路径；在 resources 下另存一份（行 65-68），用途待核实 |
| 2 | `tools/prepare_user_release.py`（91 行，尚未实际使用，docs/development-and-release.md:11）按 `--version`、`--previous`、`--installer`、`--out`、`--start-text`、可重复的 `--add-example NAME=DIR=CASE_JSON` 生成交付目录。步骤：输出目录已存在则拒绝；安装包文件名必须含版本号；复制安装包、LICENSE、两本 PDF 手册；整体带入上一版 `examples/`；追加新案例并记录 `projectSha256`；写 `案例目录.json`、… |
| 3 | test-facet-energy-convergence.cpp、test-facet-nested-budget.cpp、test-facet-sparse-aerodynamics.cpp；能量收敛判据边界、耦合求解器工作量预算、稀疏冠层空气动力学；仅依赖 energyphysics.h；CMake 中未发现对应目标，构建命令待核实 |
| 4 | spectral-library；23 个光谱 txt；`catalog.json` 列 20 项（目录与文件数不一致，部分 txt 未登记，待核实），`usage` 为 quick-test-only，波长单位 nm、值为无量纲分数；篷布与遮阳网类 txt 为三行（波长、反射率、透射率），由 `generate-tarp-assets.py` 生成，其余文件格式待核实 |

### 9.2 维护规则

1. 修改代码行为时，同步修改对应章节的接口说明、耦合点清单（1.4）与修改指南。
2. 新发现的缺陷或不一致先登记到对应章节，核实并修复后再移除，不要直接改写历史描述。
3. 数值性改动（几何取整、角度、热属性、求解器）不得写“已验证”，除非完成 tools/regression 的数值回归。
4. 重新生成 PDF：`python tools/build-core-manuals-pdf.py modules`（源文件 docs/code-modules.md）。

