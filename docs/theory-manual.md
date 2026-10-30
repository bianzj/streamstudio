# Stream Studio 模型理论手册

软件版本：2.0.0
工程格式：`schemaVersion = 6`
更新日期：2026-10-30

## 1. 模型定位

Stream Studio 是面向三维地表场景的光学—热红外辐射传输与能量平衡正向模型。模型把几何结构、光谱属性、组分温度、太阳—天空条件、观测方向和大气状态放入同一计算链，输出传感器可观测的辐亮度、反射率或亮度温度。

当前提供五种计算模式：

| 模式 | 空间表达 | 核心求解 | 主要结果 |
|---|---|---|---|
| FacetRT | 三角面元 | 可见性栅格与辐射度迭代 | 光学/热红外图像 |
| VoxelRT | 体元 | 沿线消光与多次散射 | 光学/热红外图像 |
| FacetEB | 三角面元 | FacetRT 与能量平衡耦合 | 面元温度及时间序列图像 |
| VoxelEB（`eVoxelEB`） | 体元 | VoxelRT 与能量平衡耦合 | 体元温度、GPP/NPP 及时间序列图像 |
| RayTracing | 三角面元 | 后向光线追踪 | 独立成像对照结果 |

这些模式共享场景、材质、气象、传感器和输出定义，但数值离散不同，因此结果不应被理解为逐像元完全相同。Facet 模式保留表面几何，Voxel 模式把结构等效到有限体积中；两者的差异本身可用于评估尺度效应与结构简化误差。

当前新建工程默认使用 `eVoxelEB`，界面简写为“体元能量平衡 / VoxelEB”；默认土壤温度方法为 `soilTemperatureMethod = 2`，即温度廓线传导模型。工程配置以 `schemaVersion = 6` 保存。

Stream Studio 1.1 的重点工作是异质性体元：在不把原始三角面长期保留到每次射线查询中的前提下，把亚体元密度、三轴方向遮挡和主要类别混合信息压缩为紧凑体元描述符。它连接了“真实 OBJ 几何”与“高效 VoxelRT/VoxelEB”两种尺度，是森林、作物和城市混合场景保持结构方向性的核心途径。

本文用以下标识区分内容状态。理论公式沿用原论文的公式、符号和编号；为适应 Markdown，仅调整排版，不改变公式含义。没有直接来源于论文的工程公式明确标为“当前实现式”。

- **当前实现**：已存在于本项目代码并可由界面配置；
- **理论基础**：用于解释当前公式和参数的物理意义；
- **扩展方向**：论文中已有方法，但尚未作为当前工程功能提供。

## 2. 场景表达与坐标体系

### 2.1 坐标与计算域

用户侧与当前计算引擎统一采用右手坐标：`+X` 指向北、`-X` 指向南，`+Y` 垂直向上，`+Z` 指向东、`-Z` 指向西。太阳与视场方位角以北为 `0°`、东为 `90°`。平行投影以地理北向作为影像上方；中心投影在倾斜观测时将世界竖直方向投影到像平面作为相机上方，使地平线保持水平，在正下视退化位置再使用地理北向。计算域由南北长度 `L_NS`、最大高度 `H` 和东西长度 `L_EW` 定义。

Facet 模式直接使用三角形位置、法向、面积和材质编号。Voxel 模式将计算域划分为：

```text
Nx = ceil(L_NS / d)
Ny = ceil(H / d)
Nz = ceil(L_EW / d)
```

其中 `d` 为体元大小。体元越小，几何细节越高，但体元数近似按 `d^-3` 增长。输出图像分辨率只控制传感器采样，不等于体元分辨率，也不等于 FacetRT 内部可见性栅格。

局部计算域采用轴对齐包围盒及 X/Y/Z 偏移表达。显示场景与计算场景分离：Three.js 仍显示原始完整背景，而运行时先做对象包围盒判定；完全域外对象跳过，跨界对象裁切到域内。对于单实例 OBJ，三角面在体元化前进行几何裁切，因此跨边界对象的域外部分既不生成体元，也不参与后续辐射和能量求解。局部坐标由原世界坐标减去计算域原点得到，材质与物理属性保持不变。

### 2.2 地物属性分层

几何对象通过四类属性进入求解：

- 光谱属性：波长相关的反射率 `rho_lambda`、透射率 `tau_lambda` 和吸收率；
- 温度属性：阳面、阴面或时间更新后的物理温度；
- 结构属性：刚体、混沌介质、火焰、雾等结构类型及密度参数；
- 物化属性：植被生理、土壤热物理、水体混合层等参数。

对任意非发光材料，能量约束为：

```text
alpha_lambda = 1 - rho_lambda - tau_lambda
0 <= rho_lambda, tau_lambda, alpha_lambda <= 1
```

热平衡条件下依据 Kirchhoff 定律，可取方向—波长发射率等于同方向—波长吸收率。对于不透明表面 `tau_lambda = 0`，因此 `epsilon_lambda = 1 - rho_lambda`。

土壤热红外的 `rho_lambda` 随体积含水量变化：孔隙被水填充后表面光学上更平滑，反射率下降、发射率升高。BSM 光谱材质因此用干端和湿端两个 TIR 反射率插值，权重与 BSM 反射率共用同一含水量几何：

```text
SMC   = clamp(SMC, 0, 1)                       # 体积分数；>1 按百分比换算
mu    = max(SMC - 0.05, 0) / 0.25              # 与 BSM 相同的干端阈值与尺度
w     = 1 - exp(-mu)                           # 0 为干端，趋近 1 为饱和
rho_TIR = rho_dry + (rho_wet - rho_dry) * w
epsilon_TIR = 1 - rho_TIR - tau_TIR
```

`rho_dry` 即材质面板的“TIR 反射率”，`rho_wet` 的缺省值是干端发射率 `+0.03`（上限 0.995），可用“湿端 TIR 反射率”覆盖；湿度只降低 TIR 反射率，所以发射率随含水量单调不减。遥感影像引擎用同一组常数，把地类半球发射率从干端值插值到饱和值（`energy.soilEmissivityWet` 可指定饱和发射率）。叶片、混凝土和导入波谱等非 BSM 材质不随含水量变化。

### 2.3 配置与运行数据链

当前正式运行链为：

```text
GUI -> project.json -> Node 本地服务 -> histream.exe
```

`project.json` 是唯一工程配置和运行入口，界面、服务与引擎围绕同一组字段传递参数。XML 仅保留在少量旧版本兼容代码中，不参与当前正式运行链，也不会在运行时作为中间配置生成。中英文切换只改变界面显示名称，不改变 JSON 字段、计算模式枚举或数值语义。

保存与另存为都先通过 schema 1–6 的兼容校验；另存为改变工程根路径并重新计算相对路径。重置属于运行期控制：结束相关进程和释放运行状态，不改变 `project.json` 的科学参数。

### 对应理论论文

- 卞尊健等（2021），光学遥感三维计算机模拟模型综述与应用框架 [R4]。
- Fan et al.（2025），STREAM 的统一场景、辐射传输与能量平衡框架 [R7]。

## 3. 光学与热红外共同辐射方程

### 3.1 光学波段

太阳短波到达场景后分为直射和天空漫射。传感器方向上的离地辐亮度可概括为：

```text
L_surface(lambda, omega_v)
  = L_direct + L_diffuse + L_multiple
```

其中直射项受太阳可见性、入射角和地物方向反射控制；漫射项来自天空半球；多次散射项来自其他面元或体元。模型不是仅做阴影着色，而是在有限追踪深度或辐射度迭代内传递波段能量。

若输出反射率，可用给定太阳—天空辐照度对离地辐亮度归一化。传感器波段值应严格写成波段响应函数 `S_b(lambda)` 下的积分：

```text
L_b = integral[L_lambda S_b(lambda) d lambda]
      / integral[S_b(lambda) d lambda]
```

当前自定义波长在缺少完整响应函数时按离散中心波长计算，不应把单波长值误认为真实宽波段响应的严格积分。

### 3.2 热红外波段

温度为 `T` 的黑体单色辐亮度由 Planck 定律给出：

$$
B_\lambda(T)=\frac{2hc^2}{\lambda^5}
\left[\exp\left(\frac{hc}{\lambda kT}\right)-1\right]^{-1}.
$$

对于异质冠层，Bian et al.（2025）给出的冠层顶部热红外辐亮度原式为：

$$
L(\theta_s,\theta_v,\phi)
=\sum_j\left[\varepsilon_j f_j(\theta_s,\theta_v,\phi)
+\varepsilon_{m,j}\right]B_\lambda(T_j)+r_cL_a^\downarrow .
\tag{R10-1}
$$

其中，$f_j$ 是传感器方向上组分 $j$ 的可见比例，$\varepsilon_{m,j}$ 是多次散射形成的附加有效发射率，$r_c$ 是冠层半球—方向反射率，$L_a^\downarrow$ 是大气下行有效辐亮度。该式清楚分开了组分自身发射、多次散射和天空下行辐射反射。

模型先求辐亮度，再通过 Planck 反函数得到亮度温度。亮度温度是传感器辐射量对应的等效温度，不一定等于某个面元或体元的物理温度。一个像元包含多个温度组分时，必须先按辐亮度混合，不能直接线性平均温度。

### 3.3 大气顶层观测

启用大气后，地表到传感器的简化观测方程为：

```text
L_TOA(lambda) = transmittance(lambda) * L_surface(lambda)
                + L_path(lambda)
```

热红外图像的处理顺序是：物理温度转为辐亮度、进行大气传播、再反演为传感器亮度温度。大气透过率和程辐射都随波长、柱状水汽、气溶胶负荷、路径高度和观测角变化。

### 对应理论论文

- Bian et al.（2022），GPU 光线追踪三维辐射传输求解 [R5]。
- Fan et al.（2025），光学—热红外辐射传输与能量平衡一体化 [R7]。
- Bian et al.（2025），热红外方向性辐射传输的物理混合、解析参数化和核驱动框架比较 [R10]。

## 4. FacetRT 面元辐射传输

### 4.1 面元与可见性

FacetRT 将 OBJ、DEM 和参数化几何离散成三角面元。每个面元保存位置、法向、面积、正反面关系和材质属性。太阳直射与面元之间的辐射交换首先依赖几何可见性：

```text
V_ij = 1  面元 i 与 j 互相可见
V_ij = 0  被其他几何遮挡
```

当前实现通过 Vulkan 栅格化建立可见性/阴影记录。重叠几何沿同一栅格像元形成 A-buffer 深度链；程序根据场景复杂度选择 16、32 或 64 个片元层。输出图像分辨率独立设置，内部可见性栅格可根据大场景自动提升。

### 4.2 形状因子与辐射度

TRGM-EB 的面元辐射度原式为：

$$
B_i=E_i+x_i\sum_jF_{i,j}B_j,
\qquad i,j=1,2,\ldots,2n_p .
\tag{R1-1}
$$

其中，$B_i$ 为面元 $i$ 的辐射度，$E_i$ 为源项，$x_i$ 为反射率或透射率，$F_{i,j}$ 为从面元 $j$ 到面元 $i$ 的形状因子，$n_p$ 为双面面元数。面元源项原式为：

$$
E_i=
\begin{cases}
[F_s(i)+F_d(i)]\rho_i+[F_s(i+n_p)+F_d(i+n_p)]\tau_i+F_e(i), & i\le n_p,\\
[F_s(i)+F_d(i)]\rho_i+[F_s(i-n_p)+F_d(i-n_p)]\tau_i+F_e(i), & i>n_p.
\end{cases}
\tag{R1-2}
$$

直射、天空漫射和面元热发射分别为：

$$
F_s(i)=E_{sun}|\mathbf n_i\mathbf s_d|a(i,\theta_d),
\tag{R1-3}
$$

$$
F_d(i)=\sum_{k=1}^{N}I_{atm}(\theta_k)\frac{2\pi}{N}
|\mathbf n_i\mathbf s_k|a(i,\theta_k),
\tag{R1-4}
$$

$$
F_e(i)=\varepsilon_iB_\lambda(T_i).
\tag{R1-5}
$$

$a(i,\theta)$ 是相应方向的可见面积比例。形状因子由几何投影获得，满足互易关系 $A_iF_{i,j}=A_jF_{j,i}$；离散后，面元 $i$ 接收的半球辐照度由所有可见面元辐射度加权求和。

原论文的传感器方向合成式为：

$$
I(v)=
\frac{\sum_{i=1}^{2n_p}\dfrac{B_i}{\pi}
|\mathbf n_i\mathbf s_v|a(i,\theta_v)\operatorname{area}(i)}
{\sum_{i=1}^{2n_p}|\mathbf n_i\mathbf s_v|a(i,\theta_v)
\operatorname{area}(i)}.
\tag{R1-23}
$$

当前 Stream Studio 保持双面反射/透射关系，并用 Vulkan compute shader 的 Jacobi/松弛迭代替代论文中的串行 Gauss–Seidel 求解，在达到残差阈值或最大迭代次数时结束。该过程适合漫反射主导的多次散射；强镜面材料需要专门的 BRDF 路径，不能简单等同于漫反射辐射度。

### 4.3 FacetRT 加速

传统模式逐波段构建源项并求解。加速模式保留每个波段的独立光谱属性和输出，只复用相邻同类型波段的初始辐射度，使迭代更快进入收敛区间。它不是把 500 个光谱简单压成 5 个光谱，也不以中间波段插值代替最终波段求解。

### 对应理论论文

- Bian et al.（2017），行作物场景的辐射度—能量平衡耦合 [R1]。
- Bian et al.（2018），三维场景热红外亮温分布模拟 [R2]。
- Bian et al.（2022），基于 GPU 的三维辐射传输求解 [R5]。
- Fan et al.（2025），STREAM 中面元框架的系统实现 [R7]。

## 5. VoxelRT 体元辐射传输

### 5.1 沿线消光

VoxelRT 将复杂几何转成有限体元。Fan et al.（2025）的体元间透过率原式为：

$$
\tau_{i\rightarrow j}=\tau_i\tau_1\cdots\tau_m
=\prod_{k=0}^{m}\exp(-LAD_k\,G\,pl_k).
\tag{R7-12}
$$

其中，$m$ 为体元 $i$ 与 $j$ 之间经过的体元数，$LAD_k$ 为体元叶面积密度，$G$ 为单位叶面积在传播方向法平面上的平均投影，$pl_k$ 为射线在体元 $k$ 内的路径长度。当前异质性体元对原式扩展为：

$$
\tau_k(\boldsymbol\omega)=
\exp[-\rho_kG(\boldsymbol\omega)CI_k(\boldsymbol\omega)pl_k s_k].
\tag{当前实现式}
$$

这里的 $\rho$ 对应真实结构密度，$CI$ 是方向团聚修正，$s_k$ 是单位尺度换算项。$CI=1$ 且 $\rho=LAD$ 时退化为论文的 Beer–Lambert 原式。

该式给出射线不与内部介质发生相互作用的概率。被截获部分再依据 `rho_lambda`、`tau_lambda` 和 `alpha_lambda` 分配为反射、透射和吸收/热发射。因而 Voxel 方法可理解为沿离散路径估计方向交换概率，但它不是显式存储全部面元对形状因子的矩阵。

### 5.2 直射、漫射与传感器成像

Fan et al.（2025）首先把体元辐射度写为：

$$
B_i=E_i+\chi_i\sum_{j=1}^{2n_p}F_{i,j}B_j,
\qquad i=1,2,\ldots,2n_p,
\tag{R7-1}
$$

$$
E_i=F_{i,s}+F_{i,d}+F_{i,e}.
\tag{R7-2}
$$

直射与天空漫射项为：

$$
F_{i,s}=E_s\cos(\theta_s)\tau_{s\rightarrow i}P_i(\Omega_s),
\tag{R7-3}
$$

$$
F_{i,d}=\frac{1}{N/2}\sum_{k'=1}^{N/2}
E_{sky}\tau_{sky(\Omega_{k'})\rightarrow i}P_i(\Omega_{k'}).
\tag{R7-4}
$$

体元间形状因子为：

$$
F_{i,j}=\frac{\tau_{i\rightarrow j}(1-\tau_j)}{N}.
\tag{R7-11}
$$

体元的等效发射率和热发射源项由光谱不变量理论给出：

$$
F_{i,e}=\varepsilon_{i,voxel}\sigma T_{i,su}^{4}\tau_{s\rightarrow i}
+\varepsilon_{i,voxel}\sigma T_{i,sh}^{4}(1-\tau_{s\rightarrow i}),
\tag{R7-6}
$$

$$
\varepsilon_{i,voxel}=\frac{1-\omega_L}{1-p\omega_L},
\qquad
p=0.88\left(1-e^{-0.7LAI^{0.75}}\right).
\tag{R7-7}
$$

论文进一步给出体元散射项：

$$
\chi_i=\omega_{i,voxel}\Gamma_{i,\Omega_{k'}\rightarrow\Omega_k}
=\omega_{i,voxel}
\left(\rho_i\Gamma^-_{\Omega_{k'}\rightarrow\Omega_k}
+\tau_i\Gamma^+_{\Omega_{k'}\rightarrow\Omega_k}\right),
\tag{R7-8}
$$

$$
\omega_{i,voxel}=\frac{\omega_L-p\omega_L}{1-p\omega_L}.
\tag{R7-9}
$$

当前代码沿相同物理链计算太阳直射、天空漫射、体元间交换和传感器成像；数值实现针对 Vulkan 并行重新组织，不要求逐行复现论文的串行程序结构。

高光谱加速模式一次最多打包 4 个相邻波段，共享相同几何遍历；每个波段仍使用自己的光谱参数并产生独立结果，因此不引入波长插值误差。

### 5.3 均质体元与尺度意义

默认情况下每个体元被看成一种综合均质介质。减小体元可以表达更细的结构，但会提高体元数、射线步数和显存需求。增大体元时，必须由密度、方向结构因子和混合光谱保持亚体元结构的统计效应，否则会出现尺度依赖。

### 5.4 体元代理几何与求解边界

VoxelRT 和 Voxel RT–EB 会先读取 OBJ 三角面，并用这些三角面判断哪些体元被占据。每个有效体元随后创建三个相互正交的中面；每个中面由两个三角形组成，因此一个体元共有 6 个代理三角形，供 GPU 射线相交和路径遍历使用。

这些代理三角形不是完整六面立方体，也不会送入 FacetRT/FacetEB 作为表面面元求解。辐射量、温度和能量状态始终按体元存储并迭代。默认均质体元在完成体元化后不再依赖原始 OBJ 三角面；只有启用异质性体元时，才额外读取原始三角面，一次性统计叶面积密度、投影率和聚集指数等亚体元结构参数。

### 5.5 周期边界与天空边界

周期邻域通过边界穿越实现，而不是复制几何。计算域增加四个水平边界面；射线命中 X 或 Z 边界后，将命中点平移一个域宽到对侧，并以相同方向继续最近交查询。设域宽为 `Lx`、`Lz`，则例如穿过 `x = Lx` 后执行：

```text
x_new = x_hit - Lx + epsilon
depth_new = depth - 1
periodic_new = periodic - 1
```

其中 `epsilon` 防止射线立即再次命中同一边界。每次穿越计作一次路径深度，不把原场景查询结果与多套复制邻域叠加。`periodicTraversalCount` 限制为 0–20，0 表示开放边界；由于穿越后仍需重新遍历场景，计算量会随有效穿越次数增加，但显存不应按邻域副本数成倍增长。

天空边界有两种表达：默认 cosine 半球分布，以及可选典型天空盒。前者以天顶角余弦权重描述各向异性漫射天空，后者把天空与云信息作为边界辐射纹理采样。天空盒改变进入域内的方向辐射，不创建额外实体，也不改变局部雾的体消光。

### 对应理论论文

- 卞尊健等（2021），三维光学遥感模型中的体元、面元和光线追踪方法 [R4]。
- Bian et al.（2022），GPU Ray Tracing 框架及大规模三维场景加速 [R5]。
- Fan et al.（2025），STREAM 的体元辐射传输实现与验证 [R7]。

## 6. 异质性体元（1.1 重点）

异质性体元工作的核心不是增加更多几何副本，而是把一次性的精细几何统计转化为运行期可直接使用的体元参数。完整处理链为：

```text
OBJ 三角面
  → 计算域裁切与有效体元判定
  → 体元内三轴投影面积和联合遮挡掩膜
  → rho、CIx/CIy/CIz 与类别贡献
  → 紧凑结构描述符和最多两个主要类别
  → VoxelRT 消光/散射/热发射
  → VoxelEB 净辐射/能量通量/温度/GPP/NPP
  → 多波段、多角度、逐时间节点输出
```

因此，预处理阶段仍利用面元提供结构真值，正式计算阶段则保持体元求解。该设计避免均质体元把方向空隙率压缩成单一常数，也避免面元模型在大规模重复实例场景中展开全部三角形。

### 6.1 统计目标

异质性体元用于在较粗网格中保留真实亚体元结构，而不是在正式辐射传输阶段继续追踪体元内每一个对象。当前提取阶段统计：

```text
voxel = {rho, CIx, CIy, CIz, mixed spectral, mixed thermal}
```

首次占位只用于确定体元存在性和候选类型，不替代后续密度及遮挡统计。同一个体元中存在建筑、植被或其他多个类别时，各类别共同参与三轴投影遮挡统计。

### 6.2 三轴结构参数

分别沿 X、Y、Z 方向计算投影覆盖率 `C_x`、`C_y`、`C_z`。对方向 `q`：

```text
CI_q = -ln(1 - C_q) / A_projected,q
```

计算任意射线方向时，用方向向量绝对分量组合三个主轴参数：

```text
CI(omega) = weighted(CIx, CIy, CIz; |omega_x|, |omega_y|, |omega_z|)
```

`rho` 由体元内真实结构量统计得到，`CIx/CIy/CIz` 描述相同密度在不同方向上的遮挡效率。二者不能互相替代：密度回答“有多少结构”，CI 回答“结构如何排列并遮挡”。

### 6.3 多类别综合属性

当前实现按各类别在 X、Y、Z 三个方向上的遮挡贡献排序，只保留最重要的两个类别。设保留类别的三轴贡献为 `c_mx`、`c_my`、`c_mz`，综合权重概念上为：

```text
w_m = (c_mx + c_my + c_mz)
      / sum_n(c_nx + c_ny + c_nz)
```

工程中权重量化到 1/1024，以便稳定存储。两个类别的光谱和热属性按贡献加权：

```text
rho_mix(lambda) = sum_m[w_m rho_m(lambda)]
tau_mix(lambda) = sum_m[w_m tau_m(lambda)]
T_mix,state      = sum_m[w_m T_m,state]
```

若类别 A、B 的投影覆盖分别为 0.4 和 0.3，即使总覆盖因重叠而小于 0.7，类别权重仍按各自对三轴遮挡的贡献归一化，而不把重叠误当作新的第三类。完成综合后，RT/EB 阶段只读取这个体元的 `rho`、三轴 CI 和综合光谱/温度，不再追究内部具体对象。

当前限制是最多保留两个主要类别，较弱类别会被舍弃；因此异质性极强且类别贡献接近的粗体元应缩小体元大小并做分辨率敏感性试验。

### 6.4 与 VoxelRT/VoxelEB 的耦合

异质结构描述符中的 `rho` 和三轴 CI 直接进入 VoxelRT/VoxelEB 的方向消光。对单位方向 $\boldsymbol\omega=(\omega_x,\omega_y,\omega_z)$，当前实现先按绝对方向分量归一化组合：

$$
CI(\boldsymbol\omega)=
\frac{|\omega_x|CI_x+|\omega_y|CI_y+|\omega_z|CI_z}
{|\omega_x|+|\omega_y|+|\omega_z|},
$$

再以 $\rho GCI(\boldsymbol\omega)$ 构成体元内光学厚度。太阳方向和传感器方向分别取值，因此同一体元可以同时表现为某一方向空隙较多、另一方向遮挡较强。该项参与太阳可见性、天空入射、体元间交换和成像路径，不只是 Three.js 的显示颜色。

多类别体元的光谱和热状态按遮挡贡献综合后进入相同求解链。VoxelEB 由吸收短波、下行与邻近长波得到净辐射，再计算显热、潜热和储热/导热；植被类别进一步输出 GPP/NPP。异质性体元因而会通过辐射吸收和阳照比例间接改变能量平衡，而不是额外增加一套独立温度模型。

### 6.5 计算代价与验证

开启异质性体元会增加 OBJ 读取、体元内三轴投影和重叠掩膜合并的预处理时间；结构提取完成后，原始内部面元不在每次射线查询中重复展开。单体元核心结构描述符 `CIx/CIy/CIz/rho` 为 4 个浮点数，运行期主要增加方向 CI 组合和类别属性读取，计算量通常远小于完整面元展开，但仍高于关闭异质性的均质体元模式。

建议使用三组对照验证：一是同一体元大小下比较异质性开/关，检查冠层透过率、阴影比例、反射率和亮温方向性；二是逐步减小体元，检查异质体元结果是否趋于稳定；三是在可承受的小场景中与 FacetRT/FacetEB 比较。日志中的有效体元数、平均密度和提取耗时用于识别异常体元化；若密度极端、方向差异不合理或三个以上类别贡献接近，应检查 OBJ 重叠、法向、尺度、填充阈值和体元大小。

### 对应理论论文

- Li et al.（2025），利用异质结构改善 UAV 热红外地表温度反演 [R9]。
- Fan et al.（2025），三维场景体元结构和热辐射过程 [R7]。

## 7. 能量平衡与温度演化

### 7.1 基本守恒关系

FacetEB 与 VoxelEB 在每个气象时间节点求解地表能量守恒。TRGM-EB 对阳面和阴面的原式分别为：

$$
R_{n,s}-H_s-\lambda E_s-G_s=0,
\tag{R1-9}
$$

$$
R_{n,h}-H_h-\lambda E_h-G_h=0.
\tag{R1-10}
$$

其中 $R_n$ 为净辐射，$H$ 为显热通量，$\lambda E$ 为潜热通量，$G$ 为土壤热通量，下标 $s$、$h$ 分别表示阳面和阴面。水体在当前实现中以混合层储热 $Q_w$ 替代土壤热通量。

论文给出的面元阳面、阴面净辐射原式为：

$$
R_{n,s}(i)=\sum_\lambda
\left[\frac{F_s(i,\lambda)}{a(i,\theta_d)}+F_d(i,\lambda)
+\sum_jF_{i,j}B_j(\lambda)-F_e(i,\lambda)\right]
[1-\rho(i,\lambda)-\tau(i,\lambda)],
\tag{R1-6}
$$

$$
R_{n,h}(i)=\sum_\lambda
\left[F_d(i,\lambda)+\sum_jF_{i,j}B_j(\lambda)-F_e(i,\lambda)\right]
[1-\rho(i,\lambda)-\tau(i,\lambda)].
\tag{R1-7}
$$

宽波段热发射原式为：

$$
F_e(i)=\varepsilon_i\sigma T_i^4,
\qquad \sigma=5.6704\times10^{-8}\ \mathrm{W\,m^{-2}\,K^{-4}}.
\tag{R1-8}
$$

长波上行辐射随温度约按 `epsilon sigma T^4` 变化，使 RT 与 EB 形成非线性耦合。

### 7.2 显热与潜热

TRGM-EB 的显热和潜热原式为：

$$
H=\rho_a c_p\frac{T_s-T_a}{r_a},
\tag{R1-11}
$$

$$
\lambda E=\gamma\frac{q_s(T_s)-q_a}{r_a+r_s}.
\tag{R1-12}
$$

其中 $\rho_a$ 为空气密度，$c_p$ 为空气比热，$T_s$、$T_a$ 分别为表面与空气温度，$q_s$、$q_a$ 分别为表面饱和比湿与空气比湿，$r_a$、$r_s$ 分别为空气动力阻力和表面/气孔阻力，$\gamma$ 为水的汽化潜热。风速越大或空气动力阻力越小，表面与空气的显热交换越强。

论文的土壤表面阻力原式为：

$$
r_s=1.439\times10^5(\theta_{sat}-\theta_{0-5})^{3.14}.
\tag{R1-14}
$$

空气动力阻力由各高度区间串联：

$$
r_a^i=r_a^I+r_a^R+r_a^c+r_w^i+r_b^i.
\tag{R1-15}
$$

当前植被可选择 Ball–Berry 或 Farquhar 机制求取光合—气孔响应；土壤蒸发同时受含水量和表面阻力限制。

这意味着光照土壤不一定必然比植被热。若土壤导热/储热强、反照率高或蒸发旺盛，而植被蒸腾受限，土壤模拟温度可能更低。比较 FacetEB 与 VoxelEB 时，应首先对齐净辐射、空气动力阻力、土壤热通量、含水量和阳/阴面比例，而不是只比较最终温度。

### 7.3 土壤、水体与植被温度

土壤支持三类时间处理：瞬时能量平衡、热惯性动态和温度廓线传导。温度廓线传导模式中，每个土壤体元对应一条实际的一维土柱；阳照和阴影状态只用于求解表面能量平衡，随后按当前阳照比例合成实际表面温度作为土柱上边界。0、0.02、0.04、0.10、0.20、0.40、0.60 和 1.00 m 为离散节点，最深的 1.00 m 节点使用材料参数 `Tsoil` 作为固定温度边界，因此短时阳影差异应随深度迅速衰减，不能完整延伸至 1 m。

TRGM-EB 的 force-restore 温度更新原式为：

$$
T_s(t+\Delta t)-T_s(t)
=\frac{\sqrt{2\omega}}{\Gamma}\Delta t\,G(t)
-\omega\Delta t[T_s(t)-\overline{T_s}].
\tag{R1-13}
$$

其中 $\omega$ 为日周期角频率，$\Gamma$ 为土壤热惯量，$\overline{T_s}$ 为年平均温度。热惯性或传导方案会把当前能量输入的一部分存入下层，使表面温度响应具有滞后。

水体使用当前工程的混合层温度表达：

$$
\rho_wc_{p,w}h_{mix}\frac{dT_w}{dt}=R_n-H-\lambda E.
\tag{当前实现式}
$$

混合层越深，等效热容量越大，日间升温和夜间降温越慢。当前模型是简化的一维储热表达，不计算三维水动力、分层和水平输运。

植被温度通过叶片净辐射、显热和蒸腾耦合更新。光合—气孔方案的作用是把辐射、二氧化碳交换和水分损失连接起来，而不是简单指定一个固定叶温。

### 7.4 植被碳同化与 GPP/NPP

VoxelEB 在植被生化—气孔耦合中得到叶片总光合速率 `Ag` 与暗呼吸 `Rd`。当前过程输出定义为：

```text
GPP = max(0, Ag)
NPP_leaf ~= Ag - Rd
```

单位均为 `μmol CO₂ m⁻²叶面积 s⁻¹`。阳面与阴面分别计算后，按照体元内当前受光比例加权合并；非植被体元写 0。C4 Farquhar 分支同样按 `Ag = A + Rd` 恢复总光合，再由上式输出，因此白天 GPP 一般为正，夜间接近 0，而 NPP 可因暗呼吸为负。

这里的 `NPP_leaf` 是当前叶片尺度净同化近似，并未扣除根、茎、维持和生长等全部自养呼吸，也未从瞬时叶面积尺度积分到日、月或单位地表面积。若需生态系统碳收支，应先按叶面积密度和时间积分转换尺度，再显式加入非叶器官呼吸；不能把当前瞬时 `npp` 直接解释为严格生态学 NPP。

### 7.5 RT–EB 迭代

每个时间节点的概念流程为：

```text
气象与太阳位置
  -> 短波直射/漫射传输
  -> 长波场景交换
  -> 净辐射与湍流/传导通量
  -> 更新温度
  -> 按松弛系数回写
  -> 温差收敛后输出
```

当前工程的温度松弛写为：

$$
T^{n+1}=(1-\eta)T^n+\eta T_{solved}^{n+1}.
\tag{当前实现式}
$$

$\eta$ 过大可能振荡，过小则收敛慢。达到温度阈值或最大耦合迭代数后结束。

### 7.6 逐节点输出与内存生命周期

能量平衡任务按气象节点推进。FacetEB 的结果后处理以单个时间节点为内存边界：读取当前节点临时数据，完成该节点各观测角的 TIFF、辐射过程和能量过程索引后立即释放数组；最后一个观测角处理完成后删除对应临时 `.bin`。实现不再用常驻映射保存全部节点，也不会把第一个节点数据保留到末尾。

因此峰值后处理内存主要受单节点的影像分辨率、波段数、观测角以及面元过程字段影响，而不再随全部时间节点近似线性累加。体元与引擎内部显存仍由场景体元数、采样数和过程输出共同决定，逐节点释放不能消除过细体元或超高分辨率本身的资源需求。

### 对应理论论文

- Bian et al.（2017），行作物辐射度与能量平衡时间耦合 [R1]。
- Bian et al.（2018），组分温度分布与亮温模拟 [R2]。
- Bian et al.（2020），细尺度热红外方向各向异性及温度结构 [R3]。
- Fan et al.（2024），地形对地表温度方向性和能量过程的影响 [R6]。
- Fan et al.（2025），STREAM 辐射传输—能量平衡统一系统 [R7]。

## 8. 光谱窗口加速

### 8.1 能量积分与传感器波段的区别

能量平衡需要的是整个短波范围的总吸收能量，而最终遥感图像需要指定传感器波段。因此计算被分为两条用途不同的光谱路径：

```text
短波窗口积分 -> Rn -> 温度
传感器波段逐波长/逐响应计算 -> 最终图像
```

短波净辐射对波长积分：

```text
SW_abs = integral[E_sun(lambda) * A(lambda) d lambda]
```

当前默认窗口宽度为 100 nm。每个窗口分别处理直射和漫射，再累加为总短波净辐射。窗口越窄越能保留太阳光谱和材料吸收的窄带变化，但求解次数更多；窗口越宽速度更快，但不适合跨越明显吸收带。

### 8.2 FacetEB 与 VoxelEB

FacetEB 的 100 nm 设置作用于短波净辐射窗口积分，窗口结果驱动温度；完成能量平衡后，用户选择的 1–6 个或更多传感器波段仍独立成像。VoxelEB 的直射与漫射短波计算均服从相同窗口宽度。两种模式由同一个界面参数控制，以保持研究设置一致。

光谱窗口不等于图像波段宽度，也不意味着把窗口中心结果复制给窗口内所有最终输出波段。

### 对应理论论文

- Bian et al.（2017），多时相短波—长波能量耦合 [R1]。
- Fan et al.（2025），STREAM 的高效 RT–EB 计算框架 [R7]。

## 9. 热红外方向性与背景异质性

### 9.1 方向性来源

同一场景在不同观测方向出现不同亮度温度，主要来自：

- 不同方向可见的阳面、阴面、土壤和植被比例不同；
- 组分物理温度与发射率不同；
- 热红外在结构内部发生遮挡和多次散射；
- 地形坡向改变局部入射角、可见性和温度；
- 大气路径随观测天顶角增长。

方向性研究应优先比较辐亮度或亮度温度相对天底方向的差值，并保证各方向使用同一时刻的组分温度。跨较长时间获取的多角度实测数据，需要先消除温度时间漂移。

Bian et al.（2025）的组分热红外方向辐亮度式直接表明，方向变化通过 $f_j(\theta_s,\theta_v,\phi)$ 和 $\varepsilon_{m,j}$ 进入：

$$
L(\theta_s,\theta_v,\phi)
=\sum_j[\varepsilon_jf_j(\theta_s,\theta_v,\phi)+\varepsilon_{m,j}]
B_\lambda(T_j)+r_cL_a^\downarrow .
\tag{R10-1}
$$

### 9.2 背景 Hapke 角度效应

当前背景可在各向同性结果和 Hapke 型方向响应之间插值：

```text
R_used = (1 - s) R_Lambert + s R_Hapke
E_used = (1 - s) E_isotropic + s E_Hapke
```

`s` 为界面保留的结构强度参数，范围 0–1。光学波段用于调整背景方向反射权重，热红外用于调整方向发射率。内部形状参数固定，使光谱颜色主要仍由材质属性决定。

该简化参数适合表达背景粗糙度造成的角度效应，但不能替代真实微地形、完整 Hapke 参数反演或地表温度阴阳分布。

### 9.3 三类方向性模型的关系

物理三维模型直接表达结构和组分温度，可解释性强但计算量大；解析参数模型将场景压缩为少量结构和温差参数；核驱动模型用经验核拟合方向变化，约束少且反演快。三者没有对所有场景都最优的方法：规则行结构通常更需要显式几何，而均质或稀疏场景可用参数模型快速近似。

论文中物理混合模型的植被可见比例原式为：

$$
f_c(\mathbf r_v)=\lambda\int_Vp_0(x,y,z,\mathbf r_v)
\,u_L\frac{G}{\mu_v}\,dx\,dy\,dz,
\tag{R10-2}
$$

$$
p_0(x,y,z,\mathbf r_v)=p_i(x,y,z,\mathbf r_v)p_b(z,\mathbf r_v).
\tag{R10-3}
$$

其中 $p_i$、$p_b$ 分别是冠层个体内部与个体之间的方向空隙概率。解析参数模型用方向团聚指数修正 Beer–Lambert 定律：

$$
f_s(\mathbf r_v)=b(LAI)=\exp\left(-\frac{G\Omega LAI}{\mu}\right).
\tag{R10-9}
$$

论文采用的核驱动模型原式为：

$$
T(\theta_s,\theta_v,\Delta\phi)
=f_{com}K_{com}(\theta_s,\theta_v,\Delta\phi)
+f_{geo}K_{geo}(\theta_s,\theta_v,\Delta\phi)+f_{iso},
\tag{R10-18}
$$

$$
K_{com}=1-\cos\theta_v,
\tag{R10-19}
$$

$$
K_{geo}=\sec\theta_s'+\sec\theta_v'-O,
\tag{R10-20}
$$

$$
O=\frac{1}{\pi}(t-\sin t\cos t)
(\sec\theta_s'+\sec\theta_v').
\tag{R10-21}
$$

当前 Stream Studio 的 FacetRT/VoxelRT 属于物理三维正向框架。解析参数化与核驱动拟合可作为结果后处理或未来快速代理，但尚未替代当前求解器。

### 对应理论论文

- Bian et al.（2020），细尺度 TIR 发射方向各向异性 [R3]。
- Fan et al.（2024），复杂地形的 LST 方向性 [R6]。
- Lu et al.（2025），方向各向异性的线扩散核函数 [R8]。
- Bian et al.（2025），三类 TIR 方向性框架的精度和适用性比较 [R10]。

## 10. 水体辐射与温度

### 10.1 水面方向反射

当前水体表面使用 Fresnel–Cox–Munk 参数化。Fresnel 项由入射角和水体折射率决定，Cox–Munk 项用微表面坡度分布表示波浪法向概率：

```text
BRDF_water = Fresnel(theta_h, n)
             * SlopeDistribution(sigma_slope)
             * geometric factor
             + diffuse fraction
```

太阳方向、观测方向和微表面法向接近镜面条件时出现太阳耀斑。坡度方差越大，高光区域通常更宽、更弱；方差越小，镜面峰更集中。Three.js 的动态波浪只服务预览，真实辐射图像由水体 BRDF 参数决定，两者当前不共享逐顶点波形。

### 10.2 水温

水温由净辐射、显热、蒸发和混合层储热共同更新。该近似适用于水面遥感信号、耀斑影响和日变化研究，不适用于波浪破碎、三维流场、深水分层或水下辐射传输研究。

### 对应理论论文

- 卞尊健等（2021），三维场景中方向反射与介质表达的综述 [R4]。
- Fan et al.（2025），统一辐射传输与能量平衡建模方法 [R7]。

## 11. 火焰与雾参与介质

### 11.1 简化表达

火焰与局部雾用于 VoxelRT/VoxelEB。用户给定一个三维区域，程序将其体元化为具有消光、吸收、发射和散射特性的参与介质。火焰使用收窄的上升形态，雾填充完整的椭球或立方体包络。局部雾已进入 VoxelRT 和 VoxelEB 的辐射路径；它与 MODTRAN 表达的大尺度均匀雾霾是两层不同的介质。沿路径 `s` 的形式解为：

```text
L_out = L_in exp(-kappa s)
        + B_lambda(T_fire) [1 - exp(-kappa s)]
```

若考虑散射，还应增加入射辐射的体散射源项。当前简化模式的核心用途是生成林下或地表火点在热红外传感器中的辐射表现，并评估其经过冠层遮挡、大气衰减和空间混合后能否被航空或卫星传感器识别。

### 11.2 与真实燃烧的边界

当前火焰形态是可视化与等效介质参数化，不求解燃料热解、化学反应、湍流燃烧和火线蔓延。需要特别区分：用户设置的局部雾已经按参与介质进入最终辐射计算；VoxelEB 流体输送产生的烟雾浓度场目前只用于过程分析，尚未反馈到最终辐射图像。因此该模块适合火点探测敏感性试验，不适合火行为预报。

### 对应理论论文

- 卞尊健等（2021），三维光学遥感模拟中的参与介质和复杂场景建模 [R4]。
- Fan et al.（2025），STREAM 多过程统一模拟框架 [R7]。

## 12. 简化流体耦合

### 12.1 控制方程

流体模块当前只接入 VoxelEB，用于建立场景尺度的风速与气温场。其理论基础是带浮力和多孔阻力的不可压缩流动：

```text
div(u) = 0

du/dt + (u dot grad)u
  = -grad(p)/rho_air + nu laplacian(u)
    + buoyancy - canopy_drag

dTa/dt + u dot grad(Ta)
  = kappa_T laplacian(Ta) + surface_heat_source
```

当前流动数值实现采用 D3Q19 格子玻尔兹曼方法。分布函数通过 BGK 碰撞和拉取式迁移演化，宏观密度与速度由离散分布恢复；压力由状态关系 `p = cs² rho` 给出，不再求解全局压力 Poisson 方程。整次模拟使用固定的物理速度—格子速度比例，保证气象节点切换时已有分布函数保持同一物理含义；每个节点至少迭代一个来流穿越时间，以近稳态方式更新局地风场。温度与烟雾仍采用半拉格朗日平流和六邻域扩散，并在该节点的物理时间区间内推进。热浮力使用 Guo 力项，树冠作为带 LAD 的多孔阻力区，建筑采用半格反弹固体边界。耦合采用显式交错顺序：流体先读取上一节点更新后的表面温度，再将当前节点的局地风速和空气温度提供给空气动力阻力、显热和潜热计算。

### 12.2 边界条件

来流边界由气象风速和界面风向确定：上游为定值入口，下游为开放出口，侧边为自由滑移，顶部采用开放/弱约束处理。外边界不是四周刚性壁，因此均匀来流不应无条件向场景中心汇聚。

固体内不定义有效空气速度和气温，输出时写为 NoData。请求的 2 m 高度会解析到最近流体体元中心；若流体体元较粗，实际高度可能偏离 2 m。

### 12.3 适用范围

该模块用于把均匀气象来流调整为受建筑、植被和热浮力影响的近地风温场，并反馈显热交换。它不是经过工程验证的 LES/RANS CFD 求解器。对于 5 km 城市等大场景，应先使用 10 m 级流体网格；对于 500 m 植被场景可使用 1 m 级网格，并通过守恒、网格敏感性和实测剖面验证。

### 对应理论论文

- Bian et al.（2017），空气动力阻力与能量平衡耦合 [R1]。
- Fan et al.（2025），场景能量平衡与环境驱动的系统框架 [R7]。

## 13. MODTRAN 大气查找表

### 13.1 查找表维度

当前大气模块以 MODTRAN 预计算结果为查找表，在以下维度插值：

```text
atmosphere profile
PWV
visibility
sensor height
view zenith angle
wavelength
```

大气类型和气溶胶类型为离散选择；PWV、能见度、高度和观测角采用多线性插值，波长采用线性插值。能见度作为气溶胶负荷输入，不与 AOD 同时要求。二者可通过假定气溶胶垂直分布与类型近似转换，但不是无条件一一对应。

### 13.2 LUT 误差来源

查找表误差来自节点间距、MODTRAN 输入廓线、真实气溶胶类型、地表高程、波段响应和插值方式。减少界面参数并不代表这些因素消失，而是由预设廓线和工程默认值吸收。若用于定量反演，应保存 LUT 版本，并用同步水汽、能见度/AOD 和传感器高度验证。

### 对应理论论文

- Li et al.（2025），UAV 热红外数据的大气校正与异质结构温度反演 [R9]。
- Bian et al.（2025），航空多角度 TIR 数据的 MODTRAN 大气校正与方向性评价 [R10]。

## 14. 模拟—观测融合与基础参数校准

### 14.1 可识别参数优先级

只有红边和热红外图像时，信息量不足以同时反演大量结构、光谱、温度和空气动力参数。推荐保留场景几何与类别不变，只校准少数可识别参数：

- 类别层：植被红边反射率尺度、土壤/建筑光谱尺度、热红外发射率、植被和非植被温度偏差；
- 体元层：仅校准结构密度或一个结构尺度系数，不逐体元同时修改全部光谱和温度；
- 全局层：一个大气水汽修正或辐射定标偏差。

观测值可作为带不确定度的相对真值，而不是绝对无误差真值。工程校准可使用带先验约束的目标函数：

```text
J(theta) = sum_b ||W_b [I_sim,b(theta) - I_obs,b]||^2
           + lambda ||theta - theta_prior||^2
```

该式是建议的工程目标函数，不是上述论文的原式。Bian et al.（2025）将方向观测写成：

$$
\mathbf y=\mathbf X\mathbf c+\boldsymbol\epsilon,
\tag{R10-28}
$$

并使用最小二乘估计：

$$
\hat{\mathbf c}=(\mathbf X^T\mathbf X)^{-1}\mathbf X^T\mathbf y.
\tag{R10-30}
$$

采用 $\mathbf X=\mathbf Q\mathbf R$ 的 QR 分解时：

$$
\hat{\mathbf c}=\mathbf R^{-1}(\mathbf R^T)^{-1}
\mathbf X^T\mathbf y.
\tag{R10-31}
$$

第一项拟合观测，第二项约束参数不要无依据地远离初猜。类别参数应先校准，再在残差具有稳定空间结构时增加体元修正。

### 14.2 求导与优化

当前引擎不是端到端自动微分求解器。最稳健的基础方案是有限差分敏感度：

```text
dI/dtheta_i ~= [I(theta_i + delta) - I(theta_i - delta)] / (2 delta)
```

再用少参数 Gauss–Newton、L-BFGS 或网格/贝叶斯优化。可微渲染是在同一计算图中通过自动微分或伴随法直接得到梯度，参数多时更高效，但体元离散、遮挡突变、随机采样和 Vulkan 自定义 Shader 都会使完整可微实现明显复杂。

Li et al.（2025）的异质场景像元辐亮度采样原式为：

$$
L_{ij}=\frac{1}{N_{sample}}\sum_{n=1}^{N_{sample}}
\left[\varepsilon_nB(T_{ij})+(1-\varepsilon_n)R_{multi,n}\right],
\tag{R9-3}
$$

$$
R_{multi,n}=\frac{1}{M_{dir}}\sum_{m=1}^{M_{dir}}
\begin{cases}
\varepsilon_{m,n}B(T_{m,n}), & next=Scene,\\
L_{atm}^{\downarrow}, & next=Sky.
\end{cases}
\tag{R9-4}
$$

像元辐亮度关系写成线性系统：

$$
\mathbf L_{target}=\mathbf W\mathbf R_{pixel}.
\tag{R9-5}
$$

其 Jacobi 温度更新原式为：

$$
B\left(T_{ij}^{(k+1)}\right)=
\frac{1}{N_{sample}}\sum_{n=1}^{N_{sample}}
\frac{L_{ij}-(1-\varepsilon_n)\dfrac{1}{M_{dir}}
\sum_{m=1}^{M_{dir}}\varepsilon_{m,n}B(T_{m,n}^{(k)})}
{\varepsilon_n}.
\tag{R9-6}
$$

这是基于三维热红外邻近效应反演像元温度的论文方法，并非当前 Stream Studio GUI 已提供的反演按钮。它可作为后续观测融合模块的直接理论基础。

深度学习更适合在大量高质量模拟—观测样本存在时充当代理模型、初值估计器或残差校正器，不应在少量观测下直接替代物理模型。建议保留传统求解器作为基准，并把学习方法作为独立加速/校正函数。

### 对应理论论文

- Li et al.（2025），异质结构条件下 UAV LST 反演 [R9]。
- Bian et al.（2025），不同复杂度方向性模型的正向模拟与逆向拟合比较 [R10]。
- Lu et al.（2025），用低维核函数表达方向性并进行参数拟合 [R8]。

## 15. 地表温度与组分温度反演

前 14 章都是正向问题：给定结构、光谱、温度、几何和大气，求出传感器可观测的辐亮度、反射率或亮温。本章说明把同一条链反过来求解地表温度时的数学形式，以及它接入 Stream Studio 与 HiStream 同化链时的约定。

反演在工程上分两类目标：

| 目标 | 未知量 | 典型观测 | 可行性 |
|---|---|---|---|
| LST（地表温度） | 1 个像元等效方向辐射温度 | 单/双热红外波段、单角度 | 分裂窗为基线，成熟可业务化 |
| LSCT（组分温度） | Tss/Tsh/Tls/Tlh 四个组分温度 | 多角度/多波段热红外 + 光学先验 | 单角度单波段欠定，必须加先验或空间/时间约束 |

**实现归属。** 卫星级分裂窗 LST、多角度/窗口 LSCT 与产品链由 LS4T（`C:\work\ls4t`）承担。STREAM 引擎与工作台另外内建了以 STREAM 正向热红外模型为观测算子的 LST / LSCT 影像反演（15.6 节，`inversion` 配置块）；LAI、土壤水分等参数的联合同化仍属**扩展方向**。`tools/stream-inverse/` 是单像元可微原型，不是产品反演器。工程接口、产品字段与已知缺口见 [lst-lsct-inversion.md](lst-lsct-inversion.md)。

### 15.1 反演问题与可识别性

观测算子把控制变量 x 映射为观测 y：

```text
y = H(x) + eps
```

其中 H 是第 3–13 章的正向链在传感器波段、观测角度和像元尺度上的复合，`eps` 包含传感器噪声、代表性误差、大气与结构误差。反演求解

```text
x_hat = argmin_x || y - H(x) ||_R^2 + || x - x_b ||_B^2 + 平滑项
```

三项分别是观测失配、背景（先验）约束和空间/时间平滑。第一项单独使用在观测数小于等于未知量且 H 病态时不可解，后两项决定问题是否适定。

可识别性由观测信息量而非优化器决定，结论有三条：

1. **温度—发射率退化**：单波段热红外只有 1 个观测，无法同时确定温度与发射率。发射率必须外部给定，或使用两个以上热红外波段。
2. **LST 可识别，LSCT 不自动可识别**：方向亮温在给定发射率与大气后唯一确定一个等效温度；四个组分温度需要四个以上彼此独立的观测（多角度、多波段组合）才能分离。
3. **结构只作输入或慢变先验**：LAI、冠层高度、叶倾角分布在热红外上的信息很弱，本轮反演目标不包含它们；其误差进入模型误差协方差，并按产品 QA 标注。

### 15.2 分裂窗地表温度反演（当前实现）

LS4T 的 `SplitWindowModel` 是一套八特征广义分裂窗，AATSR、SLSTR、MSG、MODIS、VIIRS、AHI、ASTER 共用这一核心，传感器模块只做数据映射。

先由两波段亮温与发射率构造辅助量：

```text
eps_mean = (eps1 + eps2) / 2
d_eps    = eps1 - eps2
bt_mean  = (bt1 + bt2) / 2
d_bt     = (bt1 - bt2) / 2
```

八特征向量为：

```text
f1 = 1
f2 = bt_mean
f3 = (1 - eps_mean) / eps_mean * bt_mean
f4 = d_eps / eps_mean^2 * bt_mean
f5 = d_bt
f6 = (1 - eps_mean) / eps_mean * d_bt
f7 = d_eps / eps_mean^2 * d_bt
f8 = (bt1 - bt2)^2

T_s = sum_k c_k(VZA, wv) * f_k
```

系数 `c` 是形状为 `(VZA 节点, 水汽分箱边界, 8)` 的三维数组：VZA 在相邻节点之间线性插值，柱水汽只按分箱选择、不插值。**不做外推**：VZA 允许落在最末节点上，水汽落在最末边界外即判无效。分裂窗系数按传感器与大气条件标定，标定域之外的场景不应使用。

输入有效性与质量标记：

| 条件 | 处理 |
|---|---|
| 亮温或发射率为非有限值，亮温 ≤ 0，发射率不在 `(0, 1]` | 置 NoData，标记 `INPUT_INVALID` |
| 云掩膜为真 | 置 NoData，标记 `CLOUD` |
| VZA 或水汽超出标定域 | 置 NoData，标记 `INPUT_INVALID` |
| 结果超出 220–365 K | 置 NoData，标记 `OUT_OF_RANGE` |

分裂窗只给一个等效方向温度，不能分离组分温度。城市、作物行结构等异质像元得到的是像元等效辐射温度，与任一真实表面温度都不相等。

### 15.3 覆盖率与混合有效发射率（当前实现）

分裂窗需要两个波段的像元有效发射率。当前实现先由 LAI 求方向植被覆盖率，再按土壤—植被—多次散射混合：

```text
f = 1 - exp(-G * LAI / cos(VZA))，G = 0.5
eps = eps_s (1 - f) + eps_v f + F (1 - eps_s) eps_v (1 - f)
eps = min(eps, 0.995)
```

`F` 是多次散射的经验因子，公共核心默认 `F = 0.75`，部分历史案例使用 `F = 0.05`。两者数值差异直接进入 LST，**同一批产品必须记录所用取值，不能用公共默认静默替换生产设置**。`eps_s`、`eps_v` 由地类或外部发射率库给出；土壤发射率随含水量变化的关系与第 2.2 节同一组常数。

该式是发射率的输入构造，不是反演结果：它把 LAI 与发射率库的误差直接传递到 LST，产品 QA 必须标出 LAI 与发射率的来源。

### 15.4 组分温度反演的线性系统（当前实现）

在辐射域记四个组分的黑体辐亮度为 x = [B_λ(Tss), B_λ(Tsh), B_λ(Tls), B_λ(Tlh)]，则每个观测（角度或波段）是它们的线性组合：

```text
y = A x,     A[j, k] = eps_k * P_jk
```

`P_jk` 是第 j 个观测下第 k 个组分占的辐射权重，由 `hom3` 节点辐射度（四种组分比例 + 多次散射）给出。2 组分和 3 组分模式是把两土壤或两叶片按权重合并，公式不变、只是列数减少：

| 组分数 | 组分定义 |
|---|---|
| m = 2 | 土壤（Tss + Tsh 合并）、植被（Tls + Tlh 合并） |
| m = 3 | 光照土壤、阴影土壤、植被（两叶片合并）；或光照土壤、阴影土壤 + 植被、按案例约定 |
| m = 4 | Tss、Tsh、Tls、Tlh 全部保留 |

求解与质量门槛：

```text
加权：A~ = A / sigma，y~ = y / sigma；sigma 为观测辐亮度标准差
OLS：x_hat = argmin || A~ x - y~ ||^2
ridge：x_hat = (A~^T A~ + lambda I)^-1 A~^T y~，lambda = 0 时退化为 OLS
T_k = B_lambda^-1(x_hat_k)
```

- 有效观测数小于组分数 → `INPUT_INVALID | RETRIEVAL_FAILED`；
- `cond(A~) > 1e6` → `ILL_CONDITIONED | RETRIEVAL_FAILED`；该判断先于 ridge，ridge 不救已判奇异的矩阵；
- 任一 `x_k ≤ 0` → `OUT_OF_RANGE | RETRIEVAL_FAILED`；
- 逆 Planck 后任一温度超出 200–380 K → 该像元全部置 NoData 并标记 `OUT_OF_RANGE`。

必须注意：正值与温区检查是**求解后过滤**，不是有界最小二乘。也就是说，返回的解不是“在物理约束下最优”，而是在无界解恰好物理时的结果。`residual_rms` 记录的是未加权的原单位辐射残差，不是目标函数值，也不是 K 尺度的 RMSE。

### 15.5 欠定情形：窗口、先验与约束（当前实现，案例级）

观测数少于组分数时，四个温度不能逐像元独立求解。当前实现提供三类补充信息：

**空间窗口。** 假设窗口内组分辐亮度局部常数，把 s×s 窗口的 N 个角度展平成 s²N 个方程，共用同一组 x：

```text
[y_1; y_2; ...; y_s2N] = [A_1; A_2; ...; A_s2N] x
```

窗口内 A 仍逐像元变化（地表结构与角度不同），因此方程秩由空间变化补足。边缘用不完整窗口，中心观测本身无效时也可由邻域给出结果——这既是优点也是风险：没有最少有效像元比例、中心有效性或空间相似性检查，异质窗口会把两个地块的温度混成一个解。

**先验与正则化。** 形式为 Tikhonov 或高斯 MAP：

```text
Tikhonov：min || A x - y ||^2 + lambda || x - x_b ||^2
Gaussian MAP（以 300 K 为中心、温度先验标准差与相关系数给定、噪声方差由 d B / d T 换算）
  Q = A^T A / sigma^2 + C0^-1
  x_hat = Q^-1 ( A^T y / sigma^2 + C0^-1 mu )
```

这是 MAP/Laplace 风格的近似：给出点估计和（若保留 Q）后验协方差，但不是后验采样，也没有 credible interval。

**物理约束。** 用 SLSQP 求距离加权残差平方和最小，约束写成 g(x) ≥ 0：

```text
max(x) >= max(B(LST))，组分辐亮度包住中心观测黑体辐亮度
min(x) <= min(B(LST))
-d_min <= x_i - x_j <= d_max，相邻组分温差上下限
```

温差约束实际实现为 B_λ(T_mean + d/2) - B_10.8(T_mean - d/2)，是以平均温度 T_mean 为参考的辐射差近似，不是精确的 B^-1(x_i) - B^-1(x_j)；λ ≠ 10.8 μm 时存在系统偏差。包络约束的 `max/min` 使可行域非光滑且可能多分支。历史实现还存在不检查优化 `success/status`、查询后不重算其他分量等问题，使用时必须把收敛状态写入产品。

### 15.6 STREAM 内建温度反演与同化接口

**内建温度反演（当前实现，`models/stream/app/inversion.cpp`）。** 观测算子与正向成像共用同一套权重（`app/thermal_view.cpp`）：

```text
L_j = sum_k w_jk(θv_j, φv_j, θs) * B(λ_j, T_k)
w_jk = 四组分比例_k * 组分发射率_k + hom3 多次散射项_k
y_j = B^-1(λ_j, L_j)                亮温观测
y_j = B^-1(λ_j, L_j / ε_fix,j)      LST 产品观测
```

导数由 Planck 链解析给出：

```text
dy_j/dT_k = w_jk * B'(λ_j, T_k) / (d_j * B'(λ_j, y_j))
```

其中 d_j 对亮温观测取 1、对 LST 产品取 ε_fix,j。

LST 取四组分同温 T，对 1/σ² 加权残差做 Gauss–Newton；发射率误差 σ_ε 按 σ_ε · B(T) / B'(y_j) 并入观测方差，后验 1σ 为信息量的平方根倒数。

LSCT 求 MAP 解：

```text
J(x) = sum_j (y_j - h_j(x))^2 / σ_j^2 + (x - x_b)^T B^-1 (x - x_b)
B = σ_b^2 [ (1 - r) I + r 1 1^T ]
C_post = ( H^T R^-1 H + B^-1 )^-1
DFS = m - tr( C_post B^-1 )
```

背景 x_b 取本次运行的组分温度（RT 输入或 EB 解），m 为可观测组分数（权重和低于 1e-4 的组分保留背景值）。可选 s×s 同地类窗口共享四组分温度以增加方程数。DFS 低于 1 标记为信息量不足；后验组分温度同时用于本次热红外成像，使输出亮温即后验状态的正向图像。由于观测算子与正向模型逐项一致，反演 STREAM 自身模拟的无噪声影像时可回到生成温度。

**联合同化接口（扩展方向）。** 同化的观测算子是 STREAM 的单像元正向链：

```text
H_optical(x) = canopyReflectance(band, LAI, rho_leaf, rho_soil, geometry, diffuse)
H_tir(x)     = sum_k w_k(x, geometry) * Planck_band(T_k)
```

可微性按模块区分（对应 `models/stream/physics/`）：

- **RT 链可直接可微**：四组分比例、`hom3` 节点辐射度、有效发射率、Planck 与亮温都由 `exp/log/sqrt` 与 5×5 线性求解组成，光滑可导；
- **EB 链不要逐迭代求导**：`updateTemperature` 的限幅（±1.5 K、240–380 K）、区间割线与分支判据都是折点，应使用隐式微分或弱约束。

隐式微分：收敛后四组分残差满足 R(T, x) = 0，则

```text
dT/dx = - (dR/dT)^-1 (dR/dx)
```

只需一次 4×4 线性求解。弱约束 4D-Var 则把 Tss/Tsh/Tls/Tlh 也作为控制变量，把能量平衡残差作为惩罚项：

```text
J = 观测项 + 先验项 + lambda_EB || R(T, x) ||^2
```

后验不确定性用 Laplace 近似：

```text
C_post = ( J^T R^-1 J + B^-1 )^-1
```

角色分工：HiStream 提供三维场景与 OSSE 真值样本；STREAM 提供 H(x)；STREAM-NET 只修正正向亮温残差、可作为观测偏差修正或初值，不能当作物理反演器；LS4T 负责把卫星观测落成 LST/LSCT 产品。

单像元原型（`tools/stream-inverse/`，亮温噪声 0.3 K）的数值证据：

| 量 | 结果 |
|---|---|
| autograd 梯度相对误差 | 7e-9 |
| LAI 误差（真值 2.5） | 0.04 |
| 四组分温度均值误差 | 约 1.3 K |
| 单组分温度 RMSE | 约 5.9 K |
| 观测 RMSE | 0.17 K |
| Jacobian 条件数 | 约 5.8e3 |

结论与 15.1 一致：LAI 与等效温度可识别，**单个晴/阴组分温度在单角度多波段亮温下不可独立反演**。需要多角度、多波段热红外、光学 BRF、时间序列或更强先验，这不是优化器问题。

### 15.7 产品、质量与验证

产品分层沿用卫星产品的既有约定：

```text
L1O -> L2LST / L2LSCT -> L3N -> L3C -> L4A
```

L2LST 输出温度场、经纬度、QC 与不确定度；L2LSCT 按组分顺序输出温度、残差、条件数与 QC。规范化（角度/时间/空间/传感器）与降尺度分别手工执行，融合只接受已处于相同网格与归一化基准的 L3N 数据。

质量标记的语义必须与第 15.2、15.4 节一致：`INPUT_INVALID`、`CLOUD`、`OUT_OF_RANGE`、`ILL_CONDITIONED`、`RETRIEVAL_FAILED` 各自表示输入无效、云污染、结果越界、矩阵病态和求解失败，不能合并成一个“坏像元”计数。

**不确定度的现状必须显式声明。** LS4T 的 L2LST `uncertainty` 由调用者给定（默认为 1 K 填满），不是由残差或协方差传播，LS4T 的 LSCT 结果也没有组分后验标准差字段；STREAM 内建反演（15.6 节）则输出 LST 1σ 与 LSCT 后验标准差。用 LS4T 产品做“68% 覆盖率”保守性检验前须先补传播。

建议的精度目标（产品目标，不是当前原型已达标）：

| 产品 | 场景 | 目标 | 验证方式 |
|---|---|---|---|
| LST | 植被/农田/裸土 | RMSE ≤ 1.5 K，bias ≤ 0.5 K | 站点、UAV、跨传感器 |
| LST | 城市/异质像元 | RMSE ≤ 2.0 K | UAV、高分辨率航空 |
| LSCT 晴/阴土壤 | 多角度或强先验 | RMSE ≤ 2.5 K | UAV 多角度、组分温度观测 |
| LSCT 晴/阴叶片 | 多角度或强先验 | RMSE ≤ 3.0 K | UAV/站点、热像仪 |
| 不确定性 | 全部 | 68% 覆盖接近 1σ，晴空 QA 有效率 ≥ 95% | 覆盖率、可靠性图 |

反演验证以第 16.1 节的最低要求为下限，另外必须包含：OSSE（HiStream 样本加噪声后反演，与真值比较）；闭合检查（反演结果回代正向，残差应在噪声量级）；跨传感器一致性（统一发射率、大气与几何后比较）；图级偏差与空间结构（不出现椒盐噪声），不能只报点位 RMSE。

### 对应理论论文

- Fan et al.（2025），STREAM 的正向框架与影像级产品链 [R7]。
- Li et al.（2025），异质结构条件下 UAV LST 反演 [R9]。
- Bian et al.（2025），不同 TIR 方向性框架的精度、约束与场景依赖性 [R10]。

## 16. 数值验证与适用边界

### 16.1 最低验证要求

每次新增场景或算法至少进行：

1. 能量约束：反射、透射和吸收不超出 `[0, 1]`，面元/体元交换不凭空产生能量；
2. 极限场景：黑体、完全反射体、无遮挡平面、单体元和单面元；
3. 分辨率敏感性：逐步减小面元或体元尺度，检查输出是否趋于稳定；
4. 角度对称性：对称场景在对称方向应给出一致结果；
5. 模式交叉验证：同一简单场景比较 FacetRT 与 VoxelRT、FacetEB 与 VoxelEB；
6. 观测验证：统一时间、几何、大气、空间响应和波段响应后计算偏差、RMSE 和角度残差。
7. 碳同化检查：非植被 GPP/NPP 为 0，夜间 GPP 接近 0，日间 GPP 对有效光合辐射具有合理响应；
8. 周期边界检查：对平移周期场景比较 0 次与 1 次穿越，确认没有几何复制导致的能量重复计数。

### 16.2 结果解释边界

- Three.js 只负责场景预览，不代表实际计算精度；
- Three.js 大场景最多抽样显示 5000 个实例，结果文件仍保存全部有效体元；
- 局部计算域仅保留包围盒内几何，跨界对象的域外部分不会参与体元化或求解；
- FacetRT/FacetEB 启动前按“OBJ 多边形三角化数 × 实例数”预估展开规模；超过 `1,000,000` 个三角面将直接阻止运行，应分别切换到 VoxelRT 或 Voxel RT–EB；
- 影像分辨率、波段数、观测角数和过程输出共同决定连续数组大小；极端组合可能触发 `Array buffer allocation failed`，应先缩小输出规模，再评估场景复杂度；
- 图像 2%–98% 拉伸和 colorbar 只影响显示，不改变原始 GeoTIFF；
- 体元综合属性是尺度相关的，改变体元大小后应重新统计；
- 亮度温度不是物理温度的线性平均；
- 简化流体、水体和火焰模块服务遥感正向模拟，不等同于专业 CFD、水动力或燃烧模型；
- 当前 GPP/NPP 为瞬时叶面积尺度输出，其中 NPP 是叶片净同化近似，不等同于严格生态系统 NPP；
- LUT 大气适合覆盖节点内插值，超出节点范围不应外推解释；
- 论文中的解析模型、核模型、自动微分和深度学习代理属于扩展方向，除本手册明确标注外不代表当前已实现。

### 对应理论论文

- 卞尊健等（2021），三维模型比较、验证与典型应用 [R4]。
- Fan et al.（2025），STREAM 系统验证与模式适用性 [R7]。
- Bian et al.（2025），不同 TIR 方向性框架的精度、约束和场景依赖性 [R10]。

## 17. 理论论文

[R1] Bian, Z., et al. (2017). Modeling the Temporal Variability of Thermal Emissions From Row-Planted Scenes Using a Radiosity and Energy Budget Method. *IEEE Transactions on Geoscience and Remote Sensing*. DOI: [10.1109/TGRS.2017.2719098](https://doi.org/10.1109/TGRS.2017.2719098).

[R2] Bian, Z., et al. (2018). Modeling the Distributions of Brightness Temperatures of a Cropland Scene Using the Radiosity and Energy Budget Methods. *Remote Sensing*, 10(5), 736. DOI: [10.3390/rs10050736](https://doi.org/10.3390/rs10050736).

[R3] Bian, Z., et al. (2020). Modeling the Directional Anisotropy of Fine-Scale TIR Emissions Over Tree and Crop Canopies Based on UAV Measurements. *Remote Sensing of Environment*, 252, 112150. DOI: [10.1016/j.rse.2020.112150](https://doi.org/10.1016/j.rse.2020.112150).

[R4] 卞尊健等（2021）. 光学遥感三维计算机模拟模型的研究进展与应用. *遥感学报*. DOI: [10.11834/jrs.20219274](https://doi.org/10.11834/jrs.20219274).

[R5] Bian, Z., et al. (2022). A GPU-Based Solution for Ray Tracing 3-D Radiative Transfer Model of Complex Land Surface Scenes. *IEEE Geoscience and Remote Sensing Letters*. DOI: [10.1109/LGRS.2022.3206312](https://doi.org/10.1109/LGRS.2022.3206312).

[R6] Fan, M., et al. (2024). Modeling the Topographic Effect on Directional Anisotropies of Land Surface Temperature. *Journal of Remote Sensing*. DOI: [10.34133/remotesensing.0226](https://doi.org/10.34133/remotesensing.0226).

[R7] Fan, M., et al. (2025). STREAM: A System for Tracing Radiative Transfer and Energy Balance in Three-Dimensional Land Surface Scenes. *International Journal of Applied Earth Observation and Geoinformation*, 104763. DOI: [10.1016/j.jag.2025.104763](https://doi.org/10.1016/j.jag.2025.104763).

[R8] Lu, Y., et al. (2025). A Line-Spread Kernel Function for Angular Anisotropy of Land Surface Temperature. *Remote Sensing of Environment*, 114887. DOI: [10.1016/j.rse.2025.114887](https://doi.org/10.1016/j.rse.2025.114887).

[R9] Li, H., et al. (2025). Land Surface Temperature Retrieval Method From UAV Images Considering Heterogeneous Structure. *IEEE Transactions on Geoscience and Remote Sensing*. DOI: [10.1109/TGRS.2025.3642823](https://doi.org/10.1109/TGRS.2025.3642823).

[R10] Bian, Z., et al. (2025). Evaluation of Three Modeling Frameworks of Thermal Infrared Radiative Transfer for Directional Anisotropies of Temperatures. *IEEE Transactions on Geoscience and Remote Sensing*, 63, 5001315. DOI: [10.1109/TGRS.2025.3530503](https://doi.org/10.1109/TGRS.2025.3530503).
