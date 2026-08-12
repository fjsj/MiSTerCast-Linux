#!/bin/sh
# Run MiSTerCast in Docker without installing the build dependencies.
#
#   mistercast-docker.sh                          start the GUI
#   mistercast-docker.sh stream --target HOST     any mistercast CLI arguments
#
# The container shares the pieces of the host that streaming needs: the host
# network namespace (UDP 32100 and the strict 1500-byte path MTU), the host
# IPC namespace (MIT-SHM capture), the X11 socket, the PulseAudio or
# pipewire-pulse socket, and -- on a Wayland session -- the session bus and
# PipeWire socket that the ScreenCast portal grants capture through. It runs as
# the invoking user, and settings persist in the same mistercast config
# directory as a native install.
#
# On a Wayland desktop the window is drawn through XWayland while capture goes
# through the portal, so DISPLAY is still required: the image carries only Qt's
# xcb platform plugin.
#
# Environment overrides:
#   MISTERCAST_IMAGE   image to run (default ghcr.io/fjsj/mistercast-linux:latest)
#   MISTERCAST_BUILD=1 build the image from this checkout instead of pulling
set -eu

IMAGE=${MISTERCAST_IMAGE:-ghcr.io/fjsj/mistercast-linux:latest}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/../.." && pwd)

if ! command -v docker >/dev/null 2>&1; then
  echo 'docker is required: https://docs.docker.com/engine/install/ubuntu/' >&2
  exit 1
fi
if [ -z "${DISPLAY:-}" ]; then
  if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    echo 'DISPLAY is not set. MiSTerCast draws its window through XWayland even
on a Wayland session; install XWayland (usually xwayland) and log in again.' >&2
  else
    echo 'DISPLAY is not set. Log into a graphical session and run this from it.' >&2
  fi
  exit 1
fi

build_image() {
  if [ ! -f "$repo_root/CMakeLists.txt" ]; then
    echo "Cannot build: no MiSTerCast source checkout at $repo_root." >&2
    exit 1
  fi
  docker build -f "$repo_root/packaging/docker/Dockerfile" -t "$IMAGE" "$repo_root"
  "$script_dir/smoke-test.sh" "$IMAGE"
}

# :latest is a mutable tag republished from main, so pull on every launch.
# A failed pull (offline, or a locally built MISTERCAST_IMAGE) falls back to
# the local copy, and builds from source only when there is none.
if [ "${MISTERCAST_BUILD:-0}" = 1 ]; then
  build_image
elif ! docker pull "$IMAGE"; then
  if docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Pull failed; running the local copy of $IMAGE." >&2
  else
    echo "Pull failed; building $IMAGE from source instead." >&2
    build_image
  fi
fi

# Assemble "docker run" arguments in $@, prepending option by option, so paths
# with spaces survive without needing bash arrays. $@ starts as the user's
# mistercast arguments and ends as the full docker argument list.
set -- "$IMAGE" "$@"

# PulseAudio / pipewire-pulse socket, and the auth cookie when one exists.
if [ -S "${XDG_RUNTIME_DIR:-/nonexistent}/pulse/native" ]; then
  set -- -v "$XDG_RUNTIME_DIR/pulse/native:/run/mistercast/pulse-native" \
    -e PULSE_SERVER=unix:/run/mistercast/pulse-native "$@"
else
  echo 'Warning: no PulseAudio socket found; audio capture will be unavailable.' >&2
fi
if [ -f "$HOME/.config/pulse/cookie" ]; then
  set -- -v "$HOME/.config/pulse/cookie:/run/mistercast/pulse-cookie:ro" \
    -e PULSE_COOKIE=/run/mistercast/pulse-cookie "$@"
fi

# The ScreenCast portal path: the session bus carries the handshake and the
# PipeWire socket carries the pixels. Passing the session variables through is
# what makes the capture backend inside the container resolve to the portal, the
# same way it would natively. Neither the Wayland socket nor a Wayland Qt plugin
# is involved.
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${XDG_SESSION_TYPE:-}" = wayland ]; then
  bus_path=${DBUS_SESSION_BUS_ADDRESS#unix:path=}
  bus_path=${bus_path%%,*}
  if [ -S "${bus_path:-/nonexistent}" ]; then
    set -- -v "$bus_path:/run/mistercast/bus" \
      -e DBUS_SESSION_BUS_ADDRESS=unix:path=/run/mistercast/bus "$@"
  else
    echo 'Warning: no session bus socket found; Wayland screen capture will be
unavailable. Run this from inside your desktop session.' >&2
  fi
  if [ -S "${XDG_RUNTIME_DIR:-/nonexistent}/pipewire-0" ]; then
    # PipeWire looks for its socket under XDG_RUNTIME_DIR, so the directory is
    # what has to line up inside the container, not just the socket path.
    set -- -v "$XDG_RUNTIME_DIR/pipewire-0:/run/mistercast/pipewire-0" \
      -e PIPEWIRE_RUNTIME_DIR=/run/mistercast "$@"
  else
    echo 'Warning: no PipeWire socket found; Wayland screen capture will be
unavailable.' >&2
  fi
  set -- -e XDG_SESSION_TYPE=wayland -e "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}" "$@"
fi

# X11 authority file: prefer $XAUTHORITY (GDM keeps it outside $HOME).
xauth_file=${XAUTHORITY:-$HOME/.Xauthority}
if [ -f "$xauth_file" ]; then
  set -- -v "$xauth_file:/run/mistercast/xauthority:ro" \
    -e XAUTHORITY=/run/mistercast/xauthority "$@"
fi

# Allocate a TTY only when attached to one, so piping output still works.
if [ -t 0 ] && [ -t 1 ]; then
  set -- -t "$@"
fi

# The app reads $XDG_CONFIG_HOME/mistercast before ~/.config/mistercast, so
# resolve the host directory the same way and mount it where the container
# process (which has no XDG_CONFIG_HOME) will look.
config_dir=${XDG_CONFIG_HOME:-$HOME/.config}/mistercast
mkdir -p "$config_dir"

# The X11 socket directory is a fallback for X servers reachable only through
# the pathname socket; under host networking the abstract socket carries the
# connection with no mount at all. XDG_CACHE_HOME points at /tmp because $HOME
# inside the container is root-owned except for the mounted config directory.
exec docker run --rm -i \
  --network=host \
  --ipc=host \
  --user "$(id -u):$(id -g)" \
  -e DISPLAY \
  -e QT_QPA_PLATFORM=xcb \
  -e QT_XCB_GL_INTEGRATION=none \
  -e XDG_CACHE_HOME=/tmp \
  -e HOME="$HOME" \
  -v /tmp/.X11-unix:/tmp/.X11-unix:ro \
  -v "$config_dir:$HOME/.config/mistercast" \
  "$@"
