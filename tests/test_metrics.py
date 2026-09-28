"""Metric definitions on hand-made record sequences."""

from __future__ import annotations

import math

import numpy as np
import pytest

from meshtools.metrics import MeshMetrics, compute, percentile, summarize
from meshtools.records import AnyRecord, DataRecord, DupRecord, JoinRecord, NodeStateRecord


def data(
    ts: int,
    seq: int,
    gap: int = 0,
    state: str = "new",
    up: int | None = None,
    att: int = 1,
    rtt: int = 2000,
    node: int = 1,
) -> DataRecord:
    return DataRecord(
        ts=ts,
        type="data",
        node=node,
        seq=seq,
        att=att,
        rssi=-50,
        gap=gap,
        seq_state=state,
        up_ms=ts - 100 if up is None else up,
        rtt_us=rtt,
        status=0,
        backend=("sim",),
        values={},
    )


def join(ts: int, boot: int, gap: int = 0, node: int = 1) -> JoinRecord:
    return JoinRecord(
        ts=ts,
        type="join",
        node=node,
        seq=0,
        att=1,
        rssi=-50,
        mac="aa:bb:cc:dd:ee:ff",
        gap=gap,
        status="accepted",
        new=False,
        fw="1.0.0",
        boot=boot,
        report_ms=1000,
        hb_ms=5000,
    )


@pytest.mark.parametrize("q", [0, 5, 25, 50, 90, 95, 99, 100])
def test_percentile_matches_numpy(q: float) -> None:
    rng = np.random.default_rng(1234)  # fixed seed: reproducible
    vals = rng.exponential(3.0, size=257).tolist()
    assert percentile(vals, q) == pytest.approx(float(np.percentile(vals, q)))


def test_percentile_edge_cases() -> None:
    assert math.isnan(percentile([], 50))
    assert percentile([4.0], 99) == 4.0
    with pytest.raises(ValueError, match="q must be"):
        percentile([1.0], 101)


def test_loss_late_and_duplicates() -> None:
    recs: list[AnyRecord] = [
        join(0, boot=1),
        data(1000, 1),
        data(2000, 4, gap=2),  # 2 and 3 missing
        data(2100, 3, state="late"),  # 3 arrives late -> only 1 lost
        DupRecord(ts=2200, type="dup", node=1, seq=4, att=2, rssi=-50),
    ]
    mm, act = compute(recs, bin_s=1.0)
    nm = mm.nodes[1]
    assert (nm.data, nm.lost, nm.late, nm.dups, nm.joins) == (3, 1, 1, 1, 1)
    assert nm.epoch.rx == 4  # join + 3 data
    assert nm.loss_ratio == pytest.approx(1 / (4 + 1))
    assert act.accepted[1][:3] == [0, 1, 2]
    assert sum(act.lost[1]) == 1


def test_rejoin_same_boot_keeps_epoch_reboot_resets_it() -> None:
    mm = MeshMetrics()
    for r in (join(0, boot=1), data(1000, 1), join(5000, boot=1, gap=3)):
        mm.add(r)
    assert mm.nodes[1].epoch.lost == 3
    assert mm.nodes[1].lost == 3
    mm.add(join(9000, boot=2))
    assert mm.nodes[1].epoch.lost == 0
    assert mm.nodes[1].lost == 3  # cumulative view keeps history


def test_delay_is_relative_to_best_case_per_boot() -> None:
    recs: list[AnyRecord] = [
        data(1000, 1, up=900),  # offset 100 (best case)
        data(2050, 2, up=1900),  # 150 -> +50 ms
        data(10_000, 1, up=50),  # node rebooted: new segment, offset 9950
        data(11_020, 2, up=1050),  # +20 ms
    ]
    mm, _ = compute(recs)
    assert sorted(mm.nodes[1].delay_ms) == [0, 0, 20, 50]


def test_availability_counts_offline_time() -> None:
    mm = MeshMetrics()
    mm.add(data(0, 1))
    mm.add(
        NodeStateRecord(
            ts=10_000, type="node_state", node=1, from_state="online", to_state="offline", silent_ms=10_000
        )
    )
    mm.add(
        NodeStateRecord(
            ts=30_000, type="node_state", node=1, from_state="offline", to_state="online", silent_ms=30_000
        )
    )
    mm.add(data(40_000, 2))
    assert mm.nodes[1].availability(40_000) == pytest.approx(0.5)
    mm.add(
        NodeStateRecord(
            ts=50_000, type="node_state", node=1, from_state="online", to_state="offline", silent_ms=10_000
        )
    )
    assert mm.nodes[1].availability(60_000) == pytest.approx(1 - 30_000 / 60_000)


def test_summary_on_empty_and_simple_input() -> None:
    s = summarize(MeshMetrics())
    assert s["frames_accepted"] == 0
    assert math.isnan(float(s["rtt_p50_ms"]))
    mm, _ = compute([data(1000, 1, att=2, rtt=3000), data(2000, 2)])
    s = summarize(mm)
    assert s["retried_pct"] == pytest.approx(50.0)
    assert s["rtt_p50_ms"] == pytest.approx(2.5)
