# MiSTerCast Linux Development Notes

These instructions apply to the entire repository. MiSTerCast is Linux-only and targets Ubuntu 22.04 x86-64 under X11/Xorg. Do not reintroduce the removed Windows, WPF, DXGI, COM, or unfinished Wayland/portal paths.

## Build and verification

- Use CMake/Ninja and C++17. Keep platform-neutral code in `src/core`, Linux integrations in `src/linux`, and frontend code in `src/cli` or `src/gui`.
- Build and run the core test suite after changes:

  ```sh
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```

- Qt is optional at configure time, but GUI changes must be compiled with Qt 6 available. A useful construction smoke test is:

  ```sh
  timeout 3s env QT_QPA_PLATFORM=offscreen DISPLAY= ./build/mistercast
  ```

- Run ASan/UBSan for changes affecting buffers, pixel conversion, queues, audio, or lifecycle. LeakSanitizer may need `ASAN_OPTIONS=detect_leaks=0` in ptrace-based sandboxes.
- Keep `git diff --check` clean. Preserve unrelated user changes and do not commit generated build/package artifacts.

## X11 capture gotchas

- `$DISPLAY` selects the X server/screen at process startup; values such as `:1` and `:1.1` are valid. RandR names such as `DP-0` are monitors within that server, not substitutes for `$DISPLAY`.
- Monitor enumeration is limited to the selected X server/screen. Diagnose missing outputs with `DISPLAY=:1 xrandr --listmonitors` before changing capture code.
- A 4K source exposed a severe performance trap: generic per-channel normalization ran at about 6 FPS. Preserve the native 32-bit little-endian BGRX row-copy fast path in `normalizeToBgra`; the transform and preview intentionally ignore the X padding/alpha byte. Keep the generic mask-based path for other visuals.
- Capture must remain paced to the selected modeline refresh. An unrestricted XCB loop floods memory bandwidth and causes video latency, audio overruns, and an unresponsive GUI.
- Preview is intentionally throttled to roughly 10 FPS and scaled before posting to Qt. Never enqueue full-resolution preview updates for every captured frame. All widget mutations must execute on the Qt thread.
- MIT-SHM is the preferred path; retain the functional `xcb_get_image` fallback and actionable errors for missing `$DISPLAY`, disconnects, and disappearing monitors.

## Streaming and audio invariants

- Groovy_MiSTer uses UDP port 32100. The GUI target field accepts only an IPv4 address or hostname, without a port suffix.
- Preserve protocol command framing and ordering. Audio blocks are sent before their associated video frame, matching the upstream Windows sender and Groovy_MiSTer receiver.
- Audio is stereo signed S16LE. `CMD_AUDIO` carries a little-endian byte count, followed by exactly that many PCM bytes in MTU-sized datagrams.
- Consume audio according to elapsed steady-clock time, not an assumed number of samples per video frame. Rendering can briefly run below nominal refresh; frame-count-based consumption causes ring growth, drift, and eventual overrun.
- Keep startup prebuffering, bounded ring behavior, silence-on-underrun accounting, and oldest-sample dropping on overrun. Reset all audio timing and counters on every start.
- PulseAudio/`pipewire-pulse` capture uses the default output sink's monitor source. This duplicates system playback; it does not redirect or mute laptop speakers. When diagnosing silence, distinguish:
  - captured PCM level (`audioPeak`);
  - the MiSTer ACK audio bit (`misterAudioEnabled`);
  - receiver/TV output routing.
- Nonzero PCM level plus `MiSTer audio on` proves capture and core negotiation are active. Verify receiver logs/output before rewriting Pulse capture or packet framing.
- Keep capture, rendering/network, and audio production on owned joinable threads. Stop must remain idempotent, wake blocked workers, send `CMD_CLOSE`, and join all threads.

## Direct-Ethernet testing

- A dedicated link should use a non-overlapping subnet, for example PC `192.168.200.1/24` and MiSTer `192.168.200.2/24`, with no gateway or DNS on that interface.
- Before blaming UDP transport, verify route selection:

  ```sh
  ip route get 192.168.200.2
  ```

  It must show the Ethernet device and PC Ethernet source address, not a Wi-Fi gateway.
- For live testing, prefer session-only CLI overrides and terminate cleanly with SIGINT:

  ```sh
  timeout --signal=INT --kill-after=3s 16s \
    ./build/mistercast stream --target 192.168.200.2 --monitor DP-0 --audio --frame-delay 0
  ```

- Check reported stream/capture FPS, dropped frames, audio buffered samples, PCM level, MiSTer audio state, overruns, and underruns. On the known 3840x2160 test source, the optimized path sustained approximately 59.5–60 FPS with zero continuing audio overrun/underrun; significant regressions require investigation before commit.
- `timeout` normally returns status 124 even when SIGINT triggered an orderly application shutdown.

## GUI expectations

- Retain the compact Windows-inspired hierarchy: top stream/save/load/target controls, preset plus editable modeline timings, capture controls beside a large preview, and status/logs at the bottom.
- The stream button must visibly transition through `Starting…`, `Stop Stream`, and `Stopping…`; do not let preview traffic starve these state updates.
- Keep the diagnostic log bounded. Periodic performance counters belong in the status line; log only meaningful threshold crossings rather than repeating an overrun message every frame.
