"""Reproduce the airfield-day MWIR deck videos (case 2) from its 12 two-hourly keyframes.

The previous deck stored 12 composed 2x2 sheets per view (t = 0, 2, ..., 22 h) and interpolated between them to
21 s? No: 288 frames at 24 fps = 12 s, i.e. 24 frames per keyframe interval. Each sheet already carries the four
groups (sun on/off x payload on/off) with their own titles, so this script only interpolates and encodes.

usage: build_airfield_videos.py --keyframes DIR --out DIR --ffmpeg FFMPEG [--frames 288] [--fps 24]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from video_encoder import add_argument, encoder_args, pick_encoder  # noqa: E402


def keyframes(folder: Path):
    files = sorted(folder.glob('frame_*.png'))
    if len(files) < 2:
        raise SystemExit(f'need at least two keyframes in {folder}')
    numbers = [int(f.stem.split('_')[1]) for f in files]
    return files, numbers


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--keyframes', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--ffmpeg', type=Path, required=True)
    p.add_argument('--name', default='histream_mwir_day')
    p.add_argument('--frames', type=int, default=288)
    p.add_argument('--fps', type=float, default=24)
    add_argument(p)
    a = p.parse_args()
    files, numbers = keyframes(a.keyframes)
    step = numbers[1] - numbers[0]
    a.out.mkdir(parents=True, exist_ok=True)

    with Image.open(files[0]) as probe:
        size = probe.size
    encoder = pick_encoder(a.ffmpeg, getattr(a, 'video_encoder', 'auto'))
    out = a.out / f'{a.name}.mp4'
    cmd = [str(a.ffmpeg), '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{size[0]}x{size[1]}',
           '-r', str(a.fps), '-i', '-', '-pix_fmt', 'yuv420p', *encoder_args(encoder), str(out)]
    process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    # Linear interpolation between neighbouring keyframes, clamped at both ends.
    images = [np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in files]
    try:
        for index in range(a.frames):
            position = index / step  # which keyframe interval
            lo = int(np.floor(position))
            fraction = position - lo
            left = images[min(lo, len(images) - 1)]
            right = images[min(lo + 1, len(images) - 1)]
            blend = left * (1 - fraction) + right * fraction
            process.stdin.write(np.clip(blend, 0, 255).astype(np.uint8).tobytes())
            if (index + 1) % 48 == 0:
                print(f'frames {index + 1}/{a.frames}', flush=True)
        process.stdin.close()
    finally:
        error = process.stderr.read().decode(errors='replace')
        code = process.wait()
    if code != 0:
        raise SystemExit(f'ffmpeg failed ({code}): {error[-1200:]}')
    print(f'{out.name}  {a.frames} frames  {a.frames / a.fps:.2f} s  {size[0]}x{size[1]}  {out.stat().st_size / 1e6:.2f} MB')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
