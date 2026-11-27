"""HiStream 三维面元能量平衡（FacetEB）的神经代理与可微反演原型。

模块分工：
- params.py     参数空间：可反演的物理参数与已知强迫扰动，及其写入工程/气象文件的方式；
- engine.py     运行 histream.exe 并把 FacetEB 结果压缩成“类别均值观测向量”；
- surrogate.py  MLP 集成代理：训练、评估、导出 ONNX；
- inversion.py  基于代理的可微 MAP 反演、Laplace 后验、引擎校正迭代、有限差分梯度核对。

代理只是加速与梯度来源，**不替代**已验证的求解器：反演结果总要回到引擎复核。
"""
