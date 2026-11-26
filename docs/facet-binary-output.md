# FacetRT / FacetEB 二进制结果格式（facetOutputFormat = binary）

状态：已实现（2026-11-26）。引擎 `facetrt.json` / `faceteb.json` 的二进制模式与 `tools/facet_result.py` 均按本文；作物、城市、`feb_crop` 的 JSON 与二进制数组逐位相同（用 `facet_result.load` 比较 `float32` 位模式）。

## 为什么

森林示例（1580 万面元）的 `facetrt.json` 约 9.2 GB，手写 JSON 的序列化、磁盘写入与读取都很慢，Node 的 `JSON.parse` 也无法解析。二进制模式把所有大数组放进旁路 `.bin` 文件，JSON 只保留元数据、小数组和指向二进制数据的描述符，**字段名不变**。

## 选择方式

| 方式 | 取值 | 说明 |
|---|---|---|
| 工程字段 `control.facetOutputFormat` | `json`（缺省）、`binary` | 其他值报错 |
| 环境变量 `HISTREAM_FACET_OUTPUT_FORMAT` | `json`、`binary` | 覆盖工程字段 |

缺省 `json` 时输出与此前逐位相同。**这是引擎自身的缺省**（直接运行 `histream.exe` 且工程没有该字段时仍写 JSON）；Stream Studio 的工程模型（`createDefaultProject` / `normalizeProject`）自 2026-11-26 起把缺省写成 `binary`，GUI 与服务端运行总会把该字段写入运行用工程，所以经 Stream Studio 运行的工程（包括没有该字段的旧工程）默认得到二进制结果；要旧格式，在工程 `configuration.control` 里写 `"facetOutputFormat": "json"`。

## 文件

- FacetRT：`facetrt.json`（元数据）+ `facetrt.bin`（全部大数组）；FacetEB：`faceteb.json` + `faceteb.bin`。`.bin` 与 `.json` 同目录、同主名；JSON 里用文件名（不含目录）引用，读取者按 JSON 所在目录解析。
- `faceteb_run.json` 的 `products` 追加 `faceteb.bin`；FacetEB 原有的 `.facet_steps/components.bin` 等节点文件不变。
- 二进制文件：**小端**，无文件头，数组按写入顺序首尾相接，每个数组起点按 **16 字节**对齐（间隙补零）。数据类型目前只有 `float32`（IEEE-754 单精度，小端）。数组内按 C 顺序（最后一维变化最快）。
- 数值与 JSON 模式逐位相同：JSON 模式把 `float` 以 9 位有效数字写出，可无损还原为 float32，二进制直接存同一个 float32。

## JSON 中的描述符

凡长度 **> 1024** 的 float 数组，其字段值（原来是数字数组）换成描述符对象；其余小数组（如 `wavelengths`）仍是内联 JSON 数组。读取者判断：字段值是对象且含 `dtype` → 二进制数组。

```json
"sunlit": {"file": "facetrt.bin", "offset": 570425344, "length": 31687872,
           "dtype": "float32", "shape": [31687872]}
```

| 键 | 含义 |
|---|---|
| `file` | 旁路文件名（与 JSON 同目录） |
| `offset` | 起始字节偏移（16 的倍数） |
| `length` | 元素个数（不是字节数）；字节数 = `length × 4` |
| `dtype` | `"float32"` |
| `shape` | 形状数组，乘积 = `length` |

顶层另有：

```json
"format": "binary", "formatVersion": 1, "binaryFile": "facetrt.bin", "binaryBytes": 1234567890
```

JSON 模式没有 `format` 字段（读取者视缺省为 `json`）。

## 各数组的形状（side = 2×面元 + 0/1，0 为几何法向侧；n = 面元数，s = 2n）

| 字段 | 出现于 | 形状 | 说明 |
|---|---|---|---|
| `vertexPositions` | FacetRT、FacetEB | `[n, 3, 3]` | 每面元 3 个顶点的 xyz（m） |
| `sunlit` | 两者 | `[s]` | 受光比例；背向太阳的侧为 0 |
| `radiosity` | 两者 | `[s]` | 第 0 波段（FacetRT）或最后节点（FacetEB）辐射度 |
| `lightEnhancement` | 两者 | `[s]` | 直射增强量 |
| `bandRadiosity` | FacetRT（多波段） | `[bands, s]` | 各波段辐射度 |
| `bandStates` | FacetRT | `[bands, s, 2]` | 每波段每侧（受光, 阴影）两个状态值；内存顺序与 JSON 模式数组一致 |
| `temperature`、`temperatureSunlit`、`temperatureShaded` | FacetEB | `[s]` | K |
| `netRadiation`、`sensibleHeat`、`latentHeat`、`storageHeat`、`photovoltaicPower` | FacetEB | `[s]` | W m-2（光伏为 W） |

`wavelengths`（波段数个）等小数组保持内联。`graph`、`timing`、`execution`、`quality` 等对象保持内联。

## 读取

- Python：`tools/facet_result.py`：

```python
from facet_result import load
r = load("…/facetrt.json")          # JSON 或二进制都可以
r["sunlit"]                          # numpy float32 数组，形状见上表
r["meta"]                            # 其余元数据（dict）
r = load(path, mmap=True)            # 二进制数组用 np.memmap，不整体读入
```

- Node/其他语言：读 JSON，遇到含 `dtype` 的对象，用 `file`、`offset`、`length` 从 `.bin` 里取 `length` 个小端 float32（例如 `fs.read(fd, buf, 0, length*4, offset)`，`new Float32Array(buf.buffer, buf.byteOffset, length)`，主机为小端）。

## 兼容性

- 二进制模式下 `facetrt.json` 很小（元数据），可以直接 `JSON.parse`。
- 读取者必须容忍字段位置变化：`timing` 之外的字段顺序没有保证。
- 版本：`formatVersion` 目前为 1；不兼容的改动会增加版本号。

## 实测（2026-11-26）

| 场景 | JSON | 二进制 | 写出耗时（`timing.outputWriteMs`） |
|---|---|---|---|
| frt_crop（14.1 万面元，6 波段） | 86.6 MB | 28.8 MB + 3 KB JSON | 667 ms → 12 ms |
| frt_urban_mwir | 31.4 MB | 13.3 MB + 2.7 KB | 424 ms → 6 ms |
| feb_crop（FacetEB） | — | 17.5 MB + 2.4 KB | — |
| forest（1580 万面元，6 波段） | 8.95 GB，写出 76–85 s | 3.01 GB + 约 3 KB，写出 2.7 s（冷写盘时曾 26 s） | |

体积约为 JSON 的三分之一，主要是 `bandStates`（1.5 GB）、`bandRadiosity`（0.76 GB）、`vertexPositions`（0.57 GB）。`timing` 中 `outputWriteMs`、`wallMs`、顶层 `binaryBytes` 在文件写完后原位回填（数字前有空白，仍是合法 JSON）。

## 读取 helper 与范例

`tools/facet_result.py` 的 `load(path, mmap=False)`：JSON 模式的数组（长度 > 1024 的数字列表）也转成同样形状的 `float32` 数组，两种格式得到的结果可直接互相比较；命令行 `python tools/facet_result.py facetrt.json` 打印摘要。

## Stream Studio 中的读取与缺省格式（2026-11-26）

- **服务端读取器**：`server/facet-result.mjs`（`readFacetResult` / `sampleFacetResult` / `isFacetResultFile` / `facetSidecarFiles` 等）自动识别两种格式，返回与旧 JSON 读取器相同的结果形状。几何模式只读 `vertexPositions`；三维显示抽样只读被选面元所在的数据块；单个数组超过 `STREAM_STUDIO_MAX_RESULT_BYTES`（缺省 8 GB）才拒绝（JSON 格式仍按整个文件计）。结果缓存键包含旁路 `.bin` 的 (mtime, size)；删除三维结果、清理临时结果、`process/` 副本都会连同旁路文件处理；结果列表中的 `size` 为 JSON + `.bin`。
- **其他读取者**：`tools/facet_document.py`（`read_document(path, only=…)`，JSON 格式与 `json.load` 完全相同）供 `tools/plot-faceteb-day.py`、`tools/compare-facet-outer-stomata.py`、`tools/regression/compare.py`、`tools/case005/*` 使用；独立统计工具 `statistic/histream_statistics.py`（及 `scene_example_report.py`）内置同样的读取（`read_result_document`）。
- **缺省改为 binary 的兼容风险**：① 用外部程序（自写脚本、MATLAB、旧版统计脚本）直接 `json.load` `facetrt.json` / `faceteb.json` 读大数组会读不到数据（大数组变成 `{file, offset, …}` 描述符）；请改用 `tools/facet_result.py`、`tools/facet_document.py` 或按上文“读取”一节自行读取；② 复制/归档结果时必须带上同目录同主名的 `.bin`（`faceteb_run.json` 的 `products` 与运行清单已列出）；③ 旧版 Stream Studio 打不开二进制结果；④ 已有的旧 JSON 结果仍可正常读取，不受影响。
