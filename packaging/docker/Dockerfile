# Build stage: compile MiSTerCast and produce the Debian package. Tests are
# skipped here; CI runs them natively (.github/workflows/linux.yml).
ARG BASE=ubuntu:24.04
FROM ${BASE} AS build
ARG DEBIAN_FRONTEND=noninteractive
# libgl1-mesa-dev is required for find_package(Qt6) to succeed: Qt6Gui's CMake
# config needs OpenGL, and Qt is only QUIET-optional in CMakeLists.txt.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config libgl1-mesa-dev \
      qt6-base-dev libxcb1-dev libxcb-composite0-dev libxcb-randr0-dev \
      libxcb-shm0-dev libpulse-dev liblz4-dev \
      libpipewire-0.3-dev libsystemd-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Both checks fail the build when an optional feature was quietly skipped, since
# each is only QUIET-optional in CMakeLists.txt: no Qt would produce a CLI-only
# image, and no libpipewire or libsystemd an image that cannot capture a Wayland
# desktop. Neither should ship by accident. The backend is confirmed by asking the
# binary what it would do on a Wayland session rather than by inspecting build
# files, which say different things per CMake generator.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build \
    && test -f build/libmistercast-gui.a \
    && WAYLAND_DISPLAY=wayland-0 XDG_SESSION_TYPE=wayland HOME=/tmp \
       ./build/mistercast check | grep -q 'backend: portal' \
    && cpack --config build/CPackConfig.cmake -G DEB -B /out

# Runtime stage: install the package so the image carries only the runtime
# dependencies declared in CMakeLists.txt. The Qt GUI additionally needs the
# platform plugins (qt6-qpa-plugins ships libqxcb.so), fontconfig, and a font;
# none of these are pulled in by --no-install-recommends.
#
# Only the xcb platform plugin is installed, deliberately: on a Wayland desktop
# the window is drawn through XWayland while capture goes through the host's
# ScreenCast portal, so nothing here ever speaks the Wayland protocol.
FROM ${BASE}
ARG DEBIAN_FRONTEND=noninteractive
LABEL org.opencontainers.image.source="https://github.com/fjsj/MiSTerCast-Linux" \
      org.opencontainers.image.description="MiSTerCast for Linux: X11 and Wayland capture streamed to Groovy_MiSTer over UDP"
COPY --from=build /out/*.deb /tmp/
RUN apt-get update \
    && apt-get install -y --no-install-recommends /tmp/*.deb \
         qt6-qpa-plugins fontconfig fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/* /tmp/*.deb
# No arguments starts the GUI; any arguments run the CLI (stream, pattern, ...).
ENTRYPOINT ["mistercast"]
