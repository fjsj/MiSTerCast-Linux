# GoogleTest/GMock for the test suite.
#
# An installed GTest is preferred when one exists, so distribution and offline
# builds do not need the network; otherwise a pinned release archive is fetched.
# Configure with -DMISTERCAST_USE_SYSTEM_GTEST=ON to make the installed package
# mandatory and never reach the network.

set(MISTERCAST_GTEST_VERSION 1.15.2)
set(MISTERCAST_GTEST_SHA256
  7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)

if(MISTERCAST_USE_SYSTEM_GTEST)
  find_package(GTest REQUIRED)
  return()
endif()

include(FetchContent)

# gtest_force_shared_crt is a no-op on Linux but keeps the option explicit if the
# suite is ever built elsewhere; INSTALL_GTEST keeps `cmake --install` and the
# .deb package free of test headers and libraries.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

# Prefer an installed GTest, fetch only when there is none. This is what
# FetchContent's FIND_PACKAGE_ARGS does, spelled out by hand because that option
# needs CMake 3.24 and the project supports 3.16 — and doing it in one code path
# means the supported-version range is not two configurations to reason about.
find_package(GTest QUIET)
if(GTest_FOUND)
  return()
endif()

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v${MISTERCAST_GTEST_VERSION}.tar.gz
  URL_HASH SHA256=${MISTERCAST_GTEST_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

# The fetched build is a dependency, not first-party code: keep its warnings out
# of the project's own -Wall -Wextra -Wpedantic output.
foreach(target gtest gtest_main gmock gmock_main)
  if(TARGET ${target})
    get_target_property(includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(includes)
      set_target_properties(${target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${includes}")
    endif()
  endif()
endforeach()
