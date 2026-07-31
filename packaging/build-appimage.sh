#!/bin/sh
set -eu
appdir=${1:-AppDir}
command -v linuxdeploy >/dev/null 2>&1 || { echo "linuxdeploy is required" >&2; exit 1; }
linuxdeploy --appdir "$appdir" --desktop-file "$appdir/usr/share/applications/mistercast.desktop" --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/mistercast.svg" --output appimage
