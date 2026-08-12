# MiSTerCast for Linux

MiSTerCast captures a monitor or individual application window, converts it to a selected low-resolution modeline, and streams video and system audio to the unmodified Groovy_MiSTer core over UDP port 32100.

MiSTerCast supports Ubuntu 22.04, 24.04, and 26.04 on x86-64, under both X11/Xorg
and Wayland. It chooses a capture backend from the session automatically: XCB on
an Xorg session, and the [desktop ScreenCast portal](#wayland-capture) on a
Wayland one, where an application cannot read the screen directly. Enter the
MiSTer's IPv4 address or hostname because MiSTerCast does not discover it
automatically.

## Install and build

Install Ninja, CMake, a C++17 compiler, Qt 6, XCB/RandR/SHM, PulseAudio, LZ4,
PipeWire, and sd-bus development packages:

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev \
  libxcb1-dev libxcb-composite0-dev libxcb-randr0-dev libxcb-shm0-dev libpulse-dev liblz4-dev \
  libpipewire-0.3-dev libsystemd-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests use GoogleTest. CMake fetches a pinned release unless `GTest` is installed.
Set `-DMISTERCAST_USE_SYSTEM_GTEST=ON` to require the installed package and avoid
network access. Suites that need an X server use `xvfb-run` when available. The
X11 and PulseAudio suites skip when no display or sound server is present. Run
`ctest -L unit` for suites that need neither.

The `wayland` suite needs a Wayland session with a desktop portal, which no
ordinary build machine has, so it skips too. To run it, use the headless rig —
a container with a compositor, PipeWire, and a portal in it:

```sh
./packaging/docker/wayland-test.sh            # every suite, Ubuntu 26.04
./packaging/docker/wayland-test.sh 24.04      # a different base
```

See `AGENTS.md` for `gcovr` branch coverage and the remaining test conventions.

Qt is optional at configure time so headless/build-server installations can still
build the CLI and core. Without Qt, invoking `mistercast` with no command reports
how to enable the GUI. `libpipewire-0.3-dev` and `libsystemd-dev` are optional in
the same way, and are needed together: without them Wayland capture is compiled
out and selecting it reports the missing packages.

## Run with Docker

Run MiSTerCast without the build dependencies. Docker and a graphical session,
X11 or Wayland, are all that is required:

```sh
./packaging/docker/mistercast-docker.sh                         # GUI
./packaging/docker/mistercast-docker.sh stream --target 192.168.1.50
./packaging/docker/mistercast-docker.sh pattern --target 192.168.1.50 --tone
```

The script pulls `ghcr.io/fjsj/mistercast-linux:latest` on every launch, so
published updates arrive automatically. When the pull fails it runs the local
copy, or builds the image inside a source checkout. Set `MISTERCAST_BUILD=1`
to force a local build, or `MISTERCAST_IMAGE` to run a different image.
Without a checkout, download just the script:

```sh
curl -fsSLO https://raw.githubusercontent.com/fjsj/MiSTerCast-Linux/main/packaging/docker/mistercast-docker.sh
chmod +x mistercast-docker.sh
./mistercast-docker.sh
```

The container shares the host network (UDP port 32100 and the required
1500-byte path MTU), the host IPC namespace (MIT-SHM capture), the X11
socket, and the PulseAudio or `pipewire-pulse` socket, and it runs as the
invoking user. Settings persist in the same `mistercast/config.json` under
`$XDG_CONFIG_HOME` or `~/.config` as a native install. Enter the MiSTer's
IPv4 address because `.local` mDNS names do not resolve inside the container.

On a Wayland session it also shares the session D-Bus socket and the PipeWire
socket, which is what the ScreenCast portal needs, and the screen-sharing dialog
is drawn by the host desktop as usual. The window itself is drawn through
XWayland, so `DISPLAY` must be set even there: the image carries only Qt's `xcb`
platform plugin, and no part of it speaks the Wayland protocol.

That path also runs the container with `--security-opt apparmor=unconfined`, and
only that path. Ubuntu's `dbus-daemon` enforces AppArmor D-Bus mediation and
Docker's `docker-default` profile grants none of it, so without the flag the
container's first D-Bus message is refused with `An AppArmor policy prevents
this sender from sending this message` and every later call merely reports the
connection as not connected. It is real confinement given up to reach the
portal. A native install needs none of it, and is the better choice on a machine
where that matters.

## Usage

Start the GUI with `mistercast`, or inspect and stream from a terminal:

```sh
mistercast check
mistercast list-monitors
mistercast list-modelines
mistercast stream --target 192.168.1.50
mistercast stream --target mister.local --monitor HDMI-1 --no-audio \
  --crop 4:3 --rotation none --sampling line-blend --frame-delay 0
mistercast pattern --target mister.local --content noise --tone \
  --modeline '12.336 640 662 720 784 480 488 494 525 1'
```

### CLI options

Use `mistercast stream --help` for all overrides. Overrides last for that run unless `--save` is supplied. Settings are written atomically to `$XDG_CONFIG_HOME/mistercast/config.json`, or `~/.config/mistercast/config.json`. Invalid/corrupt settings fall back to the bundled 320×240 ~60 Hz preset.

### Pattern test

`mistercast pattern` tests the protocol without capture. It does not load or change saved capture settings or initialize X11 or PulseAudio. `--content bars` (the default) shows color bars, a flashing latency square, and a frame/field marker. `--content noise` generates deterministic, changing, low-compressibility pixels to exercise UDP pacing. `--tone` adds a continuous 440 Hz stereo S16LE tone paced by elapsed monotonic time. Pattern mode accepts `--target`, `--modeline`, both interlace-buffer switches, and `--frame-delay 0..10`. SIGINT and SIGTERM close it cleanly.

### Wayland capture

On a Wayland session an application cannot read the screen for itself. MiSTerCast
asks `xdg-desktop-portal` instead, and the desktop answers with its own
screen-sharing dialog; the pixels then arrive over PipeWire. This is the only
capture path GNOME offers from Ubuntu 26.04 on, whose session has no Xorg option
left.

Three things follow from the portal owning the choice of what to share:

- **The dialog picks the source, not MiSTerCast.** `--monitor`, `list-monitors`,
  and the GUI's monitor and window choosers have nothing to name, because there
  is no way to enumerate or address a Wayland output. `mistercast check` says so
  rather than reporting an X11 failure.
- **The first stream asks; later ones do not.** MiSTerCast keeps the portal's
  restore token in `portal-token` beside `config.json`, readable only by you, and
  reuses it so the dialog appears once. Delete that file to be asked again — for
  instance to share a different screen.
- **Sharing can be revoked from the desktop.** Stopping the share from the
  system indicator ends the stream with `desktop screen sharing was stopped`.

Choose the backend explicitly with `--backend x11|portal|auto`, or in the GUI's
**Backend** control, when the automatic choice is not what you want. Capturing
through XWayland with `--backend x11` on a Wayland session generally yields a
black or empty image; the portal is the working path.

The picker is also the reason a Wayland stream starts a little differently: the
handshake waits for a person, so the GUI blocks while the dialog is up, and the
CLI prints a line telling you to answer it.

The GUI itself is not a Wayland client. Qt draws it through XWayland while
capture goes through the portal, which is why `DISPLAY` must be set even on a
Wayland session. Nothing in MiSTerCast speaks the Wayland protocol.

### Window capture

Under the portal, window capture is requested the same way but chosen in the
desktop's dialog, and some desktops offer monitors only. The rest of this section
describes X11 window capture.

To capture a window in the GUI, set **Source** to **Single window**, click
**Choose Window…**, and select a visible X11 application window. The selection
lasts for the current run. Click **Start Stream** to begin streaming. Select the
window again after restarting MiSTerCast or loading settings because X11 window
IDs are temporary and may be reused by another application.

MiSTerCast cannot capture minimized or closed windows. Window mode disables the
monitor selector but keeps the crop, rotation, sampling, audio, and preview
controls.

### Configuration reference

The GUI exposes every routine streaming setting except `syncRefresh`. The CLI accepts the overrides shown below; settings without a CLI flag can be changed in the GUI or configuration file.

| Setting | Values and default | Behavior |
| --- | --- | --- |
| `target` | IPv4 address or hostname; empty by default | Groovy_MiSTer destination on UDP port 32100. Do not append a port. CLI: `--target HOST`. |
| `captureBackend` | `auto` by default | `auto` picks `portal` on a Wayland session and `x11` otherwise; `x11` and `portal` force one. CLI: `--backend auto\|x11\|portal` (`wayland` is accepted for `portal`). GUI: `Backend`. |
| `monitor` | RandR monitor name; primary monitor by default | X11 capture source on the current `$DISPLAY`. Ignored by the portal backend, which has no way to name an output. CLI: `--monitor NAME`. |
| `modeline` | Bundled 320×240 NTSC preset by default | MiSTer output timings and transformed frame dimensions. Select/edit it in the GUI, or use `--modeline 'CLOCK HACTIVE HBEGIN HEND HTOTAL VACTIVE VBEGIN VEND VTOTAL INTERLACE'`. Clock is MHz and interlace is `0` or `1`. |
| `audio` | `true` by default | Captures stereo S16LE system playback. CLI: `--audio` or `--no-audio`. Disabled sessions explicitly negotiate audio rate/channel code `0/0` and send no audio commands. |
| `audioSink` | Default output by default | GUI audio source. Choose `MiSTerCast silent output (CRT only)` to temporarily route current and new playback away from PC speakers and into the stream, or choose a named output sink to capture its monitor without changing playback routing. |
| `preview` | `true` by default | Shows a throttled, prescaled GUI preview. It does not affect CLI output. |
| `crop` | `4:3` by default | `custom`, integer `1x`–`5x`, full `4:3`, or full `5:4` source crop. CLI: `--crop MODE`. |
| `width`, `height` | 320×240 by default | Custom crop dimensions, used directly by `custom` mode and as saved source geometry. CLI: `--size WxH`. |
| `xOffset`, `yOffset` | 0,0 by default | Crop displacement from the selected alignment anchor. CLI: `--offset X,Y`. |
| `alignment` | `center` by default | `center`, `top-left`, `top`, `top-right`, `right`, `bottom-right`, `bottom`, `bottom-left`, or `left`. CLI: `--alignment POSITION`. |
| `rotation` | `none` by default | `none`, `cw90`, `ccw90`, or `180`. CLI: `--rotation VALUE`. |
| `sampling` | `point` by default | Sender-side scaling filter: `point`, `bilinear`, or `line-blend`. Bilinear uses centered texel coordinates and matches GroovyMAME's four-neighbor filtering semantics, but it is not a large-footprint antialiasing filter. Line Blend keeps point sampling horizontally and applies an exact area-weighted filter vertically to reduce CRT line shimmer at modest CPU cost; after 90° rotation it blends source columns. GUI: `Sampling`. CLI: `--sampling MODE`. |
| `frameDelay` | `0` (automatic) by default | Selects the MiSTer raster phase. Automatic mode measures work/network time and computes a safe scanline; `1`–`10` manually select tenths of a refresh interval. CLI: `--frame-delay N`. Higher manual values trade latency margin for a later presentation phase. |
| `syncRefresh` | `true` by default | Enables MiSTer ACK/raster feedback and correction of the next frame deadline. This is currently configuration-file-only. `false` sends sync line zero and retains local refresh pacing. |
| `progressiveInterlaceBuffer` | `false` by default | For interlaced modelines, sends Groovy_MiSTer's progressive-framebuffer mode (`interlace=2`) so both output fields always read one full-height framebuffer instead of alternating field-buffer indexes. GUI: `Stable interlace (progressive framebuffer)`. CLI: `--progressive-interlace-buffer` or `--interlaced-field-buffer` to disable it. |
| `customModelines` | Empty by default | Saved named modelines loaded into the GUI preset list. A CLI modeline named `Custom` is appended when `--modeline ... --save` is used. GUI timing edits are saved as the active modeline values. |

`Save Settings` writes the active GUI values. `Load Settings` reloads the file. CLI `--save` persists that invocation's overrides.

## Audio

Audio uses PulseAudio or `pipewire-pulse` and stereo S16LE. Capture prefers
48 kHz, then falls back to 44.1 or 22.05 kHz if the selected sink rejects it.
The GUI's default and named-output choices capture the selected sink's monitor
while the PC continues playing normally.

For CRT-only sound, choose **MiSTerCast silent output (CRT only)** before starting
the stream. MiSTerCast creates a temporary null sink, makes it the default, and
moves active playback into it. This sends audio to MiSTer without playing it
through the PC speakers. When streaming stops, MiSTerCast restores the previous
default and playback routes, then removes the temporary sink. Applications that
select a hardware device directly can bypass the system default. Set those
applications to the system/default output.

## FAQ and troubleshooting

### Why does MiSTerCast say `DISPLAY is not set`?

The X11 backend was selected without an X server to talk to. On a Wayland session
leave `captureBackend` on `auto`, or pass `--backend portal`, so capture goes
through the desktop portal instead. On an Xorg session, run MiSTerCast from
inside it.

### Why does no screen-sharing dialog appear, or why does capture fail immediately?

Check that `xdg-desktop-portal` and the backend for your desktop are installed —
`xdg-desktop-portal-gnome`, `-kde`, or `-wlr`. `mistercast check` reports which
backend is selected and whether a saved permission exists. If a stale permission
is being reused for the wrong screen, delete `portal-token` beside
`config.json` and start the stream again.

### Why does the target not acknowledge `CMD_INIT`?

Check the address, confirm that Groovy_MiSTer is running, and make sure UDP port
32100 is not filtered.

### Why is there no audio?

Make sure PulseAudio or `pipewire-pulse` is running. If CRT-only mode cannot
create a sink, confirm that the server permits `module-null-sink`, or select an
existing output. A nonzero PCM level and `MiSTer audio on` confirm capture and
core negotiation, but not the TV or receiver output route.

### What should I do when a monitor disappears?

Stop streaming, run `mistercast list-monitors`, select the current output, and
restart the stream.

### Why is there a moving or intermittent tear line?

Keep `syncRefresh` enabled and leave `frameDelay` on automatic. Check that ACK
misses do not keep increasing and that the sender and FPGA frame numbers remain
adjacent. Also match the source monitor's refresh rate to the output modeline
when possible. See [X11 source-display synchronization](#x11-source-display-synchronization)
for the limits of X11 capture timing.

### Why does interlaced line parity appear to change?

Enable **Stable interlace (progressive framebuffer)**. If the artifact remains,
it comes before the core framebuffer, such as an X11 source tear, or after it,
such as display deinterlacing. It is not caused by alternating MiSTer field
buffers.

### What do `VRAM unsynced` and `VRAM synced/fb` mean?

For `VRAM unsynced`, verify the modeline and route, then check capture FPS,
stream FPS, and packet errors. `VRAM synced/fb` means the core used its
non-volatile framebuffer fallback for that sample.

### How should I configure direct Ethernet?

Use a dedicated, non-overlapping subnet without a gateway or DNS. Run
`ip route get TARGET` and confirm that it reports the Ethernet interface and its
dedicated source address.

## Technical reference

The sections below describe timing, transport, interlacing, core options, and
differences from the former Windows implementation. You do not need these
details for a normal installation.

### Raster synchronization and diagnostics

With `syncRefresh` enabled, MiSTerCast uses Groovy_MiSTer's raster feedback to schedule each frame:

1. It decodes the 13-byte ACK containing the echoed frame/scanline, current FPGA frame/scanline, VRAM synchronization and queue state, field, vblank, framebuffer fallback, and audio bit.
2. Automatic frame delay converts measured capture/transform/network time plus a 1.5 ms safety margin into a nonzero target scanline.
3. The ACK raster error corrects the next frame deadline without adding a queued frame.
4. Interlaced streams rebase their frame number and choose the field from FPGA status before transforming pixels. A modeline switch invalidates the old field phase; fields alternate from the core's deterministic reset phase until a matching post-switch ACK locks the sender back to FPGA feedback.
5. The corrected raster cycle requests one frame from the capture worker.

Every five seconds, the CLI reports sync and raster lines, sender and FPGA frame numbers, correction, compression/submission/wire times, ACK counts, VRAM/queue state, and unique unhealthy FPGA samples. Transform timing includes a 1/8 EWMA and the maximum since startup. Field-buffer interlace also reports the outgoing and FPGA fields, phase lock, and feedback-driven realignments. The GUI divides these diagnostics into video, transport, and audio rows. Percentages keep the layout compact, and tooltips show raw counters. A few ACK misses, empty-queue samples, or audio underruns can occur while buffers start. Counters that keep increasing indicate timing or transport pressure.

Video payloads larger than 32 UDP datagrams are sent in batches of at most 32, averaging 950 Mb/s. Scheduling includes each datagram's actual payload and Ethernet/IP/UDP overhead, including a short final datagram. Commands and audio are sent immediately. MiSTerCast retries partial submissions and temporary socket backpressure until the calculated wire duration plus a modeline-derived grace period of 5–100 ms expires. A failure after a blit command is fatal because the receiver would treat the next protocol command as unfinished frame data. Status labels the `TIOCOUTQ` result as an observed UDP queue high-water mark because Linux socket accounting does not equal the number of bytes on the wire.

MiSTerCast enables strict path-MTU enforcement (`IP_PMTUDISC_DO`) for every IPv4 candidate before sending protocol commands. It rejects a known connected-route MTU below 1500 because the fixed 1472-byte UDP payload requires a 1500-byte IPv4 packet. Later `EMSGSIZE` errors are reported as path-MTU failures. MiSTerCast neither changes the payload size nor relies on IPv4 fragmentation. Check tunnel, VPN, and interface MTU settings when a route is rejected.

For repeatable CPU comparisons, build and run the optional transform benchmark:

```sh
cmake --build build --target mistercast-transform-bench
./build/mistercast-transform-bench
```

It reports median Point, Bilinear, and Line Blend transform times and deterministic checksums for 960×720, 1440×1080, and 2880×2160 sources targeting 320×240. The benchmark does not include full two-dimensional Area sampling.

Synchronization adds no permanent full-frame buffer. Automatic mode keeps the upstream client's 1.5 ms safety margin. Manual frame delay changes the sub-frame phase. ACK polling has a guaranteed minimum of 2 ms and can continue across the remaining refresh-period wait.

Alternating interlaced field buffers start with a delivery reserve equal to half the vertical total. The reserve shrinks by four protocol lines after 300 unique matching ACKs when phase is locked, frame delay is automatic, VRAM is synchronized, framebuffer fallback is inactive, and the VRAM queue is not empty. It cannot fall below three-eighths of the vertical total. The first unhealthy unique ACK restores the half-field reserve. A missed ACK clears progress toward the next reduction but does not increase an already reduced reserve. Duplicate, stale, nonmatching, and pre-switch ACKs do not count. Mode switches, reconnects, manual delay, disabled synchronization, progressive output, and progressive-interlace buffering reset or bypass the adjustment. With no missed ACKs, reaching the minimum takes about 85 seconds for a 525-line 60 Hz field stream and 120 seconds for a 625-line 50 Hz field stream.

#### Stable interlaced framebuffer mode

The optional `progressiveInterlaceBuffer` mode uses the receiver's documented `interlace=2` protocol. MiSTerCast transforms and sends all `vActive` lines on every update. Groovy_MiSTer stores them in one progressive framebuffer while retaining the interlaced output modeline. Odd and even output fields use the same line-address space instead of switching between two half-height field-buffer indexes.

This mode roughly doubles the uncompressed video pixels transformed and transmitted per update. Compression can reduce the network increase, but transform time, compression time, and the automatic raster margin can grow. Framebuffer fallback can also add latency. The mode cannot synchronize X11 capture to the source monitor, repair an already torn source frame, or change flicker from the display's deinterlacer. It has no effect on progressive modelines. Leave it off for minimum work and latency. Enable it when stable interlaced line identity matters more.

#### Wayland portal capture timing

Portal capture inverts where frames come from. XCB is asked for pixels when the
raster cycle wants them; PipeWire delivers them when the compositor produces
them, and a compositor produces one only when something changed. So:

- A still desktop yields no new buffers. `next()` hands the last frame back
  again, keeping the MiSTer refreshed at the modeline rate, which is what X11
  capture does when it re-reads unchanged pixels.
- A frame can be up to one compositor refresh old before the raster cycle asks
  for it. The PC and MiSTer clocks are still independent, as under X11.
- The crop cannot be pushed to the source: the portal always sends whole frames,
  so the crop happens during the one copy out of the shared buffer. A crop change
  therefore applies to the next frame the compositor produces, not to the frame
  already in hand.
- Only mappable memory buffers are negotiated, never DMA-BUF. The EnumFormat
  advertises no modifier, which is what keeps a producer from offering GPU
  planes that would need an EGL import path.

#### X11 source-display synchronization

MiSTer raster feedback controls when MiSTerCast requests its next capture. XCB image capture is not synchronized to the source monitor's vblank, so the PC display and MiSTer remain separate physical clocks. Their phases can drift until an XCB read overlaps a source presentation. This can produce an occasional torn source frame even when ACK timing and Ethernet delivery are healthy.

When casting NTSC content to a CRT, set the PC/X11 source monitor to 59.94 Hz (60000/1001 Hz) and use a matching NTSC output modeline when possible. NTSC video uses this fractional rate. At exactly 60.00 Hz, the clocks gain or lose about one frame every 17 seconds. The application or compositor must periodically repeat or drop a frame, or MiSTerCast may capture during an update. On the CRT, this can appear as regular judder, a moving or intermittent tear line, uneven animation, or changing input latency. Larger refresh mismatches cause these problems more often. Matching the nominal rates reduces corrections but cannot phase-lock the independent PC and MiSTer oscillators or guarantee tear-free X11 capture.

For the lowest practical latency, enable the source application's low-latency VSync mode and limit its render queue to one frame when those controls are available. Double buffering can add up to one source refresh of input latency, depending on phase. Triple buffering, prerendered-frame queues, and frame generation can add more. Cap the application close to the source monitor's actual refresh and avoid unbounded or multi-frame queues. Leave application VSync off if you prefer tearing to added source-side latency. MiSTerCast does not force VSync.

Source-vblank capture would require a separate X11 presentation and timing path, such as X Present/DRI integration. It would still need to handle the independent MiSTer clock. This should be an optional latency-versus-tearing mode, not a replacement for immediate XCB capture.

#### Running without raster correction

Setting `syncRefresh` to `false` sends sync line zero and disables ACK-based raster deadline correction. Local modeline-rate pacing, capture backpressure, audio-before-video ordering, and status ACK collection remain active. The relative local wait restarts after each completed cycle, so scheduler overshoot lengthens that cycle. It also leaves the difference between the PC and MiSTer oscillators uncorrected. Expect phase drift and a moving or intermittent tear line during long runs.

Use this mode to diagnose receiver or raster-feedback behavior. An absolute cumulative PC-side deadline could prevent scheduler overshoot from accumulating, but it could not lock the PC clock to the MiSTer clock without raster feedback. Keep the default `syncRefresh` value of `true` for normal streaming.

### Differences from the legacy Windows implementation

MiSTerCast for Linux replaces the former WPF/DXGI application. It is Linux-only.

| Area | Linux implementation | Legacy Windows implementation in repository history |
| --- | --- | --- |
| Desktop/window capture | X11/Xorg through XCB and XComposite, preferring MIT-SHM with `xcb_get_image` fallback; Wayland through the `xdg-desktop-portal` ScreenCast portal and PipeWire | DXGI Desktop Duplication/D3D11 |
| User interface | Optional Qt 6 GUI plus a headless CLI | WPF frontend calling a native DLL |
| Audio | PulseAudio or `pipewire-pulse` default-sink monitor | WASAPI shared-mode loopback of the default render endpoint |
| Source selection | RandR monitor name or visible X11 application window within the selected `$DISPLAY`/screen; under Wayland, whatever the portal's own dialog grants | Numeric DXGI output index |
| Platform support | Ubuntu 22.04-26.04 x86-64, under Xorg and Wayland | Windows only |
| `syncRefresh` | Defaults to `true`, is persisted, and controls ACK/raster deadline correction; currently editable only in JSON | Hard-coded `true` and not exposed by the WPF/native interop API; it mainly guarded first-frame timing initialization |
| `frameDelay` | Defaults to automatic `0`; GUI, CLI, and JSON configurable | Initialized to automatic `0` and not exposed by the legacy frontend interop |
| Interlaced framebuffer | Field buffers by default, with an opt-in full-height progressive framebuffer exposed in GUI, CLI, and JSON | Groovy_MiSTer's protocol supports both field-buffer `interlace=1` and progressive-framebuffer `interlace=2`; RetroArch exposes the corresponding `mister_interlaced_fb` choice |
| Raster feedback | Full ACK decode, automatic sync-line calculation, FPGA frame/field alignment, and ACK-driven wait | Performed by the bundled Groovy_MiSTer `CmdBlit()`/`WaitSync()` client library |
| Preview | Limited to roughly 10 FPS and scaled before entering Qt | Windows preview callback from captured frames |

The Linux sender preserves these protocol rules: use LZ4 when available, use 1472-byte UDP payloads for an MTU of 1500, send audio before its associated video frame, retain compatible modeline and frame commands, and send `CMD_CLOSE` on orderly shutdown.

#### Windows behaviour not carried over

The Windows tree also contained the features below. They are absent from the
Linux implementation. Line references use Shane Lynch's commit `b7493f9`, the
last Windows-era commit.

**Unreachable from the Windows application.** The bundled Groovy_MiSTer client
library contained these features, but MiSTerCast had no active call path to them:

| Feature | Why it never ran |
| --- | --- |
| Delta and duplicate-frame compression, adaptive LZ4-HC (`groovymister.cpp:556-665`) | Selected by `CmdBlit`'s `matchDeltaBytes` and by compression modes 2-6. MiSTerCast called `CmdBlit(..., 15000, 0)` (`renderer_nogpu.h:318`) with `m_compression = 0x01`, so only the plain `LZ4_compress_default` branch was reachable — exactly what the Linux port does. The frame-duplicate path sits in the no-LZ4 `else` branch and so was unreachable too. |
| Joystick, PS2 keyboard and mouse back-channel (`groovymister.h:29-84`, `groovymister.cpp:837,894`) | `BindInputs`/`PollInputs` have no caller anywhere in the Windows application or frontend. |
| Registered I/O (RIO) socket path (`groovymister.cpp:22`) | Guarded by `#ifdef _WIN32`; a Windows-only API with no Linux equivalent. The ordinary non-blocking socket path is what both platforms used off Windows. |
| Network ping measurement (`groovymister.cpp:470-486`) | The ten-sample averaging loop is commented out, leaving `m_network_ping = 0`. The Linux port measures round-trip continuously from real ACKs instead. |

**Omitted from the Linux implementation.** These paths ran but had no useful effect:

| Feature | Why it was dropped |
| --- | --- |
| Congestion control (`groovymister.h:56-57`, `groovymister.cpp:647-665`) | `K_CONGESTION_TIME` is `110000`, but `DiffTime()` returns nanoseconds on Linux and raw QPC ticks on Windows, so the same constant meant 11 ms on one platform and 110 us on the other. The wait was also measured from the end of the *previous* send, and `WaitSync` already sleeps a full frame period between sends, so below roughly 90 Hz the loop exited immediately. Pacing plus the automatic sync-line lead is the real protection, and non-blocking sends handle a full socket buffer. |
| First-blit skip (`renderer_nogpu.h:290`) | Its own comment gives the reason: "so we avoid glitches while MAME loads roms". That does not apply to desktop capture. The part worth keeping — resetting the timing baseline at the first real frame — is retained. |
| Pre-`CMD_CLOSE` flush wait (`renderer_nogpu.h:132`) | Intended one frame period, but `m_period * time_sleep` yields QPC ticks fed to a nanosecond `high_resolution_clock::duration`, giving about 0.17 ms rather than 16.7 ms. It was effectively a no-op. |
| Fixed 2 ms ACK window | Replaced by polling the socket across the whole pacing wait, which is most of a frame period, so a late ACK still corrects its own frame instead of being counted as missed. |

**Windows defects corrected in the Linux implementation:**

| Defect | Effect |
| --- | --- |
| `case Rotation::CW90` fell through to `CCW90` in both rotation switches (`renderer_nogpu.h:210,240`) | 90 degrees clockwise was silently rendered as counter-clockwise. |
| `m_vsync_scanline` was computed and then a literal `0` passed to `CmdBlit` (`renderer_nogpu.h:315-318`) | The `frameDelay` setting had no effect at all. |
| Framebuffer loop ran to `pitch * m_height * 4` while indexing `% m_width` (`renderer_nogpu.h:233`) | Read past the end of the capture buffer. |
| `(uint16_t)(pDataFloat[i] * 32767)` (`AudioCapture.h:129`) | Unclamped float-to-integer conversion wraps on samples outside plus/minus 1.0. The Linux port captures S16LE natively. |
| Interlacing added a half-step source offset (`renderer_nogpu.h:206-259`) | Both fields sampled between the same source lines rather than alternating true source lines. |
| Out-of-range crop offsets | Windows clamped these into range, which is the better behaviour and is what the Linux port now does. |

### Groovy_MiSTer core settings

These are MiSTer-side options rather than MiSTerCast configuration:

| Core option | Recommended use with this sender |
| --- | --- |
| Blit at | `ASAP` provides the lowest sub-frame latency and works with the restored raster feedback. `End Line` waits for more/all frame data before blitting and is a useful diagnostic if tearing remains, at the cost of latency. |
| Jumbo frames | Keep **Off**. This sender uses a 1500-byte network MTU with 1472-byte UDP payloads and does not currently expose jumbo-MTU negotiation. |
| Volatile framebuffer | **Off** permits the core's framebuffer fallback when streamed pixels cannot safely stay ahead of the raster. Status reports this as `VRAM synced/fb`. **On** favors the volatile path and removes that protection. |
| PWM | Does not change Linux capture, UDP pacing, compression, or raster synchronization; select it for the intended MiSTer video/output hardware behavior. |
| Audio | Must be enabled/routed on the core and output device if sound is wanted. `MiSTer audio on` in sender status confirms the ACK audio bit, not the TV/receiver output route. |

## Packaging

Create a Debian package from a release build:

```sh
cpack --config build/CPackConfig.cmake -G DEB
```

The AppImage helper requires `linuxdeploy` in `PATH`:

```sh
cmake --install build --prefix AppDir/usr
./packaging/build-appimage.sh AppDir
```

## Release validation

Before a release, validate primary and secondary monitors on real MiSTer
hardware. Cover progressive and interlaced presets, crop, offsets, rotations,
preview, 48 and 44.1 kHz audio, resolution changes, and recovery from an
unreachable target. Run continuously for at least 30 minutes. Then confirm that
stopping, restarting, and exiting the application leave the core ready for
another connection.

Validate both capture backends, because they fail differently. Compare `capture`
against `video` in the counters: on the portal backend they should track each
other, and a `capture` rate well below `video` means frames are reaching the
MiSTer stale. Also confirm that the first stream shows the desktop's dialog, the
second does not, and that deleting `portal-token` brings the dialog back.

The Groovy_MiSTer wire protocol portions retain their original BSD-3-Clause lineage from GroovyMAME/Groovy_MiSTer.
