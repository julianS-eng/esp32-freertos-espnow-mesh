"""Activity, packet-loss and latency metrics computed from gateway records.

Two latency views are provided, because node and gateway clocks are not
synchronised:

* **RTT** - measured by the node itself (send of the acknowledged attempt to
  ACK reception, Karn-safe) and piggy-backed on the next DATA frame
  (``rtt_us``). Pure link + gateway processing latency.
* **Delivery delay** - ``gateway_ts - node_uptime`` minus its minimum within
  one boot of the node. Because a retransmitted frame keeps its original
  ``up_ms``, this includes retry back-off: it is the one-way *delay above the
  best case* (one-way delay variation, RFC 5481), not an absolute latency.

Loss is taken from the gateway's sequence tracker as reported on every
frame (``gap``): frames that never reached the gateway even after all
retries. It is reconstructed per JOIN epoch exactly like the firmware does,
so it can be cross-checked against the gateway's periodic ``link`` records.
"""

from __future__ import annotations

import math
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field

from meshtools.records import (
    AnyRecord,
    ConfigRecord,
    DataRecord,
    DupRecord,
    GwStateRecord,
    GwStatsRecord,
    HeartbeatRecord,
    JoinRecord,
    LinkRecord,
    NodeStateRecord,
    RxErrorRecord,
)


def percentile(values: Sequence[float], q: float) -> float:
    """Linear-interpolation percentile (same definition as ``numpy.percentile``).

    Args:
        values: Samples (need not be sorted).
        q: Percentile in [0, 100].

    Returns:
        The percentile, or ``nan`` for an empty sequence.
    """
    if not values:
        return math.nan
    if not 0.0 <= q <= 100.0:
        raise ValueError("q must be within [0, 100]")
    s = sorted(values)
    pos = (len(s) - 1) * q / 100.0
    lo = math.floor(pos)
    hi = math.ceil(pos)
    return float(s[lo] + (s[hi] - s[lo]) * (pos - lo))


@dataclass
class Epoch:
    """Sequence accounting since the last accepted JOIN (mirrors ``mesh_seq_tracker_t``)."""

    rx: int = 0
    lost: int = 0
    dup: int = 0
    late: int = 0
    resets: int = 0


@dataclass
class NodeMetrics:
    """Everything known about one node."""

    node: int
    mac: str = ""
    data: int = 0
    heartbeats: int = 0
    joins: int = 0
    dups: int = 0
    late: int = 0
    lost: int = 0
    retried_ok: int = 0
    rx_errors: int = 0
    first_ts: int | None = None
    last_ts: int | None = None
    state: str = "unknown"
    epoch: Epoch = field(default_factory=Epoch)
    rtt_us: list[int] = field(default_factory=list)
    delay_ms: list[int] = field(default_factory=list)
    rssi: list[int] = field(default_factory=list)
    attempts: dict[int, int] = field(default_factory=dict)
    transitions: list[NodeStateRecord] = field(default_factory=list)
    last_hb: HeartbeatRecord | None = None
    last_link: LinkRecord | None = None
    boot: int | None = None
    offline_ms: int = 0
    _offline_since: int | None = None
    _seg_samples: list[tuple[int, int]] = field(default_factory=list)
    _seg_last_up: int = -1

    # --- derived -----------------------------------------------------------
    @property
    def accepted(self) -> int:
        """Unique frames accepted by the gateway (data + heartbeats + joins)."""
        return self.data + self.heartbeats + self.joins

    @property
    def loss_ratio(self) -> float:
        """Frames never received even after retries / frames expected."""
        expected = self.accepted + self.lost
        return self.lost / expected if expected else 0.0

    @property
    def node_delivery_ratio(self) -> float | None:
        """Delivery ratio as seen by the node (acked / (acked + abandoned)), from its last heartbeat."""
        if self.last_hb is None:
            return None
        total = self.last_hb.tx_ok + self.last_hb.tx_fail
        return self.last_hb.tx_ok / total if total else None

    def availability(self, end_ts: int) -> float:
        """Fraction of the observed time the node was not OFFLINE."""
        if self.first_ts is None:
            return 0.0
        span = max(1, end_ts - self.first_ts)
        off = self.offline_ms + (end_ts - self._offline_since if self._offline_since is not None else 0)
        return max(0.0, 1.0 - off / span)

    # --- accumulation --------------------------------------------------------
    def _touch(self, ts: int) -> None:
        if self.first_ts is None:
            self.first_ts = ts
        self.last_ts = ts

    def _delay_sample(self, ts: int, up_ms: int) -> None:
        if up_ms < self._seg_last_up:  # node rebooted: new clock offset
            self._flush_segment()
        self._seg_last_up = up_ms
        self._seg_samples.append((ts, up_ms))

    def _flush_segment(self) -> None:
        if self._seg_samples:
            offset = min(ts - up for ts, up in self._seg_samples)
            self.delay_ms.extend(ts - up - offset for ts, up in self._seg_samples)
        self._seg_samples = []
        self._seg_last_up = -1

    def finish(self) -> None:
        """Close the open delay segment (call once after the last record)."""
        self._flush_segment()

    def add_frame(self, rec: DataRecord | HeartbeatRecord) -> None:
        """Account an accepted DATA or HEARTBEAT frame."""
        self._touch(rec.ts)
        self.rssi.append(rec.rssi)
        self.attempts[rec.att] = self.attempts.get(rec.att, 0) + 1
        if rec.att > 1:
            self.retried_ok += 1
        if rec.seq_state == "late":
            self.late += 1
            self.lost = max(0, self.lost - 1)
            self.epoch.late += 1
            self.epoch.lost = max(0, self.epoch.lost - 1)
        else:
            self.lost += rec.gap
            self.epoch.lost += rec.gap
            if rec.seq_state == "reset":
                self.epoch.resets += 1
        self.epoch.rx += 1
        if isinstance(rec, DataRecord):
            self.data += 1
            if rec.rtt_us > 0:
                self.rtt_us.append(rec.rtt_us)
            self._delay_sample(rec.ts, rec.up_ms)
        else:
            self.heartbeats += 1
            self.last_hb = rec

    def add_join(self, rec: JoinRecord) -> None:
        """Account a JOIN (same rule as the gateway: a new epoch only after a reboot)."""
        self._touch(rec.ts)
        self.mac = rec.mac
        if rec.status != "accepted":
            return
        self.joins += 1
        if self.boot is None or rec.boot != self.boot:
            self.epoch = Epoch(rx=1)
        else:
            self.epoch.rx += 1
            self.epoch.lost += rec.gap
            self.lost += rec.gap
        self.boot = rec.boot

    def add_dup(self, rec: DupRecord) -> None:
        """Account a duplicate (retransmission after a lost ACK)."""
        self._touch(rec.ts)
        self.dups += 1
        self.epoch.dup += 1

    def add_state(self, rec: NodeStateRecord) -> None:
        """Track liveness transitions and accumulate offline time."""
        self.transitions.append(rec)
        self.state = rec.to_state
        if rec.to_state == "offline":
            self._offline_since = rec.ts
        elif rec.from_state == "offline" and self._offline_since is not None:
            self.offline_ms += rec.ts - self._offline_since
            self._offline_since = None


@dataclass
class MeshMetrics:
    """Aggregate view of a whole log."""

    nodes: dict[int, NodeMetrics] = field(default_factory=dict)
    first_ts: int | None = None
    last_ts: int = 0
    gw_states: list[GwStateRecord] = field(default_factory=list)
    last_stats: GwStatsRecord | None = None
    configs: list[ConfigRecord] = field(default_factory=list)
    rx_errors: dict[str, int] = field(default_factory=dict)

    def node(self, node_id: int) -> NodeMetrics:
        """Get or create the metrics of ``node_id``."""
        nm = self.nodes.get(node_id)
        if nm is None:
            nm = NodeMetrics(node=node_id)
            self.nodes[node_id] = nm
        return nm

    def add(self, rec: AnyRecord) -> None:
        """Fold one record into the metrics (usable incrementally by the live dashboard)."""
        if self.first_ts is None:
            self.first_ts = rec.ts
        self.last_ts = max(self.last_ts, rec.ts)
        if isinstance(rec, DataRecord | HeartbeatRecord):
            nm = self.node(rec.node)
            nm.add_frame(rec)
            if nm.state in ("unknown", "offline", "suspect"):
                nm.state = "online"
        elif isinstance(rec, JoinRecord):
            nm = self.node(rec.node)
            nm.add_join(rec)
            if rec.status == "accepted":
                nm.state = "online"
        elif isinstance(rec, DupRecord):
            self.node(rec.node).add_dup(rec)
        elif isinstance(rec, NodeStateRecord):
            self.node(rec.node).add_state(rec)
        elif isinstance(rec, LinkRecord):
            nm = self.node(rec.node)
            nm.last_link = rec
            nm.rx_errors = rec.rx_err
        elif isinstance(rec, GwStatsRecord):
            self.last_stats = rec
        elif isinstance(rec, GwStateRecord):
            self.gw_states.append(rec)
        elif isinstance(rec, ConfigRecord):
            self.configs.append(rec)
        elif isinstance(rec, RxErrorRecord):
            self.rx_errors[rec.err] = self.rx_errors.get(rec.err, 0) + 1

    def finish(self) -> MeshMetrics:
        """Finalize per-node derived series. Returns ``self`` for chaining."""
        for nm in self.nodes.values():
            nm.finish()
        return self

    # --- aggregates ----------------------------------------------------------
    @property
    def duration_s(self) -> float:
        """Observed span of the log in seconds."""
        if self.first_ts is None:
            return 0.0
        return max(0, self.last_ts - self.first_ts) / 1000.0

    def all_rtt_us(self) -> list[int]:
        """RTT samples of every node."""
        return [v for nm in self.nodes.values() for v in nm.rtt_us]

    def all_delay_ms(self) -> list[int]:
        """Delivery-delay samples of every node."""
        return [v for nm in self.nodes.values() for v in nm.delay_ms]

    def total_lost(self) -> int:
        """Frames lost after retries, all nodes."""
        return sum(nm.lost for nm in self.nodes.values())

    def total_accepted(self) -> int:
        """Unique frames accepted, all nodes."""
        return sum(nm.accepted for nm in self.nodes.values())

    def loss_ratio(self) -> float:
        """Mesh-wide loss ratio after retries."""
        lost = self.total_lost()
        exp = lost + self.total_accepted()
        return lost / exp if exp else 0.0


@dataclass
class Activity:
    """Per-node counts per time bin, relative to the first record."""

    bin_s: float
    accepted: dict[int, list[int]]
    lost: dict[int, list[int]]

    @property
    def n_bins(self) -> int:
        """Number of bins."""
        return len(next(iter(self.accepted.values()))) if self.accepted else 0


def compute(records: Iterable[AnyRecord], bin_s: float = 10.0) -> tuple[MeshMetrics, Activity]:
    """Compute metrics and the activity histogram.

    Args:
        records: Parsed gateway records in log order.
        bin_s: Width of the activity bins in seconds.

    Returns:
        The finished :class:`MeshMetrics` and the per-bin :class:`Activity`.
        Losses are attributed to the bin of the frame that revealed the gap.
    """
    recs = list(records)
    mm = MeshMetrics()
    for r in recs:
        mm.add(r)
    mm.finish()
    n_bins = max(1, math.ceil(mm.duration_s / bin_s) + 1)
    acc = {nid: [0] * n_bins for nid in sorted(mm.nodes)}
    lost = {nid: [0] * n_bins for nid in sorted(mm.nodes)}
    t0 = mm.first_ts or 0
    for r in recs:
        if isinstance(r, DataRecord | HeartbeatRecord):
            idx = int((r.ts - t0) / 1000.0 // bin_s)
            acc[r.node][idx] += 1
            lost[r.node][idx] += -1 if r.seq_state == "late" else r.gap
    return mm, Activity(bin_s, acc, lost)


def summarize(mm: MeshMetrics) -> dict[str, float | int]:
    """Headline numbers for reports and the dashboard footer."""
    rtt = [v / 1000.0 for v in mm.all_rtt_us()]
    delay = [float(v) for v in mm.all_delay_ms()]
    retried = sum(nm.retried_ok for nm in mm.nodes.values())
    frames = sum(nm.data + nm.heartbeats for nm in mm.nodes.values())
    return {
        "duration_s": mm.duration_s,
        "nodes": len(mm.nodes),
        "frames_accepted": mm.total_accepted(),
        "frames_lost": mm.total_lost(),
        "loss_pct": 100.0 * mm.loss_ratio(),
        "duplicates": sum(nm.dups for nm in mm.nodes.values()),
        "retried_pct": 100.0 * retried / frames if frames else 0.0,
        "rtt_p50_ms": percentile(rtt, 50),
        "rtt_p95_ms": percentile(rtt, 95),
        "rtt_p99_ms": percentile(rtt, 99),
        "rtt_max_ms": max(rtt) if rtt else math.nan,
        "delay_p50_ms": percentile(delay, 50),
        "delay_p95_ms": percentile(delay, 95),
        "delay_p99_ms": percentile(delay, 99),
        "delay_max_ms": max(delay) if delay else math.nan,
    }
