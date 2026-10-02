cmake_minimum_required(VERSION 3.28)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_tools "${_repo}/tools")

if(NOT DEFINED LUA_EXECUTABLE OR NOT IS_ABSOLUTE "${LUA_EXECUTABLE}")
  message(FATAL_ERROR "Supply LUA_EXECUTABLE as an absolute path")
endif()
if(NOT EXISTS "${LUA_EXECUTABLE}")
  message(FATAL_ERROR "Lua executable does not exist: ${LUA_EXECUTABLE}")
endif()

get_filename_component(_cmake_dir "${CMAKE_COMMAND}" DIRECTORY)
set(_ctest "${_cmake_dir}/ctest")
if(CMAKE_HOST_WIN32)
  string(APPEND _ctest ".exe")
endif()
if(NOT EXISTS "${_ctest}")
  message(FATAL_ERROR "CTest sibling does not exist: ${_ctest}")
endif()

file(SHA256 "${_tools}/vendor/dkjson.lua" _dkjson_sha256)
if(NOT _dkjson_sha256 STREQUAL "197cb50834c642f84b4cf99fe724932c50e6d9c92faec7ad89aa25e91df4d481")
  message(FATAL_ERROR "tools/vendor/dkjson.lua SHA-256 mismatch: ${_dkjson_sha256}")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_scratch "${_repo}/build/lua-tooling-${_suffix}")
if(EXISTS "${_scratch}")
  message(FATAL_ERROR "Refusing to reuse scratch directory: ${_scratch}")
endif()
file(MAKE_DIRECTORY "${_scratch}")
message(STATUS "Tooling scratch retained at: ${_scratch}")

function(require_exit actual expected context)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "${context}: expected exit ${expected}, got ${actual}")
  endif()
endfunction()

execute_process(
  COMMAND "${LUA_EXECUTABLE}" -E ../tests/tooling/policy_test.lua
  WORKING_DIRECTORY "${_tools}"
  TIMEOUT 10
  RESULT_VARIABLE _policy_status
  OUTPUT_VARIABLE _policy_stdout
  ERROR_VARIABLE _policy_stderr
  ENCODING UTF-8)
  require_exit("${_policy_status}" "0" "policy tests; stderr: ${_policy_stderr}")
string(STRIP "${_policy_stdout}" _policy_stdout)
if(NOT _policy_stdout STREQUAL "POLICY_OK 7 groups")
  message(FATAL_ERROR "policy tests: unexpected stdout: ${_policy_stdout}")
endif()

function(cli_case mode expected)
  set(_input "${_scratch}/${mode}.json")
  execute_process(
    COMMAND "${LUA_EXECUTABLE}" -E ../tests/tooling/emit_inventory.lua "${mode}"
    WORKING_DIRECTORY "${_tools}"
    TIMEOUT 10
    RESULT_VARIABLE _emit_status
    OUTPUT_FILE "${_input}"
    ERROR_VARIABLE _emit_stderr
    ENCODING UTF-8)
  require_exit("${_emit_status}" "0" "${mode} emitter; stderr: ${_emit_stderr}")
  execute_process(
    COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua
    WORKING_DIRECTORY "${_tools}"
    INPUT_FILE "${_input}"
    TIMEOUT 10
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    ENCODING UTF-8)
  require_exit("${_status}" "${expected}" "${mode} CLI; stderr: ${_stderr}")
  if("${expected}" STREQUAL "0")
    string(STRIP "${_stdout}" _stdout)
    if(NOT _stdout STREQUAL "CTest inventory is valid: 15 avb0795 V1 cases; hardware cases 13-15.")
      message(FATAL_ERROR "${mode} CLI: unexpected success stdout: ${_stdout}")
    endif()
  elseif(_stdout MATCHES "CTest inventory is valid:")
    message(FATAL_ERROR "${mode} CLI emitted the success banner on failure")
  endif()
endfunction()

cli_case(valid 0)
foreach(_mode IN ITEMS empty malformed oversized trailing wrong-envelope null-entry)
  cli_case("${_mode}" 1)
endforeach()

# Exercise decoder exceptions using an isolated private copy of the tools tree.
set(_decoder_tools "${_scratch}/decoder")
file(COPY "${_tools}/" DESTINATION "${_decoder_tools}")
set(_decoder_faults exception fractional-position nan-position)
foreach(_fault IN LISTS _decoder_faults)
  if(_fault STREQUAL "exception")
    set(_decoder_body "error('fixture decoder exception')")
  elseif(_fault STREQUAL "fractional-position")
    set(_decoder_body "return {},1.5,nil")
  else()
    set(_decoder_body "return {},0/0,nil")
  endif()
  file(WRITE "${_decoder_tools}/vendor/dkjson.lua"
    "return {null={}, decode=function() ${_decoder_body} end}\n")
  execute_process(
    COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua
    WORKING_DIRECTORY "${_decoder_tools}"
    INPUT_FILE "${_scratch}/valid.json"
    TIMEOUT 10
    RESULT_VARIABLE _decoder_status
    OUTPUT_VARIABLE _decoder_stdout
    ERROR_VARIABLE _decoder_stderr
    ENCODING UTF-8)
  require_exit("${_decoder_status}" "1" "decoder ${_fault} CLI; stderr: ${_decoder_stderr}")
  if(_decoder_stdout MATCHES "CTest inventory is valid:")
    message(FATAL_ERROR "decoder ${_fault} CLI emitted the success banner")
  endif()
endforeach()
message(STATUS "TOOLING_PASS group 8: CLI decoding")

function(usage_case expected)
  execute_process(
    COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua ${ARGN}
    WORKING_DIRECTORY "${_tools}"
    INPUT_FILE "${_scratch}/valid.json"
    TIMEOUT 10
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    ENCODING UTF-8)
  require_exit("${_status}" "${expected}" "CLI arguments: ${ARGN}; stderr: ${_stderr}")
endfunction()

usage_case(0 --help)
usage_case(2 --unknown)
usage_case(2 --profile)
usage_case(2 --profile unknown-profile)
usage_case(2 --profile windows-v1-full --profile windows-v1-full)

set(ENV{LUA_INIT} "error('LUA_INIT must be ignored')")
set(ENV{LUA_INIT_5_4} "error('LUA_INIT_5_4 must be ignored')")
set(ENV{LUA_INIT_5_5} "error('LUA_INIT_5_5 must be ignored')")
set(ENV{LUA_PATH} "missing/?.lua")
set(ENV{LUA_CPATH} "missing/?.so")
cli_case(valid 0)
unset(ENV{LUA_INIT})
unset(ENV{LUA_INIT_5_4})
unset(ENV{LUA_INIT_5_5})
unset(ENV{LUA_PATH})
unset(ENV{LUA_CPATH})

function(runtime_guard_case probe)
  execute_process(
    COMMAND "${LUA_EXECUTABLE}" -E -e "${probe}"
    WORKING_DIRECTORY "${_tools}"
    TIMEOUT 10
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    ENCODING UTF-8)
  require_exit("${_status}" "2" "runtime guard: ${probe}; stderr: ${_stderr}")
endfunction()

# These are explicit admission and I/O simulations, not an actual Lua 5.3 VM.
runtime_guard_case("_VERSION='Lua 5.3';arg={};dofile('verify_test_inventory.lua')")
runtime_guard_case("io.read=function()return nil,'fixture read error'end;arg={};dofile('verify_test_inventory.lua')")
runtime_guard_case("io.read=function()return '{'end;io.stderr={write=function()return nil,'fixture stderr error'end};arg={};dofile('verify_test_inventory.lua')")
runtime_guard_case("io.flush=function()return nil,'fixture flush error'end;arg={'--help'};dofile('verify_test_inventory.lua')")

set(_missing_tools "${_scratch}/missing-module")
file(MAKE_DIRECTORY "${_missing_tools}")
file(COPY "${_tools}/verify_test_inventory.lua" DESTINATION "${_missing_tools}")
execute_process(
  COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua
  WORKING_DIRECTORY "${_missing_tools}"
  INPUT_FILE "${_scratch}/valid.json"
  TIMEOUT 10
  RESULT_VARIABLE _missing_status
  OUTPUT_VARIABLE _missing_stdout
  ERROR_VARIABLE _missing_stderr
  ENCODING UTF-8)
require_exit("${_missing_status}" "2" "missing local modules; stderr: ${_missing_stderr}")
message(STATUS "TOOLING_PASS group 9: admission/local modules")

if(NOT RUN_LAUNCHER)
  message(STATUS "TOOLING_OK 9 groups; launcher stage not requested")
  return()
endif()

set(_launcher "${_repo}/cmake/VerifyTestInventory.cmake")
set(_fixture_generator_args -G Ninja)
if(DEFINED TOOLING_NINJA)
  cmake_path(IS_ABSOLUTE TOOLING_NINJA _ninja_absolute)
  if(NOT _ninja_absolute OR NOT EXISTS "${TOOLING_NINJA}" OR
     IS_DIRECTORY "${TOOLING_NINJA}" OR "${TOOLING_NINJA}" MATCHES "[;\r\n]")
    message(FATAL_ERROR "TOOLING_NINJA must be an absolute existing Ninja executable path without semicolons or newlines")
  endif()
  list(APPEND _fixture_generator_args "-DCMAKE_MAKE_PROGRAM=${TOOLING_NINJA}")
endif()

set(_full_ids 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15)
function(make_tree directory ids)
  file(MAKE_DIRECTORY "${directory}")
  file(WRITE "${directory}/CTestTestfile.cmake" "cmake_minimum_required(VERSION 3.28)\n")
  foreach(_id IN LISTS ids)
    set(_name "avb0795 V1 case ${_id}: fixture 中文 日本語 &")
    set(_labels avb0795-v1)
    if(_id MATCHES "^(13|14|15)$")
      string(APPEND _labels ";hardware")
    endif()
    file(APPEND "${directory}/CTestTestfile.cmake"
      "add_test([==[${_name}]==] [==[${CMAKE_COMMAND}]==] -E touch [==[${directory}/body-ran]==])\n"
      "set_tests_properties([==[${_name}]==] PROPERTIES LABELS [==[${_labels}]==])\n")
  endforeach()
endfunction()

make_tree("${_scratch}/full" "${_full_ids}")
make_tree("${_scratch}/policy" "01;02;03;04;05;06;07;08;09;10;11;12")
make_tree("${_scratch}/core" "01;02;03;04;05;07")
file(MAKE_DIRECTORY "${_scratch}/outside")

function(gate expected build launcher)
  execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIRECTORY=${build}" "-DLUA_EXECUTABLE=${LUA_EXECUTABLE}"
    "-DCTEST_EXECUTABLE=${_ctest}" ${ARGN} -P "${launcher}"
    WORKING_DIRECTORY "${_scratch}/outside"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err
    TIMEOUT 45 ENCODING UTF-8)
  set(_text "${_out}\n${_err}")
  if("${expected}" STREQUAL "pass")
    require_exit("${_result}" "0" "launcher: ${_text}")
    if(NOT _text MATCHES "CTest inventory is valid")
      message(FATAL_ERROR "Launcher omitted success")
    endif()
  else()
    if("${_result}" STREQUAL "0" OR _text MATCHES "CTest inventory is valid")
      message(FATAL_ERROR "Launcher falsely succeeded: ${_text}")
    endif()
  endif()
  set(_last_gate_result "${_result}" PARENT_SCOPE)
  set(_last_gate_text "${_text}" PARENT_SCOPE)
endfunction()

gate(pass ../full "${_launcher}")
gate(pass ../policy "${_launcher}" -DINVENTORY_PROFILE=windows-policy)
gate(pass ../core "${_launcher}" -DINVENTORY_PROFILE=portable-core)
gate(fail ../policy "${_launcher}")
gate(fail ../full "${_launcher}" -DINVENTORY_PROFILE=OFF)
gate(fail ../full "${_launcher}" -DINVENTORY_PROFILE=)
gate(fail ../absent "${_launcher}")
file(MAKE_DIRECTORY "${_scratch}/empty")
file(WRITE "${_scratch}/empty/CTestTestfile.cmake" "")
gate(fail ../empty "${_launcher}")
gate(fail ../full "${_launcher}" -DCONFIGURATION=)

file(MAKE_DIRECTORY "${_scratch}/stale-source")
file(WRITE "${_scratch}/stale-source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(InventoryFixture NONE)
option(FIXTURE_TESTS "Generate synthetic tests" ON)
if(FIXTURE_TESTS)
  enable_testing()
  foreach(id IN ITEMS 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15)
    set(name "avb0795 V1 case ${id}: stale fixture")
    add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}" -E false)
    set_tests_properties("${name}" PROPERTIES LABELS avb0795-v1)
    if(id MATCHES "^(13|14|15)$")
      set_property(TEST "${name}" APPEND PROPERTY LABELS hardware)
    endif()
  endforeach()
endif()
]=])
foreach(_enabled IN ITEMS ON OFF)
  execute_process(COMMAND "${CMAKE_COMMAND}" ${_fixture_generator_args}
    -S "${_scratch}/stale-source" -B "${_scratch}/stale-build"
    "-DFIXTURE_TESTS=${_enabled}" RESULT_VARIABLE _result
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 15 ENCODING UTF-8)
  require_exit("${_result}" "0" "synthetic configure: ${_err}")
  gate(pass ../stale-build "${_launcher}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" ${_fixture_generator_args}
  -S "${_scratch}/stale-source" -B "${_scratch}/fresh-off" -DFIXTURE_TESTS=OFF
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 15 ENCODING UTF-8)
require_exit("${_result}" "0" "fresh OFF configure: ${_err}")
gate(fail ../fresh-off "${_launcher}")
message(STATUS "PASS tooling group 10: prerequisites profiles and metadata limits")

file(MAKE_DIRECTORY "${_scratch}/upstream-failure")
file(WRITE "${_scratch}/upstream-failure/CTestTestfile.cmake"
  "execute_process(COMMAND [==[${_ctest}]==] --test-dir [==[${_scratch}/full]==] --show-only=json-v1 RESULT_VARIABLE inner TIMEOUT 5 ENCODING UTF-8)\n"
  "if(NOT \"\${inner}\" STREQUAL \"0\")\nmessage(FATAL_ERROR \"Inner fixture query failed\")\nendif()\n"
  "message(FATAL_ERROR \"Intentional upstream failure after valid JSON\")\n")
execute_process(
  COMMAND "${_ctest}" --test-dir "${_scratch}/upstream-failure" --show-only=json-v1
  COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua
  WORKING_DIRECTORY "${_tools}" RESULTS_VARIABLE _calibration
  OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10 ENCODING UTF-8)
list(LENGTH _calibration _count)
if(NOT _count EQUAL 2)
  message(FATAL_ERROR "Upstream fixture did not produce two child results")
endif()
list(GET _calibration 0 _producer)
list(GET _calibration 1 _consumer)
if("${_producer}" STREQUAL "0" OR NOT "${_consumer}" STREQUAL "0")
  message(FATAL_ERROR "Upstream fixture is not calibrated: ${_calibration}; ${_err}")
endif()
gate(fail ../upstream-failure "${_launcher}")
if(NOT _last_gate_text MATCHES "Lua=0")
  message(FATAL_ERROR "Did not exercise upstream-only failure")
endif()

make_tree("${_scratch}/bad-label" "${_full_ids}")
file(READ "${_scratch}/bad-label/CTestTestfile.cmake" _bad_label_metadata)
string(REPLACE
  "set_tests_properties([==[avb0795 V1 case 01: fixture 中文 日本語 &]==] PROPERTIES LABELS [==[avb0795-v1]==])"
  "set_tests_properties([==[avb0795 V1 case 01: fixture 中文 日本語 &]==] PROPERTIES LABELS [==[wrong]==])"
  _bad_label_metadata "${_bad_label_metadata}")
file(WRITE "${_scratch}/bad-label/CTestTestfile.cmake" "${_bad_label_metadata}")
gate(fail ../bad-label "${_launcher}")
if(NOT _last_gate_text MATCHES "CTest=0, Lua=1")
  message(FATAL_ERROR "Did not exercise downstream-only failure")
endif()

file(MAKE_DIRECTORY "${_scratch}/incomplete/cmake")
configure_file("${_launcher}" "${_scratch}/incomplete/cmake/VerifyTestInventory.cmake" COPYONLY)
gate(fail ../full "${_scratch}/incomplete/cmake/VerifyTestInventory.cmake")
if(NOT _last_gate_text MATCHES "Inventory pipeline failed")
  message(FATAL_ERROR "Expected a pipeline launch failure, not an unrelated prerequisite failure")
endif()

file(MAKE_DIRECTORY "${_scratch}/timeout")
file(WRITE "${_scratch}/timeout/CTestTestfile.cmake" [=[
unset(ENV{SOURCE_DATE_EPOCH})
string(TIMESTAMP now "%s" UTC)
math(EXPR until "${now} + 40")
while(now LESS until)
  string(TIMESTAMP now "%s" UTC)
endwhile()
message(FATAL_ERROR "Fixture reached its own deadline")
]=])
unset(ENV{SOURCE_DATE_EPOCH})
string(TIMESTAMP _started "%s" UTC)
gate(fail ../timeout "${_launcher}")
string(TIMESTAMP _finished "%s" UTC)
math(EXPR _elapsed "${_finished} - ${_started}")
if(_elapsed LESS 25 OR _elapsed GREATER 38 OR
   NOT _last_gate_text MATCHES "Inventory pipeline failed" OR
   NOT _last_gate_text MATCHES "CTest=[^,]*[Tt]imeout" OR
   NOT "${_last_gate_result}" MATCHES "^[0-9]+$")
  message(FATAL_ERROR "Launcher timeout was not exercised: ${_elapsed}s, ${_last_gate_result}")
endif()
message(STATUS "PASS tooling group 11: actual pipeline failure modes")

set(_copy "${_scratch}/source 中文 日本語 &")
file(MAKE_DIRECTORY "${_copy}/cmake")
configure_file("${_launcher}" "${_copy}/cmake/VerifyTestInventory.cmake" COPYONLY)
file(COPY "${_tools}/" DESTINATION "${_copy}/tools")
set(_unicode_build "${_scratch}/build 中文 日本語 &")
make_tree("${_unicode_build}" "${_full_ids}")
gate(pass "../build 中文 日本語 &" "${_copy}/cmake/VerifyTestInventory.cmake")
foreach(_configuration IN ITEMS OFF "Release 中文 日本語 &")
  make_tree("${_unicode_build}" "${_full_ids}")
  file(APPEND "${_unicode_build}/CTestTestfile.cmake"
    "if(NOT \"\${CTEST_CONFIGURATION_TYPE}\" STREQUAL [==[${_configuration}]==])\n"
    "message(FATAL_ERROR \"Configuration forwarding mismatch\")\nendif()\n")
  gate(pass "../build 中文 日本語 &" "${_copy}/cmake/VerifyTestInventory.cmake"
    "-DCONFIGURATION=${_configuration}")
endforeach()
make_tree("${_unicode_build}" "${_full_ids}")
file(APPEND "${_unicode_build}/CTestTestfile.cmake"
  "if(NOT \"\${CTEST_CONFIGURATION_TYPE}\" STREQUAL \"\")\n"
  "message(FATAL_ERROR \"Omitted configuration was not preserved\")\nendif()\n")
gate(pass "../build 中文 日本語 &" "${_copy}/cmake/VerifyTestInventory.cmake")

function(rejected_path value)
  gate(fail "${value}" "${_launcher}")
  if(NOT _last_gate_text MATCHES "Unsupported semicolon or newline")
    message(FATAL_ERROR "Input was split or failed for the wrong reason")
  endif()
endfunction()
rejected_path("bad;path")
rejected_path("bad\npath")
rejected_path("bad\rpath")

set(ENV{LUA_INIT} "error('launcher must suppress Lua initialization')")
set(ENV{LUA_INIT_5_4} "error('launcher must suppress versioned initialization')")
set(ENV{LUA_INIT_5_5} "error('launcher must suppress versioned initialization')")
gate(pass ../full "${_launcher}")
unset(ENV{LUA_INIT})
unset(ENV{LUA_INIT_5_4})
unset(ENV{LUA_INIT_5_5})

if(CMAKE_HOST_WIN32)
  get_filename_component(_lua_name "${LUA_EXECUTABLE}" NAME)
  if(_lua_name STREQUAL "lua.exe")
    get_filename_component(_lua_directory "${LUA_EXECUTABLE}" DIRECTORY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
      "PATH=${_lua_directory};$ENV{PATH}" "${CMAKE_COMMAND}"
      "-DBUILD_DIRECTORY=${_scratch}/full" -P "${_launcher}"
      RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 40 ENCODING UTF-8)
    require_exit("${_result}" "0" "host PATH Lua and sibling CTest: ${_err}")
  endif()
endif()

file(GLOB_RECURSE _markers LIST_DIRECTORIES false "${_scratch}/*body-ran")
if(_markers)
  message(FATAL_ERROR "Inventory query executed a test body")
endif()
message(STATUS "PASS tooling group 12: UTF-8 paths configuration and no test execution")
message(STATUS "TOOLING_OK 12 groups")
