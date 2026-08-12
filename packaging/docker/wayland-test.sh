#!/bin/sh
# Builds MiSTerCast inside the Wayland test rig and runs the suites there, so the
# portal capture backend is exercised against a real compositor, a real PipeWire
# and a real desktop portal. Called by .github/workflows/wayland.yml and usable
# by hand:
#
#   packaging/docker/wayland-test.sh              # Ubuntu 26.04
#   packaging/docker/wayland-test.sh 24.04        # a different base
#   packaging/docker/wayland-test.sh 26.04 wayland   # one ctest suite
#
# The rig needs no GPU and no display: sway runs headless on software GL. It does
# need xdg-desktop-portal-wlr 0.8 or newer, which is why 26.04 is the default --
# on 24.04 the Wayland suite skips itself and the rest of the suites still run.
set -eu
base=${1:-26.04}
suites=${2:-}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
image="mistercast-wayland-test:$base"

docker build -f "$root/packaging/docker/Dockerfile.wayland-test" \
  --build-arg "BASE=ubuntu:$base" -t "$image" "$root"

# --shm-size because the default 64 MB is not enough for PipeWire's buffers plus
# Qt; the GUI suite otherwise fails allocating where a desktop never would.
# The source is mounted read-only and built into a container-local directory, so
# a run cannot write into the checkout.
docker run --rm --shm-size=512m \
  -v "$root:/src:ro" \
  -e "MISTERCAST_CTEST_SUITES=$suites" \
  "$image" sh -c '
set -eu
# The image ships GoogleTest, so the build never reaches the network: a rig that
# fetched a dependency would fail for a reason unrelated to what it tests.
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMISTERCAST_USE_SYSTEM_GTEST=ON
cmake --build /build
if [ -n "${MISTERCAST_CTEST_SUITES:-}" ]; then
  exec ctest --test-dir /build --output-on-failure -R "$MISTERCAST_CTEST_SUITES"
fi
exec ctest --test-dir /build --output-on-failure
'
