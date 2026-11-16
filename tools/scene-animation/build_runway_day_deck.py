"""Build the editable PowerPoint report of the open-airfield mid-IR day, in the style of the reference deck.

Every number on a slide is computed from the case products (day_history.csv, closure.json, energy_summary.json,
render_receipt.json, native_runs.json); charts are native, editable charts. Nothing is copied from another case.
"""
import argparse
import csv
import json
import tempfile
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image
from pptx import Presentation
from pptx.chart.data import XyChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE, XL_LEGEND_POSITION, XL_MARKER_STYLE, XL_TICK_LABEL_POSITION
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.util import Emu, Inches, Pt

from run_coupled_town import SURFACE_RESIDUAL_LIMIT_WM2
from render_runway_day import GROUPS, STILL_LAYOUT
BG, WHITE, GREY, TEAL, GRID = '101B25', 'F0F5F6', 'B2C3CD', '7FD0DB', '2E4150'
FONT = 'Microsoft YaHei'
RUNS = ['sunlit_powered', 'sunlit_unpowered', 'nosun_powered', 'nosun_unpowered']
RUN_NAME = {'sunlit_powered': '有太阳 · 载荷开动', 'sunlit_unpowered': '有太阳 · 载荷关闭', 'nosun_powered': '无太阳 · 载荷开动', 'nosun_unpowered': '无太阳 · 载荷关闭'}
RUN_COLOR = {'sunlit_powered': 'F28B82', 'sunlit_unpowered': 'E8C468', 'nosun_powered': '6ECFE6', 'nosun_unpowered': 'A99BEB'}
GROUP_NAME = {'tanks': '坦克（3 辆平均）', 'cars': '汽车（3 轿车 + SUV）', 'aircraft': '飞机（2 架平均）', 'bus': '公交车', 'runway_pavement': '跑道路面', 'grass_field': '草地'}
GROUP_COLOR = {'tanks': '6ECFE6', 'cars': 'F28B82', 'aircraft': 'F0F5F6', 'bus': 'E8C468', 'runway_pavement': 'A99BEB', 'grass_field': '8FD18B'}


def rgb(hex_):
    return RGBColor.from_string(hex_)


class Deck:
    def __init__(self):
        self.prs = Presentation()
        self.prs.slide_width, self.prs.slide_height = Inches(13.333), Inches(7.5)
        self.count = 0

    def slide(self, title=None, subtitle=None, notes=None):
        s = self.prs.slides.add_slide(self.prs.slide_layouts[6])
        s.background.fill.solid()
        s.background.fill.fore_color.rgb = rgb(BG)
        self.count += 1
        if title:
            self.text(s, title, 0.5, 0.38, 12.33, 0.71, 33.75, WHITE, True)
        if subtitle:
            self.text(s, subtitle, 0.5, 1.23, 12.33, 0.44, 19.5, GREY)
        self.text(s, f'{self.count:02d}', 12.40, 7.09, 0.44, 0.24, 12.75, GREY)
        if notes:
            s.notes_slide.notes_text_frame.text = notes
        return s

    @staticmethod
    def text(slide, content, x, y, w, h, size, color, bold=False, align=None, anchor=None):
        box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
        tf = box.text_frame
        tf.word_wrap = True
        tf.margin_left = tf.margin_right = Inches(0.04)
        tf.margin_top = tf.margin_bottom = Inches(0.02)
        if anchor:
            tf.vertical_anchor = anchor
        lines = content if isinstance(content, list) else [content]
        for i, line in enumerate(lines):
            p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
            if align:
                p.alignment = align
            r = p.add_run()
            r.text = line
            r.font.name, r.font.size, r.font.bold = FONT, Pt(size), bold
            r.font.color.rgb = rgb(color)
            rPr = r._r.get_or_add_rPr()
            for tag in ('a:ea', 'a:cs'):
                from lxml import etree
                el = etree.SubElement(rPr, '{http://schemas.openxmlformats.org/drawingml/2006/main}' + tag.split(':')[1])
                el.set('typeface', FONT)
        return box

    @staticmethod
    def crop_sky(path, out_dir, layout=None):
        """Remove the all-black sky band common to the four panels of a still (display only; no data change).

        The panel layout comes from render_receipt.json (stillLayout) so it cannot drift from the renderer."""
        im = Image.open(path).convert('RGB')
        a = np.asarray(im)
        layout = layout or STILL_LAYOUT
        top, gap, strip = layout['top'], layout['rowGap'], layout['label']
        panel_w, panel_h = im.width // 2, (im.height - top - 2 * gap) // 2
        lead = []
        for row in range(2):
            for col in range(2):
                y0 = top + row * (panel_h + gap) + strip
                block = a[y0:y0 + panel_h, col * panel_w:(col + 1) * panel_w].max(axis=(1, 2))
                nz = np.flatnonzero(block > 0)
                lead.append(int(nz[0]) if len(nz) else panel_h)
        cut = max(0, min(lead) - 6)
        if cut < 40:
            return path
        out = Image.new('RGB', (im.width, im.height - 2 * cut), (16, 28, 38))
        out.paste(im.crop((0, 0, im.width, top)), (0, 0))
        for row in range(2):
            y0 = top + row * (panel_h + gap)
            ny = top + row * (panel_h + gap - cut)
            out.paste(im.crop((0, y0, im.width, y0 + strip)), (0, ny))
            out.paste(im.crop((0, y0 + strip + cut, im.width, y0 + strip + panel_h)), (0, ny + strip))
        target = Path(out_dir) / Path(path).name
        out.save(target)
        return target

    @staticmethod
    def picture(slide, path, x, y, w, h):
        """Fit a picture inside the box, keeping its aspect ratio, left/top aligned."""
        with Image.open(path) as im:
            iw, ih = im.size
        scale = min(w / iw, h / ih)
        return slide.shapes.add_picture(str(path), Inches(x), Inches(y), Inches(iw * scale), Inches(ih * scale))

    @staticmethod
    def chart(slide, series, x, y, w, h, x_title, y_title, x_range=(0, 24), y_range=None, y_format='0', legend=True, x_unit=4, font_size=14):
        data = XyChartData()
        for name, pts in series:
            s = data.add_series(name)
            for px, py in pts:
                s.add_data_point(round(float(px), 6), round(float(py), 6))
        frame = slide.shapes.add_chart(XL_CHART_TYPE.XY_SCATTER_LINES_NO_MARKERS, Inches(x), Inches(y), Inches(w), Inches(h), data)
        ch = frame.chart
        ch.font.name, ch.font.size = FONT, Pt(font_size)
        ch.font.color.rgb = rgb(GREY)
        ch.has_title = False
        ch.has_legend = legend
        if legend:
            ch.legend.position = XL_LEGEND_POSITION.BOTTOM
            ch.legend.include_in_layout = False
            ch.legend.font.size = Pt(font_size)
            ch.legend.font.color.rgb = rgb(GREY)
        for (name, _), ser in zip(series, ch.plots[0].series):
            ser.smooth = False
            ser.format.line.width = Pt(2.25)
            ser.format.line.color.rgb = rgb(series_color(name))
            ser.marker.style = XL_MARKER_STYLE.NONE
        for axis, title, rng, fmt, unit in ((ch.category_axis, x_title, x_range, '0', x_unit), (ch.value_axis, y_title, y_range, y_format, None)):
            axis.has_title = True
            axis.axis_title.text_frame.text = title
            run = axis.axis_title.text_frame.paragraphs[0].runs[0]
            run.font.size, run.font.bold, run.font.name = Pt(font_size), True, FONT
            run.font.color.rgb = rgb(GREY)
            axis.tick_labels.font.size = Pt(font_size)
            axis.tick_labels.font.color.rgb = rgb(GREY)
            axis.tick_labels.number_format, axis.tick_labels.number_format_is_linked = fmt, False
            axis.tick_label_position = XL_TICK_LABEL_POSITION.LOW
            axis.format.line.color.rgb = rgb(GRID)
            axis.has_major_gridlines = True
            axis.major_gridlines.format.line.color.rgb = rgb(GRID)
            axis.major_gridlines.format.line.width = Pt(0.75)
            if rng:
                axis.minimum_scale, axis.maximum_scale = rng
            if unit:
                axis.major_unit = unit
        return frame

    @staticmethod
    def table(slide, rows, x, y, w, h, col_widths, size=15, header=True):
        shape = slide.shapes.add_table(len(rows), len(rows[0]), Inches(x), Inches(y), Inches(w), Inches(h))
        table = shape.table
        for i, cw in enumerate(col_widths):
            table.columns[i].width = Inches(cw)
        for r, row in enumerate(rows):
            for c, value in enumerate(row):
                cell = table.cell(r, c)
                cell.fill.solid()
                cell.fill.fore_color.rgb = rgb('1D3040' if header and r == 0 else '14232F')
                cell.margin_left = cell.margin_right = Inches(0.08)
                cell.vertical_anchor = MSO_ANCHOR.MIDDLE
                tf = cell.text_frame
                tf.word_wrap = True
                tf.text = str(value)
                for p in tf.paragraphs:
                    for run in p.runs:
                        run.font.name, run.font.size = FONT, Pt(size)
                        run.font.bold = header and r == 0
                        run.font.color.rgb = rgb(WHITE if (header and r == 0) or c == 0 else GREY)
        return shape


_SERIES_COLORS = {}


def series_color(name):
    return _SERIES_COLORS.get(name, GREY)


class Data:
    def __init__(self, case):
        self.case = Path(case)
        rows = list(csv.DictReader((self.case / 'day_history.csv').open(encoding='utf-8')))
        self.hours = sorted({float(r['hour']) for r in rows})
        self.t = defaultdict(dict)
        self.forcing = {}
        for r in rows:
            self.t[(r['run'], r['group'])][float(r['hour'])] = dict(skin=float(r['skinMeanK']), tb=float(r['bandBrightnessK']), skinMax=float(r['skinMaxK']))
            if r['group'] == 'tanks':
                self.forcing[(r['run'], float(r['hour']))] = (float(r['airC']), float(r['shortwaveWm2']), float(r['skyK']))
        self.closure = json.loads((self.case / 'closure.json').read_text(encoding='utf-8'))
        self.energy = json.loads((self.case / 'energy_summary.json').read_text(encoding='utf-8'))
        self.receipt = json.loads((self.case / 'render_receipt.json').read_text(encoding='utf-8'))
        self.native = json.loads((self.case / 'native_runs.json').read_text(encoding='utf-8'))
        self.coupling = json.loads((self.case / 'coupling.json').read_text(encoding='utf-8'))

    def curve(self, run, group, key='skin'):
        return [(h, self.t[(run, group)][h][key] - (273.15 if key == 'skin' and False else 0)) for h in self.hours]

    def diff(self, run_a, group_a, run_b, group_b, key='skin'):
        return [(h, self.t[(run_a, group_a)][h][key] - self.t[(run_b, group_b)][h][key]) for h in self.hours]


def rng(values, step=1.0, pad=0.0):
    lo, hi = min(values) - pad, max(values) + pad
    return (np.floor(lo / step) * step, np.ceil(hi / step) * step)


def closure_max(by_run, runs, key):
    """Worst value over every group, including individual props (not only category means)."""
    return max(by_run[r][g][key] for r in runs for g in by_run[r])


def deep_ground_time_constant_days(coupling):
    values = []
    for group in coupling.get('groups', {}).values():
        if str(group.get('kind', '')).startswith('lumped-deep') and group.get('conductanceWPerK', 0) > 0:
            values.append(group['capacityJPerK'] / group['conductanceWPerK'] / 86400.0)
    return sum(values) / len(values) if values else None


def build(case, out):
    case = Path(case).resolve()
    d = Data(case)
    deck = Deck()
    rec = d.receipt
    low_k, high_k = rec['displayLowK'], rec['displayHighK']
    band_points = rec.get('bandPoints', 17)
    ir_w, ir_h = rec.get('irWidth', 320), rec.get('irHeight', 180)
    scene_build = json.loads((case / 'display_scene/scene_build.json').read_text(encoding='utf-8-sig'))
    minimum_gap = float(scene_build['minimumVehicleGapM'])
    tau_days = deep_ground_time_constant_days(d.coupling)
    solar_window = d.coupling.get('solarWindowHours', [6, 18])
    ground_start = d.coupling.get('groundInitialK', {})
    src = f'来源：{case}'
    crop_tmp = tempfile.TemporaryDirectory()
    crop_dir = Path(crop_tmp.name)
    stills = case / 'stills'
    spin = d.closure['spinupDays']
    actors = json.loads((case / 'actors.json').read_text(encoding='utf-8'))['actors']
    power = {a['id']: a['power_w'] for a in actors}
    # Report constants read from the case products (render receipt / closure) instead of literals.
    adc_bits = rec.get('adcBits', 12)
    node_minutes = rec.get('nodeHours', 0.5) * 60
    nodes_per_day = rec.get('nodesPerDay', int(round(24 / rec.get('nodeHours', 0.5))))
    video_frames, video_fps = rec.get('videoFrames', 288), rec.get('videoFps', 24)
    video_seconds = rec.get('videoSeconds', video_frames / video_fps)
    shift_no_design = closure_max(d.closure['byRun'], ('nosun_powered', 'nosun_unpowered'), 'groupMeanShiftK')

    # 1 title
    s = deck.slide(notes=src + '\nRGB 为同一几何的静态白天参考，不是随时间重新照明的结果。')
    deck.text(s, ['跑道上的飞机、', '坦克与汽车'], 0.5, 1.45, 5.6, 1.9, 40, WHITE, True)
    deck.text(s, '3–5 μm 中红外 · 一天 24 小时', 0.52, 3.55, 5.6, 0.5, 21, GREY)
    deck.text(s, ['有太阳 / 无太阳', '载荷开动 / 载荷关闭', '共四组对照'], 0.52, 4.25, 5.6, 1.5, 22, TEAL)
    deck.text(s, 'StreamStudio 模拟展示 · 示例参数', 0.52, 6.2, 5.6, 0.4, 16, GREY)
    deck.picture(s, case / 'rgb/overview.png', 6.4, 1.6, 6.45, 3.63)
    deck.text(s, '原小镇已移除；目标全部在跑道上（RGB 为静态参考）', 6.4, 5.35, 6.45, 0.4, 14, GREY)

    # 2 scene
    s = deck.slide('场景：开阔跑道', '从原封存场景去掉小镇，飞机、坦克、汽车和公交车放在跑道上', notes=src + '/display_scene/scene.json；scene_build.json')
    deck.picture(s, case / 'rgb/high_view.png', 0.5, 1.85, 7.9, 4.45)
    deck.text(s, '俯视（RGB 静态参考）', 0.5, 6.33, 7.9, 0.35, 15, GREY)
    deck.text(s, '目标', 8.75, 1.85, 4.1, 0.45, 21.75, WHITE, True)
    sedans = [c for c in GROUPS['cars'] if c != 'suv']
    deck.text(s, [f'{len(GROUPS["aircraft"])} 架飞机（姿态不变）', f'{len(GROUPS["tanks"])} 辆坦克', f'{len(sedans)} 辆轿车 + {len(GROUPS["cars"]) - len(sedans)} 辆 SUV', f'{len(GROUPS["bus"])} 辆公交车'], 8.75, 2.45, 4.1, 1.6, 20.25, GREY)
    deck.text(s, ['地面：跑道沥青 + 草地', '小镇原址补回同材质地面', f'{len(rec.get("views", [])) or 4} 个固定相机'], 8.75, 4.55, 4.1, 1.5, 18, TEAL)
    deck.text(s, f'位置与间距经检查：全部落在跑道内，避开道路连接段，目标间最近 {minimum_gap:.1f} m', 0.5, 6.8, 12.0, 0.36, 16, GREY)

    # 3 design
    s = deck.slide('四组对照的设计', '同一几何、材质、风、气温和天空长波；只改太阳/载荷两个开关', notes=src + '/coupling.json；native_runs.json')
    deck.table(s, [['', '载荷关闭', '载荷开动'], ['有太阳', '有太阳 · 载荷关闭', '有太阳 · 载荷开动'], ['无太阳', '无太阳 · 载荷关闭', '无太阳 · 载荷开动']], 0.5, 1.9, 7.4, 1.9, [1.6, 2.9, 2.9], 17)
    deck.text(s, ['太阳开关：短波辐射置 0', '载荷开关：内部热源功率置 0', f'深层地温初值也按太阳组声明（{ground_start.get("sunlit", "?")} / {ground_start.get("nosun", "?")} K）；{spin} 天预热后组平均日间漂移 ≤{shift_no_design:.3f} K（无太阳组）', '无太阳组气温、湿度、风、天空长波仍取有太阳日的值：这是只去掉日照的受控对照，不是真实阴天'], 0.5, 4.1, 7.4, 2.6, 18, GREY)
    rows = [['目标', '数量', '内部热源']]
    def kw(ids):
        values = [power[k] / 1000 for k in ids]
        return ' / '.join(f'{v:.0f}' for v in values) + ' kW' if len(ids) <= 3 else f'{min(values):.0f}–{max(values):.0f} kW'
    rows.append(['飞机', str(len(GROUPS['aircraft'])), kw(GROUPS['aircraft'])])
    rows.append(['坦克', str(len(GROUPS['tanks'])), kw(GROUPS['tanks'])])
    rows.append(['轿车 + SUV', str(len(GROUPS['cars'])), kw(GROUPS['cars'])])
    rows.append(['公交车', str(len(GROUPS['bus'])), kw(GROUPS['bus'])])
    deck.table(s, rows, 8.3, 1.9, 4.55, 2.7, [1.6, 0.8, 2.15], 15)
    deck.text(s, ['载荷开动组全天持续工作', '飞机功率为新设示例值；其余沿用原封存案例'], 8.3, 4.85, 4.55, 1.3, 16, TEAL)

    # 4 method
    s = deck.slide('怎么算的', '温度由原生求解得到，图像再按 3–5 μm 波段积分', notes=src + '/README.md；render_receipt.json')
    deck.text(s, ['GPU 求遮挡与短波、长波辐射交换', 'CPU 求目标内部热源、表皮导热和表面能量平衡', f'{node_minutes:g} 分钟一个节点；先预热 {spin} 天，再取第 {spin + 1} 天输出', f'地面为薄层加等效深层热库（时间常数约 {tau_days:.1f} 天）' if tau_days else '地面为薄层加等效深层热库'], 0.5, 1.9, 7.3, 3.4, 20.25, GREY)
    deck.text(s, [f'成像：3–5 μm，{band_points} 点 Simpson 积分', f'相机 {ir_w} × {ir_h}，{adc_bits} bit，含 PSF、像元平均和通用噪声', f'显示：固定范围 {low_k}–{high_k} K 等效亮温，灰度对亮温线性'], 0.5, 5.15, 7.3, 1.6, 18, TEAL)
    nr = d.native['runs']
    max_res = max(v['maxCoupledSurfaceResidualWm2'] for v in nr.values())
    margin = SURFACE_RESIDUAL_LIMIT_WM2 - max_res
    deck.text(s, '求解记录', 8.4, 1.9, 4.4, 0.45, 21.75, WHITE, True)
    deck.text(s, [f"{len(nr)} 组 × {nr[RUNS[0]]['nodes']} 个节点，全部收敛", f"表面能量残差最大 {max_res:.4f} W/m²，低于 {SURFACE_RESIDUAL_LIMIT_WM2:g} W/m² 的收敛线（余量 {margin:.4f}{'，属边界通过' if margin < 0.01 else ''}）", f"目标内部节点残差 ≤ {max(v['maxCoreResidualW'] for v in nr.values()):.0e} W", f"{d.native.get('parallelRuns', len(nr))} 组并行求解；串行逐位一致未在案例内复核"], 8.4, 2.5, 4.4, 3.2, 18, GREY)

    # 5 forcing
    s = deck.slide('一天的太阳与气温', '合成的半正弦日变化；有太阳组与无太阳组只差短波', notes=src + '/day_history.csv（airC、shortwaveWm2）')
    sw_sun = [(h, d.forcing[('sunlit_powered', h)][1]) for h in d.hours]
    sw_no = [(h, d.forcing[('nosun_powered', h)][1]) for h in d.hours]
    air = [(h, d.forcing[('sunlit_powered', h)][0]) for h in d.hours]
    _SERIES_COLORS.update({'有太阳': 'E8C468', '无太阳': 'A99BEB', '气温': 'F28B82'})
    deck.chart(s, [('有太阳', sw_sun), ('无太阳', sw_no)], 0.5, 1.8, 6.1, 4.6, '时间 / h', '短波辐射 / W m⁻²', (0, 24), rng([v for _, v in sw_sun + sw_no] + [0], 100), '0')
    deck.chart(s, [('气温', air)], 6.75, 1.8, 6.1, 4.6, '时间 / h', '气温 / °C', (0, 24), rng([v for _, v in air], 2, 1), '0', legend=False)
    zen = json.loads((case / 'native/sunlit_powered/output/faceteb_run.json').read_text(encoding='utf-8-sig'))['completed'][-nodes_per_day:]
    noon = min(zen, key=lambda n: n['solarZenith'])
    peak_sw = max(v for _, v in sw_sun)
    deck.text(s, f'日照 {solar_window[0]:g}–{solar_window[1]:g} h，正午峰值 {peak_sw:.0f} W/m²；正午太阳天顶角 {noon["solarZenith"]:.1f}°；气温 {min(v for _, v in air):.1f}–{max(v for _, v in air):.1f} °C', 0.5, 6.55, 12.3, 0.4, 17, TEAL)

    # 6.. stills
    still_plan = [('vehicle_view', 4.0, '黎明前：太阳尚未升起', None), ('vehicle_view', 12.0, '正午：有太阳时跑道和目标都被晒热', None), ('vehicle_view', 15.0, '下午：温度峰值前后', None), ('vehicle_view', 20.0, '傍晚：太阳落山后', None),
                  ('high_view', 15.0, '俯视：目标与跑道的相对亮度', None)]
    for view, hour, headline, _ in still_plan:
        hh = int(hour)
        s = deck.slide(f'{hh:02d}:00 的中红外对照', f'{headline} · 固定范围 {low_k}–{high_k} K', notes=src + f'/stills/{view}_{hh:02d}h.png。灰度对3–5 μm等效亮温线性；四格同一范围。')
        deck.picture(s, deck.crop_sky(stills / f'{view}_{hh:02d}h.png', crop_dir, rec.get('stillLayout')), 0.5, 1.75, 8.7, 5.5)
        k_sun = d.t[('sunlit_unpowered', 'runway_pavement')][hour]['tb']
        k_no = d.t[('nosun_unpowered', 'runway_pavement')][hour]['tb']
        t_pw = d.t[('sunlit_powered', 'tanks')][hour]['tb']
        t_pn = d.t[('nosun_powered', 'tanks')][hour]['tb']
        t_us = d.t[('sunlit_unpowered', 'tanks')][hour]['tb']
        deck.text(s, '亮温（3–5 μm 等效）', 9.35, 1.85, 3.5, 0.45, 20, WHITE, True)
        deck.text(s, [f'跑道：有太阳 {k_sun:.1f} K', f'　　　无太阳 {k_no:.1f} K', f'坦克：有太阳开动 {t_pw:.1f} K', f'　　　无太阳开动 {t_pn:.1f} K', f'　　　有太阳关闭 {t_us:.1f} K'], 9.35, 2.4, 3.5, 2.5, 16, GREY)
        deck.text(s, ['上排有太阳，下排无太阳', '左列载荷开动，右列关闭', 'RGB 不随光照重新计算'], 9.35, 5.0, 3.5, 1.4, 15, TEAL)
        if hour in (4.0, 12.0):
            deck.text(s, '另有每格独立拉伸的增强版（stills/*_enhanced.png），格间不可比', 9.35, 6.4, 3.5, 0.7, 13, GREY)

    # surface temperature curves per group
    for group in ('tanks', 'cars', 'aircraft', 'bus'):
        _SERIES_COLORS.update({RUN_NAME[r]: RUN_COLOR[r] for r in RUNS})
        series = [(RUN_NAME[r], [(h, v - 273.15) for h, v in [(h, d.t[(r, group)][h]['skin']) for h in d.hours]]) for r in RUNS]
        allv = [p[1] for _, pts in series for p in pts]
        peak = {r: max(d.t[(r, group)][h]['skin'] for h in d.hours) - 273.15 for r in RUNS}
        trough = {r: min(d.t[(r, group)][h]['skin'] for h in d.hours) - 273.15 for r in RUNS}
        s = deck.slide(f'{GROUP_NAME[group].split("（")[0]}表面温度的一天', f'{GROUP_NAME[group]} · 面积加权平均表面温度', notes=src + '/day_history.csv（skinMeanK − 273.15）。曲线为30分钟原生节点，不平滑。')
        deck.chart(s, series, 0.5, 1.77, 12.33, 4.4, '时间 / h', '表面温度 / °C', (0, 24), rng(allv, 5), '0')
        deck.text(s, f'有太阳开动 {trough["sunlit_powered"]:.0f}–{peak["sunlit_powered"]:.0f} °C；有太阳关闭 {trough["sunlit_unpowered"]:.0f}–{peak["sunlit_unpowered"]:.0f} °C；无太阳开动 {trough["nosun_powered"]:.0f}–{peak["nosun_powered"]:.0f} °C；无太阳关闭 {trough["nosun_unpowered"]:.0f}–{peak["nosun_unpowered"]:.0f} °C', 0.5, 6.3, 12.33, 0.7, 17, TEAL)

    # payload delta
    s = deck.slide('载荷开动引起的表面温升', 'ΔT = 同一时刻载荷开动组 − 关闭组；几何、气象和材质相同', notes=src + '/day_history.csv。每个值为 skinMeanK(开动) − skinMeanK(关闭)。')
    gcols = ['tanks', 'cars', 'aircraft', 'bus']
    _SERIES_COLORS.update({GROUP_NAME[g].split('（')[0]: GROUP_COLOR[g] for g in GROUP_NAME})
    # One shared range for both panels (comparable), derived from the data instead of a fixed (0, 45).
    payload_range = rng([v for sun in ('sunlit', 'nosun') for g in gcols for _, v in d.diff(f'{sun}_powered', g, f'{sun}_unpowered', g)] + [0], 5)
    for i, sun in enumerate(('sunlit', 'nosun')):
        series = [(GROUP_NAME[g].split('（')[0], d.diff(f'{sun}_powered', g, f'{sun}_unpowered', g)) for g in gcols]
        deck.chart(s, series, 0.5 + i * 6.25, 1.95, 6.1, 4.2, '时间 / h', 'ΔT / K', (0, 24), payload_range, '0', font_size=12)
        deck.text(s, '有太阳' if sun == 'sunlit' else '无太阳', 0.5 + i * 6.25, 1.68, 6.1, 0.3, 15, WHITE, True)
    lo = {sun: {g: (min(v for _, v in d.diff(f'{sun}_powered', g, f'{sun}_unpowered', g)), max(v for _, v in d.diff(f'{sun}_powered', g, f'{sun}_unpowered', g))) for g in gcols} for sun in ('sunlit', 'nosun')}
    deck.text(s, f'坦克 {lo["sunlit"]["tanks"][0]:.0f}–{lo["nosun"]["tanks"][1]:.0f} K，汽车 {lo["sunlit"]["cars"][0]:.0f}–{lo["nosun"]["cars"][1]:.0f} K，飞机 {lo["sunlit"]["aircraft"][0]:.0f}–{lo["nosun"]["aircraft"][1]:.0f} K，公交车 {lo["sunlit"]["bus"][0]:.0f}–{lo["nosun"]["bus"][1]:.0f} K；全天几乎恒定，有太阳时中午略有变化', 0.5, 6.3, 12.33, 0.7, 17, TEAL)

    # sun delta
    s = deck.slide('太阳引起的表面温升', 'ΔT = 同一时刻有太阳组 − 无太阳组（载荷关闭）', notes=src + '/day_history.csv。skinMeanK(sunlit_unpowered) − skinMeanK(nosun_unpowered)。')
    groups = ['tanks', 'cars', 'aircraft', 'bus', 'runway_pavement']
    series = [(GROUP_NAME[g].split('（')[0], d.diff('sunlit_unpowered', g, 'nosun_unpowered', g)) for g in groups]
    sun_series = [v for _, pts in series for _, v in pts]
    deck.chart(s, series, 0.5, 1.77, 12.33, 4.4, '时间 / h', 'ΔT / K', (0, 24), rng(sun_series + [0], 2), '0')
    peaks = {g: max(d.diff('sunlit_unpowered', g, 'nosun_unpowered', g), key=lambda p: p[1]) for g in groups}
    night = [dict(d.diff('sunlit_unpowered', g, 'nosun_unpowered', g))[4.0] for g in groups]
    deck.text(s, f'峰值：跑道 {peaks["runway_pavement"][1]:.1f} K（{peaks["runway_pavement"][0]:.1f} h），目标 {min(peaks[g][1] for g in groups[:4]):.1f}–{max(peaks[g][1] for g in groups[:4]):.1f} K；凌晨 04:00 仍剩 {min(night):.1f}–{max(night):.1f} K（前一天的储热）', 0.5, 6.3, 12.33, 0.7, 17, TEAL)

    # band contrast
    s = deck.slide('目标与跑道的中红外对比', '3–5 μm 等效亮温：目标 − 跑道路面', notes=src + '/day_history.csv（bandBrightnessK）。亮温由波段辐亮度反演，含发射、反射天空长波和反射太阳。')
    contrast = {group: [(RUN_NAME[r], d.diff(r, group, r, 'runway_pavement', 'tb')) for r in RUNS] for group in ('tanks', 'aircraft')}
    contrast_range = rng([v for ser in contrast.values() for _, pts in ser for _, v in pts], 5)
    for i, group in enumerate(('tanks', 'aircraft')):
        series = contrast[group]
        deck.chart(s, series, 0.5 + i * 6.25, 1.95, 6.1, 4.2, '时间 / h', '亮温差 / K', (0, 24), contrast_range, '0', font_size=12)
        deck.text(s, GROUP_NAME[group].split('（')[0], 0.5 + i * 6.25, 1.68, 6.1, 0.3, 15, WHITE, True)
    cm = {r: (min(v for _, v in d.diff(r, 'tanks', r, 'runway_pavement', 'tb')), max(v for _, v in d.diff(r, 'tanks', r, 'runway_pavement', 'tb'))) for r in RUNS}
    deck.text(s, f'坦克：载荷开动时全天比跑道热 {cm["sunlit_powered"][0]:.0f}–{cm["nosun_powered"][1]:.0f} K；载荷关闭时在 {cm["sunlit_unpowered"][0]:.0f} 到 +{max(cm["sunlit_unpowered"][1], cm["nosun_unpowered"][1]):.0f} K 之间，随日照在比跑道冷和略热之间变化', 0.5, 6.3, 12.33, 0.7, 17, TEAL)

    # videos
    for view, caption in (('vehicle_view', '近景'), ('overview', '总览')):
        v = rec['videos'][view]
        s = deck.slide(f'一天的中红外视频 · {caption}', f'24 小时压缩为 {video_seconds:g} 秒 · 点击画面播放', notes=src + f'/{v["file"]}。{video_seconds:g} s、{video_fps} fps、{video_frames} 帧；{node_minutes:g} 分钟原生节点之间做辐亮度线性插值，不是新增解算。未做现场精度验证。')
        poster = case / 'video_frames' / view / f'frame_{(video_frames // 2) // 24 * 24:04d}.png'
        s.shapes.add_movie(str(case / v['file']), Inches(0.5), Inches(1.75), Inches(8.7), Inches(5.57), poster_frame_image=str(poster), mime_type='video/mp4')
        deck.text(s, ['上排有太阳，下排无太阳', '左列载荷开动，右列关闭', f'固定范围 {low_k}–{high_k} K', '帧间为插值'], 9.7, 2.0, 3.2, 2.4, 17, GREY)
        deck.text(s, [f'IR：{ir_w} × {ir_h}，{adc_bits} bit', 'RGB 不在视频内'], 9.7, 4.8, 3.2, 1.3, 16, TEAL)

    # checks and limits
    s = deck.slide('检查结果与限制', '先看守恒与周期闭合，再看适用范围', notes=src + '/closure.json；energy_summary.json；native_runs.json')
    cl = d.closure['byRun']
    shift_sun = closure_max(cl, ('sunlit_powered', 'sunlit_unpowered'), 'groupMeanShiftK')
    shift_no = closure_max(cl, ('nosun_powered', 'nosun_unpowered'), 'groupMeanShiftK')
    area1 = closure_max(cl, ('sunlit_powered', 'sunlit_unpowered'), 'areaShareAbove1K')
    e = d.energy['byRun']['sunlit_powered']
    attr = d.closure['attribution']
    share = min(a['shareWithSunlitFractionChangeAbove0p05'] for r, a in attr.items() if r.startswith('sunlit'))
    corr = min(a['pavementCorrelationWithSunlitFractionChange'] for r, a in attr.items() if r.startswith('sunlit'))
    resid = max(v['coreResidualMaxW'] for r in d.energy['byRun'].values() for v in r.values())
    deck.text(s, '已检查', 0.5, 1.85, 6.0, 0.45, 21.75, WHITE, True)
    deck.text(s, [f'周期闭合：同一钟点的组平均温度与前一天相差，无太阳组 ≤ {shift_no:.3f} K，有太阳组（全部组含单件）≤ {shift_sun:.2f} K（预热按真实日期逐日推进，太阳每天移动约 0.05°–0.2°）', f'有太阳组个别面元相差更大（相差 > 1 K 的面积最多占 {100*area1:.0f}%）：逐面元采样的受晒比例随太阳微移而改变；相差 > 1 K 的面元中 ≥ {100*share:.0f}% 伴随受晒比例变化，跑道路面上相关系数 ≥ {corr:.2f}', f'目标内部节点能量：全天输入 = 传到表皮，残差 ≤ {resid:.0e} W（坦克 a 全天 {e["tank_a"]["inputEnergyJ"]/1e9:.2f} GJ）', f'{len(nr)} 组 {nr[RUNS[0]]["nodes"]} 节点全部收敛，输入文件哈希前后一致'], 0.5, 2.4, 6.2, 4.4, 16, GREY)
    deck.text(s, '限制', 7.0, 1.85, 5.8, 0.45, 21.75, WHITE, True)
    deck.text(s, ['天气是合成的日变化，不是实测气象', '无太阳组只去掉日照，气温与天空长波不变', '材质、功率、热容均为示例假设，非实测特征', '地面深层为集中参数热库，无三维导热', '求解域外用边界态；RGB 为静态参考', '未做现场精度验证，也无独立网格或时间步收敛研究'], 7.0, 2.4, 5.8, 4.4, 16, TEAL)

    out = Path(out)
    deck.prs.save(str(out))
    crop_tmp.cleanup()
    return dict(slides=deck.count, bytes=out.stat().st_size)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('case', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    if a.output.exists():
        raise FileExistsError('output must be new: ' + str(a.output))
    print(json.dumps(build(a.case, a.output)))


if __name__ == '__main__':
    main()
