# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT

from dtoopt.cli import parse_args
from dtoopt.core import run


def main() -> int:
    args = parse_args()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
