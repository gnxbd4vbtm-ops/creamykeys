# Verification on 2026-10-12

Host: CachyOS, Intel Core i3-6100 (two cores/four threads), KDE Wayland session.
PipeWire 1.6.9, libevdev 1.14.0, libsndfile 1.2.2, GCC 16.2.1.
Release build: C++20, `-O3 -DNDEBUG`, CMake and Ninja. No machine-specific ISA flags.
One physical LITE-ON USB keyboard was discovered. Current output is built-in
analog stereo. No packages, group memberships, udev rules, audio server settings,
or other running services were changed.

## Performance

These measurements used the original `creamy` pack before the manual-toggle
and `smooth` sound-pack update described below.

Measured using systemd CPU accounting, wall-clock durations, and `/proc` RSS.
These are short synthetic workloads, not guarantees under every system load.
The event source uses the real daemon queue and PipeWire stream, and closes
keyboard descriptors during synthetic generation.

| Workload | Duration | Daemon CPU, percent of one core | Events | Drops | Steals |
| --- | ---: | ---: | ---: | ---: | ---: |
| Idle | 15.036 s | 0.0008% | 0 | 0 | 0 |
| Synthetic typing, 10/s | 5.321 s | 1.3729% | 50 | 0 | 0 |
| Synthetic simultaneous burst, 600/s | 5.324 s | 2.2624% | 3,000 | 0 | 2,976 |

RSS and peak RSS after both workloads: 11,556 KiB (about 11.3 MiB).
Systemd MemoryCurrent: 3,756,032 bytes; peak charged memory: 4,018,176 bytes.
RSS includes shared resident mappings, so these metrics differ. Three threads.

The preferred below-1% typing CPU target was not achieved in this short test.
Idle CPU and below-20-MB RSS targets were achieved in the measured conditions.
The extreme burst deliberately exceeds the 24-voice polyphony budget; voice
stealing is expected, and a counter increment does not mean a buffer underrun.

Actual callback quantum: 128 frames at 48 kHz, or 2.67 ms per block.
Synthetic typing input-to-callback mean: 2.67603 ms, including cold activation.
Maximum observed delay: 56.9023 ms. This includes stream/sink wake-up from idle;
it is not a steady-state or acoustic latency figure.
Combined mean over all 3,050 synthetic events: 1.42831 ms.
From the cumulative sums, the warm burst mean was approximately 1.408 ms.
No queue drops or empty PipeWire dequeue buffers were observed in either run.
A later brief live observation of ten physical events had a 27.0418 ms mean and
82.6674 ms maximum input-to-callback delay, illustrating that repeated cold
wake-ups can dominate sparse typing. No typed identities or text were recorded.

These counters cannot prove absence of audible glitches. No loopback audio
capture, acoustic measurement, Bluetooth output, or end-user listening judgement
was available for this report. Hardware buffering is excluded from the latency
counter. The stream deactivates after five seconds without events to reduce idle
CPU, trading a slower first press after idle for lower background work.

## Passed checks

- Optimized Release build; CTest core tests.
- Queue full/empty behavior and 200,000 concurrently transferred ordered events.
- All six sound categories, clipping bounds, voice stealing, stale-event rejection.
- Original 30-variation WAV pack decoding; configuration roundtrip and numeric rejection.
- Rendering with heap allocation explicitly forbidden by the test allocator.
- AddressSanitizer and UndefinedBehaviorSanitizer core checks.
- Live service installation and login enablement; user service unit validation.
- Fish CLI status, pack listing, volume, disable and enable.
- Disabled daemon has zero keyboard descriptors; enabling rediscovers the keyboard.
- Real selected analog sink links are present on both channels.
- Two private null sinks: changing the default moves the existing stream while playing.
- Private PipeWire termination/restart: same daemon reconnects, relinks and mixes again.
- Single-instance lock prevents a second daemon from claiming the socket.
- Optional warm-audio mode activates the stream without a keypress and reloads live.
- Clean daemon restart reclaims the socket and rediscovers the keyboard.
- Shell syntax checks for install, uninstall, benchmark and integration scripts.

The integration test runs under a private D-Bus and PipeWire session with
WirePlumber's policy-only profile. Its graph snapshot reported ERR=0, but was
not a long-running underrun monitor. The live desktop PipeWire was not restarted.

## Checks still requiring interaction

- Physical typing and simultaneous physical keys: input was discovered and a few
  physical events were observed later, but listening quality was not confirmed.
- Unplug/replug of a physical keyboard: implemented, not physically exercised.
- Switching real hardware outputs through KDE: private metadata routing passed;
  no second real sink was available for that check.
- Suspend/resume and actual logout/login: not performed because they would
  interrupt this session. Service target and restart behavior were inspected.
- Audible rapid-typing glitches and deep/creamy realism: subjective listening pending.
- Sustained heavy-system-load and Bluetooth latency: not benchmarked.

The fallback is synthesized foley, not an authentic lubed-switch recording.
Replacement packs and their attribution are documented in the README.

## Manual toggle and smooth-pack update

Bare `creamykeys` and `creamykeys toggle` now toggle the whole user service.
On starts and enables it for future KDE logins. Off disables future startup and
stops it. A fresh installation stays off until a manual start; upgrading preserves
the prior state. No separate startup watcher is installed.

The running user's CLI was checked from fish in both directions: off had an
inactive, disabled service; on had an active, enabled service. The remembered
state is the persistent systemd enablement link. An actual reboot was not performed.

The selected `smooth` pack contains 30 original synthesized variations, a
2.2 ms rounded attack, cascaded low-pass filtering, and no separate sharp strike
component. The original `creamy` pack is retained. Loading and allocation-free
rendering checks passed with the new pack.

Average spectral-energy fraction above 2 kHz across the 30 samples:
original `creamy` 13.1775%; `smooth` 0.1980%, a decrease of 18.23 dB in that
fraction. See `smooth-pack-2026-10-12.txt`. This is an objective spectral comparison,
not a listening assessment or an authentic switch recording.

## Mechvibes ZIP import update

Added `creamykeys -s PATH.zip` and `creamykeys --soundpack PATH.zip`.
Both import a local pack and persist its selection without changing the daemon's
remembered on/off state. Imports are cached under the user's data directory;
original third-party audio/configuration/attribution is retained there, not bundled
with the source. Per-key sample routing remains preloaded and allocation-free.

The four supplied archives were tested through the installed CLI and PipeWire:

| Archive | Layout | Synthetic presses mixed |
| --- | --- | ---: |
| banana split lubed.zip | separate WAV files | 50 |
| cherrymx-blue-pbt.zip | timed clips in shared OGG | 50 |
| Creams.zip | reused clip in shared OGG | 50 |
| nk-cream.zip | separate WAV files | 50 |

Across those 200 events: zero queue drops, zero voice steals, and zero empty
PipeWire dequeues. Each run included A, Space, Enter, Backspace, Shift, and F1
routes. These are mixer/stream checks, not a new acoustic latency assessment.
See `mechvibes-packs-2026-10-12.txt` for actual statuses.

Native fixtures also cover nested archive folders, exact extended key mapping,
clip deduplication, per-key audio output, DX end-based timings, legacy filename
ranges, version-3 cycling, release-event exclusion, path traversal rejection,
invalid timing rejection, cleanup after failed import, preserved attribution,
and allocation-free rendering. Release and sanitizer checks passed.

Separate generated fixtures verified OGG, FLAC, and MP3 decoding by libsndfile
and selection while the daemon is off, without starting it. WAV is exercised
by the separate-file packs. See `mechvibes-codecs-2026-10-12.txt`.
The live toggle check also confirmed importing while off leaves the service both
inactive and disabled; manually toggling on restores active/enabled behavior.
The private PipeWire routing/restart harness was rerun after this change.

Banana Split Lubed was selected at completion. An observed RSS after cycling
through all four packs was 18,228 KiB; this includes retained allocator pages
and resident shared libraries. No additional packages or privileged changes
were required on this host.
