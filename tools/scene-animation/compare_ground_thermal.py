"""Compare infrared temperature-model revisions with matched camera/timing."""
import argparse,json
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
from package_mesh_sequence import checked
from prepare_thermal_case import require,sha
from video_encoder import pick_encoder, encoder_args, add_argument as add_encoder_argument


def main():
    p=argparse.ArgumentParser();p.add_argument('reference',type=Path);p.add_argument('case',type=Path);p.add_argument('--ffmpeg',type=Path,required=True);add_encoder_argument(p);a=p.parse_args();video_codec=pick_encoder(a.ffmpeg,a.video_encoder);out=a.case/'comparison';out.mkdir(exist_ok=False)
    old=json.loads((a.reference/'result_final/run-manifest.json').read_text());new=json.loads((a.case/'result_final/run-manifest.json').read_text());require(old['timeline']==new['timeline'] and old['cameraObservation']==new['cameraObservation'],'Comparison time/camera differs');count=new['frameCount'];fps=new['fps'];video=out/'old_new_ir.mp4'
    checked([a.ffmpeg,'-y','-hide_banner','-i',a.reference/'video/observed_ir.mp4','-i',a.case/'video/observed_ir.mp4','-filter_complex','[0:v]setsar=1[a];[1:v]setsar=1[b];[a][b]hstack=inputs=2[v]','-map','[v]','-frames:v',count,*encoder_args(video_codec,17),'-pix_fmt','yuv420p','-movflags','+faststart',video],out/'encode.log')
    decoded=checked([a.ffmpeg,'-v','error','-i',video,'-map','0:v:0','-f','null','-','-progress','pipe:1','-nostats'],out/'decode.log');fields=dict(x.split('=',1) for x in decoded.splitlines() if '=' in x);require(fields.get('progress')=='end' and int(fields.get('frame',0))==count,'IR comparison decode incomplete')
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',25)
    for folder,name in [('observed_ir','old_new_ir_observed.png'),('fixed_frames','old_new_ir_fixed.png')]:
        im=Image.new('RGB',(2560,768),'#10171d');d=ImageDraw.Draw(im)
        for i,root,label in [(0,a.reference,'旧版：背景地温未计入附加树木'),(1,a.case,'新版：局地地表能量平衡与枝叶遮挡')]:
            with Image.open(root/'result_final'/folder/'frame_0001.png') as frame:im.paste(frame.convert('RGB'),(i*1280,48))
            d.text((i*1280+18,10),label,font=font,fill='white')
        im.save(out/name)
    receipt=dict(status='passed',frames=count,seconds=count/fps,path=str(video.resolve()),sha256=sha(video),scope='Old vs new also changes the ground thermal model; only paired local field delta isolates canopy effects. Camera/rigid/flow identical; display clipping retained.');(out/'verification.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
