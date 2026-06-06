# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT

import json
import math
import os
import re
import shlex
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Tuple

try:
    from skopt import gp_minimize
    from skopt.space import Integer, Real
except ImportError:
    print(
        "Missing dependency: skopt. Install with: pip install scikit-optimize",
        file=sys.stderr,
    )
    raise


# Large objective assigned to failed/invalid trials so the optimizer avoids them.
FAILURE_PENALTY = 1e12


@dataclass
class TrialResult:
    trial_index: int
    params: Dict[str, object]
    score_raw: float
    objective: float
    mode: str
    return_code: int
    duration_sec: float
    timed_out: bool
    metric_source: str


def build_search_space(args):
    if args.min_bytes_low <= 0 or args.min_bytes_low >= args.min_bytes_high:
        raise ValueError("Invalid bounds for DTO_MIN_BYTES")
    if args.min_bytes_step <= 0:
        raise ValueError("DTO_MIN_BYTES step must be > 0")
    if not (0.0 <= args.cpu_fraction_low < args.cpu_fraction_high <= 1.0):
        raise ValueError("DTO_CPU_SIZE_FRACTION bounds must satisfy 0 <= low < high <= 1")
    if args.cpu_fraction_step <= 0:
        raise ValueError("DTO_CPU_SIZE_FRACTION step must be > 0")

    return [
        Integer(
            low=args.min_bytes_low,
            high=args.min_bytes_high,
            prior="log-uniform",
            name="DTO_MIN_BYTES",
        ),
        Real(
            low=args.cpu_fraction_low,
            high=args.cpu_fraction_high,
            prior="uniform",
            name="DTO_CPU_SIZE_FRACTION",
        ),
    ]


def snap_int(value: int, low: int, high: int, step: int) -> int:
    snapped = low + round((value - low) / step) * step
    return max(low, min(high, snapped))


def snap_float(value: float, low: float, high: float, step: float) -> float:
    snapped = low + round((value - low) / step) * step
    snapped = max(low, min(high, snapped))
    return round(snapped, 6)


def sanitize_params(point: List[object], args) -> Dict[str, str]:
    min_bytes = snap_int(int(point[0]), args.min_bytes_low, args.min_bytes_high, args.min_bytes_step)
    cpu_fraction = snap_float(
        float(point[1]),
        args.cpu_fraction_low,
        args.cpu_fraction_high,
        args.cpu_fraction_step,
    )

    return {
        "DTO_MIN_BYTES": str(min_bytes),
        "DTO_CPU_SIZE_FRACTION": f"{cpu_fraction:.4f}",
        "DTO_AUTO_ADJUST_KNOBS": "0",
    }


def extract_metric(output_text: str, metric_pattern: re.Pattern) -> float:
    matches = metric_pattern.findall(output_text)
    if not matches:
        raise ValueError("Metric regex did not match command output")

    is_tuple_matches = isinstance(matches[0], tuple)
    total = 0.0
    for match in matches:
        total += float(match[0] if is_tuple_matches else match)

    return total


def evaluate_trial(
    command: str,
    metric_pattern: re.Pattern,
    mode: str,
    timeout_sec: int,
    env_patch: Dict[str, str],
) -> Tuple[float, float, int, float, bool, str]:
    env = os.environ.copy()
    env.update(env_patch)

    start = time.perf_counter()
    timed_out = False
    metric_source = "stdout/stderr"
    try:
        proc = subprocess.run(
            shlex.split(command),
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            check=False,
        )
        output_text = f"{proc.stdout}\n{proc.stderr}"
        raw_metric = extract_metric(output_text, metric_pattern)
        return_code = proc.returncode
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        raw_metric = math.nan
        return_code = 124
        output_text = f"{exc.stdout or ''}\n{exc.stderr or ''}"
    except Exception:
        raw_metric = math.nan
        return_code = 1
        output_text = ""

    duration_sec = time.perf_counter() - start

    if not math.isfinite(raw_metric):
        objective = math.inf
    else:
        objective = -raw_metric if mode == "maximize" else raw_metric

    if return_code != 0:
        metric_source = "failed-command"

    return raw_metric, objective, return_code, duration_sec, timed_out, metric_source


def write_jsonl(path: Path, payload: Dict[str, object]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload) + "\n")


def run(args) -> int:
    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)
    trial_log = results_dir / "trials.jsonl"
    best_path = results_dir / "best.json"

    metric_pattern = re.compile(args.metric_regex)
    search_space = build_search_space(args)

    trial_counter = {"index": 0}

    def objective(point: List[object]) -> float:
        env_patch = sanitize_params(point, args)
        trial_index = trial_counter["index"]
        trial_counter["index"] += 1

        raw_metric, objective_value, return_code, duration_sec, timed_out, metric_source = evaluate_trial(
            command=args.command,
            metric_pattern=metric_pattern,
            mode=args.mode,
            timeout_sec=args.timeout_sec,
            env_patch=env_patch,
        )

        if return_code != 0 or not math.isfinite(objective_value):
            objective_value = FAILURE_PENALTY

        result = TrialResult(
            trial_index=trial_index,
            params=env_patch,
            score_raw=raw_metric if math.isfinite(raw_metric) else float("nan"),
            objective=objective_value,
            mode=args.mode,
            return_code=return_code,
            duration_sec=duration_sec,
            timed_out=timed_out,
            metric_source=metric_source,
        )

        write_jsonl(trial_log, asdict(result))
        print(
            f"trial={trial_index} objective={objective_value:.6g} raw={raw_metric:.6g} rc={return_code} env={env_patch}",
            flush=True,
        )
        return objective_value

    result = gp_minimize(
        func=objective,
        dimensions=search_space,
        n_calls=args.trials,
        n_initial_points=min(args.initial_points, args.trials),
        random_state=args.seed,
        acq_func="EI",
    )

    best_env = sanitize_params(result.x, args)
    best_objective = float(result.fun)
    best_raw_metric = -best_objective if args.mode == "maximize" else best_objective

    summary = {
        "best_params": best_env,
        "best_objective": best_objective,
        "best_metric_estimate": best_raw_metric,
        "mode": args.mode,
        "n_calls": len(result.func_vals),
        "results_dir": str(results_dir),
    }

    with best_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)

    print("Optimization complete")
    print(json.dumps(summary, indent=2))
    return 0
