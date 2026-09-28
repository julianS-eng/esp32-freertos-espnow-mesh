"""JSON Lines parsing, including noise tolerance."""

from __future__ import annotations

from pathlib import Path

from meshtools.records import (
    DataRecord,
    GwStateRecord,
    InfoRecord,
    JoinRecord,
    ParseStats,
    parse_line,
    read_log,
)


def test_sample_log_is_parsed_and_noise_is_counted(data_dir: Path) -> None:
    stats = ParseStats()
    recs = read_log(data_dir / "gateway_sample.jsonl", stats)
    assert stats.lines == 34
    assert stats.non_json == 1  # ESP-IDF log line
    assert stats.malformed == 2  # missing keys + broken JSON
    assert stats.unknown_type == 1
    assert stats.records == len(recs) == 30
    assert isinstance(recs[0], GwStateRecord)
    assert any(isinstance(r, InfoRecord) and "host simulator" in r.msg for r in recs)
    joins = [r for r in recs if isinstance(r, JoinRecord)]
    assert joins
    assert all(j.status == "accepted" for j in joins)


def test_data_record_fields() -> None:
    line = (
        '{"ts":12345,"type":"data","node":7,"seq":42,"att":2,"rssi":-61,"gap":1,"seq_state":"new",'
        '"up_ms":999,"rtt_us":2100,"status":1,"backend":["mq2","mpu6050"],"values":{"gas_ppm":210.5}}'
    )
    rec = parse_line(line)
    assert isinstance(rec, DataRecord)
    assert rec.values == {"gas_ppm": 210.5}
    assert rec.backend == ("mq2", "mpu6050")
    assert (rec.node, rec.seq, rec.att, rec.gap, rec.rtt_us) == (7, 42, 2, 1, 2100)


def test_non_object_json_is_malformed() -> None:
    stats = ParseStats()
    assert parse_line("[1, 2]", stats) is None
    assert parse_line("   ", stats) is None
    assert stats.malformed == 0
    assert stats.non_json == 2
    assert parse_line("{}", stats) is None
    assert stats.unknown_type == 1
