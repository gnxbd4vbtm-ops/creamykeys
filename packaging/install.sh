#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
config_dir=${XDG_CONFIG_HOME:-$HOME/.config}
data_dir=${XDG_DATA_HOME:-$HOME/.local/share}
was_active=false
if systemctl --user is-active --quiet creamykeys.service; then was_active=true; fi
cmake -S "$project_dir" -B "$project_dir/build-release" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_dir/build-release"
ctest --test-dir "$project_dir/build-release" --output-on-failure
install -Dm755 "$project_dir/build-release/creamykeys" "$HOME/.local/bin/creamykeys.new"
mv -f -- "$HOME/.local/bin/creamykeys.new" "$HOME/.local/bin/creamykeys"
mkdir -p "$config_dir/creamykeys" "$data_dir/creamykeys/packs" "$config_dir/systemd/user"
if [[ ! -e "$config_dir/creamykeys/config.toml" ]]; then
 install -m600 "$project_dir/packaging/config.toml" "$config_dir/creamykeys/config.toml"
fi
for pack_dir in "$project_dir"/soundpacks/*; do
 pack_name=$(basename -- "$pack_dir")
 if [[ ! -d "$data_dir/creamykeys/packs/$pack_name" ]]; then
  cp -a "$pack_dir" "$data_dir/creamykeys/packs/$pack_name"
 fi
done
install -m644 "$project_dir/packaging/creamykeys.service" "$config_dir/systemd/user/creamykeys.service"
systemctl --user daemon-reload
if "$was_active"; then
 systemctl --user restart creamykeys.service
 sleep 2
 systemctl --user is-active --quiet creamykeys.service
 "$HOME/.local/bin/creamykeys" status
fi
printf '%s\n' 'Installed CreamyKeys. Run creamykeys to toggle on/off; that choice is remembered across reboots.'
printf '%s\n' 'Fresh installations stay off until manually started. Existing configuration, packs, and toggle state are preserved.'
