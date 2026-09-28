#!/usr/bin/env python3
"""Assert that QEMU smoke-test logs show a healthy firmware (used by CI).

Usage: qemu_check.py node <log> | qemu_check.py gateway <log>
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

FATAL = ("Guru Meditation", "abort() was called", "silent for", "stack overflow", "assert failed")


def _fail(msg: str) -> int:
    print(f"FAIL: {msg}")
    return 1


def check_node(text: str) -> int:
    """Node must join, deliver frames, apply a remote CONFIG and report task stacks."""
    for bad in FATAL:
        if bad in text:
            return _fail(f"fatal marker {bad!r} in node log")
    if "joined gateway" not in text:
        return _fail("node never joined the (emulated) gateway")
    oks = [int(m) for m in re.findall(r"hb: heap=\d+ min=\d+ ok=(\d+)", text)]
    if not oks or oks[-1] < 10:
        return _fail(f"too few acknowledged frames: {oks[-1:] or 'none'}")
    if "config key 1 = " not in text:
        return _fail("remote CONFIG was not applied")
    tasks = set(re.findall(r"stack task=(\w+) size=\d+ min_free=\d+", text))
    if tasks != {"sensor", "tx", "heartbeat", "supervisor"}:
        return _fail(f"missing stack reports: {tasks}")
    print(f"OK: node joined, {oks[-1]} frames acked, CONFIG applied, stacks reported for {sorted(tasks)}")
    return 0


def check_gateway(text: str) -> int:
    """Gateway must reach RUNNING, register nodes, detect the offline node and answer commands."""
    for bad in FATAL:
        if bad in text:
            return _fail(f"fatal marker {bad!r} in gateway log")
    recs = []
    for line in text.splitlines():
        if line.startswith("{"):
            try:
                recs.append(json.loads(line))
            except json.JSONDecodeError:
                return _fail(f"malformed JSON line: {line[:80]}")
    kinds = [r["type"] for r in recs]
    boots = kinds.count("boot")
    if boots != 1:
        return _fail(f"expected exactly one boot (no restarts), saw {boots}")
    if not any(r["type"] == "gw_state" and r["to"] == "running" for r in recs):
        return _fail("gateway never reached RUNNING")
    if kinds.count("join") < 4 or kinds.count("data") < 50:
        return _fail(f"too little traffic: {kinds.count('join')} joins, {kinds.count('data')} data")
    trans = {(r["from"], r["to"]) for r in recs if r["type"] == "node_state"}
    for needed in (("online", "suspect"), ("suspect", "offline"), ("offline", "online")):
        if needed not in trans:
            return _fail(f"missing liveness transition {needed}")
    cfg = [(r["key"], r["result"]) for r in recs if r["type"] == "config"]
    if ("report_ms", "ok") not in cfg or ("hb_ms", "ok") not in cfg or ("reboot", "ok") not in cfg:
        return _fail(f"unexpected CONFIG results: {cfg}")
    print(f"OK: {len(recs)} JSON records, transitions {sorted(trans)}, config results {cfg}")
    return 0


def main(argv: list[str]) -> int:
    """Entry point."""
    if len(argv) != 3 or argv[1] not in ("node", "gateway"):
        print(__doc__)
        return 2
    text = Path(argv[2]).read_text(encoding="utf-8", errors="replace")
    return check_node(text) if argv[1] == "node" else check_gateway(text)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
