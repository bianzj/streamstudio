# ==========================================================================
# 文件：stream_inverse.py
# 功能：STREAM 单像元热红外正向链的可微原型（autograd）与变分/Gauss–Newton 反演 demo：
#       由 LAI 与四组分温度预测多角度、多波段亮温和视角间隙，再用最小二乘反演这 5 个参数。
# 主要模块：
#   1. 工具函数：天顶角余弦、限幅、E1 级数与 E3、Planck 辐亮度及其反函数；
#   2. hom3 节点辐射度：hom_canopy（节点容量/交换因子/到达矩阵）、hom_split（直射束到达各节点的份额）；
#   3. 四组分比例：leaf_correlation（双尺度热点）、two_layer_sunlit_leaf、fractions；
#   4. thermal_weights：各组分温度对观测辐亮度的权重（直接发射 + 多次散射）；
#   5. simulate：正向算子 H(x)，x = [LAI, Tss, Tsh, Tls, Tlh]（K），输出亮温 bt 与间隙 gap；
#   6. 反演：参数界、残差构造 make_residual（观测项 + 先验项）、invert（scipy least_squares，解析 Jacobian）；
#   7. 入口：run_demo（合成数据反演与梯度校验）、export_sample / run_sample（样本 JSON 的导出与反演）。
# 数据流：config（太阳/观测几何、波段、发射率、热点）+ 参数 → simulate → 观测向量 [bt 展平, gap]
#         → 残差 → least_squares → 反演参数。Jacobian 由 autograd 对整条正向链自动求导。
# 与其他文件的关系：数值对应 models/stream/physics/canopy.cpp、radiosity.cpp、brightness.h（C++ 实现）；
#       说明与结果见同目录 README.md；autograd 放在 vendor/（运行时需把 vendor 加入 PYTHONPATH）。
# 约定：角度单位度；波长单位 μm；温度 K；Planck 辐亮度单位 W m-2 sr-1 μm-1（C1=2hc²，C2=hc/k）；
#       G = 0.5 为球面叶片投影函数；两层等厚冠层，每层 tau = G·LAI/2；相对方位取 0–180°。
# 注意：只是原型，未含完整球面叶片 BRF、大气与 EB 时间序列；只有亮温时四个组分温度不能独立反演。
# ==========================================================================

"""Differentiable single-pixel STREAM forward model and inversion prototype.

Role in the system:
  HiStream (and STREAM-Net) generate samples / correct forward simulations.
  STREAM is the inversion operator.  This module makes the STREAM pixel-level
  forward chain differentiable so it can be used as H(x) in a variational or
  Gauss-Newton retrieval.

Implemented here (single homogeneous pixel, TiRT standard hom3 closure):
  * sunlit/shaded four-component fractions with the dual-scale hotspot
  * hom3 node radiosity (soil + two canopy layers, 5 states)
  * multiple-scattering effective emissivity
  * Planck radiance / brightness temperature
  * view gap (an LAI-sensitive observation proxy)

Not yet implemented: full spherical-leaf BRF, atmosphere, EB time series.
The numerics mirror models/stream/physics/{canopy,radiosity}.cpp and
models/stream/physics/brightness.h.

Usage:
  python stream_inverse.py --demo
  python stream_inverse.py --demo --noise 0.5 --seed 2
  python stream_inverse.py --export-sample sample.json
  python stream_inverse.py --sample sample.json
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
import autograd.numpy as anp
from autograd import grad, jacobian
from scipy.optimize import least_squares

# 常量：G 为叶片投影函数（球面叶分布取 0.5）；SIGMA 为 Stefan–Boltzmann 常数；
# C1、C2 为 Planck 常数（波长 μm、辐亮度 W m-2 sr-1 μm-1，C1 = 1.19104e8，C2 = 14388.29 μm K）；
# EULER 为 Euler–Mascheroni 常数（E1 级数用）；DEG 为度转弧度。
G = 0.5
SIGMA = 5.67e-8
C1 = 11910.439340652 * 1.0e4
C2 = 14388.291040407
PI = math.pi
EULER = 0.57721566490153286
DEG = math.pi / 180.0


# ----------------------------------------------------------------------------- utilities
# 作用：天顶角（度）的余弦；角度限制在 89.999° 以内，避免掠射时除零。
def cos_zenith(angle_deg):
    return anp.cos(anp.minimum(anp.abs(angle_deg), 89.999) * DEG)


# 作用：可微限幅（minimum/maximum 在 autograd 下给出分段梯度）。
def clip(value, low, high):
    return anp.minimum(anp.maximum(value, low), high)


# 说明：E1(x) = -γ - ln x - Σ (-x)^k/(k·k!)，取前 80 项；仅对 x 不大（约 ≤ 8）时收敛得足够准。
def e1_series(x):
    """E1(x) for x in (0, ~8]: convergent series, fully differentiable."""
    total = -EULER - anp.log(x)
    power = 1.0
    for k in range(1, 81):
        power = power * x / k
        total = total + (power / k) * (1.0 if k % 2 == 1 else -1.0)
    return total


def e3(x):
    """E3(x) = [e^-x (1-x) + x^2 E1(x)] / 2."""
    return 0.5 * (anp.exp(-x) * (1.0 - x) + x * x * e1_series(x))


# 作用：Planck 辐亮度 B(λ,T) = C1 / (λ^5 · (exp(C2/(λT)) - 1))；inverse_planck 为其反函数（亮温，K）。
def planck(temperature_k, wavelength_um):
    return C1 / anp.power(wavelength_um, 5.0) / anp.expm1(C2 / (wavelength_um * temperature_k))


def inverse_planck(radiance, wavelength_um):
    return C2 / (wavelength_um * anp.log1p(C1 / (radiance * anp.power(wavelength_um, 5.0))))


# 作用：把嵌套列表（元素为 autograd 标量/数组）逐行堆叠成矩阵，保持可微。
def rows_to_matrix(rows):
    return anp.stack([anp.stack(row) for row in rows])


# ----------------------------------------------------------------------------- hom3 radiosity
def hom_canopy(lai):
    """Capacities, exchange factors, arrival operator and sensor radiance weights."""
    tau = G * lai / 2.0  # two equal layers
    capacity = anp.stack([1.0, 4.0 * tau, 4.0 * tau])
    x = lambda value: 1.0 - 2.0 * e3(value)
    exchange = [
        [0.0, x(tau), x(2.0 * tau) - x(tau)],
        [x(tau), 4.0 * tau - 2.0 * x(tau), 2.0 * x(tau) - x(2.0 * tau)],
        [x(2.0 * tau) - x(tau), 2.0 * x(tau) - x(2.0 * tau), 4.0 * tau - 2.0 * x(tau)],
    ]
    factors = [[exchange[i][j] / capacity[i] for j in range(3)] for i in range(3)]
    escape = anp.stack([
        clip(1.0 - sum(factors[i][j] for j in range(3)), 0.0, 1.0) for i in range(3)
    ])
    zero = 0.0
    arrive = rows_to_matrix([
        [factors[0][0], zero, factors[0][1], zero, factors[0][2], zero],
        [zero, zero, 1.0 - 2.0 * (factors[1][2] + escape[1]), zero, 2.0 * factors[1][2], zero],
        [zero, zero, zero, zero, 1.0 - 2.0 * escape[2], zero],
        [zero, zero, zero, 1.0, zero, zero],
        [zero, 2.0 * factors[2][0], zero, 2.0 * factors[2][1], zero, 1.0 - 2.0 * (factors[2][0] + factors[2][1])],
    ])
    radiance = anp.stack([1.0 / PI, 2.0 / (PI * capacity[1]), 2.0 / (PI * capacity[2]), 0.0, 0.0])
    return {"capacity": capacity, "factors": factors, "escape": escape, "arrive": arrive, "radiance": radiance, "tau": tau}


# 作用：单位直射束（沿天顶角方向）到达三个节点的份额：[土壤缝隙, 下层命中, 上层命中]。
# 假定两层等厚：穿过上层的透过率为 exp(-τ_total/2)，总透过率 exp(-G·LAI/cosθ)。
def hom_split(lai, zenith_deg):
    """Unit beam reaching each node: [soil gap, lower-layer hit, upper-layer hit]."""
    cosine = anp.maximum(cos_zenith(zenith_deg), 1e-6)
    optical_depth = G * lai / cosine
    half = 0.5 * optical_depth
    top = 1.0 - anp.exp(-half)
    return anp.stack([anp.exp(-optical_depth), anp.exp(-half) * top, top])


# 作用：双尺度热点的叶片相关系数（太阳与观测方向重合时趋于 1）。
# 说明：separation 为太阳/观测方向在地面投影上的角距（以 tan 表示）；hotspot 为热点尺度参数，
#       对应 canopy.cpp 的热点相关项（公式细节待与 C++ 核对）。
def leaf_correlation(hotspot, sun_zenith, view_zenith, relative_azimuth):
    sun_tan = anp.tan(anp.minimum(anp.abs(sun_zenith), 89.999) * DEG)
    view_tan = anp.tan(anp.minimum(anp.abs(view_zenith), 89.999) * DEG)
    separation = anp.sqrt(anp.maximum(
        (view_tan - sun_tan) ** 2 + 4.0 * view_tan * sun_tan * anp.sin(relative_azimuth * DEG / 2.0) ** 2,
        0.0))
    a = separation * 2.0 / (G / anp.maximum(cos_zenith(sun_zenith), 1e-6) +
                            G / anp.maximum(cos_zenith(view_zenith), 1e-6)) / hotspot
    a_safe = anp.maximum(a, 1e-12)
    return anp.where(separation < 1e-12, 1.0, -anp.expm1(-a_safe) / a_safe)


# 作用：由观测方向间隙 view_gap、太阳方向间隙 sun_gap 与联合间隙 sunlit_soil（土壤被光照且可见的份额）
# 估计观测方向上“被光照叶片”的可见份额：把冠层分成上层与下层，上层按间隙重叠计算。
# index = 0.82 - 0.08·LAI（下限 0.02）为经验的上层权重；与 C++ 的对应关系待核对。
def two_layer_sunlit_leaf(lai, view_gap, sun_gap, sunlit_soil):
    pv = clip(view_gap, 1e-12, 1.0)
    ps = clip(sun_gap, 1e-12, 1.0)
    pss = anp.maximum(sunlit_soil, 1e-300)
    kv = -anp.log(pv) / lai
    ks = -anp.log(ps) / lai
    overlap = clip(anp.log(pss / (pv * ps)) / lai, 0.0, anp.minimum(kv, ks))
    index = anp.maximum(0.82 - 0.08 * lai, 0.02)
    upper = anp.sqrt(
        (-anp.log(1.0 - (1.0 - pv) * index) / anp.maximum(kv * lai, 1e-12)) *
        (-anp.log(1.0 - (1.0 - ps) * index) / anp.maximum(ks * lai, 1e-12)))
    lai_upper = lai * upper
    lai_bottom = lai * (1.0 - upper)
    lit_background = anp.exp(-(ks + kv - overlap) * lai_upper)
    lit_bottom = 1.0 - anp.exp(-overlap * lai_bottom)
    return clip(1.0 - anp.exp(-kv * lai_upper) + lit_bottom * lit_background, 0.0, 1.0 - pv)


# 作用：观测方向上四组分的像元面积比例：soilSun、soilShade、leafSun、leafShade，另给间隙 gap（= 土壤可见份额）。
# 步骤：1 单向间隙 gs、gv；2 热点相关 → 联合间隙 joint；3 土壤光照份额 = joint；
#       4 叶片光照份额由 two_layer_sunlit_leaf 给出；5 其余按和为 1 补齐（均限幅保证非负）。
def fractions(lai, hotspot, sun_zenith, view_zenith, relative_azimuth):
    mu_s = anp.maximum(cos_zenith(sun_zenith), 1e-6)
    mu_v = anp.maximum(cos_zenith(view_zenith), 1e-6)
    ks, kv = G / mu_s, G / mu_v
    gs, gv = anp.exp(-ks * lai), anp.exp(-kv * lai)
    ts, tv = ks * lai, kv * lai
    correlation = leaf_correlation(hotspot, sun_zenith, view_zenith, relative_azimuth)
    shared = anp.minimum(anp.minimum(tv, ts), anp.sqrt(tv * ts) * correlation)
    joint = anp.exp(-(ts + tv - shared))
    leaf_sun = two_layer_sunlit_leaf(lai, gv, gs, joint)
    soil_sun = clip(joint, 0.0, gv)
    return {
        "soilSun": soil_sun,
        "soilShade": anp.maximum(gv - soil_sun, 0.0),
        "leafSun": clip(leaf_sun, 0.0, anp.maximum(1.0 - gv, 0.0)),
        "leafShade": anp.maximum(1.0 - gv - leaf_sun, 0.0),
        "gap": gv,
    }


# 作用：四个组分温度对观测方向热辐亮度的权重（长度 4，对应 Tss、Tsh、Tls、Tlh）。
# 权重 = 直接发射（组分比例 x 发射率）+ 多次散射贡献。
# 步骤：1 用直射束到达份额得各节点被光照的比例 sunlit；2 source 为各节点由四种温度发射的权重；
#       3 emitted 为 5 个状态的发射项；4 散射矩阵 scatter（土壤反照率 1-ε；叶片前向 (1-ε)/3、
#       后向 2(1-ε)/3，系数与 C++ 的一致性待核对）；
#       5 解 (I - scatter·arrive^T)·total = emitted 得全阶辐射度，减去 emitted 为散射部分；
#       6 乘观测方向的节点辐亮度系数得多次散射项。
def thermal_weights(nodes, leaf_emissivity, soil_emissivity, fractions_view,
                    view_zenith, sun_zenith):
    view = hom_split(nodes["tau"] * 4.0, view_zenith)
    sun = hom_split(nodes["tau"] * 4.0, sun_zenith)
    k_sun = G / anp.maximum(cos_zenith(sun_zenith), 1e-6)
    capacity = nodes["capacity"]
    leaf_area = capacity / (4.0 * G)
    sunlit = anp.stack([
        sun[0],
        clip(sun[1] / anp.maximum(k_sun * leaf_area[1], 1e-12), 0.0, 1.0),
        clip(sun[2] / anp.maximum(k_sun * leaf_area[2], 1e-12), 0.0, 1.0),
    ])
    source = rows_to_matrix([
        [soil_emissivity * sunlit[0], soil_emissivity * (1.0 - sunlit[0]), 0.0, 0.0],
        [0.0, 0.0, leaf_emissivity * sunlit[1], leaf_emissivity * (1.0 - sunlit[1])],
        [0.0, 0.0, leaf_emissivity * sunlit[2], leaf_emissivity * (1.0 - sunlit[2])],
    ])
    emitted = rows_to_matrix([
        PI * capacity[0] * source[0],
        0.5 * PI * capacity[1] * source[1],
        0.5 * PI * capacity[2] * source[2],
        0.5 * PI * capacity[1] * source[1],
        0.5 * PI * capacity[2] * source[2],
    ])
    soil_albedo = 1.0 - soil_emissivity
    back = 2.0 * (1.0 - leaf_emissivity) / 3.0
    forward = (1.0 - leaf_emissivity) / 3.0
    scatter = rows_to_matrix([
        [soil_albedo, soil_albedo, 0.0, 0.0, 0.0, 0.0],
        [0.0, 0.0, forward, back, 0.0, 0.0],
        [0.0, 0.0, 0.0, 0.0, forward, back],
        [0.0, 0.0, back, forward, 0.0, 0.0],
        [0.0, 0.0, 0.0, 0.0, back, forward],
    ])
    matrix = anp.eye(5) - anp.dot(scatter, anp.transpose(nodes["arrive"]))
    total = anp.linalg.solve(matrix, emitted)
    scattered = total - emitted
    seen = anp.stack([view[0] * nodes["radiance"][0], view[1] * nodes["radiance"][1],
                      view[2] * nodes["radiance"][2], 0.0, 0.0])
    multiple = anp.dot(seen, scattered)
    direct = anp.stack([
        fractions_view["soilSun"] * soil_emissivity,
        fractions_view["soilShade"] * soil_emissivity,
        fractions_view["leafSun"] * leaf_emissivity,
        fractions_view["leafShade"] * leaf_emissivity,
    ])
    return direct + multiple


# 模块：正向算子 H(x)。输入 parameters = [LAI, Tss, Tsh, Tls, Tlh]（K 为温度单位）与 config；
# 输出 {bt: [视角, 波段] 亮温 K, gap: [视角] 间隙}。
# 步骤：1 由 LAI 建节点；2 逐视角算相对方位（折到 0–180°）与四组分比例；3 算权重；
#       4 对每个波段：权重 · Planck(四组分温度) 得辐亮度，再反演成亮温。
def simulate(parameters, config):
    """Forward model.  parameters = [LAI, Tss, Tsh, Tls, Tlh] (K)."""
    lai = parameters[0]
    temperatures = anp.stack([parameters[1], parameters[2], parameters[3], parameters[4]])
    nodes = hom_canopy(lai)
    hot = config["hotspot"]
    brightness, gaps = [], []
    for view_zenith, view_azimuth in config["views"]:
        relative = abs((view_azimuth - config["saa"] + 180.0) % 360.0 - 180.0)
        parts = fractions(lai, hot, config["sza"], view_zenith, relative)
        weights = thermal_weights(nodes, config["leaf_emissivity"], config["soil_emissivity"],
                                  parts, view_zenith, config["sza"])
        band_values = []
        for wavelength in config["bands"]:
            radiance = anp.sum(weights * planck(temperatures, wavelength))
            band_values.append(inverse_planck(radiance, wavelength))
        brightness.append(anp.stack(band_values))
        gaps.append(parts["gap"])
    return {"bt": anp.stack(brightness), "gap": anp.stack(gaps)}


# ----------------------------------------------------------------------------- inversion helpers
# 参数物理界：LAI 0.05–8；四个组分温度 240–380 K（least_squares 的 bounds）。
def parameter_bounds():
    return np.array([0.05, 240.0, 240.0, 240.0, 240.0]), np.array([8.0, 380.0, 380.0, 380.0, 380.0])


# 作用：把正向输出展平成观测向量：先 bt（视角 x 波段，行优先），再 gap（视角）。
def flatten(simulation):
    return anp.concatenate([anp.reshape(simulation["bt"], (-1,)), anp.reshape(simulation["gap"], (-1,))])


# 作用：构造最小二乘残差函数。观测项 = (模拟 - 观测)/σ（bt 与 gap 各用自己的 σ）；
# 先验项 = prior_weight·(x - 先验)/先验标准差，prior_weight=0.15 为弱先验（取值依据待核对）。
def make_residual(config, observations, sigma_bt, sigma_gap, prior, prior_sd, prior_weight=0.15):
    bt_count = len(config["views"]) * len(config["bands"])
    gap_count = len(config["views"])
    sigma = np.concatenate([np.full(bt_count, sigma_bt), np.full(gap_count, sigma_gap)])

    def residual(parameters):
        simulation = simulate(parameters, config)
        scaled = (flatten(simulation) - observations) / sigma
        return anp.concatenate([scaled, prior_weight * (parameters - prior) / prior_sd])
    return residual


# 作用：带界的 Gauss–Newton 类最小二乘反演（scipy least_squares，默认 trust-region reflective），
# Jacobian 用 autograd 的解析结果；x_scale 平衡 LAI 与温度量纲；最多 60 次函数评估。
# seed 参数目前未使用。
def invert(config, observations, sigma_bt, sigma_gap, prior, prior_sd, initial, seed=None):
    residual = make_residual(config, observations, sigma_bt, sigma_gap, prior, prior_sd)
    jac = jacobian(residual)
    lower, upper = parameter_bounds()
    fit = least_squares(residual, np.clip(initial, lower, upper), jac=jac, bounds=(lower, upper),
                        x_scale=np.array([0.5, 5.0, 5.0, 5.0, 5.0]), max_nfev=60,
                        ftol=1e-10, xtol=1e-10, gtol=1e-10)
    return fit


# ----------------------------------------------------------------------------- demo
# demo 的观测配置：太阳天顶 35°、方位 150°；三个视角 (VZA, VAA)；三个热红外波段（μm）；
# hotspot=0.2；叶片发射率 0.985，土壤发射率 0.955。
def demo_config():
    return {
        "sza": 35.0,
        "saa": 150.0,
        "views": [[0.0, 0.0], [35.0, 90.0], [55.0, 180.0]],
        "bands": [10.5, 11.5, 12.0],
        "hotspot": 0.2,
        "leaf_emissivity": 0.985,
        "soil_emissivity": 0.955,
    }


# 作用：合成数据的完整反演 demo。步骤：1 用真值生成观测并加高斯噪声（bt 的 σ 为 noise，gap 的 σ 为 0.02）；
#       2 在真值处做 autograd 与中心差分的梯度校验（对象为第 1 视角第 1 波段的亮温）；
#       3 反演并统计观测 RMSE、LAI 误差、温度误差、解处 Jacobian 条件数；4 打印 JSON。
# 注意：噪声个数按 3 个波段（3*len(views)）写死，与 demo_config 的波段数一致。
def run_demo(noise, seed):
    config = demo_config()
    truth = np.array([2.5, 312.0, 302.0, 308.0, 301.0])
    clean = flatten(simulate(truth, config))
    rng = np.random.default_rng(seed)
    sigma_bt, sigma_gap = noise, 0.02
    observations = clean + np.concatenate([
        rng.normal(0.0, sigma_bt, 3 * len(config["views"])),
        rng.normal(0.0, sigma_gap, len(config["views"])),
    ])
    prior = np.array([2.0, 305.0, 300.0, 303.0, 300.0])
    prior_sd = np.array([1.0, 8.0, 8.0, 8.0, 8.0])
    initial = np.array([1.5, 302.0, 298.0, 301.0, 299.0])

    # gradient check at the truth
    def bt_scalar(parameters):
        return simulate(parameters, config)["bt"][0, 0]
    analytic = grad(bt_scalar)(truth)
    finite = np.zeros_like(truth)
    step = np.array([1e-5, 1e-4, 1e-4, 1e-4, 1e-4])
    for index in range(truth.size):
        plus, minus = truth.copy(), truth.copy()
        plus[index] += step[index]
        minus[index] -= step[index]
        finite[index] = (bt_scalar(plus) - bt_scalar(minus)) / (2.0 * step[index])
    gradient_error = float(np.max(np.abs(analytic - finite) / np.maximum(np.abs(finite), 1e-12)))

    fit = invert(config, observations, sigma_bt, sigma_gap, prior, prior_sd, initial, seed)
    retrieved = fit.x
    prediction = flatten(simulate(retrieved, config))
    residual = prediction - observations
    jac_at_solution = np.asarray(jacobian(lambda p: flatten(simulate(p, config)))(retrieved))
    condition = float(np.linalg.cond(jac_at_solution))

    print(json.dumps({
        "truth": truth.tolist(),
        "initial": initial.tolist(),
        "retrieved": [round(float(value), 4) for value in retrieved],
        "prior": prior.tolist(),
        "evaluation_count": int(fit.nfev),
        "cost": float(fit.cost),
        "gradient_relative_error": gradient_error,
        "observation_rmse": float(np.sqrt(np.mean(residual ** 2))),
        "lai_error": float(abs(retrieved[0] - truth[0])),
        "mean_temperature_error": float(abs(np.mean(retrieved[1:]) - np.mean(truth[1:]))),
        "component_temperature_rmse": float(np.sqrt(np.mean((retrieved[1:] - truth[1:]) ** 2))),
        "jacobian_condition": condition,
        "success": bool(fit.success),
        "note": "BT alone constrains LAI and the effective temperature well; individual sunlit/shaded "
                "component temperatures are correlated (high condition number). Add multi-angle, "
                "multi-band TIR, optical BRF or tighter priors to separate them.",
    }, indent=2, ensure_ascii=False))


# 作用：导出一份“HiStream 风格”的合成样本 JSON（kind=stream-inversion-sample，schemaVersion=1），
# 含配置、带噪观测、σ、真值、先验与初值；真值仅用于评估，真实样本可省略。
def export_sample(path, noise, seed):
    config = demo_config()
    truth = np.array([2.5, 312.0, 302.0, 308.0, 301.0])
    clean = flatten(simulate(truth, config))
    rng = np.random.default_rng(seed)
    observations = clean + np.concatenate([
        rng.normal(0.0, noise, 3 * len(config["views"])),
        rng.normal(0.0, 0.02, len(config["views"])),
    ])
    sample = {
        "kind": "stream-inversion-sample",
        "schemaVersion": 1,
        "config": config,
        "observations": [float(value) for value in observations],
        "sigma_bt": noise,
        "sigma_gap": 0.02,
        "truth": truth.tolist(),
        "prior": [2.0, 305.0, 300.0, 303.0, 300.0],
        "prior_sd": [1.0, 8.0, 8.0, 8.0, 8.0],
        "initial": [1.5, 302.0, 298.0, 301.0, 299.0],
    }
    Path(path).write_text(json.dumps(sample, indent=2), encoding="utf-8")
    print(path)


# 作用：读样本 JSON 并反演，打印反演参数、真值（若有）、代价与评估次数。
def run_sample(path):
    sample = json.loads(Path(path).read_text(encoding="utf-8"))
    config = sample["config"]
    observations = np.asarray(sample["observations"], dtype=float)
    prior = np.asarray(sample["prior"], dtype=float)
    prior_sd = np.asarray(sample["prior_sd"], dtype=float)
    initial = np.asarray(sample["initial"], dtype=float)
    fit = invert(config, observations, float(sample["sigma_bt"]), float(sample["sigma_gap"]),
                 prior, prior_sd, initial)
    print(json.dumps({
        "retrieved": [round(float(value), 4) for value in fit.x],
        "truth": sample.get("truth"),
        "cost": float(fit.cost),
        "evaluation_count": int(fit.nfev),
        "success": bool(fit.success),
    }, indent=2, ensure_ascii=False))


# 命令行入口：--export-sample 导出样本；--sample 反演样本；否则运行 demo（--noise 为亮温噪声 σ，K）。
def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--demo", action="store_true", help="synthetic single-pixel inversion demo")
    parser.add_argument("--noise", type=float, default=0.3, help="BT noise sigma (K)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--export-sample", type=Path, help="write a HiStream-export style sample JSON")
    parser.add_argument("--sample", type=Path, help="run inversion on a sample JSON")
    args = parser.parse_args()
    if args.export_sample:
        export_sample(args.export_sample, args.noise, args.seed)
    elif args.sample:
        run_sample(args.sample)
    else:
        run_demo(args.noise, args.seed)


if __name__ == "__main__":
    main()
