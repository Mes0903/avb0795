cmake_minimum_required(VERSION 3.28)

project(Airelinna LANGUAGES CXX)

option(AIRELINNA_BUILD_TESTS "Build Airelinna tests" ON)
option(AIRELINNA_ENABLE_WINDOWS_INTEGRATION "Enable Windows integration" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/Dependencies.cmake)
include(cmake/VerifySourceBoundaries.cmake)

add_subdirectory(src)

airelinna_assert_target_boundaries()
airelinna_add_source_boundary_target()

if(AIRELINNA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
