#!/usr/bin/env bash
set -euo pipefail
binary=${CREAMYKEYS_BIN:-$HOME/.local/bin/creamykeys}
cpu_ns() { systemctl --user show creamykeys.service -p CPUUsageNSec --value; }
measure() {
 local label=$1 duration=$2 before after start end
 before=$(cpu_ns)
 start=$(date +%s%N)
 if [[ "$label" != idle ]]; then "$binary" benchmark "$label"; fi
 sleep "$duration"
 after=$(cpu_ns)
 end=$(date +%s%N)
 awk -v name="$label" -v cpu="$((after-before))" -v wall="$((end-start))" 'BEGIN {printf "%s: %.4f%% of one core, %.3f ms CPU over %.3f s\n",name,100*cpu/wall,cpu/1e6,wall/1e9}'
 "$binary" status
}
printf '%s\n' 'Synthetic events only; no physical keystrokes are injected or recorded.'
measure idle 15
measure typing 5.3
measure burst 5.3
pid=$(systemctl --user show creamykeys.service -p MainPID --value)
awk '/VmRSS|VmHWM|Threads/ {print}' "/proc/$pid/status"
systemctl --user show creamykeys.service -p MemoryCurrent -p MemoryPeak
