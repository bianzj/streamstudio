"""Reproduce the four-camera deck videos (case 1) from the split case frame folders.

Source panels (both from the original expanded_ground four-camera case):
  external RGB / IR  <- 01_打击_四路相机/外部视角/result/observed_optical | observed_ir   (504 frames)
  forward  IR / RGB  <- 01_打击_四路相机/前视相机/result/observed_ir | observed_optical   (290 frames, then a lost-signal card)

Videos (21 s, 24 fps, 504 frames), mirroring the previous deck:
  00_基准_四路相机.mp4          4 panels, no effects
  01_强抖动.mp4                  forward panels shaken in the display domain (1.25x overscan)
  02_低观测质量.mp4              forward panels downsampled, blurred, noisy, 42% previous-frame blend
  03_低量化.mp4                  forward panels quantised to 4 display codes per channel
  04_区域外观差异.mp4            fixed spatial gain / IR offset patterns (image-space re-implementation)
  05_同屏对照_IR与RGB.mp4        4 rows (original, shake, poor, quantised) x 2 columns (IR, RGB)

The display effects are generic post-processing of displayed images; no simulation is re-run and no calibrated
hardware response is implied. Parameters follow camera_visual_variants_20261109/source/build_variants.py.
"""
from __future__ import annotations

import argparse
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from video_encoder import add_argument, encoder_args, pick_encoder  # noqa: E402

FPS = 24
PW, PH = 1280, 720
FONT = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 32)
SMALL = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 25)
TITLE = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 26)


def shake_parameters(n):
    t = (n - 1) / FPS
    dx = 38 * math.sin(2 * math.pi * 7.3 * t + .2) + 23 * math.sin(2 * math.pi * 10.7 * t + 1.3) + 9 * math.sin(2 * math.pi * 2.1 * t + .7)
    dy = 18 * math.sin(2 * math.pi * 6.7 * t + 1.1) + 8 * math.sin(2 * math.pi * 9.4 * t + .4) + 4 * math.sin(2 * math.pi * 1.8 * t)
    degrees = 2.6 * math.sin(2 * math.pi * 5.3 * t + .7) + math.sin(2 * math.pi * 8.9 * t + 2) + .4 * math.sin(2 * math.pi * 1.7 * t)
    return dx, dy, degrees


def shake(image, n):
    dx, dy, degrees = shake_parameters(n)
    a = math.radians(degrees)
    s, c = math.sin(a) / 1.25, math.cos(a) / 1.25
    coeff = (c, s, 640 - c * (640 + dx) - s * (360 + dy), -s, c, 360 + s * (640 + dx) - c * (360 + dy))
    return image.transform((PW, PH), Image.Transform.AFFINE, coeff, Image.Resampling.BICUBIC)


class PoorQuality:
    def __init__(self, ir):
        self.ir = ir
        self.size = (128, 72) if ir else (160, 90)
        w, h = self.size
        channels = 1 if ir else 3
        self.rng = np.random.default_rng(10921 if ir else 24019)
        self.fpn = self.rng.normal(0, 5.0, (h, w, channels)).astype(np.float32)
        self.previous = None

    def __call__(self, image):
        low = image.resize(self.size, Image.Resampling.LANCZOS).filter(ImageFilter.GaussianBlur(.65))
        a = np.asarray(low, dtype=np.float32)
        if self.ir:
            a = np.mean(a, axis=2, keepdims=True)
        h, w, _ = a.shape
        if self.previous is not None:
            a = .58 * a + .42 * self.previous
        self.previous = a.copy()
        a = a + self.fpn + self.rng.normal(0, 13.5, a.shape) + self.rng.normal(0, 5.5, (h, 1, 1))
        a = np.clip(a, 0, 255).astype(np.uint8)
        if self.ir:
            a = np.repeat(a, 3, axis=2)
        return Image.fromarray(a).resize((PW, PH), Image.Resampling.BILINEAR)


def quantize(image):
    a = np.asarray(image, dtype=np.float32)
    return Image.fromarray((np.rint(a / 85) * 85).astype(np.uint8))


def hash2(x, y, seed=0):
    a = np.sin(x * 127.1 + y * 311.7 + seed * 19.713) * 43758.5453123
    return a - np.floor(a)


def noise2(x, y, scale, seed):
    x, y = x / scale, y / scale
    ix, iy = np.floor(x), np.floor(y)
    fx, fy = x - ix, y - iy
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    a = hash2(ix, iy, seed) * (1 - fx) + hash2(ix + 1, iy, seed) * fx
    b = hash2(ix, iy + 1, seed) * (1 - fx) + hash2(ix + 1, iy + 1, seed) * fx
    return ((a * (1 - fy) + b * fy) * 2 - 1).astype(np.float32)


def regional_fields(width, height):
    """Fixed image-space patterns: mottling, medium blotches, repair patches, per-region tint.

    Re-implemented from the previous deck's description (fixed spatial functions, RGB display gain and IR display
    offset) without the 3D surface proxy, because the split case results keep no per-frame camera table.
    """
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    u, v = xx / width, yy / height
    broad = noise2(u * 1000, v * 1000, 240, 19)
    medium = noise2(u * 1000, v * 1000, 75, 37)
    fine = noise2(u * 1000, v * 1000, 22, 73)
    cx, cy = np.floor(xx / (width / 46)), np.floor(yy / (height / 30))
    px, py = xx - cx * (width / 46), yy - cy * (height / 30)
    patch = ((hash2(cx, cy, 291) > .55) & (px > width / 90) & (px < width / 45) & (py > height / 60) & (py < height / 36)).astype(np.float32)
    effect = .13 * broad + .08 * medium + .035 * fine - .07 * patch
    gain = np.repeat((1 + effect)[..., None], 3, axis=2)
    gain[..., 0] += .025 * medium
    gain[..., 2] -= .035 * medium
    offset = -12 - 22 * broad - 10 * medium + 6 * patch
    return gain.astype(np.float32), offset.astype(np.float32)


def apply_regional(rgb, ir, fields):
    gain, offset = fields
    a = np.asarray(rgb, dtype=np.float32)
    gray = np.asarray(ir, dtype=np.float32).mean(2)
    weight = np.ones(a.shape[:2], np.float32)
    weight *= np.clip((gray - 4) / 25, 0, 1)
    sky = (np.clip((a[..., 0] - 135) / 10, 0, 1) * np.clip((a[..., 1] - a[..., 0] - 2) / 3, 0, 1)
           * np.clip((a[..., 2] - a[..., 1] - 1) / 2, 0, 1) * np.clip((a[..., 2] - a[..., 0] - 7) / 3, 0, 1))
    weight *= 1 - sky
    vegetation = (a[..., 1] > a[..., 0] + 3) & (a[..., 1] > a[..., 2] - 2)
    weight[vegetation] = .2
    weight *= np.clip(a.mean(2) / 25, 0, 1)
    weight[a.min(2) > 205] *= .2
    new = np.clip(a * (1 + (gain - 1) * weight[..., None]), 0, 255).astype(np.uint8)
    iw = weight * np.clip(gray / 35, 0, 1)
    corrected = np.clip(gray + offset * iw, 0, 255).astype(np.uint8)
    return Image.fromarray(new), Image.fromarray(corrected).convert('RGB')


def lost_card():
    image = Image.new('RGB', (PW, PH), '#18222a')
    d = ImageDraw.Draw(image)
    d.text((220, 270), '信号中断 · 接触后无有效前视观测', font=FONT, fill='#d6b48d')
    d.text((220, 330), '接触时刻 12.0596 s', font=SMALL, fill='#a9bbc7')
    return image


def four_camera(external, pair, n, label, title_left, title_right):
    sheet = Image.new('RGB', (2560, 1600), '#10171d')
    d = ImageDraw.Draw(sheet)
    names = [title_left, title_right, f'前视 IR · {label}', f'前视 RGB · {label}']
    for i, (name, image) in enumerate(zip(names, [external[0], external[1], pair[0], pair[1]])):
        x, y = (i % 2) * 1280, (i // 2) * 800
        d.text((x + 24, y + 20), name, font=FONT, fill='white')
        d.text((x + 1040, y + 25), f't={(n - 1) / FPS:.3f}s', font=SMALL, fill='#c9dce8')
        sheet.paste(image, (x, y + 80))
    return sheet


def comparison(pairs, n):
    sheet = Image.new('RGB', (1280, 1600), '#10171d')
    d = ImageDraw.Draw(sheet)
    labels = ['原版', '强抖动（画面后期）', '低观测质量（画面后期）', '低量化 · 每通道4级（显示后期）']
    for row, (label, pair) in enumerate(zip(labels, pairs)):
        y = row * 400
        d.text((12, y + 3), label, font=TITLE, fill='white')
        d.text((1130, y + 7), f'{(n - 1) / FPS:.2f}s', font=SMALL, fill='#c9dce8')
        d.text((520, y + 7), 'IR', font=SMALL, fill='#95d7dc')
        d.text((1000, y + 7), 'RGB', font=SMALL, fill='#95d7dc')
        for col, image in enumerate(pair):
            resample = Image.Resampling.NEAREST if row == 3 else Image.Resampling.BILINEAR
            sheet.paste(image.resize((640, 360), resample), (col * 640, y + 40))
    return sheet


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--external', type=Path, required=True, help='result folder of the external (observer) case')
    p.add_argument('--forward', type=Path, required=True, help='result folder of the forward camera case')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--ffmpeg', type=Path, required=True)
    p.add_argument('--frames', type=int, default=504)
    p.add_argument('--first', type=int, default=1)
    add_argument(p)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    ext_rgb = sorted((a.external / 'observed_optical').glob('frame_*.png'))
    ext_ir = sorted((a.external / 'observed_ir').glob('frame_*.png'))
    fwd_ir = sorted((a.forward / 'observed_ir').glob('frame_*.png'))
    fwd_rgb = sorted((a.forward / 'observed_optical').glob('frame_*.png'))
    if not (ext_rgb and ext_ir and fwd_ir and fwd_rgb):
        raise SystemExit('missing observed_* frame folders')
    valid = min(len(fwd_ir), len(fwd_rgb), len(ext_ir))
    first, last = a.first, a.first + a.frames - 1
    if last > len(ext_ir):
        raise SystemExit(f'external frames end at {len(ext_ir)}, requested {last}')

    encoder = pick_encoder(a.ffmpeg, getattr(a, 'video_encoder', 'auto'))
    args = encoder_args(encoder)
    outputs = {
        '00_基准_四路相机': ((2560, 1600), None),
        '01_强抖动': ((2560, 1600), '强抖动 · 画面后期'),
        '02_低观测质量': ((2560, 1600), '低观测质量 · 画面后期'),
        '03_低量化': ((2560, 1600), '低量化 · 每通道4级 · 显示后期'),
        '04_区域外观差异': ((2560, 1600), '区域外观差异 · 显示后期'),
        '05_同屏对照_IR与RGB': ((1280, 1600), None),
    }
    procs = {}
    for name, (size, _) in outputs.items():
        path = a.out / f'{name}.mp4'
        cmd = [str(a.ffmpeg), '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{size[0]}x{size[1]}',
               '-r', str(FPS), '-i', '-', '-pix_fmt', 'yuv420p', *args, str(path)]
        procs[name] = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)

    poor_ir, poor_rgb = PoorQuality(True), PoorQuality(False)
    card = lost_card()
    fields = regional_fields(PW, PH)
    try:
        for index, n in enumerate(range(first, last + 1)):
            external = (Image.open(ext_rgb[index]).convert('RGB'), Image.open(ext_ir[index]).convert('RGB'))
            if n <= valid:
                original = (Image.open(fwd_ir[index]).convert('RGB'), Image.open(fwd_rgb[index]).convert('RGB'))
                shaking = tuple(shake(image, n) for image in original)
                poor = (poor_ir(original[0]), poor_rgb(original[1]))
                quantized = tuple(quantize(image) for image in original)
                regional = apply_regional(original[1], original[0], fields)
            else:
                original = shaking = poor = quantized = regional = (card, card)
            for name, pair, label in (
                    ('00_基准_四路相机', original, '原版'),
                    ('01_强抖动', shaking, '强抖动 · 画面后期'),
                    ('02_低观测质量', poor, '低观测质量 · 画面后期'),
                    ('03_低量化', quantized, '低量化 · 每通道4级 · 显示后期'),
                    ('04_区域外观差异', regional, '区域外观差异 · 显示后期')):
                procs[name].stdin.write(four_camera(external, pair, n, label, '外部相机 · RGB', '外部相机 · IR').tobytes())
            procs['05_同屏对照_IR与RGB'].stdin.write(comparison([original, shaking, poor, quantized], n).tobytes())
            if (index + 1) % 60 == 0:
                print(f'frames {index + 1}/{a.frames}', flush=True)
        for process in procs.values():
            process.stdin.close()
    finally:
        errors = []
        for name, process in procs.items():
            err = process.stderr.read().decode(errors='replace')
            if process.wait() != 0:
                errors.append(f'{name}: {err[-400:]}')
        if errors:
            raise SystemExit('encoding failed: ' + '; '.join(errors))
    for name in outputs:
        path = a.out / f'{name}.mp4'
        print(f'{path.name}  {path.stat().st_size / 1e6:.2f} MB')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
