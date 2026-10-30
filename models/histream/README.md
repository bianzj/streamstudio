# HiStream
实现局部区域（1米到50公里）场景的遥感观测模拟

现有模块：
1）rayrt： 面元的光线追踪；
2）voxelrt: 体元的光线追踪；
3）voxeleb: 体元的能量平衡；

已有的工作：
1）voxeleb-huailai:怀来站的热红外遥感模拟；


接下来的工作：
1）rayrt-spectral：面元的高光谱模拟；
2）voxelrt—hex：异质性体元的模拟；
3）voxeleb-beijing: 北京市/奥运区的模拟
4）voxeleb-uav: 大量的无人机模拟；

待进行的优化调整：
1）maxStep 没有很好定义，建议改小，改成8或者4；
2）多次散射方向，加密，现在太系数，改成64；
3）考虑到现在的方向太稀疏，建议加一个随机角度偏置系数；


参考文献：

Tengyuan Fan, Zunjian Bian, Jean-Louis Roujean, Huaguo Huang, Hua Li, Junhua Bai, Biao Cao, Yongming Du, Qing Xiao, Qinhuo Liu,
STREAM: a system for tracing radiative transfer and energy balance in heterogeneous surfaces,
International Journal of Applied Earth Observation and Geoinformation,
Volume 142,
2025,
104763,
ISSN 1569-8432,
https://doi.org/10.1016/j.jag.2025.104763.

(https://www.sciencedirect.com/science/article/pii/S1569843225004108)

Abstract: Land Surface Temperature (LST) is a critical climate variable that influences various surface processes such as water and carbon cycles. Observations from thermal infrared (TIR) remote sensing can only reflect a short period thermal information, although it provides an effective method for global and local LST products. Existing radiative transfer models can provide temporal variability of LST by combining energy balance process, but fail for heterogeneous surfaces over large-scale areas (> tens of meters) due to the challenges of computational complexity and scene reconstruction. To address this problem, this study introduces a system for tracing radiative transfer and energy balance in heterogeneous surfaces, hereafter called STREAM, in which the three-dimensional structural heterogeneity is obtained through LiDAR data and a voxel-based radiosity method is proposed for radiative transfer process. The temperature of each voxel is obtained by iterating the radiative transfer and energy balance processes, and the parallel technique on graphics processing units is adopted for high performance. The proposed model is validated using multi-temporal and multi-angle measurements from an unmanned aerial vehicle (UAV) system, and also evaluated by using one- and three-dimensional models. Results show that: 1) the temporal variability of LST can be modeled by using the proposed model, achieving a mean maximum absolute error (MAE) of 2.04 K and root mean square error (RMSE) of 2.6 K in a pixel-by-pixel validation. The proposed model performed better for LST anisotropies with RMSE of approximately 0.48 K than a one-dimensional model with RMSE of approximately 0.76 K; 2) Based on the inter-comparison with other models, STREAM demonstrates a good consistency with the one-dimensional model with a mean RMSE of less than 0.5 K. The difference in LST results between the voxel-based and facet-based three-dimensional models is less than 1.35 K, but the running time of the former (∼2s) is significantly lower than the latter (∼99 s). This novel approach provides an efficient and accurate framework for simulating LST temporal characteristics for large-scale scenarios on TIR remote sensing.

Keywords: TIR remote sensing; Radiosity; Energy balance; Unmanned aerial vehicle; LiDAR
