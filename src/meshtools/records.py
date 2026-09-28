"""Typed parsing of the gateway's JSON Lines stream.

The gateway prints one JSON object per line (schema in ``docs/PROTOCOL.md``,
section "Gateway JSON Lines"). On real hardware those lines share the UART
with ESP-IDF log output, so the parser silently skips anything that is not a
JSON object and counts malformed lines instead of failing.
"""

from __future__ import annotations

import json
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Record:
    """Fields common to every record."""

    ts: int
    type: str


@dataclass(frozen=True)
class FrameRecord(Record):
    """Record produced by a frame from a node (data, hb, dup, join)."""

    node: int
    seq: int
    att: int
    rssi: int


@dataclass(frozen=True)
class DataRecord(FrameRecord):
    """A reading accepted by the gateway."""

    gap: int
    seq_state: str
    up_ms: int
    rtt_us: int
    status: int
    backend: tuple[str, ...]
    values: dict[str, float] = field(default_factory=dict)


@dataclass(frozen=True)
class HeartbeatRecord(FrameRecord):
    """Node health telemetry."""

    gap: int
    seq_state: str
    up_s: int
    heap: int
    min_heap: int
    tx_ok: int
    retries: int
    tx_fail: int
    q_drops: int
    rtt_avg_us: int
    rtt_max_us: int
    ack_rssi: int
    hwm: tuple[int, ...]


@dataclass(frozen=True)
class DupRecord(FrameRecord):
    """A retransmission of an already accepted frame."""


@dataclass(frozen=True)
class JoinRecord(FrameRecord):
    """A (re)join announcement and the gateway's decision."""

    mac: str
    gap: int
    status: str
    new: bool
    fw: str
    boot: int
    report_ms: int
    hb_ms: int


@dataclass(frozen=True)
class NodeStateRecord(Record):
    """Liveness transition of a node."""

    node: int
    from_state: str
    to_state: str
    silent_ms: int


@dataclass(frozen=True)
class LinkRecord(Record):
    """Periodic per-node summary computed by the gateway's sequence tracker."""

    node: int
    state: str
    rx: int
    lost: int
    dup: int
    late: int
    resets: int
    loss_ppm: int
    rx_err: int
    rssi: int


@dataclass(frozen=True)
class GwStatsRecord(Record):
    """Periodic gateway statistics."""

    up_s: int
    rx: int
    rx_err: int
    acks: int
    busy: int
    dups: int
    out_q_drops: int
    out_q_peak: int
    heap: int
    nodes: dict[str, int]
    hwm: dict[str, int]


@dataclass(frozen=True)
class ConfigRecord(Record):
    """Outcome of a remote configuration command."""

    node: int
    key: str
    value: int
    result: str
    attempts: int


@dataclass(frozen=True)
class RxErrorRecord(Record):
    """A frame the gateway rejected."""

    err: str
    mac: str
    node: int | None


@dataclass(frozen=True)
class GwStateRecord(Record):
    """Gateway lifecycle transition."""

    from_state: str
    to_state: str
    event: str


@dataclass(frozen=True)
class BootRecord(Record):
    """Gateway boot banner."""

    fw: str
    mac: str
    channel: int
    encryption: bool


@dataclass(frozen=True)
class InfoRecord(Record):
    """Free-text information line."""

    msg: str


AnyRecord = (
    DataRecord
    | HeartbeatRecord
    | DupRecord
    | JoinRecord
    | NodeStateRecord
    | LinkRecord
    | GwStatsRecord
    | ConfigRecord
    | RxErrorRecord
    | GwStateRecord
    | BootRecord
    | InfoRecord
)


def _frame_common(d: dict[str, Any]) -> dict[str, Any]:
    return {
        "ts": int(d["ts"]),
        "type": str(d["type"]),
        "node": int(d["node"]),
        "seq": int(d["seq"]),
        "att": int(d["att"]),
        "rssi": int(d["rssi"]),
    }


def _build(d: dict[str, Any]) -> AnyRecord | None:  # noqa: PLR0911 - one branch per record type
    kind = d.get("type")
    if kind == "data":
        return DataRecord(
            **_frame_common(d),
            gap=int(d["gap"]),
            seq_state=str(d["seq_state"]),
            up_ms=int(d["up_ms"]),
            rtt_us=int(d["rtt_us"]),
            status=int(d["status"]),
            backend=tuple(str(b) for b in d["backend"]),
            values={str(k): float(v) for k, v in d["values"].items()},
        )
    if kind == "hb":
        return HeartbeatRecord(
            **_frame_common(d),
            gap=int(d["gap"]),
            seq_state=str(d["seq_state"]),
            up_s=int(d["up_s"]),
            heap=int(d["heap"]),
            min_heap=int(d["min_heap"]),
            tx_ok=int(d["tx_ok"]),
            retries=int(d["retries"]),
            tx_fail=int(d["tx_fail"]),
            q_drops=int(d["q_drops"]),
            rtt_avg_us=int(d["rtt_avg_us"]),
            rtt_max_us=int(d["rtt_max_us"]),
            ack_rssi=int(d["ack_rssi"]),
            hwm=tuple(int(x) for x in d["hwm"]),
        )
    if kind == "dup":
        return DupRecord(**_frame_common(d))
    if kind == "join":
        return JoinRecord(
            **_frame_common(d),
            mac=str(d["mac"]),
            gap=int(d.get("gap", 0)),
            status=str(d["status"]),
            new=bool(d["new"]),
            fw=str(d["fw"]),
            boot=int(d["boot"]),
            report_ms=int(d["report_ms"]),
            hb_ms=int(d["hb_ms"]),
        )
    if kind == "node_state":
        return NodeStateRecord(
            ts=int(d["ts"]),
            type="node_state",
            node=int(d["node"]),
            from_state=str(d["from"]),
            to_state=str(d["to"]),
            silent_ms=int(d["silent_ms"]),
        )
    if kind == "link":
        return LinkRecord(
            ts=int(d["ts"]),
            type="link",
            node=int(d["node"]),
            state=str(d["state"]),
            rx=int(d["rx"]),
            lost=int(d["lost"]),
            dup=int(d["dup"]),
            late=int(d["late"]),
            resets=int(d["resets"]),
            loss_ppm=int(d["loss_ppm"]),
            rx_err=int(d["rx_err"]),
            rssi=int(d["rssi"]),
        )
    if kind == "gw_stats":
        return GwStatsRecord(
            ts=int(d["ts"]),
            type="gw_stats",
            up_s=int(d["up_s"]),
            rx=int(d["rx"]),
            rx_err=int(d["rx_err"]),
            acks=int(d["acks"]),
            busy=int(d["busy"]),
            dups=int(d["dups"]),
            out_q_drops=int(d["out_q_drops"]),
            out_q_peak=int(d["out_q_peak"]),
            heap=int(d["heap"]),
            nodes={str(k): int(v) for k, v in d["nodes"].items()},
            hwm={str(k): int(v) for k, v in d["hwm"].items()},
        )
    if kind == "config":
        return ConfigRecord(
            ts=int(d["ts"]),
            type="config",
            node=int(d["node"]),
            key=str(d["key"]),
            value=int(d["value"]),
            result=str(d["result"]),
            attempts=int(d["attempts"]),
        )
    if kind == "rx_error":
        return RxErrorRecord(
            ts=int(d["ts"]),
            type="rx_error",
            err=str(d["err"]),
            mac=str(d["mac"]),
            node=int(d["node"]) if "node" in d else None,
        )
    if kind == "gw_state":
        return GwStateRecord(
            ts=int(d["ts"]),
            type="gw_state",
            from_state=str(d["from"]),
            to_state=str(d["to"]),
            event=str(d["event"]),
        )
    if kind == "boot":
        return BootRecord(
            ts=int(d["ts"]),
            type="boot",
            fw=str(d["fw"]),
            mac=str(d["mac"]),
            channel=int(d["channel"]),
            encryption=bool(d["encryption"]),
        )
    if kind == "info":
        return InfoRecord(ts=int(d["ts"]), type="info", msg=str(d["msg"]))
    return None


@dataclass
class ParseStats:
    """Counters describing what the parser saw."""

    lines: int = 0
    records: int = 0
    non_json: int = 0
    malformed: int = 0
    unknown_type: int = 0


def parse_line(line: str, stats: ParseStats | None = None) -> AnyRecord | None:
    """Parse one line of gateway output.

    Args:
        line: A line of text (may be ESP-IDF log output, which is ignored).
        stats: Optional counters updated in place.

    Returns:
        The typed record, or ``None`` for lines that are not gateway records.
    """
    st = stats if stats is not None else ParseStats()
    st.lines += 1
    text = line.strip()
    if not text.startswith("{"):
        st.non_json += 1
        return None
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        st.malformed += 1
        return None
    if not isinstance(obj, dict):
        st.malformed += 1
        return None
    try:
        rec = _build(obj)
    except (KeyError, TypeError, ValueError, AttributeError):
        st.malformed += 1
        return None
    if rec is None:
        st.unknown_type += 1
        return None
    st.records += 1
    return rec


def iter_records(lines: Iterable[str], stats: ParseStats | None = None) -> Iterator[AnyRecord]:
    """Yield records from an iterable of lines, skipping non-record lines."""
    for line in lines:
        rec = parse_line(line, stats)
        if rec is not None:
            yield rec


def read_log(path: str | Path, stats: ParseStats | None = None) -> list[AnyRecord]:
    """Read a whole log file (firmware capture or simulator output)."""
    with Path(path).open(encoding="utf-8", errors="replace") as fh:
        return list(iter_records(fh, stats))
