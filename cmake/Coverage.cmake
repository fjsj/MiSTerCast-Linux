# Branch-coverage reporting for the instrumented build.
#
#   cmake -S . -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
#         -DMISTERCAST_ENABLE_COVERAGE=ON
#   cmake --build build-coverage
#   ctest --test-dir build-coverage --output-on-failure
#   cmake --build build-coverage --target coverage
#
# The `coverage` target prints a per-file branch summary and writes
# coverage/index.html plus coverage.xml (Cobertura) for CI consumption.

if(NOT MISTERCAST_ENABLE_COVERAGE)
  return()
endif()

set(MISTERCAST_COVERAGE_BRANCH_FLOOR 90 CACHE STRING
  "Branch coverage percentage below which the `coverage` target fails")

find_program(GCOVR_EXECUTABLE gcovr)
if(NOT GCOVR_EXECUTABLE)
  message(WARNING
    "gcovr was not found, so the `coverage` target is unavailable. Install it "
    "with `uv tool install gcovr`, `pipx install gcovr`, or `pip install gcovr`.")
  return()
endif()

# --exclude-throw-branches / --exclude-unreachable-branches drop the implicit
# branches g++ emits for exception unwinding and for code the optimiser proved
# dead. Counting those makes a branch percentage that no test can ever move.
add_custom_target(coverage
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/coverage
  COMMAND ${GCOVR_EXECUTABLE}
    --root ${CMAKE_SOURCE_DIR}
    # Search only this build tree. Without it gcovr walks the whole source tree
    # and trips over any other build directory that is lying around.
    ${CMAKE_BINARY_DIR}
    --filter ${CMAKE_SOURCE_DIR}/src/
    --filter ${CMAKE_SOURCE_DIR}/include/
    --exclude ${CMAKE_BINARY_DIR}/_deps/
    --exclude-unreachable-branches
    --exclude-throw-branches
    --txt-metric branch
    --print-summary
    --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
    --cobertura ${CMAKE_BINARY_DIR}/coverage.xml
    --cobertura-pretty
    --fail-under-branch ${MISTERCAST_COVERAGE_BRANCH_FLOOR}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Measuring MiSTerCast branch coverage with gcovr"
  VERBATIM)
