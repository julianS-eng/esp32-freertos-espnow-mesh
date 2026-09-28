"""Terminal dashboard rendering and figure generation."""

from __future__ import annotations

import io
from pathlib import Path

from rich.console import Console

from meshtools import plots
from meshtools.dashboard import DashboardState, describe_event, render, replay_lines, sparkline
from meshtools.metrics import compute
from meshtools.records import read_log


def _render_text(state: DashboardState) -> str:
    console = Console(record=True, width=200, file=io.StringIO())
    console.print(render(state))
    return console.export_text()


def test_sparkline_scales() -> None:
    assert sparkline([0, 0, 0]) == "   "
    assert sparkline([0, 4, 8]) == " ▄█"
    assert sparkline([]) == ""


def test_dashboard_renders_nodes_and_events(data_dir: Path) -> None:
    state = DashboardState()
    for line in (data_dir / "gateway_sample.jsonl").read_text().splitlines():
        state.feed_line(line)
    text = _render_text(state)
    assert "gateway: running" in text
    assert "JOIN accepted" in text
    for node in range(1, 7):
        assert f"node {node} JOIN" in text
    assert state.parse.non_json == 1


def test_empty_dashboard_renders() -> None:
    text = _render_text(DashboardState())
    assert "no events yet" in text
    assert "waiting for first gw_stats" in text


def test_replay_without_delay_yields_every_line(data_dir: Path) -> None:
    with (data_dir / "gateway_sample.jsonl").open() as fh:
        lines = list(replay_lines(fh, speed=0))
    assert len(lines) == 34


def test_describe_event_ignores_plain_data(data_dir: Path) -> None:
    recs = read_log(data_dir / "gateway_sample.jsonl")
    descs = [describe_event(r) for r in recs]
    assert any(d and "gateway boot -> load_registry" in d for d in descs)


def test_figures_and_gif(sim_run: tuple[Path, Path], tmp_path: Path) -> None:
    log, _ = sim_run
    mm, act = compute(read_log(log))
    gif = plots.replay_gif(mm, act, tmp_path / "replay.gif", frames=4, fps=2)
    assert gif.stat().st_size < 5 * 1024 * 1024
    assert plots.node_color(0) == "#2a78d6"


def test_node_color_refuses_to_cycle() -> None:
    import pytest  # noqa: PLC0415

    with pytest.raises(ValueError, match="facet"):
        plots.node_color(8)
