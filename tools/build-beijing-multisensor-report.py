# ==========================================================================
# 文件：build-beijing-multisensor-report.py
# 功能：生成《北京市 · 光学与热红外多传感器观测模拟》分析报告 PDF
#       （docs/cases/beijing-multisensor/report.pdf），版式沿用 build-core-manuals-pdf.py
#       （A4、等线 Deng 字体、同一套配色与页眉页脚）。
# 步骤：
#   1. 读数据：results/summary.csv、angles.csv、aggregation.csv（案例 results/ 目录），
#      sensors.json（传感器、波段表与数据依据 basis），reference/parent_scene_summary.json（母工程摘要），
#      tables/tir_bt_minus_lst.csv（亮温-LST 差）、tables/class_analysis.json（分地类统计）。
#      报告里所有数值表与正文数字都由这些文件计算得出，不手抄。
#   2. 出图：调用 tools/plot-beijing-multisensor.py 的 generate_figures（--source reference|run，缺省 reference，
#      即只用仓库内 reference/ 精简结果重建；--skip-figures 则直接使用 figures/ 里已有 PNG）。
#   3. 排版：封面标题块 + “本案例做到了什么” → 1 目的与场景 → 2 传感器与波段表（含数据依据）→ 3 方法
#      → 4 结果与分析（EB、实验 A/B/C，每节先给结论）→ 5 适用范围与后续工作 → 6 可复现步骤 → 附录出处，
#      经 reportlab 输出 PDF。
# 数据流：assets/examples/beijing_multisensor/{results,reference,sensors.json} + tables/*
#         → figures/*.png → report.pdf。
# 运行：C:\work\miniconda\python.exe -I -X utf8 tools\build-beijing-multisensor-report.py [--source reference|run] [--skip-figures]
# 依赖：reportlab、Pillow、numpy、matplotlib、osgeo（出图时用）；字体 C:\Windows\Fonts\Deng.ttf / Dengb.ttf。
# 说明：各传感器结果是无大气的冠层顶方向量，波段为高斯近似，尚未与实测数据对比（见报告第 3、5 章）。
# ==========================================================================

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import sys
from pathlib import Path

from PIL import Image as PILImage
from reportlab.lib import colors
from reportlab.lib.enums import TA_JUSTIFY
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (BaseDocTemplate, Frame, Image, KeepTogether, PageBreak, PageTemplate,
                                Paragraph, Spacer, Table, TableStyle)

ROOT = Path(__file__).resolve().parents[1]
CASE = ROOT / "assets" / "examples" / "beijing_multisensor"
REPORT_DIR = ROOT / "docs" / "cases" / "beijing-multisensor"
FIG_DIR = REPORT_DIR / "figures"
TABLE_DIR = REPORT_DIR / "tables"
OUTPUT = REPORT_DIR / "report.pdf"

# 模块：字体与配色（与 build-core-manuals-pdf.py 一致）。
pdfmetrics.registerFont(TTFont("StreamCN", r"C:\Windows\Fonts\Deng.ttf"))
pdfmetrics.registerFont(TTFont("StreamCN-Bold", r"C:\Windows\Fonts\Dengb.ttf"))
pdfmetrics.registerFontFamily("StreamCN", normal="StreamCN", bold="StreamCN-Bold", italic="StreamCN", boldItalic="StreamCN-Bold")
PAGE_W, PAGE_H = A4
NAVY = colors.HexColor("#10273D")
BLUE = colors.HexColor("#2674D9")
TEXT = colors.HexColor("#23384B")
MUTED = colors.HexColor("#687D90")
LINE = colors.HexColor("#D9E4EC")
PALE = colors.HexColor("#F2F7FA")
PALE_BLUE = colors.HexColor("#EAF2FD")
TITLE = "北京市 · 光学与热红外多传感器观测模拟：分析报告"
ANGLE_BACK, ANGLE_FWD = 139.0, 319.0


# ----------------------------------------------------------------------------
# 步骤 1：读数据
# ----------------------------------------------------------------------------

# 作用：读取 CSV 为字典列表（兼容 BOM），数值列转 float，其余保持字符串。
def read_csv(path: Path) -> list[dict]:
    with path.open(encoding="utf-8-sig", newline="") as f:
        rows = []
        for raw in csv.DictReader(f):
            row = {}
            for key, value in raw.items():
                try:
                    row[key] = float(value) if value not in ("", None) else None
                except ValueError:
                    row[key] = value
            rows.append(row)
        return rows


# 作用：集中加载报告用到的全部数据文件，返回一个字典。
def load_data() -> dict:
    data = {
        "summary": read_csv(CASE / "results" / "summary.csv"),
        "angles": read_csv(CASE / "results" / "angles.csv"),
        "aggregation": read_csv(CASE / "results" / "aggregation.csv"),
        "sensors": json.loads((CASE / "sensors.json").read_text(encoding="utf-8")),
        "parent": json.loads((CASE / "reference" / "parent_scene_summary.json").read_text(encoding="utf-8")),
        "lst": read_csv(TABLE_DIR / "tir_bt_minus_lst.csv"),
        "analysis": json.loads((TABLE_DIR / "class_analysis.json").read_text(encoding="utf-8")),
    }
    return data


# 作用：在 summary 行里按传感器/波段取一行（找不到抛错，避免默默出错）。
def srow(data: dict, sensor: str, band: str) -> dict:
    for r in data["summary"]:
        if r["sensor"] == sensor and r["band"] == band:
            return r
    raise KeyError(f"summary.csv 中没有 {sensor}/{band}")


# 作用：取聚合 csv 的一行。
def arow(data: dict, sensor: str, band: str) -> dict:
    for r in data["aggregation"]:
        if r["sensor"] == sensor and r["band"] == band:
            return r
    raise KeyError(f"aggregation.csv 中没有 {sensor}/{band}")


# 作用：取角度实验一行（实验名、VZA、VAA）。
def grow(data: dict, experiment: str, vza: float, vaa: float | None):
    for r in data["angles"]:
        if r["experiment"] == experiment and abs(r["vza"] - vza) < 1e-6 and (vaa is None or abs(r["vaa"] - vaa) < 1e-6):
            return r
    return None


# 作用：数值格式化：保留 n 位小数；带符号用 sgn=True（负号用 ASCII，避免字体缺字）。
def f(value, n: int = 3, sgn: bool = False) -> str:
    if value is None:
        return "—"
    return f"{value:+.{n}f}" if sgn else f"{value:.{n}f}"


# ----------------------------------------------------------------------------
# 步骤 2：样式与排版部件
# ----------------------------------------------------------------------------

# 作用：集中定义段落样式。
def make_styles() -> dict:
    sample = getSampleStyleSheet()
    return {
        "body": ParagraphStyle("BodyCN", parent=sample["BodyText"], fontName="StreamCN", fontSize=9.4, leading=15.2, textColor=TEXT,
                               alignment=TA_JUSTIFY, wordWrap="CJK", spaceAfter=2.4 * mm),
        "title": ParagraphStyle("TitleCN", fontName="StreamCN-Bold", fontSize=19, leading=27, textColor=NAVY, spaceAfter=3 * mm, wordWrap="CJK"),
        "subtitle": ParagraphStyle("SubtitleCN", fontName="StreamCN", fontSize=10.5, leading=16, textColor=MUTED, spaceAfter=4 * mm, wordWrap="CJK"),
        "h1": ParagraphStyle("H1CN", fontName="StreamCN-Bold", fontSize=16, leading=22, textColor=NAVY, spaceBefore=3 * mm,
                             spaceAfter=3 * mm, keepWithNext=True, wordWrap="CJK"),
        "h2": ParagraphStyle("H2CN", fontName="StreamCN-Bold", fontSize=12, leading=17, textColor=BLUE, spaceBefore=3 * mm,
                             spaceAfter=2 * mm, keepWithNext=True, wordWrap="CJK"),
        "bullet": ParagraphStyle("BulletCN", fontName="StreamCN", fontSize=9.2, leading=14.5, textColor=TEXT, leftIndent=5.5 * mm,
                                 firstLineIndent=-3.4 * mm, bulletIndent=1.1 * mm, wordWrap="CJK", spaceAfter=1.1 * mm),
        "code": ParagraphStyle("CodeCN", fontName="StreamCN", fontSize=8, leading=12, textColor=colors.HexColor("#173B50"),
                               leftIndent=2 * mm, rightIndent=2 * mm, wordWrap="CJK"),
        "caption": ParagraphStyle("CaptionCN", fontName="StreamCN", fontSize=8.4, leading=12.4, textColor=MUTED, alignment=1,
                                  wordWrap="CJK", spaceBefore=1 * mm, spaceAfter=3.5 * mm),
        "table": ParagraphStyle("TableCN", fontName="StreamCN", fontSize=7.6, leading=10.4, textColor=TEXT, wordWrap="CJK"),
        "table_r": ParagraphStyle("TableRCN", fontName="StreamCN", fontSize=7.6, leading=10.4, textColor=TEXT, alignment=2),
        "table_head_r": ParagraphStyle("TableHeadRCN", fontName="StreamCN-Bold", fontSize=7.7, leading=10.4, textColor=colors.white, alignment=2),
        "table_head": ParagraphStyle("TableHeadCN", fontName="StreamCN-Bold", fontSize=7.7, leading=10.4, textColor=colors.white, wordWrap="CJK"),
        "box": ParagraphStyle("BoxCN", fontName="StreamCN", fontSize=9.2, leading=14.5, textColor=TEXT, leftIndent=4 * mm,
                              firstLineIndent=-3.2 * mm, wordWrap="CJK", spaceAfter=1.2 * mm),
    }


STY = make_styles()
FRAME_W = PAGE_W - 36 * mm


# 作用：普通段落、列表项、代码块。
def P(text: str, style: str = "body") -> Paragraph:
    return Paragraph(text, STY[style])


def B(text: str) -> Paragraph:
    return Paragraph(text, STY["bullet"], bulletText="•")


def CODE(lines: list[str]) -> Table:
    table = Table([[Paragraph("<br/>".join(lines), STY["code"])]], colWidths=[FRAME_W])
    table.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, -1), PALE), ("BOX", (0, 0), (-1, -1), 0.5, LINE),
                               ("TOPPADDING", (0, 0), (-1, -1), 4), ("BOTTOMPADDING", (0, 0), (-1, -1), 4)]))
    return table


# 作用：编号计数器，图题/表题自动编号。
class Counter:
    figure = 0
    table = 0


# 图编号表：由 main 按实际存在的图填入 {文件名前缀: 序号}，正文用 fnum() 引用，避免手写编号随图数量漂移。
FN: dict = {}


def fnum(prefix: str) -> int:
    return FN[prefix]


# 作用：插入一张图（宽度不超过版心，高度不超过 max_h）和图题。
def FIG(path: Path, caption: str, max_h: float = 205 * mm) -> list:
    Counter.figure += 1
    with PILImage.open(path) as im:
        w, h = im.size
    width = FRAME_W
    height = width * h / w
    if height > max_h:
        height = max_h
        width = height * w / h
    return [KeepTogether([Image(str(path), width=width, height=height), P(f"图 {Counter.figure}　{caption}", "caption")])]


# 作用：生成带表题的数据表。rows 的首行为表头；align_right 为右对齐的列号集合。
# 单元格是字符串时自动包成 Paragraph（支持 <font color=...> 标记）。
def TBL(caption: str, rows: list[list], col_widths: list[float], align_right: set | None = None, repeat: int = 1) -> list:
    Counter.table += 1
    align_right = align_right or set()
    wrapped = []
    for r_index, row in enumerate(rows):
        cells = []
        for c_index, cell in enumerate(row):
            if r_index == 0:
                cells.append(Paragraph(str(cell), STY["table_head_r" if c_index in align_right else "table_head"]))
            else:
                cells.append(Paragraph(str(cell), STY["table_r" if c_index in align_right else "table"]))
        wrapped.append(cells)
    table = Table(wrapped, colWidths=col_widths, repeatRows=repeat)
    table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), NAVY), ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, PALE]), ("LINEBELOW", (0, 0), (-1, -1), 0.3, LINE),
        ("TOPPADDING", (0, 0), (-1, -1), 2.2), ("BOTTOMPADDING", (0, 0), (-1, -1), 2.2),
        ("LEFTPADDING", (0, 0), (-1, -1), 3), ("RIGHTPADDING", (0, 0), (-1, -1), 3)]))
    block = [P(f"表 {Counter.table}　{caption}", "caption"), table]
    if len(rows) <= 16:
        return [KeepTogether(block), Spacer(1, 3 * mm)]
    return block + [Spacer(1, 3 * mm)]


# 作用：文档模板（A4，页眉标题，页脚页码；第 1 页不画页眉）。
class ReportDoc(BaseDocTemplate):
    def __init__(self, filename: str):
        super().__init__(filename, pagesize=A4, leftMargin=18 * mm, rightMargin=18 * mm, topMargin=18 * mm, bottomMargin=17 * mm,
                         title=TITLE, author="Stream Studio", subject="STREAM 多传感器观测模拟案例报告")
        frame = Frame(self.leftMargin, self.bottomMargin, self.width, self.height, leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
        self.addPageTemplates(PageTemplate("content", [frame], onPage=self.draw_page))

    def draw_page(self, canvas, doc):
        canvas.saveState()
        canvas.setStrokeColor(LINE)
        canvas.setLineWidth(0.5)
        if doc.page > 1:
            canvas.line(18 * mm, PAGE_H - 12 * mm, PAGE_W - 18 * mm, PAGE_H - 12 * mm)
            canvas.setFont("StreamCN", 8)
            canvas.setFillColor(MUTED)
            canvas.drawString(18 * mm, PAGE_H - 9 * mm, TITLE)
        canvas.setFont("StreamCN", 8)
        canvas.setFillColor(MUTED)
        canvas.drawRightString(PAGE_W - 18 * mm, 9 * mm, f"{doc.page}")
        canvas.restoreState()


# ----------------------------------------------------------------------------
# 步骤 3：各章内容
# ----------------------------------------------------------------------------

SENSOR_NAMES = {"landsat89": "Landsat 8/9", "modis": "MODIS (Terra)", "sentinel2": "Sentinel-2A MSI", "aster": "ASTER",
                "ecostress": "ECOSTRESS", "viirs": "VIIRS (S-NPP/NOAA-20)", "fy3d_mersi2": "FY-3D MERSI-II", "parent": "母工程 (Landsat 波段)"}
BASIS_LABEL = {"official-spec": "官方规格", "official-srf": "官方光谱响应", "literature": "文献实测值", "nominal": "标称值"}
SHORT = {"landsat89": "Landsat", "modis": "MODIS", "sentinel2": "Sentinel-2", "viirs": "VIIRS", "fy3d_mersi2": "MERSI-II",
         "aster": "ASTER", "ecostress": "ECOSTRESS"}
OPTICAL_ORDER = ["landsat89", "modis", "sentinel2", "viirs", "fy3d_mersi2"]
THERMAL_ORDER = ["landsat89", "modis", "viirs", "fy3d_mersi2", "aster", "ecostress"]


# 作用：计算全报告用到的“派生数字”（均值差、最大最小等），统一放一个字典，正文只引用这里的值。
def derive(data: dict) -> dict:
    d: dict = {}
    d["ndvi"] = {sid: srow(data, sid, next(r["band"] for r in data["summary"] if r["sensor"] == sid and r["kind"] == "ndvi")) for sid in OPTICAL_ORDER}
    d["ndvi_parent"] = next(r for r in data["summary"] if r["sensor"] == "parent" and r["kind"] == "ndvi")
    values = [r["mean"] for r in d["ndvi"].values()]
    d["ndvi_min"], d["ndvi_max"] = min(values), max(values)
    # 母工程 (EB 路径) 与 landsat89 变体 (RT 路径) 的光学差（按波段名对齐 B1–B7）
    diffs = []
    for r in data["summary"]:
        if r["sensor"] == "parent" and r["kind"] == "optical":
            v = srow(data, "landsat89", r["band"])
            diffs.append((r["band"], r["mean"], v["mean"], r["center"], v["center"]))
    d["parent_vs_variant"] = diffs
    d["parent_opt_maxdiff"] = max(abs(a - b) for _, a, b, _, _ in diffs)
    d["parent_opt_maxband"] = max(diffs, key=lambda x: abs(x[1] - x[2]))[0]
    d["parent_ndvi_diff"] = d["ndvi_parent"]["mean"] - d["ndvi"]["landsat89"]["mean"]
    d["parent_b10"] = srow(data, "parent", "B10")["mean"]
    d["variant_b10"] = srow(data, "landsat89", "B10")["mean"]
    d["parent_b10_diff"] = d["parent_b10"] - d["variant_b10"]
    # 热红外：同波段不同传感器差、8.3 与 12 μm 差
    therm = [r for r in data["summary"] if r["kind"] == "thermal" and r["sensor"] != "parent"]
    d["thermal_rows"] = therm
    group_a = [srow(data, s, b)["mean"] for s, b in (("landsat89", "B10"), ("modis", "B31"), ("viirs", "M15"), ("fy3d_mersi2", "CH24"))]
    group_b = [srow(data, s, b)["mean"] for s, b in (("landsat89", "B11"), ("modis", "B32"), ("viirs", "M16"), ("fy3d_mersi2", "CH25"), ("ecostress", "TIR5"))]
    d["group_a_spread"], d["group_b_spread"] = max(group_a) - min(group_a), max(group_b) - min(group_b)
    d["aster_b10"], d["aster_b14"] = srow(data, "aster", "B10")["mean"], srow(data, "aster", "B14")["mean"]
    d["eco_tir1"], d["eco_tir5"] = srow(data, "ecostress", "TIR1")["mean"], srow(data, "ecostress", "TIR5")["mean"]
    d["tir_max"], d["tir_min"] = max(r["mean"] for r in therm), min(r["mean"] for r in therm)
    # 角度实验：太阳几何、各点相对天底的变化、最大值是否随角度不变
    sol = {x["name"]: x for x in data["parent"]["solarGeometry"]}
    d["sza"] = sol["solar zenith angle (deg)"]["mean"]
    d["saa"] = sol["solar azimuth angle (deg)"]["mean"]
    d["saa_min"], d["saa_max"] = sol["solar azimuth angle (deg)"]["minimum"], sol["solar azimuth angle (deg)"]["maximum"]
    maxima = [r["max"] for r in data["angles"] if r["experiment"] == "tirs_b10"]
    d["angle_max_spread"] = max(maxima) - min(maxima)
    return d


# 作用：运行次数与引擎总耗时（来自 results/*.csv 的 seconds 列）。
def run_stats(data: dict) -> tuple[int, float]:
    seconds = {}
    for r in data["summary"]:
        seconds[r["sensor"]] = r["seconds"]
    total = sum(seconds.values()) + sum(r["seconds"] for r in data["angles"])
    return len(seconds) + len(data["angles"]), total


# 作用：取 class_analysis.json 里某地类（IGBP 编码）的统计。
def cls(data: dict, code: int) -> dict:
    for c in data["analysis"]["classes"]:
        if c["code"] == code:
            return c
    raise KeyError(f"class_analysis.json 中没有地类 {code}")


# 作用：封面标题块与“本案例做到了什么”。
def cover(data: dict, d: dict) -> list:
    b10 = grow(data, "tirs_b10", 0, None)
    hot = grow(data, "tirs_b10", 32, ANGLE_BACK)
    agg = arow(data, "landsat89", "B10")
    ndvi_txt = "、".join(f"{SHORT[s]} {d['ndvi'][s]['mean']:.3f}" for s in OPTICAL_ORDER)
    sensors = data["sensors"]["sensors"]
    n_opt, n_th = sum(len(s["optical"]) for s in sensors), sum(len(s["thermal"]) for s in sensors)
    runs, total = run_stats(data)
    g = lambda v, a: f(grow(data, "tirs_b10", v, a)["delta_mean_vs_nadir"], 2, True)
    story = [Spacer(1, 2 * mm), P(TITLE, "title"),
             P("StreamStudio · STREAM 遥感影像模拟案例　|　场景时刻 2019-08-15 02:55 UTC（北京时间 10:55）　|　300 m，EPSG:32650，592 × 598", "subtitle")]
    items = [
        f"<b>一个场景，{len(sensors)} 个传感器，光学与热红外一体化。</b>在同一个北京 300 m 场景上，完成 Landsat 8/9、MODIS、Sentinel-2、ASTER、ECOSTRESS、VIIRS、FY-3D MERSI-II 的模拟，"
        f"共 {n_opt} 个光学波段与 {n_th} 个热红外波段，每个波段都有数据依据与出处（第 2 章、附录）。",
        f"<b>流程可一键复现。</b>1 次 EB 母工程 + {len(sensors)} 个传感器变体 + {len(data['angles'])} 个观测角变体，共 {runs} 次引擎运行，引擎总耗时约 {total:.0f} s；"
        "run_all.mjs 一条命令完成运行与统计，仓库内的 reference/ 即可重建全部配图与本报告。",
        f"<b>光谱实验（A）。</b>五个传感器的 NDVI 在 {d['ndvi_min']:.3f}–{d['ndvi_max']:.3f} 之间（{ndvi_txt}）；热红外亮温随波长平滑变化"
        f"（ASTER 由 {d['aster_b10']:.3f} K（8.3 μm）到 {d['aster_b14']:.3f} K（11.3 μm）），同波段不同传感器均值极差 {max(d['group_a_spread'], d['group_b_spread']) * 1000:.1f} mK。",
        f"<b>角度实验（B）。</b>Landsat B10 相对天底（{b10['mean']:.3f} K），后向 VZA 20°/32°（热点）/40°/55° 为 {g(20, ANGLE_BACK)}/{f(hot['delta_mean_vs_nadir'], 2, True)}/{g(40, ANGLE_BACK)}/{g(55, ANGLE_BACK)} K，"
        f"前向 20°/40°/55° 为 {g(20, ANGLE_FWD)}/{g(40, ANGLE_FWD)}/{g(55, ANGLE_FWD)} K；热点出现在 VZA ≈ 太阳天顶角（{d['sza']:.2f}°）处。",
        f"<b>尺度实验（C）。</b>B10 由 300 m 聚合到 1 km，均值 {f(agg['mean_1km_avg'] - agg['mean_300m'], 3, True)} K，标准差 {agg['std_300m']:.2f}→{agg['std_1km_avg']:.2f} K，"
        f"最大值 {agg['max_300m']:.2f}→{agg['max_1km_avg']:.2f} K，热点占比 {agg['hot_frac_300m_p95']:.3f}→{agg['hot_frac_1km_avg_p95']:.3f}。",
        "<b>结果的含义。</b>输出是无大气的冠层顶反射率与亮温，传感器之间的数值差异反映波段设置的影响，不是各传感器实际可观测到的差异；波段采用高斯近似；"
        "尚未与实测数据对比。与 Landsat 8 B10 实测、MOD11 产品的对比，大气校正，官方光谱响应函数与城市三维结构的补充见第 5 章“适用范围与后续工作”。",
    ]
    box = Table([[Paragraph("本案例做到了什么", STY["h2"])]] + [[Paragraph(t, STY["box"], bulletText="•")] for t in items], colWidths=[FRAME_W])
    box.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, -1), PALE_BLUE), ("BOX", (0, 0), (-1, -1), 0.6, LINE),
                             ("LEFTPADDING", (0, 0), (-1, -1), 8), ("RIGHTPADDING", (0, 0), (-1, -1), 8),
                             ("TOPPADDING", (0, 0), (-1, -1), 2), ("BOTTOMPADDING", (0, 0), (-1, -1), 2)]))
    story += [box, Spacer(1, 4 * mm)]
    return story


# 作用：第 1 章 目的与场景。
def section_scene(data: dict, d: dict) -> list:
    summary = data["parent"]
    counts = summary["counts"]
    runs, _ = run_stats(data)
    story = [P("1　目的与场景", "h1"),
             P("<b>结论。</b>同一个地表状态可以被不同卫星的波段设置“成像”：先用 STREAM 的 ERA5 能量平衡（EB）求一次四组分温度（光照/阴影土壤、光照/阴影叶片），"
               f"再让 Landsat 8/9、MODIS、Sentinel-2、ASTER、ECOSTRESS、VIIRS、FY-3D MERSI-II 的波段设置复用这组温度成像（共 {runs} 次引擎运行），"
               "并在其上做观测角（热点）实验与 300 m → 1 km 聚合实验。本案例由此给出波段设置、观测角与空间尺度三类因素各自的影响量级。"),
             P("<b>范围与时刻。</b>北京市行政区范围（约 177 km × 179 km 的外包矩形，300 m 网格，EPSG:32650，592 × 598 像元，"
               f"其中市域内有效像元 {int(counts['thermalPixels'])} 个，光学有效像元 {int(counts['opticalPixels'])} 个，水体 {int(counts['waterOrFillPixels'])} 个只做热红外）。"
               f"过境时刻为 2019-08-15 02:55 UTC，即北京时间 10:55；场景平均太阳天顶角 {d['sza']:.2f}°，太阳方位角 {d['saa']:.2f}°（范围 {d['saa_min']:.1f}–{d['saa_max']:.1f}°）。"),
             P("<b>数据来源。</b>地类由 2019 年 GLC_FCS30D 映射到 IGBP，DEM 为 FABDEM，LAI 为 Landsat 30 m 产品，300 m 输入由 30 m 网格聚合；气象强迫为 ERA5（2019-08-15，约 25 km），"
               "与示例 landsat_beijing_eb 完全相同（输入与 ERA5 数据包复制进本案例的 inputs/、era5/，使案例自包含）。"
               f"外包矩形里市域外的 255 值填充像元有 {592 * 598 - int(counts['thermalPixels'])} 个（= 592 × 598 - {int(counts['thermalPixels'])}），"
               f"引擎日志把它们统计为“未知地类 {int(counts['unknownTypePixels'])}”，这是市域外填充值的统计项。")]
    story += FIG(FIG_DIR / "fig01_inputs.png", "输入图：(a) IGBP 地类（括号内为像元数）；(b) LAI；(c) DEM 高程。坐标为相对栅格西北角的东向/北向距离（km）。", 90 * mm)
    return story


# 作用：第 2 章 传感器与波段表（来自 sensors.json，含“数据依据”列，并列出处编号）。
def section_sensors(data: dict, refs: dict) -> list:
    def sensor_rows(kind: str) -> list[list]:
        rows = [["传感器", "波段", "中心波长", "带宽 FWHM", "官方范围", "数据依据", "出处"]]
        for s in data["sensors"]["sensors"]:
            for b in s[kind]:
                rng = f"{b['range'][0]:g}–{b['range'][1]:g}" if b.get("range") else "—"
                rows.append([SENSOR_NAMES.get(s["id"], s["name"]), b["name"], f"{b['center']:g}", f"{b['fwhm']:g}", rng, BASIS_LABEL[b["basis"]], f"[{refs[b['source']]}]"])
        return rows
    optical, thermal = sensor_rows("optical"), sensor_rows("thermal")
    widths = [34 * mm, 16 * mm, 24 * mm, 24 * mm, 28 * mm, 28 * mm, 14 * mm]
    counts: dict = {}
    for s in data["sensors"]["sensors"]:
        for kind in ("optical", "thermal"):
            for b in s[kind]:
                counts[b["basis"]] = counts.get(b["basis"], 0) + 1
    meaning = {"official-spec": "机构公布的波段范围或中心/带宽", "official-srf": "机构公布的光谱响应函数", "literature": "公开文献给出的在轨实测中心与带宽（ECOSTRESS）",
               "nominal": "标称值"}
    basis_txt = "；".join(f"{BASIS_LABEL[k]} {counts[k]} 个（{meaning[k]}）" for k in BASIS_LABEL if k in counts)
    story = [P("2　传感器与波段表", "h1"),
             P(f"<b>结论。</b>{len(data['sensors']['sensors'])} 个传感器共 {len(optical) - 1} 个光学波段、{len(thermal) - 1} 个热红外波段，每个波段都记录了数据依据与出处。"
               f"“数据依据”一栏统计：{basis_txt}。出处编号见附录。"),
             P("波段设置全部来自案例的 sensors.json。凡官方给出波段范围 [lo, hi] 的，取中心 = (lo + hi)/2、带宽 = hi - lo（等效矩形近似，范围保留在“官方范围”列）；"
               "官方给出中心与带宽时直接采用。")]
    story += TBL("光学波段（中心波长、带宽、官方范围单位均为 nm）", optical, widths, align_right={2, 3, 4})
    story += TBL("热红外波段（单位均为 μm）", thermal, widths, align_right={2, 3, 4})
    return story


# 作用：第 3 章 方法与假设。
def section_method(data: dict, d: dict) -> list:
    runs, total = run_stats(data)
    n_sensor = len({r["sensor"] for r in data["summary"]}) - 1
    par = data["parent"]["temperature"]
    return [
        P("3　方法", "h1"),
        P("<b>结论。</b>两步法让组分温度只算一次，所有传感器、角度、聚合实验共享同一个地表状态，因此结果之间的差异只来自观测设置。"),
        P("<b>两步法。</b>(1) 母工程 project.json：STREAM 的 EB 模式用 ERA5 气象强迫（2019-08-15 00–04 UTC 的整点时次）求能量平衡，"
          "输出 4 个组分温度 Tss/Tsh/Tcs/Tch（光照土壤、阴影土壤、光照叶片、阴影叶片，components.tif）；"
          "(2) 各传感器变体 variants/&lt;传感器&gt;.json 改用辐射传输（RT）模式，组分温度来源设为 components.tif，只含该传感器的波段，直接成像。"
          f"本报告共 {runs} 次引擎运行（母工程 1 + 传感器 {n_sensor} + 角度 {len(data['angles'])}），引擎总耗时约 {total:.1f} s。"),
        P("<b>EB 求解状态。</b>"
          f"{int(par['convergedPixels'])}/{int(par['solvedPixels'])} 个 EB 像元收敛，{int(par['nonConvergedPixels'])} 个未收敛，{int(par['bracketFallbackPixels'])} 个使用了 1/L 括号法回退；"
          f"ERA5 数据包只覆盖 2019-08-15 一天，起转时次缺省请求 {int(par['spinupNodesRequested'])} 个，实际使用 {int(par['spinupNodesUsed'])} 个（运行日志有相应提示）。"),
        P("<b>结果的含义。</b>本案例输出无大气的冠层顶量：光学为冠层顶方向反射率，热红外为冠层顶方向亮温（STREAM 的 TiRT 四组分闭合，大气 = none），"
          "不含大气透过率、程辐射与传感器噪声。因此各传感器结果之间的差异反映的是波段设置（中心波长与带宽）的影响，不是各传感器实际观测到的差异。"),
        P("<b>与实测数据的对比。</b>目前结果尚未与实测数据（Landsat 8 B10 实测亮温、MOD11 地表温度产品等）对比；这部分作为后续工作列入第 5 章。"),
        P("<b>波段近似。</b>STREAM 用 center/fwhm 描述<b>高斯形</b>光谱响应函数，本案例各波段采用高斯近似，官方光谱响应函数（SRF）文件作为后续工作接入。"
          "对宽而平的波段（如 MODIS B1、MERSI-II 的 50 nm 波段）高斯形与真实响应的差别相对较大，对窄波段更接近；"
          "因此光学结果适合比较波段位置与宽度的影响，热红外波段的差异则主要来自地表发射率光谱（见 4.3）。"),
        P("<b>数据依据。</b>波段数值取自各机构公布的规格（NASA、USGS、ESA、NOAA、JPL、国家卫星气象中心），ECOSTRESS 采用公开文献给出的在轨实测中心与带宽；"
          "逐波段的依据与出处见第 2 章表格与附录。"),
        P("<b>观测角设置。</b>VAA 约定为像元指向传感器的方位：VAA = 139° ≈ 太阳方位角，传感器在太阳一侧（后向散射，含热点）；VAA = 319° 为前向。"
          f"角度实验只做热红外（Landsat TIRS B10 与 MODIS 31），VZA 取 0°、20°、40°、55°（前后向各一）并额外加了 VZA = 32° 后向（≈ 太阳天顶角 {d['sza']:.2f}°）的热点方向点。"),
        P("<b>聚合设置。</b>用 GDAL warp 把 300 m 栅格聚合到 1 km（平均 average 与最大值 max 两种），1 km 网格<b>从原栅格西北角起算</b>，边缘格为部分覆盖，不是 MODIS 正弦网格。"
          "热点定义为≥ 300 m 场 P95 的像元（另给均值 + 2σ 阈值的结果，见 aggregation.csv）；另外按辐亮度平均再反算亮温，与直接平均亮温比较。"),
        P("<b>反射天空。</b>热红外亮温 = 四组分发射 + (1 - 有效发射率) × 天空辐亮度。母工程的 EB 路径用 ERA5 的向下长波 strd 均值，变体的 RT 路径用 Swinbank 晴空公式（见 4.3）。"),
    ]


# 作用：4.1 EB 概览。
def section_eb(data: dict, d: dict) -> list:
    comp = data["parent"]["components"]
    flux = {x["name"].split(" (")[0]: x for x in data["parent"]["fluxes"]}
    rows = [["量", "有效像元", "均值", "最小", "最大"]]
    for x in comp + [flux["net radiation"], flux["sensible heat"], flux["latent heat"], flux["ground heat"]]:
        unit = "K" if "(K)" in x["name"] else "W m<super>-2</super>"
        label = {"net radiation": "净辐射 Rn", "sensible heat": "感热 H", "latent heat": "潜热 LE", "ground heat": "土壤热通量 G"}.get(x["name"].split(" (")[0], x["name"].split(" (")[0])
        rows.append([f"{label}（{unit}）", f"{int(x['validPixels'])}", f(x["mean"], 2), f(x["minimum"], 2), f(x["maximum"], 2)])
    by_tss = sorted(data["analysis"]["classes"], key=lambda c: -c["tss"])
    hot_names = "、".join(c["name"] for c in by_tss[:3])
    story = [P("4　结果与分析", "h1"), P("4.1　EB 概览：四组分温度与通量", "h2"),
             P(f"<b>结论。</b>母工程给出了清晰的组分温度层次：光照土壤 {comp[0]['mean']:.1f} K 明显最热，光照叶片 {comp[2]['mean']:.1f} K、阴影叶片 {comp[3]['mean']:.1f} K、阴影土壤 {comp[1]['mean']:.1f} K 依次更低；"
               f"土壤的光照/阴影温差（{comp[0]['mean'] - comp[1]['mean']:.1f} K）远大于叶片（{comp[2]['mean'] - comp[3]['mean']:.1f} K）。"),
             P(f"10:55（当地）太阳天顶角 {d['sza']:.1f}°。按地类统计（图 {fnum('fig02_')}），光照土壤温度最高的三类依次是{hot_names}（{by_tss[0]['tss']:.1f}、{by_tss[1]['tss']:.1f}、{by_tss[2]['tss']:.1f} K），"
               f"最低的是{by_tss[-1]['name']}（{by_tss[-1]['tss']:.1f} K）。"
               f"能量平衡上，Rn 均值 {flux['net radiation']['mean']:.0f}，LE {flux['latent heat']['mean']:.0f}，H {flux['sensible heat']['mean']:.0f}，G {flux['ground heat']['mean']:.0f} W m<super>-2</super>"
               f"（残差均值 {flux['energy balance residual']['mean']:.2f}，最大 {flux['energy balance residual']['maximum']:.1f} W m<super>-2</super>）。")]
    story += FIG(FIG_DIR / "fig02_eb_components.png", "母工程 EB 组分温度：(a)–(d) 四个组分温度（各自 1–99% 拉伸）；(e) 各地类组分温度均值（n 为像元数，水体不参与 EB）。", 110 * mm)
    flux_fig = FIG_DIR / "fig02b_eb_fluxes.png"
    if flux_fig.exists():
        story += FIG(flux_fig, "母工程 EB 通量：(a)–(d) Rn、H、LE、G 地图（1–99% 拉伸）；(e) 各地类通量均值。", 110 * mm)
    story += TBL("母工程 EB 的组分温度与通量统计（取自 reference/parent_scene_summary.json）", rows, [58 * mm, 28 * mm, 30 * mm, 30 * mm, 28 * mm], align_right={1, 2, 3, 4})
    return story


# 作用：4.2 实验 A 光学。
def section_optical(data: dict, d: dict) -> list:
    bands = {sid: [r for r in data["summary"] if r["sensor"] == sid and r["kind"] == "optical"] for sid in OPTICAL_ORDER}
    rows = [["传感器", "波段", "中心 nm", "均值", "标准差", "P05", "P50", "P95"]]
    for sid in OPTICAL_ORDER:
        for r in bands[sid]:
            rows.append([SHORT[sid], r["band"], f"{r['center']:g}", f(r["mean"], 4), f(r["std"], 4), f(r["p05"], 4), f(r["p50"], 4), f(r["p95"], 4)])
    ndvi_rows = [["传感器", "NDVI 波段对", "NDVI 均值", "标准差", "与 Landsat 之差"]]
    base = d["ndvi"]["landsat89"]["mean"]
    for sid in OPTICAL_ORDER:
        r = d["ndvi"][sid]
        ndvi_rows.append([SHORT[sid], r["band"].replace("NDVI", ""), f(r["mean"], 3), f(r["std"], 3), f(r["mean"] - base, 3, True)])
    pv = [["波段", "母工程中心 nm", "变体中心 nm", "母工程 EB 路径均值", "Landsat 变体 RT 路径均值", "差（母 - 变体）"]]
    for band, a, b, ca, cb in d["parent_vs_variant"]:
        pv.append([band, f"{ca:g}", f"{cb:g}", f(a, 4), f(b, 4), f(a - b, 4, True)])
    pv.append(["NDVI(B4,B5)", "—", "—", f(d["ndvi_parent"]["mean"], 3), f(d["ndvi"]["landsat89"]["mean"], 3), f(d["parent_ndvi_diff"], 3, True)])
    red = {sid: next(r for r in bands[sid] if r["band"] == nm) for sid, nm in (("landsat89", "B4"), ("modis", "B1"), ("sentinel2", "B4"), ("viirs", "M5"), ("fy3d_mersi2", "CH3"))}
    red_sorted = sorted(red.values(), key=lambda r: r["center"])
    red_monotone = all(a["mean"] > b["mean"] for a, b in zip(red_sorted, red_sorted[1:]))
    red_txt = "、".join(f"{r['center']:g} nm 的 {SHORT[sid]} {r['band']} 为 {r['mean']:.4f}" for r, sid in ((red[s], s) for s in sorted(red, key=lambda s: red[s]["center"])))
    nir4 = [srow(data, "landsat89", "B5"), srow(data, "modis", "B2"), srow(data, "viirs", "M7"), srow(data, "fy3d_mersi2", "CH4")]
    s2 = {b: srow(data, "sentinel2", b) for b in ("B7", "B8", "B8A")}
    swir_long = [srow(data, "modis", "B6"), srow(data, "fy3d_mersi2", "CH6")]
    swir_short = [srow(data, "landsat89", "B6"), srow(data, "sentinel2", "B11"), srow(data, "viirs", "M10")]
    story = [P("4.2　实验 A：各传感器的光学反射率与 NDVI", "h2"),
             P(f"<b>结论。</b>近红外反射率在各传感器之间几乎一致，红光差异最明显，五个传感器的 NDVI 均值落在 {d['ndvi_min']:.3f}–{d['ndvi_max']:.3f}（极差 {d['ndvi_max'] - d['ndvi_min']:.3f}），"
               "波段设置的影响因此可以被定量地分离出来。"),
             P(f"图 {fnum('fig03')} 把五个传感器的红光、近红外、短波红外 1 波段放在一起比较。<b>近红外</b>均值 {min(r['mean'] for r in nir4):.4f}–{max(r['mean'] for r in nir4):.4f}"
               f"（Landsat B5、MODIS B2、VIIRS M7、MERSI-II CH4）；Sentinel-2 的 B7（{s2['B7']['center']:g} nm）、B8（{s2['B8']['center']:g} nm，带宽 106 nm）、B8A（{s2['B8A']['center']:g} nm）分别为 "
               f"{s2['B7']['mean']:.4f}、{s2['B8']['mean']:.4f}、{s2['B8A']['mean']:.4f}，中心越靠近 865 nm 越接近上述平台值。"
               f"<b>红光</b>差异最明显：按中心波长由短到长，{red_txt}；{'反射率随中心波长增大单调降低' if red_monotone else '反射率随中心波长变化'}。"
               f"<b>短波红外 1</b>：中心 1640 nm 的 MODIS B6、MERSI-II CH6 为 {swir_long[0]['mean']:.4f}、{swir_long[1]['mean']:.4f}，"
               f"中心在 1608–1614 nm 的 Landsat B6、Sentinel-2 B11、VIIRS M10 为 {swir_short[0]['mean']:.4f}、{swir_short[1]['mean']:.4f}、{swir_short[2]['mean']:.4f}。"
               "这些差异都来自波段中心与带宽的不同（本案例不含大气）。")]
    story += FIG(FIG_DIR / "fig03_optical_band_boxplots.png", "同名波段反射率的分布箱线图（须线 5–95%，菱形为均值）：(a) 红光；(b) 近红外；(c) 短波红外 1。", 75 * mm)
    bins = data["analysis"]["ndvi_bins"]
    bin_txt = "；".join(f"Landsat NDVI {b['lo']:g}–{b['hi']:g} 的像元为 {b['spread_mean']:.3f}" if b["lo"] > 0 else f"NDVI &lt; {b['hi']:g} 的像元为 {b['spread_mean']:.3f}" for b in bins)
    story += [P(f"<b>NDVI。</b>五个传感器的 NDVI 均值在 {d['ndvi_min']:.3f}–{d['ndvi_max']:.3f} 之间（表 {Counter.table + 2}）。"
                f"VIIRS 最高（{d['ndvi']['viirs']['mean']:.3f}），其红光反射率最低（{red['viirs']['mean']:.4f}）；MODIS 最低（{d['ndvi']['modis']['mean']:.3f}），其红光反射率最高（{red['modis']['mean']:.4f}），近红外与其他传感器相同；"
                f"Sentinel-2 为 {d['ndvi']['sentinel2']['mean']:.3f}，MERSI-II 为 {d['ndvi']['fy3d_mersi2']['mean']:.3f}，与 MODIS 接近（红光带都是 50 nm 宽）。"
                f"图 {fnum('fig04')} 的散点给出逐像元对比；按 Landsat NDVI 分段，五个传感器逐像元 NDVI 的极差（最大 - 最小）均值为：{bin_txt}。")]
    story += FIG(FIG_DIR / "fig04_ndvi.png", "五个传感器的 NDVI 地图与相对 Landsat 的逐像元散点（随机抽样 8000 点）。", 125 * mm)
    story += TBL("各传感器光学波段的反射率统计（300 m 场景，来自 results/summary.csv）", rows, [24 * mm, 16 * mm, 20 * mm, 22 * mm, 22 * mm, 22 * mm, 22 * mm, 22 * mm], align_right={2, 3, 4, 5, 6, 7})
    story += TBL("各传感器 NDVI（逐像元 NDVI 的均值与标准差）", ndvi_rows, [30 * mm, 36 * mm, 30 * mm, 30 * mm, 38 * mm], align_right={2, 3, 4})
    diff_bands = [band for band, _, _, ca, cb in d["parent_vs_variant"] if abs(ca - cb) > 1e-9]
    same_band = max((x for x in d["parent_vs_variant"] if abs(x[3] - x[4]) <= 1e-9), key=lambda x: abs(x[1] - x[2]))
    story += [P(f"<b>母工程与变体的对照。</b>母工程走 EB 路径、直接带 Landsat 波段；landsat89 变体走 RT 路径、复用同一组组分温度。两者反射率之差最大为 {d['parent_opt_maxdiff']:.4f}（{d['parent_opt_maxband']}），"
                f"NDVI 差 {f(d['parent_ndvi_diff'], 3, True)}（表 {Counter.table + 1}），两条路径一致性良好。两套工程对 {'、'.join(diff_bands)} 的中心取值略有不同（见表中中心列），"
                f"中心相同的 {same_band[0]} 仍相差 {abs(same_band[1] - same_band[2]):.4f}；两条路径在光学处理细节上的逐项拆解列入后续工作。"
                "报告里引用 Landsat 数值一律采用 landsat89 变体，母工程的数值只在本表和 4.3 用作对照。")]
    story += TBL("母工程（EB 路径）与 Landsat 8/9 变体（RT 路径）的光学差", pv, [26 * mm, 28 * mm, 26 * mm, 34 * mm, 38 * mm, 22 * mm], align_right={1, 2, 3, 4, 5})
    return story


# 作用：4.3 热红外。
def section_thermal(data: dict, d: dict) -> list:
    rows = [["传感器", "波段", "中心 μm", "均值 K", "标准差 K", "最小 K", "最大 K", "相对 Landsat B10"]]
    base = srow(data, "landsat89", "B10")["mean"]
    for sid in THERMAL_ORDER:
        for r in [x for x in data["summary"] if x["sensor"] == sid and x["kind"] == "thermal"]:
            rows.append([SHORT[sid], r["band"], f"{r['center']:g}", f(r["mean"], 3), f(r["std"], 3), f(r["min"], 2), f(r["max"], 2), f(r["mean"] - base, 3, True)])
    lst = [["传感器", "波段", "中心 μm", "亮温均值 K", "归一化 LST 均值 K", "差均值 K", "差标准差 K", "差最小 K", "差最大 K"]]
    for r in data["lst"]:
        lst.append([SHORT[r["sensor"]], r["band"], f"{r['center_um']:g}", f(r["bt_mean"], 3), f(r["lst_mean"], 3), f(r["diff_mean"], 3, True), f(r["diff_std"], 3), f(r["diff_min"], 2, True), f(r["diff_max"], 2, True)])
    b10, b11 = srow(data, "landsat89", "B10"), srow(data, "landsat89", "B11")
    eco1, eco5 = srow(data, "ecostress", "TIR1"), srow(data, "ecostress", "TIR5")
    am = data["analysis"]["aster_b10_b14"]
    lowc = [(cls(data, c)["name"], 100 * (1 - cls(data, c)["frac_high_mode"])) for c in (1, 4, 10)]
    highc = [(cls(data, c)["name"], 100 * cls(data, c)["frac_high_mode"]) for c in (12, 13)]
    story = [P("4.3　实验 A（续）：各载荷的热红外亮温", "h2"),
             P(f"<b>结论。</b>各载荷的热红外亮温随波长平滑变化（8.3 μm 比 12 μm 高约 {d['eco_tir1'] - d['eco_tir5']:.3f} K），同波段不同传感器的均值极差小于 0.01 K"
               f"（10.5–11.3 μm 窗区 {d['group_a_spread'] * 1000:.1f} mK，12 μm 附近 {d['group_b_spread'] * 1000:.1f} mK）。"),
             P("这里的亮温是没有大气的冠层顶方向亮温（STREAM 的 TiRT 四组分闭合，大气 = none），不含大气透过率、程辐射和传感器噪声，"
               "各载荷之间的亮温差反映的是波段中心波长与带宽的影响，不是传感器之间实际可观测到的差异。"),
             P(f"Landsat B10/B11 为 {b10['mean']:.3f}/{b11['mean']:.3f} K（标准差 {b10['std']:.2f} K，最小 {b10['min']:.1f}，最大 {b10['max']:.2f} K）；"
               f"ASTER 五个波段由 B10（8.3 μm）{d['aster_b10']:.3f} K 单调降到 B14（11.3 μm）{d['aster_b14']:.3f} K；ECOSTRESS 由 TIR1（{eco1['center']:g} μm）{d['eco_tir1']:.3f} K 降到 TIR5（{eco5['center']:g} μm）{d['eco_tir5']:.3f} K。"
               f"8.3 μm 比 12 μm 高约 {d['eco_tir1'] - d['eco_tir5']:.3f} K（ECOSTRESS）/ {d['aster_b10'] - srow(data,'landsat89','B11')['mean']:.3f} K（ASTER B10 对 Landsat B11），"
               "这一趋势来自模型中随波长变化的地表发射率（以及随之变化的反射天空贡献）。"
               f"图 {fnum('fig06')}(c) 的 ASTER B10 - B14 逐像元差呈双峰分布，峰位在 {am['peaks'][0]:.3f} K 与 {am['peaks'][-1]:.3f} K。"
               f"按地类统计：低值峰由森林与草地构成（{'、'.join(f'{n} {p:.1f}%' for n, p in lowc)} 的像元落在两峰间谷值 {am['valley']:.3f} K 以下），"
               f"高值峰由农田与城市建成区构成（{'、'.join(f'{n} {p:.1f}%' for n, p in highc)} 的像元在谷值以上）。"),
             P(f"<b>同波段不同传感器：</b>10.5–11.3 μm 窗区的 Landsat B10、MODIS 31、VIIRS M15、MERSI-II CH24 均值极差 {d['group_a_spread'] * 1000:.1f} mK；"
               f"12 μm 的 Landsat B11、MODIS 32、VIIRS M16、MERSI-II CH25、ECOSTRESS TIR5 极差 {d['group_b_spread'] * 1000:.1f} mK。这是中心波长相近、同样的无大气假设下的结果。"),
             P(f"<b>母工程与变体。</b>母工程 Landsat B10 为 {d['parent_b10']:.3f} K，变体 {d['variant_b10']:.3f} K，母工程低 {abs(d['parent_b10_diff']):.3f} K。"
               "两条路径的反射天空来源不同：EB 路径取 ERA5 向下长波 strd 的场景均值（运行日志 382.5 W m<super>-2</super>），RT 路径取 Swinbank 晴空公式（运行日志 394.5 W m<super>-2</super>）。")]
    story += FIG(FIG_DIR / "fig05_tir_brightness_maps.png", "各载荷热红外亮温地图（统一色标 298–317 K）：Landsat B10、MODIS 31、ASTER B10 与 B14、ECOSTRESS TIR1 与 TIR5。统一色标下肉眼难以区分，差别见图 " + str(fnum("fig06")) + "。", 150 * mm)
    story += FIG(FIG_DIR / "fig06_tir_spectral.png", "(a) 场景均值亮温随中心波长（横线为 FWHM/2）；(b) 亮温与发射率归一化地表温度之差随波长；(c) ASTER B10 - B14 的逐像元亮温差分布。", 72 * mm)
    story += TBL("各载荷热红外波段的亮温统计（300 m，来自 results/summary.csv）", rows, [24 * mm, 16 * mm, 20 * mm, 24 * mm, 24 * mm, 20 * mm, 20 * mm, 26 * mm], align_right={2, 3, 4, 5, 6, 7})
    story += [P(f"<b>亮温与发射率归一化地表温度之差。</b>引擎同时输出按地类固定发射率归一的方向温度（tir_surface_temperature.tif，只含发射项）。亮温与该 LST 的均值差为 "
                f"{f(min(r['diff_mean'] for r in data['lst']), 3, True)} 至 {f(max(r['diff_mean'] for r in data['lst']), 3, True)} K，随波长增大向更负变化；逐像元的差在 {f(min(r['diff_min'] for r in data['lst']), 2, True)} 至 {f(max(r['diff_max'] for r in data['lst']), 2, True)} K 之间。"
                "该表由完整运行输出统计得到，已存入 tables/tir_bt_minus_lst.csv（reference 精简结果没有 LST 栅格）。")]
    story += TBL("亮温与发射率归一化地表温度（LST）之差（tables/tir_bt_minus_lst.csv）", lst, [20 * mm, 14 * mm, 17 * mm, 22 * mm, 26 * mm, 18 * mm, 19 * mm, 18 * mm, 18 * mm], align_right={2, 3, 4, 5, 6, 7, 8})
    return story


# 作用：4.4 实验 B 观测角。
def section_angles(data: dict, d: dict) -> list:
    rows = [["VZA", "方向", "VAA", "B10 均值 K", "B10 相对天底 K", "MODIS 31 相对天底 K", "B10 标准差 K", "B10 最大 K"]]
    for vza, vaa, name in ((0, None, "天底"), (20, ANGLE_BACK, "后向"), (32, ANGLE_BACK, "后向（热点）"), (40, ANGLE_BACK, "后向"), (55, ANGLE_BACK, "后向"),
                           (20, ANGLE_FWD, "前向"), (40, ANGLE_FWD, "前向"), (55, ANGLE_FWD, "前向")):
        a, m = grow(data, "tirs_b10", vza, vaa), grow(data, "modis_b31", vza, vaa)
        rows.append([f"{vza}°", name, "—" if vaa is None else f"{vaa:g}°", f(a["mean"], 3), f(a["delta_mean_vs_nadir"], 3, True), f(m["delta_mean_vs_nadir"], 3, True), f(a["std"], 3), f(a["max"], 2)])
    nad = grow(data, "tirs_b10", 0, None)
    hot = grow(data, "tirs_b10", 32, ANGLE_BACK)
    g = lambda v, a: grow(data, "tirs_b10", v, a)["delta_mean_vs_nadir"]
    modis_diff = max(abs(r["mean"] - grow(data, "tirs_b10", r["vza"], r["vaa"])["mean"]) for r in data["angles"] if r["experiment"] == "modis_b31" and grow(data, "tirs_b10", r["vza"], r["vaa"]))
    ana = data["analysis"]
    cr = ana["corr"]
    classes = sorted(ana["classes"], key=lambda c: c["d_hot32"])
    crows = [["地类", "像元数", "光照土壤 Tss K", "Tss - Tsh K", "Tcs - Tch K", "热点 32° 后向 K", "55° 后向 K", "55° 前向 K"]]
    for c in ana["classes"]:
        crows.append([c["name"], f"{c['n']}", f(c["tss"], 1), f(c["tss_tsh"], 2), f(c["tcs_tch"], 2), f(c["d_hot32"], 2, True), f(c["d_b55"], 2, True), f(c["d_f55"], 2, True)])
    story = [P("4.4　实验 B：观测角与热点", "h2"),
             P(f"<b>结论。</b>后向热点方向（VZA 32° ≈ 太阳天顶角 {d['sza']:.2f}°）的场景均值亮温比天底高 {f(hot['delta_mean_vs_nadir'], 2)} K，是所有观测方向中最高的；"
               f"大角度观测的亮温低于天底，前向 55° 最低（{f(g(55, ANGLE_FWD), 2, True)} K）。MODIS 31 与 Landsat B10 的角度曲线几乎重合（场景均值最多差 {modis_diff:.3f} K）。"),
             P(f"太阳天顶角 {d['sza']:.2f}°、方位角 {d['saa']:.2f}°。传感器在太阳一侧（后向，VAA = 139°）时，相对天底（{nad['mean']:.3f} K）的场景均值亮温为：VZA 20° {f(g(20, ANGLE_BACK), 2, True)} K、"
               f"<b>VZA 32°（热点方向）{f(hot['delta_mean_vs_nadir'], 2, True)} K</b>、40° {f(g(40, ANGLE_BACK), 2, True)} K、55° {f(g(55, ANGLE_BACK), 2, True)} K；"
               f"前向（VAA = 319°）20°/40°/55° 为 {f(g(20, ANGLE_FWD), 2, True)}/{f(g(40, ANGLE_FWD), 2, True)}/{f(g(55, ANGLE_FWD), 2, True)} K。"
               f"VZA 20° 与 40° 恰好分居热点两侧，后向亮温几乎对称（{f(g(20, ANGLE_BACK), 3, True)} 与 {f(g(40, ANGLE_BACK), 3, True)} K）。"
               f"标准差在热点处最大（{hot['std']:.2f} K，天底 {nad['std']:.2f} K）；场景最大值（{nad['max']:.2f} K，出现在{ana['nadir_max']['name']}像元）在所有角度几乎不变（B10 各角度最大值的极差 {d['angle_max_spread'] * 1000:.1f} mK）。"),
             P(f"<b>热点增量与组分温差。</b>逐像元统计显示，热点增量（VZA 32° 后向亮温减天底亮温）与光照/阴影土壤温差 Tss - Tsh、光照/阴影叶片温差 Tcs - Tch 的相关系数分别为 "
               f"{cr['hot32:tss_tsh']:.2f} 与 {cr['hot32:tcs_tch']:.2f}（不含水体）：光照/阴影温差越大的像元，热点方向的亮温增量越大。"
               f"分地类看（表 {Counter.table + 2}），热点增量在 {classes[0]['name']}最小（{f(classes[0]['d_hot32'], 2, True)} K）、{classes[-1]['name']}最大（{f(classes[-1]['d_hot32'], 2, True)} K）。"),
             P("<b>适用范围。</b>角度结果来自 STREAM 单层冠层模型中的光照/阴影组分，没有建筑物阴影、墙面与街谷多次散射；城市三维结构引起的方向亮温各向异性建议用 HiStream 的面元/体元模式补充（见第 5 章）。"
               "本实验只做热红外，光学 BRDF 作为后续工作。")]
    story += FIG(FIG_DIR / "fig07_angle_principal_plane.png", "主平面（后向为正、前向为负）观测天顶角实验：(a) 亮温相对天底的变化，星号为热点方向 VZA = 32°；(b) 场景亮温标准差；(c) 场景亮温 P95。虚线为太阳天顶角。", 70 * mm)
    story += TBL("Landsat TIRS B10 与 MODIS 31 主平面亮温随 VZA 变化（results/angles.csv）", rows, [14 * mm, 28 * mm, 14 * mm, 24 * mm, 26 * mm, 32 * mm, 22 * mm, 20 * mm], align_right={0, 2, 3, 4, 5, 6, 7})
    story += TBL("分地类的组分温差与 B10 观测角亮温变化（相对天底；tables/class_analysis.json）", crows, [24 * mm, 16 * mm, 22 * mm, 20 * mm, 20 * mm, 24 * mm, 24 * mm, 24 * mm], align_right={1, 2, 3, 4, 5, 6, 7})
    return story


# 作用：4.5 实验 C 聚合。
def section_aggregation(data: dict, d: dict) -> list:
    rows = [["产品", "300 m 均值", "1 km 均值", "300 m 标准差", "1 km 标准差", "300 m 最大", "1 km 最大", "热点占比 300 m", "热点占比 1 km 均值", "热点占比 1 km 取最大"]]
    for r in data["aggregation"]:
        label = f"{SHORT.get(r['sensor'], r['sensor'])} {r['band'].replace('NDVI(B4,B5)', 'NDVI').replace('NDVI(B1,B2)', 'NDVI')}"
        rows.append([label, f(r["mean_300m"], 4 if r["kind"] != "thermal" else 3), f(r["mean_1km_avg"], 4 if r["kind"] != "thermal" else 3), f(r["std_300m"], 3), f(r["std_1km_avg"], 3),
                     f(r["max_300m"], 3), f(r["max_1km_avg"], 3), f(r["hot_frac_300m_p95"], 3), f(r["hot_frac_1km_avg_p95"], 3), f(r["hot_frac_1km_maxagg_p95"], 3)])
    thermal = arow(data, "landsat89", "B10")
    b5, ndvi, b4 = arow(data, "landsat89", "B5"), arow(data, "landsat89", "NDVI(B4,B5)"), arow(data, "landsat89", "B4")
    pct = lambda r, a, b: 100.0 * (r[b] / r[a] - 1.0)
    story = [P("4.5　实验 C：300 m 聚合到 1 km", "h2"),
             P(f"<b>结论。</b>聚合到 1 km 后均值守恒（{f(thermal['mean_1km_avg'] - thermal['mean_300m'], 3, True)} K），而标准差、极值与热点面积占比都明显下降："
               f"B10 标准差 {pct(thermal, 'std_300m', 'std_1km_avg'):.0f}%，最大值 {thermal['max_300m']:.2f} → {thermal['max_1km_avg']:.2f} K，热点占比 {thermal['hot_frac_300m_p95']:.3f} → {thermal['hot_frac_1km_avg_p95']:.3f}。"),
             P(f"把 Landsat B10 由 300 m 聚合到 1 km（{int(thermal['n_300m'])} 个 300 m 像元 → {int(thermal['n_1km'])} 个 1 km 格）：均值 {thermal['mean_300m']:.3f} → {thermal['mean_1km_avg']:.3f} K，"
               f"标准差由 {thermal['std_300m']:.2f} 降到 {thermal['std_1km_avg']:.2f} K（{pct(thermal, 'std_300m', 'std_1km_avg'):.0f}%），最大值由 {thermal['max_300m']:.2f} 降到 {thermal['max_1km_avg']:.2f} K，P95 由 {thermal['p95_300m']:.2f} 降到 {thermal['p95_1km_avg']:.2f} K。"
               "均值守恒、极值被抹平、分布变窄，是面积平均的典型表现。"),
             P(f"<b>热点占比</b>（定义为≥ 300 m 场 P95 的面积占比，300 m 自身为 {thermal['hot_frac_300m_p95']:.2f}）：B10 聚合后为 {thermal['hot_frac_1km_avg_p95']:.3f}，降幅约 {abs(pct(thermal, 'hot_frac_300m_p95', 'hot_frac_1km_avg_p95')):.0f}%；"
               f"光学 B4 {b4['hot_frac_1km_avg_p95']:.3f}，B5 {b5['hot_frac_1km_avg_p95']:.3f}，NDVI {ndvi['hot_frac_1km_avg_p95']:.3f}，降幅分别为 {abs(pct(b4, 'hot_frac_300m_p95', 'hot_frac_1km_avg_p95')):.0f}%、{abs(pct(b5, 'hot_frac_300m_p95', 'hot_frac_1km_avg_p95')):.0f}%、{abs(pct(ndvi, 'hot_frac_300m_p95', 'hot_frac_1km_avg_p95')):.0f}%。"
               f"若改用 1 km 内取最大值聚合，热点占比升到 {thermal['hot_frac_1km_maxagg_p95']:.3f}（B10）——一个 1 km 格里只要有一个热像元就算热点。"
               f"改用均值 + 2σ 阈值（B10 {thermal['hot_thr_2s']:.2f} K）时，热点占比由 {thermal['hot_frac_300m_2s']:.3f} 变为 {thermal['hot_frac_1km_avg_2s']:.3f}（均值聚合）/ {thermal['hot_frac_1km_maxagg_2s']:.3f}（取最大）。"
               f"另外，按辐亮度平均再反算亮温比直接平均亮温只高 {thermal['bias_radiance_vs_avg']:.3f} K，可以忽略。"),
             P("因此在 1 km 级产品（如 MODIS 1 km 地表温度）上识别城市热点时，热点的面积占比与极值会低于 300 m 结果；这是聚合尺度本身的效应，与具体传感器无关。"
               "实现上是 GDAL 面积平均，1 km 网格从原栅格西北角起算，不是 MODIS 正弦网格，不含 MODIS 的点扩散函数与视场边缘增宽。")]
    story += FIG(FIG_DIR / "fig08_aggregation_1km.png", "聚合实验：(a) 300 m 与 (b) 1 km（均值聚合）B10 亮温；(c) 分布对比；(d) 热点占比；(e) 1 km 均值聚合后标准差与最大值的相对变化。", 125 * mm)
    story += TBL("300 m → 1 km 聚合统计（results/aggregation.csv；热点占比以 300 m P95 为阈值）", rows,
                 [28 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 17 * mm, 17 * mm], align_right=set(range(1, 10)))
    return story


# 作用：第 5 章 适用范围与后续工作。
def section_scope(data: dict, d: dict) -> list:
    return [P("5　适用范围与后续工作", "h1"),
            P("<b>结论。</b>本案例适合用来比较波段设置、观测角与空间尺度三类因素的影响量级；要得到可与卫星实测直接比较的数值，下面的后续工作依次补充即可，案例的运行流程与输入组织无需改动。"),
            P("5.1　适用范围", "h2"),
            B("<b>模拟量。</b>光学是冠层顶方向反射率、热红外是冠层顶方向亮温（无大气），不含传感器噪声、点扩散函数与几何配准；传感器之间的数值差异反映波段设置的影响，不是各传感器实际可观测到的差异。"),
            B("<b>对比数据。</b>目前尚未与实测数据对比，所有数值差异都是模型内部的比较结果。"),
            B("<b>波段近似。</b>各波段采用高斯形光谱响应（center/fwhm），数据依据见第 2 章；对 50 nm 级宽波段，高斯形与真实响应的差别相对较大。"),
            B("<b>冠层模型。</b>STREAM 把每个像元当单层冠层，适合区域尺度；地类 13（城市建成区）是参数化的不透水面/植被混合，角度与热点结论反映的是冠层内部的光照/阴影分配。"),
            B("<b>气象强迫。</b>ERA5 约 25 km，在 300 m 网格内气象强迫近似为常数（高程订正未执行，ERA5 缺地表位势），温度的空间差异主要来自地类、LAI 和光照几何。"),
            B("<b>实验设置。</b>角度实验只做热红外（Landsat TIRS B10 与 MODIS 31），VZA = 32° 是为捕捉热点额外加的点；聚合是 GDAL 面积平均，热点定义（300 m P95 / 均值 + 2σ）是人为设定。"),
            B("<b>数值来源。</b>reference/ 的 float16 光学栅格与 results/*.csv 的统计略有舍入差；图由 float16 参考栅格或完整运行输出绘制，表数值一律取自 CSV。"),
            P("5.2　后续工作", "h2"),
            B("<b>城市三维结构。</b>城市三维结构（建筑阴影、墙面、街谷多次散射）效应建议用 HiStream 的面元/体元模式补充，与本案例的单层冠层结果对照。"),
            B("<b>实测对比。</b>与 Landsat 8 B10（及 B11）同日同区的实测亮温或 Level-2 地表温度对比，检验 303.8 K 级的均值与 2.4 K 级的空间标准差；与 MOD11 / MYD11（1 km）地表温度产品对比，检验聚合实验的标准差下降与热点占比变化。"),
            B("<b>大气校正。</b>接入 MODTRAN 查找表后，可由冠层顶量给出各传感器的入瞳辐亮度，使结果直接可与卫星 L1 数据比较。"),
            B("<b>光谱响应函数。</b>以各传感器官方 SRF 文件替代高斯近似，并补充光学 BRDF 角度实验。"),
            B("<b>同日观测。</b>引入同日的 ASTER/ECOSTRESS 过境数据（若存在）及地面站气象，对 ERA5 强迫与反射天空做对照。")]


# 作用：第 6 章 可复现步骤。
def section_repro(data: dict) -> list:
    runs, _ = run_stats(data)
    story = [P("6　可复现步骤", "h1"),
             P("在仓库根目录（Windows PowerShell）依次运行："),
             CODE([f"# 1. 运行 {runs} 次引擎并汇总（输出到指定目录；加 --write-reference 刷新 reference/ 精简结果与 results/*.csv）",
                   "node assets\\examples\\beijing_multisensor\\run_all.mjs --out &lt;输出目录&gt; [--write-reference]",
                   "# 2. 只用仓库内 reference/ 重建全部配图与本报告 PDF",
                   "python tools\\build-beijing-multisensor-report.py",
                   "# 可选：用完整运行输出重画（额外生成 EB 通量图，并刷新亮温-LST 差表与分地类统计表）",
                   "python tools\\build-beijing-multisensor-report.py --source run --run-dir &lt;输出目录&gt;"]),
             Spacer(1, 2 * mm),
             P("在 StreamStudio 界面里可通过“打开示例”→“北京市 · 光学与热红外多传感器观测模拟”复制案例后打开 project.json（母工程）或 variants/ 下各变体；"
               "变体默认读取 ../reference/components.tif，不先跑母工程也能直接成像。run_all.mjs 的 Python 部分需要 numpy 与 osgeo（GDAL ≥ 3.11 才能写 float16 参考栅格）。")]
    return story


# 作用：附录：出处列表（按编号，附使用该出处的传感器与数据依据）。
def appendix_sources(data: dict, refs: dict) -> list:
    users: dict = {}
    for s in data["sensors"]["sensors"]:
        for kind in ("optical", "thermal"):
            for b in s[kind]:
                users.setdefault(b["source"], {"sensors": [], "basis": b["basis"]})
                name = SENSOR_NAMES.get(s["id"], s["name"])
                if name not in users[b["source"]]["sensors"]:
                    users[b["source"]]["sensors"].append(name)
    story = [P("附录　出处", "h1")]
    for url, number in sorted(refs.items(), key=lambda kv: kv[1]):
        info = users[url]
        story.append(Paragraph(f"[{number}]　{url}<br/>　　{'、'.join(info['sensors'])}（{BASIS_LABEL[info['basis']]}）", STY["table"]))
        story.append(Spacer(1, 1 * mm))
    story.append(Spacer(1, 3 * mm))
    story.append(P("以上均为 sensors.json 记录的出处；逐波段的备注见该文件的 note 字段。", "body"))
    return story


# 作用：按 sensors.json 里出处首次出现的顺序给 URL 编号。
def number_sources(data: dict) -> dict:
    refs: dict = {}
    for s in data["sensors"]["sensors"]:
        for kind in ("optical", "thermal"):
            for b in s[kind]:
                refs.setdefault(b["source"], len(refs) + 1)
    return refs


# 作用：加载 plot-beijing-multisensor.py（文件名含连字符，不能直接 import）。
def load_plot_module():
    spec = importlib.util.spec_from_file_location("plot_beijing_multisensor", ROOT / "tools" / "plot-beijing-multisensor.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["plot_beijing_multisensor"] = module
    spec.loader.exec_module(module)
    return module


# 作用：主流程：出图 → 排版 → 输出 PDF，并检查体积。
def main() -> None:
    parser = argparse.ArgumentParser(description="生成北京多传感器案例报告 PDF")
    parser.add_argument("--source", choices=("reference", "run"), default="reference")
    parser.add_argument("--run-dir", type=Path, default=None)
    parser.add_argument("--skip-figures", action="store_true", help="不重画，直接使用 figures/ 里已有 PNG")
    args = parser.parse_args()
    if not args.skip_figures:
        plot = load_plot_module()
        plot.generate_figures(args.source, args.run_dir or plot.DEFAULT_RUN, FIG_DIR)
    names = ["fig01", "fig02_", "fig02b", "fig03", "fig04", "fig05", "fig06", "fig07", "fig08"]
    present = [n for n in names if any(q.name.startswith(n) for q in FIG_DIR.glob("*.png"))]
    FN.update({n: i + 1 for i, n in enumerate(present)})
    data = load_data()
    d = derive(data)
    refs = number_sources(data)
    story = []
    story += cover(data, d)
    story += section_scene(data, d)
    story += section_sensors(data, refs)
    story += section_method(data, d)
    story += section_eb(data, d)
    story += section_optical(data, d)
    story += section_thermal(data, d)
    story += section_angles(data, d)
    story += section_aggregation(data, d)
    story += section_scope(data, d)
    story += section_repro(data)
    story += appendix_sources(data, refs)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    doc = ReportDoc(str(OUTPUT))
    doc.build(story)
    size = OUTPUT.stat().st_size
    print(f"已生成 {OUTPUT}（{size / 1024 / 1024:.2f} MB，{doc.page} 页，图 {Counter.figure} 张，表 {Counter.table} 张）")
    assert size < 15 * 1024 * 1024, "PDF 超过 15 MB"


if __name__ == "__main__":
    main()
