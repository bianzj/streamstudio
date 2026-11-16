"""Package native camera observations; verify every frame and fully decode videos."""
import argparse
import csv
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from package_mesh_sequence import checked
from prepare_thermal_case import require, sha
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def main():
    p = argparse.ArgumentParser()
    p.add_argument('run', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--ffmpeg', type=Path, required=True)
    add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    run, out = a.run.resolve(), a.output.resolve()
    m = json.loads((run / 'run-manifest.json').read_text(encoding='utf-8'))
    require(m['status'] == 'complete' and m['cameraObservation']['enabled'], 'Completed camera run required')
    count, fps = m['timelineFrameCount'], m['fps']
    require(m['firstFrame'] == 1 and m['lastFrame'] == count, 'Full sequence required')
    require(not out.exists(), 'Use a new packaging directory')
    records = {}
    ir_size = tuple(m['cameraObservation']['ir']['pixels'])
    adc_max = (1 << m['cameraObservation']['ir']['adcBits']) - 1
    for folder in ['frames', 'fixed_frames', 'optical_frames', 'observed_ir', 'observed_optical', 'detector_ir']:
        require(len(list((run / folder).glob('*.png'))) == count, 'Frame count: ' + folder)
        for n in range(1, count + 1):
            f = run / folder / f'frame_{n:04d}.png'
            with Image.open(f) as im:
                require(im.size == (ir_size if folder == 'detector_ir' else (m['width'], m['height'])), 'Frame dimensions')
                if folder == 'detector_ir':
                    values = np.asarray(im)
                    require(values.min() >= 0 and values.max() <= adc_max, 'ADC range')
                else:
                    im.verify()
            records[str(f.relative_to(run))] = sha(f)
    rows = list(csv.DictReader((run / 'observation.csv').open(encoding='utf-8')))
    require([int(float(r['frame'])) for r in rows] == list(range(1, count + 1)), 'Observation receipt gaps')
    require(all(abs(float(r['exposureWeightSum']) - 1) < 1e-5 for r in rows), 'Exposure weights')
    out.mkdir(parents=True)
    comparisons = out / 'comparison_frames'
    comparisons.mkdir()
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 20)
    small = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 17)
    kinds = ['optical_frames', 'observed_optical', 'frames', 'observed_ir']
    ir_profile = f'{ir_size[0]}×{ir_size[1]} / {m["cameraObservation"]["ir"]["adcBits"]} bit ADC'
    labels = ['材质与光照 · 理想 RGB', 'RGB 观测 · 曝光、PSF、抖动',
              '参考红外显示 · 原成像链', '红外观测 · ' + ir_profile]
    if m['cameraObservation'].get('irDisplay', {}).get('mode') == 'linear-percent-2':
        labels[3] += ' · 2%拉伸'
        sigma = m['cameraObservation'].get('regionalBlur', {}).get('sigmaSourcePixels', [0, 0, 0, 0])
        if m['cameraObservation']['rgb']['gaussianPsfSigmaDetectorPixels'] == 0 and sigma[1] == 0 and sigma[3] == 0:
            labels[1] = 'RGB 观测 · 清晰、撞击后抖动'
    for n in range(1, count + 1):
        sheet = Image.new('RGB', (1280, 800), '#10171d')
        draw = ImageDraw.Draw(sheet)
        for i, (kind, label) in enumerate(zip(kinds, labels)):
            x, y = i % 2 * 640, i // 2 * 400
            draw.text((x + 12, y + 8), label, font=font, fill='white')
            with Image.open(run / kind / f'frame_{n:04d}.png') as im:
                sheet.paste(im.convert('RGB').resize((640, 360), Image.Resampling.LANCZOS), (x, y + 40))
        draw.text((1130, 9), f't={(n-1)/fps:.2f}s', font=small, fill='#cbdce6')
        sheet.save(comparisons / f'frame_{n:04d}.png', compress_level=2)
    videos = {}
    for name, folder in [('observed_rgb', run / 'observed_optical'), ('observed_ir', run / 'observed_ir'),
                         ('camera_comparison', comparisons)]:
        target = out / (name + '.mp4')
        checked([a.ffmpeg, '-y', '-hide_banner', '-framerate', fps, '-start_number', 1,
                 '-i', folder / 'frame_%04d.png', '-frames:v', count, *encoder_args(video_codec,17,'medium'), '-pix_fmt', 'yuv420p', '-movflags', '+faststart', target],
                out / (name + '-encode.log'))
        decoded = checked([a.ffmpeg, '-v', 'error', '-i', target, '-map', '0:v:0', '-f', 'null', '-',
                           '-progress', 'pipe:1', '-nostats'], out / (name + '-decode.log'))
        fields = dict(line.split('=', 1) for line in decoded.splitlines() if '=' in line)
        require(fields.get('progress') == 'end' and int(fields.get('frame', 0)) == count, 'Incomplete video decode')
        require(abs(float(fields['out_time_us']) / 1e6 - count / fps) < 1 / fps, 'Video duration')
        videos[name] = dict(path=str(target), sha256=sha(target), decodedFrames=count, seconds=count / fps)
    selected = [1, round((m['impactTime'] - .15) * fps) + 1, round((m['impactTime'] + .125) * fps) + 1,
                round((m['impactTime'] + .5) * fps) + 1, round((m['impactTime'] + 2) * fps) + 1, count]
    for kind, name in [('observed_optical', 'stages_rgb.jpg'), ('observed_ir', 'stages_ir.jpg')]:
        sheet = Image.new('RGB', (1920, 800), '#10171d')
        draw = ImageDraw.Draw(sheet)
        for i, n in enumerate(selected):
            n = max(1, min(count, n)); x, y = i % 3 * 640, i // 3 * 400
            with Image.open(run / kind / f'frame_{n:04d}.png') as im:
                sheet.paste(im.convert('RGB').resize((640, 360), Image.Resampling.LANCZOS), (x, y + 40))
            draw.text((x + 12, y + 8), f't={(n-1)/fps:.2f}s', font=font, fill='white')
        sheet.save(out / name, quality=95)
    chosen = max(1, min(count, round((m['impactTime'] + .5) * fps) + 1))
    with Image.open(comparisons / f'frame_{chosen:04d}.png') as im:
        im.save(out / 'camera_comparison.png')
    receipt = dict(status='passed', nativeFrameCount=count, fps=fps, nativeFrameSha256=records, videos=videos,
                   cameraModel=m['cameraObservation'], physicalAppearance=m.get('physicalAppearance'),
                   scope='Numerical pipeline and complete decoding verified; generic parameters, no measured-device calibration',
                   fieldValidation=False, structuralPrediction=False)
    (out / 'verification.json').write_text(json.dumps(receipt, indent=2, ensure_ascii=False), encoding='utf-8')
    (out / 'index.html').write_text('''<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>HiStream RGB / IR camera observations</title><style>body{background:#10171d;color:#e5edf2;font:16px system-ui;max-width:1280px;margin:24px auto;padding:12px}video,img{width:100%;display:block;margin:14px 0}</style>
<h1>HiStream · 材质与相机成像改进</h1><p>通用示例参数；红外原生 IR_PROFILE，存于 16 位 PNG。展示视频 DISPLAY_PROFILE，无新增探测信息。场景热源和破口为预设，未做设备实测标定。</p>
<h2>可见光观测</h2><video controls src="observed_rgb.mp4"></video><h2>红外观测</h2><video controls src="observed_ir.mp4"></video>
<h2>参考 / 观测连续对照</h2><video controls src="camera_comparison.mp4"></video><img src="camera_comparison.png"><img src="stages_rgb.jpg"><img src="stages_ir.jpg"></html>'''.replace('IR_PROFILE', ir_profile).replace('DISPLAY_PROFILE', f'{m["width"]}×{m["height"]} / {fps} fps / {count/fps:g} s'), encoding='utf-8')
    print(json.dumps(videos, indent=2))


if __name__ == '__main__':
    main()
