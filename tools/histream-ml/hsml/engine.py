"""运行 histream.exe（FacetEB）并把结果压缩成“类别均值观测向量”。

观测向量按节点依次排列，每个节点：
- 叶片类（面积加权，几何法向侧）：T、Tsun、Tsh（光照/阴影部分温度，对应 LSCT 的叶片组分）、LE、H、Rn、G；
- 地面类：T、LE、H、Rn、G。
通量为 W m-2，温度为 K。选择类别均值而不是逐面元，是因为精度目标按类别/场景均值口径给定
（温度 0.1 K、辐射 0.25 W m-2，见项目文档），也让代理的输出维度保持很小。
"""
from __future__ import annotations

import copy
import json
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

from . import params as P

TOOLS = Path(__file__).resolve().parents[2]            # tools/
STUDIO = TOOLS.parent
sys.path.insert(0, str(TOOLS))
from facet_result import load as load_facet  # noqa: E402

DEFAULT_ENGINE_DIR = STUDIO / "models" / "bin_x64" / "Release"

# process/faceteb_T=*.bin 的记录布局（faceteb_T=*.json 的 fields）：7 个 float32/侧。
RECORD_FIELDS = ("LE", "H", "G", "T", "Rn", "Tsun", "Tsh")
LEAF_FIELDS = ("T", "Tsun", "Tsh", "LE", "H", "Rn", "G")
GROUND_FIELDS = ("T", "LE", "H", "Rn", "G")


def observation_names(n_nodes: int) -> list[str]:
    names = []
    for k in range(n_nodes):
        names += [f"n{k}.leaf.{f}" for f in LEAF_FIELDS]
        names += [f"n{k}.ground.{f}" for f in GROUND_FIELDS]
    return names


def observation_sigma_hint(name: str) -> float:
    """观测噪声的缺省标准差：温度 0.1 K，通量 1 W m-2（只用于 OSSE 演示；真实观测请按仪器给）。"""
    return 0.1 if name.endswith((".T", ".Tsun", ".Tsh")) else 1.0


class EngineError(RuntimeError):
    pass


class FacetEngine:
    """对一个冻结输入目录（project.json + meteo.txt + 位置文件）反复运行引擎。"""

    def __init__(self, base_input: Path, engine_dir: Path | None = None, work_dir: Path | None = None,
                 coupling_iterations: int = 60, timeout_s: int = 900, lean_outputs: bool = False):
        self.base_input = Path(base_input)
        self.engine_dir = Path(engine_dir or os.environ.get("HISTREAM_ENGINE_DIR") or DEFAULT_ENGINE_DIR)
        self.work_dir = Path(work_dir or (self.base_input.parent / "runs"))
        self.coupling_iterations = coupling_iterations
        self.timeout_s = timeout_s
        self.lean_outputs = lean_outputs
        self._base_project = json.loads((self.base_input / "project.json").read_text(encoding="utf-8-sig"))
        self._base_meteo = (self.base_input / "meteo.txt").read_text(encoding="utf-8")
        self._areas: tuple[np.ndarray, int] | None = None
        exe = self.engine_dir / "histream.exe"
        if not exe.is_file():
            raise EngineError(f"找不到引擎 {exe}（可设置环境变量 HISTREAM_ENGINE_DIR）")

    # -- 运行 ---------------------------------------------------------------------------------
    def run(self, theta: dict[str, float], tag: str, keep: bool = False, sensitivity: list[str] | None = None,
            sensitivity_iterations: int = 24, sensitivity_step: float = 0.05) -> dict:
        """运行一次，返回 {"y": 观测向量, "converged", "iterations", "residual_W_m2", "wall_s", "names"}。

        sensitivity：要引擎在收敛点给出导数的参数名（params.py 的名字，须在 ENGINE_SENSITIVITY 里）。
        给出时结果另含 "jacobian"：∂y/∂u（行 = 观测，列 = 这些参数，归一化坐标），由引擎内切线敏感度
        （control.facetSensitivity）得到，节点间热历史冻结。"""
        run = self.work_dir / tag
        if run.exists():
            shutil.rmtree(run)
        shutil.copytree(self.base_input, run)
        out = run / "out"
        out.mkdir()
        project = copy.deepcopy(self._base_project)
        cfg = project["configuration"]
        cfg["outDir"] = str(out)
        cfg["meteo"]["path"] = str(run / "meteo.txt")
        cfg["control"]["couplingIterations"] = int(self.coupling_iterations)
        cfg["control"]["facetOutputFormat"] = "binary"
        if sensitivity:
            unsupported = [s for s in sensitivity if s not in ENGINE_SENSITIVITY]
            if unsupported:
                raise EngineError(f"引擎不支持这些参数的敏感度：{unsupported}（支持 {sorted(ENGINE_SENSITIVITY)}）")
            cfg["control"]["facetSensitivity"] = ",".join(ENGINE_SENSITIVITY[s] for s in sensitivity)
            cfg["control"]["facetSensitivityIterations"] = int(sensitivity_iterations)
            cfg["control"]["facetSensitivityStep"] = float(sensitivity_step)
        if self.lean_outputs:
            # 只需要逐节点能量过程文件；关掉传感器影像与辐射过程文件以省时间和磁盘（不影响能量求解）。
            cfg["sensor"]["image"] = False
            cfg["sensor"]["radiationProcess"] = False
            cfg["sensor"]["energyProcess"] = True
        P.apply_to_project(project, theta)
        (run / "meteo.txt").write_text(P.apply_to_meteo(self._base_meteo, theta), encoding="utf-8")
        (run / "project.json").write_text(json.dumps(project, ensure_ascii=False), encoding="utf-8")
        t0 = time.perf_counter()
        proc = subprocess.run(
            [str(self.engine_dir / "histream.exe"), "eFacetEB", str(run / "project.json"), str(out / "faceteb.json")],
            cwd=str(self.engine_dir), capture_output=True, timeout=self.timeout_s)
        wall = time.perf_counter() - t0
        # 退出码 2 = 未收敛但结果保留；其余非零视为失败。
        if proc.returncode not in (0, 2) or not (out / "faceteb.json").is_file():
            tail = proc.stderr.decode("utf-8", "replace")[-600:] + proc.stdout.decode("utf-8", "replace")[-600:]
            raise EngineError(f"引擎失败 exit={proc.returncode} tag={tag}\n{tail}")
        result = self.parse(out)
        result["wall_s"] = wall
        result["exit_code"] = proc.returncode
        if sensitivity:
            result["jacobian"] = self.parse_sensitivity(out, theta, sensitivity)
            result["sensitivity_wall_s"] = sum(c["sensitivity"]["wallSeconds"] for c in
                                               json.loads((out / "faceteb_run.json").read_text(encoding="utf-8"))["completed"])
        if not keep:
            shutil.rmtree(run, ignore_errors=True)
        return result

    # -- 解析 ---------------------------------------------------------------------------------
    def _geometry(self, out: Path) -> tuple[np.ndarray, int]:
        if self._areas is None:
            r = load_facet(out / "faceteb.json", mmap=True)
            n = int(r["facetCount"])
            p = np.asarray(r["vertexPositions"], dtype=np.float64).reshape(n, 3, 3)
            area = 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1)
            self._areas = (area, int(r["leafFacetCount"]))
        return self._areas

    def parse(self, out: Path) -> dict:
        index = json.loads((out / "faceteb_run.json").read_text(encoding="utf-8"))
        area, leaf = self._geometry(out)
        n = area.size
        y: list[float] = []
        nodes = index["completed"]
        for node in nodes:
            data = np.fromfile(out / "process" / f"faceteb_T={node['token']}.bin", dtype="<f4").reshape(n, 2, 7)
            side0 = data[:, 0, :].astype(np.float64)           # 几何法向侧；地面背侧无意义
            if not np.isfinite(side0).all():
                raise EngineError("结果含非有限值")
            for cls, sl, fields in (("leaf", slice(0, leaf), LEAF_FIELDS), ("ground", slice(leaf, n), GROUND_FIELDS)):
                w = area[sl]
                for f in fields:
                    y.append(float((side0[sl, RECORD_FIELDS.index(f)] * w).sum() / w.sum()))
        return {
            "y": y,
            "names": observation_names(len(nodes)),
            "converged": bool(index["status"] == "converged" or all(c["converged"] for c in nodes)),
            "iterations": [int(c["iterations"]) for c in nodes],
            "residual_W_m2": [float(c["energyResidual"]) for c in nodes],
            "solar_zenith": [float(c["solarZenith"]) for c in nodes],
        }

    def parse_sensitivity(self, out: Path, theta: dict[str, float], names: list[str]) -> np.ndarray:
        """读 process/facetsens_T=*.bin（[参数][面侧][7]，d/d ln θ），按与 parse 相同的类别均值约定
        压成 ∂y/∂ln θ，再乘 d ln θ / du 换到归一化坐标。"""
        index = json.loads((out / "faceteb_run.json").read_text(encoding="utf-8"))
        area, leaf = self._geometry(out)
        n = area.size
        rows = []
        for node in index["completed"]:
            data = np.fromfile(out / node["sensitivity"]["file"], dtype="<f4").reshape(len(names), n, 2, 7)[:, :, 0, :]
            data = data.astype(np.float64)
            for cls, sl, fields in (("leaf", slice(0, leaf), LEAF_FIELDS), ("ground", slice(leaf, n), GROUND_FIELDS)):
                w = area[sl]
                for f in fields:
                    rows.append((data[:, sl, RECORD_FIELDS.index(f)] * w).sum(1) / w.sum())
        dy_dln = np.array(rows)                                   # (观测, 参数)
        scale = []
        for name in names:
            p = P.BY_NAME[name]
            scale.append(math.log(p.hi / p.lo) if p.log else (p.hi - p.lo) / float(theta[name]))
        return dy_dln * np.array(scale)


# params.py 的参数名 → 引擎 control.facetSensitivity 的参数名（facetrtcore.cpp，2026-11-27）。
ENGINE_SENSITIVITY = {
    "vcmax": "leaf.vcmax",
    "ball_berry_m": "leaf.ballBerrySlope",
    "soil_rss": "ground.surfaceResistance",
    "soil_lambda": "ground.thermalConductivity",
}
