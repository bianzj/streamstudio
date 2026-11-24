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
raise SystemExit(1 if failures else 0)
