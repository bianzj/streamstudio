# ==========================================================================
# 文件：generate-tarp-assets.py
# 功能：生成篷布/遮阳网/沙地的示意光谱与无缝（周期）纹理，并更新两个资源目录的 catalog.json。
#       光谱为合成曲线，非实测；用途与限制见下方原有文档字符串。
# 主要模块：
#   1. SPECTRA：每种材料的标签、热红外反射/透射率、透射/反射比和可见-短波锚点反射率
#   2. write_spectra：400-2500 nm、10 nm 间隔插值；透射率 = min(比值 × 反射率, 1 - 反射率)；
#      写 spectral-library/<id>.txt（三行：波长、反射率、透射率），并合并进 catalog.json
#   3. 噪声与纹理生成：periodic_noise（FFT 高斯滤波的周期噪声）、camouflage、canvas_weave、
#      digital_camouflage、awning_stripes、crumpled_film、damaged_net、weave_density、
#      soil_mottling、sand_mottling
#   4. write_textures：生成 1024×1024 的方差纹理(JPG)、分类纹理(PNG，调色板精确颜色)、
#      开孔率纹理(PNG 灰度)，计算 SHA-256 并合并进 texture-library/catalog.json
# 输出：assets/spectral-library/*.txt、assets/texture-library/*.{jpg,png}、两处 catalog.json。
# 约定：波长 nm；反射/透射率 0-1；suggestedRepeatSize 为纹理在场景中重复一次覆盖的边长（m）；
#   固定随机种子，重复运行得到相同纹理；catalog 中同 id 的旧条目被新条目替换，其余保留。
# ==========================================================================

"""Generate tarp / shade-net spectra and seamless variance textures.

    python tools/generate-tarp-assets.py

Spectra are representative synthetic curves for quick tests (anchor points
from typical dyed canvas, PE, aluminised film and HDPE net behaviour), not
measurements. Textures are periodic by construction (FFT-filtered noise).
"""
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SPECTRAL = ROOT / 'assets' / 'spectral-library'
TEXTURES = ROOT / 'assets' / 'texture-library'
# 统一波长网格：400-2500 nm，步长 10 nm。
GRID = np.arange(400, 2501, 10)

# 材料表：锚点 (nm, R) 间做线性插值得到整条光谱；吸收带附近（如 1400/1900 nm）的锚点
# 用于保留水汽/材料吸收谷。
# name: (label, refTir, tauTir, transmittance/reflectance ratio, [(nm, R), ...])
SPECTRA = {
    'tarp_olive_canvas': ('篷布·军绿帆布', 0.10, 0.0, 0.18, [
        (400, .055), (450, .060), (500, .075), (550, .105), (600, .090), (650, .080), (680, .082), (700, .100),
        (720, .160), (750, .300), (800, .420), (900, .480), (1000, .500), (1100, .510), (1200, .490), (1300, .500),
        (1400, .420), (1450, .360), (1500, .400), (1650, .440), (1800, .380), (1950, .250), (2000, .270),
        (2100, .330), (2200, .320), (2300, .280), (2400, .240), (2500, .220)]),
    'tarp_desert_tan': ('篷布·荒漠沙色', 0.09, 0.0, 0.15, [
        (400, .140), (450, .170), (500, .210), (550, .260), (600, .320), (650, .360), (700, .390), (750, .420),
        (800, .450), (900, .500), (1000, .530), (1200, .550), (1400, .480), (1450, .430), (1500, .470),
        (1650, .520), (1800, .470), (1950, .330), (2000, .350), (2100, .420), (2200, .400), (2300, .360),
        (2400, .320), (2500, .300)]),
    'tarp_white_pe': ('篷布·白色 PE 编织布', 0.08, 0.10, 0.30, [
        (400, .600), (450, .660), (500, .680), (600, .690), (800, .680), (1000, .660), (1150, .630), (1210, .550),
        (1250, .620), (1400, .580), (1650, .600), (1730, .450), (1770, .550), (2000, .500), (2200, .460),
        (2310, .300), (2350, .340), (2500, .300)]),
    'tarp_silver_reflective': ('篷布·镀铝反光膜', 0.72, 0.0, 0.0, [
        (400, .800), (500, .830), (600, .850), (700, .855), (800, .840), (830, .820), (870, .840), (1000, .870),
        (1500, .890), (2000, .900), (2500, .910)]),
    'tarp_ir_camouflage_green': ('篷布·仿植被红外伪装布', 0.06, 0.0, 0.12, [
        (400, .050), (450, .055), (500, .070), (550, .120), (600, .080), (650, .060), (680, .060), (700, .090),
        (720, .180), (750, .330), (800, .420), (900, .450), (1000, .450), (1200, .420), (1400, .300), (1450, .250),
        (1500, .300), (1650, .360), (1800, .300), (1950, .170), (2000, .180), (2200, .250), (2400, .180), (2500, .150)]),
    'shade_net_black_fiber': ('遮阳网纤维·黑色 HDPE', 0.06, 0.0, 0.20, [
        (400, .045), (1000, .045), (1800, .050), (2500, .050)]),
    'shade_net_green_fiber': ('遮阳网纤维·绿色 HDPE', 0.07, 0.0, 0.30, [
        (400, .050), (500, .080), (550, .140), (600, .080), (650, .050), (700, .070), (750, .250), (800, .450),
        (1000, .500), (1200, .480), (1210, .420), (1250, .470), (1400, .440), (1730, .360), (1770, .420),
        (2310, .250), (2500, .250)]),
    # Iron-oxide-coated quartz dune sand: steep VIS rise, Fe3+ shoulder near 900 nm,
    # weak 1400/1900 nm water and ~2200 nm clay features. Opaque.
    'sand_desert_dune': ('沙漠沙丘沙（含铁石英砂）', 0.05, 0.0, 0.0, [
        (400, .120), (450, .160), (500, .220), (550, .300), (600, .380), (650, .430), (700, .460), (750, .480),
        (800, .490), (900, .485), (1000, .500), (1100, .520), (1300, .550), (1400, .530), (1450, .510), (1500, .540),
        (1650, .580), (1800, .580), (1900, .540), (1950, .530), (2000, .550), (2100, .560), (2200, .540),
        (2300, .530), (2400, .510), (2500, .490)]),
    'sand_desert_dark': ('沙漠丘间地（粗砂与砾石）', 0.06, 0.0, 0.0, [
        (400, .090), (450, .120), (500, .160), (550, .210), (600, .260), (650, .300), (700, .325), (800, .345),
        (900, .345), (1000, .360), (1300, .400), (1400, .385), (1450, .370), (1650, .420), (1900, .390),
        (1950, .385), (2100, .405), (2200, .390), (2500, .360)]),
}


# 作用：写出所有材料的光谱文件，并把条目合并进 spectral-library/catalog.json
# （同 id 旧条目被替换；catalog 的其他顶层字段原样保留，格式手工拼接以保持原有缩进）。
def write_spectra():
    entries = []
    for name, (label, ref_tir, tau_tir, ratio, anchors) in SPECTRA.items():
        wl, r = np.array(anchors, dtype=float).T
        refl = np.interp(GRID, wl, r)
        trans = np.minimum(ratio * refl, 1 - refl)
        rows = [' '.join(str(int(v)) for v in GRID), ' '.join(f'{v:.4f}' for v in refl), ' '.join(f'{v:.4f}' for v in trans)]
        text = f'# Representative synthetic spectrum ({label}) for quick tests; wavelength unit: nm\n' + '\n'.join(rows) + '\n'
        (SPECTRAL / f'{name}.txt').write_text(text, encoding='utf-8')
        entries.append({'id': name, 'label': label, 'file': f'{name}.txt', 'tirReflectance': ref_tir, 'tirTransmittance': tau_tir})
    catalog_path = SPECTRAL / 'catalog.json'
    catalog = json.loads(catalog_path.read_text(encoding='utf-8'))
    known = {item['id'] for item in entries}
    catalog['spectra'] = [item for item in catalog['spectra'] if item['id'] not in known] + entries
    lines = ',\n'.join('    ' + json.dumps(item, ensure_ascii=False) for item in catalog['spectra'])
    head = {k: v for k, v in catalog.items() if k != 'spectra'}
    body = ',\n'.join(f'  {json.dumps(k)}: {json.dumps(v, ensure_ascii=False)}' for k, v in head.items())
    catalog_path.write_text('{\n' + body + ',\n  "spectra": [\n' + lines + '\n  ]\n}\n', encoding='utf-8')


# 作用：生成 size×size 的周期（首尾可无缝拼接）噪声：白噪声经傅里叶域高斯核滤波，
# sigma_x/sigma_y 为两个方向的相关长度（像素），输出零均值、单位标准差。
def periodic_noise(size, sigma_x, sigma_y, seed):
    """Gaussian-blurred white noise; sigma in pixels. Periodic, zero mean, unit std."""
    rng = np.random.default_rng(seed)
    f = np.fft.fftfreq(size)
    fx, fy = np.meshgrid(f, f)
    kernel = np.exp(-2 * np.pi ** 2 * ((sigma_x * fx) ** 2 + (sigma_y * fy) ** 2))
    field = np.real(np.fft.ifft2(np.fft.fft2(rng.standard_normal((size, size))) * kernel))
    return (field - field.mean()) / field.std()


# 作用：迷彩分类图。大尺度 + 小尺度噪声叠加后，按 fractions 给定的面积比例用分位数切成
# len(palette) 类，输出每类对应的 RGB。
def camouflage(size, palette, fractions, seed):
    field = periodic_noise(size, 34, 20, seed) + 0.35 * periodic_noise(size, 9, 7, seed + 1)
    edges = np.quantile(field, np.cumsum(fractions)[:-1])
    classes = np.digitize(field, edges)
    return np.array(palette, dtype=np.uint8)[classes]


# 作用：帆布织纹（8 像素周期的经纬交织）叠加纵横向疏密条纹与大尺度斑驳，着军绿基色。
def canvas_weave(size, seed):
    period = 8
    y, x = np.mgrid[0:size, 0:size]
    over = ((x // (period // 2) + y // (period // 2)) % 2) * 2 - 1
    weave = 0.5 + 0.18 * over * (np.cos(2 * np.pi * x / period) - np.cos(2 * np.pi * y / period))
    slub = 0.08 * periodic_noise(size, 60, 3, seed) + 0.06 * periodic_noise(size, 3, 60, seed + 1)
    mottling = 0.07 * periodic_noise(size, 90, 90, seed + 2)
    gray = np.clip(weave + slub + mottling, 0, 1)
    base = np.array([88, 97, 61], dtype=float) / 255
    return np.clip(gray[..., None] * 2 * base * 255, 0, 255).astype(np.uint8)


def digital_camouflage(size, palette, fractions, block, seed):
    """Pixelated camouflage: the smooth camouflage field quantised on a block grid."""
    field = periodic_noise(size // block, 34 / block * 2, 20 / block * 2, seed) + 0.5 * periodic_noise(size // block, 1.2, 1.2, seed + 1)
    edges = np.quantile(field, np.cumsum(fractions)[:-1])
    classes = np.digitize(field, edges).repeat(block, axis=0).repeat(block, axis=1)
    return np.array(palette, dtype=np.uint8)[classes]


def awning_stripes(size, stripes, seed):
    """Alternating light/dark woven stripes with slight fibre noise."""
    x = np.arange(size)
    band = ((x * stripes // size) % 2).astype(float)
    gray = 0.25 + 0.6 * band[None, :] + 0.03 * periodic_noise(size, 2, 40, seed)
    return (np.clip(gray, 0, 1)[..., None] * np.array([235, 235, 235])).astype(np.uint8)


def crumpled_film(size, seed):
    """Low-frequency creases of a crumpled reflective film."""
    ridges = np.abs(periodic_noise(size, 28, 28, seed)) + 0.6 * np.abs(periodic_noise(size, 10, 10, seed + 1))
    gray = 1.0 - ridges / ridges.max()
    return (np.clip(0.35 + 0.6 * gray, 0, 1)[..., None] * np.array([215, 220, 228])).astype(np.uint8)


def damaged_net(size, seed):
    """Openness map: mostly intact net (dark) with scattered tears (white)."""
    holes = periodic_noise(size, 14, 9, seed) + 0.4 * periodic_noise(size, 4, 4, seed + 1)
    gray = np.full((size, size), 0.3) + 0.06 * periodic_noise(size, 30, 30, seed + 2)
    gray[holes > 2.0] = 1.0
    return (np.clip(gray, 0, 1) * 255).astype(np.uint8)


def weave_density(size, seed):
    """Openness map: smooth density variations of a loosely woven fabric."""
    field = periodic_noise(size, 70, 45, seed) + 0.3 * periodic_noise(size, 12, 12, seed + 1)
    field = (field - field.min()) / (field.max() - field.min())
    return (field * 255).astype(np.uint8)


def soil_mottling(size, seed):
    """Bare soil: aggregates (~5 cm), clods/patches (~0.5 m) and a moisture trend (~3 m) at a 10 m repeat."""
    field = (0.5 * periodic_noise(size, 5, 5, seed) + 0.9 * periodic_noise(size, 45, 45, seed + 1)
             + 0.6 * periodic_noise(size, 220, 220, seed + 2))
    gray = np.clip(0.5 + 0.12 * field, 0, 1)
    return (gray[..., None] * np.array([190, 160, 125])).astype(np.uint8)


def sand_mottling(size, seed):
    """Wind-streaked sand: features >= ~0.2 m at a 20 m repeat so 0.25 m facets resolve them."""
    streaks = 0.6 * periodic_noise(size, 10, 80, seed) + 0.8 * periodic_noise(size, 60, 60, seed + 1)
    gray = np.clip(0.55 + 0.12 * streaks, 0, 1)
    return (gray[..., None] * np.array([235, 205, 160])).astype(np.uint8)


# 迷彩调色板与各色面积占比：林地 4 色、荒漠 3 色、数码 4 色；占比之和为 1，
# 分类 PNG 使用同样的调色板以便按精确颜色识别类别。
WOODLAND = ([(86, 99, 52), (52, 64, 31), (110, 82, 50), (30, 31, 25)], [0.34, 0.30, 0.24, 0.12])
DESERT = ([(200, 177, 131), (168, 141, 95), (131, 112, 76)], [0.45, 0.33, 0.22])
DIGITAL = ([(92, 104, 62), (58, 70, 40), (124, 112, 80), (36, 38, 30)], [0.36, 0.30, 0.22, 0.12])


# 作用：生成全部纹理并更新 texture-library/catalog.json。
# images 中每项为 文件名: (标签, suggestedRepeatSize（m）, 像素数组)；
# 同一种子同时用于方差纹理(JPG)与对应的分类纹理(PNG)，两者图案一致。
# kind 按文件名前缀判定：class_ → class，openness_ → openness，其余 → variance。
def write_textures():
    size = 1024
    images = {
        'camouflage_woodland_variance.jpg': ('林地迷彩', 1.5, camouflage(size, *WOODLAND, 11)),
        'camouflage_desert_variance.jpg': ('荒漠迷彩', 1.5, camouflage(size, *DESERT, 23)),
        'canvas_weave_variance.jpg': ('帆布织纹', 0.5, canvas_weave(size, 37)),
        'camouflage_digital_variance.jpg': ('数码迷彩', 1.6, digital_camouflage(size, *DIGITAL, 64, 41)),
        'stripes_variance.jpg': ('遮阳条纹', 1.2, awning_stripes(size, 4, 53)),
        'crumpled_film_variance.jpg': ('褶皱反光膜', 2.0, crumpled_film(size, 67)),
        'sand_mottling_variance.jpg': ('沙面风纹', 20.0, sand_mottling(size, 97)),
        'soil_mottling_variance.jpg': ('裸土斑块', 10.0, soil_mottling(size, 101)),
        # Lossless class maps (exact palette colours) and grey openness maps.
        'class_woodland_4.png': ('林地迷彩 4 类（分类纹理）', 1.5, camouflage(size, *WOODLAND, 11)),
        'class_digital_4.png': ('数码迷彩 4 类（分类纹理）', 1.6, digital_camouflage(size, *DIGITAL, 64, 41)),
        'class_desert_3.png': ('荒漠迷彩 3 类（分类纹理）', 1.5, camouflage(size, *DESERT, 23)),
        'openness_damaged_net.png': ('遮阳网破损（开孔率纹理）', 1.0, damaged_net(size, 71)),
        'openness_weave_density.png': ('织物疏密不均（开孔率纹理）', 1.0, weave_density(size, 83)),
    }
    catalog_path = TEXTURES / 'catalog.json'
    catalog = json.loads(catalog_path.read_text(encoding='utf-8'))
    entries = []
    for file_name, (label, repeat, pixels) in images.items():
        path = TEXTURES / file_name
        mode = 'L' if pixels.ndim == 2 else 'RGB'
        if file_name.endswith('.png'):
            Image.fromarray(pixels, mode).save(path, optimize=True)
        else:
            Image.fromarray(pixels, mode).save(path, quality=92)
        digest = hashlib.sha256(path.read_bytes()).hexdigest().upper()
        kind = 'class' if file_name.startswith('class_') else 'openness' if file_name.startswith('openness_') else 'variance'
        entries.append({'id': file_name.rsplit('.', 1)[0].replace('_variance', ''), 'kind': kind, 'label': label, 'fileName': f'assets/texture-library/{file_name}',
                        'sourceAsset': 'tools/generate-tarp-assets.py', 'sourceUrl': '', 'suggestedRepeatSize': repeat, 'sha256': digest})
    known = {item['id'] for item in entries}
    catalog['textures'] = [item for item in catalog['textures'] if item['id'] not in known] + entries
    catalog_path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


write_spectra()
write_textures()
print('spectra:', ', '.join(SPECTRA))
print('textures written to', TEXTURES)
