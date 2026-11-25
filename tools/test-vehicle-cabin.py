# ==========================================================================
# 文件：test-vehicle-cabin.py
# 功能：FacetEB 车辆座舱热平衡（docs/vehicle-thermal.md 第 3、10 节）的独立单元式检查。
#   这里用纯 Python（只依赖 math）重写 facetrtcore.cpp 的离散隐式方程，与解析解/能量闭合对照；
#   脚本顺序执行，失败则退出码 1（用法见原有说明：python -I tools/test-vehicle-cabin.py）。
# 主要模块：
#   1. step/run：两节点模型（座舱空气 Ta + 内饰集总质量 Ti）的后向欧拉步，含壁板线性响应
#      q(Ta) = q0 - K (Ta - Ta0) 与比例控制空调（带宽 band，功率上限 P）；
#      在不同步长 dt 与换气次数 ACH 下迭代到稳态，与解析稳态比较，并检查每步能量闭合
#   2. 空调检查：稳态落在比例带内，或过载时受功率上限约束
#   3. gauss/parts_step：第二版部件模型（空气 + 4 个内饰节点，可选发动机块与发动机舱）
#      的联立线性系统，部分选主元高斯消元，与 facetrtcore.cpp 的做法一致
#   4. 检查 1-3：稳态（φ = 0 有闭式解）、瞬态逐步闭合（60/600/3600 s）、
#      熄火后发动机块/舱余热衰减与 2x2 两指数解析解的对比
# 约定：温度 K，功率 W，热容 J/K，换热 W/K，Gv = Ca·ACH/3600（通风导热，W/K）；
#   Qtrans 为透射太阳功率，f 为进入内饰的吸收份额（f_abs），Qocc 为乘员代谢热；
#   phi 为壁板内侧换热中辐射所占份额，w 为各内饰节点的辐射面积权重。
# 关系：对应实现为 models/histream 中 FacetEB 的 facetrtcore.cpp 座舱步；数值参数为本测试自设，
#   与引擎缺省不一定相同（部分与文档的典型值一致，待核对）。
# ==========================================================================

"""Unit-style check of the FacetEB vehicle cabin step (docs/vehicle-thermal.md section 3).

Replicates the discrete implicit air + interior-mass equations of facetrtcore.cpp for a constant forcing and a
linear wall response q(Ta) = q0 - K (Ta - Ta0), iterates to the steady state and compares with the analytic value
    Gv (Ta - Tout) = K (Tw - Ta) + Q_occ + f_abs Q_trans,   Ti = Ta + f_abs Q_trans / Gi
and checks the energy closure of every step (sum of inputs = storage + ventilation + air conditioning).
Run: python -I tools/test-vehicle-cabin.py
"""
import math

# 作用：两节点座舱的一个后向欧拉步，返回 (Ta, Ti, qAc, residual)。
#   方程：Ca(Ta-Ta0)/dt = Gi(Ti-Ta) + qWall + Qocc + Gv(Tout-Ta) - qAc，
#         Ci(Ti-Ti0)/dt = f·Qtrans - Gi(Ti-Ta)；qWall = q0 - K(Ta-Ta0)。
#   先消去 Ti 得到只含 Ta 的线性方程 lhs·Ta = rhs（convEff 为内饰对空气的有效串联导热）；
#   开空调时按比例控制 qAc = clamp(P(Ta-Tset)/band, 0, P)：带内解 prop，超出带宽则取 P 满功率。
#   residual 为该步的能量收支残差（输入 = 储热 + 通风 + 空调），应为舍入量级。
def step(Ta0, Ti0, p, dt):
    storeA, storeI = p['Ca'] / dt, p['Ci'] / dt
    total = storeI + p['Gi']
    convEff = p['Gi'] * storeI / total
    qSol = p['f'] * p['Qtrans']
    rhs = storeA * Ta0 + p['q0'] + p['K'] * Ta0 + p['Qocc'] + p['Gv'] * p['Tout'] + p['Gi'] * (storeI * Ti0 + qSol) / total
    lhs = storeA + p['K'] + p['Gv'] + convEff
    Ta, qAc = rhs / lhs, 0.0
    if p.get('ac'):
        Tset, band, P = p['Tset'], p['band'], p['P']
        if Ta > Tset:
            gain = P / band
            prop = (rhs + gain * Tset) / (lhs + gain)
            if prop <= Tset + band: Ta, qAc = prop, gain * (prop - Tset)
            else: Ta, qAc = (rhs - P) / lhs, P
    Ti = (storeI * Ti0 + qSol + p['Gi'] * Ta) / total
    qWall = p['q0'] - p['K'] * (Ta - Ta0)
    residual = (p['Ca'] * (Ta - Ta0) + p['Ci'] * (Ti - Ti0)) / dt - (qWall + p['Qocc'] + qSol - p['Gv'] * (Ta - p['Tout']) - qAc)
    return Ta, Ti, qAc, residual

# 作用：从 Ta = Ti = Tout 起算，反复调用 step 直到稳态（steps 步），返回末态与最大单步残差。
#   壁板取常导热 K 到固定壳体温度 Tw：每步令 q0 = K(Tw - Ta)（Ta 为上一步的座舱温度）。
def run(p, dt, steps=4000):
    Ta, Ti, worst = p['Tout'], p['Tout'], 0.0
    for _ in range(steps):
        p['q0'] = p['K'] * (p['Tw'] - Ta)           # wall: conductance K to a fixed shell temperature Tw
        Ta, Ti, qAc, res = step(Ta, Ti, p, dt)
        worst = max(worst, abs(res))
    return Ta, Ti, qAc, worst

# 基准参数：Ca = ρ·cp·V（V = 2.8 m³），Ci = 80 kJ/K，Gi = 8 W m-2 K-1 × 8 m² = 64 W/K，
#   f_abs = 0.8，Qtrans = 1000 W，乘员 100 W，室外 300 K，壁板 K = 40 W/K、壳体 Tw = 320 K。
# 检查 1：3 种步长 × 3 种换气次数（ACH 0.5/10/25），稳态与解析值
#   Ta = (Gv Tout + K Tw + Qocc + f Qtrans)/(Gv + K)、Ti = Ta + f Qtrans/Gi 相对差 < 1e-6，
#   且每步闭合残差 < 1e-6 W。
base = dict(Ca=1.2 * 1005 * 2.8, Ci=80000.0, Gi=64.0, f=0.8, Qtrans=1000.0, Qocc=100.0, Tout=300.0, K=40.0, q0=0.0, Tw=320.0)
failures = 0
for dt in (600.0, 1800.0, 3600.0):
    for Gv_ach in (0.5, 10.0, 25.0):
        p = dict(base, Gv=base['Ca'] * Gv_ach / 3600.0)
        Ta, Ti, _, worst = run(p, dt)
        Ta_an = (p['Gv'] * p['Tout'] + p['K'] * p['Tw'] + p['Qocc'] + p['f'] * p['Qtrans']) / (p['Gv'] + p['K'])
        Ti_an = Ta_an + p['f'] * p['Qtrans'] / p['Gi']
        ok = abs(Ta - Ta_an) < 1e-6 * Ta_an and abs(Ti - Ti_an) < 1e-6 * Ti_an and worst < 1e-6
        failures += not ok
        print('dt %5.0f ACH %5.1f  Ta %.4f (analytic %.4f)  Ti %.4f (%.4f)  max step residual %.2e W  %s' % (dt, Gv_ach, Ta, Ta_an, Ti, Ti_an, worst, 'ok' if ok else 'FAIL'))
# 检查 2：空调。P = 3500 W 时应落在比例带内；P = 300 W 为过载，应取功率上限；
#   两种情形都检查稳态能量平衡 balance 与 qAc ≤ P。
# air conditioning: the cabin must settle inside the proportional band, or at the power limit when overloaded
for P in (3500.0, 300.0):
    p = dict(base, Gv=base['Ca'] * 10.0 / 3600.0, ac=True, Tset=297.15, band=2.0, P=P)
    Ta, Ti, qAc, worst = run(p, 1800.0)
    balance = p['K'] * (p['Tw'] - Ta) + p['Qocc'] + p['f'] * p['Qtrans'] - p['Gv'] * (Ta - p['Tout']) - qAc
    ok = abs(balance) < 1e-6 * 1000 and (qAc <= P + 1e-9) and worst < 1e-5
    failures += not ok
    print('AC P=%6.0f W: Ta %.3f K (set %.2f) Q_ac %.1f W  steady balance %.2e  %s' % (P, Ta, p['Tset'], qAc, balance, 'ok' if ok else 'FAIL'))

# 模块：第二版部件模型的单元检查（见上方原有说明与 docs/vehicle-thermal.md 第 10 节）。
# ---------------------------------------------------------------------------------------------------------------
# Parts model (docs/vehicle-thermal.md sections 3-4): air + interior nodes (+ engine block and bay) as one implicit
# linear system solved with a small Gaussian elimination, exactly as facetrtcore.cpp does.
# 作用：解 n 阶线性方程组 a·x = b（部分选主元高斯消元，不改动输入）；n 很小（≤ 8，见文档）。
def gauss(a, b):
    n = len(b)
    a = [row[:] for row in a]; b = b[:]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(a[r][i]))
        a[i], a[p] = a[p], a[i]; b[i], b[p] = b[p], b[i]
        for r in range(i + 1, n):
            f = a[r][i] / a[i][i]
            for c in range(i, n): a[r][c] -= f * a[i][c]
            b[r] -= f * b[i]
    x = [0.0] * n
    for i in range(n - 1, -1, -1):
        x[i] = (b[i] - sum(a[i][c] * x[c] for c in range(i + 1, n))) / a[i][i]
    return x

# NODES：4 个内饰节点（仪表台、座椅、方向盘、车顶内衬/门板）的参数——
#   share = 透射太阳吸收份额 s_k（和为 1）；cap = 热容 C_k（J/K）；hcA = 有效 hA_k（W/K）；
#   rad = 辐射面积 radA（m²，用于壁板辐射平均温度的权重 w_k = radA_k/ΣradA）。
#   数值为文档 10.3 的典型值（待核对）。
NODES = dict(share=[0.30, 0.35, 0.05, 0.30], cap=[10000.0, 40000.0, 2000.0, 60000.0], hcA=[8.0, 24.0, 2.0, 40.0], rad=[1.0, 3.0, 0.25, 8.0])
# 作用：部件模型的一个后向欧拉步。未知量排列：x[0] = 座舱空气 Ta，x[1..m] = 内饰节点 Tk，
#   有 engine 时再追加 x[1+m] = 发动机块 Te、x[2+m] = 发动机舱空气 Tb。
#   q 为外部条件字典：Ca、Gv、Qocc、Qsol（透射吸收后的太阳功率）、Tout、壁板 K、phi，
#   以及壁板初始热流 q0 或常导热 G 与壳体温度 Tw；engine 含 Ce、Cb、Geb、Gg、Gfw、Qblk、Qrad、Te0、Tb0。
#   返回字典：新温度、各方程残差之和 res（应接近 0）、壁板热流 qw，以及发动机分支的独立闭合 eres。
def parts_step(Ta0, Tk0, dt, q, engine=None):
    """One backward Euler step; wall response q(Tb) = q0 - K (Tb - Tb0) with Tb = (1-phi) Ta + phi sum(w Tk)."""
    m = len(Tk0); n = 1 + m + (2 if engine else 0)
    phi, K = q['phi'], q['K']
    w = [r / sum(NODES['rad']) for r in NODES['rad']]
    Tb0 = (1 - phi) * Ta0 + phi * sum(wk * t for wk, t in zip(w, Tk0))
    q0 = q['G'] * (q['Tw'] - Tb0) if 'G' in q else q['q0']
    A = [[0.0] * n for _ in range(n)]; b = [0.0] * n
    storeA = q['Ca'] / dt
    A[0][0] = storeA + sum(NODES['hcA']) + (1 - phi) ** 2 * K + q['Gv'] + (engine['Gfw'] if engine else 0)
    b[0] = storeA * Ta0 + (1 - phi) * (q0 + K * Tb0) + q['Qocc'] + q['Gv'] * q['Tout']
    for k in range(m):
        A[0][1 + k] = -NODES['hcA'][k] + (1 - phi) * K * phi * w[k]
        sk = NODES['cap'][k] / dt
        A[1 + k][0] = -NODES['hcA'][k] + phi * w[k] * K * (1 - phi)
        for j in range(m): A[1 + k][1 + j] = phi * w[k] * K * phi * w[j]
        A[1 + k][1 + k] += sk + NODES['hcA'][k]
        b[1 + k] = sk * Tk0[k] + NODES['share'][k] * q['Qsol'] + phi * w[k] * (q0 + K * Tb0)
    if engine:
        e = engine; iE, iB = 1 + m, 2 + m
        A[0][iB] = -e['Gfw']
        A[iE][iE] = e['Ce'] / dt + e['Geb']; A[iE][iB] = -e['Geb']; b[iE] = e['Ce'] / dt * e['Te0'] + e['Qblk']
        A[iB][iB] = e['Cb'] / dt + e['Geb'] + e['Gg'] + e['Gfw']; A[iB][iE] = -e['Geb']; A[iB][0] = -e['Gfw']
        b[iB] = e['Cb'] / dt * e['Tb0'] + e['Qrad'] + e['Gg'] * q['Tout']
    x = gauss(A, b)
    Ta, Tk = x[0], x[1:1 + m]
    Tb = (1 - phi) * Ta + phi * sum(wk * t for wk, t in zip(w, Tk))
    qw = q0 - K * (Tb - Tb0)
    res = []
    res.append(storeA * (Ta - Ta0) - (sum(h * (t - Ta) for h, t in zip(NODES['hcA'], Tk)) + (1 - phi) * qw + q['Qocc'] + (engine['Gfw'] * (x[2 + m] - Ta) if engine else 0) - q['Gv'] * (Ta - q['Tout'])))
    for k in range(m):
        res.append(NODES['cap'][k] / dt * (Tk[k] - Tk0[k]) - (NODES['share'][k] * q['Qsol'] - NODES['hcA'][k] * (Tk[k] - Ta) + phi * w[k] * qw))
    out = dict(Ta=Ta, Tk=Tk, res=sum(res), qw=qw, w=w)
    if engine:
        out.update(Te=x[1 + m], Tb=x[2 + m])
        fw = engine['Gfw'] * (x[2 + m] - Ta)
        out['eres'] = (engine['Ce'] * (x[1 + m] - engine['Te0']) + engine['Cb'] * (x[2 + m] - engine['Tb0'])) / dt - (engine['Qblk'] + engine['Qrad'] - engine['Gg'] * (x[2 + m] - q['Tout']) - fw)
    return out

# qbase：部件模型检查的基准条件（ACH = 0.5，Qsol = 640 W，K = 60 W/K，Tw = 310 K，phi = 0.45）。
qbase = dict(Ca=1.2 * 1005 * 2.8, Gv=1.2 * 1005 * 2.8 * 0.5 / 3600, Qocc=0.0, Qsol=640.0, Tout=300.0, K=60.0, Tw=310.0, phi=0.45)
# 1) steady state: with the wall at a fixed temperature the converged step must satisfy the steady equations of an
#    independent fixed-point (Jacobi) solution; phi = 0 has the closed form Ta = (Gv Tout + K Tw + Qocc + Qsol)/(Gv + K), Tk = Ta + s_k Qsol / hA_k.
for phi in (0.0, 0.45):
    q = dict(qbase, phi=phi, G=qbase['K'])
    Ta, Tk = 300.0, [300.0] * 4
    for _ in range(3000):
        s = parts_step(Ta, Tk, 1800.0, q); Ta, Tk = s['Ta'], s['Tk']
    w = s['w']
    # independent check of the steady equations (residual of the continuous balance at the converged state)
    Tb = (1 - phi) * Ta + phi * sum(a * t for a, t in zip(w, Tk)); qw = q['K'] * (q['Tw'] - Tb)
    air = sum(h * (t - Ta) for h, t in zip(NODES['hcA'], Tk)) + (1 - phi) * qw + q['Qocc'] + q['Gv'] * (q['Tout'] - Ta)
    nodes = [NODES['share'][k] * q['Qsol'] - NODES['hcA'][k] * (Tk[k] - Ta) + phi * w[k] * qw for k in range(4)]
    total = q['Qsol'] + qw + q['Qocc'] - q['Gv'] * (Ta - q['Tout'])
    ok = abs(air) < 1e-6 and max(abs(v) for v in nodes) < 1e-6 and abs(total) < 1e-6
    if phi == 0.0:
        Ta_an = (q['Gv'] * q['Tout'] + q['K'] * q['Tw'] + q['Qocc'] + q['Qsol']) / (q['Gv'] + q['K'])
        ok = ok and abs(Ta - Ta_an) < 1e-6 and all(abs(Tk[k] - (Ta_an + NODES['share'][k] * q['Qsol'] / NODES['hcA'][k])) < 1e-6 for k in range(4))
    failures += not ok
    print('parts steady phi=%.2f: Ta %.4f, nodes %s, balance residuals air %.1e nodes %.1e total %.1e  %s' % (phi, Ta, ['%.2f' % t for t in Tk], air, max(abs(v) for v in nodes), total, 'ok' if ok else 'FAIL'))
# 2) closure of every transient step (air + 4 nodes), several step sizes
worst = 0.0
for dt in (60.0, 600.0, 3600.0):
    Ta, Tk = 300.0, [300.0] * 4
    for _ in range(100):
        s = parts_step(Ta, Tk, dt, dict(qbase, G=qbase['K'])); worst = max(worst, abs(s['res'])); Ta, Tk = s['Ta'], s['Tk']
ok = worst < 1e-8; failures += not ok
print('parts transient closure: max |residual| %.2e W (relative %.1e of 640 W)  %s' % (worst, worst / 640.0, 'ok' if ok else 'FAIL'))
# 检查 3 的准备：线性系统 x' = A x（x 为发动机块与舱内空气相对室外的温差）的 2x2 矩阵特征分解，
#   给出两个负特征值 l1（慢）、l2（快）与解析解 analytic(t)。此处防火墙传热 Gfw = 0。
# 3) engine cool-down after parking: block (Ce, Geb) and bay (Cb, Gg + firewall) against the analytic two-exponential decay
eng = dict(Ce=70000.0, Cb=15000.0, Geb=40.0, Gg=1.2 * 1005 * 0.04, Gfw=0.0, Qblk=0.0, Qrad=0.0)
a11, a12, a21, a22 = -eng['Geb'] / eng['Ce'], eng['Geb'] / eng['Ce'], eng['Geb'] / eng['Cb'], -(eng['Geb'] + eng['Gg']) / eng['Cb']
tr, det = a11 + a22, a11 * a22 - a12 * a21
disc = math.sqrt(tr * tr / 4 - det); l1, l2 = tr / 2 + disc, tr / 2 - disc      # both negative; l1 = slow
T0e, T0b = 90.0, 30.0                                                           # excess over the outdoor air (K)
def analytic(t):
    # x' = A x, x(0) = (T0e, T0b); eigen decomposition of the 2x2 matrix
    def comp(l):
        v = (a12, l - a11)                                                      # eigenvector of (A - l I) v = 0 from the first row
        return v
    v1, v2 = comp(l1), comp(l2)
    det_v = v1[0] * v2[1] - v2[0] * v1[1]
    c1 = (T0e * v2[1] - v2[0] * T0b) / det_v; c2 = (v1[0] * T0b - T0e * v1[1]) / det_v
    return c1 * v1[0] * math.exp(l1 * t) + c2 * v2[0] * math.exp(l2 * t), c1 * v1[1] * math.exp(l1 * t) + c2 * v2[1] * math.exp(l2 * t)
tau_min = -1.0 / l1 / 60.0
# 对 dt = 10 s、60 s 各积分 2 h：温度整体加 300 K 偏移后调用 parts_step（太阳与壁板关闭，室外 300 K 仅作参考温度），
#   相对初始温差的误差应小于 tol（后向欧拉的一阶误差，故步长越大容差越宽），且发动机闭合 < 1e-8 W。
for dt, tol in ((10.0, 2e-3), (60.0, 1.2e-2)):
    Te, Tbay, worst_res, t = T0e, T0b, 0.0, 0.0
    while t < 7200.0 - 1e-9:
        e = dict(eng, Te0=Te, Tb0=Tbay)
        s = parts_step(300.0, [300.0] * 4, dt, dict(qbase, Qsol=0.0, Tw=300.0, K=0.0, G=0.0, Tout=0.0 + 300.0), engine=dict(e, Tb0=Tbay + 300.0, Te0=Te + 300.0, Qblk=0.0))
        Te, Tbay = s['Te'] - 300.0, s['Tb'] - 300.0; worst_res = max(worst_res, abs(s['eres'])); t += dt
    Ta_e, Ta_b = analytic(t)
    err = max(abs(Te - Ta_e) / T0e, abs(Tbay - Ta_b) / T0e)
    ok = err < tol and worst_res < 1e-8; failures += not ok
    print('engine cool-down dt %4.0f s, 2 h: block %.3f K (analytic %.3f), bay %.3f K (%.3f), error %.1e, closure %.1e W, slow tau %.1f min  %s' % (dt, Te, Ta_e, Tbay, Ta_b, err, worst_res, tau_min, 'ok' if ok else 'FAIL'))
raise SystemExit(1 if failures else 0)