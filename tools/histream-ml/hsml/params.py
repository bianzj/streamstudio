"""参数空间定义。

每个参数有物理取值范围 [lo, hi]；log=True 时在对数坐标上归一化（电阻、风速倍率等跨度大的量）。
代理和反演都在归一化坐标 u ∈ [0, 1]^d 中工作，物理量只出现在写入工程文件与报告里。

两类参数：
- 待反演参数（role="state"）：叶片 Vcmax、Ball-Berry 斜率、土壤导热率、土壤表面阻抗；
- 强迫扰动（role="forcing"）：气温偏移、风速倍率、下行短波倍率。反演时通常当作已知（测站给定）
  或带紧先验的干扰量，训练代理时一并采样，使代理能在不同天气下使用。
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass
from typing import Callable

import numpy as np


@dataclass(frozen=True)
class Param:
    name: str
    lo: float
    hi: float
    default: float
    role: str          # "state" | "forcing"
    unit: str
    log: bool = False
    note: str = ""

    def to_unit(self, value):
        value = np.asarray(value, dtype=float)
        if self.log:
            return (np.log(value) - math.log(self.lo)) / (math.log(self.hi) - math.log(self.lo))
        return (value - self.lo) / (self.hi - self.lo)

    def from_unit(self, u):
        u = np.asarray(u, dtype=float)
        if self.log:
            return np.exp(math.log(self.lo) + u * (math.log(self.hi) - math.log(self.lo)))
        return self.lo + u * (self.hi - self.lo)


# 参数空间。v1：feb_crop 两节点原型（2026-11-27 第一版）；v2：再加 BSM 光谱土壤含水量（48 节点日循环）。
# 默认值等于示例工程里的设置。代码一律通过 P.PARAMS / P.NAMES / P.BY_NAME 取当前空间，
# 用 set_space() 切换；Surrogate.load 会按模型里保存的参数名自动切换。
_COMMON: tuple[Param, ...] = (
    Param("vcmax", 25.0, 120.0, 60.0, "state", "umol m-2 s-1", note="materials[leaf_c4].params.Vcmax"),
    Param("ball_berry_m", 2.0, 9.0, 4.0, "state", "-", note="materials[leaf_c4].params.m"),
    Param("soil_lambda", 0.4, 2.5, 1.0, "state", "W m-1 K-1", note="materials[soilset].params.lambdas（土壤导热率）"),
    Param("soil_rss", 500.0, 6000.0, 2000.0, "state", "s m-1", log=True, note="materials[soilset].params.rss"),
)
_FORCING: tuple[Param, ...] = (
    Param("d_ta", -4.0, 4.0, 0.0, "forcing", "K", note="气象文件 Ta 列整体偏移"),
    Param("u_scale", 0.5, 2.0, 1.0, "forcing", "-", log=True, note="气象文件风速列倍率"),
    Param("rin_scale", 0.6, 1.05, 1.0, "forcing", "-", note="气象文件下行短波列倍率"),
)
# BSM 含水量取 5–40 %：引擎在 5 % 以下取干土光谱（compo.cpp，2026-11-27），该区间导数为 0。
# 以百分数写入（> 1），避开引擎“≤ 1 按小数”的约定区间。
_BSM_SMC = Param("bsm_smc", 5.0, 40.0, 25.0, "state", "%", note="spectra[soil].params.SMC（BSM 反射率与 TIR 发射率）")
SPACES: dict[str, tuple[Param, ...]] = {
    "v1": _COMMON + _FORCING,
    "v2": _COMMON + (_BSM_SMC,) + _FORCING,
}
SPACE = "v1"
PARAMS: tuple[Param, ...] = SPACES[SPACE]
NAMES = tuple(p.name for p in PARAMS)
BY_NAME = {p.name: p for p in PARAMS}


def set_space(name: str) -> None:
    global SPACE, PARAMS, NAMES, BY_NAME
    if name not in SPACES:
        raise KeyError(f"未知参数空间 {name}（可选 {sorted(SPACES)}）")
    SPACE, PARAMS = name, SPACES[name]
    NAMES = tuple(p.name for p in PARAMS)
    BY_NAME = {p.name: p for p in PARAMS}


def set_space_for(names) -> None:
    """按参数名列表找到对应空间（读旧模型/数据时用）。"""
    for key, space in SPACES.items():
        if tuple(p.name for p in space) == tuple(names):
            set_space(key)
            return
    raise KeyError(f"没有与参数名 {list(names)} 匹配的参数空间")


def defaults() -> dict[str, float]:
    return {p.name: p.default for p in PARAMS}


def to_unit(theta: dict[str, float]) -> np.ndarray:
    return np.array([BY_NAME[n].to_unit(theta[n]) for n in NAMES], dtype=float)


def from_unit(u) -> dict[str, float]:
    u = np.asarray(u, dtype=float)
    return {n: float(BY_NAME[n].from_unit(u[i])) for i, n in enumerate(NAMES)}


def _material(project: dict, name: str) -> dict:
    for item in project["configuration"]["materials"]:
        if item["name"] == name:
            return item
    raise KeyError(f"工程里没有材料 {name}")


def apply_to_project(project: dict, theta: dict[str, float]) -> None:
    """把状态参数写进工程 JSON（就地修改）。强迫参数由 apply_to_meteo 处理。"""
    leaf = _material(project, "leaf_c4")["params"]
    leaf["Vcmax"] = float(theta["vcmax"])
    leaf["m"] = float(theta["ball_berry_m"])
    soil = _material(project, "soilset")["params"]
    soil["lambdas"] = float(theta["soil_lambda"])
    soil["rss"] = float(theta["soil_rss"])
    if "bsm_smc" in theta:
        spectral = [s for s in project["configuration"]["spectra"] if s["name"] == "soil" and s.get("model") == "BSM"]
        if not spectral:
            raise KeyError("工程里没有 BSM 光谱 soil")
        spectral[0]["params"]["SMC"] = float(theta["bsm_smc"])


_NUMBER = re.compile(r"\S+")


def apply_to_meteo(text: str, theta: dict[str, float]) -> str:
    """气象文件：首行记录数，之后每行 JulianTime Ta ea p u Rin Rli [Rb Rd]。

    只改 Ta（偏移）、u（倍率）、Rin 与可选的 Rb/Rd（同一倍率，保持直散比例）。
    Rli 与 ea 不变：气温偏移因此同时改变饱和差（相对湿度随之变化），这是有意的。
    """
    d_ta, u_scale, rin_scale = float(theta["d_ta"]), float(theta["u_scale"]), float(theta["rin_scale"])
    lines = text.splitlines()
    out = [lines[0]]
    for line in lines[1:]:
        cols = line.split()
        if len(cols) < 7:
            out.append(line)
            continue
        cols[1] = f"{float(cols[1]) + d_ta:.6g}"
        cols[4] = f"{float(cols[4]) * u_scale:.6g}"
        cols[5] = f"{float(cols[5]) * rin_scale:.6g}"
        if len(cols) >= 9:
            cols[7] = f"{float(cols[7]) * rin_scale:.6g}"
            cols[8] = f"{float(cols[8]) * rin_scale:.6g}"
        out.append(" ".join(cols))
    return "\n".join(out) + "\n"


def sobol_unit(n: int, seed: int, dims: int | None = None) -> np.ndarray:
    """n 个 Sobol 点（带扰动，n 取 2 的幂时平衡性最好）。"""
    from scipy.stats import qmc
    d = dims or len(PARAMS)
    return qmc.Sobol(d=d, scramble=True, seed=seed).random(n)
