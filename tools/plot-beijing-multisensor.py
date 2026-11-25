# ==========================================================================
# 文件：plot-beijing-multisensor.py
# 功能：为「北京市 · 光学与热红外多传感器观测模拟」案例生成报告配图（PNG，每张宽高均 < 3000 px）。
# 数据源（--source）：
#   reference（缺省）：只用 assets/examples/beijing_multisensor/reference/ 里的精简结果
#       （组分温度 components.tif、各传感器 *_optical.tif(float16)、*_tir_bt.tif、results/*.csv），可在仓库内复现；
#   run：用 run_all.mjs 的完整输出目录（--run-dir，缺省 C:\work\streamsim\scratch\bj_multisensor\run），
#       额外生成 EB 通量图，并用 tir_surface_temperature.tif 写出 tables/tir_bt_minus_lst.csv
#       （亮温 - 发射率归一化地表温度的逐波段统计；reference 源没有该栅格，改读这份 CSV 画图）。
# 步骤：
#   1. 注册 Deng/SimHei 中文字体；Data 类按源读取栅格（光学/热红外/组分温度/输入图）
#   2. 逐图函数 fig_inputs / fig_eb / fig_eb_fluxes / fig_optical_bands / fig_ndvi / fig_tir_maps /
#      fig_tir_spectral / fig_angles / fig_aggregation 绘图并经 save_fig 保存（保存后用 PIL 检查尺寸并断言）
#   3. generate_figures 依次调用并返回 {图名: 路径}；命令行 main 解析参数
# 数据流：reference/ 或 run/ 的栅格 + results/*.csv + sensors.json → docs/cases/beijing-multisensor/figures/*.png
# 运行：C:\work\miniconda\python.exe -I tools\plot-beijing-multisensor.py [--source reference|run] [--run-dir DIR]
# 依赖：numpy、matplotlib、Pillow、osgeo(GDAL ≥ 3.11 才能读 float16 参考栅格)
# ==========================================================================

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib import font_manager  # noqa: E402
from matplotlib.colors import BoundaryNorm, ListedColormap  # noqa: E402
from PIL import Image  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CASE = ROOT / "assets" / "examples" / "beijing_multisensor"
REPORT_DIR = ROOT / "docs" / "cases" / "beijing-multisensor"
FIG_DIR = REPORT_DIR / "figures"
TABLE_DIR = REPORT_DIR / "tables"
DEFAULT_RUN = Path(r"C:\work\streamsim\scratch\bj_multisensor\run2")
MAX_PX = 3000  # 全局约定：任何图片宽高都必须小于 3000 像素
DPI = 150

sys.path.insert(0, str(CASE))
import raster_tools  # noqa: E402  案例自带的栅格读取工具（依赖 osgeo）

SENSORS = {
    "landsat89": ("Landsat 8/9", "#1f77b4"),
    "modis": ("MODIS", "#d62728"),
    "sentinel2": ("Sentinel-2", "#2ca02c"),
    "viirs": ("VIIRS", "#9467bd"),
    "fy3d_mersi2": ("FY-3D MERSI-II", "#ff7f0e"),
    "aster": ("ASTER", "#8c564b"),
    "ecostress": ("ECOSTRESS", "#17becf"),
}
OPTICAL_SENSORS = ["landsat89", "modis", "sentinel2", "viirs", "fy3d_mersi2"]
THERMAL_SENSORS = ["landsat89", "modis", "viirs", "fy3d_mersi2", "aster", "ecostress"]
# 同名（同光谱区）波段对照：红、近红外、短波红外 1
BAND_GROUPS = {
    "红光": {"landsat89": "B4", "modis": "B1", "sentinel2": "B4", "viirs": "M5", "fy3d_mersi2": "CH3"},
    "近红外": {"landsat89": "B5", "modis": "B2", "sentinel2": "B8", "viirs": "M7", "fy3d_mersi2": "CH4"},
    "短波红外 1": {"landsat89": "B6", "modis": "B6", "sentinel2": "B11", "viirs": "M10", "fy3d_mersi2": "CH6"},
}
# IGBP 地类：编码 → (名称, 颜色)；255 为市域外填充值
IGBP = {
    1: ("常绿针叶林", "#1b5e20"), 3: ("落叶针叶林", "#558b2f"), 4: ("落叶阔叶林", "#7cb342"),
    10: ("草地", "#d4e157"), 11: ("湿地", "#26a69a"), 12: ("农田", "#ffd54f"),
    13: ("城市建成区", "#e53935"), 16: ("裸地", "#bcaaa4"), 17: ("水体", "#1e88e5"),
}


# 作用：注册中文字体（Deng 优先，SimHei 备选），并统一全局绘图风格。
def setup_fonts() -> None:
    names = []
    for name in ("Deng.ttf", "simhei.ttf"):
        path = Path(r"C:\Windows\Fonts") / name
        if path.exists():
            font_manager.fontManager.addfont(str(path))
            names.append(font_manager.FontProperties(fname=str(path)).get_name())
    plt.rcParams["font.sans-serif"] = names + ["DejaVu Sans"]
    plt.rcParams["font.family"] = "sans-serif"
    plt.rcParams["axes.unicode_minus"] = False
    plt.rcParams["axes.grid"] = False
    plt.rcParams["font.size"] = 10


# 作用：按数据源读取栅格并缓存。reference 用精简参考结果，run 用 run_all.mjs 的完整输出。
# 输出：optical(sid)/thermal(sid) → {波段名: (中心波长, 数组)}；components()/fluxes()/lst(sid) 等。
class Data:
    def __init__(self, source: str, run_dir: Path):
        self.source = source
        self.run_dir = run_dir
        self.cache: dict = {}
        self.sensors = json.loads((CASE / "sensors.json").read_text(encoding="utf-8"))["sensors"]
        # 网格几何由输入图确定
        _, _, arr, geo, proj = raster_tools.read_bands(str(CASE / "inputs" / "type.tif"))
        self.geo, self.proj = geo, proj
        self.type = arr[0]
        self.valid = self.type != 255
        self.height, self.width = self.type.shape

    # 作用：读取多波段栅格，返回 {名称: (中心, 数组)}；热红外中心波长换算为 μm。
    def _bands(self, path: Path, thermal: bool) -> dict:
        key = str(path)
        if key not in self.cache:
            names, centers, arr, _, _ = raster_tools.read_bands(key)
            scale = 1000.0 if thermal else 1.0
            out = {}
            for i, name in enumerate(names):
                center = float(centers[i].replace("nm", "").strip()) / scale if centers[i] else float("nan")
                out[name] = (center, arr[i])
            self.cache[key] = out
        return self.cache[key]

    def optical(self, sid: str) -> dict:
        path = CASE / "reference" / f"{sid}_optical.tif" if self.source == "reference" else self.run_dir / sid / "optical_reflectance.tif"
        return self._bands(path, False)

    def thermal(self, sid: str) -> dict:
        path = CASE / "reference" / f"{sid}_tir_bt.tif" if self.source == "reference" else self.run_dir / sid / "tir_brightness_temperature.tif"
        return self._bands(path, True)

    def components(self) -> dict:
        path = CASE / "reference" / "components.tif" if self.source == "reference" else self.run_dir / "parent" / "components.tif"
        names, _, arr, _, _ = raster_tools.read_bands(str(path))
        return {n: arr[i] for i, n in enumerate(names)}

    def fluxes(self):
        path = self.run_dir / "parent" / "eb_fluxes.tif"
        if self.source != "run" or not path.exists():
            return None
        _, _, arr, _, _ = raster_tools.read_bands(str(path))
        return {"Rn": arr[0], "H": arr[1], "LE": arr[2], "G": arr[3]}

    def lst(self, sid: str):
        path = self.run_dir / sid / "tir_surface_temperature.tif"
        if self.source != "run" or not path.exists():
            return None
        return self._bands(path, True)

    def input_raster(self, name: str) -> np.ndarray:
        return raster_tools.read_bands(str(CASE / "inputs" / name))[2][0]

    def extent_km(self):
        return [0, self.width * abs(self.geo[1]) / 1000.0, 0, self.height * abs(self.geo[5]) / 1000.0]

    def thermal_fwhm(self, sid: str, band: str):
        for s in self.sensors:
            if s["id"] == sid:
                for b in s["thermal"]:
                    if b["name"] == band:
                        return b["fwhm"]
        return None


# 作用：保存图片，保存后用 PIL 读取实际尺寸并断言宽高都小于 3000 像素（bbox_inches=tight 可能改变尺寸）。
def save_fig(fig, name: str, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{name}.png"
    fig.savefig(path, dpi=DPI, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    with Image.open(path) as image:
        width, height = image.size
    assert width < MAX_PX and height < MAX_PX, f"{path.name} 尺寸 {width}x{height} 超过 {MAX_PX} px"
    print(f"  {path.name}: {width}x{height}px, {path.stat().st_size / 1024:.0f} KB")
    return path


# 作用：画一张地图（imshow，单位 km），无效像元透明；返回 AxesImage 便于加色标。
def draw_map(ax, data: Data, array, cmap, vmin, vmax, title: str):
    masked = np.ma.masked_invalid(np.where(data.valid, array, np.nan))
    cm = plt.get_cmap(cmap).copy()
    cm.set_bad("white")
    im = ax.imshow(masked, cmap=cm, vmin=vmin, vmax=vmax, extent=data.extent_km(), origin="upper", interpolation="nearest")
    ax.set_title(title, fontsize=10)
    ax.set_xlabel("东向 (km)", fontsize=8)
    ax.set_ylabel("北向 (km)", fontsize=8)
    ax.tick_params(labelsize=8)
    return im


# 作用：有效像元（有限值）的 1 维数组。
def finite(array) -> np.ndarray:
    return array[np.isfinite(array)]


# 作用：图 1 输入图：地类（IGBP）、LAI、DEM。
def fig_inputs(data: Data, out_dir: Path) -> Path:
    # 步骤 1：读取输入栅格；LAI/DEM 的 -9999 为无效值
    lai = data.input_raster("lai.tif")
    lai = np.where(lai > -100, lai, np.nan)
    dem = data.input_raster("dem.tif")
    dem = np.where(dem > -100, dem, np.nan)
    fig, axes = plt.subplots(1, 3, figsize=(16, 5.6))
    # 步骤 2：地类用离散色标，右侧图例列出各类像元数
    codes = sorted(IGBP)
    cmap = ListedColormap([IGBP[c][1] for c in codes])
    norm = BoundaryNorm([c - 0.5 for c in codes] + [codes[-1] + 0.5], cmap.N)
    cat = np.where(np.isin(data.type, codes), data.type, np.nan)
    axes[0].imshow(np.ma.masked_invalid(cat), cmap=cmap, norm=norm, extent=data.extent_km(), interpolation="nearest")
    axes[0].set_title("(a) 地类（IGBP）", fontsize=10)
    handles = [plt.Rectangle((0, 0), 1, 1, color=IGBP[c][1]) for c in codes]
    labels = [f"{c} {IGBP[c][0]} ({int((data.type == c).sum())})" for c in codes]
    axes[0].legend(handles, labels, fontsize=7, loc="lower right", frameon=True, framealpha=0.9)
    # 步骤 3：LAI 与 DEM 连续色标
    im = draw_map(axes[1], data, lai, "YlGn", 0, 4, "(b) LAI (m² m$^{-2}$)")
    fig.colorbar(im, ax=axes[1], shrink=0.8, pad=0.02)
    im = draw_map(axes[2], data, dem, "terrain", 0, 1500, "(c) DEM 高程 (m)")
    fig.colorbar(im, ax=axes[2], shrink=0.8, pad=0.02)
    for ax in axes[:1]:
        ax.set_xlabel("东向 (km)", fontsize=8)
        ax.set_ylabel("北向 (km)", fontsize=8)
        ax.tick_params(labelsize=8)
    fig.tight_layout()
    return save_fig(fig, "fig01_inputs", out_dir)


# 作用：按地类计算数组均值（地类编码 → 均值）。
def class_means(data: Data, array) -> dict:
    out = {}
    for code in sorted(IGBP):
        values = finite(array[data.type == code])
        out[code] = (float(values.mean()) if values.size else float("nan"), int(values.size))
    return out


# 作用：把较长的地类名拆成两行，避免柱状图横轴标签重叠。
def wrap_name(name: str) -> str:
    return name if len(name) <= 3 else name[: len(name) // 2] + "\n" + name[len(name) // 2:]


# 作用：图 2 EB 概览：四组分温度地图（Tss/Tsh/Tcs/Tch）与按地类的均值柱状图。
def fig_eb(data: Data, out_dir: Path) -> Path:
    comp = data.components()
    keys = list(comp)
    titles = {"Tss": "Tss 光照土壤", "Tsh": "Tsh 阴影土壤", "Tcs": "Tcs 光照叶片", "Tch": "Tch 阴影叶片"}
    fig = plt.figure(figsize=(16, 8))
    grid = fig.add_gridspec(2, 3, width_ratios=[1, 1, 1.25])
    im = None
    for i, key in enumerate(keys):
        ax = fig.add_subplot(grid[i // 2, i % 2])
        short = key.split()[0]
        lo, hi = np.percentile(finite(np.where(data.valid, comp[key], np.nan)), [1, 99])
        im = draw_map(ax, data, comp[key], "inferno", lo, hi, f"({'abcd'[i]}) {titles.get(short, key)} (K)")
        fig.colorbar(im, ax=ax, shrink=0.75, pad=0.02)
    ax = fig.add_subplot(grid[:, 2])
    codes = [c for c in sorted(IGBP) if c != 17]  # 水体不参与 EB 求解
    width = 0.2
    colors = ["#e65100", "#8d6e63", "#2e7d32", "#9ccc65"]
    for i, key in enumerate(keys):
        means = class_means(data, comp[key])
        ax.bar(np.arange(len(codes)) + (i - 1.5) * width, [means[c][0] for c in codes], width, color=colors[i], label=titles.get(key.split()[0], key))
    ax.set_xticks(np.arange(len(codes)))
    ax.set_xticklabels([f"{wrap_name(IGBP[c][0])}\n(n={class_means(data, comp[keys[0]])[c][1]})" for c in codes], fontsize=8, rotation=0)
    ax.tick_params(axis="x", labelsize=7)
    ax.set_ylim(290, 322)
    ax.set_ylabel("地类均值温度 (K)")
    ax.set_title("(e) 各地类组分温度均值", fontsize=10)
    ax.legend(fontsize=8, ncol=2, loc="upper left")
    ax.grid(axis="y", alpha=0.3)
    return save_fig(fig, "fig02_eb_components", out_dir)


# 作用：（仅 run 源）图 2b：Rn/H/LE/G 通量地图与地类均值。
def fig_eb_fluxes(data: Data, out_dir: Path):
    fluxes = data.fluxes()
    if fluxes is None:
        return None
    names = {"Rn": "净辐射 Rn", "H": "感热 H", "LE": "潜热 LE", "G": "土壤热通量 G"}
    ranges = {"Rn": (450, 650), "H": (0, 450), "LE": (0, 450), "G": (0, 200)}
    fig = plt.figure(figsize=(16, 8))
    grid = fig.add_gridspec(2, 3, width_ratios=[1, 1, 1.25])
    for i, key in enumerate(fluxes):
        ax = fig.add_subplot(grid[i // 2, i % 2])
        valid = finite(np.where(data.valid, fluxes[key], np.nan))
        lo, hi = np.percentile(valid, [1, 99])
        im = draw_map(ax, data, fluxes[key], "viridis", lo, hi, f"({'abcd'[i]}) {names[key]} (W m$^{-2}$)")
        fig.colorbar(im, ax=ax, shrink=0.75, pad=0.02)
    ax = fig.add_subplot(grid[:, 2])
    codes = [c for c in sorted(IGBP) if c != 17]
    colors = ["#1565c0", "#e65100", "#2e7d32", "#8d6e63"]
    for i, key in enumerate(fluxes):
        means = class_means(data, fluxes[key])
        ax.bar(np.arange(len(codes)) + (i - 1.5) * 0.2, [means[c][0] for c in codes], 0.2, color=colors[i], label=names[key])
    ax.set_xticks(np.arange(len(codes)))
    ax.set_xticklabels([wrap_name(IGBP[c][0]) for c in codes], fontsize=7)
    ax.set_ylabel("地类均值 (W m$^{-2}$)")
    ax.set_title("(e) 各地类通量均值", fontsize=10)
    ax.legend(fontsize=8, ncol=2)
    ax.grid(axis="y", alpha=0.3)
    return save_fig(fig, "fig02b_eb_fluxes", out_dir)


# 作用：由传感器 id 取该传感器的红/近红外 NDVI 波段名（来自 sensors.json 的 ndvi 配对）。
def ndvi_pair(data: Data, sid: str):
    for s in data.sensors:
        if s["id"] == sid and "ndvi" in s:
            return s["ndvi"]["red"], s["ndvi"]["nir"]
    return None


# 作用：计算某传感器的逐像元 NDVI 数组。
def ndvi_array(data: Data, sid: str) -> np.ndarray:
    red, nir = ndvi_pair(data, sid)
    bands = data.optical(sid)
    r, n = bands[red][1], bands[nir][1]
    return (n - r) / (n + r)


# 作用：图 3 光学：各传感器同名波段（红光/近红外/短波红外 1）反射率分布箱线图（5–95% 须线，菱形为均值）。
def fig_optical_bands(data: Data, out_dir: Path) -> Path:
    fig, axes = plt.subplots(1, 3, figsize=(16, 5.2))
    for ax, (group, mapping) in zip(axes, BAND_GROUPS.items()):
        series, labels, colors = [], [], []
        for sid in OPTICAL_SENSORS:
            center, array = data.optical(sid)[mapping[sid]]
            series.append(finite(array))
            labels.append(f"{SENSORS[sid][0]}\n{mapping[sid]}\n{center:.0f} nm")
            colors.append(SENSORS[sid][1])
        box = ax.boxplot(series, whis=(5, 95), showfliers=False, patch_artist=True, widths=0.55,
                         medianprops={"color": "black"}, showmeans=True,
                         meanprops={"marker": "D", "markerfacecolor": "white", "markeredgecolor": "black", "markersize": 5})
        for patch, color in zip(box["boxes"], colors):
            patch.set_facecolor(color)
            patch.set_alpha(0.6)
        ax.set_xticks(range(1, len(labels) + 1))
        ax.set_xticklabels(labels, fontsize=8)
        ax.set_ylabel("反射率")
        ax.set_title(f"({'abc'[list(BAND_GROUPS).index(group)]}) {group}波段", fontsize=10)
        ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    return save_fig(fig, "fig03_optical_band_boxplots", out_dir)


# 作用：图 4 NDVI：五个传感器的 NDVI 地图与相对 Landsat 的散点（随机抽样 8000 个像元）。
def fig_ndvi(data: Data, out_dir: Path) -> Path:
    fig = plt.figure(figsize=(16, 10))
    grid = fig.add_gridspec(2, 3)
    arrays = {sid: ndvi_array(data, sid) for sid in OPTICAL_SENSORS}
    im = None
    for i, sid in enumerate(OPTICAL_SENSORS):
        ax = fig.add_subplot(grid[i // 3, i % 3])
        red, nir = ndvi_pair(data, sid)
        mean = float(np.nanmean(np.where(data.valid, arrays[sid], np.nan)))
        im = draw_map(ax, data, arrays[sid], "RdYlGn", 0.2, 0.95, f"({'abcde'[i]}) {SENSORS[sid][0]} NDVI({red},{nir})  均值 {mean:.3f}")
        fig.colorbar(im, ax=ax, shrink=0.75, pad=0.02)
    ax = fig.add_subplot(grid[1, 2])
    rng = np.random.default_rng(0)
    base = arrays["landsat89"]
    ok = data.valid & np.isfinite(base)
    index = np.flatnonzero(ok.ravel())
    pick = rng.choice(index, size=min(8000, index.size), replace=False)
    for sid in OPTICAL_SENSORS[1:]:
        ax.scatter(base.ravel()[pick], arrays[sid].ravel()[pick], s=2, alpha=0.35, color=SENSORS[sid][1], label=SENSORS[sid][0], rasterized=True)
    ax.plot([0.2, 1], [0.2, 1], "k--", lw=1, label="1:1")
    ax.set_xlim(0.2, 1)
    ax.set_ylim(0.2, 1)
    ax.set_xlabel("Landsat 8/9 NDVI(B4,B5)")
    ax.set_ylabel("其他传感器 NDVI")
    ax.set_title("(f) NDVI 散点（相对 Landsat）", fontsize=10)
    ax.legend(fontsize=8, markerscale=4, loc="lower right")
    ax.grid(alpha=0.3)
    return save_fig(fig, "fig04_ndvi", out_dir)


# 作用：图 5 热红外亮温地图：Landsat B10、MODIS 31、ASTER B10/B14、ECOSTRESS TIR1/TIR5，统一色标。
def fig_tir_maps(data: Data, out_dir: Path) -> Path:
    panels = [("landsat89", "B10"), ("modis", "B31"), ("aster", "B10"), ("aster", "B14"), ("ecostress", "TIR1"), ("ecostress", "TIR5")]
    fig, axes = plt.subplots(2, 3, figsize=(16, 10.5))
    im = None
    for i, (ax, (sid, band)) in enumerate(zip(axes.ravel(), panels)):
        center, array = data.thermal(sid)[band]
        valid = finite(np.where(data.valid, array, np.nan))
        im = draw_map(ax, data, array, "inferno", 298, 317,
                      f"({'abcdef'[i]}) {SENSORS[sid][0]} {band} ({center:.2f} μm)\n均值 {valid.mean():.3f} K，标准差 {valid.std():.2f} K")
    fig.colorbar(im, ax=axes, shrink=0.7, pad=0.01, label="亮温 (K)")
    return save_fig(fig, "fig05_tir_brightness_maps", out_dir)


# 作用：读取（或在 run 源下计算并写出）亮温 - 发射率归一化地表温度的逐波段统计表。
def bt_minus_lst_table(data: Data) -> list[dict]:
    path = TABLE_DIR / "tir_bt_minus_lst.csv"
    if data.source == "run":
        rows = []
        for sid in THERMAL_SENSORS:
            lst = data.lst(sid)
            if lst is None:
                continue
            for band, (center, bt) in data.thermal(sid).items():
                diff = finite(np.where(data.valid, bt - lst[band][1], np.nan))
                rows.append({"sensor": sid, "band": band, "center_um": center,
                             "bt_mean": float(finite(np.where(data.valid, bt, np.nan)).mean()),
                             "lst_mean": float(finite(np.where(data.valid, lst[band][1], np.nan)).mean()),
                             "diff_mean": float(diff.mean()), "diff_std": float(diff.std()),
                             "diff_min": float(diff.min()), "diff_max": float(diff.max())})
        TABLE_DIR.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader()
            for row in rows:
                writer.writerow({k: (f"{v:.5f}" if isinstance(v, float) else v) for k, v in row.items()})
        return rows
    if not path.exists():
        return []
    with path.open(encoding="utf-8") as f:
        return [{k: (float(v) if k not in ("sensor", "band") else v) for k, v in row.items()} for row in csv.DictReader(f)]


# 作用：按地类与逐像元统计 ASTER B10−B14 的双峰、各地类的组分温差与角度亮温变化，供报告直接引用（run 源计算并写出
# tables/class_analysis.json，reference 源只读该文件，因为 reference/ 不含角度实验栅格）。
ANALYSIS_CLASSES = (1, 4, 10, 11, 12, 13)  # 像元数 ≥100 的非水体地类
ANGLE_RUNS = {"hot32": "angles_tirs_b10_vza32_back", "b55": "angles_tirs_b10_vza55_back", "f55": "angles_tirs_b10_vza55_fwd"}


def class_analysis(data: Data) -> dict:
    path = TABLE_DIR / "class_analysis.json"
    if data.source != "run":
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    comp = data.components()
    aster = data.thermal("aster")
    diff = aster["B10"][1] - aster["B14"][1]
    ok = data.valid & np.isfinite(diff)
    nadir = raster_tools.read_bands(str(data.run_dir / "angles_tirs_b10_vza00" / "tir_brightness_temperature.tif"))[2][0]
    delta = {k: raster_tools.read_bands(str(data.run_dir / run / "tir_brightness_temperature.tif"))[2][0] - nadir for k, run in ANGLE_RUNS.items()}
    # B10−B14 直方图的两个峰（> 最高峰 15% 的局部极大），及以两峰间谷值为界时各地类落在高值一侧的比例
    hist, edges = np.histogram(diff[ok], bins=80)
    peaks = [i for i in range(1, 79) if hist[i] > hist[i - 1] and hist[i] >= hist[i + 1] and hist[i] > 0.15 * hist.max()]
    centers = [float((edges[i] + edges[i + 1]) / 2) for i in peaks]
    valley = float(centers[0] + (centers[-1] - centers[0]) / 2) if len(centers) >= 2 else float(np.median(diff[ok]))
    classes = []
    for code in ANALYSIS_CLASSES:
        m = ok & (data.type == code)
        classes.append({"code": code, "name": IGBP[code][0], "n": int(m.sum()), "b10_b14_mean": float(np.nanmean(diff[m])),
                        "frac_high_mode": float((diff[m] >= valley).mean()),
                        "tss_tsh": float(np.nanmean(comp["Tss"][m] - comp["Tsh"][m])), "tcs_tch": float(np.nanmean(comp["Tcs"][m] - comp["Tch"][m])),
                        **{f"d_{k}": float(np.nanmean(v[m])) for k, v in delta.items()}})
    land = ok & (data.type != 17)
    corr = {}
    for k, v in delta.items():
        for name, x in (("tss_tsh", comp["Tss"] - comp["Tsh"]), ("tcs_tch", comp["Tcs"] - comp["Tch"])):
            m = land & np.isfinite(x) & np.isfinite(v)
            corr[f"{k}:{name}"] = float(np.corrcoef(v[m], x[m])[0, 1])
    result = {"aster_b10_b14": {"peaks": centers, "valley": valley, "n_valid": int(ok.sum())}, "classes": classes, "corr": corr}
    # 各地类光照土壤温度均值（Tss），以及天底亮温最大值所在像元的地类
    for entry in result["classes"]:
        entry["tss"] = float(np.nanmean(comp["Tss"][ok & (data.type == entry["code"])]))
    peak = np.nanargmax(np.where(data.valid, nadir, np.nan))
    result["nadir_max"] = {"value": float(nadir.flat[peak]), "code": int(data.type.flat[peak]), "name": IGBP.get(int(data.type.flat[peak]), ("其他",))[0]}
    # 五个传感器逐像元 NDVI 的极差（最大 − 最小），按 Landsat NDVI 分段取均值
    ndvis = np.stack([ndvi_array(data, sid) for sid in OPTICAL_SENSORS])
    spread, base = ndvis.max(axis=0) - ndvis.min(axis=0), ndvis[0]
    bins = []
    for lo, hi in ((-1.0, 0.4), (0.4, 0.6), (0.6, 0.8), (0.8, 1.01)):
        m = data.valid & np.isfinite(spread) & (base >= lo) & (base < hi)
        bins.append({"lo": lo, "hi": min(hi, 1.0), "n": int(m.sum()), "spread_mean": float(spread[m].mean())})
    result["ndvi_bins"] = bins
    TABLE_DIR.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


# 作用：图 6 热红外光谱：(a) 亮温随中心波长；(b) 亮温 - 发射率归一化 LST 随波长；(c) ASTER B10-B14 的逐像元差异直方图。
def fig_tir_spectral(data: Data, out_dir: Path) -> Path:
    fig, axes = plt.subplots(1, 3, figsize=(16, 5.2))
    ax = axes[0]
    for sid in THERMAL_SENSORS:
        for band, (center, array) in data.thermal(sid).items():
            mean = float(finite(np.where(data.valid, array, np.nan)).mean())
            fwhm = data.thermal_fwhm(sid, band)
            ax.errorbar(center, mean, xerr=(fwhm / 2 if fwhm else None), fmt="o", color=SENSORS[sid][1], ms=5, capsize=2, lw=1,
                        label=SENSORS[sid][0] if band == list(data.thermal(sid))[0] else None)
    ax.set_xlabel("中心波长 (μm)，横线为半宽 FWHM/2")
    ax.set_ylabel("300 m 场景均值亮温 (K)")
    ax.set_title("(a) 亮温随中心波长", fontsize=10)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    ax = axes[1]
    rows = bt_minus_lst_table(data)
    for sid in THERMAL_SENSORS:
        points = sorted((r["center_um"], r["diff_mean"]) for r in rows if r["sensor"] == sid)
        if points:
            ax.plot([p[0] for p in points], [p[1] for p in points], "o-", ms=5, lw=1, color=SENSORS[sid][1], label=SENSORS[sid][0])
    ax.set_xlabel("中心波长 (μm)")
    ax.set_ylabel("场景均值：亮温 - 归一化 LST (K)")
    ax.set_title("(b) 亮温与发射率归一化地表温度之差", fontsize=10)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    ax = axes[2]
    aster = data.thermal("aster")
    diff = finite(np.where(data.valid, aster["B10"][1] - aster["B14"][1], np.nan))
    ax.hist(diff, bins=80, color="#8c564b", alpha=0.8)
    ax.axvline(diff.mean(), color="k", ls="--", lw=1, label=f"均值 {diff.mean():.3f} K")
    ax.set_xlabel("ASTER B10 (8.3 μm) - B14 (11.3 μm) 亮温差 (K)")
    ax.set_ylabel("像元数")
    ax.set_title("(c) 同一传感器两个窗区波段的逐像元亮温差", fontsize=10)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    return save_fig(fig, "fig06_tir_spectral", out_dir)


# 作用：读取 results/ 下的 csv 为字典列表。
def read_csv(name: str) -> list[dict]:
    with (CASE / "results" / name).open(encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


# 作用：太阳天顶角（场景均值，取自母工程 scene_summary / parent_scene_summary.json）。
def solar_zenith() -> float:
    summary = json.loads((CASE / "reference" / "parent_scene_summary.json").read_text(encoding="utf-8"))
    return float(summary["solarGeometry"][0]["mean"])


# 作用：图 7 角度实验：B10 与 MODIS 31 的主平面亮温变化。横轴前向为负、后向为正；热点点用星号标出，虚线为太阳天顶角。
def fig_angles(data: Data, out_dir: Path) -> Path:
    rows = read_csv("angles.csv")
    sza = solar_zenith()
    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    series = {"tirs_b10": ("Landsat TIRS B10", "#1f77b4", "o"), "modis_b31": ("MODIS 31", "#d62728", "s")}
    for key, (label, color, marker) in series.items():
        pts = []
        for r in rows:
            if r["experiment"] != key:
                continue
            vza, vaa = float(r["vza"]), float(r["vaa"])
            signed = 0.0 if vza == 0 else (vza if abs(vaa - 139) < 1 else -vza)
            pts.append((signed, float(r["mean"]), float(r["delta_mean_vs_nadir"]), float(r["std"]), float(r["p95"]), float(r["p05"])))
        pts.sort()
        x = [p[0] for p in pts]
        for ax, col, ylabel in zip(axes, (2, 3, 4), ("亮温相对天底的变化 (K)", "场景亮温标准差 (K)", "场景亮温 P95 (K)")):
            ax.plot(x, [p[col] for p in pts], marker + "-", color=color, ms=6, lw=1.2, label=label, markerfacecolor="white" if key == "modis_b31" else color)
            ax.set_ylabel(ylabel)
        hot = [p for p in pts if abs(p[0] - round(sza)) < 1]
        for p in hot:
            axes[0].plot(p[0], p[2], marker="*", ms=16, color="gold", markeredgecolor="k", zorder=5)
    axes[0].plot([], [], marker="*", ls="", ms=12, color="gold", markeredgecolor="k", label=f"热点方向 VZA≈{sza:.1f}°")
    for ax in axes:
        ax.axvline(sza, color="gray", ls="--", lw=1)
        ax.axvline(0, color="gray", ls=":", lw=1)
        ax.set_xlabel("观测天顶角 VZA (°)，后向（太阳一侧）为正，前向为负")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    axes[0].axhline(0, color="k", lw=0.8)
    axes[0].set_title("(a) 亮温变化", fontsize=10)
    axes[1].set_title("(b) 标准差", fontsize=10)
    axes[2].set_title("(c) P95（场景最大值不随角度变化，见表）", fontsize=10)
    fig.tight_layout()
    return save_fig(fig, "fig07_angle_principal_plane", out_dir)


# 作用：图 8 聚合实验：Landsat B10 的 300 m 与 1 km（均值聚合）地图、分布直方图，以及各产品热点占比对比。
def fig_aggregation(data: Data, out_dir: Path) -> Path:
    t300 = data.thermal("landsat89")["B10"][1]
    t300 = np.where(data.valid, t300, np.nan).astype(np.float32)
    t1k = raster_tools.aggregate_grid(t300, data.geo, data.proj, 1000.0, "average")
    fig = plt.figure(figsize=(16, 9.5))
    grid = fig.add_gridspec(2, 6)
    ax1 = fig.add_subplot(grid[0, 0:2])
    im = ax1.imshow(np.ma.masked_invalid(t300), cmap="inferno", vmin=298, vmax=317, extent=data.extent_km(), interpolation="nearest")
    ax1.set_title(f"(a) 300 m  B10 亮温 (K)\n均值 {np.nanmean(t300):.3f}，标准差 {np.nanstd(t300):.2f}，最大 {np.nanmax(t300):.2f}", fontsize=10)
    ax2 = fig.add_subplot(grid[0, 2:4])
    top_km = data.height * abs(data.geo[5]) / 1000.0  # 1 km 网格与 300 m 网格共用西北角，向下取整格数
    ax2.imshow(np.ma.masked_invalid(t1k), cmap="inferno", vmin=298, vmax=317, interpolation="nearest",
               extent=[0, t1k.shape[1], top_km - t1k.shape[0], top_km])
    ax2.set_title(f"(b) 1 km 均值聚合 B10 亮温 (K)\n均值 {np.nanmean(t1k):.3f}，标准差 {np.nanstd(t1k):.2f}，最大 {np.nanmax(t1k):.2f}", fontsize=10)
    for ax in (ax1, ax2):
        ax.set_xlabel("东向 (km)", fontsize=8)
        ax.set_ylabel("北向 (km)", fontsize=8)
        ax.tick_params(labelsize=8)
    fig.colorbar(im, ax=ax1, shrink=0.8, pad=0.02, label="亮温 (K)")
    fig.colorbar(im, ax=ax2, shrink=0.8, pad=0.02, label="亮温 (K)")
    # 分布直方图（密度）与 P95 阈值
    ax3 = fig.add_subplot(grid[0, 4:6])
    v300, v1k = finite(t300), finite(t1k)
    bins = np.linspace(298, 317, 80)
    ax3.hist(v300, bins=bins, density=True, alpha=0.55, color="#1f77b4", label=f"300 m（{v300.size} 像元）")
    ax3.hist(v1k, bins=bins, density=True, alpha=0.55, color="#d62728", label=f"1 km 均值（{v1k.size} 格）")
    p95 = np.percentile(v300, 95)
    ax3.axvline(p95, color="k", ls="--", lw=1, label=f"300 m P95 = {p95:.2f} K")
    ax3.set_xlabel("亮温 (K)")
    ax3.set_ylabel("概率密度")
    ax3.set_title("(c) 分布对比", fontsize=10)
    ax3.legend(fontsize=8)
    ax3.grid(alpha=0.3)
    # 热点占比：各产品在 300 m / 1 km 均值 / 1 km 取最大下，≥300 m P95 的面积占比
    rows = read_csv("aggregation.csv")
    pick = [("landsat89", "B10"), ("landsat89", "B4"), ("landsat89", "B5"), ("landsat89", "NDVI(B4,B5)")]
    labels = ["Landsat B10", "Landsat B4", "Landsat B5", "Landsat NDVI"]
    def frac(sid, band, col):
        for r in rows:
            if r["sensor"] == sid and r["band"] == band:
                return float(r[col])
        return float("nan")
    ax4 = fig.add_subplot(grid[1, 0:3])
    x = np.arange(len(pick))
    for k, (col, name, color) in enumerate([("hot_frac_300m_p95", "300 m", "#1f77b4"), ("hot_frac_1km_avg_p95", "1 km 均值聚合", "#d62728"), ("hot_frac_1km_maxagg_p95", "1 km 取最大值", "#ff7f0e")]):
        values = [frac(s, b, col) for s, b in pick]
        bars = ax4.bar(x + (k - 1) * 0.26, values, 0.26, color=color, label=name)
        for bar, v in zip(bars, values):
            ax4.text(bar.get_x() + bar.get_width() / 2, v + 0.004, f"{v:.3f}", ha="center", fontsize=7)
    ax4.set_xticks(x)
    ax4.set_xticklabels(labels)
    ax4.set_ylabel("热点（≥300 m P95）面积占比")
    ax4.set_title("(d) 热点占比对比", fontsize=10)
    ax4.legend(fontsize=8)
    ax4.grid(axis="y", alpha=0.3)
    # 统计量相对变化：标准差、最大值（1 km 均值聚合相对 300 m）
    ax5 = fig.add_subplot(grid[1, 3:6])
    rel_pick = [("landsat89", "B10", "B10"), ("landsat89", "B2", "B2"), ("landsat89", "B4", "B4"), ("landsat89", "B5", "B5"), ("landsat89", "B6", "B6"), ("landsat89", "NDVI(B4,B5)", "NDVI")]
    x = np.arange(len(rel_pick))
    for k, (a, b, name, color) in enumerate([("std_300m", "std_1km_avg", "标准差", "#2ca02c"), ("max_300m", "max_1km_avg", "最大值", "#9467bd")]):
        values = [100.0 * (frac(s, band, b) / frac(s, band, a) - 1.0) for s, band, _ in rel_pick]
        ax5.bar(x + (k - 0.5) * 0.35, values, 0.35, color=color, label=name)
        for xi, v in zip(x + (k - 0.5) * 0.35, values):
            ax5.text(xi, v / 2, f"{v:.1f}%", ha="center", va="center", fontsize=7, color="white" if abs(v) > 3 else "black")
    ax5.set_xticks(x)
    ax5.set_xticklabels([f"Landsat {r[2]}" for r in rel_pick], fontsize=8)
    ax5.set_ylabel("1 km 均值聚合相对 300 m 的变化 (%)")
    ax5.set_title("(e) 聚合后统计量的相对变化", fontsize=10)
    ax5.axhline(0, color="k", lw=0.8)
    ax5.legend(fontsize=8)
    ax5.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    return save_fig(fig, "fig08_aggregation_1km", out_dir)


# 作用：依次生成全部配图并返回 {图名: 路径}。
def generate_figures(source: str = "reference", run_dir: Path = DEFAULT_RUN, out_dir: Path = FIG_DIR) -> dict:
    setup_fonts()
    if source == "run" and not run_dir.exists():
        raise SystemExit(f"run 源目录不存在：{run_dir}")
    data = Data(source, run_dir)
    print(f"数据源：{source}；输出：{out_dir}")
    figures = {}
    for key, func in (("inputs", fig_inputs), ("eb", fig_eb), ("eb_fluxes", fig_eb_fluxes), ("optical", fig_optical_bands),
                      ("ndvi", fig_ndvi), ("tir_maps", fig_tir_maps), ("tir_spectral", fig_tir_spectral),
                      ("angles", fig_angles), ("aggregation", fig_aggregation)):
        path = func(data, out_dir)
        if path:
            figures[key] = path
    class_analysis(data)
    return figures


def main() -> None:
    parser = argparse.ArgumentParser(description="生成北京多传感器案例配图")
    parser.add_argument("--source", choices=("reference", "run"), default="reference")
    parser.add_argument("--run-dir", type=Path, default=DEFAULT_RUN)
    parser.add_argument("--out", type=Path, default=FIG_DIR)
    args = parser.parse_args()
    generate_figures(args.source, args.run_dir, args.out)


if __name__ == "__main__":
    main()
