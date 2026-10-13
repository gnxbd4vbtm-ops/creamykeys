#!/usr/bin/env bash
set -euo pipefail
config_dir=${XDG_CONFIG_HOME:-$HOME/.config}
systemctl --user disable --now creamykeys.service || true
rm -f -- "$config_dir/systemd/user/creamykeys.service" "$HOME/.local/bin/creamykeys"
systemctl --user daemon-reload
printf '%s\n' 'Removed executable and service. Your configuration and sound packs were preserved.'
