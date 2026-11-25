#!/usr/bin/env python
# ==========================================================================
# 文件：test-forcing-metadata.py
# 功能：STREAM 引擎“元数据驱动气象强迫”机制的端到端测试（规格见 docs/forcing-metadata.md），
#       全部使用合成数据；缺少 numpy/netCDF4/GDAL 或未构建引擎时输出 SKIP 并以 0 退出
# 运行：python -I tools/test-forcing-metadata.py [--exe models/bin_x64/Release/stream.exe] [--keep]
# 主要模块/步骤：
#   1. 合成数据：空间均匀、带解析日变化的 ERA5 类强迫，写成多种形态
#      （原始 ERA5、ERA5-Land 累积量、改名/改单位变量、GLDAS 式 3 h 栅格、站点 CSV）
#   2. make_scene / run_case：12×12 像元的小场景，以 EB 模式运行 stream.exe，读 tir_surface_temperature.tif
#   3. main 中的检查：预设列表与校验、错误信息、ERA5 预设与缺省逐字节一致（sha256）、
#      改名/改单位还原、ERA5-Land 去累积、GLDAS 式 3 h、站点 CSV、场景摘要记录的元数据
#   4. 汇总失败项；退出码 0=全部通过，1=有失败
# 比较方式：各形态得到的地表温度与基准 ERA5 运行比较平均绝对差（阈值见各检查项）。
# 约定：温度 K（或 degC 视变量而定）；ERA5 辐射量为累积 J m-2，其他为 W m-2；
#       时间以“自 1970 起的整小时数”表示（FIRST / HOURS），场景覆盖 2019-08-14 起 48 h。
# ==========================================================================

"""Tests of the weather-forcing metadata mechanism of the STREAM engine (docs/forcing-metadata.md).

    python -I tools/test-forcing-metadata.py [--exe models/bin_x64/Release/stream.exe] [--keep]

Needs numpy, netCDF4 and GDAL's python bindings (the same ones the other tools use). Everything is synthetic: a spatially
uniform ERA5-like data set with an analytic diurnal cycle is written to a temporary folder in several guises (original ERA5
layout, ERA5-Land accumulation, renamed variables with other units, GLDAS-style 3-hourly, a station CSV) and the engine's
surface temperature from each is compared with the ERA5 run. No real GLDAS data are involved.
"""
import argparse, hashlib, json, math, os, subprocess, sys, tempfile
from pathlib import Path

try:
    import numpy as np
    import netCDF4 as nc
    from osgeo import gdal, osr
except ImportError as error:  # pragma: no cover
    print('SKIP: ' + str(error))
    sys.exit(0)

gdal.UseExceptions()
root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--exe', default=str(root / 'models' / 'bin_x64' / 'Release' / 'stream.exe'))
parser.add_argument('--keep', action='store_true')
options = parser.parse_args()
exe = Path(options.exe)
if not exe.exists():
    exe = exe.with_suffix('')
if not exe.exists():
    print('SKIP: engine not built: ' + options.exe)
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='forcing-meta-test-'))
failures = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + (' — ' + detail if detail else ''))
    if not ok:
        failures.append(name)


def engine(*args, cwd=None):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=cwd)
    return result.returncode, result.stdout + result.stderr


# ---------------------------------------------------------------- synthetic ERA5-like data (uniform in space)
# 合成数据的时间轴：2019-08-14 00 UTC 起连续 48 个整点（自 1970 起的小时数）；
# 空间网格 9×9，纬度 41→39°N，经度 115.5→117.5°E，各格点取同一值（空间均匀）。
FIRST = int(np.datetime64('2019-08-14T00:00:00', 's').astype('int64') // 3600)
HOURS = list(range(FIRST, FIRST + 48))
LAT = np.array([41.0 - 0.25 * i for i in range(9)])
LON = np.array([115.5 + 0.25 * j for j in range(9)])


# 作用：给定小时数 h，返回解析的气象场：气温（298 K ± 6 K 日变化）、露点（低 8 K）、气压 101000 Pa、
# 风 (2, 1.5) m s-1、短波/长波下行、地表温度（气温 + 2 K）、土壤水分 0.25。
# sun 为简化的日照因子（4.25 h 为峰值，半宽 7.25 h；时刻取小时区间中点，约定待核对）。
def fields(h):
    hod = h % 24
    t = 298.0 + 6.0 * math.sin(2 * math.pi * (hod - 9.0) / 24.0)
    centre = (h - 0.5) % 24
    sun = max(0.0, math.cos((centre - 4.25) / 7.25 * math.pi / 2)) if abs(centre - 4.25) < 7.25 else 0.0
    return dict(t2m=t, d2m=t - 8.0, sp=101000.0, u10=2.0, v10=1.5, sw=900.0 * sun, lw=380.0 + 20.0 * sun, skt=t + 2.0, swvl1=0.25)


# 饱和水汽压（hPa），输入摄氏度（Magnus 型公式 6.107·10^(7.5c/(237.3+c))）。
def es(c):
    return 6.107 * 10 ** (7.5 * c / (237.3 + c))


# 作用：写一个 netCDF：时间维 + 经纬度维 + 若干 (time, lat, lon) 变量，每个变量把时间序列广播到全部格点。
# variables 形如 {名: (数值序列, 单位)}。
def write_nc(path, tname, tunits, times, variables, lat='latitude', lon='longitude'):
    d = nc.Dataset(path, 'w')
    d.createDimension(tname, len(times)); d.createDimension(lat, len(LAT)); d.createDimension(lon, len(LON))
    d.createVariable(tname, 'f8', (tname,))[:] = times
    d.variables[tname].units = tunits
    d.createVariable(lat, 'f8', (lat,))[:] = LAT; d.createVariable(lon, 'f8', (lon,))[:] = LON
    for name, (values, unit) in variables.items():
        v = d.createVariable(name, 'f4', (tname, lat, lon))
        v.units = unit
        v[:] = np.broadcast_to(np.array(values, dtype='f4')[:, None, None], (len(values), len(LAT), len(LON)))
    d.close()


# 作用：写 ERA5 布局文件。land=False 时辐射为逐时累积量（W m-2 × 3600 → J m-2）；
# land=True 模拟 ERA5-Land：自 00 UTC 起累积，00 UTC 的记录属于前一天（累计 24 h）。
def era5_file(path, land=False):
    f = [fields(h) for h in HOURS]
    ssrd, strd = [], []
    for k, h in enumerate(HOURS):
        if land:  # accumulated from 00 UTC; the 00 UTC record carries the previous day (24 h)
            start = ((h - 1) // 24) * 24
            ssrd.append(sum(fields(x)['sw'] * 3600 for x in range(start + 1, h + 1)))
            strd.append(sum(fields(x)['lw'] * 3600 for x in range(start + 1, h + 1)))
        else:
            ssrd.append(f[k]['sw'] * 3600); strd.append(f[k]['lw'] * 3600)
    base = int(np.datetime64('1900-01-01', 's').astype('int64') // 3600)
    write_nc(path, 'valid_time', 'hours since 1900-01-01 00:00:00', [h - base for h in HOURS], {
        't2m': ([x['t2m'] for x in f], 'K'), 'd2m': ([x['d2m'] for x in f], 'K'), 'sp': ([x['sp'] for x in f], 'Pa'),
        'u10': ([x['u10'] for x in f], 'm s-1'), 'v10': ([x['v10'] for x in f], 'm s-1'), 'ssrd': (ssrd, 'J m-2'), 'strd': (strd, 'J m-2'),
        'skt': ([x['skt'] for x in f], 'K'), 'swvl1': ([x['swvl1'] for x in f], 'm3 m-3')})


# 作用：写同样内容但变量名、单位都不同的文件（degC、hPa、km/h、W m-2 ...），用于测试元数据还原。
def renamed_file(path):
    f = [fields(h) for h in HOURS]
    base = int(np.datetime64('2000-01-01', 's').astype('int64') // 3600)
    write_nc(path, 'valid_time', 'hours since 2000-01-01 00:00:00', [h - base for h in HOURS], {
        'Tair': ([x['t2m'] - 273.15 for x in f], 'degC'), 'Td': ([x['d2m'] - 273.15 for x in f], 'degC'), 'ps': ([x['sp'] / 100 for x in f], 'hPa'),
        'uwind': ([x['u10'] * 3.6 for x in f], 'km/h'), 'vwind': ([x['v10'] * 3.6 for x in f], 'km/h'),
        'SW': ([x['sw'] for x in f], 'W m-2'), 'LW': ([x['lw'] for x in f], 'W m-2'),
        'Tskin': ([x['skt'] for x in f], 'K'), 'SM': ([x['swvl1'] for x in f], 'm3 m-3')})


# 作用：写 GLDAS-Noah 风格的 3 小时数据（瞬时量取整点值，*_tavg 为前 3 小时均值，时间单位为分钟，
# Qair 由露点与气压换算，土壤水分由体积含水量换算为 kg m-2）。仅为合成，不代表真实 GLDAS 产品。
def gldas_file(path):
    keys = [h for h in HOURS if h % 3 == 0 and h - 2 >= FIRST]
    base = int(np.datetime64('2000-01-01', 's').astype('int64') // 3600)
    mean = lambda name, h: sum(fields(x)[name] for x in (h - 2, h - 1, h)) / 3.0
    write_nc(path, 'time', 'minutes since 2000-01-01 00:00:00', [(h - base) * 60 for h in keys], {
        'Tair_f_inst': ([fields(h)['t2m'] for h in keys], 'K'),
        'Qair_f_inst': ([0.622 * es(fields(h)['d2m'] - 273.15) / (fields(h)['sp'] / 100 - 0.378 * es(fields(h)['d2m'] - 273.15)) for h in keys], 'kg kg-1'),
        'Psurf_f_inst': ([fields(h)['sp'] for h in keys], 'Pa'), 'Wind_f_inst': ([math.hypot(fields(h)['u10'], fields(h)['v10']) for h in keys], 'm s-1'),
        'SWdown_f_tavg': ([mean('sw', h) for h in keys], 'W m-2'), 'LWdown_f_tavg': ([mean('lw', h) for h in keys], 'W m-2'),
        'AvgSurfT_inst': ([fields(h)['skt'] for h in keys], 'K'), 'SoilMoi0_10cm_inst': ([fields(h)['swvl1'] * 100 for h in keys], 'kg m-2')},
        lat='lat', lon='lon')


# 作用：写单站点 CSV（摄氏度、相对湿度 %、hPa、风速、下行短波/长波）；站点在 (40.0°N, 116.5°E, 50 m)。
# 参数 rows 未使用（待核对）。
def station_csv(path, rows=None, header='time,station_id,lat,lon,elev,temp,rh,pressure,wind_speed,swdown,lwdown'):
    lines = [header]
    for h in HOURS:
        f = fields(h)
        stamp = str(np.datetime_as_string(np.datetime64(h * 3600, 's'), unit='s')).replace('T', ' ') + 'Z'
        rh = 100 * es(f['d2m'] - 273.15) / es(f['t2m'] - 273.15)
        lines.append(f"{stamp},A,40.0,116.5,50,{f['t2m'] - 273.15:.5f},{rh:.5f},{f['sp'] / 100:.4f},{math.hypot(f['u10'], f['v10']):.5f},{f['sw']:.4f},{f['lw']:.4f}")
    Path(path).write_text('\n'.join(lines) + '\n', encoding='utf-8')


# ---------------------------------------------------------------- tiny scene
# 作用：生成 12×12 像元、300 m 分辨率（UTM 50N）的小场景：地类 12、LAI 2、DEM 50 m 三个 GeoTIFF。
def make_scene(folder):
    folder.mkdir(parents=True, exist_ok=True)
    srs = osr.SpatialReference(); srs.ImportFromEPSG(32650)
    for name, value, dtype in (('type', 12, gdal.GDT_Byte), ('lai', 2.0, gdal.GDT_Float32), ('dem', 50.0, gdal.GDT_Float32)):
        d = gdal.GetDriverByName('GTiff').Create(str(folder / (name + '.tif')), 12, 12, 1, dtype)
        d.SetGeoTransform((447000.0, 300.0, 0.0, 4419000.0, 0.0, -300.0)); d.SetProjection(srs.ExportToWkt())
        d.GetRasterBand(1).WriteArray(np.full((12, 12), value)); d = None


# 作用：组装 EB 模式的 stream 配置（TIRT 能量方案等固定选项）并运行，返回 (退出码, 输出文本, 输出目录)。
# temperature 字典覆盖/追加 temperature 配置（如 era5 数据包、forcing 元数据）。
def run_case(name, scene, temperature, overpass='2019-08-15T02:55:00Z'):
    out = work / 'runs' / name
    out.mkdir(parents=True, exist_ok=True)
    config = {
        'kind': 'stream-image-config', 'schemaVersion': 2, 'outputDir': str(out), 'dataDir': str(exe.parent / 'stream-defined'), 'overpass': overpass,
        'surface': {k: {'file': str(scene / (k + '.tif'))} for k in ('type', 'lai', 'dem')},
        'view': {'vza': {'value': 0}, 'vaa': {'value': 0}},
        'optical': {'enabled': False, 'bands': []},
        'thermal': {'enabled': True, 'bands': [{'name': 'B10', 'center': 10.9, 'fwhm': 0.6}]},
        'temperature': {'mode': 'eb', 'soilMoisture': {'source': 'era5'}, 'elevationCorrection': True,
                        'energy': {'scheme': 'tirt', 'soilHeatMethod': 'surface', 'temperatureMethod': 'robust', 'diffuseFraction': 0.2,
                                   'enforceSolarLimit': True, 'spectralBinNm': 50, 'soilThermal': 'constant', 'soilEmissivityModel': 'hapke',
                                   'surfaceTemperatureNormalisation': 'retrieval', 'soilResistance': {'method': 'constant', 'value': 200}}}, 'threads': 0}
    config['temperature'].update(temperature)
    (out / 'config.json').write_text(json.dumps(config), encoding='utf-8')
    code, text = engine('run', out / 'config.json')
    (out / 'log.txt').write_text(text, encoding='utf-8')
    return code, text, out


def surface(out):
    d = gdal.Open(str(out / 'tir_surface_temperature.tif'))
    return d.GetRasterBand(1).ReadAsArray().astype('f8')


def digest(out):
    return hashlib.sha256((out / 'tir_surface_temperature.tif').read_bytes()).hexdigest()


# 作用：调用引擎命令 era5-register 或 forcing-register 登记数据包，返回索引文件路径；失败则断言。
def register(index, *files, metadata=None, era5=False):
    args = ['era5-register', index, *files] if era5 else ['forcing-register', index, '--metadata', metadata, *files]
    code, text = engine(*args)
    assert code == 0, text
    return str(index)


# ---------------------------------------------------------------- tests
# 各检查按顺序运行：预设与校验 → 错误路径 → 1. ERA5 预设 = 缺省 → 2. 改名变量 → 3. ERA5-Land 去累积
# → 4. GLDAS 式 3 h → 5. 站点 CSV。第 1 项失败则后续不再运行。
def main():
    # presets and the checker
    code, text = engine('forcing-preset')
    check('preset list', code == 0 and all(n in text.split() for n in ('era5', 'era5-land', 'gldas-noah-0.25', 'gldas-noah-1.0', 'station-csv')), text.strip().replace('\n', ' '))
    for name in ('era5', 'era5-land', 'gldas-noah-0.25', 'gldas-noah-1.0', 'station-csv'):
        code, text = engine('forcing-check', name, '--json')
        data = json.loads(text)
        check('preset valid: ' + name, code == 0 and data['ok'] and data['resolved']['metadata']['name'] == name)
    code, text = engine('forcing-preset', 'era5')
    check('preset JSON round trip', code == 0 and json.loads(text)['variables']['shortwaveDown']['source'] == 'ssrd')

    # error paths
    def bad(label, document, needle, extra=()):
        path = work / (label + '.json')
        path.write_text(json.dumps(document), encoding='utf-8')
        code, text = engine('forcing-check', path, *extra)
        check('error message: ' + label, code != 0 and needle in text, text.strip().splitlines()[0] if text.strip() else '(empty)')

    code, text = engine('forcing-check', 'no-such-preset')
    check('error message: unknown preset', code != 0 and '未知的气象强迫预设' in text and 'era5-land' in text)
    bad('bad-unit', {'preset': 'era5', 'variables': {'airTemperature': {'unit': 'furlong'}}}, 'variables.airTemperature.unit "furlong" 不能识别')
    bad('kind-unit', {'preset': 'era5', 'variables': {'shortwaveDown': {'unit': 'W m-2'}}}, 'kind "accumulated" 需要能量')
    bad('bad-enum', {'preset': 'era5', 'variables': {'humidity': {'type': 'foo'}}}, 'variables.humidity.type "foo" 无效；可选')
    bad('no-shortwave', {'kind': 'stream-forcing-metadata', 'schemaVersion': 1, 'name': 'x', 'form': 'gridded',
                          'variables': {'airTemperature': {'source': 't'}}}, 'variables.shortwaveDown')
    bad('wind-roughness', {'preset': 'gldas-noah-0.25', 'variables': {'wind': {'height_m': 0.02}}}, 'roughness_m')
    bad('bad-kind-field', {'kind': 'other', 'schemaVersion': 1, 'name': 'x', 'form': 'gridded'}, '无法解析')
    code, text = engine('forcing-check', 'station-csv', '--json')
    check('height warning', any('未声明 height_m' in w for w in json.loads(text)['warnings']))
    code, text = engine('forcing-check', json.dumps({'preset': 'gldas-noah-0.25', 'variables': {'wind': {'height_m': 2}}}), '--json')
    check('wind height correction note', code == 0 and any('风速由 2 m 订正到 10 m' in n for n in json.loads(text)['notes']))

    scene = work / 'scene'
    make_scene(scene)
    data = work / 'data'; data.mkdir()
    era5_file(data / 'era5.nc')
    pack = register(data / 'era5-pack.json', data / 'era5.nc', era5=True)

    # 以下各节中的 d、dl、dgl、ds 为与 ERA5 基准运行的平均绝对温差（K），阈值依次为 0.02、0.2、1.0、1.5 K（见各 check）。
    # 1. ERA5 preset equals the default (byte-for-byte)
    code, text, a = run_case('era5-default', scene, {'era5': {'pack': pack}})
    check('ERA5 default run', code == 0 and (a / 'tir_surface_temperature.tif').exists(), text.strip().splitlines()[-1] if code else '')
    if code:
        return
    code, text, b = run_case('era5-preset', scene, {'era5': {'pack': pack}, 'forcing': {'metadata': 'era5'}})
    check('ERA5 preset run identical to default (sha256)', code == 0 and digest(a) == digest(b))
    base = surface(a)
    check('ERA5 surface temperature is plausible', 280 < np.nanmean(base) < 340, f'mean {np.nanmean(base):.2f} K')

    # 2. renamed variables / other units restored by metadata
    renamed_file(data / 'renamed.nc')
    meta = {'preset': 'era5', 'name': 'renamed-test', 'variables': {
        'airTemperature': {'source': 'Tair', 'unit': 'degC'}, 'humidity': {'source': 'Td', 'unit': 'degC'}, 'pressure': {'source': 'ps', 'unit': 'hPa'},
        'wind': {'u': {'source': 'uwind', 'unit': 'km/h'}, 'v': {'source': 'vwind', 'unit': 'km/h'}},
        'shortwaveDown': {'source': 'SW', 'unit': 'W m-2', 'kind': 'interval_mean'}, 'longwaveDown': {'source': 'LW', 'unit': 'W m-2', 'kind': 'interval_mean'},
        'skinTemperature': {'source': 'Tskin'}, 'soilMoisture': {'source': 'SM'}}}
    (data / 'renamed.json').write_text(json.dumps(meta), encoding='utf-8')
    rp = register(data / 'renamed-pack.json', data / 'renamed.nc', metadata=str(data / 'renamed.json'))
    code, text, c = run_case('renamed', scene, {'forcing': {'pack': rp}})
    d = float(np.nanmean(np.abs(surface(c) - base))) if code == 0 else 99
    check('renamed variables + units restored by metadata', code == 0 and d < 0.02, f'mean |dT| = {d:.4f} K')
    # a wrong source name is reported with the aliases tried
    broken = dict(meta); broken['variables'] = dict(meta['variables']); broken['variables']['airTemperature'] = {'source': ['T_wrong', 'T_alias'], 'unit': 'degC'}
    (data / 'broken.json').write_text(json.dumps(broken), encoding='utf-8')
    code, text = engine('forcing-check', data / 'broken.json', '--data', rp)
    check('missing variable reported with aliases', code != 0 and 'T_wrong' in text and 'T_alias' in text and '数据里有' in text, text.strip().splitlines()[0][:120] if text.strip() else '')

    # 3. ERA5-Land accumulation: original path vs the generic de-accumulation
    era5_file(data / 'land.nc', land=True)
    land = register(data / 'land-pack.json', data / 'land.nc', era5=True)
    code, text = engine('era5-register', data / 'land-pack2.json', '--product', 'era5-land', data / 'land.nc')
    code, text, e = run_case('land-legacy', scene, {'era5': {'pack': str(data / 'land-pack2.json')}})
    dl = float(np.nanmean(np.abs(surface(e) - base))) if code == 0 else 99
    check('ERA5-Land preset path (accumulated flux) ~ hourly ERA5', code == 0 and dl < 0.2, f'mean |dT| = {dl:.4f} K')
    forced = {'preset': 'era5-land', 'name': 'land-generic', 'variables': {'airTemperature': {'missing': [-99999]}}}
    code, text, f = run_case('land-generic', scene, {'era5': {'pack': str(data / 'land-pack2.json')}, 'forcing': {'metadata': forced}})
    dg = float(np.nanmean(np.abs(surface(f) - surface(e)))) if code == 0 else 99
    check('generic de-accumulation matches the ERA5-Land path', code == 0 and dg < 0.02, f'mean |dT| = {dg:.4f} K')
    # standardised 标志：只有布局与原 ERA5 不同（通用去累积、改名变量）时才应使用标准化层，原路径与 ERA5 预设不用。
    flags = [json.loads((x / 'scene_summary.json').read_text(encoding='utf-8'))['temperature']['forcing']['standardised'] for x in (e, f, b, c)]
    check('standardisation layer used only where the layout differs', flags == [False, True, False, True], str(flags))

    # 4. GLDAS-style 3-hourly grid
    gldas_file(data / 'gldas.nc')
    gp = register(data / 'gldas-pack.json', data / 'gldas.nc', metadata='gldas-noah-0.25')
    code, text, g = run_case('gldas', scene, {'forcing': {'pack': gp}})
    dgl = float(np.nanmean(np.abs(surface(g) - base))) if code == 0 else 99
    check('GLDAS-style 3 h data (synthetic) ~ hourly ERA5', code == 0 and dgl < 1.0, f'mean |dT| = {dgl:.3f} K' + ('' if code == 0 else ' ' + text.strip().splitlines()[-1]))
    summary = json.loads((g / 'scene_summary.json').read_text(encoding='utf-8')) if code == 0 else {}
    forcing = summary.get('temperature', {}).get('forcing', {})
    check('scene_summary records the metadata', forcing.get('metadata', {}).get('name') == 'gldas-noah-0.25' and any('d2m' in x for x in forcing.get('derived', [])), str(forcing.get('derived')))

    # 5. station CSV (spatially uniform data: equal to the grid run)
    station_csv(data / 'station.csv')
    smeta = {'preset': 'station-csv', 'name': 'station-test', 'variables': {'airTemperature': {'height_m': 2}, 'humidity': {'height_m': 2}, 'wind': {'height_m': 10}}}
    (data / 'station.json').write_text(json.dumps(smeta), encoding='utf-8')
    code, text = engine('forcing-check', data / 'station.json', '--data', data / 'station.csv')
    check('station check with data', code == 0, text.strip().splitlines()[-1] if text.strip() else '')
    code, text, s = run_case('station', scene, {'soilMoisture': {'source': 'user'}, 'forcing': {'metadata': str(data / 'station.json'), 'files': [str(data / 'station.csv')]}})
    ds = float(np.nanmean(np.abs(surface(s) - base))) if code == 0 else 99
    # the grid run takes soil moisture and the skin temperature of water from ERA5; the scene has no water, soil moisture default differs a little
    check('station CSV run ~ ERA5 run', code == 0 and ds < 1.5, f'mean |dT| = {ds:.3f} K' + ('' if code == 0 else ' ' + text.strip().splitlines()[-1]))
    # 3 h station file with one flux column accumulated
    lines = (data / 'station.csv').read_text(encoding='utf-8').splitlines()
    (data / 'station-bad-time.csv').write_text('\n'.join([lines[0], 'yesterday,A,40.0,116.5,50,20,50,1000,2,100,300']) + '\n', encoding='utf-8')
    code, text = engine('forcing-check', data / 'station.json', '--data', data / 'station-bad-time.csv')
    check('error message: unparsable time', code != 0 and '第 2 行时间 "yesterday" 无法按 iso8601 解析' in text, text.strip().splitlines()[0][:100] if text.strip() else '')
    (data / 'station-noloc.csv').write_text('time,temp,rh,pressure,wind_speed,swdown,lwdown\n2019-08-14 00:00:00Z,20,50,1000,2,100,300\n', encoding='utf-8')
    code, text = engine('forcing-check', data / 'station.json', '--data', data / 'station-noloc.csv')
    check('error message: station without coordinates', code != 0 and '缺少经纬度' in text, text.strip().splitlines()[0][:100] if text.strip() else '')
    code, text = engine('forcing-check', data / 'station.json', '--data', data / 'station.csv', '--json')
    check('forcing-check --json has warnings array', 'warnings' in json.loads(text))


# 收尾：运行 main，除非 --keep 否则删除临时目录；以失败数决定退出码。
try:
    main()
finally:
    if not options.keep:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('kept ' + str(work))
print('\n%d failure(s)' % len(failures) if failures else '\nall passed')
sys.exit(1 if failures else 0)
