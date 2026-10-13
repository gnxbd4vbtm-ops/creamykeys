#!/usr/bin/env bash
# Integration check for the four user-supplied archives; plays 5 seconds per pack.
set -euo pipefail
binary=${CREAMYKEYS_BIN:-$HOME/.local/bin/creamykeys}
for filename in 'banana split lubed.zip' 'cherrymx-blue-pbt.zip' 'Creams.zip' 'nk-cream.zip'; do
 printf '\nArchive: %s\n' "$filename"
 "$binary" --soundpack "$HOME/Downloads/$filename"
 "$binary" benchmark typing
 sleep 5.3
 "$binary" status
done
"$binary" -s "$HOME/Downloads/banana split lubed.zip"
printf '%s\n' 'All four archives imported, selected and exercised through PipeWire. Banana Split Lubed selected.'
