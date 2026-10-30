"""Plot native FacetEB temperature rasters and area-weighted component series.

Run with the research WSL Python: plot-faceteb-day.py EXPORT_DIR.
This exporter supports the single-placement OBJ objects of this maize case.
"""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
import numpy as np
import pandas as pd
from osgeo import gdal
from PIL import Image


def host_path(value):
    value = str(value).replace('\\', '/')
    if re.match(r'^[A-Za-z]:/', value):
        return Path('/mnt/' + value[0].lower() + value[2:])
    return Path(value)


def object_triangles(item, scene):
    vertices, faces = [], []
    with host_path(item['fileName']).open() as stream:
        for line in stream:
            if line.startswith('v '):
                vertices.append([float(x) for x in line.split()[1:4]])
            elif line.startswith('f '):
                ids = [int(x.split('/')[0]) for x in line.split()[1:]]
                ids = [i - 1 if i > 0 else len(vertices) + i for i in ids]
                faces.extend([[ids[0], ids[k], ids[k + 1]] for k in range(1, len(ids) - 1)])
    placement = np.loadtxt(host_path(item['positionFile']), ndmin=2)
    if placement.shape != (1, 5) or placement[0, 3] != 1 or placement[0, 4] != 0:
        raise ValueError('Component identification needs one unscaled, unrotated placement')
    p = placement[0]
    triangles = np.asarray(vertices, dtype=np.float32)[faces]
    triangles += np.array([p[0] - scene['x'] / 2, p[2], p[1] - scene['y'] / 2], dtype=np.float32)
    normal = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    return triangles[np.sum(normal * normal, axis=1) > np.float32(1e-16)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('export_dir', type=host_path)
    args = parser.parse_args()
    out = args.export_dir
    manifest = json.loads((out / 'export-manifest.json').read_text())
    source = host_path(manifest['sourceRun'])
    project = json.loads((source / 'project.json').read_text())['configuration']
    geo = json.loads((source / 'output/faceteb.json').read_text())
    vertices = np.asarray(geo['vertexPositions'], dtype=float).reshape(-1, 3, 3)
    area = np.linalg.norm(np.cross(vertices[:, 1] - vertices[:, 0], vertices[:, 2] - vertices[:, 0]), axis=1) / 2
    groups, offset = {}, 0
    for item in project['objects']['items']:
        triangles = object_triangles(item, project['scene'])
        end = offset + len(triangles)
        if not np.allclose(vertices[offset:end], triangles, rtol=0, atol=1e-6):
            raise ValueError('Native geometry order disagrees with object: ' + item['name'])
        groups[item['name']] = np.arange(offset, end)
        offset = end
    groups['background'] = np.arange(offset, len(vertices))
    if len(groups['background']) != 2 or len(vertices) != geo['facetCount']:
        raise ValueError('Unexpected native background or facet count')
    gdal.UseExceptions()
    font = Path('/mnt/c/Windows/Fonts/msyh.ttc')
    if font.exists():
        font_manager.fontManager.addfont(str(font))
        plt.rcParams['font.family'] = font_manager.FontProperties(fname=str(font)).get_name()
    plt.rcParams.update({'axes.unicode_minus': False, 'font.size': 10, 'pdf.fonttype': 42})
    nodes = manifest['nodes']
    if len(nodes) != 48 or [n['node'] for n in nodes] != list(range(48)):
        raise ValueError('Expected 48 consecutive half-hour nodes')
    weather = np.loadtxt(host_path(project['meteo']['path']), skiprows=1)
    images, rows = [], []
    for node in nodes:
        token = node['token']
        binary = source / 'output/.facet_steps' / ('energy_T=' + token + '.bin')
        if hashlib.sha256(binary.read_bytes()).hexdigest() != node['sourceStepSha256']:
            raise ValueError('Original step was changed: ' + token)
        data = np.fromfile(binary, dtype='<f4').reshape(len(vertices), 2, -1)
        if data.shape[2] != 17 or not np.isfinite(data).all():
            raise ValueError('Invalid step field layout or values')
        node['tiff'] = node['tiff'].replace('\\', '/')
        tif = gdal.Open(str(out / node['tiff']))
        image = tif.ReadAsArray().astype(float) - 273.15
        if image.shape != (512, 512) or not np.isfinite(image).any():
            raise ValueError('Invalid exported raster')
        images.append(image)
        hour = int(token[-5:-3]) + int(token[-2:]) / 60
        row = {'node': node['node'], 'time': token[-5:].replace('-', ':'), 'hour_local': hour,
               'DOY': 160, 'air_temperature_C': float(weather[node['node'], 1]),
               'incoming_shortwave_W_m2': float(weather[node['node'], 5]),
               'incoming_longwave_W_m2': float(weather[node['node'], 6]),
               'image_mean_C': float(np.nanmean(image)), 'image_min_C': float(np.nanmin(image)),
               'image_max_C': float(np.nanmax(image))}
        for name, ids in groups.items():
            # Objects store a whole-object/leaf flux on side 0; do not double-count sides.
            # Native background soil uses its upward-facing side.
            side = np.zeros(len(ids), dtype=int)
            if name == 'background':
                normal = np.cross(vertices[ids, 1] - vertices[ids, 0], vertices[ids, 2] - vertices[ids, 0])
                side = (normal[:, 1] < 0).astype(int)
            values = data[ids, side].astype(float)
            weights = area[ids]
            for key, col in [('temperature_C', 0), ('Rn_W_m2', 4), ('H_W_m2', 5),
                             ('LE_W_m2', 6), ('storage_W_m2', 7), ('sunlit_fraction', 8),
                             ('sunlit_temperature_C', 9), ('shaded_temperature_C', 10)]:
                v = values[:, col]
                if 'temperature' in key:
                    v = v - 273.15
                row[name + '_' + key] = float(np.average(v, weights=weights))
            # Conditional means use the sunlit/shaded one-sided area in each node.
            # The stored mask describes geometric exposure even at night.
            # With zero shortwave or a sun below the horizon, all area is unlit.
            fraction = values[:, 8] if row['incoming_shortwave_W_m2'] > 1e-6 and node['solarZenith'] < 90 else np.zeros(len(ids))
            row[name + '_effective_sunlit_fraction'] = float(np.average(fraction, weights=weights))
            for key, col, w in [('sunlit', 9, weights * fraction), ('shaded', 10, weights * (1 - fraction))]:
                row[name + '_' + key + '_conditional_temperature_C'] = float(np.average(values[:, col] - 273.15, weights=w)) if w.sum() > 1e-10 else np.nan
        rows.append(row)
    table = pd.DataFrame(rows)
    table.to_csv(out / 'timeseries.csv', index=False, encoding='utf-8-sig', float_format='%.7f')
    stack = np.stack(images)
    vmin, vmax = np.floor(np.nanmin(stack)), np.ceil(np.nanmax(stack))
    cmap = plt.get_cmap('turbo').copy()
    cmap.set_bad('#eeeeee')
    png_dir = out / 'png'
    png_dir.mkdir(exist_ok=True)
    for i, (node, image) in enumerate(zip(nodes, images)):
        fig, ax = plt.subplots(figsize=(7.2, 6.3), dpi=160, layout='constrained')
        im = ax.imshow(image, cmap=cmap, vmin=vmin, vmax=vmax, extent=[0, 5, 0, 5])
        ax.set(xlabel='场景横向距离 [m]', ylabel='场景纵向距离 [m]',
               title=f"FacetEB 玉米场景 · DOY 160 · {rows[i]['time']}\n俯视表面温度（合成晴天气象）")
        fig.colorbar(im, ax=ax, label='表面温度 [°C]')
        file = png_dir / (node['token'] + '.png')
        fig.savefig(file)
        plt.close(fig)
        node['png'] = str(file.relative_to(out))
    fig, axes = plt.subplots(6, 8, figsize=(22, 17), dpi=140)
    for ax, image, row in zip(axes.flat, images, rows):
        im = ax.imshow(image, cmap=cmap, vmin=vmin, vmax=vmax)
        ax.set_title(row['time'], fontsize=12)
        ax.set_axis_off()
    fig.subplots_adjust(left=.015, right=.92, top=.90, bottom=.02, wspace=.07, hspace=.16)
    fig.colorbar(im, cax=fig.add_axes([.94, .12, .014, .7]), label='表面温度 [°C]')
    fig.suptitle('FacetEB 玉米场景：48 个半小时时刻的俯视温度图\nDOY 160 · 同一色标 · 合成晴天气象', fontsize=19)
    fig.savefig(out / 'temperature_48_overview.png')
    plt.close(fig)
    fig, axes = plt.subplots(2, 3, figsize=(18, 10.2), dpi=160, layout='constrained')
    t = table.hour_local
    for name, label, color in [('leaf', '叶片', '#20948b'), ('stem', '茎', '#ba6b35'), ('soil', '地表网格', '#bc3158'), ('background', '底层土壤列', '#6e66ad')]:
        axes[0, 0].plot(t, table[name + '_temperature_C'], label=label, color=color, lw=1.8, ls='--' if name == 'background' else '-')
    axes[0, 0].plot(t, table.air_temperature_C, ':', color='black', label='气温')
    axes[0, 0].set(title='面元面积加权平均温度', ylabel='温度 [°C]')
    axes[0, 1].plot(t, table.leaf_sunlit_conditional_temperature_C, color='#e88a21', label='受光叶片')
    axes[0, 1].plot(t, table.leaf_shaded_conditional_temperature_C, color='#4477aa', label='阴影叶片')
    axes[0, 1].set(title='按受光／阴影面积加权的叶温', ylabel='温度 [°C]')
    axes[0, 2].plot(t, table.incoming_shortwave_W_m2, label='入射短波', color='#e88a21')
    axes[0, 2].plot(t, table.incoming_longwave_W_m2, label='入射长波', color='#745596')
    axes[0, 2].set(title='合成气象辐射驱动', ylabel='辐射 [W/m²]')
    for ax, name, label in zip(axes[1], ['leaf', 'stem', 'soil'], ['叶片', '茎', '地表网格']):
        for col, text, color in [('Rn', '净辐射', '#c64a46'), ('H', '显热', '#df9128'), ('LE', '潜热', '#237eb1'), ('storage', '储热', '#5c995c')]:
            ax.plot(t, table[name + '_' + col + '_W_m2'], label=text, color=color)
        ax.axhline(0, color='gray', lw=.6)
        ax.set(title=label + '能量通量（按单面面元面积平均）', ylabel='通量 [W/m²]')
    for ax in axes.flat:
        ax.set_xlim(0, 24)
        ax.set_xticks(range(0, 25, 3))
        ax.set_xlabel('当地时间 [h，UTC+8]')
        ax.grid(alpha=.22)
        ax.legend(fontsize=8)
    fig.suptitle('FacetEB 玉米场景的日内变化 · 48 个半小时时刻\n合成晴天气象；地表 OBJ 网格与额外底层土壤列分别统计', fontsize=16)
    fig.savefig(out / 'time_variation.png')
    fig.savefig(out / 'time_variation.pdf')
    plt.close(fig)
    thumbs = []
    for node in nodes:
        with Image.open(out / node['png']) as frame:
            frame.thumbnail((720, 630))
            thumbs.append(frame.convert('RGB').copy())
    thumbs[0].save(out / 'temperature_day.gif', save_all=True, append_images=thumbs[1:], duration=220, loop=0)
    manifest.update({'colorRange_C': [float(vmin), float(vmax)], 'pngCount': len(nodes),
                     'originalStepHashesVerified': True, 'componentFacetCounts': {k: len(v) for k, v in groups.items()},
                     'componentAreas_m2': {k: float(area[v].sum()) for k, v in groups.items()},
                     'syntheticWeather': True, 'temperatureMeaning': 'Native sensor surface temperature; no atmosphere correction. Not a retrieved satellite brightness temperature.'})
    (out / 'export-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
    figures = ''.join(f'<figure><img loading="lazy" src="{n["png"]}"><figcaption>{r["time"]}</figcaption></figure>' for n, r in zip(nodes, rows))
    (out / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>FacetEB 48 时刻结果</title><style>body{font-family:system-ui;margin:24px;background:#f5f5f5}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px}figure{margin:0;background:white}img{max-width:100%;height:auto}figcaption{text-align:center;padding:8px}</style><h1>FacetEB 玉米场景：48 时刻温度图</h1><p>DOY160，00:00–23:30，UTC+8；合成晴天气象，统一色标。TIFF 单位为 K，PNG 单位为 °C。</p><p><a href="timeseries.csv">下载曲线数据</a> · <a href="time_variation.pdf">下载曲线 PDF</a></p><img src="time_variation.png"><main>' + figures + '</main>', encoding='utf-8')
    (out / 'README.md').write_text('# 玉米 FacetEB 48 时刻图像\n\n- 48 张原生 512×512 浮点温度 TIFF（K），以及 48 张统一色标 PNG（°C）。\n- temperature_48_overview.png 为总览；temperature_day.gif 为动画；time_variation.png/PDF 为时间曲线；timeseries.csv 为全部曲线数据；index.html 为本地浏览页。\n- 采用已完成 GPU 运行的逐时刻结果，不重新求解；原始 48 个二进制文件的 SHA256 均已核对。\n- 地表 OBJ 与附加底层土壤列分别统计，避免重叠面积重复计入。叶片／茎身份按导入 OBJ 顺序及逐三角形坐标匹配确认，不能仅用 Vegetation 分量编号区分。温度与通量按面元单面面积加权，叶片通量不将正反两面重复相加。\n- 受光／阴影叶温按各自面积分数加权；无受光面积时留空，不绘制虚构受光叶温。\n- DOY160、半小时步长、合成晴天气象；这是仿真结果，不是实测温度。温度图采用原生 HiStream 传感器出图路径及逐像素受光／阴影叶温，大气校正关闭。\n', encoding='utf-8')
    for node in nodes:
        with Image.open(out / node['png']) as image:
            image.verify()
    archive = out.with_suffix('.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for file in out.rglob('*'):
            if file.is_file():
                bundle.write(file, out.name + '/' + str(file.relative_to(out)))
    print(json.dumps({'output': str(out), 'pngCount': len(nodes), 'tiffCount': len(list((out / 'tiff').glob('*.tif'))), 'range_C': [vmin, vmax], 'csvRows': len(table), 'archive': str(archive)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
