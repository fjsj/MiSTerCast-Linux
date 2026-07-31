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
| `preview` | `true` by default | Shows a throttled, prescaled GUI preview. It does not affect CLI output. |
| `crop` | `4:3` by default | `custom`, integer `1x`–`5x`, full `4:3`, or full `5:4` source crop. CLI: `--crop MODE`. |
| `width`, `height` | 320×240 by default | Custom crop dimensions, used directly by `custom` mode and as saved source geometry. CLI: `--size WxH`. |
| `xOffset`, `yOffset` | 0,0 by default | Crop displacement from the selected alignment anchor. CLI: `--offset X,Y`. |
| `alignment` | `center` by default | `center`, `top-left`, `top`, `top-right`, `right`, `bottom-right`, `bottom`, `bottom-left`, or `left`. CLI: `--alignment POSITION`. |
| `rotation` | `none` by default | `none`, `cw90`, `ccw90`, or `180`. CLI: `--rotation VALUE`. |
| `frameDelay` | `0` (automatic) by default | Selects the MiSTer raster phase. Automatic mode measures work/network time and computes a safe scanline; `1`–`10` manually select tenths of a refresh interval. CLI: `--frame-delay N`. Higher manual values trade latency margin for a later presentation phase. |
| `syncRefresh` | `true` by default | Enables MiSTer ACK/raster feedback and correction of the next frame deadline. This is currently configuration-file-only. `false` sends sync line zero and retains local refresh pacing. |
| `customModelines` | Empty by default | Saved named modelines loaded into the GUI preset list. A CLI modeline named `Custom` is appended when `--modeline ... --save` is used. GUI timing edits are saved as the active modeline values. |

`Save Settings` writes the active GUI values. `Load Settings` reloads the file. CLI `--save` persists that invocation's overrides.

### Raster synchronization and diagnostics

With `syncRefresh` enabled, MiSTerCast implements the Groovy_MiSTer client timing loop rather than free-running the PC and MiSTer clocks independently:

1. It decodes the 13-byte ACK containing the echoed frame/scanline, current FPGA frame/scanline, VRAM state, field, vblank, framebuffer fallback, and audio bit.
2. Automatic frame delay converts measured capture/transform/network time plus a 1.5 ms safety margin into a nonzero target scanline.
3. The ACK raster error corrects the next frame deadline without adding a queued frame.
4. Interlaced streams rebase their frame number and choose the field from FPGA status before transforming pixels.

The CLI reports the requested sync line, current raster line, sender/FPGA frame numbers, correction and stream times, matched/missed ACKs, and VRAM state every five seconds. The GUI shows a compact subset in its status line. A small number of startup ACK misses or audio underrun samples can occur while buffers start; counters that continue increasing indicate a real timing or transport problem.

Synchronization does not introduce a permanent full-frame buffer. Automatic mode deliberately keeps the 1.5 ms safety margin used by the upstream client. Manual frame delay changes sub-frame phase. ACK acquisition is bounded to 2 ms and is accounted inside the existing refresh-period wait.

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

Audio uses PulseAudio or `pipewire-pulse`, stereo S16LE at 48 kHz. Select the default output device's **monitor** as MiSTerCast's recording source (for example in `pavucontrol`). Use `--no-audio` if no monitor source is available.

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
- Audio errors: ensure PulseAudio/pipewire-pulse is running and select the sink monitor source.
- Monitor disappeared: stop, run `list-monitors`, select the current output, and restart.
- Moving or intermittent tear line: keep `syncRefresh` enabled and `frameDelay` at automatic first. Check that ACK misses do not keep increasing and that sender/FPGA frame numbers remain adjacent.
- `VRAM unsynced`: verify the modeline and route, then check capture/stream FPS and packet errors. `VRAM synced/fb` means the core used its non-volatile framebuffer fallback for that sample.
- Direct Ethernet: use a dedicated non-overlapping subnet without gateway or DNS, and confirm `ip route get TARGET` names the Ethernet interface and its dedicated source address.

Before a release, validate primary and secondary monitors on real MiSTer hardware; progressive and interlaced presets; crop, offsets and rotations; preview; 48/44.1 kHz audio; resolution changes and unreachable-target recovery. Run continuously for at least 30 minutes, then confirm stop/restart and application termination always leave the core ready for another connection.

The Groovy_MiSTer wire protocol portions retain their original BSD-3-Clause lineage from GroovyMAME/Groovy_MiSTer.
