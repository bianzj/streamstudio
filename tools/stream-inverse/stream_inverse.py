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

G = 0.5
SIGMA = 5.67e-8
C1 = 11910.439340652 * 1.0e4
C2 = 14388.291040407
PI = math.pi
EULER = 0.57721566490153286
DEG = math.pi / 180.0


# ----------------------------------------------------------------------------- utilities
def cos_zenith(angle_deg):
    return anp.cos(anp.minimum(anp.abs(angle_deg), 89.999) * DEG)


def clip(value, low, high):
    return anp.minimum(anp.maximum(value, low), high)


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


def planck(temperature_k, wavelength_um):
    return C1 / anp.power(wavelength_um, 5.0) / anp.expm1(C2 / (wavelength_um * temperature_k))


def inverse_planck(radiance, wavelength_um):
    return C2 / (wavelength_um * anp.log1p(C1 / (radiance * anp.power(wavelength_um, 5.0))))


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


def hom_split(lai, zenith_deg):
    """Unit beam reaching each node: [soil gap, lower-layer hit, upper-layer hit]."""
    cosine = anp.maximum(cos_zenith(zenith_deg), 1e-6)
    optical_depth = G * lai / cosine
    half = 0.5 * optical_depth
    top = 1.0 - anp.exp(-half)
    return anp.stack([anp.exp(-optical_depth), anp.exp(-half) * top, top])


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
def parameter_bounds():
    return np.array([0.05, 240.0, 240.0, 240.0, 240.0]), np.array([8.0, 380.0, 380.0, 380.0, 380.0])


def flatten(simulation):
    return anp.concatenate([anp.reshape(simulation["bt"], (-1,)), anp.reshape(simulation["gap"], (-1,))])


def make_residual(config, observations, sigma_bt, sigma_gap, prior, prior_sd, prior_weight=0.15):
    bt_count = len(config["views"]) * len(config["bands"])
    gap_count = len(config["views"])
    sigma = np.concatenate([np.full(bt_count, sigma_bt), np.full(gap_count, sigma_gap)])

    def residual(parameters):
        simulation = simulate(parameters, config)
        scaled = (flatten(simulation) - observations) / sigma
        return anp.concatenate([scaled, prior_weight * (parameters - prior) / prior_sd])
    return residual


def invert(config, observations, sigma_bt, sigma_gap, prior, prior_sd, initial, seed=None):
    residual = make_residual(config, observations, sigma_bt, sigma_gap, prior, prior_sd)
    jac = jacobian(residual)
    lower, upper = parameter_bounds()
    fit = least_squares(residual, np.clip(initial, lower, upper), jac=jac, bounds=(lower, upper),
                        x_scale=np.array([0.5, 5.0, 5.0, 5.0, 5.0]), max_nfev=60,
                        ftol=1e-10, xtol=1e-10, gtol=1e-10)
    return fit


# ----------------------------------------------------------------------------- demo
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
