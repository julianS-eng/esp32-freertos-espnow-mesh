"""Markdown report, ground-truth validation and QEMU stack report.

Everything the README quotes is produced here from an actual run: the
simulator log, its ground-truth file, and QEMU console captures.
"""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from meshtools.metrics import MeshMetrics, percentile, summarize


def _f(v: float, d: int = 2) -> str:
    return "n/a" if math.isnan(v) else f"{v:.{d}f}"


def summary_table(mm: MeshMetrics) -> str:
    """Mesh-wide headline metrics as a Markdown table."""
    s = summarize(mm)
    rows = [
        ("Simulated duration", f"{s['duration_s']:.0f} s"),
        ("Nodes", f"{s['nodes']}"),
        ("Unique frames accepted by the gateway", f"{s['frames_accepted']}"),
        ("Frames lost after all retries", f"{s['frames_lost']} ({_f(float(s['loss_pct']), 3)} %)"),
        ("Frames that needed at least one retry", f"{_f(float(s['retried_pct']))} %"),
        ("Duplicates suppressed (lost ACKs)", f"{s['duplicates']}"),
        (
            "Link RTT p50 / p95 / p99 / max",
            "{} / {} / {} / {} ms".format(
                *(_f(float(s[k])) for k in ("rtt_p50_ms", "rtt_p95_ms", "rtt_p99_ms", "rtt_max_ms"))
            ),
        ),
        (
            "Delivery delay above best case p50 / p95 / p99 / max",
            "{} / {} / {} / {} ms".format(
                *(
                    _f(float(s[k]), 0)
                    for k in ("delay_p50_ms", "delay_p95_ms", "delay_p99_ms", "delay_max_ms")
                )
            ),
        ),
    ]
    out = ["| Metric | Value |", "|---|---|"]
    out += [f"| {k} | {v} |" for k, v in rows]
    return "\n".join(out)


def node_table(mm: MeshMetrics) -> str:
    """Per-node metrics as a Markdown table."""
    end = mm.last_ts
    out = [
        "| Node | Data | HB | Lost (gaps) | Loss % | Retried % | Dups | Node: abandoned / queue drops "
        "| RTT p50 ms | RTT p95 ms | Delay p95 ms | RSSI avg dBm | Availability % | Transitions |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for nid in sorted(mm.nodes):
        nm = mm.nodes[nid]
        frames = nm.data + nm.heartbeats
        rtt = [v / 1000.0 for v in nm.rtt_us]
        rssi = sum(nm.rssi) / len(nm.rssi) if nm.rssi else float("nan")
        out.append(
            f"| {nid} | {nm.data} | {nm.heartbeats} | {nm.lost} | {_f(100 * nm.loss_ratio, 3)} | "
            f"{_f(100 * nm.retried_ok / frames if frames else float('nan'), 1)} | {nm.dups} | "
            f"{f'{nm.last_hb.tx_fail} / {nm.last_hb.q_drops}' if nm.last_hb else 'n/a'} | "
            f"{_f(percentile(rtt, 50))} | {_f(percentile(rtt, 95))} | "
            f"{_f(percentile([float(x) for x in nm.delay_ms], 95), 0)} | {_f(rssi, 1)} | "
            f"{_f(100 * nm.availability(end), 1)} | {len(nm.transitions)} |"
        )
    return "\n".join(out)


@dataclass(frozen=True)
class Check:
    """One consistency check between two independent computations."""

    name: str
    expected: int
    actual: int

    @property
    def ok(self) -> bool:
        """True if both computations agree."""
        return self.expected == self.actual


def cross_checks(mm: MeshMetrics, truth: dict[str, Any] | None = None) -> list[Check]:
    """Validate the dashboard's reconstruction against independent sources.

    * Per node, the loss/duplicate/late counters rebuilt from the per-frame
      records must equal the gateway's own sequence tracker (final ``link``
      record). Node ACK frames are invisible in the log, so nodes that received
      CONFIG commands are compared on ``lost``/``dup`` only.
    * With a simulator ground-truth file: the gateway-side duplicate count
      must match, and the unique DATA frames seen by the dashboard must lie
      between what the nodes had acknowledged and what they had sent.
    """
    checks: list[Check] = []
    config_nodes = {c.node for c in mm.configs}
    for nid in sorted(mm.nodes):
        nm = mm.nodes[nid]
        if nm.last_link is None:
            continue
        link = nm.last_link
        checks.append(Check(f"node {nid} lost == gateway tracker", link.lost, nm.epoch.lost))
        checks.append(Check(f"node {nid} dup == gateway tracker", link.dup, nm.epoch.dup))
        checks.append(Check(f"node {nid} late == gateway tracker", link.late, nm.epoch.late))
        if nid not in config_nodes:
            checks.append(Check(f"node {nid} rx == gateway tracker", link.rx, nm.epoch.rx))
    if truth is not None:
        gw = truth["gateway"]
        checks.append(
            Check(
                "duplicates == simulator ground truth",
                int(gw["duplicates"]),
                sum(n.dups for n in mm.nodes.values()),
            )
        )
        for t in truth["per_node"]:
            seen = mm.nodes.get(int(t["node"]))
            data = seen.data if seen else 0
            acked, sent = int(t["data_acked"]), int(t["data_sent"])
            inside = acked <= data <= sent
            checks.append(Check(f"node {t['node']} acked <= data seen <= sent", 1, 1 if inside else 0))
    return checks


def checks_table(checks: list[Check]) -> str:
    """Render consistency checks as Markdown."""
    out = ["| Check | Expected | Dashboard | Result |", "|---|---:|---:|---|"]
    out += [f"| {c.name} | {c.expected} | {c.actual} | {'pass' if c.ok else 'FAIL'} |" for c in checks]
    return "\n".join(out)


def truth_table(truth: dict[str, Any]) -> str:
    """Node-side ground truth from the simulator as Markdown."""
    out = [
        "| Node | Channel loss (good/bad) | Readings | Queue drops | DATA sent | DATA acked | DATA abandoned "
        "| Radio attempts | Re-joins | Boots |",
        "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for t in truth["per_node"]:
        out.append(
            f"| {t['node']} | {100 * t['loss_good']:.1f} % / {100 * t['loss_bad']:.0f} % | {t['readings']} | "
            f"{t['queue_drops']} | {t['data_sent']} | {t['data_acked']} | {t['data_failed']} | "
            f"{t['attempts']} | {t['rejoins']} | {t['boots']} |"
        )
    return "\n".join(out)


# --------------------------------------------------------------------------- #
# QEMU stack report                                                           #
# --------------------------------------------------------------------------- #

_NODE_STACK = re.compile(r"stack task=(\w+) size=(\d+) min_free=(\d+)")


@dataclass(frozen=True)
class StackUsage:
    """Stack measurement of one task (bytes)."""

    firmware: str
    task: str
    size: int
    min_free: int
    samples: int

    @property
    def peak_used(self) -> int:
        """Peak bytes used."""
        return self.size - self.min_free

    @property
    def headroom_pct(self) -> float:
        """Unused fraction of the stack at the peak."""
        return 100.0 * self.min_free / self.size if self.size else 0.0


def parse_node_stacks(text: str) -> list[StackUsage]:
    """Minimum free stack per task from the node's ``stack task=...`` log lines."""
    agg: dict[str, tuple[int, int, int]] = {}
    for m in _NODE_STACK.finditer(text):
        task, size, free = m.group(1), int(m.group(2)), int(m.group(3))
        prev = agg.get(task)
        agg[task] = (size, min(free, prev[1]) if prev else free, (prev[2] if prev else 0) + 1)
    return [StackUsage("sensor_node", t, s, f, n) for t, (s, f, n) in agg.items()]


def parse_gateway_stacks(text: str, sizes: dict[str, int]) -> list[StackUsage]:
    """Minimum free stack per task from the gateway's ``gw_stats`` records.

    Args:
        text: Gateway console capture.
        sizes: Configured stack size per task (from ``gw_tasks.c``).
    """
    agg: dict[str, tuple[int, int]] = {}
    for line in text.splitlines():
        if '"type":"gw_stats"' not in line:
            continue
        try:
            hwm = json.loads(line)["hwm"]
        except (json.JSONDecodeError, KeyError):
            continue
        for task, free in hwm.items():
            prev = agg.get(task)
            agg[task] = (min(int(free), prev[0]) if prev else int(free), (prev[1] if prev else 0) + 1)
    return [StackUsage("gateway", t, sizes.get(t, 0), f, n) for t, (f, n) in agg.items()]


def stack_table(rows: list[StackUsage]) -> str:
    """Render stack measurements as Markdown."""
    out = [
        "| Firmware | Task | Stack (B) | Min free (B) | Peak used (B) | Head-room | Samples |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    out += [
        f"| {r.firmware} | {r.task} | {r.size} | {r.min_free} | {r.peak_used} | {r.headroom_pct:.0f} % | "
        f"{r.samples} |"
        for r in rows
    ]
    return "\n".join(out)


def load_truth(path: str | Path) -> dict[str, Any]:
    """Load a simulator ground-truth JSON file."""
    data: dict[str, Any] = json.loads(Path(path).read_text(encoding="utf-8"))
    return data
