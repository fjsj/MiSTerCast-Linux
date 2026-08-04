# Test suite. Each executable covers one layer so a failure names the layer, and
# the ctest labels let a developer run just the fast ones:
#
#   ctest --test-dir build -L unit          # no display, no network, no daemon
#   ctest --test-dir build -L integration   # X11, PulseAudio, subprocesses
#
# Suites that need an X server are wrapped in xvfb-run when it is available, and
# suites that need hardware exit 77 so ctest records a skip instead of a failure.

find_program(XVFB_RUN xvfb-run)

add_library(mistercast-test-support STATIC
  support/fake_groovy_endpoint.cpp
  support/subprocess.cpp)
target_include_directories(mistercast-test-support
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mistercast-test-support PUBLIC mistercast-core)
target_compile_options(mistercast-test-support PRIVATE -Wall -Wextra -Wpedantic)
mistercast_instrument(mistercast-test-support)

# Adds one gtest executable. GTEST_MAIN links GoogleTest's own main(); suites
# that must decide whether to skip the whole binary provide their own instead.
function(mistercast_add_test_suite name)
  cmake_parse_arguments(SUITE "GTEST_MAIN" "" "SOURCES;LIBRARIES" ${ARGN})
  set(target mistercast-${name}-tests)
  add_executable(${target} ${SUITE_SOURCES})
  target_link_libraries(${target} PRIVATE
    mistercast-test-support GTest::gmock ${SUITE_LIBRARIES})
  if(SUITE_GTEST_MAIN)
    target_link_libraries(${target} PRIVATE GTest::gtest_main)
  else()
    target_link_libraries(${target} PRIVATE GTest::gtest)
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

mistercast_add_test_suite(core GTEST_MAIN SOURCES
  core/types_tests.cpp
  core/protocol_tests.cpp
  core/config_tests.cpp
  core/audio_tests.cpp
  core/adaptive_timing_tests.cpp
  core/transform_tests.cpp
  core/udp_pacing_tests.cpp)
add_test(NAME core COMMAND mistercast-core-tests)
set_tests_properties(core PROPERTIES LABELS unit)

mistercast_add_test_suite(transport GTEST_MAIN SOURCES core/transport_tests.cpp)
add_test(NAME transport COMMAND mistercast-transport-tests)
set_tests_properties(transport PROPERTIES LABELS unit TIMEOUT 300)

mistercast_add_test_suite(session GTEST_MAIN SOURCES core/session_tests.cpp)
add_test(NAME session COMMAND mistercast-session-tests)

mistercast_add_test_suite(pattern GTEST_MAIN
  SOURCES cli/pattern_tests.cpp LIBRARIES mistercast-pattern)
add_test(NAME pattern COMMAND mistercast-pattern-tests)

mistercast_add_test_suite(cli GTEST_MAIN SOURCES cli/cli_tests.cpp)
target_compile_definitions(mistercast-cli-tests PRIVATE
  MISTERCAST_EXECUTABLE="$<TARGET_FILE:mistercast>")
if(Qt6_FOUND)
  target_compile_definitions(mistercast-cli-tests PRIVATE MISTERCAST_HAVE_QT=1)
endif()
add_dependencies(mistercast-cli-tests mistercast)
if(XVFB_RUN)
  add_test(NAME cli COMMAND ${XVFB_RUN} -a $<TARGET_FILE:mistercast-cli-tests>)
else()
  add_test(NAME cli COMMAND mistercast-cli-tests)
endif()

mistercast_add_test_suite(x11 SOURCES linux/x11_tests.cpp)
if(XVFB_RUN)
  add_test(NAME x11 COMMAND ${XVFB_RUN} -a $<TARGET_FILE:mistercast-x11-tests>)
else()
  add_test(NAME x11 COMMAND mistercast-x11-tests)
endif()
set_tests_properties(x11 PROPERTIES SKIP_RETURN_CODE 77)

mistercast_add_test_suite(pulse-audio SOURCES linux/pulse_audio_tests.cpp)
if(PULSE_FOUND)
  # The suite opens its own playback stream on the private test server, so it
  # needs libpulse-simple directly rather than only through mistercast-core.
  target_link_libraries(mistercast-pulse-audio-tests PRIVATE PkgConfig::PULSE)
endif()
add_test(NAME pulse-audio COMMAND mistercast-pulse-audio-tests)
set_tests_properties(pulse-audio PROPERTIES SKIP_RETURN_CODE 77)

if(Qt6_FOUND)
  mistercast_add_test_suite(gui SOURCES gui/gui_tests.cpp
    LIBRARIES mistercast-gui)
  if(XVFB_RUN)
    add_test(NAME gui COMMAND ${XVFB_RUN} -a $<TARGET_FILE:mistercast-gui-tests>)
  else()
    add_test(NAME gui COMMAND mistercast-gui-tests)
  endif()
endif()

# StreamSession, pattern mode, and the GUI all dial the real protocol port, so
# the fake receiver has to own UDP 32100. Serialise the suites that bind it so
# `ctest -j` cannot make them skip each other out.
set(mistercast_port_bound_tests session pattern cli)
if(Qt6_FOUND)
  list(APPEND mistercast_port_bound_tests gui)
endif()
set_tests_properties(${mistercast_port_bound_tests} PROPERTIES
  RESOURCE_LOCK groovy-protocol-port)
set_tests_properties(${mistercast_port_bound_tests} x11 pulse-audio PROPERTIES
  LABELS integration TIMEOUT 900)
