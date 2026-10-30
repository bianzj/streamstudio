from __future__ import annotations

import json
import math
from pathlib import Path
from zipfile import ZipFile

from PIL import Image as PILImage
from PIL import ImageDraw, ImageFont, ImageOps
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate,
    Flowable,
    Frame,
    Image,
    KeepTogether,
    PageBreak,
    PageTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)


ROOT = Path(__file__).resolve().parents[1]
TMP = ROOT / "tmp" / "pdfs" / "stream_studio_example_manual"
OUTPUT = ROOT / "output" / "pdf" / "Stream Studio功能与案例手册.pdf"
TMP.mkdir(parents=True, exist_ok=True)
OUTPUT.parent.mkdir(parents=True, exist_ok=True)

FONT_REGULAR = Path(r"C:\Windows\Fonts\Deng.ttf")
FONT_BOLD = Path(r"C:\Windows\Fonts\Dengb.ttf")
PIL_FONT = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")
pdfmetrics.registerFont(TTFont("StreamCN", str(FONT_REGULAR)))
pdfmetrics.registerFont(TTFont("StreamCN-Bold", str(FONT_BOLD)))
pdfmetrics.registerFontFamily(
    "StreamCN", normal="StreamCN", bold="StreamCN-Bold",
    italic="StreamCN", boldItalic="StreamCN-Bold"
)

PAGE_W, PAGE_H = A4
NAVY = colors.HexColor("#10273D")
BLUE = colors.HexColor("#2674D9")
GREEN = colors.HexColor("#1B9C73")
MINT = colors.HexColor("#E7F7F1")
PALE_BLUE = colors.HexColor("#EAF2FD")
AMBER = colors.HexColor("#F0A542")
TEXT = colors.HexColor("#23384B")
MUTED = colors.HexColor("#65798B")
LINE = colors.HexColor("#DCE5ED")


def pil_font(size: int, bold: bool = False):
    source = FONT_BOLD if bold else PIL_FONT
    return ImageFont.truetype(str(source), size=size)


def parse_obj(path: Path):
    vertices = []
    vertex_colors = []
    triangles = []
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = raw.strip().split()
        if not parts:
            continue
        if parts[0] == "v" and len(parts) >= 4:
            vertices.append(tuple(float(value) for value in parts[1:4]))
            if len(parts) >= 7:
                vertex_colors.append(tuple(max(0.0, min(1.0, float(value))) for value in parts[4:7]))
            else:
                vertex_colors.append(None)
        elif parts[0] == "f" and len(parts) >= 4:
            indices = []
            for token in parts[1:]:
                index = int(token.split("/")[0])
                indices.append(index - 1 if index > 0 else len(vertices) + index)
            for offset in range(1, len(indices) - 1):
                triangles.append((indices[0], indices[offset], indices[offset + 1]))
    return vertices, vertex_colors, triangles


def transform_point(point, scale=1.0, rotation=0.0, translation=(0.0, 0.0, 0.0)):
    x, y, z = point
    angle = math.radians(rotation)
    cosine, sine = math.cos(angle), math.sin(angle)
    return (
        (x * cosine - z * sine) * scale + translation[0],
        y * scale + translation[1],
        (x * sine + z * cosine) * scale + translation[2],
    )


def render_obj_preview(
    source: Path,
    destination: Path,
    title: str,
    subtitle: str,
    palette: tuple[int, int, int],
    instances=None,
    max_triangles: int = 60000,
):
    vertices, vertex_colors, faces = parse_obj(source)
    instance_specs = instances or [(1.0, 0.0, (0.0, 0.0, 0.0))]
    total_triangles = len(faces) * len(instance_specs)
    step = max(1, math.ceil(total_triangles / max_triangles))
    rendered = []
    all_points = []
    sequence = 0
    for scale, rotation, translation in instance_specs:
        transformed = [transform_point(point, scale, rotation, translation) for point in vertices]
        all_points.extend(transformed)
        for face in faces:
            if sequence % step == 0:
                points = [transformed[index] for index in face]
                colors_for_face = [vertex_colors[index] for index in face if vertex_colors[index] is not None]
                rendered.append((points, colors_for_face))
            sequence += 1

    width, height = 1400, 760
    canvas = PILImage.new("RGB", (width, height), (244, 248, 251))
    draw = ImageDraw.Draw(canvas)
    for y in range(height):
        ratio = y / max(1, height - 1)
        color = tuple(int(238 + 12 * ratio) for _ in range(3))
        draw.line((0, y, width, y), fill=color)

    min_x = min(point[0] for point in all_points)
    max_x = max(point[0] for point in all_points)
    min_y = min(point[1] for point in all_points)
    max_y = max(point[1] for point in all_points)
    min_z = min(point[2] for point in all_points)
    max_z = max(point[2] for point in all_points)
    center = ((min_x + max_x) / 2, min_y, (min_z + max_z) / 2)
    yaw, elevation = math.radians(38), math.radians(24)

    def project(point):
        x, y, z = point[0] - center[0], point[1] - center[1], point[2] - center[2]
        u = x * math.cos(yaw) - z * math.sin(yaw)
        depth = x * math.sin(yaw) + z * math.cos(yaw)
        v = y * math.cos(elevation) - depth * math.sin(elevation)
        camera_depth = depth * math.cos(elevation) + y * math.sin(elevation)
        return u, v, camera_depth

    projected_points = [project(point) for point in all_points]
    min_u = min(point[0] for point in projected_points)
    max_u = max(point[0] for point in projected_points)
    min_v = min(point[1] for point in projected_points)
    max_v = max(point[1] for point in projected_points)
    plot_left, plot_top, plot_right, plot_bottom = 55, 115, width - 55, height - 55
    scale = min(
        (plot_right - plot_left) / max(1e-6, max_u - min_u),
        (plot_bottom - plot_top) / max(1e-6, max_v - min_v),
    ) * 0.92
    center_u, center_v = (min_u + max_u) / 2, (min_v + max_v) / 2

    def screen(point):
        u, v, depth = project(point)
        return (
            width / 2 + (u - center_u) * scale,
            (plot_top + plot_bottom) / 2 - (v - center_v) * scale,
            depth,
        )

    projected_faces = []
    light = (0.35, 0.82, 0.45)
    height_span = max(1e-6, max_y - min_y)
    for points, colors_for_face in rendered:
        projected = [screen(point) for point in points]
        ax, ay, az = (points[1][i] - points[0][i] for i in range(3))
        bx, by, bz = (points[2][i] - points[0][i] for i in range(3))
        normal = (ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx)
        length = math.sqrt(sum(value * value for value in normal)) or 1
        brightness = 0.55 + 0.45 * abs(sum(normal[i] / length * light[i] for i in range(3)))
        if colors_for_face:
            base = tuple(sum(color[channel] for color in colors_for_face) / len(colors_for_face) * 255 for channel in range(3))
        else:
            relative_height = (sum(point[1] for point in points) / 3 - min_y) / height_span
            if "森林" in title:
                base = (83 - 30 * relative_height, 118 + 70 * relative_height, 74 - 16 * relative_height)
            elif "北京" in title:
                base = (126 + 44 * relative_height, 145 + 46 * relative_height, 159 + 43 * relative_height)
            else:
                base = palette
        fill = tuple(max(0, min(255, int(value * brightness))) for value in base)
        projected_faces.append((sum(point[2] for point in projected) / 3, projected, fill))

    ground_y = plot_bottom - 18
    draw.rounded_rectangle((34, ground_y - 13, width - 34, ground_y + 18), 10, fill=(219, 228, 234))
    for _, points, fill in sorted(projected_faces, key=lambda item: item[0], reverse=True):
        polygon = [(point[0], point[1]) for point in points]
        draw.polygon(polygon, fill=fill)

    draw.rounded_rectangle((36, 28, width - 36, 98), 14, fill=(16, 39, 61))
    draw.text((62, 42), title, font=pil_font(32, True), fill=(255, 255, 255))
    draw.text((width - 62, 49), subtitle, font=pil_font(20), fill=(181, 207, 229), anchor="ra")
    destination.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(destination, quality=94)


def render_carbon_chart(destination: Path):
    summary_path = ROOT / "assets" / "examples" / "crop_gpp_npp" / "validated-carbon-summary.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    rows = summary["rows"]
    hours = []
    gpp = []
    npp = []
    for time, gross, net in rows:
        hour, minute = [int(value) for value in time.split(":")]
        hours.append(hour + minute / 60)
        gpp.append(float(gross))
        npp.append(float(net))

    width, height = 1400, 680
    canvas = PILImage.new("RGB", (width, height), (250, 252, 254))
    draw = ImageDraw.Draw(canvas)
    left, top, right, bottom = 115, 76, width - 54, height - 100
    minimum, maximum = -2.0, 24.0

    def x(value):
        return left + value / 24 * (right - left)

    def y(value):
        return bottom - (value - minimum) / (maximum - minimum) * (bottom - top)

    for value in range(0, 25, 4):
        px = x(value)
        draw.line((px, top, px, bottom), fill=(222, 230, 237), width=2)
        draw.text((px, bottom + 18), f"{value:02d}:00", font=pil_font(18), fill=(91, 111, 127), anchor="ma")
    for value in [-2, 0, 5, 10, 15, 20]:
        py = y(value)
        draw.line((left, py, right, py), fill=(222, 230, 237), width=2)
        draw.text((left - 18, py), str(value), font=pil_font(18), fill=(91, 111, 127), anchor="rm")
    draw.line((left, y(0), right, y(0)), fill=(113, 130, 143), width=3)

    gpp_points = [(x(hour), y(value)) for hour, value in zip(hours, gpp)]
    npp_points = [(x(hour), y(value)) for hour, value in zip(hours, npp)]
    draw.line(gpp_points, fill=(27, 156, 115), width=7, joint="curve")
    draw.line(npp_points, fill=(240, 165, 66), width=6, joint="curve")
    for points, color in [(gpp_points, (27, 156, 115)), (npp_points, (240, 165, 66))]:
        for px, py in points[::4]:
            draw.ellipse((px - 4, py - 4, px + 4, py + 4), fill=color)

    draw.text((left, 22), "作物示例验证运行：GPP / NPP 日变化", font=pil_font(30, True), fill=(31, 54, 73))
    draw.line((right - 330, 37, right - 275, 37), fill=(27, 156, 115), width=7)
    draw.text((right - 260, 37), "GPP", font=pil_font(19, True), fill=(31, 54, 73), anchor="lm")
    draw.line((right - 155, 37, right - 100, 37), fill=(240, 165, 66), width=7)
    draw.text((right - 85, 37), "NPP", font=pil_font(19, True), fill=(31, 54, 73), anchor="lm")
    draw.text((left, height - 40), "时间（DOY 214）", font=pil_font(19), fill=(91, 111, 127))
    draw.text(
        (left + 10, top + 10),
        "单位：微摩尔 CO2 /（平方米叶面积·秒）",
        font=pil_font(17),
        fill=(91, 111, 127),
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(destination, quality=94)


class SectionBand(Flowable):
    def __init__(self, number: str, title: str, subtitle: str):
        super().__init__()
        self.number = number
        self.title = title
        self.subtitle = subtitle
        self.height = 23 * mm

    def wrap(self, avail_width, avail_height):
        self.width = avail_width
        return self.width, self.height

    def draw(self):
        self.canv.setFillColor(PALE_BLUE)
        self.canv.roundRect(0, 0, self.width, self.height, 4 * mm, stroke=0, fill=1)
        self.canv.setFillColor(BLUE)
        self.canv.roundRect(4 * mm, 4 * mm, 15 * mm, 15 * mm, 3 * mm, stroke=0, fill=1)
        self.canv.setFillColor(colors.white)
        self.canv.setFont("StreamCN-Bold", 13)
        self.canv.drawCentredString(11.5 * mm, 9 * mm, self.number)
        self.canv.setFillColor(NAVY)
        self.canv.setFont("StreamCN-Bold", 17)
        self.canv.drawString(24 * mm, 12.5 * mm, self.title)
        self.canv.setFillColor(MUTED)
        self.canv.setFont("StreamCN", 8.5)
        self.canv.drawString(24 * mm, 6.5 * mm, self.subtitle)


def page_background(canvas, doc):
    canvas.saveState()
    if doc.page == 1:
        canvas.setFillColor(NAVY)
        canvas.rect(0, 0, PAGE_W, PAGE_H, stroke=0, fill=1)
        canvas.setFillColor(colors.HexColor("#173D5D"))
        canvas.circle(PAGE_W - 10 * mm, PAGE_H - 18 * mm, 58 * mm, stroke=0, fill=1)
        canvas.setFillColor(BLUE)
        canvas.setFont("Helvetica-Bold", 176)
        canvas.drawString(18 * mm, 64 * mm, "S")
    else:
        canvas.setStrokeColor(LINE)
        canvas.line(18 * mm, PAGE_H - 16 * mm, PAGE_W - 18 * mm, PAGE_H - 16 * mm)
        canvas.setFont("StreamCN", 7.5)
        canvas.setFillColor(MUTED)
        canvas.drawString(18 * mm, PAGE_H - 12 * mm, "STREAM STUDIO · 功能与案例手册")
        canvas.drawRightString(PAGE_W - 18 * mm, 10 * mm, f"{doc.page - 1:02d}")
    canvas.restoreState()


styles = getSampleStyleSheet()
styles.add(ParagraphStyle(
    name="CoverTitle", fontName="StreamCN-Bold", fontSize=31, leading=40,
    textColor=colors.white, spaceAfter=8 * mm, alignment=TA_LEFT
))
styles.add(ParagraphStyle(
    name="CoverSub", fontName="StreamCN", fontSize=13, leading=22,
    textColor=colors.HexColor("#C5D8E8"), spaceAfter=18 * mm
))
styles.add(ParagraphStyle(
    name="H1CN", fontName="StreamCN-Bold", fontSize=20, leading=28,
    textColor=NAVY, spaceBefore=3 * mm, spaceAfter=4 * mm
))
styles.add(ParagraphStyle(
    name="H2CN", fontName="StreamCN-Bold", fontSize=12.5, leading=19,
    textColor=BLUE, spaceBefore=3.2 * mm, spaceAfter=1.8 * mm
))
styles.add(ParagraphStyle(
    name="BodyCN", fontName="StreamCN", fontSize=9.4, leading=15,
    textColor=TEXT, spaceAfter=2.4 * mm, wordWrap="CJK"
))
styles.add(ParagraphStyle(
    name="SmallCN", fontName="StreamCN", fontSize=7.8, leading=12,
    textColor=MUTED, spaceAfter=1.8 * mm, wordWrap="CJK"
))
styles.add(ParagraphStyle(
    name="CalloutCN", fontName="StreamCN", fontSize=9, leading=14,
    textColor=NAVY, backColor=MINT, borderColor=colors.HexColor("#BCE5D7"),
    borderWidth=0.6, borderPadding=8, borderRadius=5, spaceBefore=2 * mm, spaceAfter=3 * mm
))
styles.add(ParagraphStyle(
    name="CaptionCN", fontName="StreamCN", fontSize=7.5, leading=11,
    textColor=MUTED, alignment=TA_CENTER, spaceAfter=2.5 * mm
))
styles.add(ParagraphStyle(
    name="TableCN", fontName="StreamCN", fontSize=7.6, leading=11,
    textColor=TEXT, wordWrap="CJK"
))
styles.add(ParagraphStyle(
    name="TableHeadCN", fontName="StreamCN-Bold", fontSize=7.8, leading=11,
    textColor=colors.white, alignment=TA_CENTER
))


def P(text, style="BodyCN"):
    return Paragraph(text, styles[style])


def bullet(text):
    return P(f"<font color='#2674D9'>●</font>&nbsp;&nbsp;{text}")


def data_table(rows, widths, header=True):
    prepared = []
    for row_index, row in enumerate(rows):
        style = "TableHeadCN" if header and row_index == 0 else "TableCN"
        prepared.append([P(str(value), style) for value in row])
    table = Table(prepared, colWidths=widths, repeatRows=1 if header else 0, hAlign="LEFT")
    commands = [
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("GRID", (0, 0), (-1, -1), 0.45, LINE),
        ("LEFTPADDING", (0, 0), (-1, -1), 6),
        ("RIGHTPADDING", (0, 0), (-1, -1), 6),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
        ("BACKGROUND", (0, 1 if header else 0), (-1, -1), colors.white),
    ]
    if header:
        commands.append(("BACKGROUND", (0, 0), (-1, 0), NAVY))
    for row_index in range(2 if header else 1, len(rows), 2):
        commands.append(("BACKGROUND", (0, row_index), (-1, row_index), colors.HexColor("#F5F8FA")))
    table.setStyle(TableStyle(commands))
    return table


SOURCE_DOCX = Path(r"C:\Users\jiank\Desktop\新建 Microsoft Word 文档 (2).docx")


def extract_material_images(source: Path):
    images = {}
    with ZipFile(source) as archive:
        for entry in archive.namelist():
            if not entry.startswith("word/media/image"):
                continue
            destination = TMP / Path(entry).name
            destination.write_bytes(archive.read(entry))
            number = int(destination.stem.removeprefix("image"))
            images[number] = destination
    return images


def compose_panel(destination: Path, material, items, columns=2):
    width, height = 1400, 760
    canvas = PILImage.new("RGB", (width, height), (242, 246, 250))
    draw = ImageDraw.Draw(canvas)
    rows = math.ceil(len(items) / columns)
    gap = 18
    outer = 24
    cell_width = (width - outer * 2 - gap * (columns - 1)) // columns
    cell_height = (height - outer * 2 - gap * (rows - 1)) // rows
    label_height = 48
    for index, (image_number, label) in enumerate(items):
        column = index % columns
        row = index // columns
        left = outer + column * (cell_width + gap)
        top = outer + row * (cell_height + gap)
        right = left + cell_width
        bottom = top + cell_height
        draw.rounded_rectangle((left, top, right, bottom), 14, fill=(255, 255, 255), outline=(211, 223, 233), width=2)
        draw.rounded_rectangle((left, top, right, top + label_height), 14, fill=(16, 39, 61))
        draw.rectangle((left, top + label_height - 14, right, top + label_height), fill=(16, 39, 61))
        draw.text((left + 18, top + 10), label, font=pil_font(23, True), fill=(255, 255, 255))
        with PILImage.open(material[image_number]) as source_image:
            source_image = source_image.convert("RGB")
            fitted = ImageOps.contain(source_image, (cell_width - 20, cell_height - label_height - 20), method=PILImage.Resampling.LANCZOS)
        x = left + (cell_width - fitted.width) // 2
        y = top + label_height + (cell_height - label_height - fitted.height) // 2
        canvas.paste(fitted, (x, y))
    canvas.save(destination, quality=94)


material = extract_material_images(SOURCE_DOCX)
overview_panel = TMP / "overview_panel.png"
forest_scene_panel = TMP / "forest_scene_panel.png"
forest_results_panel = TMP / "forest_results_panel.png"
forest_carbon_panel = TMP / "forest_carbon_panel.png"
city_scene_panel = TMP / "city_scene_panel.png"
city_results_panel = TMP / "city_results_panel.png"
city_scenarios_panel = TMP / "city_scenarios_panel.png"
ship_scene_panel = TMP / "ship_scene_panel.png"
ship_results_panel = TMP / "ship_results_panel.png"

compose_panel(overview_panel, material, [(1, "森林"), (11, "城市"), (24, "水上目标")], columns=3)
compose_panel(forest_scene_panel, material, [(1, "森林场景与计算域")], columns=1)
compose_panel(forest_results_panel, material, [
    (2, "三维辐射分布"), (3, "多角度分析"),
    (4, "时间序列"), (6, "土壤分层温度"),
], columns=2)
compose_panel(forest_carbon_panel, material, [(7, "森林 GPP"), (8, "森林 NPP")], columns=2)
compose_panel(city_scene_panel, material, [(11, "城市局部计算域")], columns=1)
compose_panel(city_results_panel, material, [
    (12, "短波辐射"), (13, "长波辐射"),
    (14, "方向效应"), (15, "结果对比"),
], columns=2)
compose_panel(city_scenarios_panel, material, [
    (16, "城市雾场"), (18, "城市火灾"), (20, "空气温度"),
    (21, "水平风速"), (22, "前视天际线"), (23, "高天顶角影像"),
], columns=3)
compose_panel(ship_scene_panel, material, [(24, "船舶与水面场景")], columns=1)
compose_panel(ship_results_panel, material, [
    (25, "船舶红外温度"), (26, "水面温度时序"), (27, "多角度方向效应"),
], columns=3)


doc = BaseDocTemplate(
    str(OUTPUT), pagesize=A4,
    leftMargin=18 * mm, rightMargin=18 * mm,
    topMargin=21 * mm, bottomMargin=16 * mm,
    title="Stream Studio 功能与案例手册",
    author="中国科学院空天信息创新研究院红外组",
    subject="Stream Studio overview and Forest, Beijing, Ship examples",
)
frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="normal")
doc.addPageTemplates(PageTemplate(id="main", frames=[frame], onPage=page_background))

story = [
    Spacer(1, 29 * mm),
    P("STREAM STUDIO", "CoverTitle"),
    P("功能与案例手册", "CoverTitle"),
    P("总体功能 · 森林 · 城市 · 水上目标<br/>从场景配置到影像与过程分析", "CoverSub"),
    data_table([
        ["软件版本", "1.0"],
        ["案例范围", "Forest / Beijing / Ship"],
        ["计算模式", "Facet RT / Facet EB / Voxel RT / Voxel EB"],
        ["红外组人员", "卞尊健、曹彪、历华、杜永明、肖青、柳钦火"],
        ["联系方式", "bianzj@aircas.ac.cn"],
    ], [30 * mm, 92 * mm], header=False),
    Spacer(1, 8 * mm),
    Image(str(overview_panel), width=174 * mm, height=94 * mm),
    P("本手册依据现有软件界面和结果截图整理。红外组联系人：卞尊健，bianzj@aircas.ac.cn。", "SmallCN"),
    PageBreak(),

    SectionBand("01", "总体功能与工作流程", "统一管理场景 几何 材质 气象 传感器和计算结果"),
    Spacer(1, 5 * mm),
    P("Stream Studio 面向三维地表场景的辐射传输与能量过程模拟。工程文件统一保存场景尺寸、对象分布、材质属性、太阳与天空、气象驱动、传感器和求解控制参数。Three.js 负责场景检查和结果浏览，HiStream 引擎完成辐射、能量与流体相关计算。", "BodyCN"),
    data_table([
        ["模式", "几何表达", "主要用途"],
        ["Facet RT", "三角面元", "表面方向性和精细遮挡"],
        ["Facet EB", "三角面元", "表面温度与能量收支"],
        ["Voxel RT", "三维体元", "复杂冠层和大场景辐射传输"],
        ["Voxel EB", "三维体元", "温度 通量 植被生理和流体耦合"],
    ], [31 * mm, 45 * mm, 98 * mm]),
    Spacer(1, 4 * mm),
    data_table([
        ["分析入口", "适合回答的问题"],
        ["图像结果", "查看各波段影像 亮温和空间异常"],
        ["角度分析", "比较不同观测天顶角和方位角的方向效应"],
        ["波段分析", "比较反射率 光谱辐亮度和热红外结果"],
        ["时间分析", "查看日变化和多气象节点响应"],
        ["变化分析", "计算两组影像的差值 误差和像元相关性"],
        ["三维分析", "定位体元辐射 温度 通量 风速及空气温度"],
    ], [36 * mm, 138 * mm]),
    P("标准运行顺序", "H2CN"),
    bullet("导入 OBJ 或创建 PRIM 原型，先在三维窗口核对坐标、尺度和材质绑定。"),
    bullet("设置计算域和体元大小。大场景优先选择局部包围盒并从较粗体元开始。"),
    bullet("先运行少量气象节点，确认太阳方向、输出字段和影像范围，再扩大时段。"),
    bullet("结果生成后依次检查图像、统计分析和三维过程，避免只依据单张影像判断。"),
    PageBreak(),

    SectionBand("02", "森林场景", "展示冠层辐射 温度 多角度 多时相 土壤分层和碳通量"),
    Spacer(1, 4 * mm),
    Image(str(forest_scene_panel), width=174 * mm, height=94 * mm),
    P("图 2-1  森林场景及其计算域", "CaptionCN"),
    P("森林案例由乔木、林下植被和土壤背景构成。界面截图展示了 100 m × 100 m 场景、太阳方向、体元大小和对象分布。该类场景适合使用 Voxel RT 或 Voxel EB，因为冠层内部包含大量遮挡、空隙和不同高度的叶片。", "BodyCN"),
    data_table([
        ["设置重点", "建议"],
        ["场景范围", "完整包住树冠和根部附近地表，避免边缘裁断"],
        ["体元大小", "先粗后细；体元越小，冠层结构越清楚，显存占用也越高"],
        ["对象属性", "乔木和林下植被使用 Vegetation，土壤使用 Soil"],
        ["过程输出", "开启辐射过程和能量过程，便于查看冠层内部差异"],
    ], [37 * mm, 137 * mm]),
    P("结果应先检查树冠位置、地表背景和太阳方位是否一致，再解释辐射和温度差异。", "BodyCN"),
    PageBreak(),

    SectionBand("03", "森林结果分析", "把空间分布 时间变化 观测方向和植被生理放在一起检查"),
    Spacer(1, 4 * mm),
    Image(str(forest_results_panel), width=174 * mm, height=94 * mm),
    P("图 3-1  森林案例的三维 辐射 时间和土壤分层结果", "CaptionCN"),
    P("三维辐射图用于判断冠层顶部与内部的能量差异；极坐标图用于检查观测方向效应；时间曲线反映亮温或辐射随气象节点的变化；多层土壤图显示热量向下传播时的衰减和滞后。", "BodyCN"),
    Spacer(1, 3 * mm),
    Image(str(forest_carbon_panel), width=174 * mm, height=94 * mm),
    P("图 3-2  森林植被体元的 GPP 与 NPP 空间分布", "CaptionCN"),
    P("GPP 和 NPP 在本案例中用于观察森林植被体元的空间差异。解释时应同时检查有效辐射、叶温和植被体元位置。夜间 GPP 应接近零；NPP 可因暗呼吸出现负值。", "BodyCN"),
    PageBreak(),

    SectionBand("04", "城市场景", "在完整城市背景中选择局部区域进行体元化和求解"),
    Spacer(1, 4 * mm),
    Image(str(city_scene_panel), width=174 * mm, height=94 * mm),
    P("图 4-1  北京城市案例的局部计算域", "CaptionCN"),
    P("城市案例包含建筑、道路、植被及可选的雾、火焰和流体过程。大范围建筑可保持完整显示，但计算时应移动包围盒选择目标街区。完全位于计算域外的对象可直接跳过，跨越边界的对象只处理域内部分。", "BodyCN"),
    data_table([
        ["阶段", "体元设置", "检查内容"],
        ["场景定位", "较粗体元", "建筑范围 高度 太阳方位和计算域位置"],
        ["热环境试算", "中等体元", "短波 长波 净辐射和建筑温度"],
        ["街区精算", "较细体元", "局部阴影 风速 空气温度及目标影像"],
    ], [34 * mm, 40 * mm, 100 * mm]),
    bullet("城市全场直接使用细体元会迅速增加 GPU 内存和求交开销。"),
    bullet("先裁切再体元化，能够避免为域外建筑建立无用体元和加速结构。"),
    PageBreak(),

    SectionBand("05", "城市结果分析", "组合辐射 方向性 情景过程 流体结果和前视影像"),
    Spacer(1, 4 * mm),
    Image(str(city_results_panel), width=174 * mm, height=94 * mm),
    P("图 5-1  城市短波 长波 方向效应与结果对比", "CaptionCN"),
    P("短波结果突出受光屋顶与阴影立面，长波结果反映材料温度及周围表面的热辐射。角度分析显示不同方向的整幅影像统计值；变化分析可比较两种方法或两个时刻，并给出差值图、误差和像元相关性。", "BodyCN"),
    Spacer(1, 3 * mm),
    Image(str(city_scenarios_panel), width=174 * mm, height=94 * mm),
    P("图 5-2  城市雾 火灾 空气温度 风速与前视结果", "CaptionCN"),
    P("雾和火焰作为局部参与介质加入场景；空气温度与水平风速属于体元流体力学结果。高天顶角及前视视场适合生成城市天际线，但应核对相机朝向，避免把侧视误判为前视。", "BodyCN"),
    PageBreak(),

    SectionBand("06", "水上目标", "联合处理船体 水面 红外温度 时间变化和多角度观测"),
    Spacer(1, 4 * mm),
    Image(str(ship_scene_panel), width=174 * mm, height=94 * mm),
    P("图 6-1  集装箱船与水面组成的水上目标场景", "CaptionCN"),
    P("水上目标案例由船舶 OBJ 和水面 PRIM 对象组成。水面应绑定 Water 类型及相应光学和热学属性，船体按不同部件绑定材料。场景截图采用 300 m × 300 m 范围，可覆盖船体及周边背景。", "BodyCN"),
    Spacer(1, 3 * mm),
    Image(str(ship_results_panel), width=174 * mm, height=94 * mm),
    P("图 6-2  船舶红外温度 水面温度时序和多角度方向效应", "CaptionCN"),
    P("顶视热红外影像可辨认船体轮廓和部件温差；时间分析用于比较水面或船体亮温的日变化；多角度分析反映船体几何和水面方向反射共同造成的观测差异。", "BodyCN"),
    PageBreak(),

    SectionBand("07", "运行检查与结果判读", "先确认几何和输入 再判断辐射 温度和流体结果"),
    Spacer(1, 5 * mm),
    data_table([
        ["检查阶段", "关键问题", "异常处理"],
        ["场景检查", "对象是否在正确位置 尺度是否合理", "核对坐标约定 OBJ 轴向和位置文件"],
        ["体元化", "体元数是否超过显存承受范围", "增大体元或缩小计算域"],
        ["辐射结果", "向阳和背阴区域是否符合太阳方向", "检查直接辐射 遮挡和材质绑定"],
        ["温度结果", "时间曲线是否连续 土壤层是否逐渐滞后", "检查气象节点 初始温度和时间步"],
        ["多角度", "天顶角 方位角和前视方向是否一致", "核对相机投影和坐标方向"],
        ["流体结果", "建筑内部是否为无效值 风速箭头是否合理", "检查固体掩膜 边界和流体体元大小"],
    ], [31 * mm, 70 * mm, 73 * mm]),
    P("常见运行问题", "H2CN"),
    bullet("Array buffer allocation failed 通常发生在一次加载过多过程数据时。磁盘可保留全部体元，界面采用抽样显示。"),
    bullet("VK_ERROR_DEVICE_LOST 多与 GPU 压力或残留计算进程有关。使用重置结束相关进程，再降低体元规模或周期穿透次数。"),
    bullet("城市或森林计算缓慢时，先查看体元化规模和计算域，不应只降低射线深度。"),
    bullet("不同模式或时刻的对比应保持场景、体元、波段和气象条件一致。"),
    Spacer(1, 5 * mm),
    P("三个案例覆盖了植被冠层、城市建筑和水上目标。它们使用同一套工程结构和结果分析入口，可以作为新工程的配置参考。", "BodyCN"),
]

doc.build(story)
print(OUTPUT)
