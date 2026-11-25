# ==========================================================================
# 文件：test-stream-bugs-20261124.py
# 功能：STREAM 引擎 2026-11-24 第二批修复（C4、C5、A2、I1/I2、L5、冰雪融化等）的 pytest 回归测试；
#   用极小的合成场景调用真实的 stream.exe，读取输出栅格与 scene_summary.json 断言。
# 主要模块：
#   1. 辅助导入：通过 importlib 按路径加载 test-stream-fixes-20261121.py 作为 base，复用
#      CASE/run/eb_config/rt_config/read；summary() 读取 scene_summary.json
#   2. C4：已弃用的 maxStabilityIterations 不再参与范围校验
#   3. C5：RT 成像读取 energy.diffuseFraction；-1（Erbs）等无效值被拒绝
#   4. A2：无可用观测的同化像元有独立的 QA 位（第 12 位），区别于输入无效
#   5. I1/I2：LSCT 背景算子失败只记为"反演失败"，并拆分停止原因计数
#   6. L5：面元（facet）地形方法的最外一圈边界像元会被报告
#   7. 冰雪融化：雪/冰类被限制在 273.15 K，多余能量记为融化通量（eb_fluxes 第 19 波段）
# 关系：依赖 test-stream-fixes-20261121.py（base）、test-assimilation-contract.py（A2 用例）、
#   test-stream-review-20261124.py（冰雪场景用其 REGION 与 clone）。
# 运行：python -m pytest tools/test-stream-bugs-20261124.py；STREAM_RUNTIME / STREAM_FIX_CASE 同 base。
# ==========================================================================

"""Regression checks for the second batch of 2026-11-24 STREAM engine fixes (C4, C5, T1, ...). Small synthetic scenes.

Run:  python -m pytest tools/test-stream-bugs-20261124.py
      (STREAM_RUNTIME overrides the engine dir, default models/bin_x64/Release; STREAM_FIX_CASE the case)
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
CASE, run, eb_config, rt_config, read = base.CASE, base.run, base.eb_config, base.rt_config, base.read


# 作用：读取输出目录下的 scene_summary.json（引擎写出的场景/运行摘要）。
def summary(out):
    return json.loads((out / 'scene_summary.json').read_text(encoding='utf-8'))


# 检查 C4：maxStabilityIterations 为 0 或负数时引擎仍应正常运行（该字段已弃用，不再校验范围）。
# ---------- C4: the deprecated maxStabilityIterations no longer takes part in range validation ----------
@pytest.mark.parametrize('value', [0, -3])
def test_deprecated_max_stability_iterations_is_not_validated(tmp_path, value):
    ret, _ = run(tmp_path, 'msi', eb_config(maxStabilityIterations=value))
    assert ret.returncode == 0, ret.stderr + ret.stdout


# 检查 C5：RT 光学成像按 energy.diffuseFraction 取散射比例（缺省 0.2）；
#   取 -1（Erbs 模型，需要短波观测）或超出范围的 1.5 时应拒绝运行并在 stderr 中报错。
# ---------- C5: RT imaging reads energy.diffuseFraction; -1 (Erbs) needs shortwave and is rejected ----------
# 作用：在 rt_config 基础上打开一个 650 nm（FWHM 40）的光学波段并关闭热红外，
#   energy 并入 temperature.energy（如 diffuseFraction）。
def rt_optical(tmp_path, tag, **energy):
    config = rt_config(tmp_path, tag, thermal=False)
    config['optical'] = dict(enabled=True, bands=[dict(name='R', center=650, fwhm=40)])
    config['temperature']['energy'].update(energy)
    return config


def test_rt_diffuse_fraction_is_read(tmp_path):
    values = {}
    for name, extra in (('d0', dict(diffuseFraction=0.0)), ('d1', dict(diffuseFraction=1.0)), ('d02', dict(diffuseFraction=0.2)), ('def', {})):
        ret, out = run(tmp_path, name, rt_optical(tmp_path, name, **extra))
        assert ret.returncode == 0, ret.stderr + ret.stdout
        values[name] = read(out / 'optical_reflectance.tif')
    assert values['d0'] != values['d1'], values
    assert values['def'] == values['d02']  # the default stays 0.2


@pytest.mark.parametrize('value', [-1, 1.5])
def test_rt_diffuse_fraction_erbs_is_rejected(tmp_path, value):
    ret, _ = run(tmp_path, 'rtd', rt_optical(tmp_path, 'rtd', diffuseFraction=value))
    assert ret.returncode != 0
    assert 'diffuseFraction' in ret.stderr and 'RT' in ret.stderr, ret.stderr

# 检查 A2：同化观测被剔除（NaN）的像元，QA 同时置第 0 位（未分析）与第 12 位（4096，无观测）；
#   正常像元不应置第 12 位。同化场景由 test-assimilation-contract.py 提供（contract()）。
# ---------- A2: a pixel with no usable observation is distinguishable from invalid inputs ----------
def contract():
    spec = importlib.util.spec_from_file_location('contract', HERE / 'test-assimilation-contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_no_observation_pixel_has_its_own_qa_bit(tmp_path):
    c = contract()
    qa = c.raster(tmp_path / 'qa.tif', [float('nan')])   # the only observation is rejected
    result, out = c.run(tmp_path, dict(qa=qa))
    assert result.returncode == 0, result.stderr + result.stdout
    flags = int(read(out / 'analysis_qa.tif', 1))
    assert flags & 1 and flags & 4096, flags              # not analysed (bit 0) because nothing was observed (bit 12)
    sub = tmp_path / 'ok'
    sub.mkdir()
    ok, out2 = c.run(sub, dict())
    assert ok.returncode == 0, ok.stderr + ok.stdout
    assert int(read(out2 / 'analysis_qa.tif', 1)) & 4096 == 0


# 检查 I1/I2：LSCT（四组分温度反演）的背景算子无法求值时，QA 只置第 3 位（反演失败）
#   而不置第 4 位（病态）；停止原因计数按同化的方式拆分：
#   notConvergedPixels = iterationLimitPixels + stepEvaluationFailedPixels，
#   且收敛/停滞/未更新/未收敛四类之和等于像元总数（此处为 1）。
# ---------- I1/I2: LSCT background failure is "retrieval failed" only; stop reasons are split like the assimilation ----------
# 作用：单像元 LSCT 反演：单个 10.5 µm 亮温观测 300 K（sigma 0.3 K），先验 sd 5 K、相关 0；
#   components 为配置里的四组分温度（K，作背景/初值，待核对）；热红外正演关闭。
def lsct(tmp_path, tag, components):
    inv = dict(target='lsct', observations=[dict(kind='brightness', value=dict(value=300.), wavelength=10.5, sigma=.3)],
               prior=dict(sd=5., correlation=0.))
    config = rt_config(tmp_path, tag, components=components, thermal=False, inversion=inv, sky=dict(mode='none'))
    ret, out = run(tmp_path, tag, config)
    return ret, out


def test_lsct_background_operator_failure_is_not_ill_conditioned(tmp_path):
    ret, out = lsct(tmp_path, 'bad', (1., 1., 1., 1.))   # B(1 K) underflows: the operator cannot be evaluated
    assert ret.returncode == 0, ret.stderr + ret.stdout
    qa = int(read(out / 'lsct_qa.tif', 1))
    assert qa & 8 and not qa & 16, qa
    counts = summary(out)['inversion']['counts']
    assert counts['failedPixels'] == 1 and counts['illConditionedPixels'] == 0, counts


def test_lsct_counts_split_bit9_stops(tmp_path):
    ret, out = lsct(tmp_path, 'ok', (305.,) * 4)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    counts = summary(out)['inversion']['counts']
    assert counts['notConvergedPixels'] == counts['iterationLimitPixels'] + counts['stepEvaluationFailedPixels']
    assert counts['convergedPixels'] + counts['lineSearchStalledPixels'] + counts['notUpdatedPixels'] + counts['notConvergedPixels'] == 1

# 检查 L5：facet 地形方法下，5x5 栅格最外一圈共 16 个像元应在日志中报告（具体原因待核对）；
#   若 region 只取内部 3x3，则不应出现该警告。
# ---------- L5: the facet-method border ring is reported ----------
# 作用：把二维数组写成带地理参考的 Float32 GeoTIFF（EPSG:4326，左上角 116°E、40°N，
#   像元 0.001°），返回路径字符串。
def geo_raster(path, array):
    array = np.asarray(array, np.float32)
    ds = gdal.GetDriverByName('GTiff').Create(str(path), array.shape[1], array.shape[0], 1, gdal.GDT_Float32)
    ds.SetGeoTransform((116.0, 0.001, 0.0, 40.0, 0.0, -0.001))
    srs = __import__('osgeo.osr', fromlist=['osr']).SpatialReference()
    srs.ImportFromEPSG(4326)
    ds.SetProjection(srs.ExportToWkt())
    ds.GetRasterBand(1).WriteArray(array)
    ds = None
    return str(path)


def test_facet_border_ring_is_reported(tmp_path):
    config = rt_config(tmp_path, 'facet', terrain=dict(mode='facet'))
    rows = np.arange(5, dtype=np.float32)[:, None] * 30.0 + np.zeros((5, 5), np.float32)   # a 30 m per pixel ramp
    config['surface']['type'] = dict(file=geo_raster(tmp_path / 'type5.tif', np.full((5, 5), 10)))
    config['surface']['dem'] = dict(file=geo_raster(tmp_path / 'dem5.tif', rows))
    config['surface'].pop('latitude'), config['surface'].pop('longitude')
    ret, out = run(tmp_path, 'facet', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    assert '最外一圈' in ret.stdout and '16 个' in ret.stdout, ret.stdout
    # a region that excludes the ring produces no warning
    config['region'] = dict(startRow=1, endRow=4, startColumn=1, endColumn=4)
    sub = tmp_path / 'inner'
    sub.mkdir()
    ret2, _ = run(sub, 'facet', config)
    assert ret2.returncode == 0, ret2.stderr + ret2.stdout
    assert '最外一圈' not in ret2.stdout

# 检查：冰雪类别地表温度不超过 273.15 K，多余能量计为融化通量 melt，使
#   Rn - H - LE - G - melt = 0（eb_fluxes 前四个波段为 Rn、H、LE、G，第 19 波段为 melt）；
#   关闭 iceMelt 后恢复旧行为（雪温可高于 0 °C，波段数为 18）；无冰雪场景不应有 melt 波段。
# ---------- ice melt: snow / ice classes are capped at 273.15 K, the surplus is a melt flux ----------
# 作用：把示例 EB 场景中 review.REGION 区域的类别改为 15（雪/冰类，待核对）并设定 LAI，
#   写出临时 type/lai 栅格后返回 (配置, 区域)。
def ice_scene(tmp_path, lai_value=0.0, **energy):
    spec = importlib.util.spec_from_file_location('review', HERE / 'test-stream-review-20261124.py')
    review = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(review)
    with gdal.Open(str(CASE / 'inputs/type.tif')) as ds:
        type0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    with gdal.Open(str(CASE / 'inputs/lai.tif')) as ds:
        lai0 = ds.GetRasterBand(1).ReadAsArray().astype(np.float32)
    region = review.REGION
    type0[region] = 15
    lai0[region] = lai_value
    config = eb_config(**energy)
    config['surface'] = dict(type=dict(file=review.clone(CASE / 'inputs/type.tif', tmp_path / 'ice_type.tif', type0, gdal.GDT_Float32)),
                             lai=dict(file=review.clone(CASE / 'inputs/lai.tif', tmp_path / 'ice_lai.tif', lai0, gdal.GDT_Float32)),
                             dem=dict(file=str(CASE / 'inputs/dem.tif')))
    return config, region


@pytest.mark.parametrize('lai', [0.0, 1.0])
def test_ice_is_capped_at_the_melting_point(tmp_path, lai):
    config, region = ice_scene(tmp_path, lai)
    ret, out = run(tmp_path, 'ice', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
    assert np.nanmax(comp) <= 273.15 + 1e-9, np.nanmax(comp)
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        fl = ds.ReadAsArray()[:, 85:88, 107:110]
        assert ds.RasterCount == 19
    melt = fl[18]
    assert (melt > 50).all(), melt                            # midday: a large energy surplus goes into melting
    # the balance closes with the melt flux: Rn - H - LE - G - melt = 0 (bands: Rn, H, LE, G)
    assert np.abs(fl[0] - fl[1] - fl[2] - fl[3] - melt).max() < 1.0
    counts = summary(out)['temperature']
    assert counts['iceMelt'] is True and counts['iceCappedPixels'] == 9 and counts['iceMeltPixels'] == 9
    assert '冰雪融化' in ret.stdout


def test_ice_melt_can_be_switched_off(tmp_path):
    config, region = ice_scene(tmp_path, 0.0, iceMelt=False)
    ret, out = run(tmp_path, 'ice_off', config)
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'components.tif')) as ds:
        comp = ds.ReadAsArray()[:, 85:88, 107:110]
        assert np.nanmax(comp) > 275.0                         # the old behaviour: snow warmer than 0 degC
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        assert ds.RasterCount == 18


def test_scene_without_ice_has_no_melt_band(tmp_path):
    ret, out = run(tmp_path, 'plain', eb_config())
    assert ret.returncode == 0, ret.stderr + ret.stdout
    with gdal.Open(str(out / 'eb_fluxes.tif')) as ds:
        assert ds.RasterCount == 18
    assert 'iceMeltPixels' not in summary(out)['temperature']
