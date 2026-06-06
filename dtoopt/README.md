# dtoopt Directory Guide

This folder contains the standalone DTO parameter optimization tool.

## Source files

- `__init__.py`  
  Marks `dtoopt` as a Python package.

- `__main__.py`  
  Entrypoint for module execution (`python -m dtoopt`) and console script handoff.

- `cli.py`  
  Defines and parses command-line arguments (search ranges, trials, mode, regex, output paths, etc.).

- `core.py`  
  Implements optimization logic: search space setup, trial execution, metric extraction, failure penalty handling, and output artifact writing.

- `pyproject.toml` and `setup.py`
  Packaging/build metadata for installing `dtoopt` via `pip install ./dtoopt`.


## Typical outputs (for normal usage)

During regular runs, the optimizer writes results to the directory you pass via `--results-dir`, with:

- `trials.jsonl`: one JSON record per trial
- `best.json`: best parameter set and objective summary
