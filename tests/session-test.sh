#!/usr/bin/env bash
# Run with: dbus-run-session -- bash tests/session-test.sh
# Completely private PipeWire runtime; no live audio services are stopped.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d)
export XDG_RUNTIME_DIR="$test_dir/runtime" XDG_CONFIG_HOME="$test_dir/config" XDG_STATE_HOME="$test_dir/state"
export PIPEWIRE_CONFIG_DIR=/usr/share/pipewire
mkdir -m700 -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/creamykeys" "$XDG_STATE_HOME"
cp "$project_dir/packaging/config.toml" "$XDG_CONFIG_HOME/creamykeys/config.toml"
pipe_pid= wp_pid= daemon_pid=
cleanup() {
 for process_pid in "$daemon_pid" "$wp_pid" "$pipe_pid"; do
  if [[ -n "$process_pid" ]]; then kill "$process_pid" 2>/dev/null || true; wait "$process_pid" 2>/dev/null || true; fi
 done
 rm -rf -- "$test_dir"
}
trap cleanup EXIT
start_audio() {
 pipewire >"$test_dir/pipewire.log" 2>&1 & pipe_pid=$!
 sleep 0.5
 wireplumber --profile policy >"$test_dir/wireplumber.log" 2>&1 & wp_pid=$!
 sleep 1
 pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = ck-a media.class = Audio/Sink object.linger = true audio.position = [ FL FR ] }'
 pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = ck-b media.class = Audio/Sink object.linger = true audio.position = [ FL FR ] }'
 sleep 0.5
}
node_id() {
 pw-dump | python -c 'import json,sys; name=sys.argv[1]; print(next(o["id"] for o in json.load(sys.stdin) if o.get("info",{}).get("props",{}).get("node.name")==name))' "$1"
}
 check_links() {
  local sink=$1
  pw-link -l | tee "$test_dir/links"
  rg -q "creamykeys:output_FL" "$test_dir/links"
  rg -q -- "-> $sink:playback_FL" "$test_dir/links"
  rg -q -- "-> $sink:playback_FR" "$test_dir/links"
  rg -q -- "-> creamykeys:input_FL" "$test_dir/links"
  rg -q -- "-> creamykeys:input_FR" "$test_dir/links"
 }
binary="${CREAMYKEYS_BIN:-$project_dir/build-release/creamykeys}"
start_audio
wpctl set-default "$(node_id ck-a)"
"$binary" daemon >"$test_dir/daemon.log" 2>&1 & daemon_pid=$!
sleep 1
"$binary" status
check_links ck-a
"$binary" benchmark typing
sleep 0.5
wpctl set-default "$(node_id ck-b)"
sleep 0.5
check_links ck-b
printf '%s\n' 'PASS: existing stream followed default sink from ck-a to ck-b while playing.'
sleep 4.3
"$binary" disable
if find "/proc/$daemon_pid/fd" -type l -printf '%l\n' | rg -q '^/dev/input/event'; then
 printf '%s\n' 'FAIL: input descriptor remains open while disabled'; exit 1
fi
"$binary" enable
"$binary" status | rg -q 'keyboards=[1-9][0-9]*'
printf '%s\n' 'PASS: disable closes input descriptors; enable rediscovers keyboard.'
if "$binary" daemon >"$test_dir/duplicate.log" 2>&1; then
 printf '%s\n' 'FAIL: duplicate daemon started'; exit 1
fi
rg -q 'already running' "$test_dir/duplicate.log"
printf '%s\n' 'PASS: singleton lock rejects second instance.'
"$binary" status | rg -q 'mic_only=true'
"$binary" mic-only off
"$binary" status | rg -q 'mic_only=false'
"$binary" mic-only on
"$binary" status | rg -q 'mic_only=true'
printf '%s\n' 'PASS: mic-only is on by default (key sounds only in the mic) and toggles.'
sed -i 's/idle_timeout_ms = 5000/idle_timeout_ms = 0/' "$XDG_CONFIG_HOME/creamykeys/config.toml"
sleep 0.5
"$binary" status | rg 'audio_active=true'
printf '%s\n' 'PASS: warm-audio configuration activates stream without a keypress.'
sed -i 's/idle_timeout_ms = 0/idle_timeout_ms = 5000/' "$XDG_CONFIG_HOME/creamykeys/config.toml"
sleep 0.5
"$binary" status | rg 'idle_timeout_ms=5000'
kill "$pipe_pid"
wait "$pipe_pid" || true
pipe_pid=
sleep 1.5
"$binary" status | rg 'audio_state=-1'
kill "$wp_pid" 2>/dev/null || true
wait "$wp_pid" || true
wp_pid=
start_audio
wpctl set-default "$(node_id ck-a)"
sleep 3
"$binary" status
check_links ck-a
"$binary" benchmark typing
sleep 5.3
"$binary" status | rg 'audio_state=3'
printf '%s\n' 'PASS: daemon reconnects and resumes mixing after private PipeWire restart.'
pw-top -b -n 1
kill "$daemon_pid"
wait "$daemon_pid"
daemon_pid=
"$binary" daemon >"$test_dir/daemon-restart.log" 2>&1 & daemon_pid=$!
sleep 1
"$binary" status | rg -q 'keyboards=[1-9][0-9]*'
printf '%s\n' 'PASS: clean daemon restart reclaims socket and lock.'
