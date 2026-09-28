"""Conformance of the Python codec with the C firmware codec (golden vectors)."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from meshtools import protocol as p


@pytest.fixture(scope="module")
def golden(data_dir: Path) -> dict[str, object]:
    data: dict[str, object] = json.loads((data_dir / "golden_frames.json").read_text())
    return data


def test_crc_catalogue_value_matches_c(golden: dict[str, object]) -> None:
    crc_check = golden["crc_check"]
    assert isinstance(crc_check, dict)
    assert p.crc16_ccitt(b"123456789") == 0x29B1 == crc_check["crc"]


def test_every_golden_frame_decodes_and_reencodes_identically(golden: dict[str, object]) -> None:
    vectors = golden["vectors"]
    assert isinstance(vectors, list)
    assert len(vectors) == 7
    for v in vectors:
        raw = bytes.fromhex(v["hex"])
        frame = p.decode(raw)
        assert frame.header.type == v["type"], v["name"]
        assert frame.header.node_id == v["node_id"]
        assert frame.header.seq == v["seq"]
        assert frame.header.uptime_ms == v["uptime_ms"]
        assert frame.header.attempt == v["attempt"]
        assert frame.header.flags == v["flags"]
        assert p.encode(frame) == raw, v["name"]


def test_golden_payload_contents(golden: dict[str, object]) -> None:
    vectors = {v["name"]: v for v in golden["vectors"]}  # type: ignore[attr-defined]
    data = p.decode(bytes.fromhex(vectors["data"]["hex"])).payload
    assert isinstance(data, p.DataPayload)
    assert data.channels == ((3, 210500), (11, -981), (4, 2**31 - 1), (17, -(2**31)))
    assert data.last_rtt_us == 2345
    hb = p.decode(bytes.fromhex(vectors["heartbeat"]["hex"])).payload
    assert isinstance(hb, p.HeartbeatPayload)
    assert hb.last_ack_rssi == -67
    assert hb.stack_hwm == (1000, 1100, 1200, 1300, 1400, 1500)
    ack = p.decode(bytes.fromhex(vectors["ack"]["hex"])).payload
    assert isinstance(ack, p.AckPayload)
    assert (ack.acked_seq, ack.status, ack.rssi) == (0xFFFF, 1, -128)


def _sample() -> bytes:
    hdr = p.Header(type=p.MsgType.CONFIG, flags=p.FLAG_ACK_REQ, node_id=0, seq=5, uptime_ms=10)
    return p.encode(p.Frame(hdr, p.ConfigPayload(key=1, value=500)))


@pytest.mark.parametrize(
    ("mutate", "code"),
    [
        (lambda b: b[:10], "too_short"),
        (lambda b: bytes([0x00]) + b[1:], "bad_magic"),
        (lambda b: b[:1] + bytes([2]) + b[2:], "bad_version"),
        (lambda b: b + b"\x00", "bad_length"),
        (lambda b: b[:15] + bytes([b[15] ^ 1]) + b[16:], "bad_crc"),
    ],
)
def test_rejections_use_firmware_error_codes(mutate: object, code: str) -> None:
    raw = _sample()
    assert callable(mutate)
    with pytest.raises(p.ProtocolError) as exc:
        p.decode(mutate(raw))
    assert exc.value.code == code


def test_payload_type_mismatch_and_oversize() -> None:
    hdr = p.Header(type=p.MsgType.ACK, flags=0, node_id=1, seq=1, uptime_ms=1)
    with pytest.raises(p.ProtocolError):
        p.encode(p.Frame(hdr, p.ConfigPayload(1, 1)))
    dhdr = p.Header(type=p.MsgType.DATA, flags=0, node_id=1, seq=1, uptime_ms=1)
    with pytest.raises(p.ProtocolError):
        p.encode(p.Frame(dhdr, p.DataPayload(0, 0, 0, tuple((1, i) for i in range(21)))))


def test_all_types_round_trip() -> None:
    payloads: list[tuple[int, p.Payload]] = [
        (p.MsgType.JOIN, p.JoinPayload(1, 2, 3, 4, 5, 6)),
        (p.MsgType.JOIN_ACK, p.JoinAckPayload(0, 1, 2)),
        (p.MsgType.DATA, p.DataPayload(1, 0, 5, ((1, -1),))),
        (p.MsgType.HEARTBEAT, p.HeartbeatPayload(1, 2, 3, 4, 5, 6, 7, 8, 9, -10, 2, (1, 2, 0, 0, 0, 0))),
        (p.MsgType.ACK, p.AckPayload(1, 3, 1, 0, -50)),
        (p.MsgType.CONFIG, p.ConfigPayload(2, 9000)),
    ]
    for t, pl in payloads:
        f = p.Frame(p.Header(type=t, flags=0, node_id=9, seq=65535, uptime_ms=2**32 - 1, attempt=8), pl)
        assert p.decode(p.encode(f)) == f


def test_seq_diff_wraps() -> None:
    assert p.seq_diff(0, 0xFFFF) == 1
    assert p.seq_diff(0xFFFF, 0) == -1
    assert p.seq_diff(0x8000, 0) == -0x8000
