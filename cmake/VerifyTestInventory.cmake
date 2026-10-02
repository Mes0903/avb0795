# Public developer-only inventory launcher, not an application build or test run.
# CMake owns native paths, executable selection and the CTest-to-Lua process pipe;
# Lua owns JSON/schema/profile policy. Input must come from a trusted configured
# build tree: CTest discovery can interpret its metadata, so this is not a sandbox.
cmake_minimum_required(VERSION 3.28)

set(_invoking_directory "${CMAKE_CURRENT_BINARY_DIR}")
string(ASCII 11 12 _extra_whitespace)

# Reject explicitly blank options and values that cannot safely remain one CMake
# list argument. Do not silently convert a supplied invalid value into a default;
# a narrower profile or different executable must always be an explicit choice.
function(require_value name value)
  if("${value}" MATCHES "^[ \t\r\n${_extra_whitespace}]*$")
    message(FATAL_ERROR "${name} must not be blank")
  endif()
  if("${value}" MATCHES "[;\r\n]")
    message(FATAL_ERROR "Unsupported semicolon or newline in ${name}")
  endif()
endfunction()

if(NOT DEFINED BUILD_DIRECTORY)
  message(FATAL_ERROR "Supply -DBUILD_DIRECTORY")
endif()
require_value(BUILD_DIRECTORY "${BUILD_DIRECTORY}")
cmake_path(SET _build "${BUILD_DIRECTORY}")
cmake_path(ABSOLUTE_PATH _build BASE_DIRECTORY "${_invoking_directory}")
if(NOT IS_DIRECTORY "${_build}" OR NOT EXISTS "${_build}/CTestTestfile.cmake")
  message(FATAL_ERROR "BUILD_DIRECTORY must contain CTestTestfile.cmake")
endif()
file(REAL_PATH "${_build}" _build)
require_value(BUILD_DIRECTORY "${_build}")

cmake_path(SET _tools NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../tools")
require_value(tools-directory "${_tools}")

set(_configuration_args)
if(DEFINED CONFIGURATION)
  require_value(CONFIGURATION "${CONFIGURATION}")
  list(APPEND _configuration_args -C "${CONFIGURATION}")
endif()

set(_profile_args)
if(DEFINED INVENTORY_PROFILE)
  require_value(INVENTORY_PROFILE "${INVENTORY_PROFILE}")
  list(APPEND _profile_args --profile "${INVENTORY_PROFILE}")
endif()

# Prefer explicit absolute native tools. Otherwise select the first PATH Lua and
# the CTest beside this CMake (then PATH); version probes validate that selection
# rather than searching onward until some later executable happens to succeed.
if(DEFINED LUA_EXECUTABLE)
  set(_lua "${LUA_EXECUTABLE}")
else()
  unset(_lua)
  unset(_lua CACHE)
  find_program(_lua NAMES lua lua5.5 lua5.4 PATHS ENV PATH
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
endif()

if(DEFINED CTEST_EXECUTABLE)
  set(_ctest "${CTEST_EXECUTABLE}")
else()
  cmake_path(GET CMAKE_COMMAND PARENT_PATH _cmake_bin)
  set(_ctest "${_cmake_bin}/ctest")
  if(CMAKE_HOST_WIN32)
    string(APPEND _ctest ".exe")
  endif()
  if(NOT EXISTS "${_ctest}")
    unset(_ctest)
    unset(_ctest CACHE)
    find_program(_ctest NAMES ctest PATHS ENV PATH
      NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
  endif()
endif()

foreach(_tool IN ITEMS _lua _ctest)
  require_value("${_tool}" "${${_tool}}")
  cmake_path(SET _path "${${_tool}}")
  cmake_path(IS_ABSOLUTE _path _absolute)
  if(NOT _absolute OR NOT EXISTS "${_path}" OR IS_DIRECTORY "${_path}")
    message(FATAL_ERROR "${_tool} must identify an existing absolute executable")
  endif()
  if(CMAKE_HOST_WIN32 AND NOT _path MATCHES "\\.([eE][xX][eE]|[cC][oO][mM])$")
    message(FATAL_ERROR "${_tool}: Windows requires a native executable, not a shell script")
  endif()
  set(${_tool} "${_path}")
endforeach()

execute_process(COMMAND "${_ctest}" --version RESULT_VARIABLE _status
  OUTPUT_VARIABLE _version ERROR_VARIABLE _error TIMEOUT 5 ENCODING UTF-8 COMMAND_ECHO NONE)
if(NOT "${_status}" STREQUAL "0" OR NOT _version MATCHES "^ctest version ([0-9]+\\.[0-9]+\\.[0-9]+)")
  message(FATAL_ERROR "CTest version probe failed (${_status}): ${_error}")
endif()
if(CMAKE_MATCH_1 VERSION_LESS 3.28)
  message(FATAL_ERROR "CTest 3.28 or newer is required")
endif()

execute_process(COMMAND "${_lua}" -E -v RESULT_VARIABLE _status
  OUTPUT_VARIABLE _version ERROR_VARIABLE _error TIMEOUT 5 ENCODING UTF-8 COMMAND_ECHO NONE)
set(_lua_version "${_version}${_error}")
if(NOT "${_status}" STREQUAL "0" OR NOT _lua_version MATCHES "^Lua 5\\.[45]\\.[0-9]+")
  message(FATAL_ERROR "Use stock PUC Lua 5.4.x or 5.5.x (probe status ${_status})")
endif()

# Multiple COMMAND entries form a native pipe without a shell text round-trip.
# The timeout bounds the entire pipeline. Both processes must exit normally with
# zero: a valid JSON document from a failing producer is not an accepted run.
# Buffer Lua's output until that joint result is known, so a failed producer
# cannot leave a misleading success banner on the public launcher path.
execute_process(
  COMMAND "${_ctest}" --test-dir "${_build}" --show-only=json-v1 ${_configuration_args}
  COMMAND "${_lua}" -E verify_test_inventory.lua ${_profile_args}
  WORKING_DIRECTORY "${_tools}" TIMEOUT 30 RESULTS_VARIABLE _results
  OUTPUT_VARIABLE _success ERROR_VARIABLE _failure ENCODING UTF-8 COMMAND_ECHO NONE)
if(NOT "${_results}" STREQUAL "0;0")
  set(_ctest_status missing)
  set(_lua_status missing)
  list(LENGTH _results _count)
  if(_count GREATER 0)
    list(GET _results 0 _ctest_status)
  endif()
  if(_count GREATER 1)
    list(GET _results 1 _lua_status)
  endif()
  message(FATAL_ERROR
    "Inventory pipeline failed: CTest=${_ctest_status}, Lua=${_lua_status}\n${_failure}")
endif()

string(STRIP "${_success}" _success)
message("${_success}")
