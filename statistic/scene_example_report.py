"""Render the actual building/forest demo outputs using numpy and Pillow."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from histream_statistics import colorize, draw_colorbar, load_font, read_structure


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    destination = args.directory.resolve()
    scenes = []
    rows = []
    for key, title, groups in [('building', '建筑场景 · 单栋房屋', [(1, '土壤/背景'), (3, '建筑')]),
                               ('forest', '森林场景 · 9 株简化树木', [(1, '土壤/背景'), (2, '植被')])]:
        files = sorted((destination / key / 'output/process').glob('faceteb_T=*.json'))
        if len(files) != 1:
            raise ValueError(f'{key}: expected exactly one energy timestep, got {len(files)}')
        structure = read_structure(files[0], 'mean')
        if structure.components is None:
            raise ValueError(f'{key}: missing component labels')
        fields = {field.identifier: field.values for field in structure.fields}
        temperature = fields['temperature'] - 273.15
        geometry_path = files[0].parent / structure.metadata.get('geometryFile', '../faceteb.json')
        geometry = json.loads(geometry_path.read_text(encoding='utf-8'))
        triangles = np.asarray(geometry['vertexPositions']).reshape(-1, 3, 3)
        assert len(triangles) == len(temperature) == len(structure.components)
        for component, label in groups:
            mask = structure.components == component
            for field, values in [('temperatureC', temperature), *[(f, fields[f]) for f in
                                  ('netRadiation', 'sensibleHeat', 'latentHeat', 'surfaceHeatFlux')]]:
                valid = values[mask & np.isfinite(values)]
                if not len(valid):
                    raise ValueError(f'{key}: no valid {label}/{field} data')
                rows.append(dict(scene=key, component=label, field=field,
                                 unit='degC' if field == 'temperatureC' else 'W/m2',
                                 count=len(valid), invalid_count=int(mask.sum()) - len(valid),
                                 mean=float(valid.mean()), minimum=float(valid.min()),
                                 maximum=float(valid.max()), standard_deviation=float(valid.std())))
        scenes.append((key, title, groups, structure, temperature, triangles))

    canvas = Image.new('RGB', (1800, 1430), '#eef3f7')
    draw = ImageDraw.Draw(canvas)
    def text(x, y, message, size=23, fill='#20394b', bold=False):
        draw.text((x, y), message, font=load_font(size, bold), fill=fill)

    text(40, 24, '建筑与森林 | 温度、能量组分统计', 38, bold=True)
    text(40, 79, 'FacetEB · 24 × 24 m · DOY 213 12:15 · 单时刻演示 · 面元正反面均值', 23)
    low = float(np.floor(min(scene[4].min() for scene in scenes)))
    high = float(np.ceil(max(scene[4].max() for scene in scenes)))
    palette = {1: '#ab7536', 2: '#248866', 3: '#456ac2'}
    for index, (key, title, groups, structure, temperature, triangles) in enumerate(scenes):
        x = 30 + index * 885
        draw.rounded_rectangle((x, 128, x + 855, 1280), radius=16, fill='white')
        text(x + 22, 147, title, 29, bold=True)
        text(x + 22, 191, '三维面元温度（℃）· 两场景共用色标', 21)

        # Y-up geometry; fit an orthographic oblique view and paint far to near.
        center = (triangles.min(axis=(0, 1)) + triangles.max(axis=(0, 1))) / 2
        points = triangles - center
        yaw, elevation = np.radians([38, 30])
        u = points[..., 0] * np.cos(yaw) - points[..., 2] * np.sin(yaw)
        depth = points[..., 0] * np.sin(yaw) + points[..., 2] * np.cos(yaw)
        v = points[..., 1] * np.cos(elevation) - depth * np.sin(elevation)
        camera_depth = depth * np.cos(elevation) + points[..., 1] * np.sin(elevation)
        scale = min(790 / max(float(np.ptp(u)), 1e-9), 370 / max(float(np.ptp(v)), 1e-9))
        px = x + 427 + (u - (u.min() + u.max()) / 2) * scale
        py = 440 - (v - (v.min() + v.max()) / 2) * scale
        colors = colorize((temperature - low) / (high - low))
        for face in np.argsort(camera_depth.mean(axis=1)):
            draw.polygon(list(zip(px[face], py[face])), fill=tuple(int(c) for c in colors[face]))
        draw_colorbar(canvas, draw, x + 28, 659, 798, 15, low, high)

        text(x + 22, 705, '温度分布 · 每组内面元比例（%）', 24, bold=True)
        left, right, top, bottom = x + 72, x + 810, 794, 962
        edges = np.linspace(low, high, 29)
        histogram_data = []
        for component, label in groups:
            values = temperature[structure.components == component]
            histogram_data.append((component, label, np.histogram(values, edges)[0] * 100 / len(values)))
        ymax = max(10, np.ceil(max(float(hist.max()) for _, _, hist in histogram_data) / 10) * 10)
        for tick in range(5):
            y = bottom - tick * (bottom - top) / 4
            draw.line((left, y, right, y), fill='#e0e8ef', width=1)
            text(left - 53, y - 12, f'{ymax * tick / 4:.0f}', 18)
        for component, label, hist in histogram_data:
            line = []
            for j, count in enumerate(hist):
                y = bottom - float(count) / ymax * (bottom - top)
                line.extend([(left + j / len(hist) * (right - left), y),
                             (left + (j + 1) / len(hist) * (right - left), y)])
            draw.line(line, fill=palette[component], width=3)
        for tick in range(5):
            value = low + tick * (high - low) / 4
            text(left + tick * (right - left) / 4 - 15, bottom + 8, f'{value:g}', 18)
        for j, (component, label) in enumerate(groups):
            n = int((structure.components == component).sum())
            text(x + 25 + j * 395, 753, f'{label}：{n:,} 面元', 21, palette[component])

        text(x + 22, 1010, '组分均值 · 通量单位 W/m²', 24, bold=True)
        columns = [(22, '组分'), (185, '温度℃'), (315, '净辐射'), (445, '显热'), (560, '潜热'), (675, '表面热通量')]
        for offset, header in columns:
            text(x + offset, 1060, header, 19)
        for j, (component, label) in enumerate(groups):
            y = 1105 + j * 43
            text(x + 22, y, label, 21, palette[component])
            for (offset, _), field in zip(columns[1:], ['temperatureC', 'netRadiation', 'sensibleHeat', 'latentHeat', 'surfaceHeatFlux']):
                row = next(r for r in rows if r['scene'] == key and r['component'] == label and r['field'] == field)
                text(x + offset, y, f"{row['mean']:.2f}", 21)
        text(x + 22, 1213, '全量统计；未按面积加权；不存在的组分不画图。', 20, '#617381')
    text(40, 1305, '说明：单节点、默认物化参数；森林 OBJ 整体赋予植被属性，未细分木质部。', 23)
    text(40, 1349, '仅验证输出与统计展示，不代表实测标定或长期热状态；CSV/TXT 和各组分布图随附。', 23)
    canvas.save(destination / '建筑与森林统计对照.png')
    with (destination / '建筑与森林统计汇总.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    notes = ['建筑与森林组分统计演示', 'FacetEB；DOY213 12:15；24×24m；64样本；深度3；关闭周期邻域与流体。',
             '每个场景只跑一个节点：检查统计功能，不是实测验证或长期预热结果。',
             '面元正反面取均值；按面元数量统计，不按面积加权。温度℃；能量通量W/m²。',
             '森林9株简化树木，整株使用leaf_c3/green_leaf；未细分树干与叶片。',
             '三维图使用完整三角面和统一温度色标；直方图统计全部面元。',
             '各场景 project.json 可在当前机器打开；inputs 中保存模型、位置和气象副本。',
             'project.json 使用绝对路径，搬到其他位置后需重新指定输入与输出路径。',
             'output 保存原始结果；statistics 保存全部组分CSV、TXT、三维点图和直方图。', '']
    for row in rows:
        notes.append(f"{row['scene']} | {row['component']} | {row['field']}: 均值={row['mean']:.3f}, 范围=[{row['minimum']:.3f}, {row['maximum']:.3f}] {row['unit']}, N={row['count']}")
    (destination / '说明与统计摘要.txt').write_text('\n'.join(notes), encoding='utf-8-sig')
    print(destination / '建筑与森林统计对照.png')


if __name__ == '__main__':
    main()
