"""End-to-end: simulator output -> metrics must agree with the gateway's own tracker and the ground truth."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from meshtools.cli import build_parser, main
from meshtools.metrics import compute
from meshtools.records import read_log
from meshtools.report import (
    cross_checks,
    load_truth,
    parse_gateway_stacks,
    parse_node_stacks,
    stack_table,
)

ROOT = Path(__file__).resolve().parents[1]


def test_simulator_run_passes_every_consistency_check(sim_run: tuple[Path, Path]) -> None:
    log, truth_path = sim_run
    mm, _ = compute(read_log(log))
    truth = load_truth(truth_path)
    checks = cross_checks(mm, truth)
    assert len(checks) >= 3 * len(mm.nodes)
    failed = [c for c in checks if not c.ok]
    assert not failed, failed


def test_simulator_is_deterministic(sim_run: tuple[Path, Path], tmp_path: Path) -> None:
    import subprocess  # noqa: PLC0415

    from tests.conftest import SIM_BIN  # noqa: PLC0415

    log, _ = sim_run
    again = tmp_path / "again.jsonl"
    subprocess.run(
        [
            str(SIM_BIN),
            "--duration",
            "180",
            "--seed",
            "7",
            "--out",
            str(again),
            "--truth",
            str(tmp_path / "t.json"),
        ],
        check=True,
        capture_output=True,
    )
    assert again.read_bytes() == log.read_bytes()


def test_every_simulator_line_is_valid_json(sim_run: tuple[Path, Path]) -> None:
    log, _ = sim_run
    for line in log.read_text().splitlines():
        json.loads(line)


def test_cli_report_writes_markdown_and_figures(sim_run: tuple[Path, Path], tmp_path: Path) -> None:
    log, truth = sim_run
    md = tmp_path / "r.md"
    rc = main(
        [
            "report",
            str(log),
            "--truth",
            str(truth),
            "--markdown",
            str(md),
            "--figures",
            str(tmp_path),
            "--strict",
        ]
    )
    assert rc == 0
    text = md.read_text()
    assert "### Mesh-wide" in text
    assert "FAIL" not in text
    for name in ("activity_heatmap.png", "node_states.png", "loss_retries.png", "latency_cdf.png"):
        assert (tmp_path / name).stat().st_size > 5000


def test_stack_report_from_qemu_excerpts(data_dir: Path) -> None:
    node = parse_node_stacks((data_dir / "qemu_node_excerpt.log").read_text())
    by_task = {s.task: s for s in node}
    assert set(by_task) == {"sensor", "tx", "heartbeat", "supervisor"}
    assert by_task["tx"].min_free == 1208
    assert by_task["tx"].peak_used == 4096 - 1208
    gw = parse_gateway_stacks((data_dir / "qemu_gw_excerpt.log").read_text(), {"rx": 4096})
    assert {s.task for s in gw} == {"rx", "liveness", "out", "cmd", "supervisor"}
    table = stack_table(node + gw)
    assert table.count("\n") == len(node) + len(gw) + 1


def test_cli_stack_command(data_dir: Path, tmp_path: Path) -> None:
    md = tmp_path / "s.md"
    rc = main(
        [
            "stack",
            "--node-log",
            str(data_dir / "qemu_node_excerpt.log"),
            "--gateway-log",
            str(data_dir / "qemu_gw_excerpt.log"),
            "--gateway-src",
            str(ROOT / "gateway/main/gw_tasks.c"),
            "--markdown",
            str(md),
        ]
    )
    assert rc == 0
    assert "| gateway | rx | 4096 |" in md.read_text()


def test_parser_requires_subcommand() -> None:
    with pytest.raises(SystemExit):
        build_parser().parse_args([])
