cmake_minimum_required(VERSION 3.28)

project(avb0795 LANGUAGES CXX)

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

avb0795_assert_target_boundaries()
avb0795_add_source_boundary_target()

if(AVB0795_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
