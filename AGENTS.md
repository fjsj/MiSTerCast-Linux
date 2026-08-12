# MiSTerCast Linux Development Notes

These instructions apply to the entire repository. MiSTerCast is Linux-only and targets Ubuntu 22.04, 24.04, and 26.04 on x86-64, under X11/Xorg and Wayland. Do not reintroduce the removed Windows, WPF, DXGI, or COM paths.

Wayland capture goes through the `xdg-desktop-portal` ScreenCast portal and PipeWire, in `src/linux/portal_screencast.*` and `src/linux/portal_capture.cpp`. It exists because 24.04 defaults to Wayland and 26.04's GNOME has no Xorg session at all, so X11 capture alone reaches neither desktop. Both halves are optional at configure time and needed together; without them the backend compiles to a stub that names the missing packages.

Some Windows behaviour is absent on purpose because it was unreachable, inert, or wrong — congestion control, the first-blit skip, the pre-`CMD_CLOSE` flush wait, delta/duplicate-frame compression, and the joystick/PS2 back-channel among them. "Windows behaviour not carried over" in `README.md` records each one with its reasoning and `b7493f9` line references. Check it before restoring something that looks like a porting regression.

## Build and verification

- Use CMake/Ninja and C++17. Keep platform-neutral code in `src/core`, Linux integrations in `src/linux`, and frontend code in `src/cli` or `src/gui`.
- Build on more than the development machine before believing a change compiles. Each release moves the compiler and the PipeWire/SPA headers, and all three have already broken this tree: gcc 13 stopped including `<array>` transitively, and `SPA_DATA_FLAG_MAPPABLE` does not exist in 22.04's PipeWire. `packaging/docker/wayland-test.sh RELEASE` builds and tests inside any of them.
- Build and run the test suite after changes:

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
- `packaging/docker/` holds the container distribution (`Dockerfile`, `mistercast-docker.sh`, `smoke-test.sh`; usage in `README.md`), published as `ghcr.io/fjsj/mistercast-linux` by `.github/workflows/docker.yml`, plus the Wayland test rig (`Dockerfile.wayland-test`, `wayland-test-session.sh`, `wayland-test.sh`). When the build or runtime dependencies in `CMakeLists.txt` change, update both Dockerfiles' apt lists and the CPack `DEPENDS` list to match.
- The coverage job deliberately builds *without* the Wayland backend. Its code can only be exercised where there is a compositor and a portal, and no coverage runner has either, so compiling it in would drag the branch floor down for code the job cannot reach. The `wayland` CI job covers it instead.
- Keep `git diff --check` clean. Preserve unrelated user changes and do not commit generated build/package artifacts.

## Tests

Tests use GoogleTest, fetched by CMake from a pinned release when no `GTest`
package is installed; `-DMISTERCAST_USE_SYSTEM_GTEST=ON` requires the installed
one and never reaches the network. Sources mirror `src/`, with shared fakes in
`tests/support`.

| ctest name | Covers | Needs |
| --- | --- | --- |
| `core` | types, protocol limits, config persistence, audio ring/pacer, adaptive timing, transform, UDP pacing | nothing |
| `transport` | `GroovyTransport` against a fake endpoint on an arbitrary port | loopback |
| `session` | `StreamSession` with fake capture devices | UDP 32100 |
| `pattern` | pattern parsing, generation, tone, and streaming | UDP 32100 |
| `cli` | the built `mistercast` binary, one case per argument path | X11, UDP 32100 |
| `gui` | `MainWindow` driven through its real widgets, offscreen | X11, UDP 32100 |
| `x11` | capture, display connection, and window catalog against a real X server | X11 |
| `wayland` | the portal handshake, PipeWire negotiation, and delivered frames against a real portal | a Wayland session with a desktop portal |
| `pulse-audio` | capture and silent-output routing against a private sound server | `pulseaudio` or `pipewire-pulse` |

- `ctest -L unit` runs only the suites that need no display, no daemon and no
  fixed port: `core` needs nothing at all and `transport` needs only loopback
  on an arbitrary port, so both are safe to run anywhere and in parallel.
- Suites that bind UDP 32100 share a ctest `RESOURCE_LOCK`, so `ctest -j` will not
  make them skip each other out. A machine already using that port skips them.
- That lock only covers one ctest invocation. Do not run two at once against the
  same machine: the second one's receiver answers the first one's `CMD_INIT`, and
  the port-bound suites then **fail rather than skip**, because `GTEST_SKIP()`
  inside a helper such as `bindReceiver` returns from the helper and leaves the
  test body running against an unbound receiver. Reproduce by holding 32100 with
  anything that replies while `ctest -R session` runs. Fixing it properly means
  guarding all 47 `bindReceiver` call sites — with `if (IsSkipped()) return;`, or
  by moving the bind into a `SetUp()` where `GTEST_SKIP()` does stop the test.
- `x11`, `wayland`, and `pulse-audio` exit 77 (a ctest skip) when there is no X
  server, no Wayland session with a portal, or no sound server; everything else
  must pass everywhere.
- The `pulse-audio` suite runs against whichever sound server the machine has,
  because both implement the protocol the capture code speaks:
  `MISTERCAST_TEST_SOUND_SERVER=pulseaudio|pipewire` forces one. The
  pipewire-pulse flavour must start `wireplumber` too — pipewire-pulse keeps the
  default-sink choice in session-manager metadata, and without one
  `set-default-sink` answers "Not supported" and every silent-output test fails
  for a reason unrelated to MiSTerCast.
- Widgets carry an `objectName` matching the member name without its trailing
  underscore, so tests reach them with `findChild<QPushButton*>("streamButton")`
  instead of production accessors that exist only for testing. Keep new widgets
  named the same way.
- The audio suite starts its **own** PulseAudio server with a null sink. Never
  point a test at the developer's sound server: the silent-output feature moves
  the default sink and live playback streams, which would reroute whatever the
  machine is playing.
- Anything this suite forks (Xvfb, pulseaudio) must redirect its stdio to
  `/dev/null` and call `setsid()`. A forked daemon holding the test binary's
  stdout keeps ctest waiting for EOF long after the tests have finished.
  `BackgroundProcess` in `tests/support/subprocess.hpp` does both; start helper
  daemons through it rather than forking again by hand.
- There are two Groovy fakes. `FakeGroovyEndpoint` records every datagram so a
  transport test can assert on the traffic; `FakeMister` decodes in place and keeps
  counters. Rebuilding the second on the first was tried and reverted: the suites
  using `FakeMister` stream real full-resolution frames, roughly 850 datagrams per
  frame at 60 Hz, and routing those through the recording endpoint made the GUI
  suite fail intermittently (3 runs in 25, against 0 in 22 before and 0 in 16
  after reverting), with acknowledgements lost while the session under test
  reported errors nothing in production caused. What that experiment did *not*
  do is isolate the cause: suppressing packet accumulation alone still left 1
  failure in 14, and a non-owning-view handler that would remove the per-datagram
  allocation was never tried. So treat this as measured-flaky-when-shared rather
  than proven-unmergeable, and note the corollary — these tests are sensitive
  enough to fake overhead that a shared design must be measured, not reasoned
  about. Peak RSS was never the problem (55 MB).
- The wire vocabulary the fakes and suites assert against lives once in
  `tests/support/groovy_wire.hpp`, and is deliberately not taken from the
  production headers: reading the opcodes out of the code under test would make
  "CMD_INIT is opcode 2 in a 5-byte datagram" unfalsifiable.
- `FakeVideo`'s `CaptureGate` is the one piece of `tests/support` carrying its
  own test (`support/capture_gate_tests.cpp`, in the `core` suite, ~40 ms). Its
  contract is that `disarm()` publishes the wait predicate **under** the gate
  mutex. Writing it outside loses a wakeup, which wedges the capture thread
  inside `next()` and hangs `StreamSession::stop()` on its join instead of
  failing; the session suite would show only a 900 s timeout, and nothing here
  is a data race so ThreadSanitizer cannot see it either. The test races
  `disarm()` against gate entry over a sweep of offsets — the defect reproduces
  within single-digit trials — and prods a stuck waiter loose after a deadline
  so a regression fails in two seconds rather than hanging the suite.
- Suites reach a layer with no public header (`src/linux`, `src/gui`) by passing
  `SOURCE_TREE` to `mistercast_add_test_suite`, which puts `src/` on that test
  target's include path only. Do not make `src/` a public include directory of a
  library: that exports every internal header to every consumer.
- Xvfb refuses roughly one connection in four when a process opens several in
  quick succession; a real Xorg server never does. `tests/linux/x11_tests.cpp`
  retries for that reason, and it is a harness allowance, not a product defect.

## Coverage

```sh
cmake -S . -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DMISTERCAST_ENABLE_COVERAGE=ON
cmake --build build-coverage
ctest --test-dir build-coverage --output-on-failure
cmake --build build-coverage --target coverage   # summary + HTML + Cobertura
```

- Needs `gcovr` (`uv tool install gcovr`, `pipx install gcovr`, or `pip install
  gcovr`). Only first-party targets are instrumented, so GoogleTest stays out of
  both the build and the report.
- The `coverage` target fails below `MISTERCAST_COVERAGE_BRANCH_FLOOR`
  percent branch coverage. Raise the floor when coverage rises; do not lower it
  to make a change pass.
- Instrumented builds use `-fprofile-update=atomic` because capture, rendering,
  and audio run concurrently and racy counter updates silently lose coverage.
- Always start from a clean build directory, or delete stale `*.gcda` first;
  mismatched profile data is discarded with a warning rather than merged.

## The Wayland test rig

`packaging/docker/` carries a container with a whole miniature Wayland desktop in
it — sway headless on software GL, PipeWire, wireplumber, and
xdg-desktop-portal-wlr with its picker disabled — because the portal path cannot
be usefully faked: what breaks is the handshake a real portal answers, the format
a real compositor negotiates, and the buffers PipeWire really delivers. Run it
with `packaging/docker/wayland-test.sh [release] [ctest-regex]`.

- It needs xdg-desktop-portal-wlr **0.8 or newer**, so it defaults to Ubuntu
  26.04. Up to 0.7 the portal captures through `wlr-screencopy`, which offers it
  no acceptable format on a GPU-less headless output and refuses every `Start`;
  0.8 switched to `ext-image-copy-capture`, which works. Passing a DRM node
  through instead is not a way out — sway 1.9 segfaults on one here. On an older
  base the rig reports itself incapable and the `wayland` suite skips.
- sway needs `--unsupported-gpu`: it refuses to start when it sees the host's
  proprietary Nvidia modules through `/proc`, and nothing here touches a GPU.
- Do not repaint the desktop while the portal is streaming. It kills
  xdg-desktop-portal-wlr 0.8.1, and every session after it fails to start. The
  rig paints a known background *before* starting the portal instead, and names
  the colour in `MISTERCAST_RIG_BACKGROUND` so a test can check real pixels.
- Suites that assume a backend must pin it rather than inherit the session: this
  rig runs the whole suite, so `cli` passes `--backend x11` or clears the Wayland
  variables, and `gui` pins the session in its `main`.

## Wayland capture gotchas

- Do not use `PW_STREAM_FLAG_MAP_BUFFERS`. PipeWire derives the mapping's
  protection from the producer's `SPA_DATA_FLAG_READABLE`/`WRITABLE`, and
  xdg-desktop-portal-wlr publishes capture buffers with neither — only
  `SPA_DATA_FLAG_MAPPABLE`. The mapping then succeeds and faults with
  `SEGV_ACCERR` on the first read, from inside the copy, on a pointer that looks
  valid; `gdb` reads it happily because ptrace bypasses page protection. The
  buffers are mapped in `add_buffer` with `PROT_READ` for that reason.
- Do not answer a negotiated format with `SPA_PARAM_Buffers`. The reply
  renegotiates, which reallocates buffers, which fires `param_changed` again, and
  the churn reaches the copy with metadata that no longer describes the mapping.
  Nothing is needed from it: omitting `SPA_FORMAT_VIDEO_modifier` from the
  EnumFormat is what keeps the producer on memory buffers.
- Never trust `chunk->size` alone. Bound every copy by `maxsize - offset` too, and
  let `cropToBgra`'s own bounds check drop a buffer whose metadata disagrees with
  the negotiated format.
- The portal owns source selection. There is no way to enumerate or name a
  Wayland output, so `--monitor`, `list-monitors`, and the GUI's choosers have
  nothing to say; do not add code that pretends otherwise.
- A restore token must be a UUID or `SelectSources` fails with an error instead
  of showing a picker. `loadPortalRestoreToken` treats a file that cannot hold one
  as absent, and a rejected grant is retried once without it, so a stale
  permission costs a dialog rather than the stream.
- The token is cached permission state, not a setting: it lives in its own
  user-only file beside `config.json` so that "settings are saved only on
  request" still holds, and so a capability handle is not left group-readable.
- Capture is compositor-paced, unlike X11's pull. A still desktop produces no
  buffers, so `next()` re-delivers the last frame; and a crop change applies to
  the next frame produced, so the held frame is dropped rather than handed back at
  a geometry the caller has stopped expecting.

## X11 capture gotchas

- These apply to the X11 backend. Backend selection is a pure function of the session environment in `resolveCaptureBackend`; a Wayland session wins over a set `DISPLAY`, because that `DISPLAY` is XWayland's and it serves no desktop content.
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

- Retain the compact hierarchy: top stream/save/load/target controls, a one-row preset chooser, capture controls beside a large preview, and status/logs at the bottom.
- Selecting a modeline preset applies it immediately (there is no apply button), including live while streaming via the debounced switch. The modeline timing fields live inside the preset editor dialog, not the main window — but the widgets exist for the whole session (hidden between openings, reparented into each dialog), so values set on them keep driving validation and live switching. Custom presets are managed in the same dialog and persisted to the user configuration the moment they change, without writing the other, possibly unsaved, settings.
- The backend chooser sits above the source row and its indexes match `CaptureBackend`, so the two convert by cast. Selecting the portal refills the monitor list with a placeholder naming who does own the choice, disables both source choosers, and lets window mode start without a pre-selected window — there is nothing to select until the portal asks.
- The monitor chooser and the window chooser share one capture-source row; the capture mode decides which pair is visible. Size is enabled only for the Custom crop mode, but offset stays editable in every mode because `calculateCrop` applies it after alignment unconditionally.
- Settings are saved only on explicit request: the Save Settings button, or the save/discard/cancel prompt shown when closing with unsaved changes. Starting a stream does not persist anything, and a SIGINT/SIGTERM close never prompts.
- The stream button must visibly transition through `Starting…`, `Stop Stream`, and `Stopping…`; do not let preview traffic starve these state updates.
- Keep the diagnostic log bounded. Periodic performance counters belong in the status line; log only meaningful threshold crossings rather than repeating an overrun message every frame.
