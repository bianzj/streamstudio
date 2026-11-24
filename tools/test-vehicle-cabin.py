"""Unit-style check of the FacetEB vehicle cabin step (docs/vehicle-thermal.md section 3).

Replicates the discrete implicit air + interior-mass equations of facetrtcore.cpp for a constant forcing and a
linear wall response q(Ta) = q0 - K (Ta - Ta0), iterates to the steady state and compares with the analytic value
    Gv (Ta - Tout) = K (Tw - Ta) + Q_occ + f_abs Q_trans,   Ti = Ta + f_abs Q_trans / Gi
and checks the energy closure of every step (sum of inputs = storage + ventilation + air conditioning).
Run: python -I tools/test-vehicle-cabin.py
"""
import math

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

def run(p, dt, steps=4000):
    Ta, Ti, worst = p['Tout'], p['Tout'], 0.0
    for _ in range(steps):
        p['q0'] = p['K'] * (p['Tw'] - Ta)           # wall: conductance K to a fixed shell temperature Tw
        Ta, Ti, qAc, res = step(Ta, Ti, p, dt)
        worst = max(worst, abs(res))
    return Ta, Ti, qAc, worst

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
# air conditioning: the cabin must settle inside the proportional band, or at the power limit when overloaded
for P in (3500.0, 300.0):
    p = dict(base, Gv=base['Ca'] * 10.0 / 3600.0, ac=True, Tset=297.15, band=2.0, P=P)
    Ta, Ti, qAc, worst = run(p, 1800.0)
    balance = p['K'] * (p['Tw'] - Ta) + p['Qocc'] + p['f'] * p['Qtrans'] - p['Gv'] * (Ta - p['Tout']) - qAc
    ok = abs(balance) < 1e-6 * 1000 and (qAc <= P + 1e-9) and worst < 1e-5
    failures += not ok
    print('AC P=%6.0f W: Ta %.3f K (set %.2f) Q_ac %.1f W  steady balance %.2e  %s' % (P, Ta, p['Tset'], qAc, balance, 'ok' if ok else 'FAIL'))

# ---------------------------------------------------------------------------------------------------------------
# Parts model (docs/vehicle-thermal.md sections 3-4): air + interior nodes (+ engine block and bay) as one implicit
# linear system solved with a small Gaussian elimination, exactly as facetrtcore.cpp does.
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

NODES = dict(share=[0.30, 0.35, 0.05, 0.30], cap=[10000.0, 40000.0, 2000.0, 60000.0], hcA=[8.0, 24.0, 2.0, 40.0], rad=[1.0, 3.0, 0.25, 8.0])
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