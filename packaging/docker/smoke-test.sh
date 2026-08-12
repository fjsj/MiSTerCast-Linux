#!/bin/sh
# Verify a MiSTerCast image runs headless. Called by the Docker workflow and
# by mistercast-docker.sh after a local build, so both paths test the same way.
set -eu
image=${1:?usage: smoke-test.sh IMAGE}
docker run --rm "$image" --help >/dev/null
docker run --rm "$image" list-modelines >/dev/null
# The GUI must construct and stay alive on the offscreen platform: timeout
# status 124 proves the Qt runtime (QPA plugin, fonts) is complete. A CLI-only
# binary exits 2 immediately, so this check fails closed.
docker run --rm -e QT_QPA_PLATFORM=offscreen -e HOME=/tmp --entrypoint sh "$image" \
  -c 'timeout 3 mistercast; test "$?" -eq 124'
echo "smoke test passed: $image"
