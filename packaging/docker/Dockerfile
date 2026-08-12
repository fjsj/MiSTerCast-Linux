# Build stage: compile MiSTerCast and produce the Debian package. Tests are
# skipped here; CI runs them natively (.github/workflows/linux.yml).
FROM ubuntu:22.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
# libgl1-mesa-dev is required for find_package(Qt6) to succeed: Qt6Gui's CMake
# config needs OpenGL, and Qt is only QUIET-optional in CMakeLists.txt.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config libgl1-mesa-dev \
      qt6-base-dev libxcb1-dev libxcb-composite0-dev libxcb-randr0-dev \
      libxcb-shm0-dev libpulse-dev liblz4-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# The test on libmistercast-gui.a fails the build when Qt was not found, so a
# missing build dependency cannot silently produce a CLI-only image.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build \
    && test -f build/libmistercast-gui.a \
    && cpack --config build/CPackConfig.cmake -G DEB -B /out

# Runtime stage: install the package so the image carries only the runtime
# dependencies declared in CMakeLists.txt. The Qt GUI additionally needs the
# platform plugins (qt6-qpa-plugins ships libqxcb.so on jammy), fontconfig,
# and a font; none of these are pulled in by --no-install-recommends.
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
LABEL org.opencontainers.image.source="https://github.com/fjsj/MiSTerCast-Linux" \
      org.opencontainers.image.description="MiSTerCast for Linux: X11 capture streamed to Groovy_MiSTer over UDP"
COPY --from=build /out/*.deb /tmp/
RUN apt-get update \
    && apt-get install -y --no-install-recommends /tmp/*.deb \
         qt6-qpa-plugins fontconfig fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/* /tmp/*.deb
# No arguments starts the GUI; any arguments run the CLI (stream, pattern, ...).
ENTRYPOINT ["mistercast"]
