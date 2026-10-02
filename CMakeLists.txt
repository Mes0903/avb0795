# Project composition only: dependency population precedes production targets,
# and architecture checks run before test-only targets enter the directory tree.
# Platform selection controls which native owners are compiled; it does not add
# a platform service to the portable application/core libraries.
cmake_minimum_required(VERSION 3.28)

project(avb0795 VERSION 1.0.0 LANGUAGES CXX)

option(AVB0795_BUILD_TESTS "Build avb0795 tests" ON)
option(AVB0795_ENABLE_WINDOWS_INTEGRATION "Enable Windows integration" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/Dependencies.cmake)
include(cmake/VerifySourceBoundaries.cmake)

if(WIN32)
  add_subdirectory(shaders)
endif()

add_subdirectory(src)

if(WIN32)
  include(cmake/Packaging.cmake)
endif()

# Validate the configured ownership/link graph now. The separate source target
# scans source text on demand; neither check is a replacement for the other.
avb0795_assert_target_boundaries()
avb0795_add_source_boundary_target()

if(AVB0795_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
