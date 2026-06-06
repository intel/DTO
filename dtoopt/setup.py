# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT

from setuptools import setup
from pathlib import Path


setup(
    name="dtoopt",
    version="0.1.0",
    description="Bayesian optimization tool for DTO tuning",
    long_description=Path("README.md").read_text(encoding="utf-8"),
    long_description_content_type="text/markdown",
    python_requires=">=3.8",
    install_requires=["scikit-optimize"],
    package_dir={"dtoopt": "."},
    packages=["dtoopt"],
    entry_points={
        "console_scripts": [
            "dtoopt=dtoopt.__main__:main",
        ]
    },
)
