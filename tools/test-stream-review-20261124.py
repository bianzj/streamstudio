# ==========================================================================
# 文件：test-stream-review-20261124.py
# 功能：STREAM 引擎 2026-11-24 评审修复（M1 LAI 填充码、M2 冷场景温度下限、C1-C3 配置校验、
#   收敛性改进等）的 pytest 回归测试；数据为 MODIS 北京示例 3x3 区域的最小合成修改版。
# 主要模块：
#   1. 辅助：沿用 test-stream-fixes-20261121.py（base）的 run/eb_config/CASE；clone 复制栅格并替换内容，
#      summary 读 scene_summary.json；REGION = 行 85-88、列 107-110 的 3x3 区域
#   2. M1：打包整型 LAI 的填充码与水体像元的 LAI nodata 处理
#   3. M2：极冷场景（ERA5 温度整体降低、长波缩放）不再被钉在 240 K 下限；未收敛像元被报告
#   4. C1/C2/C3：非数值配置项被拒绝而不是静默变成 0；天空长波、地形模式的校验
#   5. 收敛性：强不稳定的农田/森林像元（12x12 窗口）借助括号式回退全部收敛
# 关系：被 test-stream-bugs-20261124.py 导入（REGION、clone）。需要 GDAL、numpy、pytest；
#   cold_pack 还需要 netCDF4。运行：python -m pytest tools/test-stream-review-20261124.py
# 约定：eb_fluxes.tif 波段顺序按本文件断言推断：0 净辐射、1 显热、2 潜热、3 地热通量、
#   4 迭代数、5 是否收敛（其余波段含义待核对）；components.tif 的 0-3 为四个组分温度（K）。
# ==========================================================================

"""Regression checks for the 2026-11-24 STREAM engine review fixes (M1 LAI fill codes, M2 cold-scene temperature limit,
E1/R5 robustness items). Minimal synthetic data on the 3 x 3 region of the MODIS Beijing example.

Run:  python -m pytest tools/test-stream-review-20261124.py
      (STREAM_RUNTIME overrides the engine dir, default models/bin_x64/Release; STREAM_FIX_CASE the case)
Against the pre-fix engine the M1 / M2 checks fail (they document the bugs).
"""
import importlib.util
import json
from pathlib import Path

import numpy as np
import pytest
from osgeo import gdal

gdal.UseExceptions()
HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location('fixes20261121', HERE / 'test-stream-fixes-20261121.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)
CASE, run, eb_config = base.CASE, base.run, base.eb_config
# REGION：示例栅格中被测 3x3 区域的行/列切片（与 eb_config 的 region 对应，终点不含）。
REGION = (slice(85, 88), slice(107, 110))


# 作用：以 src 的尺寸与地理参考新建单波段 GeoTIFF dst，写入 array（类型 dtype），
#   可设置 scale（打包整型的缩放系数）与 nodata；返回 dst 路径字符串。
def clone(src, dst, array, dtype, scale=None, nodata=None):
    with gdal.Open(str(src)) as ds:
        out = gdal.GetDriverByName('GTiff').Create(str(dst), ds.RasterXSize, ds.RasterYSize, 1, dtype)
        out.SetGeoTransform(ds.GetGeoTransform())
        out.SetProjection(ds.GetProjection())
        band = out.GetRasterBand(1)
        if scale is not None:
            band.SetScale(scale)
        if nodata is not None:
            band.SetNoDataValue(nodata)
        band.WriteArray(array)
        out = None
    return str(dst)


def summary(out):
    return json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))


# 检查 M1：LAI 以 Byte 打包（scale 0.1，nodata 255）。填充码 249/250/254 缩放后为 24.9–25.4，
#   超出有效范围 [0, 20]，必须判为无效；LAI = 0 为裸地（只解土壤）；
#   水体像元即使 LAI 为 nodata 也应由表观温度求解。期望统计：laiOutOfRangePixels = 3，
#   laiNoDataPixels = 1。
# ---------- M1: packed-integer LAI fill codes and water pixels with LAI nodata ----------
def test_lai_fill_codes_and_water_nodata(tmp_path):
    with gdal.Open(str(CASE / 'inputs/lai.tif')) as ds:
        lai0 = ds.GetRasterBand(1).ReadAsArray().astype(float)
    with gdal.Open(str(CASE / 'inputs/type.tif')) as ds:
        type0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    raw = np.clip(np.round(lai0 * 10), 0, 100).astype(np.uint8)
    # row 85: valid 2.0 / 10.0 / 0.0 (bare ground stays valid); row 86: fill codes 249, 250, 254; row 87: nodata 255,
    # then two water pixels (LAI nodata / LAI 2.0)
    raw[REGION] = np.array([[20, 100, 0], [249, 250, 254], [255, 40, 20]], np.uint8)
    lai = clone(CASE / 'inputs/lai.tif', tmp_path / 'lai_modis.tif', raw, gdal.GDT_Byte, scale=0.1, nodata=255)
    land = int(np.bincount(type0[REGION].astype(int).ravel()).argmax())
    t = type0.copy()
    t[REGION] = land
    t[87, 108] = 17   # water, LAI 4.0
    t[87, 109] = 17   # water, LAI 2.0
    # (87, 107) is land with LAI nodata
    type_path = clone(CASE / 'inputs/type.tif', tmp_path / 'type.tif', t, gdal.GDT_Float32)
    config = eb_config()
    config['surface'] = dict(type=dict(file=type_path), lai=dict(file=lai), dem=dict(file=str(CASE / 'inputs/dem.tif')))
    ret, out = run(tmp_path, 'm1', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    leaf = comp[2]
    assert np.isfinite(comp[0][0]).all(), 'valid LAI 2.0 / 10.0 / 0.0 must be solved'
    assert np.isfinite(leaf[0, :2]).all() and np.isnan(leaf[0, 2]), 'LAI = 0 is bare ground: soil only, no leaf temperature'
    assert np.isnan(comp[:, 1, :]).all(), 'fill codes 249/250/254 (24.9-25.4 after scaling) must be invalid'
    assert np.isnan(comp[:, 2, 0]).all(), 'nodata LAI must be invalid'
    assert np.isfinite(comp[:, 2, 1:]).all(), 'water pixels are solved from the skin temperature'
    counts = summary(out)
    assert 'LAI 超出有效范围' in ret.stdout and '[0, 20]' in ret.stdout
    assert counts['counts']['laiOutOfRangePixels'] == 3
    assert counts['counts']['laiNoDataPixels'] == 1


# 作用：整个区域设为水体（类别 17，待核对）且 LAI 全为 nodata，要求所有像元仍有有限的组分温度，
#   且 components[0] 与 components[3] 相等（原因待核对）。
def test_water_pixel_with_nodata_lai_gets_skin_temperature(tmp_path):
    with gdal.Open(str(CASE / 'inputs/lai.tif')) as ds:
        lai0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    with gdal.Open(str(CASE / 'inputs/type.tif')) as ds:
        type0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    lai0[REGION] = np.nan
    t = type0.copy()
    t[REGION] = 17
    lai = clone(CASE / 'inputs/lai.tif', tmp_path / 'lai_nan.tif', lai0, gdal.GDT_Float32, nodata=-9999)
    type_path = clone(CASE / 'inputs/type.tif', tmp_path / 'type.tif', t, gdal.GDT_Float32)
    config = eb_config()
    config['surface'] = dict(type=dict(file=type_path), lai=dict(file=lai), dem=dict(file=str(CASE / 'inputs/dem.tif')))
    ret, out = run(tmp_path, 'water', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    assert np.isfinite(comp).all() and np.allclose(comp[0], comp[3])
    with gdal.Open(str(out / 'tir_surface_temperature.tif')) as ds:
        assert np.isfinite(ds.GetRasterBand(1).ReadAsArray()[REGION]).all()


# 检查 M2：把 ERA5 包的 t2m/d2m/skt/stl1/stl2 整体减去 delta（K），向下长波 strd 乘 lw_scale，
#   构造极冷夜间场景（overpass 16:00 UTC）；组分温度应能低于旧的 240 K 下限且仍收敛。
# ---------- M2: a very cold scene is no longer pinned at 240 K ----------
# 作用：复制示例 ERA5 包并按上述方式改写温度/长波变量（去掉 scale_factor/add_offset/_FillValue，
#   改存 float32），其余变量原样复制；更新索引中的文件大小，返回新 era5-pack.json 路径。
def cold_pack(tmp_path, delta, lw_scale=0.45):
    import netCDF4
    folder = tmp_path / f'pack_cold{delta}'
    folder.mkdir()
    index = json.loads((CASE / 'era5' / 'era5-pack.json').read_text(encoding='utf-8'))
    for item in index['files']:
        with netCDF4.Dataset(CASE / 'era5' / item['path']) as a, netCDF4.Dataset(folder / item['path'], 'w') as b:
            for n, dim in a.dimensions.items():
                b.createDimension(n, None if dim.isunlimited() else len(dim))
            for n, var in a.variables.items():
                if n in ('t2m', 'd2m', 'strd', 'skt', 'stl1', 'stl2'):
                    out = b.createVariable(n, 'f4', var.dimensions)
                    out.setncatts({k: var.getncattr(k) for k in var.ncattrs() if k not in ('scale_factor', 'add_offset', '_FillValue')})
                    data = np.ma.filled(var[:], np.nan).astype('f4')
                    out[:] = data * lw_scale if n == 'strd' else data - delta
                else:
                    fill = var.getncattr('_FillValue') if '_FillValue' in var.ncattrs() else None
                    out = b.createVariable(n, var.dtype, var.dimensions, fill_value=fill)
                    out.setncatts({k: var.getncattr(k) for k in var.ncattrs() if k != '_FillValue'})
                    out.set_auto_maskandscale(False)
                    var.set_auto_maskandscale(False)
                    out[:] = var[:]
        item['bytes'] = (folder / item['path']).stat().st_size
    (folder / 'era5-pack.json').write_text(json.dumps(index), encoding='utf-8')
    return folder / 'era5-pack.json'


# 作用：断言最低组分温度 < 238 K 且 > 200 K、所有像元收敛、|显热| < 150 W m-2（量级判据）、
#   nonConvergedPixels = 0。
def test_cold_night_converges_below_240_k(tmp_path):
    config = eb_config()
    config['overpass'] = '2019-08-15T16:00:00Z'
    config['temperature']['era5']['pack'] = str(cold_pack(tmp_path, 60))
    ret, out = run(tmp_path, 'cold', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        fl = ds.ReadAsArray()[:, 85:88, 107:110]
    converged, sensible = fl[5], fl[1]  # band order: net radiation, sensible, latent, ground, iterations, converged
    print('cold components min', np.nanmin(comp), 'max', np.nanmax(comp), 'sensible', np.nanmin(sensible), np.nanmax(sensible))
    assert np.nanmin(comp) < 238.0, 'components must not be clamped at the former 240 K limit'
    assert np.nanmin(comp) > 200.0
    assert converged.min() == 1.0
    assert np.abs(sensible).max() < 150.0
    assert summary(out)['temperature']['nonConvergedPixels'] == 0


# 作用：把 maxIterations 限为 1，必有未收敛像元；要求 summary 与日志都明确报告，
#   且 nonConvergedPixels = solvedPixels - convergedPixels。
def test_unconverged_pixels_are_reported(tmp_path):
    ret, out = run(tmp_path, 'maxit1', eb_config(maxIterations=1))
    assert ret.returncode == 0, ret.stderr + ret.stdout
    temperature = summary(out)['temperature']
    assert temperature['nonConvergedPixels'] > 0
    assert temperature['nonConvergedPixels'] == temperature['solvedPixels'] - temperature['convergedPixels']
    assert 'warning' in temperature and 'did not converge' in temperature['warning']
    assert '未收敛' in ret.stdout and '警告' in ret.stdout
    assert 'temperatureLimitedPixels' in temperature


# 检查 C1-C3：非数值（或非整数）的能量参数、skyLongwave 的数字/字符串写法被拒绝，
#   错误信息需含字段名与"须为数值/须为整数"；未知地形模式的报错应列出全部合法模式。
# ---------- C1 / C2 / C3: configuration values are validated, never silently turned into 0 ----------
@pytest.mark.parametrize('key,value', [('diffuseFraction', 'abc'), ('minWindSpeed', 'x'), ('maxAerodynamicResistance', True),
                                       ('maxIterations', 'many'), ('maxIterations', 2.5), ('stomatalG0', {})])
def test_non_numeric_energy_fields_are_rejected(tmp_path, key, value):
    ret, _ = run(tmp_path, 'c2', eb_config(**{key: value}))
    assert ret.returncode != 0, ret.stdout
    assert key in ret.stderr and ('须为数值' in ret.stderr or '须为整数' in ret.stderr), ret.stderr


def test_sky_longwave_number_and_string_value_are_rejected(tmp_path):
    for sky in (300, 'auto', dict(value='abc')):
        config = eb_config()
        config['skyLongwave'] = sky
        ret, _ = run(tmp_path, f'sky{abs(hash(str(sky)))}', config)
        assert ret.returncode != 0, sky
        assert 'skyLongwave' in ret.stderr, ret.stderr


def test_unknown_terrain_mode_lists_every_mode(tmp_path):
    config = eb_config()
    config['surface']['terrain'] = dict(mode='bogus')
    ret, _ = run(tmp_path, 'terrain', config)
    assert ret.returncode != 0
    for mode in ('none', 'single', 'facet', 'composite', 'hill', 'ridge'):
        assert mode in ret.stderr

# 检查收敛性：强不稳定条件下，旧版 1/L 松弛迭代会出现温度、阵风与 1/L 的极限环；
#   过境节点的括号式（bracketing）回退应让 144 个像元中至少 140 个收敛（旧引擎为 79）。
#   fl[8] 为第 9 个波段，含义待核对（此处与 fl[5] 一起作收敛判据）。
# ---------- convergence: strongly unstable cropland / forest pixels no longer end in 1/L limit cycles ----------
def test_unstable_pixels_converge_with_bracketing_fallback(tmp_path):
    """12 x 12 window of the MODIS example in which the plain 1/L relaxation left 65 of 144 pixels unconverged
    (limit cycles of temperature, gust and 1/L); the bracketing fallback of the overpass node closes them all."""
    config = eb_config()
    config['region'] = dict(startRow=89, endRow=101, startColumn=146, endColumn=158)
    ret, out = run(tmp_path, 'unstable', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        fl = ds.ReadAsArray()[:, 89:101, 146:158]
    solved = np.isfinite(fl[0])
    converged = (fl[5] == 1) & (fl[8] == 1) & solved
    print('solved', int(solved.sum()), 'converged', int(converged.sum()), 'max iterations', float(np.nanmax(fl[4])))
    assert solved.sum() == 144
    assert converged.sum() >= 140                       # engine before the fix: 79
    closure = np.abs(fl[0] - fl[1] - fl[2] - fl[3])
    assert np.nanmax(closure) < 100.0
    assert summary(out)['temperature']['nonConvergedPixels'] == int(solved.sum() - converged.sum())