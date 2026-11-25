#!/usr/bin/env python3
# ==========================================================================
# 文件：histream_statistics.py
# 功能：HiStream 结果的独立统计与查看命令行工具（不依赖引擎和服务端）。对单个文件或整个结果目录，
#       批量导出统计表（CSV/TXT）、热力图、直方图和三维结构示意图（PNG）。
#       输入三类：GeoTIFF 影像；体元（VoxelRT/VoxelEB/体元流体）过程 JSON + 二进制记录；
#       面元（FacetRT/FacetEB）过程 JSON 或面元结果 JSON。
# 主要模块：
#   1. 统计与输出：finite_values / calculate_statistics / write_statistics / output_folder
#   2. 绘图：colorize 色带、render_heatmap、render_histogram、render_structure（Pillow 简易投影）
#   3. GeoTIFF 读取：read_raster（rasterio → Pillow → 内置未压缩 strip 读取器）
#   4. 三维结构读取：voxel_structure、facet_process_structure、facet_result_structure、read_structure
#   5. 分析流程：analyze_image、analyze_structure、analyze_components（按土壤/植被/建筑分组）
#   6. 命令行入口：candidate_files 扫描输入，main 解析参数并逐文件处理（单个文件失败不阻断批处理）
# 数据流：输入路径 → candidate_files → analyze_image / analyze_structure →
#       输出目录 <输出根>/<名称>_<哈希>/ 下的 statistics.csv、summary.txt、sample_points.csv、
#       *_heatmap.png、*_histogram.png、*_3d.png，以及分组统计子目录 components/。
# 用法：python histream_statistics.py <输入> [-o 输出目录] [--fields 字段] [--bands 波段]
#       [--facet-side mean|front|back] [--max-points N] [--components soil,vegetation,building|none]
# 与其他文件：scene_example_report.py 复用其 colorize、draw_colorbar、load_font、read_structure；
#       run_scene_examples.mjs 以子进程调用本脚本统计 output/process；用法示例见 statistic/README.md。
# 重要约定：栅格数据按 [波段, 行, 列] 排列；能量字段为通量（W m-2）而非总功率，均值不做面积加权；
#       原生表面温度为 K，三维统计时另导出 temperatureC（℃）；结果文本与 CSV 为 UTF-8。
# ==========================================================================

"""HiStream 独立结果查看与统计工具。

支持：
1. GeoTIFF 图像：逐波段统计、热力图、直方图。
2. VoxelRT/VoxelEB/体元流体过程：字段统计、抽样点 CSV、三维结构图。
3. FacetRT/FacetEB：面元中心统计和三维结构图。

仅需 numpy 与 Pillow；安装 rasterio 后可更完整地读取多波段 GeoTIFF 和 NoData。
"""

from __future__ import annotations

# 依赖：numpy 与 Pillow 为必需依赖（rasterio 的使用见后文读取 GeoTIFF 处）。
import argparse
import csv
import hashlib
import json
import math
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np
from PIL import Image, ImageDraw, ImageFont


# 说明：把标准输出/错误强制设为 UTF-8，避免 Windows 控制台默认编码下中文输出乱码。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


# 常量：统计表的列顺序（statistics.csv 与 summary.txt 共用）。percentile_02/98 为 2%/98% 分位数；
# count 为有效值个数，invalid_count 为 NaN/Inf/NoData 个数；standard_deviation 为总体标准差（np.std 缺省）。
STAT_COLUMNS = (
    "count",
    "invalid_count",
    "minimum",
    "percentile_02",
    "percentile_25",
    "median",
    "mean",
    "percentile_75",
    "percentile_98",
    "maximum",
    "standard_deviation",
)


# 数据类：一个标量字段。identifier 为字段标识（用作文件名/CSV 键），label 为显示名，values 为一维数值数组。
@dataclass
class FieldData:
    identifier: str
    label: str
    values: np.ndarray


# 数据类：一份三维结构数据（体元或面元）。positions 为 N×3 的中心坐标（单位随结果文件，
# 通常为 m（待核对）），fields 为各标量字段；components 为每个元素的组分编码（可缺），
# source_indices 为抽样后对应的原始下标（可缺），element_count 为原始元素总数。
@dataclass
class StructureData:
    source: Path
    kind: str
    positions: np.ndarray
    fields: list[FieldData]
    element_count: int
    metadata: dict
    components: np.ndarray | None = None
    source_indices: np.ndarray | None = None


# 常量：组分编码表。COMPONENTS 为按名称筛选组分的 {名称: (编码, 显示名)}；
# COMPONENT_LABELS 为组分文件（uint8）中全部合法编码 → 显示名；ENERGY_FIELDS 为能量平衡结果里的字段名集合。
COMPONENTS = {"soil": (1, "土壤/背景"), "vegetation": (2, "植被"), "building": (3, "建筑")}
COMPONENT_LABELS = {0: "未分类", 1: "土壤/背景", 2: "植被", 3: "建筑", 4: "光伏", 5: "水体"}
ENERGY_FIELDS = {"temperature", "shortwaveRadiation", "longwaveRadiation", "netRadiation",
                 "sensibleHeat", "latentHeat", "surfaceHeatFlux", "storageHeat"}


# 作用：读取与结果同目录的组分标识文件（metadata["componentFile"]，每个元素 1 字节 uint8）。
# 输入：结果文件路径 path、元数据 metadata、元素个数 count；输出：长度为 count 的 uint8 数组，未声明则返回 None。
# 校验：文件字节数必须等于元素个数，且编码必须在 COMPONENT_LABELS 内，否则抛 ValueError。
def read_components(path: Path, metadata: dict, count: int) -> np.ndarray | None:
    name = metadata.get("componentFile")
    if not name:
        return None
    source = (path.parent / str(name)).resolve()
    if source.stat().st_size != count:
        raise ValueError(f"组分标识数量与几何不一致：{source}")
    values = np.fromfile(source, dtype=np.uint8)
    if not np.isin(values, list(COMPONENT_LABELS)).all():
        raise ValueError("组分文件包含未知编码")
    return values


# 作用：把任意字符串规整成可作文件名的形式（非字母数字及 . _ - 的连续字符替换为 _，空则用 result）。
def safe_name(value: str) -> str:
    value = re.sub(r"[^0-9A-Za-z._-]+", "_", value.strip())
    return value.strip("._") or "result"


# 作用：确定并创建某个输入文件的输出目录：<root>/<规整名>_<源文件绝对路径 SHA1 前 8 位>。
# 说明：root 缺省为源文件目录下的 statistics_output；哈希后缀用于避免不同目录下同名文件互相覆盖。
def output_folder(source: Path, root: Path | None, relative_key: str = "") -> Path:
    if root is None:
        root = source.parent / "statistics_output"
    suffix = hashlib.sha1(str(source.resolve()).encode("utf-8")).hexdigest()[:8]
    name = safe_name(relative_key or source.stem)
    folder = root / f"{name}_{suffix}"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


# 作用：按平台依次尝试等线/微软雅黑（Windows）、Noto CJK、DejaVu，找不到时退回 Pillow 内置字体（不含中文字形）。
def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = []
    if sys.platform.startswith("win"):
        candidates.extend(
            [
                Path(r"C:\Windows\Fonts\Dengb.ttf" if bold else r"C:\Windows\Fonts\Deng.ttf"),
                Path(r"C:\Windows\Fonts\msyhbd.ttc" if bold else r"C:\Windows\Fonts\msyh.ttc"),
            ]
        )
    candidates.extend(
        [
            Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc" if bold else
                 "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
            Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
        ]
    )
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size=size)
    return ImageFont.load_default()


# 作用：取出有效值。有效 = 有限数（非 NaN/Inf），且不等于 NoData（相对误差 0、绝对误差 max(1e-12, |NoData|×1e-7)）。
# 输出：(有效值一维数组, 与输入同形的有效掩膜)。
def finite_values(values: np.ndarray, nodata: float | None = None) -> tuple[np.ndarray, np.ndarray]:
    array = np.asarray(values, dtype=np.float64)
    valid = np.isfinite(array)
    if nodata is not None and math.isfinite(float(nodata)):
        valid &= ~np.isclose(array, float(nodata), rtol=0.0, atol=max(1e-12, abs(float(nodata)) * 1e-7))
    return array[valid], valid


# 作用：计算 STAT_COLUMNS 所列的 11 项统计。无有效值时，除两个计数外其余项均为 NaN。
def calculate_statistics(values: np.ndarray, nodata: float | None = None) -> dict[str, float | int]:
    array = np.asarray(values)
    valid_values, valid_mask = finite_values(array, nodata)
    result: dict[str, float | int] = {
        "count": int(valid_values.size),
        "invalid_count": int(array.size - np.count_nonzero(valid_mask)),
    }
    if not valid_values.size:
        for key in STAT_COLUMNS[2:]:
            result[key] = float("nan")
        return result
    percentiles = np.percentile(valid_values, [2, 25, 50, 75, 98])
    result.update(
        {
            "minimum": float(np.min(valid_values)),
            "percentile_02": float(percentiles[0]),
            "percentile_25": float(percentiles[1]),
            "median": float(percentiles[2]),
            "mean": float(np.mean(valid_values)),
            "percentile_75": float(percentiles[3]),
            "percentile_98": float(percentiles[4]),
            "maximum": float(np.max(valid_values)),
            "standard_deviation": float(np.std(valid_values)),
        }
    )
    return result


# 作用：把数值格式化为最多 8 位有效数字的短字符串；整数原样输出，非有限值输出 nan，无法转数值则原样转字符串。
def compact_number(value: object) -> str:
    if isinstance(value, (int, np.integer)):
        return str(int(value))
    try:
        number = float(value)
    except (TypeError, ValueError):
        return str(value)
    if not math.isfinite(number):
        return "nan"
    return f"{number:.8g}"


# 作用：写出统计结果两份文件：statistics.csv（UTF-8 带 BOM，便于 Excel 打开）与 summary.txt（可读摘要）。
# 输入：输出目录 folder、源文件 source、结果类型 kind、行 rows=[(字段标识, 显示名, 统计字典)]、元数据 metadata（可缺，
# 仅摘要中列出 model/processType/geometry/time 等关键项）；输出：(csv 路径, txt 路径)。
def write_statistics(folder: Path, source: Path, kind: str,
                     rows: list[tuple[str, str, dict[str, float | int]]],
                     metadata: dict | None = None) -> tuple[Path, Path]:
    csv_path = folder / "statistics.csv"
    with csv_path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("field_id", "field_label", *STAT_COLUMNS))
        for identifier, label, stats in rows:
            writer.writerow((identifier, label, *(compact_number(stats[key]) for key in STAT_COLUMNS)))

    txt_path = folder / "summary.txt"
    with txt_path.open("w", encoding="utf-8") as stream:
        stream.write("HiStream external statistics\n")
        stream.write(f"source: {source.resolve()}\n")
        stream.write(f"kind: {kind}\n")
        if metadata:
            for key in ("model", "processType", "geometry", "time", "node", "voxelCount",
                        "facetCount", "voxelSize", "dataFile"):
                if key in metadata:
                    stream.write(f"{key}: {metadata[key]}\n")
        stream.write("\n")
        for identifier, label, stats in rows:
            stream.write(f"[{identifier}] {label}\n")
            for key in STAT_COLUMNS:
                stream.write(f"  {key}: {compact_number(stats[key])}\n")
            stream.write("\n")
    return csv_path, txt_path


# 常量：热力图色带的节点，每行为 (归一化位置 0–1, R, G, B)，由 colorize 分通道线性插值（与 turbo 色带相近，待核对）。
COLOR_STOPS = np.asarray(
    [
        (0.00, 48, 18, 59),
        (0.14, 65, 68, 135),
        (0.29, 42, 120, 142),
        (0.43, 34, 168, 132),
        (0.57, 122, 209, 81),
        (0.71, 210, 226, 27),
        (0.86, 251, 159, 58),
        (1.00, 180, 4, 38),
    ],
    dtype=np.float64,
)


# 作用：把 0–1 的归一化数组（越界截断）映射为 uint8 RGB 数组，形状为输入形状 + (3,)。
def colorize(normalized: np.ndarray) -> np.ndarray:
    normalized = np.clip(np.asarray(normalized, dtype=np.float64), 0.0, 1.0)
    channels = [np.interp(normalized, COLOR_STOPS[:, 0], COLOR_STOPS[:, index]) for index in range(1, 4)]
    return np.stack(channels, axis=-1).astype(np.uint8)


# 作用：求显示用的数值范围：取有效值 2%–98% 分位数做拉伸，抑制极端值的影响。
# 退化处理：分位数不合法或上限不大于下限时改用最小/最大值；仍相等则上限取 低 + 1；无有效值返回 (0, 1)。
def data_range(values: np.ndarray, nodata: float | None = None) -> tuple[float, float]:
    valid, _ = finite_values(values, nodata)
    if not valid.size:
        return 0.0, 1.0
    low, high = np.percentile(valid, [2, 98])
    if not math.isfinite(low) or not math.isfinite(high) or high <= low:
        low, high = float(np.min(valid)), float(np.max(valid))
    if high <= low:
        high = low + 1.0
    return float(low), float(high)


# 作用：在画布 (x0, y0) 处画一条宽 width、高 height 的色带，并在其下方左端标 low、右端标 high。
def draw_colorbar(canvas: Image.Image, draw: ImageDraw.ImageDraw, x0: int, y0: int,
                  width: int, height: int, low: float, high: float) -> None:
    ramp = np.linspace(0.0, 1.0, width, dtype=np.float64)[None, :]
    colors = colorize(ramp)
    colors = np.repeat(colors, height, axis=0)
    canvas.paste(Image.fromarray(colors, "RGB"), (x0, y0))
    draw.rectangle((x0, y0, x0 + width, y0 + height), outline=(70, 84, 96), width=1)
    font = load_font(15)
    draw.text((x0, y0 + height + 4), compact_number(low), font=font, fill=(45, 55, 65))
    high_text = compact_number(high)
    box = draw.textbbox((0, 0), high_text, font=font)
    draw.text((x0 + width - (box[2] - box[0]), y0 + height + 4), high_text,
              font=font, fill=(45, 55, 65))


# 作用：把二维数组渲染成带标题与色带的热力图 PNG。
# 步骤 1：按 data_range 归一化并着色，无效像元涂浅灰 (225, 229, 233)；
# 步骤 2：小图按比例以最近邻放大到最长边约 1500 px（scale 至少为 1，大图不缩小）；
# 步骤 3：放到白色画布上，加标题与色带。输入必须是二维数组，否则抛 ValueError。
def render_heatmap(values: np.ndarray, destination: Path, title: str,
                   nodata: float | None = None) -> None:
    array = np.asarray(values, dtype=np.float64)
    if array.ndim != 2:
        raise ValueError("热力图输入必须是二维数组")
    low, high = data_range(array, nodata)
    valid_values, valid = finite_values(array, nodata)
    del valid_values
    normalized = (array - low) / (high - low)
    rgb = colorize(np.nan_to_num(normalized, nan=0.0, posinf=1.0, neginf=0.0))
    rgb[~valid] = (225, 229, 233)
    image = Image.fromarray(rgb, "RGB")
    max_side = 1500
    scale = min(max_side / max(1, image.width), max_side / max(1, image.height))
    scale = max(1.0, scale)
    image = image.resize((max(1, int(image.width * scale)), max(1, int(image.height * scale))),
                         Image.Resampling.NEAREST)
    canvas = Image.new("RGB", (max(900, image.width + 80), image.height + 150), "white")
    canvas.paste(image, ((canvas.width - image.width) // 2, 64))
    draw = ImageDraw.Draw(canvas)
    draw.text((34, 18), title, font=load_font(24, bold=True), fill=(20, 45, 65))
    draw_colorbar(canvas, draw, 35, image.height + 82, canvas.width - 70, 18, low, high)
    canvas.save(destination)


# 作用：把有效值绘成 1100×700 的直方图 PNG（柱数至少 5，缺省 50）。无有效数据时只写提示文字。
def render_histogram(values: np.ndarray, destination: Path, title: str,
                     nodata: float | None = None, bins: int = 50) -> None:
    valid, _ = finite_values(values, nodata)
    width, height = 1100, 700
    margin_left, margin_right, margin_top, margin_bottom = 95, 45, 78, 90
    canvas = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(canvas)
    draw.text((margin_left, 22), title, font=load_font(24, bold=True), fill=(20, 45, 65))
    if not valid.size:
        draw.text((margin_left, margin_top), "No valid data", font=load_font(24), fill=(70, 80, 90))
        draw.text((margin_left, height - 38), "valid count: 0", font=load_font(16), fill=(70, 80, 90))
        canvas.save(destination)
        return
    counts, edges = np.histogram(valid, bins=max(5, bins))
    x0, y0 = margin_left, height - margin_bottom
    x1, y1 = width - margin_right, margin_top
    draw.line((x0, y0, x1, y0), fill=(55, 65, 75), width=2)
    draw.line((x0, y0, x0, y1), fill=(55, 65, 75), width=2)
    maximum = max(1, int(counts.max()))
    bar_width = (x1 - x0) / len(counts)
    for index, count in enumerate(counts):
        left = x0 + index * bar_width
        right = x0 + (index + 1) * bar_width
        top = y0 - (y0 - y1) * int(count) / maximum
        draw.rectangle((left, top, right, y0), fill=(49, 126, 199), outline=(255, 255, 255))
    font = load_font(16)
    draw.text((x0, y0 + 16), compact_number(edges[0]), font=font, fill=(45, 55, 65))
    high_text = compact_number(edges[-1])
    box = draw.textbbox((0, 0), high_text, font=font)
    draw.text((x1 - (box[2] - box[0]), y0 + 16), high_text, font=font, fill=(45, 55, 65))
    draw.text((16, y1 - 8), str(maximum), font=font, fill=(45, 55, 65))
    draw.text((x0, height - 38), f"valid count: {valid.size}", font=font, fill=(70, 80, 90))
    canvas.save(destination)


# 作用：等间隔抽样下标。count 不超过 maximum 时取全部，否则用 linspace 取 maximum 个（含首尾）。
def sample_indices(count: int, maximum: int) -> np.ndarray:
    if count <= maximum:
        return np.arange(count, dtype=np.int64)
    return np.linspace(0, count - 1, maximum, dtype=np.int64)


# 作用：把体元/面元中心点云按某个标量字段着色，渲染成一张三维结构示意图 PNG（纯 Pillow 的简易正交投影）。
# 输入：positions 为 N×3 坐标，values 为 N 个标量；maximum_points 为最多绘制的点数（缺省 50000）。
# 步骤 1：去掉含非有限值的点并等间隔抽样；步骤 2：以包围盒中心为原点、按最大跨度归一化；
# 步骤 3：绕 Y 轴转 38° 再抬升 24° 做正交投影，Y 轴朝上；
# 步骤 4：按画布尺寸缩放（留 6% 余量）；步骤 5：按 camera_depth 升序依次画点（后画覆盖先画，
# 远近方向约定待核对），颜色由 data_range + colorize 决定，点半径随点数在 1–5 px 之间自适应。
# 没有有效点时抛 ValueError。
def render_structure(positions: np.ndarray, values: np.ndarray, destination: Path,
                     title: str, maximum_points: int = 50000) -> None:
    positions = np.asarray(positions, dtype=np.float64)
    values = np.asarray(values, dtype=np.float64).reshape(-1)
    valid = np.isfinite(positions).all(axis=1) & np.isfinite(values)
    positions = positions[valid]
    values = values[valid]
    if not len(values):
        raise ValueError(f"{title} 没有有效三维点")
    indices = sample_indices(len(values), maximum_points)
    positions = positions[indices]
    values = values[indices]

    # 步骤 2：居中并归一化（跨度下限 1e-9，防止单点或共面数据除零）。
    center = 0.5 * (positions.min(axis=0) + positions.max(axis=0))
    span = np.maximum(positions.max(axis=0) - positions.min(axis=0), 1e-9)
    scaled = (positions - center) / float(np.max(span))
    # 步骤 3：固定视角投影：u 为画布横向，v 为画布纵向（向上为正），depth 为沿视线的深度。
    yaw = math.radians(38.0)
    elevation = math.radians(24.0)
    u = scaled[:, 0] * math.cos(yaw) - scaled[:, 2] * math.sin(yaw)
    depth = scaled[:, 0] * math.sin(yaw) + scaled[:, 2] * math.cos(yaw)
    v = scaled[:, 1] * math.cos(elevation) - depth * math.sin(elevation)
    camera_depth = depth * math.cos(elevation) + scaled[:, 1] * math.sin(elevation)
    order = np.argsort(camera_depth)

    # 步骤 4：画布 1400×980，四周留边（左右 70 px、上 90 px、下 120 px，下方放统计文字与色带）。
    width, height = 1400, 980
    left, right, top, bottom = 70, width - 70, 90, height - 120
    u_min, u_max = float(u.min()), float(u.max())
    v_min, v_max = float(v.min()), float(v.max())
    projection_scale = min((right - left) / max(u_max - u_min, 1e-9),
                           (bottom - top) / max(v_max - v_min, 1e-9)) * 0.94
    px = left + (u - u_min) * projection_scale
    py = bottom - (v - v_min) * projection_scale
    low, high = data_range(values)
    rgb = colorize((values - low) / (high - low))

    canvas = Image.new("RGB", (width, height), (247, 250, 252))
    draw = ImageDraw.Draw(canvas)
    draw.text((42, 22), title, font=load_font(25, bold=True), fill=(20, 45, 65))
    radius = max(1, min(5, int(160 / math.sqrt(max(1, len(values)))) + 1))
    for index in order:
        color = tuple(int(channel) for channel in rgb[index])
        x, y = float(px[index]), float(py[index])
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)
    draw.text((42, height - 72),
              f"shown: {len(values)}  X:[{compact_number(positions[:, 0].min())}, {compact_number(positions[:, 0].max())}]  "
              f"Y:[{compact_number(positions[:, 1].min())}, {compact_number(positions[:, 1].max())}]  "
              f"Z:[{compact_number(positions[:, 2].min())}, {compact_number(positions[:, 2].max())}]",
              font=load_font(16), fill=(65, 77, 87))
    draw_colorbar(canvas, draw, 42, height - 42, width - 84, 16, low, high)
    canvas.save(destination)


# 作用：读取 GeoTIFF。读取顺序：rasterio（若已安装，可得到坐标系与仿射变换）→ Pillow →
# 内置的未压缩 strip TIFF 读取器（Pillow 读不了 HiStream 生成的多波段浮点 TIFF 时使用，避免依赖 GDAL）。
# 输出：(数据[波段, 行, 列] float64, NoData 值或 None, 各波段描述, 元数据字典)。
def read_raster(path: Path) -> tuple[np.ndarray, float | None, list[str], dict]:
    try:
        import rasterio  # type: ignore

        with rasterio.open(path) as dataset:
            data = dataset.read().astype(np.float64, copy=False)
            descriptions = [description or f"band_{index + 1}"
                            for index, description in enumerate(dataset.descriptions)]
            metadata = {
                "width": dataset.width,
                "height": dataset.height,
                "band_count": dataset.count,
                "crs": str(dataset.crs or ""),
                "transform": tuple(dataset.transform),
                "driver": dataset.driver,
            }
            return data, dataset.nodata, descriptions, metadata
    except ImportError:
        pass

    image = Image.open(path)
    try:
        return read_raster_with_pillow(image)
    except (OSError, ValueError):
        return read_uncompressed_tiff_strips(path, image)


# 作用：用 Pillow 读取 TIFF：多页 TIFF 的每一页、多通道图像的每个通道各作为一个波段，波段名为 band_n。
# NoData 取自 TIFF 标签 42113（GDAL_NODATA，文本形式）。
def read_raster_with_pillow(image: Image.Image) -> tuple[np.ndarray, float | None, list[str], dict]:
    frames: list[np.ndarray] = []
    descriptions: list[str] = []
    for frame_index in range(getattr(image, "n_frames", 1)):
        image.seek(frame_index)
        array = np.asarray(image, dtype=np.float64)
        if array.ndim == 2:
            frames.append(array)
            descriptions.append(f"band_{len(frames)}")
        elif array.ndim == 3:
            for channel in range(array.shape[2]):
                frames.append(array[:, :, channel])
                descriptions.append(f"band_{len(frames)}")
    if not frames:
        raise ValueError(f"无法读取 TIFF 像元：{path}")
    nodata = image.tag_v2.get(42113)
    try:
        nodata = float(str(nodata).strip("\x00")) if nodata is not None else None
    except ValueError:
        nodata = None
    data = np.stack(frames, axis=0)
    return data, nodata, descriptions, {
        "width": int(data.shape[2]),
        "height": int(data.shape[1]),
        "band_count": int(data.shape[0]),
        "reader": "Pillow fallback",
    }


# 作用：把 TIFF 标签值统一成整数元组（标量也包成单元素元组）。
def tag_tuple(image: Image.Image, key: int, default: tuple[int, ...] = ()) -> tuple[int, ...]:
    value = image.tag_v2.get(key, default)
    if isinstance(value, (tuple, list)):
        return tuple(int(item) for item in value)
    return (int(value),)


# 作用：直接按 TIFF 标签解析未压缩 strip 布局的 TIFF，读出全部波段（Pillow 无法处理时的兜底）。
# 使用的标签：256 宽、257 高、258 位深、259 压缩、273 strip 偏移、277 每像素样本数、278 每 strip 行数、
# 279 strip 字节数、284 平面配置、339 样本格式、42112 GDAL_METADATA、42113 GDAL_NODATA。
# 步骤 1：读标签并校验（仅支持压缩=1 的未压缩、各波段同类型的数据）；步骤 2：由样本格式与位深确定 dtype 与字节序；
# 步骤 3：按 strip 读入（PlanarConfiguration=2 分波段存放，=1 逐像素交错）；
# 步骤 4：从 GDAL_METADATA 的 XML 里取 role=description 的项作为波段描述；步骤 5：解析 NoData。
def read_uncompressed_tiff_strips(path: Path, image: Image.Image) -> tuple[np.ndarray, float | None, list[str], dict]:
    """读取 HiStream 生成的未压缩、strip 布局 TIFF，避免依赖 GDAL。"""
    width = int(image.tag_v2.get(256, image.width))
    height = int(image.tag_v2.get(257, image.height))
    compression = int(image.tag_v2.get(259, 1))
    samples = int(image.tag_v2.get(277, 1))
    rows_per_strip = int(image.tag_v2.get(278, height))
    planar = int(image.tag_v2.get(284, 1))
    offsets = tag_tuple(image, 273)
    byte_counts = tag_tuple(image, 279)
    bits = tag_tuple(image, 258, (8,))
    sample_formats = tag_tuple(image, 339, (1,))
    if compression != 1:
        raise ValueError("Pillow 无法读取该 TIFF，且它不是未压缩格式；请安装 rasterio")
    if not offsets or len(offsets) != len(byte_counts):
        raise ValueError("TIFF strip 偏移或长度无效；请安装 rasterio")

    # 步骤 2：按 (样本格式, 位深) 选 numpy 类型；样本格式 1=无符号整型、2=有符号整型、3=浮点；8 位数据无需字节序。
    bit_depth = bits[0]
    sample_format = sample_formats[0]
    if any(value != bit_depth for value in bits) or any(value != sample_format for value in sample_formats):
        raise ValueError("各波段 TIFF 数据类型不同；请安装 rasterio")
    with path.open("rb") as header_stream:
        byte_order = "<" if header_stream.read(2) == b"II" else ">"
    type_code = {
        (1, 8): "u1", (1, 16): "u2", (1, 32): "u4", (1, 64): "u8",
        (2, 8): "i1", (2, 16): "i2", (2, 32): "i4", (2, 64): "i8",
        (3, 32): "f4", (3, 64): "f8",
    }.get((sample_format, bit_depth))
    if type_code is None:
        raise ValueError(f"不支持的 TIFF sampleFormat/bits：{sample_format}/{bit_depth}")
    dtype = np.dtype(type_code if bit_depth == 8 else byte_order + type_code)
    data = np.empty((samples, height, width), dtype=np.float64)
    strips_per_plane = math.ceil(height / rows_per_strip)

    with path.open("rb") as stream:
        if planar == 2:
            if len(offsets) < samples * strips_per_plane:
                raise ValueError("TIFF planar strip 数量不足")
            for band in range(samples):
                for strip in range(strips_per_plane):
                    index = band * strips_per_plane + strip
                    row0 = strip * rows_per_strip
                    rows = min(rows_per_strip, height - row0)
                    stream.seek(offsets[index])
                    raw = stream.read(byte_counts[index])
                    values = np.frombuffer(raw, dtype=dtype, count=rows * width)
                    data[band, row0:row0 + rows, :] = values.reshape(rows, width)
        elif planar == 1:
            for strip, (offset, byte_count) in enumerate(zip(offsets, byte_counts)):
                row0 = strip * rows_per_strip
                if row0 >= height:
                    break
                rows = min(rows_per_strip, height - row0)
                stream.seek(offset)
                raw = stream.read(byte_count)
                values = np.frombuffer(raw, dtype=dtype, count=rows * width * samples)
                chunk = values.reshape(rows, width, samples)
                data[:, row0:row0 + rows, :] = np.moveaxis(chunk, -1, 0)
        else:
            raise ValueError(f"不支持的 TIFF PlanarConfiguration：{planar}")

    # 步骤 4：波段描述缺省为 band_n；GDAL 写入的 description 项覆盖缺省值（sample 为从 0 起的波段序号）。
    descriptions = [f"band_{index + 1}" for index in range(samples)]
    gdal_metadata = image.tag_v2.get(42112)
    if gdal_metadata:
        try:
            root = ET.fromstring(str(gdal_metadata).strip("\x00"))
            for item in root.findall(".//Item"):
                if item.attrib.get("role") != "description":
                    continue
                sample = int(item.attrib.get("sample", "0"))
                if 0 <= sample < samples and item.text:
                    descriptions[sample] = item.text
        except (ET.ParseError, ValueError):
            pass
    nodata = image.tag_v2.get(42113)
    try:
        nodata = float(str(nodata).strip("\x00")) if nodata is not None else None
    except ValueError:
        nodata = None
    return data, nodata, descriptions, {
        "width": width,
        "height": height,
        "band_count": samples,
        "bits_per_sample": bit_depth,
        "sample_format": sample_format,
        "reader": "built-in TIFF strip reader",
    }


# 作用：解析 --bands / --fields 之类的选择字符串，返回去重后的下标列表（保持输入顺序）。
# 规则：缺省（None）时不超过 default_limit 个则全选，否则取前 default_limit-1 个再加最后一个；
# "all" 或空串为全选；逗号分隔的每项可为标识名或从 1 起的序号；未知项抛 ValueError。
def parse_selection(value: str | None, identifiers: Sequence[str], default_limit: int = 8) -> list[int]:
    if not identifiers:
        return []
    if value is None:
        if len(identifiers) <= default_limit:
            return list(range(len(identifiers)))
        return list(range(default_limit - 1)) + [len(identifiers) - 1]
    tokens = [token.strip() for token in value.split(",") if token.strip()]
    if not tokens or tokens == ["all"]:
        return list(range(len(identifiers)))
    selected: list[int] = []
    for token in tokens:
        if token in identifiers:
            selected.append(identifiers.index(token))
        elif token.isdigit() and 1 <= int(token) <= len(identifiers):
            selected.append(int(token) - 1)
        else:
            raise ValueError(f"未知字段/波段 {token}；可选值：{', '.join(identifiers)}")
    return list(dict.fromkeys(selected))


# 作用：处理一幅 GeoTIFF：写统计 CSV/摘要，并为选定波段生成热力图与直方图。输出：生成的文件路径列表。
# 步骤 1：读取栅格；步骤 2：逐波段确定 NoData；步骤 3：计算统计并写 statistics.csv / summary.txt；
# 步骤 4：对选定波段渲染 <band>_heatmap.png 与 <band>_histogram.png。
def analyze_image(path: Path, destination: Path, bands: str | None) -> list[Path]:
    data, nodata, descriptions, metadata = read_raster(path)
    identifiers = [f"band_{index + 1}" for index in range(data.shape[0])]
    band_nodata: list[float | None] = []
    for index, description in enumerate(descriptions):
        effective_nodata = nodata
        # 步骤 2：兼容未写 NoData 的热红外产品。若波段描述像热红外（含 temperature/thermal/亮温/温度，
        # 或形如“@ 数千 nm”的波长，具体含义待核对），且至少 1% 的有限值不大于 0，就把 0 当作 NoData，
        # 避免无数据像元（温度为 0 K）拉低统计与色带范围。
        thermal = bool(re.search(r"temperature|thermal|亮温|温度|@\s*[3-9][0-9]{3,}\s*nm",
                                 description, re.IGNORECASE))
        if effective_nodata is None and thermal:
            finite = np.asarray(data[index])[np.isfinite(data[index])]
            if finite.size and np.count_nonzero(finite <= 0.0) / finite.size >= 0.01:
                effective_nodata = 0.0
        band_nodata.append(effective_nodata)
    rows = [
        (identifier, descriptions[index], calculate_statistics(data[index], band_nodata[index]))
        for index, identifier in enumerate(identifiers)
    ]
    csv_path, txt_path = write_statistics(destination, path, "GeoTIFF", rows, metadata)
    files = [csv_path, txt_path]
    for index in parse_selection(bands, identifiers):
        identifier = identifiers[index]
        title = f"{path.name} - {descriptions[index]}"
        heatmap = destination / f"{identifier}_heatmap.png"
        histogram = destination / f"{identifier}_histogram.png"
        render_heatmap(data[index], heatmap, title, band_nodata[index])
        render_histogram(data[index], histogram, title, band_nodata[index])
        files.extend((heatmap, histogram))
    return files


# 作用：以内存映射方式打开过程结果的二进制文件，形状为 (记录数, recordFloats)，小端 float32。
# 校验：recordFloats 必须为正，二进制文件必须存在且不短于 记录数 × recordFloats × 4 字节，否则抛异常。
def load_float_records(metadata_path: Path, metadata: dict,
                       record_count: int) -> np.memmap:
    record_floats = int(metadata.get("recordFloats", 0))
    if record_floats <= 0:
        raise ValueError("过程 JSON 缺少有效 recordFloats")
    data_path = (metadata_path.parent / str(metadata.get("dataFile", ""))).resolve()
    if not data_path.is_file():
        raise FileNotFoundError(f"找不到过程二进制文件：{data_path}")
    expected = record_count * record_floats * 4
    actual = data_path.stat().st_size
    if actual < expected:
        raise ValueError(f"过程二进制文件不完整：期望至少 {expected} 字节，实际 {actual} 字节")
    return np.memmap(data_path, dtype="<f4", mode="r", shape=(record_count, record_floats))


# 作用：读取体元过程结果（JSON 元数据 + 二进制记录）为 StructureData。
# 输入：metadata 需含 voxelCount、recordFloats、dataFile，字段由 fields[{id,label,offset}] 给出；
# positionOffsets（缺省 0/1/2）指明每条记录里 X/Y/Z 的位置。
# 步骤 1：读取记录与坐标；步骤 2：按 fields 建立各标量字段；步骤 3：若有 windX/windVertical/windY 则派生三维风速；
# 步骤 4：按 soilProfile 的深度与偏移追加各层土壤温度（深度 m，标签单位 degC）；
# 步骤 5：若有 componentOffset，则读取并校验体元组分编码。
def voxel_structure(metadata_path: Path, metadata: dict) -> StructureData:
    count = int(metadata.get("voxelCount", 0))
    if count <= 0:
        raise ValueError("体元过程 JSON 缺少 voxelCount")
    records = load_float_records(metadata_path, metadata, count)
    offsets = [int(value) for value in metadata.get("positionOffsets", [0, 1, 2])]
    if len(offsets) != 3:
        raise ValueError("positionOffsets 必须包含 X/Y/Z 三个偏移")
    positions = np.asarray(records[:, offsets], dtype=np.float64)
    fields = [
        FieldData(str(field.get("id", f"field_{field['offset']}")),
                  str(field.get("label", field.get("id", "field"))),
                  np.asarray(records[:, int(field["offset"])], dtype=np.float64))
        for field in metadata.get("fields", [])
        if isinstance(field, dict) and "offset" in field
    ]
    by_id = {field.identifier: field for field in fields}
    if {"windX", "windVertical", "windY"}.issubset(by_id):
        speed = np.sqrt(by_id["windX"].values ** 2 + by_id["windVertical"].values ** 2 +
                        by_id["windY"].values ** 2)
        fields.append(FieldData("windSpeed", "三维风速 [m s^-1]", speed))
    profile = metadata.get("soilProfile")
    if isinstance(profile, dict):
        depths = profile.get("depths", [])
        profile_offsets = profile.get("offsets", [])
        for depth, offset in zip(depths, profile_offsets):
            identifier = f"soil_temperature_{str(depth).replace('.', 'p')}m"
            fields.append(FieldData(identifier, f"土壤温度 {depth} m [degC]",
                                    np.asarray(records[:, int(offset)], dtype=np.float64)))
    components = None
    if "componentOffset" in metadata:
        offset = int(metadata["componentOffset"])
        if not 0 <= offset < records.shape[1]:
            raise ValueError("componentOffset 超出记录范围")
        raw = records[:, offset]
        if not np.isin(raw, list(COMPONENT_LABELS)).all():
            raise ValueError("体元组分字段包含无效编码")
        components = np.asarray(raw, dtype=np.uint8)
    return StructureData(metadata_path, "voxel", positions, fields, count, metadata, components)


# 作用：读取面元过程结果为 StructureData。每个面元有正反两面各一个值（记录数 = facetCount × 2）。
# 几何：从 geometryFile（缺省 ../faceteb.json）读顶点 vertexPositions，每面元 3 个顶点（共 9 个数），取三顶点平均为中心。
# side：front 取正面，back 取反面，其他（mean）取两面均值（忽略 NaN）。组分编码先找过程文件，找不到再找几何文件。
def facet_process_structure(metadata_path: Path, metadata: dict,
                            side: str) -> StructureData:
    facet_count = int(metadata.get("facetCount", 0))
    if facet_count <= 0:
        raise ValueError("面元过程 JSON 缺少 facetCount")
    records = load_float_records(metadata_path, metadata, facet_count * 2)
    geometry_path = (metadata_path.parent / str(metadata.get("geometryFile", "../faceteb.json"))).resolve()
    geometry = json.loads(geometry_path.read_text(encoding="utf-8"))
    vertices = np.asarray(geometry.get("vertexPositions", []), dtype=np.float64)
    if vertices.size != facet_count * 9:
        raise ValueError(f"面元几何数量与 facetCount 不一致：{geometry_path}")
    centers = vertices.reshape(facet_count, 3, 3).mean(axis=1)
    fields: list[FieldData] = []
    for field in metadata.get("fields", []):
        if not isinstance(field, dict) or "offset" not in field:
            continue
        values = np.asarray(records[:, int(field["offset"])], dtype=np.float64).reshape(facet_count, 2)
        if side == "front":
            selected = values[:, 0]
        elif side == "back":
            selected = values[:, 1]
        else:
            selected = np.nanmean(values, axis=1)
        identifier = str(field.get("id", f"field_{field['offset']}"))
        fields.append(FieldData(identifier, f"{field.get('label', identifier)} ({side})", selected))
    components = read_components(metadata_path, metadata, facet_count)
    if components is None:
        components = read_components(geometry_path, geometry, facet_count)
    return StructureData(metadata_path, "facet", centers, fields, facet_count, metadata, components)


# 作用：读取面元结果 JSON（含 vertexPositions 与逐面元数组）为 StructureData。
# 说明：长度等于 facetCount 的数值数组作为单值字段；长度等于 2×facetCount 的按正反面两列处理，再按 side 选取；其他键忽略。
def facet_result_structure(path: Path, payload: dict, side: str) -> StructureData:
    facet_count = int(payload.get("facetCount", 0))
    vertices = np.asarray(payload.get("vertexPositions", []), dtype=np.float64)
    if facet_count <= 0 or vertices.size != facet_count * 9:
        raise ValueError("面元结果缺少有效 facetCount/vertexPositions")
    centers = vertices.reshape(facet_count, 3, 3).mean(axis=1)
    ignored = {"vertexPositions", "wavelengths"}
    fields: list[FieldData] = []
    for key, raw in payload.items():
        if key in ignored or not isinstance(raw, list):
            continue
        values = np.asarray(raw)
        if not np.issubdtype(values.dtype, np.number):
            continue
        if values.size == facet_count:
            fields.append(FieldData(key, key, values.astype(np.float64)))
        elif values.size == facet_count * 2:
            surfaces = values.astype(np.float64).reshape(facet_count, 2)
            selected = surfaces[:, 0] if side == "front" else surfaces[:, 1] if side == "back" \
                else np.nanmean(surfaces, axis=1)
            fields.append(FieldData(key, f"{key} ({side})", selected))
    return StructureData(path, "facet", centers, fields, facet_count, payload,
                         read_components(path, payload, facet_count))


# 作用：按 JSON 内容判别类型并读取：kind 以“-process”结尾为过程结果（geometry 为 voxel 或 facet）；
# 含 vertexPositions 的为面元结果；其他抛 ValueError。
def read_structure(path: Path, facet_side: str) -> StructureData:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if str(payload.get("kind", "")).endswith("-process"):
        geometry = str(payload.get("geometry", ""))
        if geometry == "voxel":
            return voxel_structure(path, payload)
        if geometry == "facet":
            return facet_process_structure(path, payload, facet_side)
        raise ValueError(f"不支持的过程几何类型：{geometry}")
    if "vertexPositions" in payload:
        return facet_result_structure(path, payload, facet_side)
    raise ValueError("JSON 不是 HiStream 三维过程或面元结果")


# 作用：等间隔抽样至多 maximum_points 个元素，写出 sample_points.csv（来源下标、坐标 x/y/z、组分名与所选字段值）。
# 说明：source_indices 记录抽样前在原结果中的下标，组分结果的子集据此仍可追溯到原元素。
def write_point_csv(destination: Path, structure: StructureData,
                    fields: Sequence[FieldData], maximum_points: int) -> Path:
    indices = sample_indices(structure.element_count, maximum_points)
    path = destination / "sample_points.csv"
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("source_index", "x", "y", "z", "component", *(field.identifier for field in fields)))
        for index in indices:
            writer.writerow(
                (
                    int(structure.source_indices[index]) if structure.source_indices is not None else int(index),
                    compact_number(structure.positions[index, 0]),
                    compact_number(structure.positions[index, 1]),
                    compact_number(structure.positions[index, 2]),
                    COMPONENT_LABELS[int(structure.components[index])] if structure.components is not None else "未分类",
                    *(compact_number(field.values[index]) for field in fields),
                )
            )
    return path


# 作用：按组分（soil/vegetation/building）分别统计温度与能量字段，写入 destination/components/。
# 输出：生成文件的路径列表。每个组分一个子目录，另有汇总表 component_statistics.csv 与 summary.txt。
# 步骤 1：筛选能量类字段（ENERGY_FIELDS），把温度（K）另导出为 ℃ 字段 temperatureC；
# 步骤 2：对每个组分按编码取出元素，写统计、抽样点表、直方图与三维图；步骤 3：写总说明 summary.txt。
# 说明：统计用全部有效元素，不抽样、不加权；光伏、水体与未分类元素不并入这三类。
def analyze_components(structure: StructureData, destination: Path,
                       components: Sequence[str], maximum_points: int) -> list[Path]:
    """All-element, unweighted distributions; never infer identity from geometry or temperature."""
    output = destination / "components"
    output.mkdir(parents=True, exist_ok=True)
    combined = output / "component_statistics.csv"
    files = [combined]
    fields = [field for field in structure.fields if field.identifier in ENERGY_FIELDS]
    # Native surface temperatures are K. Export a convenient Celsius field,
    # while leaving the generic statistics in their original units.
    fields = [FieldData("temperatureC", "温度 [℃]", field.values - 273.15)
              if field.identifier == "temperature" and ("[K]" in field.label or structure.kind == "facet")
              else field for field in fields]
    with combined.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("component", "component_label", "field_id", "field_label", *STAT_COLUMNS))
        for name in components:
            code, label = COMPONENTS[name]
            indices = np.flatnonzero(structure.components == code)
            folder = output / name
            folder.mkdir(parents=True, exist_ok=True)
            selected = [FieldData(field.identifier, field.label, field.values[indices]) for field in fields]
            rows = [(field.identifier, field.label, calculate_statistics(field.values)) for field in selected]
            for identifier, field_label, stats in rows:
                writer.writerow((name, label, identifier, field_label, *(compact_number(stats[k]) for k in STAT_COLUMNS)))
            files.extend(write_statistics(folder, structure.source, label, rows, structure.metadata))
            subset = StructureData(structure.source, structure.kind, structure.positions[indices], selected,
                                   len(indices), structure.metadata, structure.components[indices], indices)
            files.append(write_point_csv(folder, subset, selected, maximum_points))
            for field in selected:
                if not np.isfinite(field.values).any():
                    continue
                histogram = folder / f"{safe_name(field.identifier)}_histogram.png"
                plot = folder / f"{safe_name(field.identifier)}_3d.png"
                title = f"{label} - {field.label} - {structure.metadata.get('time', '')}"
                render_histogram(field.values, histogram, title)
                files.append(histogram)
                if (np.isfinite(subset.positions).all(axis=1) & np.isfinite(field.values)).any():
                    render_structure(subset.positions, field.values, plot, title, maximum_points)
                    files.append(plot)
    summary = output / "summary.txt"
    counts = {label: int(np.count_nonzero(structure.components == code)) for code, label in COMPONENT_LABELS.items()}
    summary.write_text("按全部有效元素统计；均值不做面积加权，能量字段为通量而非总功率。\n"
                       "点表与三维图最多按每组抽样；直方图和统计表不抽样。\n"
                       "光伏、水体和未分类元素不混入土壤/植被/建筑。\n"
                       + "\n".join(f"{name}: {count}" for name, count in counts.items())
                       + ("\n当前文件没有温度/能量字段，请选择能量或辐射过程结果。" if not fields else "")
                       + "\n", encoding="utf-8")
    files.append(summary)
    return files


# 作用：处理一份三维过程/面元结果：写全部字段的统计与所选字段的点表、三维图、直方图。
# 分支：有组分标识且未用 --fields 指定字段时，改为按组分分析（analyze_components）；
# 没有组分标识的旧结果则写 component_notice.txt 并在 stderr 提示，随后仍做通用统计。
def analyze_structure(path: Path, destination: Path, fields_option: str | None,
                      facet_side: str, maximum_points: int,
                      components: Sequence[str] = tuple(COMPONENTS)) -> list[Path]:
    structure = read_structure(path, facet_side)
    identifiers = [field.identifier for field in structure.fields]
    selected_indices = parse_selection(fields_option, identifiers)
    selected = [structure.fields[index] for index in selected_indices]
    rows = [(field.identifier, field.label, calculate_statistics(field.values))
            for field in structure.fields]
    csv_path, txt_path = write_statistics(destination, path, structure.kind, rows, structure.metadata)
    if components and structure.components is not None and fields_option is None:
        return [csv_path, txt_path, *analyze_components(structure, destination, components, maximum_points)]
    if components and structure.components is None:
        message = "旧结果缺少组分标识，无法按土壤/背景、植被和建筑分类；请用新版引擎重跑并开启过程输出。"
        (destination / "component_notice.txt").write_text(message + "\n", encoding="utf-8")
        print(message, file=sys.stderr)
    files = [csv_path, txt_path, write_point_csv(destination, structure, selected, maximum_points)]
    for field in selected:
        name = safe_name(field.identifier)
        plot = destination / f"{name}_3d.png"
        histogram = destination / f"{name}_histogram.png"
        render_structure(structure.positions, field.values, plot,
                         f"{path.name} - {field.label}", maximum_points)
        render_histogram(field.values, histogram, f"{path.name} - {field.label}")
        files.extend((plot, histogram))
    return files


# 作用：枚举可处理的文件：单个文件原样返回；目录则递归找 .tif/.tiff 以及 kind 以“-process”结尾或含 vertexPositions 的 JSON。
# 说明：跳过路径中含 statistics_output 的文件，避免把上次的输出再统计一遍。
def candidate_files(root: Path) -> Iterable[Path]:
    if root.is_file():
        yield root
        return
    for path in root.rglob("*"):
        if not path.is_file() or "statistics_output" in path.parts:
            continue
        if path.suffix.lower() in {".tif", ".tiff"}:
            yield path
        elif path.suffix.lower() == ".json":
            try:
                payload = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError, UnicodeDecodeError):
                continue
            if str(payload.get("kind", "")).endswith("-process") or "vertexPositions" in payload:
                yield path


# 作用：命令行入口。解析并校验参数 → 确定输出根目录（缺省在输入旁建 statistics_output）→ 逐个文件分析。
# 返回：全部成功且至少成功一个返回 0，否则返回 1；单个文件异常只记录到 stderr，不中断后续文件。
# 约定：--max-points 不得小于 100；--components 为 none 时保留通用字段/绘图模式。
def main() -> int:
    parser = argparse.ArgumentParser(
        description="将 HiStream TIFF 和三维过程结果导出为 TXT、CSV 与 PNG。"
    )
    parser.add_argument("input", type=Path, help="TIFF/JSON 文件，或包含结果的目录")
    parser.add_argument("-o", "--output", type=Path, help="统计输出目录，默认在输入旁创建 statistics_output")
    parser.add_argument("--fields", help="三维字段 ID，逗号分隔；all 表示全部")
    parser.add_argument("--bands", help="TIFF 波段编号，逗号分隔；all 表示全部")
    parser.add_argument("--facet-side", choices=("mean", "front", "back"), default="mean",
                        help="面元正反面处理方式，默认 mean")
    parser.add_argument("--max-points", type=int, default=50000,
                        help="CSV 与三维图最大抽样点数，默认 50000")
    parser.add_argument("--components", default="soil,vegetation,building",
                        help="分组统计土壤/背景、植被、建筑的温度与能量；none 保留原通用字段/绘图模式")
    args = parser.parse_args()
    components = [] if args.components == "none" else list(dict.fromkeys(args.components.split(",")))
    if any(name not in COMPONENTS for name in components):
        parser.error("--components 只支持 soil,vegetation,building 或 none")

    source = args.input.resolve()
    if not source.exists():
        parser.error(f"输入不存在：{source}")
    if args.max_points < 100:
        parser.error("--max-points 不能小于 100")
    destination_root = args.output.resolve() if args.output else (
        source / "statistics_output" if source.is_dir() else source.parent / "statistics_output"
    )
    destination_root.mkdir(parents=True, exist_ok=True)

    succeeded = 0
    failed = 0
    for path in candidate_files(source):
        try:
            relative = str(path.relative_to(source)) if source.is_dir() else path.stem
            destination = output_folder(path, destination_root, relative)
            if path.suffix.lower() in {".tif", ".tiff"}:
                outputs = analyze_image(path, destination, args.bands)
            else:
                outputs = analyze_structure(path, destination, args.fields,
                                            args.facet_side, args.max_points, components)
            succeeded += 1
            print(f"OK  {path}")
            print(f"    {destination} ({len(outputs)} files)")
        except Exception as error:  # 单个文件失败不阻断批处理。
            failed += 1
            print(f"ERROR {path}: {error}", file=sys.stderr)

    print(f"完成：成功 {succeeded}，失败 {failed}，输出 {destination_root}")
    return 0 if succeeded > 0 and failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
