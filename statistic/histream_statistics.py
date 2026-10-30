#!/usr/bin/env python3
"""HiStream 独立结果查看与统计工具。

支持：
1. GeoTIFF 图像：逐波段统计、热力图、直方图。
2. VoxelRT/VoxelEB/体元流体过程：字段统计、抽样点 CSV、三维结构图。
3. FacetRT/FacetEB：面元中心统计和三维结构图。

仅需 numpy 与 Pillow；安装 rasterio 后可更完整地读取多波段 GeoTIFF 和 NoData。
"""

from __future__ import annotations

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


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


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


@dataclass
class FieldData:
    identifier: str
    label: str
    values: np.ndarray


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


COMPONENTS = {"soil": (1, "土壤/背景"), "vegetation": (2, "植被"), "building": (3, "建筑")}
COMPONENT_LABELS = {0: "未分类", 1: "土壤/背景", 2: "植被", 3: "建筑", 4: "光伏", 5: "水体"}
ENERGY_FIELDS = {"temperature", "shortwaveRadiation", "longwaveRadiation", "netRadiation",
                 "sensibleHeat", "latentHeat", "surfaceHeatFlux", "storageHeat"}


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


def safe_name(value: str) -> str:
    value = re.sub(r"[^0-9A-Za-z._-]+", "_", value.strip())
    return value.strip("._") or "result"


def output_folder(source: Path, root: Path | None, relative_key: str = "") -> Path:
    if root is None:
        root = source.parent / "statistics_output"
    suffix = hashlib.sha1(str(source.resolve()).encode("utf-8")).hexdigest()[:8]
    name = safe_name(relative_key or source.stem)
    folder = root / f"{name}_{suffix}"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


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


def finite_values(values: np.ndarray, nodata: float | None = None) -> tuple[np.ndarray, np.ndarray]:
    array = np.asarray(values, dtype=np.float64)
    valid = np.isfinite(array)
    if nodata is not None and math.isfinite(float(nodata)):
        valid &= ~np.isclose(array, float(nodata), rtol=0.0, atol=max(1e-12, abs(float(nodata)) * 1e-7))
    return array[valid], valid


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


def colorize(normalized: np.ndarray) -> np.ndarray:
    normalized = np.clip(np.asarray(normalized, dtype=np.float64), 0.0, 1.0)
    channels = [np.interp(normalized, COLOR_STOPS[:, 0], COLOR_STOPS[:, index]) for index in range(1, 4)]
    return np.stack(channels, axis=-1).astype(np.uint8)


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


def sample_indices(count: int, maximum: int) -> np.ndarray:
    if count <= maximum:
        return np.arange(count, dtype=np.int64)
    return np.linspace(0, count - 1, maximum, dtype=np.int64)


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

    center = 0.5 * (positions.min(axis=0) + positions.max(axis=0))
    span = np.maximum(positions.max(axis=0) - positions.min(axis=0), 1e-9)
    scaled = (positions - center) / float(np.max(span))
    yaw = math.radians(38.0)
    elevation = math.radians(24.0)
    u = scaled[:, 0] * math.cos(yaw) - scaled[:, 2] * math.sin(yaw)
    depth = scaled[:, 0] * math.sin(yaw) + scaled[:, 2] * math.cos(yaw)
    v = scaled[:, 1] * math.cos(elevation) - depth * math.sin(elevation)
    camera_depth = depth * math.cos(elevation) + scaled[:, 1] * math.sin(elevation)
    order = np.argsort(camera_depth)

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


def tag_tuple(image: Image.Image, key: int, default: tuple[int, ...] = ()) -> tuple[int, ...]:
    value = image.tag_v2.get(key, default)
    if isinstance(value, (tuple, list)):
        return tuple(int(item) for item in value)
    return (int(value),)


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


def analyze_image(path: Path, destination: Path, bands: str | None) -> list[Path]:
    data, nodata, descriptions, metadata = read_raster(path)
    identifiers = [f"band_{index + 1}" for index in range(data.shape[0])]
    band_nodata: list[float | None] = []
    for index, description in enumerate(descriptions):
        effective_nodata = nodata
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
