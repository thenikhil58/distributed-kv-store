#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
stopped=0
pids=()

for command_file in /proc/[0-9]*/cmdline; do
    [[ -r "$command_file" ]] || continue
    pid="${command_file#/proc/}"
    pid="${pid%/cmdline}"
    [[ "$pid" != "$$" ]] || continue

    process_root="$(readlink -f "/proc/$pid/cwd" 2>/dev/null || true)"
    [[ "$process_root" == "$ROOT" ]] || continue
    command_line="$(tr '\0' ' ' < "$command_file" 2>/dev/null || true)"

    if [[ "$command_line" == *"/build/server "* || "$command_line" == "./build/server "* ]]; then
        kill "$pid" 2>/dev/null && { printf 'Sent graceful stop to Raft node (pid %s).\n' "$pid"; pids+=("$pid"); ((stopped += 1)); }
    elif [[ "$command_line" == *"gateway.py "* ]]; then
        kill "$pid" 2>/dev/null && { printf 'Sent graceful stop to gateway (pid %s).\n' "$pid"; pids+=("$pid"); ((stopped += 1)); }
    elif [[ "$command_line" == *"-m http.server "* && "$command_line" == *"--directory web"* ]]; then
        kill "$pid" 2>/dev/null && { printf 'Sent stop to dashboard server (pid %s).\n' "$pid"; pids+=("$pid"); ((stopped += 1)); }
    fi
done

for attempt in {1..120}; do
    remaining=()
    for pid in "${pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            remaining+=("$pid")
        fi
    done
    pids=("${remaining[@]}")
    ((${#pids[@]} == 0)) && break
    sleep 0.1
done

if ((stopped == 0)); then
    echo "No running project services found."
elif ((${#pids[@]} > 0)); then
    printf 'Services did not stop gracefully (remaining PIDs: %s).\n' "${pids[*]}" >&2
    exit 1
else
    printf 'Stopped %s project service(s). Runtime data was preserved.\n' "$stopped"
fi
