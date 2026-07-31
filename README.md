# MiSTerCast for Linux

MiSTerCast captures an X11 monitor, converts it to a selected low-resolution modeline, and streams video and system audio to the unmodified Groovy_MiSTer core over UDP port 32100.

The first Linux release supports Ubuntu 22.04 x86-64 under **X11/Xorg only**. Native Wayland capture is intentionally unsupported. An XWayland display is usable only when it exposes the desktop content; otherwise log into an “Ubuntu on Xorg” session. The application does not discover a MiSTer: enter its IPv4 address or hostname explicitly.

## Build

Install Ninja, CMake, a C++17 compiler, Qt 6, XCB/RandR/SHM, PulseAudio, and LZ4 development packages:

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev \
  libxcb1-dev libxcb-randr0-dev libxcb-shm0-dev libpulse-dev liblz4-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Qt is optional at configure time so headless/build-server installations can still build the CLI and core. Without Qt, invoking `mistercast` with no command reports how to enable the GUI.

## Use

Start the GUI with `mistercast`, or inspect and stream from a terminal:

```sh
mistercast check
mistercast list-monitors
mistercast list-modelines
mistercast stream --target 192.168.1.50
mistercast stream --target mister.local --monitor HDMI-1 --no-audio \
  --crop 4:3 --rotation none --frame-delay 0
```

Use `mistercast stream --help` for all overrides. Overrides last for that run unless `--save` is supplied. Settings are written atomically to `$XDG_CONFIG_HOME/mistercast/config.json`, or `~/.config/mistercast/config.json`. Invalid/corrupt settings fall back to the bundled 320×240 ~60 Hz preset.

### Settings reference

The GUI exposes every routine streaming setting except `syncRefresh`. The CLI accepts the overrides shown below; settings without a CLI flag can be changed in the GUI or configuration file.

| Setting | Values and default | Behavior |
| --- | --- | --- |
| `target` | IPv4 address or hostname; empty by default | Groovy_MiSTer destination on UDP port 32100. Do not append a port. CLI: `--target HOST`. |
| `monitor` | RandR monitor name; primary monitor by default | X11 capture source on the current `$DISPLAY`. CLI: `--monitor NAME`. |
| `modeline` | Bundled 320×240 NTSC preset by default | MiSTer output timings and transformed frame dimensions. Select/edit it in the GUI, or use `--modeline 'CLOCK HACTIVE HBEGIN HEND HTOTAL VACTIVE VBEGIN VEND VTOTAL INTERLACE'`. Clock is MHz and interlace is `0` or `1`. |
| `audio` | `true` by default | Captures stereo S16LE system playback. CLI: `--audio` or `--no-audio`. |
| `audioSink` | Default output by default | GUI audio source. Choose `MiSTerCast silent output (CRT only)` to temporarily route current and new playback away from PC speakers and into the stream, or choose a named output sink to capture its monitor without changing playback routing. |
| `preview` | `true` by default | Shows a throttled, prescaled GUI preview. It does not affect CLI output. |
| `crop` | `4:3` by default | `custom`, integer `1x`–`5x`, full `4:3`, or full `5:4` source crop. CLI: `--crop MODE`. |
| `width`, `height` | 320×240 by default | Custom crop dimensions, used directly by `custom` mode and as saved source geometry. CLI: `--size WxH`. |
| `xOffset`, `yOffset` | 0,0 by default | Crop displacement from the selected alignment anchor. CLI: `--offset X,Y`. |
| `alignment` | `center` by default | `center`, `top-left`, `top`, `top-right`, `right`, `bottom-right`, `bottom`, `bottom-left`, or `left`. CLI: `--alignment POSITION`. |
| `rotation` | `none` by default | `none`, `cw90`, `ccw90`, or `180`. CLI: `--rotation VALUE`. |
| `frameDelay` | `0` (automatic) by default | Selects the MiSTer raster phase. Automatic mode measures work/network time and computes a safe scanline; `1`–`10` manually select tenths of a refresh interval. CLI: `--frame-delay N`. Higher manual values trade latency margin for a later presentation phase. |
| `syncRefresh` | `true` by default | Enables MiSTer ACK/raster feedback and correction of the next frame deadline. This is currently configuration-file-only. `false` sends sync line zero and retains local refresh pacing. |
| `progressiveInterlaceBuffer` | `false` by default | For interlaced modelines, sends Groovy_MiSTer's progressive-framebuffer mode (`interlace=2`) so both output fields always read one full-height framebuffer instead of alternating field-buffer indexes. GUI: `Stable interlace (progressive framebuffer)`. CLI: `--progressive-interlace-buffer` or `--interlaced-field-buffer` to disable it. |
| `customModelines` | Empty by default | Saved named modelines loaded into the GUI preset list. A CLI modeline named `Custom` is appended when `--modeline ... --save` is used. GUI timing edits are saved as the active modeline values. |

`Save Settings` writes the active GUI values. `Load Settings` reloads the file. CLI `--save` persists that invocation's overrides.

### Raster synchronization and diagnostics

With `syncRefresh` enabled, MiSTerCast implements the Groovy_MiSTer client timing loop rather than free-running the PC and MiSTer clocks independently:

1. It decodes the 13-byte ACK containing the echoed frame/scanline, current FPGA frame/scanline, VRAM state, field, vblank, framebuffer fallback, and audio bit.
2. Automatic frame delay converts measured capture/transform/network time plus a 1.5 ms safety margin into a nonzero target scanline.
3. The ACK raster error corrects the next frame deadline without adding a queued frame.
4. Interlaced streams rebase their frame number and choose the field from FPGA status before transforming pixels.
5. The capture worker takes one frame when requested by the corrected raster cycle; it does not free-run on a second independent refresh clock or flood full-resolution captures.

The CLI reports the requested sync line, current raster line, sender/FPGA frame numbers, correction and stream times, matched/missed ACKs, and VRAM state every five seconds. The GUI shows a compact subset in its status line. A small number of startup ACK misses or audio underrun samples can occur while buffers start; counters that continue increasing indicate a real timing or transport problem.

Synchronization does not introduce a permanent full-frame buffer. Automatic mode deliberately keeps the 1.5 ms safety margin used by the upstream client. Manual frame delay changes sub-frame phase. ACK acquisition is bounded to 2 ms and is accounted inside the existing refresh-period wait.

#### Stable interlaced framebuffer mode

The optional `progressiveInterlaceBuffer` mode uses the receiver's documented `interlace=2` protocol. MiSTerCast transforms and transmits all `vActive` lines on every update, and Groovy_MiSTer stores them in one progressive framebuffer while retaining the interlaced output modeline. This guarantees that MiSTerCast and the core do not switch between two half-height field-buffer indexes during that stream; odd and even output fields are derived from the same line-address space.

This mode roughly doubles the uncompressed video pixels transformed and transmitted per update. Compression can reduce the network increase, but transform/compression time and the automatically selected safe raster margin can grow, and framebuffer fallback can add latency. It does not synchronize X11 capture to the source monitor, repair a source frame that was already torn, or change flicker introduced by the display's deinterlacer. It has no effect on progressive modelines. Leave it off for minimum work/latency; enable it when stable interlaced line identity matters more.

#### X11 source-display synchronization

MiSTer raster feedback controls when MiSTerCast requests its next capture, but XCB image capture is not synchronized to the source monitor's vblank. The PC display and MiSTer therefore remain separate physical clocks. Their phase can drift until an XCB read overlaps a source presentation, producing an occasional source-side torn frame even when ACK timing and Ethernet delivery are healthy.

For the lowest practical latency, enable the source application's low-latency VSync mode and keep its render queue at one frame if those controls are available. A compositor or application mode that uses double buffering can add anywhere from nearly zero to one source refresh of input latency depending on phase; triple buffering, prerendered-frame queues, and frame-generation features can add more. Cap the application close to the source monitor's actual refresh and avoid unbounded or multi-frame queues. If tearing is preferable to added source-side latency, leave application VSync off; MiSTerCast itself does not force it.

True source-vblank capture would require a different, X11-specific presentation/timing path (for example X Present/DRI integration) and would still need to handle the independent MiSTer clock. Such a mode should remain an explicit latency/tearing tradeoff rather than replacing the current immediate XCB capture path by default.

#### Running without raster correction

Setting `syncRefresh` to `false` sends sync line zero and disables ACK-based raster deadline correction. It does not disable local modeline-rate pacing, capture backpressure, audio-before-video ordering, or status ACK collection. The local wait is relative and re-anchors after each completed cycle, so scheduler overshoot lengthens that cycle; the independent PC and MiSTer oscillator error is also left uncorrected. Phase drift and a moving or intermittent tear line are therefore expected over a long run.

Use this mode to diagnose receiver/raster-feedback behavior or to opt out deliberately, not as the normal low-tearing configuration. An absolute cumulative PC-side deadline could prevent scheduler overshoot from accumulating, but it could not lock the PC clock to the MiSTer clock without raster feedback. Keep `syncRefresh` at its default `true` for normal streaming.

### Differences from the legacy Windows implementation

This repository is a Linux replacement, not a cross-platform continuation of the removed WPF/DXGI application.

| Area | Linux implementation | Legacy Windows implementation in repository history |
| --- | --- | --- |
| Desktop capture | X11/Xorg through XCB, preferring MIT-SHM with `xcb_get_image` fallback | DXGI Desktop Duplication/D3D11 |
| User interface | Optional Qt 6 GUI plus a headless CLI | WPF frontend calling a native DLL |
| Audio | PulseAudio or `pipewire-pulse` default-sink monitor | WASAPI shared-mode loopback of the default render endpoint |
| Monitor selection | RandR monitor name within the selected `$DISPLAY`/screen | Numeric DXGI output index |
| Platform support | Ubuntu 22.04 x86-64 under Xorg; no native Wayland path | Windows only |
| `syncRefresh` | Defaults to `true`, is persisted, and controls ACK/raster deadline correction; currently editable only in JSON | Hard-coded `true` and not exposed by the WPF/native interop API; it mainly guarded first-frame timing initialization |
| `frameDelay` | Defaults to automatic `0`; GUI, CLI, and JSON configurable | Initialized to automatic `0` and not exposed by the legacy frontend interop |
| Interlaced framebuffer | Field buffers by default, with an opt-in full-height progressive framebuffer exposed in GUI, CLI, and JSON | Groovy_MiSTer's protocol supports both field-buffer `interlace=1` and progressive-framebuffer `interlace=2`; RetroArch exposes the corresponding `mister_interlaced_fb` choice |
| Raster feedback | Full ACK decode, automatic sync-line calculation, FPGA frame/field alignment, and ACK-driven wait | Performed by the bundled Groovy_MiSTer `CmdBlit()`/`WaitSync()` client library |
| Preview | Limited to roughly 10 FPS and scaled before entering Qt | Windows preview callback from captured frames |

The Linux sender preserves the shared wire invariants: LZ4 when available, 1472-byte UDP payloads for an MTU of 1500, audio before its associated video frame, protocol-compatible modeline/frame commands, and `CMD_CLOSE` on orderly shutdown.

### Groovy_MiSTer core settings

These are MiSTer-side options rather than MiSTerCast configuration:

| Core option | Recommended use with this sender |
| --- | --- |
| Blit at | `ASAP` provides the lowest sub-frame latency and works with the restored raster feedback. `End Line` waits for more/all frame data before blitting and is a useful diagnostic if tearing remains, at the cost of latency. |
| Jumbo frames | Keep **Off**. This sender uses a 1500-byte network MTU with 1472-byte UDP payloads and does not currently expose jumbo-MTU negotiation. |
| Volatile framebuffer | **Off** permits the core's framebuffer fallback when streamed pixels cannot safely stay ahead of the raster. Status reports this as `VRAM synced/fb`. **On** favors the volatile path and removes that protection. |
| PWM | Does not change Linux capture, UDP pacing, compression, or raster synchronization; select it for the intended MiSTer video/output hardware behavior. |
| Audio | Must be enabled/routed on the core and output device if sound is wanted. `MiSTer audio on` in sender status confirms the ACK audio bit, not the TV/receiver output route. |

## Audio

Audio uses PulseAudio or `pipewire-pulse`, stereo S16LE at 48 kHz. The GUI's default and named-output choices capture that sink's monitor while the PC continues playing normally.

For CRT-only sound, choose **MiSTerCast silent output (CRT only)** before starting the stream. MiSTerCast creates a temporary null sink, makes it the default, and moves active playback into it, so its audio is captured for MiSTer without reaching PC speakers. On stop, the prior default and active-stream routes are restored and the temporary sink is removed. Applications that explicitly force a hardware device can bypass the system default; set those applications to the system/default output.

## Packages

Create a Debian package from a release build:

```sh
cpack --config build/CPackConfig.cmake -G DEB
```

The AppImage helper requires `linuxdeploy` in `PATH`:

```sh
cmake --install build --prefix AppDir/usr
./packaging/build-appimage.sh AppDir
```

## Troubleshooting and hardware validation

- `DISPLAY is not set`: log into Xorg and run from that session.
- `target did not acknowledge CMD_INIT`: verify the address, Groovy_MiSTer is running, and UDP/32100 is not filtered.
- Audio errors: ensure PulseAudio/pipewire-pulse is running. If CRT-only mode cannot create a sink, verify that the server permits `module-null-sink`; otherwise select an existing output.
- Monitor disappeared: stop, run `list-monitors`, select the current output, and restart.
- Moving or intermittent tear line: keep `syncRefresh` enabled and `frameDelay` at automatic first. Check that ACK misses do not keep increasing and that sender/FPGA frame numbers remain adjacent.
- Interlaced line parity/index appears to change: enable `Stable interlace (progressive framebuffer)`. If the artifact remains, it originates before the core framebuffer (for example an X11 source tear) or after it (display deinterlacing), rather than from alternating MiSTer field buffers.
- `VRAM unsynced`: verify the modeline and route, then check capture/stream FPS and packet errors. `VRAM synced/fb` means the core used its non-volatile framebuffer fallback for that sample.
- Direct Ethernet: use a dedicated non-overlapping subnet without gateway or DNS, and confirm `ip route get TARGET` names the Ethernet interface and its dedicated source address.

Before a release, validate primary and secondary monitors on real MiSTer hardware; progressive and interlaced presets; crop, offsets and rotations; preview; 48/44.1 kHz audio; resolution changes and unreachable-target recovery. Run continuously for at least 30 minutes, then confirm stop/restart and application termination always leave the core ready for another connection.

The Groovy_MiSTer wire protocol portions retain their original BSD-3-Clause lineage from GroovyMAME/Groovy_MiSTer.
