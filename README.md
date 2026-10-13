# CreamyKeys

A native C++20 keyboard sound daemon for CachyOS and KDE Plasma on Wayland. It reads physical and virtual keyboard events with libevdev, mixes overlapping sounds through the native PipeWire API, and follows the selected default output through WirePlumber. No GUI or application window is needed.

The selected `smooth` pack contains 30 original synthesized variations with a rounded 2.2 ms attack and cascaded filtering to reduce sharp clicks: normal keys, Space, Enter, Backspace, modifiers, and special keys. It is soft, low-pitched keyboard-inspired foley. It is **not a recording of Oil Kings or another real switch**; audible realism still needs your listening assessment. The WAV files are CC0; the code is MIT.

## Installed on this desktop

- Executable: `~/.local/bin/creamykeys`
- Configuration: `~/.config/creamykeys/config.toml`
- Selected sounds: `~/.local/share/creamykeys/packs/smooth/` (original `creamy` pack retained)
- User service: `~/.config/systemd/user/creamykeys.service`
- Private IPC: `$XDG_RUNTIME_DIR/creamykeys/control.sock`

Run bare `creamykeys` to toggle the daemon on or off. Your choice is remembered: on resumes after reboot/KDE login; off leaves no daemon running at login. A fresh install stays off until your first manual start. No separate startup controller runs while off. The service stops with the graphical session and restarts after a crash. PipeWire and WirePlumber remain the existing system services.

These commands work in fish and bash:

```fish
creamykeys # toggle on/off and remember across reboots
creamykeys status
creamykeys disable
creamykeys enable
creamykeys toggle
creamykeys volume 0.5
creamykeys pack list
creamykeys pack set smooth
creamykeys reload
creamykeys restart
creamykeys stop
creamykeys start
creamykeys benchmark typing
creamykeys benchmark burst
```

`creamykeys` and `creamykeys toggle` both toggle the whole service. `start` turns it on and remembers on; `stop` turns it off and remembers off. `restart` turns it on and remembers on. `enable` and `disable` are separate playback controls for an already running daemon; disable closes keyboard descriptors. Use the bare command or `stop` when you want the process and future autostart turned off.

A benchmark plays synthetic category events for five seconds, temporarily closes keyboard descriptors, then rediscovers the keyboards. `typing` produces 10 events/s and `burst` 600 events/s with six simultaneous categories. `benchmark` alone selects burst. Nothing is injected into the desktop's input stack. Read `status` after completion. Counters are cumulative since daemon startup. Use `bash tools/benchmark.sh` for reproducible CPU and memory measurements.

## Build and install

Dependencies: GCC or Clang with C++20, CMake, Ninja, pkg-config, PipeWire, libevdev, libsndfile, libarchive, nlohmann-json headers, and systemd. These were already installed on the inspected CachyOS host. If missing, review the package changes before running:

```fish
sudo pacman -S --needed base-devel cmake ninja pkgconf pipewire libevdev libsndfile libarchive nlohmann-json
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
bash packaging/install.sh
```

The installer needs no root access when keyboard permissions already exist. It installs only user files and preserves existing configuration, packs, and on/off state. A fresh install does not start or enable the service. Upgrading an already running service restarts it and checks IPC. On this desktop it detected one keyboard and reused the account's existing input permissions. It does not modify your groups, udev rules, PipeWire settings, or other services.

```fish
bash packaging/uninstall.sh
```

Uninstall removes the executable and service and preserves configuration and sounds. Remove those directories separately only if you intend to discard them.

## Configuration

The supported flat TOML schema is:

```toml
enabled = true
volume = 0.35
sound_pack = "smooth"
pitch_variation = 0.025
randomize_samples = true
play_on_repeat = false
max_voices = 24
follow_default_sink = true
idle_timeout_ms = 5000
```

Volume is 0–1; pitch variation is 0–0.1; voice count is 1–64. The optional idle timeout is 0–60,000 ms and defaults to 5,000 ms. Set `idle_timeout_ms = 0` to keep audio running after startup, avoid repeated cold wake-up delays, and accept recurring PipeWire callbacks while idle. Unknown and duplicate keys, nonfinite numbers, and invalid values are rejected. Editing or atomically replacing `config.toml` reloads it through inotify. You can also run `creamykeys reload` or send SIGHUP. A failed live reload leaves the current working configuration and pack in use. The daemon logs errors and device counts, never key identities or text.

Live changes reset active sounds before swapping the new settings. A successful configuration edit can therefore cut a currently playing sound short. Startup with an invalid configuration or pack fails with a clear error in `journalctl --user -u creamykeys.service`.

## Mechvibes ZIP sound packs

Import and select a downloaded pack directly, keeping quotes around paths with spaces:

```fish
creamykeys -s "/home/byte/Downloads/banana split lubed.zip"
creamykeys --soundpack /home/byte/Downloads/cherrymx-blue-pbt.zip
creamykeys -s /home/byte/Downloads/Creams.zip
creamykeys -s /home/byte/Downloads/nk-cream.zip
```

This imports the ZIP once into `~/.local/share/creamykeys/packs/` and saves its selection. If the daemon is running, it switches packs immediately; if it is off, the command selects the pack for the next manual start. It does not toggle on/off or alter the remembered startup choice. The original ZIP can subsequently be moved or deleted. Reimporting the same archive reuses its local copy; changed ZIP contents receive a separate pack name. `creamykeys pack list` shows imported names, and `pack set NAME` switches between them.

The importer handles [Mechvibes single-recording clips and separate-file mappings](https://github.com/hainguyents13/mechvibes/wiki/Config-Versions), including ZIPs wrapped in a folder. It preserves individual key assignments and converts libuiohook extended key codes to Linux evdev codes. Repeated references to a file or timed clip share a preloaded sample. Undefined keys use category fallbacks from the imported pack. Imported recordings are normalized and trimmed at load time; no synthetic samples are mixed into them.

Supported audio decoding: WAV, OGG, FLAC, MP3 through libsndfile. Legacy version 2 filename ranges and MechvibesDX key names/timing pairs are also accepted. Version 3 key-down references support files, clips, variations, and cycling; custom hold timers and release playback are not implemented. CreamyKeys keeps its existing key-down-only behavior and optional evdev repeats.

A ZIP must contain exactly one `config.json`. Limits: 128 MiB compressed, 256 MiB unpacked, 2,048 entries, 32 MiB of unique decoded samples, and two seconds per individual key sound. A shared recording can be much longer because only its referenced clips are decoded. Invalid timings, path traversal, links, encrypted entries, and duplicate files are rejected before selection. A failed import leaves the current pack in use.

Original audio, configuration, author metadata, and any license/attribution files are retained in the local import. User-imported third-party packs are not included in this project's source or relabeled CC0.

## Replacement sound packs

Create `~/.local/share/creamykeys/packs/NAME/` with these filename prefixes:

| Category | Files |
| --- | --- |
| Ordinary letters and digits | `normal-1.wav`, `normal-2.wav`, … |
| Space | `space-1.wav`, … |
| Enter and keypad Enter | `enter-1.wav`, … |
| Backspace | `backspace-1.wav`, … |
| Shift, Ctrl, Alt, Meta | `modifier-1.wav`, … |
| Function keys, navigation, punctuation, others | `special-1.wav`, … |

Each category requires 1–32 variations. Use mono or stereo WAV files, 8–192 kHz, no longer than 0.5 seconds. The entire decoded pack is limited to 8 MiB. Startup/loading downmixes to mono, trims leading silence, normalizes peak amplitude to 0.7, and converts to 48 kHz. Rate conversion and pitch playback use linear interpolation; supply 48 kHz recordings with any needed filtering already applied for the best fidelity. All samples stay in memory during playback.

Run `creamykeys pack set NAME` to validate and select a new pack. Restart after replacing WAV files inside the already selected pack. Include the source license and any required attribution with your pack.

The original fallback can be regenerated with:

```fish
build-release/creamykeys-generate soundpacks/smooth
```

A researched recording, [Keyboard Typing 10 (WhiteFox, Mechanical) by grcekh](https://freesound.org/people/grcekh/sounds/546167/), is marked CC0 by its author. It is a continuous typing recording rather than the requested isolated lubed-linear variations, so it was not bundled. No third-party pack with uncertain licensing was downloaded.

## Audio and input behavior

The main thread blocks in epoll on libevdev descriptors, inotify, a private same-user Unix socket, signal descriptors, and armed timers. Keyboard classification accepts devices with typing/navigation/function-key capabilities, including sparse uinput macro devices such as Lunge Swapper. Virtual and unspecified bus types are accepted. Mouse-only devices, power-button-only devices, and mouse-button events on hybrid devices are excluded. Hotplug notifications rescan devices; libevdev synchronization drops recovered state events instead of replaying lost presses. The daemon never uses EVIOCGRAB, writes input events, reconstructs text, or sends input data over the network.

Each descriptor tracks held-key state. Reports with the same physical identity, key, and a timestamp within 2 ms are suppressed as duplicates. Separate keyboards have independent identities. This is a duplicate-report heuristic, not a guarantee for every unusual composite keyboard or firmware.

A 511-entry usable SPSC queue carries an ephemeral key code, category, and monotonic timestamp to the audio callback. Those events are consumed immediately and never retained as a history or logged. The callback uses fixed voice slots, deterministic oldest-voice stealing, randomized variations, small pitch changes, and smooth bounded saturation. Stealing replaces a voice immediately; extreme overload can change the audible texture. Queue overflow discards the newest event; events older than 250 ms are discarded to avoid delayed sound bursts.

The callback performs no sample I/O, heap allocation, logging, or mutex locking. Configuration swaps run through a synchronous PipeWire data-loop invocation, so sample pointers cannot be changed during mixing. Old sample memory is released outside the callback. The application has one main thread and two PipeWire-managed threads on the tested installation.

The stream initially uses normal PipeWire autoconnection without specifying a device. When following is enabled, it observes `default.audio.sink` metadata and updates **only its own stream's** target metadata. WirePlumber performs routing and moves existing links. It does not change the system default. This requires WirePlumber's normal stream-moving policy; custom policies that prohibit stream moves can prevent automatic following. See [WirePlumber linking settings](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/settings.html).

With the default idle timeout, after five seconds without a press the stream deactivates to eliminate recurring audio callbacks. A first press after idle can incur output wake-up buffering. Subsequent presses keep the stream active. No server-wide quantum, device, or scheduling settings are changed. A lost PipeWire connection is recreated with a one-second initial retry and two-second retries while unavailable; keys during that outage produce no sound.

## Permissions

This installation reused an existing `input` group membership. That group gives the account broader input-device access than CreamyKeys itself uses; the installer did not grant it. The service runs as the user, has `NoNewPrivileges`, and restricts sockets to AF_UNIX.

On a new host, prefer an administrator-reviewed dedicated read-only keyboard group rather than adding the user to the broad `input` group. An example rule is supplied in `packaging/70-creamykeys.rules.example`. It is **not installed automatically**. Creating a group, changing membership, and applying udev rules require approval and may affect other input consumers. Do not run the audio daemon as root. If access is unavailable, `status` reports inaccessible input nodes and the number of detected keyboards; mouse or other denied event nodes can contribute to the inaccessible count.

## Validation and measurements

See [the test report](reports/RESULTS.md), [raw measurements](reports/benchmark-2026-10-12.txt), and [isolated session output](reports/session-test-2026-10-12.txt).

Core tests exercise queue bounds and concurrent producer/consumer ordering, category selection, sample decoding, polyphony, deterministic voice stealing, clipping bounds, stale event rejection, silent output, config roundtrip, invalid numeric rejection, and allocation-free rendering. AddressSanitizer and UndefinedBehaviorSanitizer also passed.

```fish
ctest --test-dir build-release --output-on-failure
dbus-run-session -- bash tests/session-test.sh
```

The integration harness uses private runtime, configuration, state, D-Bus, PipeWire, and policy-only WirePlumber instances with two null sinks. It changes no live audio defaults. It requires Python only as a test helper to parse `pw-dump`; the daemon and ordinary CLI commands do not use Python.

The software latency counter ends at the first mixer callback that consumes an event. It excludes the rest of the current audio block, graph/device buffers, DAC, speakers, and any Bluetooth codec/transport buffering. The requested 128/48000 quantum is a hint; the graph chooses the actual quantum. At 128 frames one block is 2.67 ms. Scheduling pressure can delay delivery, and cold sink activation can dominate latency. These figures are not an end-to-end acoustic latency measurement or a zero-latency claim.

Physical unplug/replug, KDE hardware-output switching, suspend/resume, subjective realism, and audible glitch checks were not completed automatically. The isolated routing/restart tests cover the software paths but cannot substitute for those hardware checks.
