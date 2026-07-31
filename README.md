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

Before a release, validate primary and secondary monitors on real MiSTer hardware; progressive and interlaced presets; crop, offsets and rotations; preview; 48/44.1 kHz audio; resolution changes and unreachable-target recovery. Run continuously for at least 30 minutes, then confirm stop/restart and application termination always leave the core ready for another connection.

The Groovy_MiSTer wire protocol portions retain their original BSD-3-Clause lineage from GroovyMAME/Groovy_MiSTer.
