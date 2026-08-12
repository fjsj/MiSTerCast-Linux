cmake_minimum_required(VERSION 3.16)
project(MiSTerCast VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-shm xcb-composite)
pkg_check_modules(XCB_RANDR IMPORTED_TARGET xcb-randr)
pkg_check_modules(PULSE IMPORTED_TARGET libpulse-simple)
# Wayland capture needs both halves: sd-bus to talk to the ScreenCast portal and
# PipeWire to read the pixels it grants. Neither alone is usable, so they are
# checked as one optional feature.
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SDBUS IMPORTED_TARGET libsystemd)
find_library(LZ4_LIBRARY lz4)
find_package(Qt6 COMPONENTS Core Gui Widgets QUIET)

option(MISTERCAST_ENABLE_COVERAGE
  "Instrument MiSTerCast targets for gcov line/branch coverage" OFF)
option(MISTERCAST_USE_SYSTEM_GTEST
  "Require an installed GTest instead of fetching a pinned release" OFF)

# Applied only to first-party targets, so GoogleTest and other dependencies stay
# out of the coverage report and out of the instrumented build.
# -fprofile-update=atomic matters here: capture, rendering, and audio run on
# concurrent threads, and racy counter updates silently lose coverage.
function(mistercast_instrument target)
  if(MISTERCAST_ENABLE_COVERAGE)
    target_compile_options(${target} PRIVATE
      --coverage -fprofile-update=atomic -O0 -g)
    target_link_options(${target} PUBLIC --coverage)
  endif()
endfunction()

add_library(mistercast-core
  src/core/types.cpp src/core/groovy_protocol.cpp src/core/config.cpp src/core/transform.cpp
  src/core/audio_ring.cpp src/core/audio_pacer.cpp src/linux/x11_display.cpp
  src/linux/x11_capture.cpp
  src/linux/x11_source_catalog.cpp src/linux/pulse_audio.cpp
  src/linux/capture_backend.cpp src/linux/portal_capture.cpp
  src/core/udp_pacing.cpp src/core/adaptive_timing.cpp
  src/core/groovy_transport.cpp src/core/stream_session.cpp)
add_library(MiSTerCast::core ALIAS mistercast-core)
target_include_directories(mistercast-core PUBLIC include)
target_link_libraries(mistercast-core PUBLIC Threads::Threads PkgConfig::XCB)
target_compile_options(mistercast-core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(mistercast-core PRIVATE _GNU_SOURCE MISTERCAST_VERSION="${PROJECT_VERSION}")
mistercast_instrument(mistercast-core)
if(XCB_RANDR_FOUND)
  target_link_libraries(mistercast-core PRIVATE PkgConfig::XCB_RANDR)
  target_compile_definitions(mistercast-core PRIVATE MISTERCAST_HAVE_RANDR=1)
endif()
if(PULSE_FOUND)
  target_link_libraries(mistercast-core PRIVATE PkgConfig::PULSE)
  target_compile_definitions(mistercast-core PRIVATE MISTERCAST_HAVE_PULSE=1)
endif()
if(PIPEWIRE_FOUND AND SDBUS_FOUND)
  # The portal client includes <systemd/sd-bus.h> unguarded, so it is added with
  # the feature rather than compiled to nothing without it.
  target_sources(mistercast-core PRIVATE src/linux/portal_screencast.cpp)
  target_link_libraries(mistercast-core
    PRIVATE PkgConfig::PIPEWIRE PkgConfig::SDBUS)
  target_compile_definitions(mistercast-core PRIVATE MISTERCAST_HAVE_PORTAL=1)
else()
  message(WARNING
    "libpipewire-0.3 or libsystemd was not found: Wayland capture through the "
    "ScreenCast portal is disabled, leaving X11/Xorg as the only capture "
    "backend. Install libpipewire-0.3-dev and libsystemd-dev and reconfigure.")
endif()
if(LZ4_LIBRARY)
  target_link_libraries(mistercast-core PRIVATE ${LZ4_LIBRARY})
  target_compile_definitions(mistercast-core PRIVATE MISTERCAST_HAVE_LZ4=1)
else()
  message(WARNING
    "liblz4 was not found: frames will be sent uncompressed, using roughly 3-5x "
    "the network bandwidth. Install liblz4-dev and reconfigure.")
endif()

add_library(mistercast-pattern src/cli/pattern.cpp)
target_include_directories(mistercast-pattern PUBLIC include)
target_link_libraries(mistercast-pattern PUBLIC mistercast-core)
target_compile_options(mistercast-pattern PRIVATE -Wall -Wextra -Wpedantic)
mistercast_instrument(mistercast-pattern)

add_executable(mistercast src/cli/main.cpp)
target_link_libraries(mistercast PRIVATE mistercast-pattern)
mistercast_instrument(mistercast)
if(Qt6_FOUND)
  # A library rather than a source file on the executable, so the window can be
  # constructed and driven by offscreen tests.
  add_library(mistercast-gui src/gui/main_window.cpp)
  target_link_libraries(mistercast-gui
    PUBLIC mistercast-core Qt6::Core Qt6::Gui Qt6::Widgets)
  target_compile_options(mistercast-gui PRIVATE -Wall -Wextra -Wpedantic)
  mistercast_instrument(mistercast-gui)
  target_link_libraries(mistercast PRIVATE mistercast-gui)
  target_compile_definitions(mistercast PRIVATE MISTERCAST_HAVE_QT=1)
endif()

# Explicitly built with: cmake --build build --target mistercast-transform-bench
add_executable(mistercast-transform-bench EXCLUDE_FROM_ALL
  tools/transform_bench.cpp)
target_link_libraries(mistercast-transform-bench PRIVATE mistercast-core)

configure_file(data/mistercast.desktop.in mistercast.desktop @ONLY)
install(TARGETS mistercast RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/modelines.dat DESTINATION ${CMAKE_INSTALL_DATADIR}/mistercast)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mistercast.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/mistercast.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "MiSTerCast maintainers")
if(Qt6_FOUND)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6 (>= 6.2), libxcb1, libxcb-composite0, libxcb-randr0, libxcb-shm0, libpulse0, liblz4-1")
else()
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libxcb1, libxcb-composite0, libxcb-shm0, libpulse0, liblz4-1")
endif()
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
include(CPack)

if(BUILD_TESTING)
  include(cmake/GoogleTest.cmake)
  include(cmake/Coverage.cmake)
  add_subdirectory(tests)
endif()
