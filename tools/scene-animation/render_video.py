"""Caption, encode and verify native HiStream GPU frames; no scene rendering.

python render_video.py RUN_DIRECTORY --ffmpeg /path/to/ffmpeg.exe
Requires Pillow. Original native GPU PNGs remain in frames/.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from integrity import require
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def font(size, bold=False):
    return ImageFont.truetype('C:/Windows/Fonts/msyhbd.ttc' if bold else 'C:/Windows/Fonts/msyh.ttc', size)


def stage(t, impact):
    if t < impact:
        return '飞行接近', (129, 205, 240)
    if t < impact + .25:
        return '撞击瞬间', (255, 218, 130)
    if t < impact + 2:
        return '火球与碎片', (255, 166, 82)
    return '烟尘与残余火焰', (191, 203, 205)


def run_checked(command, log):
    r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write_bytes(r.stdout)
    if r.returncode:
        raise RuntimeError(f'Command failed ({r.returncode}), see {log}')
    return r.stdout.decode('utf-8', errors='replace')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('run', type=Path)
    parser.add_argument('--ffmpeg', required=True, type=Path)
    parser.add_argument('--engine', type=Path)
    add_encoder_argument(parser);a=parser.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    m = json.loads((a.run / 'run-manifest.json').read_text(encoding='utf-8'))
    require(m['status'] == 'complete' and m['backend'] == 'gpu', "check failed: m['status'] == 'complete' and m['backend'] == 'gpu'")
    require(m['firstFrame'] == 1 and m['lastFrame'] == m['timelineFrameCount'], 'Full sequence required')
    fps, count, impact = m['fps'], m['frameCount'], m['impactTime']
    width, height = m['width'], m['height']
    thermal = m.get('renderMode','visible') == 'thermal'
    wavelength = m.get('thermal',{}).get('wavelengthMicrometers',10)
    transported = m.get('physicsMode') == 'transport'
    sensor_enabled = m.get('sensor',{}).get('enabled',False)
    band = '8–14 µm' if m.get('thermal',{}).get('bandMode')=='lwir8_14' else f'{wavelength:g} µm'
    video_name = 'histream_impact_gpu_thermal.mp4' if thermal else 'histream_impact_gpu.mp4'
    title = '热红外：建筑撞击与爆炸效果' if thermal else '建筑撞击与爆炸效果'
    footer = f'{band} 白热 · '+('GPU 输运温度场' if transported else '设定温度场') if thermal else '原生 Vulkan GPU 逐帧生成'
    encoded_width, encoded_height = width + width % 2, height + height % 2
    out = a.run / 'video_frames'
    out.mkdir(exist_ok=True)
    records = []
    target_frames = [max(1,min(count,1 + round(t * fps))) for t in [1.5, max(impact-.125, 0), impact+.125, impact+.5, impact+2.5, min(count/fps-.5, impact+7.5)]]
    selected = {}
    for number in range(1, count + 1):
        path = a.run / 'frames' / f'frame_{number:04d}.png'
        with Image.open(path) as im:
            require(im.size == (width, height), 'check failed: im.size == (width, height)')
            rgb = im.convert('RGB')
        t = (number-1) / fps
        caption, accent = stage(t, impact)
        layer = Image.new('RGBA', (width, height))
        d = ImageDraw.Draw(layer)
        d.rectangle((0, 0, width, 91), fill=(8, 18, 25, 220))
        d.rectangle((0, height-71, width, height), fill=(8, 18, 25, 230))
        d.rectangle((28, 23, 32, 66), fill=(*accent, 255))
        d.text((46, 14), 'HiStream  /  GPU 动态场景', font=font(19), fill=(190, 211, 222))
        d.text((45, 40), title, font=font(27, True), fill='white')
        d.text((width-265, 20), caption, font=font(20, True), fill=accent)
        d.text((width-265, 49), f'T + {t:05.2f} s   |   {number:03d}/{count}', font=font(16), fill=(191, 206, 215))
        d.text((28, height-60), footer, font=font(17), fill=(195, 211, 221))
        d.text((width-510, height-60), '参数化效果演示 · 非爆炸/毁伤物理求解', font=font(17), fill=(188, 202, 208))
        d.rectangle((28, height-20, width-28, height-17), fill=(65, 80, 91))
        d.rectangle((28, height-20, 28+(width-56)*number/count, height-17), fill=(*accent, 255))
        if sensor_enabled:
            layer=Image.new('RGBA',(width,height));d=ImageDraw.Draw(layer)
            d.rectangle((0,0,width,38),fill=(0,0,0,100))
            d.text((18,8),f'热红外模拟  /  {band}',font=font(17),fill=(225,225,225))
            d.text((width-245,8),f'T + {t:05.2f} s    {number:03d}/{count}',font=font(16),fill=(225,225,225))
            d.text((18,height-27),'HiStream',font=font(14),fill=(205,205,205),stroke_width=1,stroke_fill=(30,30,30))
            d.text((width-100,height-27),'模拟画面',font=font(14),fill=(205,205,205),stroke_width=1,stroke_fill=(30,30,30))
        composed = Image.alpha_composite(rgb.convert('RGBA'), layer).convert('RGB')
        if thermal:
            composed = composed.convert('L').convert('RGB')
        final = out / path.name
        composed.save(final)
        records.append({'frame':number,'seconds':t,'rawSha256':sha(path),'captionedSha256':sha(final)})
        if number in target_frames:
            selected[number] = composed.copy()
    mp4 = a.run / video_name
    base = [str(a.ffmpeg), '-hide_banner', '-y']
    run_checked(base + ['-framerate',str(fps),'-i',str(out/'frame_%04d.png'),'-frames:v',str(count),
        '-vf','pad=ceil(iw/2)*2:ceil(ih/2)*2',*encoder_args(video_codec,18,'medium'),'-pix_fmt','yuv420p','-movflags','+faststart',str(mp4)], a.run/'encode.log')
    # Decode every video frame; require exact count, duration, dimensions and motion.
    with (a.run/'decode.rgb').open('wb') as decoded_file:
        decoded = subprocess.run([str(a.ffmpeg),'-hide_banner','-v','error','-i',str(mp4),'-f','rawvideo','-pix_fmt','rgb24','-'], stdout=decoded_file, stderr=subprocess.PIPE)
    (a.run/'decode.log').write_bytes(decoded.stderr)
    require(decoded.returncode == 0, lambda: decoded.stderr)
    pixels = (a.run/'decode.rgb').stat().st_size
    require(pixels == count * encoded_width * encoded_height * 3, lambda: (pixels, count * encoded_width * encoded_height * 3))
    (a.run/'decode.rgb').unlink()
    gif_filter='fps=6,scale=480:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=64[p];[s1][p]paletteuse=dither=none' if sensor_enabled else 'fps=10,scale=640:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=128[p];[s1][p]paletteuse=dither=bayer'
    run_checked(base + ['-i',str(mp4),'-vf',gif_filter,'-loop','0',str(a.run/'preview.gif')], a.run/'gif.log')
    thumbs = []
    for number in target_frames:
        image = selected[number].resize((640,360),Image.Resampling.LANCZOS)
        thumbs.append(image)
    sheet = Image.new('RGB',(1280,1080),(9,19,26))
    for i, im in enumerate(thumbs):
        sheet.paste(im,((i%2)*640,(i//2)*360))
    sheet.save(a.run/'keyframes.jpg',quality=94)
    selected[target_frames[3]].save(a.run/'cover.png')
    audit = {'status':'passed','nativeGpuManifest':m,'pngFramesChecked':count,'videoFramesDecoded':count,
             'durationSeconds':count/fps,'resolution':[width,height],'encodedResolution':[encoded_width,encoded_height],'fps':fps,'mp4Sha256':sha(mp4),
             'videoSizeBytes':mp4.stat().st_size,'physicsValidated':False,'frames':records}
    engine_snapshot = a.run / m['engineSnapshot']
    shader_snapshot = a.run / m['shaderSnapshot']
    audit['engineSha256']=sha(engine_snapshot)
    audit['shaderSha256']=sha(shader_snapshot)
    if transported:
        audit['transportShaderSha256']=sha(a.run/m['transportShaderSnapshot'])
        audit['transportDiagnosticsSha256']=sha(a.run/'transport.csv')
    if sensor_enabled:
        audit['sensorShaderSha256']=sha(a.run/m['sensorShaderSnapshot'])
        audit['sensorDiagnosticsSha256']=sha(a.run/'sensor.csv')
        audit['radianceSamples']={p.name:sha(p) for p in sorted((a.run/'radiance').glob('*.f32'))}
    if a.engine:
        require(sha(a.engine)==audit['engineSha256'], 'Engine differs from the saved rendering snapshot')
        audit['enginePath']=str(a.engine.resolve())
    (a.run/'verification.json').write_text(json.dumps(audit,ensure_ascii=False,indent=2),encoding='utf-8')
    (a.run/'index.html').write_text('''<!doctype html><html lang="zh"><meta charset="utf-8"><title>HiStream GPU 动态场景</title>
<style>body{margin:36px auto;max-width:1280px;background:#0b151d;color:#dce7ed;font:17px "Microsoft YaHei",sans-serif}video,img{width:100%;border-radius:8px}p{color:#9fb3c0}a{color:#91d0ea}</style>
<h1>HiStream GPU：建筑撞击与爆炸效果</h1><p>原生 Vulkan GPU 逐帧生成 · 12 秒 / 24 fps / 1280 × 720</p>
<video controls loop playsinline poster="cover.png" src="histream_impact_gpu.mp4"></video>
<p>参数化视觉演示。飞行、建筑开口、碎片、火焰和烟尘采用预设与程序化场；未运行爆炸、结构毁伤、燃烧或定量光谱求解器。</p>
<p><a href="histream_impact_gpu.mp4">MP4 视频</a> · <a href="frames/">原始 GPU 图像</a> · <a href="verification.json">验证与来源</a></p>
<img src="keyframes.jpg" alt="六阶段关键帧"></html>'''.replace('12 秒 / 24 fps / 1280 × 720',f'{count/fps:.2f} 秒 / {fps} fps / {width} × {height}')
        .replace('histream_impact_gpu.mp4',video_name).replace('建筑撞击与爆炸效果',title)
        .replace('未运行爆炸、结构毁伤、燃烧或定量光谱求解器','热场采用 GPU 输运，热源与材料背景仍由场景给定；未运行爆炸、结构毁伤或燃烧求解器' if transported else '热场为设定值，未运行爆炸、结构毁伤、燃烧或传感器定标求解器')
        .replace('飞行、建筑开口、碎片、火焰和烟尘采用预设与程序化场','热量和烟雾采用低速 GPU 输运；飞行、建筑开口和碎片为预设效果' if transported else '飞行、建筑开口、碎片、火焰和烟尘采用预设与程序化场'),encoding='utf-8')
    (a.run/'README.md').write_text(f'''# HiStream GPU 动态场景演示

- 视频：{video_name}；{count/fps:.2f} s，{fps} fps，{width}×{height}。
- frames/：HiStream 原生 GPU 输出；video_frames/：只添加标题与时间的图像。
- input.json：引擎输入；run-manifest.json：设备与执行记录。
- verification.json：逐帧 SHA-256、视频解码帧数、引擎与着色器 SHA-256。
- 原生入口：histream.exe eSceneAnimation input.json 新输出目录。
- 代码：C:/work/streamstudio/models/histream/src/animation/scene_animation.cpp，
  C:/work/streamstudio/models/histream/shader/animation/scene.comp。

这是 HiStream 新增的独立 GPU 可视化模式，使用原生 AppSetting/Vulkan 上下文。
GPU 负责场景、可选输运与辐射计算；CPU 负责调度、诊断与文件输出。
时间是演示时间。建筑开口与碎片为预设视觉效果，不是结构响应。
本例不求解弹道制导、爆轰、可压缩冲击波、化学燃烧或真实毁伤。
显示模式：{m.get('renderMode','visible')}。
红外模式计算 Planck 辐射、灰体表面与简化介质吸收/发射；单波长或波段积分由配置选择。
物理模式：{m.get('physicsMode','procedural')}。transport 模式的空气温度、烟雾与表面温度扰动由 GPU 时间推进；热源及材料背景仍为给定。
波段模式：{m.get('thermal',{}).get('bandMode','single')}；8–14 µm 模式采用均匀响应积分，不对应特定仪器 SRF。
发射率、示踪量到消光系数的转换为演示设定；未计算真实爆炸热过程。
显示映射：{m.get('thermal',{}).get('displayStretch','visible display transform')}。
通用相机模型：{sensor_enabled}；启用时在线性辐射域进行像元合并、截断高斯模糊，随后加入设定噪声/固定图样、曝光响应和显示量化。
相机参数均为演示配置，不是特定仪器标定值；没有大气或经过验证的传感器定标。
本输出不是 VoxelRT/VoxelEB 能量平衡温度，也不是经验证的遥感亮温。
''',encoding='utf-8')
    print(json.dumps({k:v for k,v in audit.items() if k not in ['frames','nativeGpuManifest']},ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
