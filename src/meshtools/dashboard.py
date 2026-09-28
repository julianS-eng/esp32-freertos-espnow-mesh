"""Live terminal dashboard (rich) for the gateway's JSON Lines stream.

Sources: a serial port (real gateway), a file being appended to (``--follow``)
or a finished log replayed at an accelerated pace. Rendering is a pure
function of the metrics so it is unit-testable without a terminal.
"""

from __future__ import annotations

import math
import time
from collections import deque
from collections.abc import Iterator
from pathlib import Path
from typing import TextIO

from rich.console import Group, RenderableType
from rich.live import Live
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from meshtools.metrics import MeshMetrics, percentile
from meshtools.records import (
    AnyRecord,
    ConfigRecord,
    GwStateRecord,
    JoinRecord,
    NodeStateRecord,
    ParseStats,
    RxErrorRecord,
    parse_line,
)

SPARK = " ▁▂▃▄▅▆▇█"
STATE_STYLE = {"online": "bold green", "suspect": "bold yellow", "offline": "bold red", "unknown": "dim"}
SPARK_BINS = 12
SPARK_BIN_MS = 5000


def sparkline(values: list[int]) -> str:
    """Unicode sparkline scaled to the maximum of ``values``."""
    top = max(values) if values else 0
    if top <= 0:
        return SPARK[0] * len(values)
    return "".join(SPARK[min(len(SPARK) - 1, round(v / top * (len(SPARK) - 1)))] for v in values)


def describe_event(rec: AnyRecord) -> str | None:
    """One-line human description of a notable record, or ``None``."""
    t = f"{rec.ts / 1000.0:8.1f}s"
    if isinstance(rec, NodeStateRecord):
        return (
            f"{t}  node {rec.node}: {rec.from_state} -> {rec.to_state} (silent {rec.silent_ms / 1000:.1f} s)"
        )
    if isinstance(rec, JoinRecord):
        return f"{t}  node {rec.node} JOIN {rec.status} (boot #{rec.boot}, fw {rec.fw}, {rec.mac})"
    if isinstance(rec, ConfigRecord):
        return (
            f"{t}  CONFIG node {rec.node} {rec.key}={rec.value}: {rec.result} after {rec.attempts} attempt(s)"
        )
    if isinstance(rec, GwStateRecord):
        return f"{t}  gateway {rec.from_state} -> {rec.to_state} ({rec.event})"
    if isinstance(rec, RxErrorRecord):
        return f"{t}  rx error {rec.err} from {rec.mac}"
    return None


class DashboardState:
    """Incremental state behind the dashboard."""

    def __init__(self, max_events: int = 10) -> None:
        """Create an empty state keeping the last ``max_events`` notable events."""
        self.metrics = MeshMetrics()
        self.events: deque[str] = deque(maxlen=max_events)
        self.recent: dict[int, deque[int]] = {}
        self.parse = ParseStats()
        self.gw_state = "?"

    def feed(self, rec: AnyRecord) -> None:
        """Fold one record into the state."""
        self.metrics.add(rec)
        desc = describe_event(rec)
        if desc is not None:
            self.events.append(desc)
        if isinstance(rec, GwStateRecord):
            self.gw_state = rec.to_state
        node = getattr(rec, "node", None)
        if rec.type in ("data", "hb") and isinstance(node, int):
            self.recent.setdefault(node, deque(maxlen=4096)).append(rec.ts)

    def feed_line(self, line: str) -> None:
        """Parse and fold a raw line (non-record lines are counted and ignored)."""
        rec = parse_line(line, self.parse)
        if rec is not None:
            self.feed(rec)

    def activity(self, node: int) -> list[int]:
        """Frames per 5 s over the last minute for ``node``."""
        now = self.metrics.last_ts
        bins = [0] * SPARK_BINS
        for ts in self.recent.get(node, ()):
            age = now - ts
            if 0 <= age < SPARK_BINS * SPARK_BIN_MS:
                bins[SPARK_BINS - 1 - age // SPARK_BIN_MS] += 1
        return bins


def _fmt(v: float, digits: int = 1) -> str:
    return "-" if math.isnan(v) else f"{v:.{digits}f}"


def render(state: DashboardState) -> RenderableType:
    """Build the full dashboard renderable from the current state."""
    mm = state.metrics
    rtt_all = [v / 1000.0 for v in mm.all_rtt_us()]
    header = Text.assemble(
        ("ESP-NOW mesh  ", "bold"),
        ("gateway: ", "dim"),
        (state.gw_state, "bold cyan"),
        ("   t = ", "dim"),
        (f"{mm.last_ts / 1000.0:.1f} s", "bold"),
        ("   frames: ", "dim"),
        (str(mm.total_accepted()), "bold"),
        ("   loss after retries: ", "dim"),
        (f"{100 * mm.loss_ratio():.2f} %", "bold"),
        ("   RTT p50/p95: ", "dim"),
        (f"{_fmt(percentile(rtt_all, 50), 2)}/{_fmt(percentile(rtt_all, 95), 2)} ms", "bold"),
    )
    table = Table(expand=True, header_style="bold", pad_edge=False)
    for col, justify in (
        ("node", "right"),
        ("state", "left"),
        ("data", "right"),
        ("hb", "right"),
        ("lost", "right"),
        ("loss %", "right"),
        ("dups", "right"),
        ("retried %", "right"),
        ("RTT p50 ms", "right"),
        ("RTT p95 ms", "right"),
        ("RSSI dBm", "right"),
        ("seen s ago", "right"),
        ("last 60 s", "left"),
    ):
        table.add_column(col, justify=justify)  # type: ignore[arg-type,unused-ignore]
    for nid in sorted(mm.nodes):
        nm = mm.nodes[nid]
        rtt = [v / 1000.0 for v in nm.rtt_us]
        frames = nm.data + nm.heartbeats
        rssi = sum(nm.rssi[-20:]) / len(nm.rssi[-20:]) if nm.rssi else float("nan")
        ago = (mm.last_ts - nm.last_ts) / 1000.0 if nm.last_ts is not None else float("nan")
        table.add_row(
            str(nid),
            Text(nm.state, style=STATE_STYLE.get(nm.state, "")),
            str(nm.data),
            str(nm.heartbeats),
            str(nm.lost),
            f"{100 * nm.loss_ratio:.2f}",
            str(nm.dups),
            f"{100 * nm.retried_ok / frames:.1f}" if frames else "-",
            _fmt(percentile(rtt, 50), 2),
            _fmt(percentile(rtt, 95), 2),
            _fmt(rssi, 0),
            _fmt(ago, 1),
            sparkline(state.activity(nid)),
        )
    events = Text("\n".join(state.events) if state.events else "(no events yet)", style="dim")
    st = mm.last_stats
    footer = Text(
        f"gateway rx={st.rx} rx_err={st.rx_err} acks={st.acks} busy={st.busy} dups={st.dups} "
        f"out_q_peak={st.out_q_peak} out_q_drops={st.out_q_drops}"
        if st
        else "gateway stats: waiting for first gw_stats record",
        style="dim",
    )
    return Group(
        Panel(header, border_style="cyan"),
        table,
        Panel(events, title="events", title_align="left", border_style="dim"),
        footer,
    )


def follow_file(path: Path, poll_s: float = 0.2) -> Iterator[str]:
    """Yield lines appended to ``path`` (like ``tail -f``), starting from the beginning."""
    with path.open(encoding="utf-8", errors="replace") as fh:
        while True:
            line = fh.readline()
            if line:
                yield line
            else:
                time.sleep(poll_s)


def replay_lines(fh: TextIO, speed: float) -> Iterator[str]:
    """Yield lines of a finished log, sleeping so that gateway time runs ``speed`` x faster."""
    last_ts: int | None = None
    for line in fh:
        rec = parse_line(line)
        if rec is not None and speed > 0:
            if last_ts is not None and rec.ts > last_ts:
                time.sleep((rec.ts - last_ts) / 1000.0 / speed)
            last_ts = rec.ts
        yield line


def serial_lines(port: str, baud: int) -> Iterator[str]:
    """Yield lines from the gateway's serial port (requires the ``serial`` extra)."""
    try:
        import serial  # noqa: PLC0415 - optional dependency
    except ImportError as exc:  # pragma: no cover - depends on the environment
        raise SystemExit("pyserial is required: pip install 'espnow-mesh-tools[serial]'") from exc
    with serial.Serial(port, baudrate=baud, timeout=1) as ser:
        while True:
            raw = ser.readline()
            if raw:
                yield raw.decode("utf-8", errors="replace")


def run(lines: Iterator[str], refresh_hz: float = 4.0, max_lines: int | None = None) -> DashboardState:
    """Drive the live dashboard from an iterator of lines until it is exhausted or interrupted."""
    state = DashboardState()
    with Live(render(state), refresh_per_second=refresh_hz, screen=False) as live:
        try:
            for i, line in enumerate(lines):
                state.feed_line(line)
                live.update(render(state))
                if max_lines is not None and i + 1 >= max_lines:
                    break
        except KeyboardInterrupt:
            pass
    return state
