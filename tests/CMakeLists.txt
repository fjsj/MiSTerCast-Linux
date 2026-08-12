# Test suite. Each executable covers one layer so a failure names the layer, and
# the ctest labels let a developer run just the fast ones:
#
#   ctest --test-dir build -L unit          # no display, no network, no daemon
#   ctest --test-dir build -L integration   # X11, PulseAudio, subprocesses
#
# Suites that need an X server are wrapped in xvfb-run when it is available, and
# suites that need hardware exit 77 so ctest records a skip instead of a failure.

find_program(XVFB_RUN xvfb-run)
if(XVFB_RUN)
  set(MISTERCAST_XVFB ${XVFB_RUN} -a)
endif()

add_library(mistercast-test-support STATIC
  support/fake_groovy_endpoint.cpp
  support/subprocess.cpp)
target_include_directories(mistercast-test-support
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mistercast-test-support PUBLIC mistercast-core)
target_compile_options(mistercast-test-support PRIVATE -Wall -Wextra -Wpedantic)

# Adds one gtest executable and registers it with ctest:
#
#   GTEST_MAIN   link GoogleTest's own main(); omit for a suite that provides one
#                because it decides whether to skip the whole binary
#   SOURCE_TREE  put src/ on the include path, for the layers that have no public
#                header (src/linux, src/gui)
#   XVFB         run under xvfb-run when it is available
#   SKIP_77      treat exit code 77 as a ctest skip rather than a failure
#   PORT_LOCK    serialise against the other suites that must own UDP 32100
#   LABEL        ctest label; TIMEOUT ctest timeout in seconds
function(mistercast_add_test_suite name)
  cmake_parse_arguments(SUITE "GTEST_MAIN;SOURCE_TREE;XVFB;SKIP_77;PORT_LOCK"
    "LABEL;TIMEOUT" "SOURCES;LIBRARIES" ${ARGN})
  set(target mistercast-${name}-tests)
  add_executable(${target} ${SUITE_SOURCES})
  target_link_libraries(${target} PRIVATE
    mistercast-test-support GTest::gmock ${SUITE_LIBRARIES})
  if(SUITE_GTEST_MAIN)
    target_link_libraries(${target} PRIVATE GTest::gtest_main)
  else()
    target_link_libraries(${target} PRIVATE GTest::gtest)
  endif()
  # Kept on the test target rather than the libraries: making src/ a public
  # include root would export every internal header to every consumer and
  # dissolve the include/ boundary.
  if(SUITE_SOURCE_TREE)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)

  if(SUITE_XVFB)
    # Expands to nothing when xvfb-run was not found.
    add_test(NAME ${name}
      COMMAND ${MISTERCAST_XVFB} $<TARGET_FILE:${target}>)
  else()
    add_test(NAME ${name} COMMAND ${target})
  endif()
  set_tests_properties(${name} PROPERTIES LABELS ${SUITE_LABEL})
  if(SUITE_TIMEOUT)
    set_tests_properties(${name} PROPERTIES TIMEOUT ${SUITE_TIMEOUT})
  endif()
  if(SUITE_SKIP_77)
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # StreamSession, pattern mode, the CLI, and the GUI all dial the real protocol
  # port, so the fake receiver has to own UDP 32100. Serialising the suites that
  # bind it keeps `ctest -j` from making them skip each other out.
  if(SUITE_PORT_LOCK)
    set_tests_properties(${name} PROPERTIES RESOURCE_LOCK groovy-protocol-port)
  endif()
endfunction()

mistercast_add_test_suite(core GTEST_MAIN LABEL unit SOURCES
  core/types_tests.cpp
  core/protocol_tests.cpp
  core/config_tests.cpp
  core/audio_tests.cpp
  core/adaptive_timing_tests.cpp
  core/transform_tests.cpp
  core/udp_pacing_tests.cpp
  core/capture_backend_tests.cpp
  support/capture_gate_tests.cpp)

mistercast_add_test_suite(transport GTEST_MAIN LABEL unit TIMEOUT 300 SOURCES
  core/transport_open_tests.cpp
  core/transport_framing_tests.cpp
  core/transport_pacing_tests.cpp)

mistercast_add_test_suite(session GTEST_MAIN LABEL integration TIMEOUT 900
  PORT_LOCK SOURCES core/session_tests.cpp)

mistercast_add_test_suite(pattern GTEST_MAIN LABEL integration TIMEOUT 900
  PORT_LOCK SOURCES cli/pattern_tests.cpp LIBRARIES mistercast-pattern)

mistercast_add_test_suite(cli GTEST_MAIN LABEL integration TIMEOUT 900
  PORT_LOCK XVFB SOURCES cli/cli_tests.cpp)
target_compile_definitions(mistercast-cli-tests PRIVATE
  MISTERCAST_EXECUTABLE="$<TARGET_FILE:mistercast>")
if(Qt6_FOUND)
  target_compile_definitions(mistercast-cli-tests PRIVATE MISTERCAST_HAVE_QT=1)
endif()
if(PIPEWIRE_FOUND AND SDBUS_FOUND)
  # The monitor commands answer differently on a Wayland session depending on
  # whether the portal backend was built, so the suite has to know which.
  target_compile_definitions(mistercast-cli-tests PRIVATE
    MISTERCAST_HAVE_PORTAL=1)
endif()
add_dependencies(mistercast-cli-tests mistercast)

mistercast_add_test_suite(x11 LABEL integration TIMEOUT 900 XVFB SKIP_77
  SOURCE_TREE SOURCES linux/x11_tests.cpp)

# Needs a Wayland session with a desktop portal, which no CI runner and no build
# machine has by default. packaging/docker/wayland-test.sh brings one up; without
# it the suite exits 77 and CTest records a skip.
mistercast_add_test_suite(wayland LABEL integration TIMEOUT 900 SKIP_77
  SOURCE_TREE SOURCES linux/portal_capture_tests.cpp)

mistercast_add_test_suite(pulse-audio LABEL integration TIMEOUT 900 SKIP_77
  SOURCES linux/pulse_audio_tests.cpp)
if(PULSE_FOUND)
  # The suite opens its own playback stream on the private test server, so it
  # needs libpulse-simple directly rather than only through mistercast-core, and
  # it asks at compile time whether the feature under test was built at all.
  target_link_libraries(mistercast-pulse-audio-tests PRIVATE PkgConfig::PULSE)
  target_compile_definitions(mistercast-pulse-audio-tests PRIVATE
    MISTERCAST_HAVE_PULSE=1)
endif()

if(Qt6_FOUND)
  mistercast_add_test_suite(gui LABEL integration TIMEOUT 900 XVFB PORT_LOCK
    SOURCE_TREE SOURCES gui/gui_tests.cpp LIBRARIES mistercast-gui)

  # The window driving a real portal, which only the Wayland rig can host. Kept
  # out of the `gui` suite because reaching a real portal captures the screen of
  # whoever runs it; that is acceptable in the rig and nowhere else.
  mistercast_add_test_suite(gui-wayland LABEL integration TIMEOUT 900 SKIP_77
    PORT_LOCK SOURCE_TREE SOURCES gui/gui_portal_tests.cpp
    LIBRARIES mistercast-gui)
endif()
