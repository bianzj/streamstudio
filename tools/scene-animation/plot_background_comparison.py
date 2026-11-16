"""Make a scientific comparison with the same native spectral-radiance scale."""
import argparse
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager,colors,cm
import numpy as np
from PIL import Image

from verify_background_render import planck


def main():
    p=argparse.ArgumentParser();p.add_argument('case',type=Path);a=p.parse_args();root=a.case.resolve()
    font_manager.fontManager.addfont('C:/Windows/Fonts/msyh.ttc');plt.rcParams['font.family']=font_manager.FontProperties(fname='C:/Windows/Fonts/msyh.ttc').get_name()
    fig,axes=plt.subplots(1,3,figsize=(18,6));fig.subplots_adjust(left=.015,right=.98,bottom=.24,top=.79,wspace=.02)
    for ax,path,label in zip(axes,[root/'noon_final/rear_thermal.png',root/'noon_final/street_thermal.png',root/'evening_final/rear_thermal.png'],['第二天 12:00 · 向阳立面视角','第二天 12:00 · 背阳立面视角','第二天 20:00 · 相同视角']):
        with Image.open(path) as image:ax.imshow(np.asarray(image));ax.set_axis_off();ax.set_title(label,fontsize=13,pad=10)
    low,high=planck(10,280),planck(10,330);bar=fig.add_axes([.29,.15,.42,.025])
    fig.colorbar(cm.ScalarMappable(norm=colors.Normalize(low,high),cmap='gray'),cax=bar,orientation='horizontal').set_label(r'10 μm 辐亮度 / $\mathrm{W\,m^{-2}\,sr^{-1}\,\mu m^{-1}}$',fontsize=11)
    fig.suptitle('HiStream 原生能量平衡背景场 → GPU 红外渲染',fontsize=19,y=.97)
    fig.text(.5,.885,'同一街区网格 · 同一固定显示范围（280–330 K 黑体辐亮度） · 无自动增益',ha='center',fontsize=12)
    fig.text(.5,.025,'合成气象与参考建筑；温度来自原生 FacetEB。灰体图像含简化天空反射；未进行现场或仪器精度验证。',ha='center',fontsize=10,color='#444444')
    fig.savefig(root/'background-comparison.png',dpi=160,facecolor='white');fig.savefig(root/'background-comparison.pdf',facecolor='white');plt.close(fig)


if __name__=='__main__':main()
