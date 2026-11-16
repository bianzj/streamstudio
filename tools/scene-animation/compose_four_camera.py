"""Compose the requested four native camera channels without rerendering."""
import argparse,hashlib,json,math,shutil,subprocess
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def checked(command,log):
    result=subprocess.run([str(x) for x in command],capture_output=True)
    log.write_bytes(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError(f'Command failed; see {log}')
    return result.stdout.decode('utf-8',errors='replace')

def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);p.add_argument('output',type=Path);p.add_argument('--ffmpeg',type=Path,required=True);add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder)
    case,out=a.case.resolve(),a.output.resolve();config=read(case/'case.json');body=case/'result_final';external=Path(config['reference'])/'result_final';m,b=read(body/'run-manifest.json'),read(external/'run-manifest.json')
    if out.exists():raise FileExistsError('Use a new presentation directory')
    if m['status']!='complete' or b['status']!='complete':raise ValueError('Both native runs must be complete')
    if m['fps']!=b['fps'] or m['impactTime']!=b['impactTime'] or m['firstFrame']!=1 or b['firstFrame']!=1:raise ValueError('Native timelines differ')
    if sha(external/'run-manifest.json')!=config['referenceManifestSha256']:raise ValueError('External reference changed')
    if m['frameCount']!=config['validFrames'] or b['frameCount']!=config['frameCount']:raise ValueError('Native frame counts differ')
    fps,total,valid,impact=m['fps'],b['frameCount'],m['frameCount'],m['impactTime']
    if not (valid-1)/fps<impact<=valid/fps:raise ValueError('Invalid body contact boundary')
    if any((x['width'],x['height'])!=(1280,720) for x in [m,b]):raise ValueError('This layout requires1280x720 native display frames')
    # Reading order requested by the user: external RGB, external IR,
    # forward IR, forward RGB. Keep each native1280x720 image intact.
    panels=[('external_rgb','外部相机 · RGB',external/'observed_optical',False),
            ('external_ir','外部相机 · IR',external/'observed_ir',False),
            ('forward_ir','前相机 · IR',body/'observed_ir',True),
            ('forward_rgb','前相机 · RGB',body/'observed_optical',True)]
    out.mkdir(parents=True);frames=out/'frames';frames.mkdir();font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',36);small=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',27)
    inputs={};timeline=[];snapshots={1,min(145,valid),min(265,valid),min(valid+30,total)}
    for n in range(1,total+1):
        t=(n-1)/fps;bodyValid=n<=valid and t<impact;sheet=Image.new('RGB',(2560,1600),'#10171d');draw=ImageDraw.Draw(sheet);sources={}
        for i,(key,label,folder,isBody) in enumerate(panels):
            x,y=i%2*1280,i//2*800;draw.text((x+24,y+19),label,font=font,fill='white');draw.text((x+1010,y+24),f't = {t:.3f} s',font=small,fill='#c9dce8')
            if not isBody or bodyValid:
                f=folder/f'frame_{n:04d}.png'
                with Image.open(f) as im:
                    if im.size!=(1280,720):raise ValueError(f'Unexpected source dimensions: {f}')
                    sheet.paste(im.convert('RGB'),(x,y+80))
                h=sha(f);inputs[str(f)]=h;sources[key]=dict(frame=n,path=str(f),sha256=h)
            else:
                draw.rectangle((x,y+80,x+1279,y+799),fill='#18222a');draw.text((x+220,y+350),'信号中断 · 接触后无有效前视观测',font=font,fill='#d6b48d');draw.text((x+220,y+410),f'接触时刻 {impact:.4f} s',font=small,fill='#a9bbc7');sources[key]=None
        sheet.save(frames/f'frame_{n:04d}.png',compress_level=2)
        if n in snapshots:sheet.save(out/f'preview_{n:04d}.png')
        timeline.append(dict(frame=n,presentationTimeSeconds=t,forwardObservationValid=bodyValid,channels=sources))
    (out/'synchronization.json').write_text(json.dumps(timeline,indent=2,ensure_ascii=False),encoding='utf-8')
    video=out/'four_camera_combined.mp4'
    checked([a.ffmpeg,'-y','-v','error','-framerate',fps,'-start_number',1,'-i',frames/'frame_%04d.png','-frames:v',total,*encoder_args(video_codec,17,'medium'),'-pix_fmt','yuv420p','-movflags','+faststart',video],out/'encode.log')
    decoded=checked([a.ffmpeg,'-v','error','-i',video,'-map','0:v:0','-f','null','-','-progress','pipe:1','-nostats'],out/'decode.log');fields=dict(x.split('=',1) for x in decoded.splitlines() if '=' in x)
    if fields.get('progress')!='end' or int(fields.get('frame',0))!=total:raise ValueError('Full video decoding incomplete')
    if abs(float(fields['out_time_us'])/1e6-total/fps)>1/fps:raise ValueError('Video duration mismatch')
    for f,h in inputs.items():
        if sha(Path(f))!=h:raise ValueError('Source image changed while composing')
    sample=min(145,valid);decodedPng=out/'decoded_preview.png'
    checked([a.ffmpeg,'-v','error','-i',video,'-vf',f'select=eq(n\\,{sample-1})','-fps_mode','passthrough','-frames:v','1',decodedPng],out/'preview-decode.log')
    layout=[dict(position=position,channel=row[0],label=row[1],sourceFolder=str(row[2])) for position,row in zip(['top-left','top-right','bottom-left','bottom-right'],panels)]
    # Preserve the exact presentation code with the derived media.
    shutil.copy2(Path(__file__),out/'compose_four_camera.py')
    receipt=dict(status='passed',video=str(video),sha256=sha(video),frames=total,fps=fps,seconds=total/fps,pixels=[2560,1600],nativePanelPixels=[1280,720],decodedFrames=int(fields['frame']),layout=layout,forwardValidFrames=valid,firstContactSeconds=impact,sourceImagesHashed=len(inputs),sourceImagesUnchanged=True,codeSha256=sha(out/'compose_four_camera.py'),sourceCase=str(case),scope='Presentation-only composition from native PNGs; existing camera simulation and sensor data unchanged')
    (out/'verification.json').write_text(json.dumps(receipt,indent=2,ensure_ascii=False),encoding='utf-8')
    (out/'index.html').write_text('''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>四路相机同步</title><style>body{background:#10171d;color:#e5edf2;font:16px/1.6 system-ui;max-width:1600px;margin:24px auto;padding:12px}video,img{width:100%}a{color:#9ddaff}</style><h1>四路相机同步画面</h1><p>上排：外部RGB／外部IR；下排：前相机IR／前相机RGB。21秒、24fps。前相机接触后显示信号中断。</p><video controls src="four_camera_combined.mp4" poster="preview_0145.png"></video><p><a href="verification.json">核验</a> · <a href="synchronization.json">共同时间轴</a> · <a href="compose_four_camera.py">合成代码</a></p></html>''',encoding='utf-8')
    print(json.dumps({k:v for k,v in receipt.items() if k!='layout'},indent=2))
if __name__=='__main__':main()
