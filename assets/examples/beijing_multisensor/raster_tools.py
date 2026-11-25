#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""北京多传感器案例：栅格统计、角度曲线、300 m → 1 km 聚合与参考结果导出。

由 run_all.mjs 调用（也可单独运行）：
    python -I raster_tools.py analyze <multisensor-manifest.json> --sensors <sensors.json>
                              --results <results 目录> [--reference <reference 目录>]

依赖：numpy、osgeo（GDAL ≥ 3.11，写 float16 需要；更旧的 GDAL 自动退回 uint16×10 缩放存储）。

数据流
    multisensor-manifest.json（各次运行的输出目录、退出码、耗时）
      ├─ 传感器运行 ──> results/summary.csv（光学反射率、NDVI、热红外亮温的统计）
      ├─ 角度运行   ──> results/angles.csv（亮温随 VZA/VAA 变化，相对天底的差）
      ├─ landsat89 / modis ──> results/aggregation.csv（300 m 与 1 km 聚合对比）
      └─ （可选）reference/（精简参考：组分温度、float16 的全部光学反射率与热红外亮温、汇总）
"""
import argparse
import csv
import json
import math
import os
import shutil
import sys

import numpy as np
from osgeo import gdal

gdal.UseExceptions()

H, C, KB = 6.62607015e-34, 2.99792458e8, 1.380649e-23


def read_json(path):
    with open(path, 'r', encoding='utf-8-sig') as handle:
        return json.load(handle)


def fmt(value):
    """CSV 数值格式：缺失/NaN 写空，其余保留 5 位小数。"""
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return ''
    if isinstance(value, (int, np.integer)):
        return str(int(value))
    if isinstance(value, str):
        return value
    return f'{float(value):.5f}'


def write_csv(path, header, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8-sig', newline='') as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        for row in rows:
            writer.writerow([fmt(row.get(name)) for name in header])


def read_bands(path):
    """读取多波段 GeoTIFF，返回 (名称列表, 中心波长列表, 数组 [n,h,w] float32, 地理变换, 投影)。"""
    ds = gdal.Open(path)
    names, centers, arrays = [], [], []
    for index in range(1, ds.RasterCount + 1):
        band = ds.GetRasterBand(index)
        description = band.GetDescription()
        names.append(description.split(' ')[0])
        centers.append(description.split('@')[-1].strip() if '@' in description else '')
        array = band.ReadAsArray().astype(np.float32)
        nodata = band.GetNoDataValue()
        if nodata is not None and np.isfinite(nodata):
            array[array == nodata] = np.nan
        arrays.append(array)
    result = (names, centers, np.stack(arrays), ds.GetGeoTransform(), ds.GetProjection())
    ds = None
    return result


def stats(values):
    """有效像元（有限值）的统计量。"""
    valid = values[np.isfinite(values)]
    if valid.size == 0:
        return {}
    q = np.percentile(valid, [5, 25, 50, 75, 95])
    return {'n_valid': valid.size, 'mean': float(valid.mean()), 'std': float(valid.std()), 'min': float(valid.min()),
            'p05': float(q[0]), 'p25': float(q[1]), 'p50': float(q[2]), 'p75': float(q[3]), 'p95': float(q[4]), 'max': float(valid.max())}


def planck(wavelength_um, temperature):
    """黑体光谱辐亮度 W m-2 sr-1 m-1。"""
    wavelength = wavelength_um * 1e-6
    return 2 * H * C ** 2 / wavelength ** 5 / np.expm1(H * C / (wavelength * KB * temperature))


def inverse_planck(wavelength_um, radiance):
    """由光谱辐亮度求亮温（K）。"""
    wavelength = wavelength_um * 1e-6
    return H * C / (wavelength * KB * np.log1p(2 * H * C ** 2 / wavelength ** 5 / radiance))


def band_wavelength(description):
    """从波段描述 'B10 brightness temperature (K) @ 10900 nm' 取波长（μm）。"""
    return float(description.split('@')[-1].replace('nm', '').strip()) / 1000.0


def load_run(run):
    """读取一次运行的光学与热红外波段。返回 dict：optical/thermal 各为 (名称, 波长μm, 数组)。"""
    out = {'optical': None, 'thermal': None, 'geo': None}
    for key, name in (('optical', 'optical_reflectance.tif'), ('thermal', 'tir_brightness_temperature.tif')):
        path = os.path.join(run['dir'], name)
        if not os.path.exists(path):
            continue
        names, centers, array, geo, projection = read_bands(path)
        wavelengths = [float(center.replace('nm', '').strip()) / (1000.0 if key == 'thermal' else 1.0) for center in centers]
        out[key] = (names, wavelengths, array)
        out['geo'] = (geo, projection)
    return out


def scene_view(project_path):
    scene = read_json(project_path)['scene']
    return float(scene['view']['vza']['value']), float(scene['view']['vaa']['value'])


def sun_geometry(parent_dir):
    path = os.path.join(parent_dir, 'solar_geometry.tif')
    if not os.path.exists(path):
        return None, None
    _, _, array, _, _ = read_bands(path)
    return float(np.nanmean(array[0])), float(np.nanmean(array[1]))


def sensor_rows(runs, sensors):
    """summary.csv 的行：光学波段、NDVI、热红外波段。"""
    rows = []
    sensor_by_id = {sensor['id']: sensor for sensor in sensors}
    for run in runs:
        if run['kind'] not in ('sensor', 'parent'):
            continue
        base = {'sensor': run['id'], 'exit_code': run['exitCode'], 'seconds': run.get('seconds')}
        if run['exitCode'] != 0:
            rows.append({**base, 'kind': 'failed'})
            continue
        data = load_run(run)
        if data['optical']:
            names, wavelengths, array = data['optical']
            for name, wavelength, band in zip(names, wavelengths, array):
                rows.append({**base, 'kind': 'optical', 'band': name, 'center': wavelength, 'unit': 'nm', **stats(band)})
            spec = (sensor_by_id.get(run['id']) or {}).get('ndvi') or ({'red': 'B4', 'nir': 'B5'} if run['id'] == 'parent' else None)
            if spec and spec['red'] in names and spec['nir'] in names:
                red, nir = array[names.index(spec['red'])], array[names.index(spec['nir'])]
                with np.errstate(invalid='ignore', divide='ignore'):
                    ndvi = (nir - red) / (nir + red)
                rows.append({**base, 'kind': 'ndvi', 'band': f"NDVI({spec['red']},{spec['nir']})", **stats(ndvi)})
        if data['thermal']:
            names, wavelengths, array = data['thermal']
            for name, wavelength, band in zip(names, wavelengths, array):
                rows.append({**base, 'kind': 'thermal', 'band': name, 'center': wavelength, 'unit': 'um', **stats(band)})
    return rows


def angle_rows(runs, sensors, saa):
    """angles.csv 的行：亮温的统计随观测角变化，及相对天底的差。"""
    rows = []
    for run in runs:
        if run['kind'] != 'angle':
            continue
        vza, vaa = scene_view(run['project'])
        parts = os.path.basename(run['project'])[len('angles_'):-len('.json')].split('_vza')
        sensor_key = parts[0]
        base = {'experiment': sensor_key, 'run_id': run['id'], 'vza': vza, 'vaa': vaa, 'exit_code': run['exitCode'], 'seconds': run.get('seconds')}
        if saa is not None and vza > 0:
            relative = (vaa - saa + 540.0) % 360.0 - 180.0
            base['relation'] = 'back(hotspot side)' if abs(relative) < 90 else 'forward'
        else:
            base['relation'] = 'nadir'
        if run['exitCode'] != 0:
            rows.append(base)
            continue
        names, wavelengths, array = load_run(run)['thermal']
        rows.append({**base, 'band': names[0], 'center': wavelengths[0], **stats(array[0])})
    for experiment in sorted({row['experiment'] for row in rows}):
        group = [row for row in rows if row['experiment'] == experiment]
        nadir = next((row for row in group if row['vza'] == 0 and 'mean' in row), None)
        for row in group:
            if nadir and 'mean' in row:
                row['delta_mean_vs_nadir'] = row['mean'] - nadir['mean']
                row['delta_max_vs_nadir'] = row['max'] - nadir['max']
                row['delta_std_vs_nadir'] = row['std'] - nadir['std']
    rows.sort(key=lambda row: (row['experiment'], row['vza'], row['vaa']))
    return rows


def aggregate_grid(array, geo, projection, resolution, method):
    """用 GDAL warp 把栅格聚合到 resolution（m）网格（网格起点与原栅格相同，边缘格可能是部分覆盖）。"""
    height, width = array.shape
    src = gdal.GetDriverByName('MEM').Create('', width, height, 1, gdal.GDT_Float32)
    src.SetGeoTransform(geo)
    src.SetProjection(projection)
    src.GetRasterBand(1).WriteArray(array)
    src.GetRasterBand(1).SetNoDataValue(float('nan'))
    new_w = int(math.ceil(width * abs(geo[1]) / resolution))
    new_h = int(math.ceil(height * abs(geo[5]) / resolution))
    bounds = (geo[0], geo[3] - new_h * resolution, geo[0] + new_w * resolution, geo[3])
    dst = gdal.Warp('', src, format='MEM', outputBounds=bounds, xRes=resolution, yRes=resolution, resampleAlg=method,
                    srcNodata=float('nan'), dstNodata=float('nan'), outputType=gdal.GDT_Float32)
    result = dst.GetRasterBand(1).ReadAsArray().astype(np.float32)
    return result


def hot_fraction(values, threshold):
    valid = values[np.isfinite(values)]
    return float((valid >= threshold).sum() / valid.size) if valid.size else None


def aggregation_rows(runs, resolution=1000.0):
    """aggregation.csv：Landsat TIRS B10 与 MODIS 31 的亮温、几个光学波段与 NDVI 聚合到 1 km 的对比。"""
    rows = []
    targets = [('landsat89', 'thermal', ['B10']), ('landsat89', 'optical', ['B2', 'B4', 'B5', 'B6']),
               ('modis', 'thermal', ['B31']), ('modis', 'optical', ['B1', 'B2', 'B6'])]
    by_id = {run['id']: run for run in runs if run['exitCode'] == 0}
    for sensor, kind, wanted in targets:
        if sensor not in by_id:
            continue
        data = load_run(by_id[sensor])
        geo, projection = data['geo']
        products = []
        if data[kind]:
            names, wavelengths, array = data[kind]
            products += [(name, wavelength, array[names.index(name)], kind) for name, wavelength in zip(names, wavelengths) if name in wanted]
        if kind == 'optical' and data['optical']:
            names, _, array = data['optical']
            red, nir = ('B4', 'B5') if sensor == 'landsat89' else ('B1', 'B2')
            with np.errstate(invalid='ignore', divide='ignore'):
                products.append((f'NDVI({red},{nir})', None, (array[names.index(nir)] - array[names.index(red)]) / (array[names.index(nir)] + array[names.index(red)]), 'ndvi'))
        for name, wavelength, band, product_kind in products:
            mean_agg = aggregate_grid(band, geo, projection, resolution, 'average')
            max_agg = aggregate_grid(band, geo, projection, resolution, 'max')
            base = stats(band)
            thresholds = {'p95': base['p95'], '2s': base['mean'] + 2 * base['std']}
            row = {'sensor': sensor, 'band': name, 'kind': product_kind, 'resolution_m': resolution,
                   'n_300m': base['n_valid'], 'n_1km': int(np.isfinite(mean_agg).sum()),
                   'mean_300m': base['mean'], 'mean_1km_avg': float(np.nanmean(mean_agg)),
                   'std_300m': base['std'], 'std_1km_avg': float(np.nanstd(mean_agg)),
                   'max_300m': base['max'], 'max_1km_avg': float(np.nanmax(mean_agg)), 'max_1km_maxagg': float(np.nanmax(max_agg)),
                   'p95_300m': base['p95'], 'p95_1km_avg': float(np.nanpercentile(mean_agg, 95))}
            if product_kind == 'thermal':
                radiance = planck(wavelength, np.where(np.isfinite(band), band, np.nan).astype(np.float64)).astype(np.float64)
                radiance_agg = aggregate_grid(radiance.astype(np.float32) * 1e-6, geo, projection, resolution, 'average').astype(np.float64) * 1e6
                row['mean_1km_radiance_bt'] = float(np.nanmean(inverse_planck(wavelength, radiance_agg)))
                row['bias_radiance_vs_avg'] = row['mean_1km_radiance_bt'] - row['mean_1km_avg']
            for label, threshold in thresholds.items():
                row[f'hot_thr_{label}'] = threshold
                row[f'hot_frac_300m_{label}'] = hot_fraction(band, threshold)
                row[f'hot_frac_1km_avg_{label}'] = hot_fraction(mean_agg, threshold)
                row[f'hot_frac_1km_maxagg_{label}'] = hot_fraction(max_agg, threshold)
            rows.append(row)
    return rows


def write_float16(path, array, geo, projection, descriptions, full=False):
    """float16 压缩 GeoTIFF（GDAL < 3.11 时退回 uint16 = round(v×10)，nodata 65535）；full=True 存 float32（亮温差只有百分之几 K，float16 在 300 K 附近分辨率仅 0.25 K）。"""
    count, height, width = array.shape
    options = ['COMPRESS=DEFLATE', 'ZLEVEL=9', 'TILED=YES', 'BLOCKXSIZE=256', 'BLOCKYSIZE=256']
    if full:
        options.append('PREDICTOR=3')
        ds = gdal.GetDriverByName('GTiff').Create(path, width, height, count, gdal.GDT_Float32, options=options)
        ds.SetGeoTransform(geo)
        ds.SetProjection(projection)
        for index in range(count):
            band = ds.GetRasterBand(index + 1)
            band.SetDescription(descriptions[index])
            band.SetNoDataValue(float('nan'))
            band.WriteArray(array[index].astype(np.float32))
        ds.FlushCache()
        return 'float32'
    has_f16 = hasattr(gdal, 'GDT_Float16')
    ds = gdal.GetDriverByName('GTiff').Create(path, width, height, count, gdal.GDT_Float16 if has_f16 else gdal.GDT_UInt16, options=options)
    ds.SetGeoTransform(geo)
    ds.SetProjection(projection)
    for index in range(count):
        band = ds.GetRasterBand(index + 1)
        band.SetDescription(descriptions[index])
        if has_f16:
            band.SetNoDataValue(float('nan'))
            band.WriteArray(array[index].astype(np.float16))
        else:
            band.SetNoDataValue(65535)
            band.WriteArray(np.where(np.isfinite(array[index]), np.rint(array[index] * 10), 65535).astype(np.uint16))
    ds.FlushCache()
    ds = None
    return 'float16' if has_f16 else 'uint16 (value x 10)'


def write_reference(runs, sensors, reference_dir, results_dir):
    """reference/：组分温度（float32，供变体工程默认读取）、float16 的光学反射率与 float32 的热红外亮温、汇总。"""
    os.makedirs(reference_dir, exist_ok=True)
    by_id = {run['id']: run for run in runs if run['exitCode'] == 0}
    files = []
    if 'parent' in by_id:
        for name in ('components.tif', 'components.json', 'scene_summary.json'):
            src = os.path.join(by_id['parent']['dir'], name)
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(reference_dir, name if name != 'scene_summary.json' else 'parent_scene_summary.json'))
                files.append(name)
    for sensor in sensors:
        if sensor['id'] not in by_id:
            continue
        data = load_run(by_id[sensor['id']])
        geo, projection = data['geo']
        if data['optical']:
            names, wavelengths, array = data['optical']
            mode = write_float16(os.path.join(reference_dir, f"{sensor['id']}_optical.tif"), array, geo, projection,
                                 [f'{n} reflectance @ {w:g} nm' for n, w in zip(names, wavelengths)])
            files.append(f"{sensor['id']}_optical.tif ({mode})")
        if data['thermal']:
            names, wavelengths, array = data['thermal']
            mode = write_float16(os.path.join(reference_dir, f"{sensor['id']}_tir_bt.tif"), array, geo, projection,
                                 [f'{n} brightness temperature (K) @ {w * 1000:g} nm' for n, w in zip(names, wavelengths)], full=True)
            files.append(f"{sensor['id']}_tir_bt.tif ({mode})")
    for name in ('summary.csv', 'angles.csv', 'aggregation.csv'):
        if os.path.exists(os.path.join(results_dir, name)):
            shutil.copyfile(os.path.join(results_dir, name), os.path.join(reference_dir, name))
            files.append(name)
    with open(os.path.join(reference_dir, 'manifest.json'), 'w', encoding='utf-8') as handle:
        json.dump({'kind': 'beijing-multisensor-reference', 'note': '光学反射率为 float16、亮温为 float32 的栅格与 landsat_beijing_eb 同网格（EPSG:32650，300 m，592×598）；NaN 为无效像元',
                   'files': files}, handle, ensure_ascii=False, indent=2)
    return files


def printable(rows, sensors):
    """一屏摘要（由 run_all.mjs 原样打印）。"""
    lines = ['—— 摘要 ——']
    for run_id in dict.fromkeys(row['sensor'] for row in rows):
        group = [row for row in rows if row['sensor'] == run_id and 'mean' in row]
        optical = ' '.join(f"{row['band']}={row['mean']:.3f}" for row in group if row['kind'] == 'optical')
        ndvi = next((row['mean'] for row in group if row['kind'] == 'ndvi'), None)
        thermal = ' '.join(f"{row['band']}={row['mean']:.2f}K" for row in group if row['kind'] == 'thermal')
        seconds = next((row.get('seconds') for row in rows if row['sensor'] == run_id), 0) or 0
        lines.append(f"{run_id:<12} {seconds:5.1f}s | {optical}{' | NDVI=%.3f' % ndvi if ndvi is not None else ''}{' | ' + thermal if thermal else ''}")
    return lines


def analyze(args):
    manifest = read_json(args.manifest)
    sensors = read_json(args.sensors)['sensors']
    runs = manifest['runs']
    parent = next((run for run in runs if run['id'] == 'parent' and run['exitCode'] == 0), None)
    sza, saa = sun_geometry(parent['dir']) if parent else (None, None)
    summary = sensor_rows(runs, sensors)
    columns = ['sensor', 'kind', 'band', 'center', 'unit', 'n_valid', 'mean', 'std', 'min', 'p05', 'p25', 'p50', 'p75', 'p95', 'max', 'exit_code', 'seconds']
    write_csv(os.path.join(args.results, 'summary.csv'), columns, summary)
    angles = angle_rows(runs, sensors, saa)
    angle_columns = ['experiment', 'run_id', 'vza', 'vaa', 'relation', 'band', 'center', 'mean', 'std', 'min', 'p05', 'p50', 'p95', 'max',
                     'delta_mean_vs_nadir', 'delta_std_vs_nadir', 'delta_max_vs_nadir', 'n_valid', 'exit_code', 'seconds']
    write_csv(os.path.join(args.results, 'angles.csv'), angle_columns, angles)
    aggregation = aggregation_rows(runs)
    if aggregation:
        agg_columns = list(dict.fromkeys(key for row in aggregation for key in row))
        write_csv(os.path.join(args.results, 'aggregation.csv'), agg_columns, aggregation)
    lines = printable(summary, sensors)
    if sza is not None:
        lines.append(f'太阳天顶 {sza:.2f}°，方位 {saa:.2f}°（场景均值）')
    for experiment in sorted({row['experiment'] for row in angles}):
        curve = ' '.join(f"{row['vza']:g}/{row['vaa']:g}:{row['mean']:.2f}" for row in angles if row['experiment'] == experiment and 'mean' in row)
        lines.append(f'角度 {experiment}（VZA/VAA:亮温均值K）{curve}')
    for row in aggregation:
        if row['kind'] == 'thermal':
            lines.append(f"聚合 {row['sensor']} {row['band']}：均值 300m {row['mean_300m']:.2f} → 1km {row['mean_1km_avg']:.2f} K；最大 {row['max_300m']:.2f} → {row['max_1km_avg']:.2f} K；"
                         f"热点(P95)面积占比 {row['hot_frac_300m_p95']:.3f} → {row['hot_frac_1km_avg_p95']:.3f}")
    if args.reference:
        files = write_reference(runs, sensors, args.reference, args.results)
        total = sum(os.path.getsize(os.path.join(args.reference, name)) for name in os.listdir(args.reference))
        lines.append(f'参考结果：{len(files)} 个文件，{total / 1e6:.1f} MB → {args.reference}')
    print('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    analyze_parser = sub.add_parser('analyze')
    analyze_parser.add_argument('manifest')
    analyze_parser.add_argument('--sensors', required=True)
    analyze_parser.add_argument('--results', required=True)
    analyze_parser.add_argument('--reference')
    args = parser.parse_args()
    if args.command == 'analyze':
        analyze(args)


if __name__ == '__main__':
    sys.exit(main())
