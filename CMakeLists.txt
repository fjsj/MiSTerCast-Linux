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
find_library(LZ4_LIBRARY lz4)
find_package(Qt6 COMPONENTS Core Gui Widgets QUIET)

add_library(mistercast-core
  src/core/types.cpp src/core/groovy_protocol.cpp src/core/config.cpp src/core/transform.cpp
  src/core/audio_ring.cpp src/core/audio_pacer.cpp src/linux/x11_display.cpp
  src/linux/x11_capture.cpp
  src/linux/x11_source_catalog.cpp src/linux/pulse_audio.cpp
  src/core/udp_pacing.cpp src/core/adaptive_timing.cpp
  src/core/groovy_transport.cpp src/core/stream_session.cpp)
add_library(MiSTerCast::core ALIAS mistercast-core)
target_include_directories(mistercast-core PUBLIC include)
target_link_libraries(mistercast-core PUBLIC Threads::Threads PkgConfig::XCB)
target_compile_options(mistercast-core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(mistercast-core PRIVATE _GNU_SOURCE MISTERCAST_VERSION="${PROJECT_VERSION}")
if(XCB_RANDR_FOUND)
  target_link_libraries(mistercast-core PRIVATE PkgConfig::XCB_RANDR)
  target_compile_definitions(mistercast-core PRIVATE MISTERCAST_HAVE_RANDR=1)
endif()
if(PULSE_FOUND)
  target_link_libraries(mistercast-core PRIVATE PkgConfig::PULSE)
  target_compile_definitions(mistercast-core PRIVATE MISTERCAST_HAVE_PULSE=1)
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

add_executable(mistercast src/cli/main.cpp)
target_link_libraries(mistercast PRIVATE mistercast-pattern)
if(Qt6_FOUND)
  target_sources(mistercast PRIVATE src/gui/main_window.cpp)
  target_link_libraries(mistercast PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
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
  add_executable(mistercast-tests tests/core_tests.cpp)
  target_link_libraries(mistercast-tests PRIVATE mistercast-pattern)
  add_test(NAME core COMMAND mistercast-tests)
  add_executable(mistercast-transport-tests
    tests/transport_tests.cpp tests/fake_groovy_endpoint.cpp)
  target_link_libraries(mistercast-transport-tests PRIVATE mistercast-core)
  add_test(NAME transport COMMAND mistercast-transport-tests)
  add_executable(mistercast-udp-pacing-tests tests/udp_pacing_tests.cpp)
  target_link_libraries(mistercast-udp-pacing-tests PRIVATE mistercast-core)
  add_test(NAME udp-pacing COMMAND mistercast-udp-pacing-tests)
  add_executable(mistercast-pattern-tests
    tests/pattern_tests.cpp tests/fake_groovy_endpoint.cpp)
  target_link_libraries(mistercast-pattern-tests PRIVATE mistercast-pattern)
  add_test(NAME pattern COMMAND mistercast-pattern-tests)
  add_executable(mistercast-transform-tests tests/transform_tests.cpp)
  target_link_libraries(mistercast-transform-tests PRIVATE mistercast-core)
  add_test(NAME transform COMMAND mistercast-transform-tests)
  add_executable(mistercast-x11-capture-tests tests/x11_capture_tests.cpp)
  target_link_libraries(mistercast-x11-capture-tests PRIVATE mistercast-core)
  find_program(XVFB_RUN xvfb-run)
  if(XVFB_RUN)
    add_test(NAME x11-capture
      COMMAND ${XVFB_RUN} -a $<TARGET_FILE:mistercast-x11-capture-tests>)
  else()
    add_test(NAME x11-capture COMMAND mistercast-x11-capture-tests)
  endif()
  set_tests_properties(x11-capture PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME cli-invalid-sampling
    COMMAND mistercast stream --target 127.0.0.1 --sampling area)
  set_tests_properties(cli-invalid-sampling PROPERTIES WILL_FAIL TRUE)
  if(Qt6_FOUND)
    add_executable(mistercast-gui-signal-tests tests/gui_signal_tests.cpp)
    target_link_libraries(mistercast-gui-signal-tests PRIVATE Threads::Threads)
    add_test(NAME gui-sigint
      COMMAND mistercast-gui-signal-tests $<TARGET_FILE:mistercast>)
  endif()
endif()
