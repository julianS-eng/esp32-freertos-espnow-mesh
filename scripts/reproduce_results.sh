#!/usr/bin/env bash
# Regenerate every number, table and figure quoted in the documentation.
#
#   scripts/reproduce_results.sh            # simulator results + figures
#   QEMU_LOGS=out scripts/reproduce_results.sh   # also the stack table from QEMU logs
#
# Requires: cmake + a C compiler, and `pip install -e .[dev]`.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S host -B host/build-release -DCMAKE_BUILD_TYPE=Release -DMESH_HOST_SANITIZE=OFF > /dev/null
cmake --build host/build-release -j > /dev/null
mkdir -p out docs/img docs/results

# Scenario A - nominal: 6 nodes, 1 Hz readings, 5 s heartbeats, 600 s, seed 42,
# with a power loss (node 3), an interference burst (node 5) and two remote
# reconfigurations (node 1).
./host/build-release/mesh_sim --nodes 6 --duration 600 --seed 42 --out out/sim.jsonl --truth out/truth.json
meshdash report out/sim.jsonl --truth out/truth.json --strict \
    --figures docs/img --gif --markdown docs/results/simulation.md > /dev/null

# Scenario B - overload: 12 nodes at 5 Hz behind a 115200-baud console.
# Demonstrates DEGRADED mode / ACK(BUSY) back-pressure. Log lines dropped by
# the saturated gateway make some consistency checks fail by design, so this
# run is not --strict.
./host/build-release/mesh_sim --nodes 12 --report-ms 200 --baud 115200 --duration 120 --no-events --seed 42 \
    --out out/stress.jsonl --truth out/stress_truth.json
meshdash report out/stress.jsonl --truth out/stress_truth.json --markdown docs/results/stress.md > /dev/null
python3 - <<'PY'
import json
t = json.load(open("out/stress_truth.json"))
gw = t["gateway"]
lines = [
    "",
    "### Gateway counters (simulator ground truth)",
    "| Counter | Value |",
    "|---|---:|",
    f"| Frames received by the radio | {gw['rx_frames']} |",
    f"| ACK(BUSY) answers (back-pressure) | {gw['busy_acks']} |",
    f"| JSON records dropped (output queue full) | {gw['out_queue_drops']} |",
    f"| Output queue peak (of 256) | {gw['out_queue_peak']} |",
    f"| Node-side readings dropped by drop-oldest queues | {sum(n['queue_drops'] for n in t['per_node'])} |",
]
with open("docs/results/stress.md", "a") as fh:
    fh.write("\n".join(lines) + "\n")
PY

if [[ -n "${QEMU_LOGS:-}" ]]; then
    meshdash stack --node-log "${QEMU_LOGS}/qemu_node.log" --gateway-log "${QEMU_LOGS}/qemu_gw.log" \
        --markdown docs/results/stack.md > /dev/null
fi
echo "results written to docs/results/ and docs/img/"
