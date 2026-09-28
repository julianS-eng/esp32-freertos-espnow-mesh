"""Shared fixtures: paths to committed test data and to a fresh simulator run."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DATA = Path(__file__).resolve().parent / "data"
SIM_BIN = ROOT / "host" / "build" / "mesh_sim"


@pytest.fixture(scope="session")
def data_dir() -> Path:
    return DATA


@pytest.fixture(scope="session")
def sim_run(tmp_path_factory: pytest.TempPathFactory) -> tuple[Path, Path]:
    """Run the C simulator (built by `cmake --build host/build`) for 180 s; skip if not built."""
    if not SIM_BIN.exists() or shutil.which(str(SIM_BIN)) is None:
        pytest.skip("host/build/mesh_sim not built (cmake -S host -B host/build && cmake --build host/build)")
    out = tmp_path_factory.mktemp("sim")
    log, truth = out / "sim.jsonl", out / "truth.json"
    subprocess.run(
        [str(SIM_BIN), "--duration", "180", "--seed", "7", "--out", str(log), "--truth", str(truth)],
        check=True,
        capture_output=True,
    )
    return log, truth
