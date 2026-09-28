#!/usr/bin/env bash
# Build a firmware with the QEMU overlay (loopback radio) and run it in
# Espressif's QEMU for a fixed time, optionally typing console commands.
# Runs inside the espressif/idf image (CI) or any shell with ESP-IDF exported.
#
#   scripts/qemu_run.sh <project-dir> <seconds> <log-file> [commands-file]
#
# commands-file lines: "<delay-seconds> <command>" (sent to the UART console).
set -euo pipefail

proj=$(cd "$1" && pwd)
secs=$2
log=$(realpath -m "$3")
cmds=${4:+$(realpath "$4")}
build="${proj}/../build-$(basename "$proj")-qemu"
idf_args=(-B "$build" -DSDKCONFIG="$build/sdkconfig" -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu")

cd "$proj"
idf.py "${idf_args[@]}" build > "${log}.build" 2>&1 || { tail -50 "${log}.build"; exit 1; }

feeder() {
    if [[ -n "${cmds}" ]]; then
        while read -r delay cmd; do
            [[ -z "${delay}" || "${delay}" == \#* ]] && continue
            sleep "${delay}"
            printf '%s\r\n' "${cmd}"
        done < "${cmds}"
    fi
    sleep "${secs}" # keep stdin open until QEMU is stopped
}

feeder | timeout --signal=TERM "${secs}" idf.py "${idf_args[@]}" qemu > "${log}" 2>&1 || true
echo "QEMU run of $(basename "$proj") finished: $(wc -l < "${log}") log lines -> ${log}"
