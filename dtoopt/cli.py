# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT

import argparse


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bayesian optimization tool for DTO parameters using skopt."
    )
    parser.add_argument(
        "--command",
        required=True,
        help="Command to execute for each trial (quote as a single string).",
    )
    parser.add_argument(
        "--metric-regex",
        default=r"DTO\s+Run\s+Time:\s*([0-9]+(?:\.[0-9]+)?)\s*ms",
        help=(
            "Regex with one capturing group to extract numeric metric from command output. "
            "Default captures DTO runtime in milliseconds from lines like: 'DTO Run Time: 22826 ms'."
        ),
    )
    parser.add_argument(
        "--mode",
        choices=["maximize", "minimize"],
        default="minimize",
        help="Whether the extracted metric should be maximized or minimized.",
    )
    parser.add_argument(
        "--trials",
        type=int,
        default=30,
        help="Total optimization trials.",
    )
    parser.add_argument(
        "--initial-points",
        type=int,
        default=10,
        help="Number of initial random evaluations.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for reproducibility.",
    )
    parser.add_argument(
        "--timeout-sec",
        type=int,
        default=600,
        help="Per-trial command timeout in seconds.",
    )
    parser.add_argument(
        "--results-dir",
        required=True,
        help="Directory for artifacts: trial log and best config.",
    )

    parser.add_argument(
        "--min-bytes-low",
        type=int,
        default=4096,
        help="Lower bound for DTO_MIN_BYTES search range (inclusive).",
    )
    parser.add_argument(
        "--min-bytes-high",
        type=int,
        default=262144,
        help="Upper bound for DTO_MIN_BYTES search range (inclusive).",
    )
    parser.add_argument(
        "--min-bytes-step",
        type=int,
        default=1024,
        help="Step size used to snap DTO_MIN_BYTES trial values.",
    )
    parser.add_argument(
        "--cpu-fraction-low",
        type=float,
        default=0.0,
        help="Lower bound for DTO_CPU_SIZE_FRACTION search range (inclusive).",
    )
    parser.add_argument(
        "--cpu-fraction-high",
        type=float,
        default=1.0,
        help="Upper bound for DTO_CPU_SIZE_FRACTION search range (inclusive).",
    )
    parser.add_argument(
        "--cpu-fraction-step",
        type=float,
        default=0.05,
        help="Step size used to snap DTO_CPU_SIZE_FRACTION trial values.",
    )

    return parser.parse_args()
