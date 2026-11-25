// ==========================================================================
// 文件：faceteb/energy_math.glsl（FacetEB 面元能量平衡的 GPU 数学/物理函数库）
// 功能：被 energy.comp 以 #include 引入（依赖其中的 P、pc、SIGMA）；提供 FP64 初等函数、
//   饱和水汽压与潜热、叶片气孔模型（Collatz / Farquhar）、叶温求解和地面自由对流修正。
//   与 CPU 参考实现 facetrt/energyphysics.h 逐项对应（文件首行原注释：Keep aligned）。
// 主要模块：
//   1. 常量与 FP64 初等函数 dexp/dlog/dpow（GLSL 超越函数只有 float，故自行实现）
//   2. 二次方程稳定求根 quadratic / smallerRoot
//   3. 饱和水汽压 saturation 与潜热通量 latent（及对温度的斜率）
//   4. collatz：Ball–Berry + Collatz C3/C4 光合 → 气孔阻力（vegetationTemperatureMethod = 0）
//   5. farquhar：Farquhar/von Caemmerer 光合 + Ball–Berry → 气孔阻力（method != 0）
//   6. physiology：由叶面水汽条件分派到 collatz/farquhar；thermal：给定气孔阻力的叶温求解
//   7. groundExchange：地面侧在比空气暖时附加自由对流导度
// 数据流：energy.comp 在叶片分支对气孔阻力二分、对叶温调用 thermal、physiology 返回目标气孔阻力。
// 约定：温度 K；气压 pc.pressure hPa；水汽压 pc.vapor hPa；阻力 s m-1（上限 625000，下限 1）；
//   通量 W m-2；CO2 为 ppm；P 为每面元 32 个 float 的参数表（P[p+k] 的含义见 energy.comp 文件头）。
// 注意：分支条件和数值容差刻意与 CPU 一致（含 NaN 传播与无效输入拒绝），改动需同步 CPU 版本。
// ==========================================================================

// Keep aligned with facetrt/energyphysics.h.
// 常量：叶片气孔阻力二分的最大迭代数、ln(rs) 收敛容差、叶温收敛容差（K）。
const int LEAF_STOMATAL_MAX_ITERATIONS=24;
const double LEAF_STOMATAL_LOG_TOLERANCE=0.01lf;
const double LEAF_THERMAL_TOLERANCE_K=0.01lf;
// 模块 1：FP64 初等函数。
// FP64 elementary functions: GLSL's transcendental intrinsics are float-only.
// Range reduction and convergent series retain the CPU solver's double roots.
// 作用：双精度 exp(x)。x = k·ln2 + r（|r| ≤ ln2/2），r 用 16 项泰勒级数，再用 ldexp 乘 2^k。
//   x 超过 709.78 返回 +Inf，低于 −745.13 返回 0，NaN 原样返回。
double dexp(double x) {
    if (isnan(x)) return x;
    if (x > 709.782712893384lf) return double(uintBitsToFloat(0x7f800000u));
    if (x < -745.133219101941lf) return 0.0lf;
    int k = int(floor(x / 0.693147180559945309417lf + 0.5lf));
    double r = x - double(k) * 0.693147180559945309417lf;
    double term = 1.0lf, sum = 1.0lf;
    for (int i = 1; i <= 16; ++i) { term *= r / double(i); sum += term; }
    return ldexp(sum, k);
}
// 作用：双精度 ln(x)。frexp 分解 x = m·2^k，m 调整到 [0.7071, 1.4142)；
//   ln m = 2·atanh((m−1)/(m+1)) 用奇次幂级数（到 z^31），再加 k·ln2。
//   x=0 返回 −Inf，x<0 返回 NaN。
double dlog(double x) {
    if (isnan(x) || isinf(x)) return x;
    if (x <= 0.0lf) return double(uintBitsToFloat(x == 0.0lf ? 0xff800000u : 0x7fc00000u));
    int k; double m = frexp(x, k);
    if (m < 0.7071067811865475244lf) { m *= 2.0lf; --k; }
    double z = (m - 1.0lf) / (m + 1.0lf), zz = z * z, term = z, sum = z;
    for (int i = 3; i <= 31; i += 2) { term *= zz; sum += term / double(i); }
    return 2.0lf * sum + double(k) * 0.693147180559945309417lf;
}
// x^y = exp(y·ln x)，仅适用于 x > 0。
double dpow(double x, double y) { return dexp(y * dlog(x)); }
double fourth(double x) { return x*x*x*x; }
// 作用：稳定求解 a x² + b x + c = 0 的一个实根（避免相减抵消），larger=true 取较大根。
//   判别式为明显负值（超出相对容差 1e-12）时返回 NaN，由调用方判定无效。
double quadratic(double a, double b, double c, bool larger) {
    if (a == 0.0lf) return b != 0.0lf ? -c/b : double(uintBitsToFloat(0x7fc00000u));
    double d=b*b-4.0lf*a*c;
    if (d < -1.e-12lf*(b*b+abs(4.0lf*a*c))) return double(uintBitsToFloat(0x7fc00000u));
    double r=sqrt(max(d,0.0lf));
    if(r==0.0lf) return -b/(2.0lf*a);
    double q=-0.5lf*(b+(b<0.0lf?-r:r));
    return larger==(b<0.0lf)?q/a:c/q;
}
// 作用：饱和水汽压（hPa），Tetens 公式 6.107·10^(7.5·c/(237.3+c))，c 为 °C 并限幅到 ±80。
float saturation(float t) { float c=clamp(t-273.15,-80.0,80.0); return 6.107*pow(10.0,7.5*c/(237.3+c)); }
// 作用：饱和水汽压对温度的导数 des/dT（hPa K-1），即 es·ln10·7.5·237.3/(237.3+c)²。
float saturationSlope(float t) { float c=clamp(t-273.15,-80.0,80.0); return saturation(t)*2.3026*7.5*237.3/pow(237.3+c,2.0); }
// 作用：单个面的潜热通量 LE（W m-2）= ρ·L·q 亏缺 /(rb+rs)。
//   输入：表面温度 t（K）、边界层阻力 rb、气孔/表面阻力 rs（s m-1，合计下限 1）。
//   L = (2.501 − 0.002361·T°C)·1e6 J kg-1；比湿亏缺 = 0.622·max(0, es(T) − e_air)/p。
float latent(float t,float rb,float rs) {
    float L=(2.501-0.002361*(t-273.15))*1.e6;
    float deficit=0.622*max(0.0,saturation(t)-pc.vapor)/max(100.0,pc.pressure);
    return pc.rho*L*deficit/max(1.0,rb+rs);
}
// 作用：dLE/dT（W m-2 K-1）= ρ/(rb+rs)·(−2361·亏缺 + L·0.622·des/dT / p)，下限 0。
float latentSlope(float t,float rb,float rs) {
    float L=(2.501-0.002361*(t-273.15))*1.e6;
    float deficit=0.622*max(0.0,saturation(t)-pc.vapor)/max(100.0,pc.pressure);
    return max(0.0,pc.rho/max(1.0,rb+rs)*(-2361.0*deficit+L*0.622*saturationSlope(t)/max(100.0,pc.pressure)));
}
// 作用：Collatz C3/C4 净同化速率 A = Ag − Rd（umol m-2 s-1）。
//   输入：胞间 CO2、C3/C4 标志、Vcmax、PEP 羧化系数、电子传输 Je、Michaelis 常数、
//   Γ*、C3 输出限制 sink、暗呼吸 rd、是否在光合（否则返回 −rd）。
//   Ag 由 Rubisco 限制 Vc 与光限制 Ve 的非直角双曲线（θ=0.8）再与 Vs（θ=0.98）合成。
double collatzAssimilation(double ci,bool c4,double vcmax,double kpep,double je,
                          double michaelis,double gamma,double sink,double rd,bool photosynthesizing) {
    if(!photosynthesizing) return -rd;
    double vc,ve,vs;
    if(c4) {vc=vcmax; vs=kpep*ci; ve=je/6.0lf;}
    else {vc=michaelis+ci>0.0lf?vcmax*(ci-gamma)/(michaelis+ci):0.0lf;
        vs=sink; ve=ci+2.0lf*gamma>0.0lf?je*(ci-gamma)/(ci+2.0lf*gamma)/5.0lf:0.0lf;}
    double v=quadratic(.8lf,-(vc+ve),vc*ve,vc<0.0lf);
    return quadratic(.98lf,-(v+vs),v*vs,false)-rd;
}
// 作用：Collatz 路径的叶片气孔阻力（s m-1），对应 CPU 的 facetbio::collatz 逻辑。
//   输入：面元号 f、叶温 t（K）、吸收 PAR par、叶面水汽压 vapor（hPa）、冠层相对深度 depth ∈ [−1,0]。
// 步骤：1 读参数并换算单位；2 温度修正各动力学常数；3 求 Je、Γ*、叶面相对湿度；
//   4 g0=0 时用 Ci = Cs(1−1.6/(m·RH)) 直接求；g0>0 时对 Ci 二分迭代（最多 24 次）；
//   5 摩尔导度 → m s-1 → 阻力，限幅 [1, 625000]。
float collatz(uint f,float t,float par,float vapor,float depth) {
    // P 参数表偏移（每面元 32 个 float）：[10] Vcmax25，[11] Ball–Berry 斜率 m，
    //   [12] 截距 g0，[13] Rd/Vcmax，[14] kV（Vcmax 随深度的衰减），[15] 胁迫因子，
    //   [16] 光合类型（>3.5 为 C4），[17] 温度修正开关，[18..22] 温度响应参数。
    uint p=f*32u;
    bool c4=P[p+16u]>3.5;
    double tc=double(t), corr=double(P[p+17u]);
    double vmax=double(P[p+10u])*dexp(double(P[p+14u]*clamp(depth,-1.0,0.0)));
    double ppm=1.e-6lf*double(pc.pressure*1.e-3), cs=double(pc.co2)*ppm;
    double o=c4?0.0lf:209.0lf*1.e-3lf*double(pc.pressure*1.e-3);
    // 温度修正：qt 为 Q10 型指数；th/tl 为高/低温抑制；Kc、Ko、Vcmax、Rd 等随温度调整
    //   （与 SCOPE biochemical.m 相同的常数 2.1、1.2、1.8、0.75 的 Q10）。
    double qt=.1lf*(tc-298.15lf)*corr;
    double th=1.0lf+corr*dexp(double(P[p+19u])*(tc-double(P[p+21u])));
    double tl=1.0lf+corr*dexp(double(P[p+18u])*(double(P[p+20u])-tc));
    double kc=350.e-6lf*dexp(dlog(2.1lf)*qt), ko=450.e-3lf*dexp(dlog(1.2lf)*qt);
    double kpep=vmax/56.0lf*1.e6lf*dexp(dlog(1.8lf)*qt);
    double vcmax=vmax*dexp(dlog(2.1lf)*qt)/(c4?th*tl:th)*double(P[p+15u]);
    double specificity=2600.0lf*dexp(dlog(.75lf)*qt);
    double rd=double(P[p+13u])*vmax*dexp(dlog(1.8lf)*qt)/(1.0lf+dexp(1.3lf*(tc-double(P[p+22u]))));
    double kd=max(.8738lf,.0301lf*(tc-273.15lf)+.0773lf);
    // Je = 0.5·(Kp/(Kf+Kd+Kp))·PAR，Kf=0.05，Kp=4；Γ* = 0.5·O/spfy；
    //   rh = 叶面水汽压/叶温饱和水汽压（限幅 [0,1]）。
    double je=.5lf*4.0lf/(.05lf+kd+4.0lf)*double(max(0.0,par));
    double gamma=.5lf*o/specificity, rh=clamp(double(vapor)/double(saturation(t)),0.0lf,1.0lf);
    double product=double(P[p+11u])*rh, csPpm=double(pc.co2);
    double minCiPpm=(c4?.1lf:.3lf)*csPpm, intercept=double(max(0.0,P[p+12u]));
    double mm=kc*(1.0lf+o/ko), sink=vmax/2.0lf*dexp(dlog(1.8lf)*qt);
    // 无光、无湿度（m·RH=0）或胁迫因子为 0 时不光合，只剩 g0 导度。
    bool photosynthesizing=par>0.0&&product>0.0lf&&P[p+15u]>0.0;
    double molarConductance=intercept;
    // 仅当叶面 CO2 > 0 且在叶面 CO2 下同化为正时才通过 Ball–Berry 求气孔导度；
    //   否则保持只有 g0 的闭合状态。
    if(csPpm>0.0lf&&collatzAssimilation(cs,c4,vcmax,kpep,je,mm,gamma,sink,rd,photosynthesizing)>0.0lf) {
        double ci=product>0.0lf?max(minCiPpm,csPpm*(1.0lf-1.6lf/product)):minCiPpm;
        if(intercept>0.0lf) {
            double lower=minCiPpm,upper=csPpm;
            for(int i=0;i<24;++i) {
                ci=.5lf*(lower+upper);
                double a=collatzAssimilation(ci*ppm,c4,vcmax,kpep,je,mm,gamma,sink,rd,photosynthesizing);
                double gs=max(intercept,intercept+product*a/csPpm);
                double target=max(minCiPpm,csPpm-1.6lf*a/gs);
                if(abs(ci-target)<=.01lf||upper-lower<=.01lf) break;
                if(ci>target) upper=ci; else lower=ci;
            }
        }
        double a=collatzAssimilation(ci*ppm,c4,vcmax,kpep,je,mm,gamma,sink,rd,photosynthesizing);
        molarConductance=max(intercept,intercept+product*a/csPpm);
        molarConductance=max(molarConductance,1.6lf*a/(csPpm-minCiPpm));
    }
    // 摩尔导度（mol m-2 s-1）→ 速度导度（m s-1）：乘 R·T/p；其倒数即气孔阻力。
    double conductance=molarConductance*8.314lf*tc/double(pc.pressure*100.0);
    return conductance>0.0lf?float(clamp(1.0lf/conductance,1.0lf,625000.0lf)):625000.0;
}
// 作用：Arrhenius 温度函数（相对 25°C=298.15 K），activation 单位 kJ mol-1，R=8.31 J mol-1 K-1。
double arrhenius(double activation,double t) { return dexp(activation*(t-298.15lf)/(298.15lf*(8.31lf*t/1000.0lf))); }
// 作用：带高温失活的峰值 Arrhenius 函数（Medlyn 型），entropy 为熵项（kJ mol-1 K-1）。
double peaked(double activation,double deactivation,double entropy,double t) {
    return arrhenius(activation,t)*(1.0lf+dexp((298.15lf*entropy-deactivation)/(8.31lf*298.15lf/1000.0lf)))/(1.0lf+dexp((t*entropy-deactivation)/(8.31lf*t/1000.0lf)));
}
// 作用：x² + b x + c = 0 的较小实根（稳定形式）；判别式明显为负返回 NaN。
double smallerRoot(double b,double c) {
    double disc=b*b-4.0lf*c;
    if(disc < -1.e-12lf*(b*b+abs(4.0lf*c))) return double(uintBitsToFloat(0x7fc00000u));
    double r=sqrt(max(0.0lf,disc)); return b<0.0lf?(b==r?0.0lf:2.0lf*c/(-b+r)):(-b-r)/2.0lf;
}
// 作用：Farquhar（C3）/von Caemmerer（C4）光合 + Ball–Berry 的气孔阻力（s m-1）。
//   输入：叶温 t（K）、吸收 PAR、叶面相对湿度 rh、冠层相对深度。
//   输出：气孔阻力（限幅 [1, 625000]）；任何同化/导度非有限则返回 NaN（由主程序报错）。
//   P[23..26] 在叶片上依次为年均温（熵项）、beta、qLs、sustainedNPQ（光系统参数，待核对）。
float farquhar(uint f,double t,float par,double rh,float depth) {
    uint p=f*32u; bool c4=P[p+16u]>3.5;
    double pressure=double(pc.pressure), pa=pressure*100.0lf, ppm=pa*1.e-11lf, cs=double(pc.co2)*ppm, oxygen=209.0lf*pa*1.e-8lf;
    double vmax=double(P[p+10u])*dexp(double(P[p+14u])*clamp(double(depth),-1.0lf,0.0lf));
    double rd=double(P[p+13u])*vmax*arrhenius(46.39lf,t), specificity=2862.0lf/arrhenius(37.83lf,t);
    double vcmax=vmax*(c4?peaked(67.29lf,144.57lf,.472lf,t):peaked(71.51lf,200.0lf,(-1.07lf*double(P[p+23u])+668.0lf)/1000.0lf,t));
    double jmax=vmax*(c4?(40.0lf/6.0lf)*peaked(77.9lf,191.9lf,.627lf,t):2.68lf*peaked(49.88lf,200.0lf,(-.75lf*double(P[p+23u])+660.0lf)/1000.0lf,t));
    double qt=(t-298.15lf)/10.0lf;
    double kc=(c4?944.0lf*dpow(2.1lf,qt):404.9lf*arrhenius(79.43lf,t))*ppm;
    double ko=(c4?633.0lf*dpow(1.2lf,qt):278.4lf*arrhenius(36.38lf,t))*pa*1.e-8lf;
    double kps=(1.e8lf+3.e7lf)*.88lf/.12lf*double(P[p+25u]);
    double denominator=kps+3.e7lf+1.e8lf+double(P[p+26u])*(3.e7lf+1.e8lf);
    double theta=(kps-1.95e8lf*double(P[p+25u]))/denominator;
    double excitation=double(P[p+24u])*double(par)*kps/denominator, capacity=jmax*double(P[p+25u]), sum=excitation+capacity;
    // 电子传输速率：激发项 excitation 与光合容量 capacity 的非直角双曲线（曲率 theta）。
    double electron=sum>0.0lf?2.0lf*excitation*capacity/(sum+sqrt(max(0.0lf,sum*sum-4.0lf*theta*excitation*capacity))):0.0lf;
    double gamma=.5lf*oxygen/specificity, lower=c4?9.9lf*ppm:gamma;
    double product=double(P[p+11u])*rh*double(P[p+15u]);
    double ci=product>0.0lf?max(lower,cs*(1.0lf-1.6lf/product)):lower, gross=0.0lf;
    // C3：总同化 = (1−Γ*/Ci)·min(Rubisco 限制, 电子传输限制)；
    //   C4：解 von Caemmerer 的联立二次式（Ac、Aj 取较小根，gbs 为鞘层导度）。
    if(par>0.0&&electron>0.0lf&&product>0.0lf&&cs>0.0lf) {
        if(!c4) gross=(1.0lf-gamma/ci)*min(vcmax*ci/(ci+kc*(1.0lf+oxygen/ko)),electron*ci/(4.5lf*ci+10.5lf*gamma));
        else {
            double vpmax=vmax*2.33lf*peaked(70.37lf,117.93lf,.376lf,t), kp=82.0lf*dpow(2.1lf,qt)*ppm;
            double vp=min(vpmax*ci/(ci+kp),80.0lf), gbs=(.0207lf*vmax+.4806lf)*1000.0lf, gam=.5lf/specificity;
            double supply=vp-.5lf*rd+gbs*ci, carbox=vcmax-rd, mm=gbs*kc*(1.0lf+oxygen/ko);
            double ls=.2lf*electron-.5lf*rd+gbs*ci, lc=.2lf*electron;
            double ac=smallerRoot(-(supply+carbox+mm),carbox*supply-gam*vcmax*gbs*oxygen+rd*mm);
            double aj=smallerRoot(-(ls+lc-rd+gbs*gam*oxygen*7.0lf/3.0lf),ls*(lc-rd)-gbs*gam*oxygen*(lc+rd*7.0lf/3.0lf));
            gross=min(ac,aj)+rd;
        }
    }
    // 导度：g0 项 + 光合驱动项 1/r，其中 r 由 1.6·(Cs−Ci)/A 换算（含空气密度/分子量常数）；
    //   只有净同化为正且 Cs>Ci 才加入。
    double net=gross-rd, g=double(P[p+12u])*8.314lf*t/pa;
    if(net>0.0lf&&cs>ci) {double r=.625lf*(cs-ci)/net*1.2047lf/28.96lf*1.e3lf*1.e6lf/pa*1.e5lf; g+=1.0lf/r;}
    // MD12 rejects invalid assimilation; do not turn a NaN root into g0-only exchange.
    if (isnan(gross)||isinf(gross)||isnan(net)||isinf(net)||isnan(ci)||isinf(ci)||isnan(g)||isinf(g)||g<0.0lf)
        return uintBitsToFloat(0x7fc00000u);
    return g>0.0lf?float(clamp(1.0lf/g,1.0lf,625000.0lf)):625000.0;
}
// 作用：给定叶温 t 与试探气孔阻力 rs，求“生理所要求的”气孔阻力（目标值）。
//   叶面水汽压 = 空气水汽压 + (饱和 − 空气)·rb/(rb+rs)；pc.method==0 用 Collatz，否则 Farquhar。
//   energy.comp 对 ln(rs) 二分使 rs = physiology(rs)。
float physiology(uint f,double t,double rs,float par,float rb,float depth) {
    float sat=saturation(float(t));
    double vapor=double(pc.vapor)+double(max(0.0,sat-pc.vapor))*double(rb)/(double(rb)+rs);
    return pc.method==0u?collatz(f,float(t),par,float(vapor),depth):farquhar(f,t,par,clamp(vapor/double(sat),0.0lf,1.0lf),depth);
}
// 作用：给定气孔阻力 rs，求叶片温度（K）。方程：
//   net − εσ(T⁴ − T0⁴) − g(T − Tair) − LE(T) − storage·(T − Tprev) = 0，T0 = initial 为
//   计算 net 时所用温度。用带区间 [220, 360] K 的牛顿法，牛顿步越界则二分，最多 32 步，
//   步长 ≤ 0.01 K 即收敛。eps 为两面发射率之和（叶片两面各发射），conductance 为显热导度。
double thermal(float initial,double rs,float rb,float net,double eps,float conductance,float storage,float previous) {
    double lower=220.0lf,upper=360.0lf,t=clamp(double(initial),lower,upper);
    for(int i=0;i<32;++i) {
        double residual=double(net)-eps*double(SIGMA)*(fourth(t)-fourth(double(initial)))-double(conductance)*(t-double(pc.air))-
            double(latent(float(t),rb,float(rs)))-double(storage)*(t-double(previous));
        double response=4.0lf*eps*double(SIGMA)*t*t*t+double(conductance)+double(storage)+double(latentSlope(float(t),rb,float(rs)));
        double correction=residual/max(1.0lf,response);
        if(abs(correction)<=LEAF_THERMAL_TOLERANCE_K)
            return clamp(t+correction,lower,upper);
        if(upper-lower<=LEAF_THERMAL_TOLERANCE_K) break;
        if(residual>0.0lf) lower=t; else upper=t;
        double trial=t+residual/max(1.0lf,response);
        t=trial>lower&&trial<upper?trial:.5lf*(lower+upper);
    }
    return t;
}
// Ground free convection, facetphysics::groundExchange (energyphysics.h) and
// VoxelEB fluid_coupling.glsl: g_free = 0.0025 x (x^2 + 1)^(-1/3) m/s for a
// side warmer than the air; mode 1 adds it in parallel to 1/r, mode 2 (MOST)
// takes the larger conductance. Returns the effective resistance; `extra` is
// the additional sensible-heat slope [m/s].
float groundFreeConductance(float excess) {
    float x=clamp(excess,0.0,60.0);
    return 0.0025*x*pow(x*x+1.0,-1.0/3.0);
}
// 作用：地面侧比空气暖（excess = Ts − Tair）时附加自由对流导度，返回等效阻力。
//   mode=1（中性）：与 1/r 并联；mode=2（MOST）：取两者较大导度。
//   extra 输出附加的显热导度对温度的斜率（m s-1 K-1，用于牛顿响应）。
float groundExchange(float resistance,float excess,float mode,out float extra) {
    extra=0.0;
    float g=groundFreeConductance(excess);
    if(!(g>0.0)) return resistance;
    if(mode>1.5&&!(g>1.0/resistance)) return resistance;
    float x=excess,s=x*x+1.0;
    if(excess<60.0) extra=x*0.0025*(pow(s,-1.0/3.0)-(2.0/3.0)*x*x*pow(s,-4.0/3.0));
    return mode>1.5?1.0/g:1.0/(1.0/resistance+g);
}
