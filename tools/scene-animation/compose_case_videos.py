"""Compose and encode case videos from scene-animation frame folders.

Layouts follow the frame count: one folder gives a single panel, two give a side-by-side (or stacked) comparison,
four give the 2x2 grid used by the airfield deck (one captioned panel per group). Frames are fitted into their panel
without distortion (letterboxed). Every panel carries a title strip, and the bottom strip carries the frame number,
the physical time and the slow-motion factor, so a slowed clip cannot be mistaken for real time.

usage:
  compose_case_videos.py --frames DIR --title TEXT [--second-frames DIR --second-title TEXT]
        [--third-frames DIR --third-title TEXT] [--fourth-frames DIR --fourth-title TEXT]
        --out FILE --ffmpeg PATH
        [--panel 640x410 | --size WxH] [--layout auto|hstack|vstack|grid]
        [--first 1] [--last N] [--fps 24] [--time-scale 0.2] [--note TEXT]
        [--video-encoder auto] [--receipt FILE]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from video_encoder import add_argument, encoder_args, pick_encoder  # noqa: E402

FONT = 'C:/Windows/Fonts/msyh.ttc'
BACKGROUND = (10, 15, 19)


def frames_in(folder: Path, first: int, last: int | None) -> list[Path]:
    files = sorted(folder.glob('frame_*.png'))
    if not files:
        raise SystemExit(f'no frame_*.png in {folder}')
    numbers = [int(f.stem.split('_')[1]) for f in files]
    if numbers != list(range(numbers[0], numbers[0] + len(numbers))):
        raise SystemExit(f'frame numbering has gaps in {folder}')
    end = numbers[-1] if last is None else min(last, numbers[-1])
    chosen = [f for f, n in zip(files, numbers) if first <= n <= end]
    if not chosen:
        raise SystemExit(f'no frames in {first}..{end} of {folder}')
    return chosen


def fit(image: Image.Image, box: tuple[int, int]) -> Image.Image:
    """Scale to fit the box preserving aspect, centred on black."""
    width, height = box
    if width < 2 or height < 2:
        return Image.new('RGB', (max(1, width), max(1, height)), (0, 0, 0))
    scale = min(width / image.width, height / image.height)
    size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    scaled = image.convert('RGB').resize(size, Image.Resampling.LANCZOS)
    panel = Image.new('RGB', (width, height), (0, 0, 0))
    panel.paste(scaled, ((width - size[0]) // 2, (height - size[1]) // 2))
    return panel


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--frames', type=Path, required=True)
    p.add_argument('--title', required=True)
    p.add_argument('--second-frames', type=Path, default=None)
    p.add_argument('--second-title', default='')
    p.add_argument('--third-frames', type=Path, default=None)
    p.add_argument('--third-title', default='')
    p.add_argument('--fourth-frames', type=Path, default=None)
    p.add_argument('--fourth-title', default='')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--layout', choices=('auto', 'hstack', 'vstack', 'grid'), default='auto')
    p.add_argument('--panel', default=None, help='one panel WxH, for example 640x410')
    p.add_argument('--size', default='1280x720', help='content canvas WxH when --panel is not given')
    p.add_argument('--first', type=int, default=1)
    p.add_argument('--last', type=int, default=None)
    p.add_argument('--fps', type=float, default=24)
    p.add_argument('--time-scale', type=float, default=1.0, help='physical seconds per video second (1 = real time)')
    p.add_argument('--note', default='', help='extra note drawn in the bottom strip')
    p.add_argument('--ffmpeg', type=Path, required=True)
    p.add_argument('--receipt', type=Path, default=None)
    add_argument(p)
    a = p.parse_args()

    sources = [(a.frames, a.title)]
    if a.second_frames:
        sources.append((a.second_frames, a.second_title))
    if a.third_frames:
        sources.append((a.third_frames, a.third_title))
    if a.fourth_frames:
        sources.append((a.fourth_frames, a.fourth_title))
    if len(sources) == 3:
        raise SystemExit('three panels are not supported; use one, two (hstack/vstack) or four (grid)')
    lists = [frames_in(folder, a.first, a.last) for folder, _ in sources]
    count = len(lists[0])
    if any(len(item) != count for item in lists):
        raise SystemExit(f'frame counts differ: ' + ', '.join(str(len(item)) for item in lists))

    layout = a.layout
    if layout == 'auto':
        layout = 'single' if len(sources) == 1 else ('grid' if len(sources) == 4 else 'hstack')
    if len(sources) == 1 and layout != 'single':
        raise SystemExit('one folder only supports the single layout')
    if len(sources) == 2 and layout not in ('hstack', 'vstack'):
        raise SystemExit('two folders need hstack or vstack')
    if len(sources) == 4 and layout != 'grid':
        raise SystemExit('four folders need the grid layout')

    if a.panel:
        panel_w, panel_h = (int(v) for v in a.panel.lower().split('x'))
    else:
        canvas_w, canvas_h = (int(v) for v in a.size.lower().split('x'))
        cols, rows = {'single': (1, 1), 'hstack': (2, 1), 'vstack': (1, 2), 'grid': (2, 2)}[layout]
        panel_w, panel_h = canvas_w // cols, canvas_h // rows
    cols, rows = {'single': (1, 1), 'hstack': (2, 1), 'vstack': (1, 2), 'grid': (2, 2)}[layout]
    out_w, content_h = panel_w * cols, panel_h * rows

    # Caption geometry scales with the panel so small clips are not all text.
    title_h = max(14, round(panel_h * 0.075))
    title_size = max(10, round(title_h * 0.55))
    bar_bottom = max(16, round(content_h * 0.04))
    small_size = max(10, round(bar_bottom * 0.55))
    out_h = content_h + bar_bottom
    title_font = ImageFont.truetype(FONT, title_size)
    small_font = ImageFont.truetype(FONT, small_size)

    encoder = pick_encoder(a.ffmpeg, getattr(a, 'video_encoder', 'auto'))
    command = [str(a.ffmpeg), '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{out_w}x{out_h}',
               '-r', str(a.fps), '-i', '-', '-pix_fmt', 'yuv420p', *encoder_args(encoder), str(a.out)]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for index in range(count):
            image = Image.new('RGB', (out_w, out_h), BACKGROUND)
            draw = ImageDraw.Draw(image)
            for slot, ((folder, title), sequence) in enumerate(zip(sources, lists)):
                col, row = slot % cols, slot // cols
                x0, y0 = col * panel_w, row * panel_h
                inner = (panel_w, panel_h - title_h)
                image.paste(fit(Image.open(sequence[index]), inner), (x0, y0 + title_h))
                draw.rectangle((x0, y0, x0 + panel_w, y0 + title_h), fill=(18, 24, 30))
                draw.text((x0 + max(4, round(panel_w * 0.01)), y0 + max(1, (title_h - title_size) // 2)),
                          title, font=title_font, fill=(226, 234, 240))
                if cols > 1 and col < cols - 1:
                    draw.line((x0 + panel_w - 1, y0, x0 + panel_w - 1, y0 + panel_h), fill=(40, 48, 56), width=2)
                if rows > 1 and row < rows - 1:
                    draw.line((x0, y0 + panel_h - 1, x0 + panel_w, y0 + panel_h - 1), fill=(40, 48, 56), width=2)
            draw.rectangle((0, out_h - bar_bottom, out_w, out_h), fill=BACKGROUND)
            number = int(lists[0][index].stem.split('_')[1])
            physical = index / a.fps * a.time_scale
            stamp = f'帧 {number}/{int(lists[0][-1].stem.split("_")[1])}  物理 t = {physical:06.3f} s'
            if a.time_scale != 1:
                stamp += f'  慢放 ×{1 / a.time_scale:g}'
            if a.note:
                stamp += '  ·  ' + a.note
            draw.text((max(4, round(out_w * 0.005)), out_h - bar_bottom + max(1, (bar_bottom - small_size) // 2)),
                      stamp, font=small_font, fill=(203, 215, 226))
            process.stdin.write(image.tobytes())
        process.stdin.close()
    finally:
        error = process.stderr.read().decode(errors='replace')
        code = process.wait()
    if code != 0:
        raise SystemExit(f'ffmpeg failed ({code}): {error[-1500:]}')
    size_mb = a.out.stat().st_size / 1e6
    print(f'{a.out}  {count} frames  {count / a.fps:.2f} s  {out_w}x{out_h}  {size_mb:.2f} MB  {encoder}  {layout}')
    if a.receipt:
        a.receipt.write_text(json.dumps(dict(output=str(a.out), frames=count, firstFrame=a.first,
                                             lastFrame=int(lists[0][-1].stem.split('_')[1]), fps=a.fps,
                                             durationSeconds=count / a.fps, width=out_w, height=out_h,
                                             layout=layout, panel=[panel_w, panel_h],
                                             presentationTimeScale=a.time_scale, slowMotionFactor=1 / a.time_scale,
                                             encoder=encoder,
                                             panels=[dict(frames=str(folder), title=title) for folder, title in sources],
                                             note=a.note or None),
                                        indent=2, ensure_ascii=False), encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
