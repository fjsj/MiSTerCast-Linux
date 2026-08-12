#!/bin/bash
# Brings up a headless Wayland session -- session bus, PipeWire, compositor,
# desktop portal -- and then runs the command it was given inside it. Every
# daemon is started here rather than left to D-Bus activation so that a failure
# names the daemon that failed instead of surfacing as a portal timeout.
#
# Used by packaging/docker/wayland-test.sh. Exits 77 (a ctest skip) when the
# session cannot be brought up, matching how the x11 and pulse-audio suites
# report a missing dependency rather than failing.
set -u

export XDG_RUNTIME_DIR=/run/user/0
export XDG_CURRENT_DESKTOP=sway
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
# Software GL rather than wlroots' pixman renderer, which is otherwise the
# obvious choice for a GPU-less container: under pixman, wlr-screencopy offers
# xdg-desktop-portal-wlr no format it accepts ("unable to receive a valid format
# from wlr_screencopy") and every Start is refused. llvmpipe costs CPU the rig
# can spare and produces frames the portal will actually hand over.
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

log() { printf 'rig: %s\n' "$*" >&2; }

# Waits for a condition rather than sleeping a guessed interval: on a loaded
# machine the compositor can take seconds, and a fixed sleep would either waste
# time or flake.
await() {
  local description=$1 deadline=$((SECONDS + 20))
  shift
  until "$@" >/dev/null 2>&1; do
    if [ "$SECONDS" -ge "$deadline" ]; then
      log "timed out waiting for $description"
      return 1
    fi
    sleep 0.2
  done
}

dbus-daemon --session --fork \
  --address="unix:path=$XDG_RUNTIME_DIR/bus" --print-address >/dev/null || {
  log 'cannot start a session bus'
  exit 77
}
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"

# Every daemon's output is kept rather than discarded: when the portal refuses a
# request, its own log is the only place that says why, and a CI run has no
# second chance to reproduce it.
mkdir -p /tmp/rig
pipewire >/tmp/rig/pipewire.log 2>&1 &
pipewire-pulse >/tmp/rig/pipewire-pulse.log 2>&1 &
wireplumber >/tmp/rig/wireplumber.log 2>&1 &
await 'the PipeWire socket' test -S "$XDG_RUNTIME_DIR/pipewire-0" || exit 77

# --unsupported-gpu because sway refuses to start when it sees the host's
# proprietary Nvidia modules through /proc. Nothing here touches a GPU: the
# headless backend renders with pixman on the CPU.
sway --unsupported-gpu >/tmp/rig/sway.log 2>&1 &
# The socket name is the compositor's own readiness signal; wlr-randr then
# proves the headless output actually came up.
await 'the Wayland display' \
  bash -c 'ls "$XDG_RUNTIME_DIR"/wayland-[0-9] >/dev/null 2>&1' || exit 77
export WAYLAND_DISPLAY=$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-[0-9] | head -1)")
export XDG_SESSION_TYPE=wayland
await 'a Wayland output' wlr-randr || exit 77
# Exported so the suite can repaint the desktop through swaymsg: a wlroots
# compositor only sends a screencopy frame for damage, and a rig desktop with
# nothing running on it never changes on its own.
export SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
await 'the compositor IPC socket' test -S "$SWAYSOCK" || exit 77
log "compositor up on $WAYLAND_DISPLAY: $(wlr-randr | head -1)"

# A known desktop colour, painted before the portal starts, so a test can check
# the pixels it receives and not just their dimensions. It has to happen here
# rather than from a test: repainting while xdg-desktop-portal-wlr is streaming
# kills it (0.8.1, headless), and the suite would lose every session after the
# first. Three distinct channels, so any permutation of them is visible.
export MISTERCAST_RIG_BACKGROUND=2080c0
swaymsg output '*' background "#$MISTERCAST_RIG_BACKGROUND solid_color" \
  >/dev/null 2>&1 || log 'could not paint the desktop; pixel checks will skip'

# xdg-desktop-portal-wlr up to 0.7 captures through wlr-screencopy, which offers
# it no format it accepts on a GPU-less headless output: every Start is refused
# with "unable to receive a valid format from wlr_screencopy". 0.8 switched to
# ext-image-copy-capture, which works. Passing a DRM node through instead is not
# a way out either -- sway 1.9 segfaults on one here. So the rig reports itself
# incapable and the Wayland suite skips, rather than failing for a reason that
# lies entirely outside MiSTerCast.
portal_version=$(dpkg-query -W -f='${Version}' xdg-desktop-portal-wlr 2>/dev/null)
case "$portal_version" in
  0.[0-7]*)
    export MISTERCAST_RIG_NO_SCREENCOPY=1
    log "xdg-desktop-portal-wlr $portal_version cannot stream headlessly"
    ;;
esac

/usr/libexec/xdg-desktop-portal-wlr -l TRACE >/tmp/rig/portal-wlr.log 2>&1 &
/usr/libexec/xdg-desktop-portal >/tmp/rig/portal.log 2>&1 &
await 'the ScreenCast portal' \
  bash -c 'busctl --user introspect org.freedesktop.portal.Desktop \
             /org/freedesktop/portal/desktop \
             org.freedesktop.portal.ScreenCast' || exit 77
log 'ScreenCast portal is answering'

status=0
"$@" || status=$?
# Printed on failure only, so a passing run stays readable but a failing one
# carries the portal's own explanation of what it refused.
if [ "$status" -ne 0 ]; then
  for logfile in /tmp/rig/*.log; do
    [ -s "$logfile" ] || continue
    log "--- $logfile ---"
    tail -40 "$logfile" >&2
  done
fi
exit "$status"
