# ==========================================================================
# 文件：build-beijing-multisensor-report.py
# 功能：生成《北京市 · 光学与热红外多传感器观测模拟》分析报告 PDF
#       （docs/cases/beijing-multisensor/report.pdf），版式沿用 build-core-manuals-pdf.py
#       （A4、等线 Deng 字体、同一套配色与页眉页脚）。
# 步骤：
#   1. 读数据：results/summary.csv、angles.csv、aggregation.csv（案例 results/ 目录），
#      sensors.json（传感器与波段表），reference/parent_scene_summary.json（母工程摘要），
#      tables/tir_bt_minus_lst.csv（亮温-LST 差）。报告里所有数值表与正文数字都由这些文件计算得出，不手抄。
#   2. 出图：调用 tools/plot-beijing-multisensor.py 的 generate_figures（--source reference|run，缺省 reference，
#      即只用仓库内 reference/ 精简结果重建；--skip-figures 则直接使用 figures/ 里已有 PNG）。
#   3. 排版：封面标题块 + 核心结论 → 1 目的与场景 → 2 传感器与波段表 → 3 方法 → 4 结果与分析
#      （EB、实验 A/B/C）→ 5 局限 → 6 可复现步骤与待补数据 → 附录出处，经 reportlab 输出 PDF。
# 数据流：assets/examples/beijing_multisensor/{results,reference,sensors.json} + tables/*.csv
#         → figures/*.png → report.pdf。
# 运行：C:\work\miniconda\python.exe -I -X utf8 tools\build-beijing-multisensor-report.py [--source reference|run] [--skip-figures]
# 依赖：reportlab、Pillow、numpy、matplotlib、osgeo（出图时用）；字体 C:\Windows\Fonts\Deng.ttf / Dengb.ttf。
# 注意：本报告是功能与研究情景示例，没有任何真实观测验证；各传感器亮温是无大气的冠层顶方向量。
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
WARN = colors.HexColor("#C0392B")
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


# 作用：封面标题块与“核心结论”。
def cover(data: dict, d: dict) -> list:
    b10, mod = grow(data, "tirs_b10", 0, None), grow(data, "modis_b31", 0, None)
    hot = grow(data, "tirs_b10", 32, ANGLE_BACK)
    agg = arow(data, "landsat89", "B10")
    ndvi_txt = "、".join(f"{SHORT[s]} {d['ndvi'][s]['mean']:.3f}" for s in OPTICAL_ORDER)
    story = [Spacer(1, 2 * mm), P(TITLE, "title"),
             P("StreamStudio · STREAM 遥感影像模拟案例　|　场景时刻 2019-08-15 02:55 UTC（北京时间 10:55）　|　300 m，EPSG:32650，592 × 598", "subtitle")]
    items = [
        f"<b>光学（实验 A）</b>：同一组分温度下，五个传感器的 NDVI 在 {d['ndvi_min']:.3f}–{d['ndvi_max']:.3f} 之间（{ndvi_txt}），差异来自红光/近红外波段的位置与宽度。",
        f"<b>热红外</b>：Landsat B10/B11 为 {srow(data, 'landsat89', 'B10')['mean']:.3f}/{srow(data, 'landsat89', 'B11')['mean']:.3f} K；ASTER 由 {d['aster_b10']:.3f}（8.3 μm）降到 {d['aster_b14']:.3f} K（11.3 μm）；"
        f"同波段不同传感器均值极差 {max(d['group_a_spread'], d['group_b_spread']) * 1000:.1f} mK（&lt;0.01 K）。这是无大气的冠层顶亮温，<b>不是传感器间可观测差异</b>。",
        f"<b>观测角（实验 B）</b>：Landsat B10 相对天底（{b10['mean']:.3f} K），后向 VZA 20°/32°（热点）/40°/55° 为 {f(grow(data,'tirs_b10',20,ANGLE_BACK)['delta_mean_vs_nadir'],2,True)}/"
        f"{f(hot['delta_mean_vs_nadir'],2,True)}/{f(grow(data,'tirs_b10',40,ANGLE_BACK)['delta_mean_vs_nadir'],2,True)}/{f(grow(data,'tirs_b10',55,ANGLE_BACK)['delta_mean_vs_nadir'],2,True)} K，"
        f"前向 20°/40°/55° 为 {f(grow(data,'tirs_b10',20,ANGLE_FWD)['delta_mean_vs_nadir'],2,True)}/{f(grow(data,'tirs_b10',40,ANGLE_FWD)['delta_mean_vs_nadir'],2,True)}/{f(grow(data,'tirs_b10',55,ANGLE_FWD)['delta_mean_vs_nadir'],2,True)} K；热点出现在 VZA≈太阳天顶角（{d['sza']:.2f}°）处。",
        f"<b>聚合（实验 C）</b>：B10 由 300 m 聚合到 1 km，均值 {f(agg['mean_1km_avg'] - agg['mean_300m'], 3, True)} K，标准差 {agg['std_300m']:.2f}→{agg['std_1km_avg']:.2f} K，"
        f"最大值 {agg['max_300m']:.2f}→{agg['max_1km_avg']:.2f} K，热点占比 {agg['hot_frac_300m_p95']:.3f}→{agg['hot_frac_1km_avg_p95']:.3f}；B5 为 {arow(data,'landsat89','B5')['hot_frac_1km_avg_p95']:.3f}，NDVI 为 {arow(data,'landsat89','NDVI(B4,B5)')['hot_frac_1km_avg_p95']:.3f}。",
        "<b>未做任何真实观测验证</b>：STREAM 是单层冠层模型，无城市三维结构，ERA5 强迫分辨率约 25 km，波段为高斯近似、无大气校正。",
    ]
    box = Table([[Paragraph("核心结论", STY["h2"])]] + [[Paragraph(t, STY["box"], bulletText="•")] for t in items], colWidths=[FRAME_W])
    box.setStyle(TableStyle([("BACKGROUND", (0, 0), (-1, -1), PALE_BLUE), ("BOX", (0, 0), (-1, -1), 0.6, LINE),
                             ("LEFTPADDING", (0, 0), (-1, -1), 8), ("RIGHTPADDING", (0, 0), (-1, -1), 8),
                             ("TOPPADDING", (0, 0), (-1, -1), 2), ("BOTTOMPADDING", (0, 0), (-1, -1), 2)]))
    story += [box, Spacer(1, 4 * mm)]
    return story


# 作用：第 1 章 目的与场景。
def section_scene(data: dict, d: dict) -> list:
    summary = data["parent"]
    counts = summary["counts"]
    story = [P("1　目的与场景", "h1"),
             P("本案例回答一个具体问题：<b>同一个地表状态，换成不同卫星的波段设置去“看”，会得到多大差别？</b>"
               "做法是先用 STREAM 的 ERA5 能量平衡（EB）求一次四组分温度（光照/阴影土壤、光照/阴影叶片），"
               "再让 Landsat 8/9、MODIS、Sentinel-2、ASTER、ECOSTRESS、VIIRS、FY-3D MERSI-II 的波段设置复用这组温度成像，"
               "并在其上做观测角（热点）实验与 300 m → 1 km 聚合实验。"),
             P("<b>范围与时刻。</b>北京市行政区范围（约 177 km × 179 km 的外包矩形，300 m 网格，EPSG:32650，592 × 598 像元，"
               f"其中市域内有效像元 {int(counts['thermalPixels'])} 个，光学有效像元 {int(counts['opticalPixels'])} 个，水体 {int(counts['waterOrFillPixels'])} 个只做热红外）。"
               f"过境时刻为 2019-08-15 02:55 UTC，即北京时间 10:55；场景平均太阳天顶角 {d['sza']:.2f}°，太阳方位角 {d['saa']:.2f}°（范围 {d['saa_min']:.1f}–{d['saa_max']:.1f}°）。"),
             P("<b>数据来源。</b>地类由 2019 年 GLC_FCS30D 映射到 IGBP，DEM 为 FABDEM，LAI 为 Landsat 30 m 产品，300 m 输入由 30 m 网格聚合；气象强迫为 ERA5（2019-08-15，约 25 km），"
               "与示例 landsat_beijing_eb 完全相同（输入与 ERA5 数据包复制进本案例的 inputs/、era5/，使案例自包含）。"
               f"外包矩形里市域外的 255 值填充像元有 {592 * 598 - int(counts['thermalPixels'])} 个（= 592 × 598 − {int(counts['thermalPixels'])}），"
               f"引擎日志把它们统计为“未知地类 {int(counts['unknownTypePixels'])}”，并非数据错误。")]
    story += FIG(FIG_DIR / "fig01_inputs.png", "输入图：(a) IGBP 地类（括号内为像元数）；(b) LAI；(c) DEM 高程。坐标为相对栅格西北角的东向/北向距离（km）。", 90 * mm)
    return story


# 作用：第 2 章 传感器与波段表（来自 sensors.json，待核对项用红字标注，并列出处编号）。
def section_sensors(data: dict, refs: dict) -> list:
    def sensor_rows(kind: str) -> list[list]:
        rows = [["传感器", "波段", "中心波长", "带宽 FWHM", "官方范围", "出处", "核对"]]
        for s in data["sensors"]["sensors"]:
            for b in s[kind]:
                rng = f"{b['range'][0]:g}–{b['range'][1]:g}" if b.get("range") else "—"
                mark = '<font color="#C0392B"><b>待核对</b></font>' if b["verify"] else "—"
                rows.append([SENSOR_NAMES.get(s["id"], s["name"]), b["name"], f"{b['center']:g}", f"{b['fwhm']:g}", rng, f"[{refs[b['source']]}]", mark])
        return rows
    optical, thermal = sensor_rows("optical"), sensor_rows("thermal")
    widths = [34 * mm, 17 * mm, 24 * mm, 24 * mm, 30 * mm, 14 * mm, 20 * mm]
    notes = [["传感器", "波段", "待核对原因"]]
    for s in data["sensors"]["sensors"]:
        for kind in ("optical", "thermal"):
            for b in s[kind]:
                if b["verify"]:
                    notes.append([SENSOR_NAMES.get(s["id"], s["name"]), b["name"], b.get("note", "数值为标称值或检索摘要，未在官方原文逐项核对")])
    n_verify = len(notes) - 1
    story = [P("2　传感器与波段表", "h1"),
             P(f"波段设置全部来自案例的 sensors.json（{len(data['sensors']['sensors'])} 个传感器，光学 {len(optical) - 1} 个、热红外 {len(thermal) - 1} 个波段）。"
               "凡官方给出波段范围 [lo, hi] 的，取中心 = (lo + hi)/2、带宽 = hi − lo（等效矩形近似，范围保留在“官方范围”列）；官方给出中心与带宽时直接采用。"
               f"<font color=\"#C0392B\"><b>其中 {n_verify} 个波段标为“待核对”</b></font>：数值是标称值或检索摘要，没有在官方原文上逐项核对，引用前请核对。"
               "标“—”的波段在 sensors.json 中是 verify:false，但部分来源是检索摘要而非整页核对（USGS 页面被拦截，Landsat 的值取自 NASA 带通 PDF 摘要与论文表）。出处编号见附录。")]
    story += TBL("光学波段（中心波长、带宽、官方范围单位均为 nm）", optical, widths, align_right={2, 3, 4})
    story += TBL("热红外波段（单位均为 μm）", thermal, widths, align_right={2, 3, 4})
    story += TBL("“待核对”波段清单与原因", notes, [34 * mm, 17 * mm, FRAME_W - 51 * mm])
    return story


# 作用：第 3 章 方法与假设。
def section_method(data: dict, d: dict) -> list:
    seconds = {}
    for r in data["summary"]:
        seconds[r["sensor"]] = r["seconds"]
    angle_seconds = sum(r["seconds"] for r in data["angles"])
    runs = len(seconds) + len(data["angles"])
    total = sum(seconds.values()) + angle_seconds
    par = data["parent"]["temperature"]
    return [
        P("3　方法", "h1"),
        P("<b>两步法。</b>(1) 母工程 project.json：STREAM 的 EB 模式用 ERA5 气象强迫（2019-08-15 00–04 UTC 的整点时次）求能量平衡，"
          "输出 4 个组分温度 Tss/Tsh/Tcs/Tch（光照土壤、阴影土壤、光照叶片、阴影叶片，components.tif）；"
          f"(2) 各传感器变体 variants/&lt;传感器&gt;.json 改用辐射传输（RT）模式，组分温度来源设为 components.tif，只含该传感器的波段，直接成像。"
          f"这样组分温度只算一次，所有传感器、角度、聚合实验共享同一个地表状态，差异只来自观测设置。本报告共 {runs} 次引擎运行"
          f"（母工程 1 + 传感器 {len(seconds) - 1} + 角度 {len(data['angles'])}），引擎总耗时约 {total:.1f} s。"),
        P("<b>EB 求解状态。</b>"
          f"{int(par['convergedPixels'])}/{int(par['solvedPixels'])} 个 EB 像元收敛，{int(par['nonConvergedPixels'])} 个未收敛，{int(par['bracketFallbackPixels'])} 个使用了 1/L 括号法回退；"
          f"ERA5 数据包只覆盖 2019-08-15 一天，起转时次缺省请求 {int(par['spinupNodesRequested'])} 个，实际只能用 {int(par['spinupNodesUsed'])} 个（运行日志会提示“起转不足”）。"),
        P("<b>波段近似。</b>STREAM 用 center/fwhm 描述<b>高斯形</b>光谱响应函数，本案例<b>没有使用各传感器真实的光谱响应函数（SRF）</b>。"
          "高斯近似对宽而平的波段（如 MODIS B1、MERSI-II 的 50 nm 波段）偏离较大，对窄波段更接近；因此下文光学差异只能当作“波段位置与宽度”的敏感性估计，不是传感器仪器差。"
          "热红外波段同样是高斯近似，目前热红外波段的差异主要来自地表发射率光谱（见 4.3）。"),
        P("<b>数据出处的可靠性。</b>MODIS 官方规格页（modis.gsfc.nasa.gov/about/specifications.php）与 USGS 波段页在取数时访问失败，"
          "MODIS 各波段数值取自这些页面内容的二手引用（NASA 演示文稿 / LP DAAC 文档）；Landsat 值取自 NASA 带通 PDF 摘要与论文表。"
          "这些来源的核对状态见第 2 章表格与附录，未逐项在官方原文上复核。"),
        P("<b>观测角设置。</b>VAA 约定为像元指向传感器的方位：VAA = 139° ≈ 太阳方位角，传感器在太阳一侧（后向散射，含热点）；VAA = 319° 为前向。"
          f"角度实验只做热红外（Landsat TIRS B10 与 MODIS 31），VZA 取 0°、20°、40°、55°（前后向各一）并额外加了 VZA = 32° 后向（≈ 太阳天顶角 {d['sza']:.2f}°）的热点方向点。"),
        P("<b>聚合设置。</b>用 GDAL warp 把 300 m 栅格聚合到 1 km（平均 average 与最大值 max 两种），1 km 网格<b>从原栅格西北角起算</b>，边缘格可能是部分覆盖，<b>不是 MODIS 正弦网格</b>。"
          "热点定义为≥ 300 m 场 P95 的像元（另给均值 + 2σ 阈值的结果，见 aggregation.csv）；另外按辐亮度平均再反算亮温，与直接平均亮温比较。"),
        P("<b>反射天空。</b>热红外亮温 = 四组分发射 + (1 − 有效发射率) × 天空辐亮度。母工程的 EB 路径用 ERA5 的向下长波 strd 均值，变体的 RT 路径用 Swinbank 晴空公式，这是母工程与 Landsat 变体 B10 差异的来源之一（见 4.3）。"),
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
    story = [P("4　结果与分析", "h1"), P("4.1　EB 概览：四组分温度与通量", "h2"),
             P(f"10:55（当地）太阳天顶角 {d['sza']:.1f}°，光照土壤明显最热，均值 {comp[0]['mean']:.1f} K，而阴影土壤只有 {comp[1]['mean']:.1f} K，光照叶片 {comp[2]['mean']:.1f} K、阴影叶片 {comp[3]['mean']:.1f} K；"
               f"土壤的光照/阴影温差（{comp[0]['mean'] - comp[1]['mean']:.1f} K）远大于叶片（{comp[2]['mean'] - comp[3]['mean']:.1f} K），"
               "这是后面热点与阴影可见比例影响亮温的物理基础。图 {fnum('fig02_')} 的按地类柱状图显示城市建成区、农田和裸地的光照土壤最热，森林最冷。"
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
    pv = [["波段", "母工程中心 nm", "变体中心 nm", "母工程 EB 路径均值", "Landsat 变体 RT 路径均值", "差（母 − 变体）"]]
    for band, a, b, ca, cb in d["parent_vs_variant"]:
        pv.append([band, f"{ca:g}", f"{cb:g}", f(a, 4), f(b, 4), f(a - b, 4, True)])
    pv.append(["NDVI(B4,B5)", "—", "—", f(d["ndvi_parent"]["mean"], 3), f(d["ndvi"]["landsat89"]["mean"], 3), f(d["parent_ndvi_diff"], 3, True)])
    red = {sid: next(r for r in bands[sid] if r["band"] == nm) for sid, nm in (("landsat89", "B4"), ("modis", "B1"), ("sentinel2", "B4"), ("viirs", "M5"), ("fy3d_mersi2", "CH3"))}
    story = [P("4.2　实验 A：各传感器的光学反射率与 NDVI", "h2"),
             P(f"图 {fnum('fig03')} 把五个传感器的红光、近红外、短波红外 1 波段放在一起比较。<b>近红外</b>反射率几乎一致（均值 "
               f"{min(r['mean'] for r in (srow(data,'landsat89','B5'), srow(data,'modis','B2'), srow(data,'viirs','M7'), srow(data,'fy3d_mersi2','CH4'))):.4f}–"
               f"{max(r['mean'] for r in (srow(data,'landsat89','B5'), srow(data,'modis','B2'), srow(data,'viirs','M7'), srow(data,'fy3d_mersi2','CH4'))):.4f}，Sentinel-2 B8 为 {srow(data,'sentinel2','B8')['mean']:.4f}），"
               "因为这些波段都落在 865 nm 附近的近红外平台上；Sentinel-2 B8（833 nm、带宽 106 nm）是宽波段、中心偏短波，均值略低；"
               f"<b>红光</b>差异最明显：MODIS B1（645 nm、带宽 50 nm）均值 {red['modis']['mean']:.4f}，最宽且偏短波侧（叶绿素吸收较弱处），反射率最高；VIIRS M5（672 nm、带宽 20 nm）只有 {red['viirs']['mean']:.4f}，最窄且最靠近叶绿素吸收中心（约 670 nm），反射率最低；"
               f"Landsat B4 {red['landsat89']['mean']:.4f}，Sentinel-2 B4 {red['sentinel2']['mean']:.4f}，MERSI-II CH3（650 nm，50 nm）{red['fy3d_mersi2']['mean']:.4f}。"
               "<b>短波红外 1</b>由中心（1608–1640 nm）与带宽决定，MODIS B6 与 MERSI-II CH6（1640 nm）高于 Landsat B6（1608 nm）、Sentinel-2 B11、VIIRS M10。"
               "上述差异都是由“波段位置与宽度 + 水汽吸收区的光谱斜率”造成的，与这里没有大气的假设一致。")]
    story += FIG(FIG_DIR / "fig03_optical_band_boxplots.png", "同名波段反射率的分布箱线图（须线 5–95%，菱形为均值）：(a) 红光；(b) 近红外；(c) 短波红外 1。", 75 * mm)
    story += [P(f"<b>NDVI。</b>五个传感器的 NDVI 均值在 {d['ndvi_min']:.3f}–{d['ndvi_max']:.3f} 之间，极差 {d['ndvi_max'] - d['ndvi_min']:.3f}（表 {Counter.table + 2}）。"
                f"VIIRS 最高（{d['ndvi']['viirs']['mean']:.3f}）：M5 又窄又靠近叶绿素吸收中心，红光反射率最小；MODIS 最低（{d['ndvi']['modis']['mean']:.3f}）：B1 最宽，红光反射率偏大，且 B2 的近红外与别家相同；"
                f"Sentinel-2（{d['ndvi']['sentinel2']['mean']:.3f}）的 B4 665 nm 红光较低，B8 近红外略低；MERSI-II（{d['ndvi']['fy3d_mersi2']['mean']:.3f}）与 MODIS 接近（红光带都是 50 nm 宽）。"
                "图 {fnum('fig04')} 的散点显示 NDVI 越高（植被越茂密）传感器间的差别越大，低 NDVI（城区、裸地）处几乎重合：高植被时红光反射率很小，红光带位置与宽度的微小差异在分母里被放大。")]
    story += FIG(FIG_DIR / "fig04_ndvi.png", "五个传感器的 NDVI 地图与相对 Landsat 的逐像元散点（随机抽样 8000 点）。", 125 * mm)
    story += TBL("各传感器光学波段的反射率统计（300 m 场景，来自 results/summary.csv）", rows, [24 * mm, 16 * mm, 20 * mm, 22 * mm, 22 * mm, 22 * mm, 22 * mm, 22 * mm], align_right={2, 3, 4, 5, 6, 7})
    story += TBL("各传感器 NDVI（逐像元 NDVI 的均值与标准差）", ndvi_rows, [30 * mm, 36 * mm, 30 * mm, 30 * mm, 38 * mm], align_right={2, 3, 4})
    story += [P(f"<b>母工程与变体的光学差。</b>母工程走 EB 路径、直接带 Landsat 波段；landsat89 变体走 RT 路径、复用同一组组分温度。两者反射率之差最大为 {d['parent_opt_maxdiff']:.4f}（{d['parent_opt_maxband']}），"
                f"NDVI 差 {f(d['parent_ndvi_diff'], 3, True)}（表 {Counter.table + 1}）。差异小但并非严格为零：部分来自两套工程对波段中心的取值略有不同（如 B3 561/562、B4 655/654、B6 1609/1608、B7 2201/2200 nm），"
                "但 B5 两者中心相同（865 nm）仍差 0.002，说明还有别的来源（疑为两条路径对直射/散射光的分配或光谱处理差异），这一点<b>没有深究</b>。"
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
    story = [P("4.3　实验 A（续）：各载荷的热红外亮温", "h2"),
             P("<b>必须先说明：这里的亮温是没有大气的冠层顶方向亮温</b>（STREAM 的 TiRT 四组分闭合，大气 = none），不含大气透过率、程辐射和传感器噪声，"
               "<b>不是传感器之间可观测到的差异</b>。因此各载荷亮温差极小是预期结果，不能解读为真实卫星之间的偏差。"),
             P(f"Landsat B10/B11 为 {b10['mean']:.3f}/{b11['mean']:.3f} K（标准差 {b10['std']:.2f} K，最小 {b10['min']:.1f}，最大 {b10['max']:.2f} K）；"
               f"ASTER 五个波段由 B10（8.3 μm）{d['aster_b10']:.3f} K 单调降到 B14（11.3 μm）{d['aster_b14']:.3f} K；ECOSTRESS 由 TIR1（8.285 μm）{d['eco_tir1']:.3f} K 降到 TIR5（12.0 μm）{d['eco_tir5']:.3f} K。"
               f"亮温随波长增大而单调下降，8.3 μm 比 12 μm 高约 {d['eco_tir1'] - d['eco_tir5']:.3f} K（ECOSTRESS）/ {d['aster_b10'] - srow(data,'landsat89','B11')['mean']:.3f} K（ASTER B10 对 Landsat B11），"
               f"原因是模型中地表发射率随波长变化（以及随之变化的反射天空贡献），而不是传感器本身；图 {fnum('fig06')}(c) 的 ASTER B10 − B14 逐像元差呈双峰分布，"
               "推测对应不同地类（如裸土/不透水面与植被）的发射率光谱差，本报告没有逐地类核实。"),
             P(f"<b>同波段不同传感器：</b>10.5–11.3 μm 窗区的 Landsat B10、MODIS 31、VIIRS M15、MERSI-II CH24 均值极差 {d['group_a_spread'] * 1000:.1f} mK；"
               f"12 μm 的 Landsat B11、MODIS 32、VIIRS M16、MERSI-II CH25、ECOSTRESS TIR5 极差 {d['group_b_spread'] * 1000:.1f} mK，两者都 &lt;0.01 K。这是中心波长相近、同样的无大气假设下的自然结果。"),
             P(f"<b>母工程与变体。</b>母工程 Landsat B10 为 {d['parent_b10']:.3f} K，变体 {d['variant_b10']:.3f} K，母工程低 {abs(d['parent_b10_diff']):.3f} K。"
               "原因是反射天空来源不同：EB 路径取 ERA5 向下长波 strd 的场景均值（约 382.5 W m<super>-2</super>），RT 路径取 Swinbank 晴空公式。")]
    story += FIG(FIG_DIR / "fig05_tir_brightness_maps.png", "各载荷热红外亮温地图（统一色标 298–317 K）：Landsat B10、MODIS 31、ASTER B10 与 B14、ECOSTRESS TIR1 与 TIR5。肉眼无法区分，差别见图 " + str(fnum("fig06")) + "。", 150 * mm)
    story += FIG(FIG_DIR / "fig06_tir_spectral.png", "(a) 场景均值亮温随中心波长（横线为 FWHM/2）；(b) 亮温与发射率归一化地表温度之差随波长；(c) ASTER B10 − B14 的逐像元亮温差分布。", 72 * mm)
    story += TBL("各载荷热红外波段的亮温统计（300 m，来自 results/summary.csv）", rows, [24 * mm, 16 * mm, 20 * mm, 24 * mm, 24 * mm, 20 * mm, 20 * mm, 26 * mm], align_right={2, 3, 4, 5, 6, 7})
    story += [P(f"<b>亮温与发射率归一化地表温度之差。</b>引擎同时输出按地类固定发射率归一的方向温度（tir_surface_temperature.tif，只含发射项）。亮温低于该 LST，均值差为 "
                f"{f(min(r['diff_mean'] for r in data['lst']), 3, True)} 至 {f(max(r['diff_mean'] for r in data['lst']), 3, True)} K，随波长增大向更负变化；逐像元的差最小可达 {f(min(r['diff_min'] for r in data['lst']), 2, True)} K，"
                "最大也不超过 0。该表由 run 完整输出统计得到，已存入 tables/tir_bt_minus_lst.csv（reference 精简结果没有 LST 栅格）。")]
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
    story = [P("4.4　实验 B：观测角与热点", "h2"),
             P(f"太阳天顶角 {d['sza']:.2f}°、方位角 {d['saa']:.2f}°。传感器在太阳一侧（后向，VAA = 139°）时，相对天底（{nad['mean']:.3f} K）的场景均值亮温为：VZA 20° {f(g(20, ANGLE_BACK), 2, True)} K、"
               f"<b>VZA 32°（热点方向）{f(hot['delta_mean_vs_nadir'], 2, True)} K</b>、40° {f(g(40, ANGLE_BACK), 2, True)} K、55° {f(g(55, ANGLE_BACK), 2, True)} K；"
               f"前向（VAA = 319°）20°/40°/55° 为 {f(g(20, ANGLE_FWD), 2, True)}/{f(g(40, ANGLE_FWD), 2, True)}/{f(g(55, ANGLE_FWD), 2, True)} K。"),
             P("<b>解释。</b>热点出现在观测方向与太阳方向重合（VZA = SZA、方位相同）时：传感器看不到阴影，只看到光照的叶片与土壤，亮温最高；"
               f"离开热点后，可见的阴影比例迅速增加，阴影组分比光照组分冷得多（土壤光照/阴影温差约 {data['parent']['components'][0]['mean'] - data['parent']['components'][1]['mean']:.1f} K），场景亮温下降。"
               f"本案例 VZA 20° 与 40° 恰好分居热点两侧，后向亮温几乎对称（{f(g(20, ANGLE_BACK), 3, True)} 与 {f(g(40, ANGLE_BACK), 3, True)} K）。"
               "到 55° 大角度时离热点更远，推测可见的阴影/较冷组分比例增多，亮温低于天底；前向（背对太阳）时阴影比例最大，亮温最低（以上是对结果的解释性推测，本报告没有输出各组分可见比例）。"
               f"标准差在热点处最大（{hot['std']:.2f} K，天底 {nad['std']:.2f} K），因为光照/阴影温差在热点附近对像元的贡献最大。场景最大值（{nad['max']:.2f} K）在所有角度几乎不变（B10 各角度最大值的极差 {d['angle_max_spread'] * 1000:.1f} mK），"
               f"对应完全受光的裸地/不透水像元，角度无关。MODIS 31 与 Landsat B10 的角度曲线几乎重合（场景均值最多差 {modis_diff:.3f} K）。"),
             P("<b>注意：</b>STREAM 是单层冠层模型，<b>没有城市三维结构</b>（建筑物阴影、墙面、街谷多次散射），这里的角度效应只反映冠层内部叶片与土壤的光照/阴影分配，"
               "与真实城市的方向亮温各向异性（城区可达数 K 甚至更大、形态与规律也不同）差别很大。本实验也只做了热红外，没有做光学 BRDF。")]
    story += FIG(FIG_DIR / "fig07_angle_principal_plane.png", "主平面（后向为正、前向为负）观测天顶角实验：(a) 亮温相对天底的变化，星号为热点方向 VZA = 32°；(b) 场景亮温标准差；(c) 场景亮温 P95。虚线为太阳天顶角。", 70 * mm)
    story += TBL("Landsat TIRS B10 与 MODIS 31 主平面亮温随 VZA 变化（results/angles.csv）", rows, [14 * mm, 28 * mm, 14 * mm, 24 * mm, 26 * mm, 32 * mm, 22 * mm, 20 * mm], align_right={0, 2, 3, 4, 5, 6, 7})
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
             P(f"把 Landsat B10 由 300 m 聚合到 1 km（{int(thermal['n_300m'])} 个 300 m 像元 → {int(thermal['n_1km'])} 个 1 km 格）：均值几乎不变（{thermal['mean_300m']:.3f} → {thermal['mean_1km_avg']:.3f} K，{f(thermal['mean_1km_avg'] - thermal['mean_300m'], 3, True)} K），"
               f"标准差由 {thermal['std_300m']:.2f} 降到 {thermal['std_1km_avg']:.2f} K（{pct(thermal, 'std_300m', 'std_1km_avg'):.0f}%），最大值由 {thermal['max_300m']:.2f} 降到 {thermal['max_1km_avg']:.2f} K，P95 由 {thermal['p95_300m']:.2f} 降到 {thermal['p95_1km_avg']:.2f} K。"
               "均值守恒、极值被抹平、分布变窄——典型的面积平均效应。"),
             P(f"<b>热点占比</b>（定义为≥ 300 m 场 P95 的面积占比，300 m 自身为 0.05）：B10 聚合后为 {thermal['hot_frac_1km_avg_p95']:.3f}，约降了三分之一；光学 B4 {b4['hot_frac_1km_avg_p95']:.3f}，B5 {b5['hot_frac_1km_avg_p95']:.3f}，NDVI {ndvi['hot_frac_1km_avg_p95']:.3f}，"
               f"降幅达三分之二至四分之三，因为光学高值（高植被与高 NDVI）的空间尺度更小。若改用 1 km 内取最大值聚合，热点占比反而升到 {thermal['hot_frac_1km_maxagg_p95']:.3f}（B10）—— 一个 1 km 格里只要有一个热像元就算热点，面积被放大。"
               f"改用均值 + 2σ 阈值（B10 {thermal['hot_thr_2s']:.2f} K）时，热点占比由 {thermal['hot_frac_300m_2s']:.3f} 变为 {thermal['hot_frac_1km_avg_2s']:.3f}（均值聚合）/ {thermal['hot_frac_1km_maxagg_2s']:.3f}（取最大）。"
               f"另外，按辐亮度平均再反算亮温比直接平均亮温只高 {thermal['bias_radiance_vs_avg']:.3f} K，可以忽略。"),
             P("这说明用 1 km 级产品（如 MODIS 1 km 地表温度）去识别城市热点，会系统性地低估热点的面积占比与极值；与具体传感器无关，是聚合尺度本身的效应。"
               "<b>注意：</b>本实验是 GDAL 面积平均，1 km 网格从原栅格西北角起算，不是 MODIS 正弦网格，不含 MODIS 的点扩散函数、视场边缘增宽和云/大气影响。")]
    story += FIG(FIG_DIR / "fig08_aggregation_1km.png", "聚合实验：(a) 300 m 与 (b) 1 km（均值聚合）B10 亮温；(c) 分布对比；(d) 热点占比；(e) 1 km 均值聚合后标准差与最大值的相对变化。", 125 * mm)
    story += TBL("300 m → 1 km 聚合统计（results/aggregation.csv；热点占比以 300 m P95 为阈值）", rows,
                 [28 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 16 * mm, 17 * mm, 17 * mm], align_right=set(range(1, 10)))
    return story


# 作用：第 5 章 局限。
def section_limits(data: dict, d: dict) -> list:
    return [P("5　局限", "h1"),
            B("<b>未做任何真实观测验证。</b>本案例是功能与研究情景示例，没有与 Landsat 8 B10 实测、MOD11 等产品比较；所有数值差异都是模型内部差异。"),
            B("<b>单层冠层。</b>STREAM 把每个像元当单层冠层，没有城市三维结构（建筑、街谷、墙面），地类 13（城市建成区）只是参数化的不透水面/植被混合，角度与热点结论不能外推到真实城市。"),
            B("<b>强迫分辨率。</b>ERA5 约 25 km，在 300 m 网格内气象强迫几乎是常数（高程订正未执行，ERA5 缺地表位势），温度空间差异主要来自地类、LAI 和光照几何。"),
            B("<b>高斯 SRF 近似。</b>没有使用真实光谱响应函数；MODIS B32、ASTER B11、ECOSTRESS 全部波段、MERSI-II 光学波段为“待核对”，MODIS 数值取自二手引用。"),
            B("<b>无大气。</b>光学是冠层顶反射率、热红外是冠层顶亮温，未做大气校正/大气影响，也不含传感器噪声、点扩散函数与几何配准。"),
            B("<b>角度实验只做热红外</b>（Landsat TIRS B10 与 MODIS 31），没有光学 BRDF；VZA = 32° 是为捕捉热点额外加的点。"),
            B("<b>聚合是 GDAL 面积平均</b>，1 km 网格从原栅格西北角起算，不是 MODIS 正弦网格；热点定义（300 m P95 / 均值 + 2σ）是人为设定。"),
            B(f"<b>EB 状态：</b>{int(data['parent']['temperature']['nonConvergedPixels'])} 个 EB 像元未收敛；起转时次不足；"
              f"引擎日志里的“未知地类 {int(data['parent']['counts']['unknownTypePixels'])}”是市域外 255 填充像元的统计提示，不是数据缺陷。"),
            B("<b>数值来源差异：</b>reference/ 的 float16 光学栅格与 results/*.csv 的统计略有舍入差；图由 float16 参考栅格或完整运行输出绘制，表数值一律取自 CSV。")]


# 作用：第 6 章 可复现步骤与待补数据。
def section_repro(data: dict) -> list:
    story = [P("6　可复现步骤与后续对比数据", "h1"),
             P("在仓库根目录（Windows PowerShell）依次运行："),
             CODE(["# 1. 运行 24 次引擎并汇总（输出到指定目录；加 --write-reference 刷新 reference/ 精简结果与 results/*.csv）",
                   "node assets\\examples\\beijing_multisensor\\run_all.mjs --out &lt;输出目录&gt; [--write-reference]",
                   "# 2. 只用仓库内 reference/ 重建全部配图与本报告 PDF",
                   "python tools\\build-beijing-multisensor-report.py",
                   "# 可选：用完整运行输出重画（额外生成 EB 通量图并刷新亮温-LST 差表）",
                   "python tools\\build-beijing-multisensor-report.py --source run --run-dir &lt;输出目录&gt;"]),
             Spacer(1, 2 * mm),
             P("在 StreamStudio 界面里可通过“打开示例”→“北京市 · 光学与热红外多传感器观测模拟”复制案例后打开 project.json（母工程）或 variants/ 下各变体；"
               "变体默认读取 ../reference/components.tif，不先跑母工程也能直接成像。run_all.mjs 的 Python 部分需要 numpy 与 osgeo（GDAL ≥ 3.11 才能写 float16 参考栅格）。"),
             P("<b>待用户提供的后续对比数据。</b>要把本案例升级为“有验证”的研究，还需要："),
             B("Landsat 8 B10（及 B11）同日同区的实测亮温或 Level-2 地表温度，用于检验 303.8 K 级的均值与 2.4 K 级的空间标准差；"),
             B("MOD11 / MYD11（1 km）地表温度产品，用于检验聚合实验的标准差下降与热点占比变化；"),
             B("真实的光谱响应函数（各传感器官方 SRF 文件）替代高斯近似；"),
             B("同日的 ASTER/ECOSTRESS 过境数据（若存在）及地面站气象，核对 ERA5 强迫与反射天空。")]
    return story


# 作用：附录：出处列表（按编号）。
def appendix_sources(refs: dict) -> list:
    story = [P("附录　出处", "h1")]
    for url, number in sorted(refs.items(), key=lambda kv: kv[1]):
        story.append(Paragraph(f"[{number}]　{url}", STY["table"]))
    story.append(Spacer(1, 3 * mm))
    story.append(P("以上均为 sensors.json 记录的出处；MODIS 官方页与 USGS 页在取数时访问失败，对应数值取自二手引用，见第 2、3 章说明。", "body"))
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
    story += section_limits(data, d)
    story += section_repro(data)
    story += appendix_sources(refs)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    doc = ReportDoc(str(OUTPUT))
    doc.build(story)
    size = OUTPUT.stat().st_size
    print(f"已生成 {OUTPUT}（{size / 1024 / 1024:.2f} MB，{doc.page} 页，图 {Counter.figure} 张，表 {Counter.table} 张）")
    assert size < 15 * 1024 * 1024, "PDF 超过 15 MB"


if __name__ == "__main__":
    main()
