# Portable Lua Tooling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Replace the PowerShell inventory gate with a portable CMake launcher and isolated Lua validation without changing application runtime dependencies or the strict full Windows inventory contract.

**Architecture:** CMake owns host tools, paths, native pipelines, and aggregate child status. A Lua CLI decodes stdin through pinned dkjson and invokes one pure validator with an explicit built-in profile. Tooling tests remain separate from the 15 Catch2 cases they inspect.

**Tech Stack:** CMake/CTest 3.28+, stock PUC Lua 5.4.x/5.5.x, unmodified dkjson 2.11, a Lua/CMake tooling harness.

---

## Approved input and execution rules

- [Approved design](../specs/2026-09-08-lua-tooling-design.md): user approved the reviewed written spec on 2026-09-08. This plan does not authorize implementation until its execution handoff is accepted.
- Do not stage, commit, push, package, publish, or run capture. User permission overrides automatic commit steps in workflow skills. Tests, specs, plans, other-issue.md, and work-record Markdown remain excluded from any later authorized commit.
- Preserve the dirty worktree. Before implementation, inspect every target for overlapping changes; do not reset, switch away from, or create a clean checkout that silently loses the local untracked test/spec contracts.
- Use apply_patch for first-party file edits. Immutable dependency download and generated fixture output are mechanical acquisition/test operations, not a way to rewrite project source with shell commands.
- Commands below run from the repository root unless a CMake command explicitly changes the working directory. The Windows examples use the verified local C:/lua/lua.exe; use the discovered absolute Lua executable on another host, not that Windows path.
- This plan contains proposed code and expected outcomes, not evidence that the implementation/tests have run.

Execution environment note: the local PATH CMake 4.0.0-rc2 crashed in a minimal
Unicode cmake_path probe. Full Windows execution uses the already-installed
Visual Studio CMake/CTest 4.3.1-msvc1 via the explicit executable
C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/CMake/bin/cmake.exe.
Use that executable in place of bare cmake for the local full-harness commands
below. No PATH change, installation, production version workaround, or Unicode
test relaxation is required. Linux actually runs CMake/CTest 3.28.3.

## File map and task order

| Task | Files | Responsibility |
| --- | --- | --- |
| 1 | .gitattributes; tools/vendor/dkjson.lua; tools/vendor/README.md | Exact dependency bytes, license, and checkout-stable pin |
| 2 | tools/lib/inventory_profiles.lua; tools/lib/test_inventory.lua; tests/tooling/inventory_fixtures.lua; tests/tooling/policy_test.lua | Profile data, pure policy, seven focused policy scenario groups |
| 3 | tools/verify_test_inventory.lua; tests/tooling/emit_inventory.lua; tests/tooling/run_tooling_tests.cmake | CLI, real-process JSON/usage checks, common harness setup |
| 4 | cmake/VerifyTestInventory.cmake; extend tests/tooling/run_tooling_tests.cmake | Native launcher and three integrated scenario groups |
| 5 | docs/development/lua-tooling.md; docs/architecture/build-and-validation.md; THIRD_PARTY_NOTICES.md; remove cmake/VerifyTestInventory.ps1 after acceptance | Active documentation and verified retirement |

No top-level CMakeLists, presets, C++ sources, application i18n, source-boundary allowlists, or tests/CMakeLists changes are needed. The additional .gitattributes rule protects only the unmodified vendor file from checkout newline conversion; it does not reformat other files.

## Task 1: Pin the development-only JSON dependency

**Files:** Create .gitattributes if still absent, otherwise append only the scoped rule; create tools/vendor/README.md and acquire tools/vendor/dkjson.lua.

- [x] **Step 1: Refresh the source/tool baseline and check destinations.**

~~~text
git status --short
git diff --cached --name-only
cmake --version
ctest --version
C:/lua/lua.exe -v
~~~

Expected local versions observed during planning: CMake/CTest 4.0.0-rc2 and Lua 5.5.1. These are not CMake 3.28 or Unix validation. Check whether the three target files now exist before writing/downloading; inspect and preserve unexpected existing content. Do not read personal imgui.ini or airelinna.json.

- [x] **Step 2: Add the narrow attributes rule and dependency note.**

Content to add to .gitattributes:

~~~gitattributes
tools/vendor/dkjson.lua -text
~~~

Create tools/vendor/README.md:

~~~markdown
# Development-only Lua dependency

dkjson 2.11, Copyright (C) 2010-2026 David Heiko Kolf.

- Source: https://dkolf.de/dkjson-lua/dkjson-2.11.lua
- SHA-256: 197cb50834c642f84b4cf99fe724932c50e6d9c92faec7ad89aa25e91df4d481
- License: MIT; the complete upstream notice is retained in dkjson.lua.

Keep the file byte-for-byte unchanged, including line endings. Use the
pure-Lua decoder, without LPeg. The inventory utility does not download
dependencies when it runs, and Airelinna does not load this module.
~~~

- [x] **Step 3: Acquire upstream bytes only after confirming the destination is absent.**

Use the available download tool, or this native curl invocation (CMake avoids PowerShell's curl alias). This is a one-time maintainer acquisition, not a gate prerequisite or runtime downloader.

~~~text
cmake -E env -- curl --fail --location --output tools/vendor/dkjson.lua https://dkolf.de/dkjson-lua/dkjson-2.11.lua
cmake -E sha256sum tools/vendor/dkjson.lua
git check-attr text -- tools/vendor/dkjson.lua
~~~

Require the exact SHA-256 above and attribute result "text: unset". On download/hash failure, stop; do not load the result or overwrite another file. If curl is unavailable, use a download-capable tool to obtain those same bytes and perform the same hash check; do not add curl to production requirements.

- [x] **Step 4: Verify the parser from the intended child working directory.**

~~~text
cmake -E chdir tools C:/lua/lua.exe -E -e "package.path='./vendor/?.lua'; package.cpath=''; local j=require('dkjson'); local v,p,e=j.decode('[]',1,j.null); assert(not e and p==3 and getmetatable(v).__jsontype=='array'); print(j.version)"
~~~

Expected: dkjson 2.11, exit 0. Retain the embedded upstream copyright/license. Checkpoint only; no commit.

## Task 2: Add explicit profiles and the pure validation policy

**Files:** Create tools/lib/inventory_profiles.lua, tools/lib/test_inventory.lua, tests/tooling/inventory_fixtures.lua, tests/tooling/policy_test.lua.

- [x] **Step 1: Write independent fixtures and the failing policy harness first.**

Create tests/tooling/inventory_fixtures.lua. Its expected lists intentionally do not import production profiles, so a wrong production profile cannot redefine the test oracle.

~~~lua
local M = {}
local object_meta, array_meta = { __jsontype = "object" }, { __jsontype = "array" }
function M.object(v) return setmetatable(v or {}, object_meta) end
function M.array(v) return setmetatable(v or {}, array_meta) end
M.ids = {
  ["windows-v1-full"] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
  ["windows-policy"] = {1,2,3,4,5,6,7,8,9,10,11,12},
  ["portable-core"] = {1,2,3,4,5,7},
}
function M.inventory(name)
  local tests = M.array()
  for _, id in ipairs(assert(M.ids[name])) do
    local labels = M.array({"airelinna-v1"})
    if id >= 13 then labels[#labels + 1] = "hardware" end
    tests[#tests + 1] = M.object({
      name = string.format("Airelinna V1 case %02d: fixture", id),
      command = M.array({"opaque-executable", "argument"}),
      properties = M.array({M.object({name = "LABELS", value = labels})}),
    })
  end
  return M.object({
    kind = "ctestInfo", version = M.object({major = 1, minor = 0}), tests = tests,
  })
end
return M
~~~

Create tests/tooling/policy_test.lua:

~~~lua
package.path = "./lib/?.lua;./vendor/?.lua;../tests/tooling/?.lua"
package.cpath = ""
local fixtures = require("inventory_fixtures")
local policy = require("test_inventory")
local profiles = require("inventory_profiles")
local json = require("dkjson")
local groups = 0
local function group(name, fn)
  fn()
  groups = groups + 1
  print("PASS policy: " .. name)
end
local function check(name, mutate, expected, needle)
  local doc = fixtures.inventory(name)
  if mutate then mutate(doc) end
  local before = json.encode(doc)
  local profile_before = json.encode(profiles[name])
  local errors = policy.validate_inventory(doc, profiles[name])
  assert((#errors == 0) == expected, table.concat(errors, "\n"))
  assert(json.encode(doc) == before, "validator mutated input")
  assert(json.encode(profiles[name]) == profile_before, "validator mutated profile")
  if needle then assert(table.concat(errors, "\n"):find(needle, 1, true)) end
end
group("full shuffled inventory and supplemental data", function()
  check("windows-v1-full", function(d)
    d.tests[1], d.tests[15] = d.tests[15], d.tests[1]
    d.extra = true
    d.tests[1].properties[1].value[3] = "additional"
  end, true)
end)
group("explicit profile sets", function()
  for _, name in ipairs({"windows-v1-full", "windows-policy", "portable-core"}) do
    check(name, nil, true)
  end
  assert(#policy.validate_inventory(fixtures.inventory("windows-policy"),
    profiles["windows-v1-full"]) > 0)
  assert(#policy.validate_inventory(fixtures.inventory("portable-core"),
    profiles["windows-policy"]) > 0)
end)
group("missing duplicate and unexpected IDs", function()
  check("windows-v1-full", function(d) table.remove(d.tests, 1) end, false, "Missing case ID 01")
  check("windows-v1-full", function(d) d.tests[2].name = d.tests[1].name end, false, "Duplicate")
  check("portable-core", function(d) d.tests[6].name = "Airelinna V1 case 06: unexpected" end, false)
  check("windows-v1-full", function(d) d.tests[1].name = "Airelinna V1 case 99: unexpected" end, false)
end)
group("name and record shapes", function()
  check("windows-v1-full", function(d) d.tests[1].name = "airelinna v1 CASE 01: fixture" end, true)
  check("windows-v1-full", function(d) d.tests[1].name = "Airelinna V1 case 1: fixture" end, false)
  check("windows-v1-full", function(d) d.tests[1] = json.null end, false)
end)
group("JSON shape preservation and envelope", function()
  for _, bad in ipairs({json.null, fixtures.object(), 3}) do
    check("windows-v1-full", function(d) d.tests = bad end, false)
  end
  check("windows-v1-full", function(d) d.version.major = 2 end, false)
  check("windows-v1-full", function(d) d.version.minor = -1 end, false)
  check("windows-v1-full", function(d) d.kind = "wrong" end, false)
  check("windows-v1-full", function(d) d.tests[1].properties = fixtures.object() end, false)
end)
group("opaque commands and intentional Unicode whitespace", function()
  for _, bad in ipairs({fixtures.array(), fixtures.object(), "command", json.null}) do
    check("windows-v1-full", function(d) d.tests[1].command = bad end, false)
  end
  check("windows-v1-full", function(d) d.tests[1].command[1] = " \t" end, false)
  check("windows-v1-full", function(d) d.tests[1].command[2] = "\194\160" end, true)
  check("windows-v1-full", function(d)
    d.tests[1].command[1] = "C:\\\230\184\172\232\169\166\\\230\151\165\230\156\172\232\170\158 &\\test.exe"
  end, true)
end)
group("LABELS shapes spelling and exact hardware placement", function()
  check("windows-v1-full", function(d) d.tests[1].properties = fixtures.array() end, false)
  check("windows-v1-full", function(d) d.tests[1].properties[2] = d.tests[1].properties[1] end, false)
  check("windows-v1-full", function(d) d.tests[1].properties[1].value = "airelinna-v1" end, false)
  check("windows-v1-full", function(d) d.tests[1].properties[1].value[1] = "AIRELINNA-V1" end, false)
  check("windows-v1-full", function(d) d.tests[1].properties[1].value[2] = "hardware" end, false)
  check("windows-v1-full", function(d) d.tests[13].properties[1].value[2] = nil end, false)
  check("windows-v1-full", function(d) d.tests[1].properties[1].value[2] = "\227\128\128" end, true)
end)
assert(groups == 7)
print("POLICY_OK 7 groups")
~~~

- [x] **Step 2: Run the harness and confirm RED is a missing policy module.**

~~~text
cmake -E chdir tools C:/lua/lua.exe -E ../tests/tooling/policy_test.lua
~~~

Expected before implementation: nonzero, module test_inventory not found. A missing dkjson or missing interpreter is setup failure, not the intended red result.

- [x] **Step 3: Add the profile module.**

Create tools/lib/inventory_profiles.lua:

~~~lua
local function profile(name, ids, hardware)
  return {name = name, ids = ids, hardware = hardware, required_label = "airelinna-v1"}
end
return {
  ["windows-v1-full"] = profile("windows-v1-full",
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, {13,14,15}),
  ["windows-policy"] = profile("windows-policy", {1,2,3,4,5,6,7,8,9,10,11,12}, {}),
  ["portable-core"] = profile("portable-core", {1,2,3,4,5,7}, {}),
}
~~~

- [x] **Step 4: Add the pure validator without I/O or native dependencies.**

Create tools/lib/test_inventory.lua:

~~~lua
local M = {}
local function kind(v, expected)
  local meta = type(v) == "table" and getmetatable(v)
  return type(meta) == "table" and meta.__jsontype == expected
end
local function nonblank(v)
  return type(v) == "string" and v:find("[^ \t\r\n\f\v]") ~= nil
end
local function set(values)
  local result = {}
  for _, v in ipairs(values) do result[v] = true end
  return result
end
local function ascii_lower(text)
  return (text:gsub("[A-Z]", function(c) return string.char(c:byte() + 32) end))
end
function M.validate_inventory(document, profile)
  local errors, seen = {}, {}
  local expected, hardware = set(profile.ids), set(profile.hardware)
  local function issue(text) errors[#errors + 1] = text end
  if not kind(document, "object") then
    issue("Inventory root must be an object")
    return errors
  end
  local version = document.version
  if document.kind ~= "ctestInfo" or not kind(version, "object")
      or version.major ~= 1 or type(version.minor) ~= "number"
      or version.minor < 0 or version.minor % 1 ~= 0 then
    issue("Unsupported CTest JSON envelope")
  end
  if not kind(document.tests, "array") then
    issue("tests must be a JSON array")
    return errors
  end
  if #document.tests ~= #profile.ids then
    issue(string.format("Expected exactly %d discovered tests; found %d",
      #profile.ids, #document.tests))
  end
  local function validate_test(test, index)
    local prefix = string.format("Test %d: ", index)
    if not kind(test, "object") then issue(prefix .. "must be an object"); return end
    local id
    if nonblank(test.name) then
      id = tonumber(ascii_lower(test.name):match("^airelinna v1 case ([0-9][0-9]): .+$"))
    end
    if not id then
      issue(prefix .. "name is not canonical")
    elseif not expected[id] then
      issue(prefix .. "unexpected case ID " .. id)
    elseif seen[id] then
      issue(prefix .. "Duplicate case ID " .. id)
    else
      seen[id] = true
    end
    if not kind(test.command, "array") or #test.command == 0 then
      issue(prefix .. "command must be a nonempty JSON array")
    else
      for _, argument in ipairs(test.command) do
        if not nonblank(argument) then
          issue(prefix .. "command contains a non-string or blank element")
          break
        end
      end
    end
    if not kind(test.properties, "array") then
      issue(prefix .. "properties must be a JSON array")
      return
    end
    local label_count, label_value = 0, nil
    for _, property in ipairs(test.properties) do
      if not kind(property, "object") then
        issue(prefix .. "property must be an object")
      elseif property.name == "LABELS" then
        label_count = label_count + 1
        label_value = property.value
      end
    end
    if label_count ~= 1 then issue(prefix .. "requires exactly one LABELS property"); return end
    if not kind(label_value, "array") then issue(prefix .. "LABELS value must be an array"); return end
    local labels = {}
    for _, label in ipairs(label_value) do
      if not nonblank(label) then issue(prefix .. "label must be a nonblank string")
      else labels[label] = true end
    end
    if not labels[profile.required_label] then issue(prefix .. "missing airelinna-v1 label") end
    if id and expected[id] and ((labels.hardware == true) ~= (hardware[id] == true)) then
      issue(prefix .. "incorrect hardware label placement")
    end
  end
  for index, test in ipairs(document.tests) do validate_test(test, index) end
  -- Profiles are trusted sorted ID arrays; do not mutate the input while reporting.
  for _, id in ipairs(profile.ids) do
    if not seen[id] then issue(string.format("Missing case ID %02d", id)) end
  end
  return errors
end
return M
~~~

- [x] **Step 5: Run the same harness GREEN and inspect dependencies.**

Require POLICY_OK 7 groups, exit 0, no input mutation assertion. Inspect the two production modules for I/O, environment, and process calls: neither may contain those operations. No commit; keep tests local.

## Task 3: Add the stdin-only Lua CLI and real-process checks

**Files:** Create tools/verify_test_inventory.lua, tests/tooling/emit_inventory.lua, and tests/tooling/run_tooling_tests.cmake.

- [x] **Step 1: Create the fixture emitter and the first part of the CMake harness.**

Create tests/tooling/emit_inventory.lua:

~~~lua
package.path = "./vendor/?.lua;../tests/tooling/?.lua"
package.cpath = ""
local json = require("dkjson")
local fixtures = require("inventory_fixtures")
local mode = arg[1] or "valid"
if mode == "empty" then return end
if mode == "malformed" then io.write("{"); return end
if mode == "oversized" then io.write(string.rep(" ", 4 * 1024 * 1024 + 1)); return end
local document = fixtures.inventory(arg[2] or "windows-v1-full")
if mode == "wrong-envelope" then document.version.major = 2 end
if mode == "null-entry" then document.tests[1] = json.null end
io.write(json.encode(document))
if mode == "trailing" then io.write(" false") end
~~~

Create tests/tooling/run_tooling_tests.cmake with the following content. Task 4 appends to this same file; RUN_LAUNCHER keeps Task 3 runnable before the launcher exists. It is a test-harness stage selector, not a production hook.

~~~cmake
cmake_minimum_required(VERSION 3.28)
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_tools "${_repo}/tools")
if(NOT DEFINED LUA_EXECUTABLE)
  message(FATAL_ERROR "Supply absolute -DLUA_EXECUTABLE for this test harness")
endif()
get_filename_component(_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
set(_ctest "${_cmake_bin}/ctest")
if(CMAKE_HOST_WIN32)
  string(APPEND _ctest ".exe")
endif()
if(NOT EXISTS "${_ctest}")
  message(FATAL_ERROR "Harness requires CTest beside the running CMake")
endif()
file(SHA256 "${_tools}/vendor/dkjson.lua" _hash)
if(NOT _hash STREQUAL "197cb50834c642f84b4cf99fe724932c50e6d9c92faec7ad89aa25e91df4d481")
  message(FATAL_ERROR "Vendored dkjson hash mismatch")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_scratch "${_repo}/build/lua-tooling-${_suffix}")
if(EXISTS "${_scratch}")
  message(FATAL_ERROR "Scratch collision; refusing to overwrite")
endif()
file(MAKE_DIRECTORY "${_scratch}")
message(STATUS "Tooling fixtures retained at: ${_scratch}")
function(require_exit actual expected context)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "${context}: expected ${expected}, got ${actual}")
  endif()
endfunction()
execute_process(COMMAND "${LUA_EXECUTABLE}" -E ../tests/tooling/policy_test.lua
  WORKING_DIRECTORY "${_tools}" TIMEOUT 10
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err ENCODING UTF-8)
require_exit("${_result}" "0" "policy harness: ${_err}")
if(NOT _out MATCHES "POLICY_OK 7 groups")
  message(FATAL_ERROR "Incomplete policy harness")
endif()
function(cli_case mode expected)
  set(_input "${_scratch}/${mode}.json")
  execute_process(COMMAND "${LUA_EXECUTABLE}" -E ../tests/tooling/emit_inventory.lua "${mode}"
    WORKING_DIRECTORY "${_tools}" OUTPUT_FILE "${_input}"
    ERROR_VARIABLE _error RESULT_VARIABLE _status TIMEOUT 10 ENCODING UTF-8)
  require_exit("${_status}" "0" "fixture emitter: ${_error}")
  execute_process(COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua
    WORKING_DIRECTORY "${_tools}" INPUT_FILE "${_input}"
    OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr RESULT_VARIABLE _status
    TIMEOUT 10 ENCODING UTF-8)
  require_exit("${_status}" "${expected}" "CLI ${mode}: ${_stderr}")
  if("${expected}" STREQUAL "0")
    string(STRIP "${_stdout}" _stdout)
    if(NOT _stdout STREQUAL
        "CTest inventory is valid: 15 Airelinna V1 cases; hardware cases 13-15.")
      message(FATAL_ERROR "Full-profile success text changed")
    endif()
  elseif(_stdout MATCHES "CTest inventory is valid")
    message(FATAL_ERROR "False success on CLI failure")
  endif()
endfunction()
cli_case(valid 0)
foreach(_mode IN ITEMS empty malformed oversized trailing wrong-envelope null-entry)
  cli_case("${_mode}" 1)
endforeach()
# Fault containment: only the private copied module is altered.
file(MAKE_DIRECTORY "${_scratch}/decoder")
file(COPY "${_tools}/" DESTINATION "${_scratch}/decoder")
file(WRITE "${_scratch}/decoder/vendor/dkjson.lua"
  "return {null={}, decode=function() error('fixture decoder exception') end}\n")
execute_process(COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua
  WORKING_DIRECTORY "${_scratch}/decoder" INPUT_FILE "${_scratch}/valid.json"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10 ENCODING UTF-8)
require_exit("${_result}" "1" "decoder exception containment")
if(_out MATCHES "CTest inventory is valid")
  message(FATAL_ERROR "Decoder exception reported success")
endif()
message(STATUS "PASS tooling group 8: CLI decoding")

function(usage_case expected)
  execute_process(COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua ${ARGN}
    WORKING_DIRECTORY "${_tools}" INPUT_FILE "${_scratch}/valid.json"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10 ENCODING UTF-8)
  require_exit("${_result}" "${expected}" "CLI usage: ${_err}")
endfunction()
usage_case(0 --help)
usage_case(2 --unknown)
usage_case(2 --profile)
usage_case(2 --profile unknown)
usage_case(2 --profile windows-v1-full --profile windows-v1-full)
set(ENV{LUA_INIT} "error('environment initializer must not run')")
set(ENV{LUA_INIT_5_4} "error('versioned initializer must not run')")
set(ENV{LUA_INIT_5_5} "error('versioned initializer must not run')")
set(ENV{LUA_PATH} "missing/?.lua")
set(ENV{LUA_CPATH} "missing/?.so")
cli_case(valid 0)
unset(ENV{LUA_INIT})
unset(ENV{LUA_INIT_5_4})
unset(ENV{LUA_INIT_5_5})
unset(ENV{LUA_PATH})
unset(ENV{LUA_CPATH})
# Constant test code only: this checks admission/containment, not an actual older VM.
function(runtime_guard_case _probe)
  execute_process(COMMAND "${LUA_EXECUTABLE}" -E -e "${_probe}"
    WORKING_DIRECTORY "${_tools}" RESULT_VARIABLE _result
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10 ENCODING UTF-8)
  require_exit("${_result}" "2" "CLI runtime/I/O guard")
endfunction()
runtime_guard_case("_VERSION='Lua 5.3';arg={};dofile('verify_test_inventory.lua')")
runtime_guard_case("io.read=function()return nil,'fixture read error'end;arg={};dofile('verify_test_inventory.lua')")
file(MAKE_DIRECTORY "${_scratch}/missing-module")
file(COPY "${_tools}/verify_test_inventory.lua" DESTINATION "${_scratch}/missing-module")
execute_process(COMMAND "${LUA_EXECUTABLE}" -E verify_test_inventory.lua
  WORKING_DIRECTORY "${_scratch}/missing-module" INPUT_FILE "${_scratch}/valid.json"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10 ENCODING UTF-8)
require_exit("${_result}" "2" "missing local module")
message(STATUS "PASS tooling group 9: CLI admission and local modules")
if(NOT RUN_LAUNCHER)
  message(STATUS "TOOLING_OK 9 groups; launcher stage not requested")
  return()
endif()
~~~

The harness writes only new, uniquely named fixture files under build/. It never recursively removes anything or overwrites the real tools. Retain fixtures for diagnostics; any later cleanup must resolve and check that exact scratch directory first.

- [x] **Step 2: Run the CLI stage RED.**

~~~text
cmake -DLUA_EXECUTABLE=C:/lua/lua.exe -P tests/tooling/run_tooling_tests.cmake
~~~

Expected: policy stage passes; the first valid CLI case fails because verify_test_inventory.lua does not exist. Do not misclassify failed policy/dependency setup as the CLI red result.

- [x] **Step 3: Create the bounded stdin-only CLI.**

Create tools/verify_test_inventory.lua:

~~~lua
local function run()
  if _VERSION ~= "Lua 5.4" and _VERSION ~= "Lua 5.5" then
    io.stderr:write("Use stock PUC Lua 5.4.x or 5.5.x\n")
    return 2
  end
  package.path = "./lib/?.lua;./vendor/?.lua"
  package.cpath = ""
  local profiles = require("inventory_profiles")
  local policy = require("test_inventory")
  local json = require("dkjson")
  if #arg == 1 and arg[1] == "--help" then
    assert(io.stdout:write("Usage: verify_test_inventory.lua [--profile NAME] < inventory.json\n"))
    return 0
  end
  local name = "windows-v1-full"
  if #arg ~= 0 then
    if #arg ~= 2 or arg[1] ~= "--profile" or arg[2] == "" then
      io.stderr:write("Expected only --profile NAME, or standalone --help\n")
      return 2
    end
    name = arg[2]
  end
  local profile = profiles[name]
  if not profile then io.stderr:write("Unknown inventory profile\n"); return 2 end
  -- Bound the text delivered by Lua stdio, not the raw OS-pipe byte count.
  local limit = 4 * 1024 * 1024
  local text, read_error = io.read(limit + 1)
  if not text and read_error then io.stderr:write("Inventory input read failed\n"); return 2 end
  text = text or ""
  if #text > limit then io.stderr:write("Inventory exceeds 4 MiB\n"); return 1 end
  if not text:find("[^ \t\r\n\f\v]") then io.stderr:write("Inventory is empty\n"); return 1 end
  local decoded, document, position, decode_error = pcall(json.decode, text, 1, json.null)
  if not decoded or decode_error or type(position) ~= "number"
      or position % 1 ~= 0 or position < 1 or position > #text + 1 then
    io.stderr:write("Inventory JSON decoding failed\n")
    return 1
  end
  if text:sub(position):find("[^ \t\r\n]") then
    io.stderr:write("Inventory has trailing non-whitespace data\n")
    return 1
  end
  local errors = policy.validate_inventory(document, profile)
  if #errors ~= 0 then
    io.stderr:write("CTest inventory validation failed:\n")
    for _, error_text in ipairs(errors) do io.stderr:write("  - ", error_text, "\n") end
    return 1
  end
  if name == "windows-v1-full" then
    assert(io.stdout:write(
      "CTest inventory is valid: 15 Airelinna V1 cases; hardware cases 13-15.\n"))
  else
    assert(io.stdout:write(string.format(
      "CTest inventory is valid: profile %s; %d Airelinna V1 cases; no hardware cases.\n",
      name, #profile.ids)))
  end
  return 0
end
local ok, code = pcall(run)
if not ok then
  io.stderr:write("Inventory tool setup/internal failure: ", tostring(code):sub(1, 300), "\n")
  code = 2
end
os.exit(code, true)
~~~

- [x] **Step 4: Run the same CLI stage GREEN.**

Require TOOLING_OK 9 groups and exit 0. Decoder exceptions use one private dependency-fault fixture; implementation review additionally requires fractional and NaN decoder-position variants in that same group, rejected with exit 1. Unsupported-runtime admission is simulated explicitly, not claimed as an actual Lua 5.3 run. The real CLI performs the work in all cases.

The size and trailing-data checks apply to the string returned by Lua stdio. Stock Windows Lua uses CRT text-mode stdin: CRLF may become LF, and a raw Ctrl+Z may terminate the visible stream. Do not call this a raw-pipe byte limiter or a validator of arbitrary binary producers. Keep the approved trusted-CTest input boundary; no native binary-mode module, platform adapter, or transport rewrite is required.

- [x] **Step 5: Inspect isolation and checkpoint without committing.**

Confirm the CLI has no process launch, OS detection, native call, application i18n dependency, or input-as-Lua evaluation. Only the test harness may create JSON fixtures or inject a dependency fault into its private copy.

Implementation review corrections: ordinary diagnostic writes must check their I/O result so the outer guard returns exit 2 on stderr failure. The implementation uses one checked write helper; keep its regression within the existing group 9 I/O probes. Both successful stdout paths must explicitly check flush before returning 0; implicit exit-time flushing cannot revise the chosen exit code. Keep that regression in group 9 as well. The final emergency diagnostic remains best effort.

## Task 4: Implement native launching and the remaining integration groups

**Files:** Create cmake/VerifyTestInventory.cmake; append the integration code below to tests/tooling/run_tooling_tests.cmake.

- [x] **Step 1: Add the real-launcher integration fixtures before the launcher.**

Append this block to the existing harness, after its RUN_LAUNCHER early return:

~~~cmake
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
    set(_name "Airelinna V1 case ${_id}: fixture 中文 日本語 &")
    set(_labels airelinna-v1)
    if(_id MATCHES "^(13|14|15)$")
      string(APPEND _labels ";hardware")
    endif()
    # Bracket arguments preserve arbitrary fixture paths without Lua/shell escaping.
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

# Real ON -> OFF reconfiguration of a language-free synthetic project.
file(MAKE_DIRECTORY "${_scratch}/stale-source")
file(WRITE "${_scratch}/stale-source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(InventoryFixture NONE)
option(FIXTURE_TESTS "Generate synthetic tests" ON)
if(FIXTURE_TESTS)
  enable_testing()
  foreach(id IN ITEMS 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15)
    set(name "Airelinna V1 case ${id}: stale fixture")
    add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}" -E false)
    set_tests_properties("${name}" PROPERTIES LABELS airelinna-v1)
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
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 15)
require_exit("${_result}" "0" "fresh OFF configure")
gate(fail ../fresh-off "${_launcher}")
message(STATUS "PASS tooling group 10: prerequisites profiles and metadata limits")

# Upstream emits a real valid JSON inventory, then fails before its own JSON output.
file(MAKE_DIRECTORY "${_scratch}/upstream-failure")
file(WRITE "${_scratch}/upstream-failure/CTestTestfile.cmake"
  "execute_process(COMMAND [==[${_ctest}]==] --test-dir [==[${_scratch}/full]==] --show-only=json-v1 RESULT_VARIABLE inner TIMEOUT 5 ENCODING UTF-8)\n"
  "if(NOT \"\${inner}\" STREQUAL \"0\")\nmessage(FATAL_ERROR \"Inner fixture query failed\")\nendif()\n"
  "message(FATAL_ERROR \"Intentional upstream failure after valid JSON\")\n")
# Calibrate with BOTH real children; do not assume SEND_ERROR alone emits JSON.
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
# Valid producer + invalid inventory exercises the other side of the real pipeline.
make_tree("${_scratch}/bad-label" "${_full_ids}")
# Repeated LABELS assignments can accumulate in CTest; replace the original.
file(READ "${_scratch}/bad-label/CTestTestfile.cmake" _bad_label_metadata)
string(REPLACE
  "set_tests_properties([==[Airelinna V1 case 01: fixture 中文 日本語 &]==] PROPERTIES LABELS [==[airelinna-v1]==])"
  "set_tests_properties([==[Airelinna V1 case 01: fixture 中文 日本語 &]==] PROPERTIES LABELS [==[wrong]==])"
  _bad_label_metadata "${_bad_label_metadata}")
file(WRITE "${_scratch}/bad-label/CTestTestfile.cmake" "${_bad_label_metadata}")
gate(fail ../bad-label "${_launcher}")
if(NOT _last_gate_text MATCHES "CTest=0, Lua=1")
  message(FATAL_ERROR "Did not exercise downstream-only failure")
endif()
# Exact launcher, deliberately incomplete private checkout: native cwd launch error.
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

# Copy only scoped tooling to a new Unicode/metacharacter source location.
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
# On this Windows validation path, lua.exe lives with its matching runtime DLL.
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
if(VERIFY_LEGACY)
  if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "VERIFY_LEGACY is the Windows-only retirement comparison")
  endif()
  execute_process(COMMAND powershell.exe -NoProfile -File
    "${_repo}/cmake/VerifyTestInventory.ps1" -BuildDirectory "${_scratch}/full"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 20 ENCODING UTF-8)
  require_exit("${_result}" "0" "legacy full-inventory parity: ${_err}")
  if(NOT _out MATCHES "CTest inventory is valid: 15 Airelinna V1 cases; hardware cases 13-15.")
    message(FATAL_ERROR "Unexpected legacy success output")
  endif()
  configure_file("${_repo}/cmake/VerifyTestInventory.ps1"
    "${_scratch}/VerifyTestInventory.ps1.backup" COPYONLY)
  file(SHA256 "${_repo}/cmake/VerifyTestInventory.ps1" _legacy_hash)
  file(SHA256 "${_scratch}/VerifyTestInventory.ps1.backup" _backup_hash)
  if(NOT _legacy_hash STREQUAL _backup_hash)
    message(FATAL_ERROR "Legacy recovery copy does not match")
  endif()
  message(STATUS "Legacy recovery copy: ${_scratch}/VerifyTestInventory.ps1.backup")
endif()
file(GLOB_RECURSE _markers LIST_DIRECTORIES false "${_scratch}/*body-ran")
if(_markers)
  message(FATAL_ERROR "Inventory query executed a test body")
endif()
message(STATUS "PASS tooling group 12: UTF-8 paths configuration and no test execution")
message(STATUS "TOOLING_OK 12 groups")
~~~

The synthetic ON/OFF configure uses Ninja as a development-test prerequisite only; it compiles nothing and does not modify the project's configured builds. TOOLING_NINJA is an optional harness-only absolute executable override; it is not a production launcher option. On this Windows host, Ninja is not on PATH, but the existing Visual Studio installation provides the executable used below. Recheck that path before execution. On another host, use Ninja from PATH or pass that host's absolute override; if neither is available, report the missing prerequisite rather than silently skipping the stale-metadata scenario.

The timeout fixture deliberately occupies one CPU core for approximately 30 seconds once per full harness run. It has a finite in-process deadline and no sleep child that could outlive the terminated CTest. Require a timeout in the reported CTest child status, not merely the word in a fixture path or a generic nonzero result. The 38-second upper bound excludes the fixture's own 40-second failure while allowing version-probe overhead; the numeric outer status excludes the harness's 45-second timeout. Give this command a terminal session/polling timeout appropriate to its duration.

- [x] **Step 2: Run the integration stage RED.**

~~~text
cmake -DLUA_EXECUTABLE=C:/lua/lua.exe -DTOOLING_NINJA=C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake
~~~

Expected: the nine existing groups pass, then the first full-inventory invocation fails because the launcher is absent.

- [x] **Step 3: Add the thin launcher.**

Create cmake/VerifyTestInventory.cmake:

~~~cmake
cmake_minimum_required(VERSION 3.28)
set(_invoking_directory "${CMAKE_CURRENT_BINARY_DIR}")
string(ASCII 11 12 _extra_whitespace)
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
execute_process(
  COMMAND "${_ctest}" --test-dir "${_build}" --show-only=json-v1 ${_configuration_args}
  COMMAND "${_lua}" -E verify_test_inventory.lua ${_profile_args}
  WORKING_DIRECTORY "${_tools}" TIMEOUT 30 RESULTS_VARIABLE _results
  OUTPUT_VARIABLE _success ERROR_VARIABLE _failure ENCODING UTF-8 COMMAND_ECHO NONE)
# CMake 3.28 supports RESULTS_VARIABLE. Do not add the 4.0-only fatal-mode NONE.
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
~~~

Only the launcher discovers host tools. Optional arguments use DEFINED, preserving values such as OFF; cmake_path absolute checks reject Windows drive-relative tool overrides. Do not duplicate Lua profile IDs or parse inventory JSON here.

- [x] **Step 4: Run the same full harness GREEN and calibrate every failure fixture.**

Require TOOLING_OK 12 groups, including upstream nonzero/consumer zero calibration, downstream zero/one, actual missing-working-directory launch failure, and the launcher's own approximately 30-second timeout. A harness timeout or a fixture calibration failure is not a passing negative test. Validate synthetic metadata behavior on each actual CTest version before claiming that host passed.

- [x] **Step 5: Check native discovery once outside the override-only harness.**

~~~text
cmake -DBUILD_DIRECTORY=build/browser-keep-alive-vs -DCONFIGURATION=Release -DINVENTORY_PROFILE=windows-policy -DLUA_EXECUTABLE=C:/lua/lua.exe -P cmake/VerifyTestInventory.cmake
~~~

This existing build's cache currently says tests ON/integration OFF; recheck its inventory rather than infer freshness from the cache. Expected only if its existing metadata is current: 12-case windows-policy success. Omit CTEST_EXECUTABLE to exercise sibling discovery. The harness additionally covers PATH-only Lua discovery on the local Windows lua.exe installation using a child-local PATH; it never changes machine/user PATH. No commit.

## Task 5: Verify cross-platform behavior, update active docs, and retire PowerShell

**Files:** Create docs/development/lua-tooling.md; modify docs/architecture/build-and-validation.md and THIRD_PARTY_NOTICES.md; remove only cmake/VerifyTestInventory.ps1 after the gates below.

- [x] **Step 1: Record final Windows evidence and legacy parity before deletion.**

~~~text
cmake -DLUA_EXECUTABLE=C:/lua/lua.exe -DTOOLING_NINJA=C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe -DRUN_LAUNCHER=ON -DVERIFY_LEGACY=ON -P tests/tooling/run_tooling_tests.cmake
cmake -DBUILD_DIRECTORY=build/browser-keep-alive-vs -DCONFIGURATION=Release -DINVENTORY_PROFILE=windows-policy -DLUA_EXECUTABLE=C:/lua/lua.exe -P cmake/VerifyTestInventory.cmake
~~~

Require the full synthetic harness and old/new full-profile parity to pass. The second command inspects only the available real nonhardware inventory; if its metadata is stale or unavailable, report that independently and use the calibrated synthetic tree for tool correctness. Do not reconfigure that user's build to make a check pass.

The windows-clangd preset declares tests OFF, but the existing build/clangd cache currently says tests/integration ON. This discrepancy is not repaired by this migration. If a current full Windows build is available, inspect it with the default profile without running its hardware cases:

~~~text
cmake -DBUILD_DIRECTORY=build/clangd -DCONFIGURATION=Release -DLUA_EXECUTABLE=C:/lua/lua.exe -P cmake/VerifyTestInventory.cmake
~~~

Expected only for current complete metadata: the preserved 15-case success line. A matching inventory is still not evidence of metadata freshness, executable availability, or test-body success.

- [x] **Step 2: Run the shared harness on an actual Linux host with Lua 5.4.**

Use the same current tools and local tests, not a clean clone missing uncommitted files. From that repository root in a Linux shell:

~~~sh
command -v lua5.4
lua5.4 -v
cmake --version
ctest --version
ninja --version
cmake -DLUA_EXECUTABLE="$(command -v lua5.4)" -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake
~~~

The interpreter lookup must yield an absolute executable before the final command. Require TOOLING_OK 12 groups and retain the actual OS/tool versions. Linux+Lua 5.4 and Windows+Lua 5.5 satisfy the two interpreter lines without an unnecessary cross-product. If no Linux/Lua 5.4 environment is available, stop the retirement acceptance at this checkpoint, report the missing environment, and request direction rather than install or claim coverage without evidence. Keep the old PowerShell file until required replacement acceptance is complete.

When an actual macOS host is available, run the same harness from its repo root with its supported absolute Lua path; otherwise label macOS unverified. Likewise record CMake 3.28 runtime coverage only if that version actually runs; source/help compatibility review is not an execution result.

- [x] **Step 3: Add the active developer guide.**

Create docs/development/lua-tooling.md:

~~~markdown
# Lua inventory tooling

The inventory gate checks CTest metadata without executing test bodies.
It does not attest that metadata is fresh, test commands work, tests passed,
or capture works. Use trusted, separately configured build trees for acceptance.

## Prerequisites

CMake and CTest 3.28+, and stock PUC Lua 5.4.x or 5.5.x.
Lua is only required when invoking this developer utility. The gate uses
the checked-in pure-Lua dkjson parser and does not download dependencies,
require LuaRocks/LPeg, or modify PATH. Ordinary Airelinna builds and the
application do not load Lua.

## Usage

Run from the repository root:

    cmake -DBUILD_DIRECTORY=build/windows-full -DCONFIGURATION=Release -DLUA_EXECUTABLE=C:/lua/lua.exe -P cmake/VerifyTestInventory.cmake

The build directory above is illustrative, not created by the command.
BUILD_DIRECTORY is required. CONFIGURATION is optional for single-config
generators; provide the intended configuration for multi-config builds.
If LUA_EXECUTABLE is omitted, the host PATH is searched for lua, lua5.5,
or lua5.4. An unsupported first match is an error; use an explicit override.
CTEST_EXECUTABLE optionally overrides the CTest beside CMake or on PATH.
Tool overrides must be absolute native executable paths.

| INVENTORY_PROFILE | Required IDs | Hardware IDs |
| --- | --- | --- |
| windows-v1-full, the default | 01-15 | 13-15 |
| windows-policy | 01-12 | None |
| portable-core | 01, 02, 03, 04, 05, 07 | None |

Choose a smaller profile explicitly with -DINVENTORY_PROFILE=windows-policy
or -DINVENTORY_PROFILE=portable-core. A smaller-profile pass never satisfies
full Windows V1 acceptance. Unknown/blank supplied options do not select defaults.

Relative build paths are relative to your invoking directory. From outside
the repo, give the launcher its correct absolute path. Quote the entire
-DNAME=value argument according to your shell when it contains spaces or
metacharacters such as &. Semicolons and newlines in paths/configuration
are rejected explicitly. Unicode paths are supported by the CMake launch
boundary; commands in the inventory remain opaque strings.

## Results and limitations

Zero means CTest and Lua both succeeded and the inventory matched the
selected profile. A launch/probe error, any child failure, or the 30-second
inventory timeout gives a nonzero result without a success banner.
The gate does not execute the command arrays in the JSON.

The 4 MiB limit and trailing-data check apply to text delivered by Lua stdio,
not raw OS-pipe bytes. Stock Windows Lua uses text-mode stdin: CRLF can become
LF and raw Ctrl+Z can hide subsequent bytes. The gate consumes trusted CTest
JSON, whose string control characters are escaped; it is not a general
binary-stream validator. No native module is added to change stdin mode.

Disabling tests in a reused build tree can leave old valid CTest metadata.
This gate can accept that retained inventory; it does not audit CMake
cache freshness or delete stale build files. A fresh tests-OFF/no-metadata
build fails. Tool portability does not add Linux/macOS application capture.

## Developer verification

The local, uncommitted tooling harness lives under tests/tooling:

    cmake -DLUA_EXECUTABLE=C:/lua/lua.exe -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake

On another host, supply that host's absolute supported Lua executable.
The stale-metadata fixture additionally needs Ninja on PATH, or the optional
harness-only -DTOOLING_NINJA argument with an absolute Ninja executable path.
This is not an inventory launcher option. The harness has 12
focused groups, creates isolated fixtures under build/, and does not add
Catch2/CTest inventory cases. Its single timeout fixture occupies one CPU
core for approximately 30 seconds. It keeps its uniquely named scratch
directory for inspection and never automatically deletes existing data.

Tests are excluded from commits by project policy; a clean checkout may
not contain this harness or the application's test sources. Document actual
OS/interpreter/CMake runs in local verification records; do not infer macOS
coverage from a Windows or Linux pass.
~~~

- [x] **Step 4: Update only the active architecture reference and dependency notice.**

In docs/architecture/build-and-validation.md, replace the inventory Mermaid node line with:

~~~mermaid
    Inventory["VerifyTestInventory.cmake → Lua<br/>default full profile"] --> Cases["CTest-discovered 15 cases<br/>IDs and hardware labels"]
~~~

Replace its paragraph naming the three gate entry points with:

~~~markdown
[VerifySourceBoundaries.cmake](../../cmake/VerifySourceBoundaries.cmake),
tests/CMakeLists.txt, and
[VerifyTestInventory.cmake](../../cmake/VerifyTestInventory.cmake) are the
active verification entry points. The Lua inventory tool keeps the default
full Windows contract at 15 cases; smaller profiles are explicit and do
not replace full acceptance. Its separate tooling harness does not add
Catch2 cases. See the [Lua tooling guide](../development/lua-tooling.md)
for prerequisites, profiles, and the metadata-freshness limitation.
~~~

Append this subsection to THIRD_PARTY_NOTICES.md, without modifying other notices:

~~~markdown
## dkjson 2.11 — development tooling dependency only

Source: https://dkolf.de/dkjson-lua/dkjson-2.11.lua

License: MIT

Copyright (C) 2010-2026 David Heiko Kolf.

The unmodified source and complete upstream license are retained in
tools/vendor/dkjson.lua; its source hash is recorded in tools/vendor/README.md.
Only the optional developer inventory utility uses this pure-Lua module.
The Airelinna executable does not load it.
~~~

Historical plans/specs and PowerShell shell-setup examples are not bulk-rewritten. Do not convert compiler setup, other CMake helpers, or release automation as part of this change.

- [x] **Step 5: Retire the exact old script only after the acceptance checkpoints pass.**

Re-read cmake/VerifyTestInventory.ps1 and confirm it has no unrelated user edits since the baseline. Remove this one file with apply_patch, not a glob, recursive delete, git clean, or reset. The old file is currently untracked: do not claim Git can recover an uncommitted deletion. The successful VERIFY_LEGACY run above prints an exact recovery-copy path after checking both SHA-256 values. Before removal, recheck that copy still matches the current script; if the script changed meanwhile, stop and inspect the overlap rather than removing unreviewed content. Record that recovery path for the user. Do not commit that backup or any fixtures.

Remove the temporary VERIFY_LEGACY block from the test harness after its parity result is recorded; the final harness must not require or invoke PowerShell. Keep the rest of the harness unchanged and rerun it once:

~~~text
cmake -DLUA_EXECUTABLE=C:/lua/lua.exe -DTOOLING_NINJA=C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake
rg --files --hidden --no-ignore -g "*.ps1" -g "!build/**" -g "!.git/**" -g "!.agents/**" -g "!.codex/**" .
rg -n "VerifyTestInventory.ps1|powershell|pwsh" cmake tools docs/architecture docs/development tests/tooling
git diff --check
git diff --cached --name-only
git status --short
~~~

Require the final harness to pass, no remaining first-party .ps1 outside build/dependencies, no active inventory-tool PowerShell dependency, and no staged files. Existing developer-shell documentation may still mention PowerShell; inspect matches rather than deleting unrelated guidance. Document the single retired file and the exact local recovery copy.

## Self-review and final acceptance checklist

| Spec requirement | Implementation/check location |
| --- | --- |
| No runtime dependency or speculative platform framework | File map; Tasks 2-4; no C++/top-level CMake edits |
| Full default, exact IDs/labels, smaller explicit profiles | Task 2 profiles/policy; groups 1-4 and 7; launcher profile admission |
| JSON types/null/envelope/trailing/limit/error containment | Task 2 shape group 5 and Task 3 actual CLI group 8 |
| Intentional ASCII whitespace difference, opaque command strings | Task 2 groups 6-7; developer guide/spec reference |
| Local parser pin/license, no global module fallback or downloads at runtime | Task 1, Task 3 group 9, harness preflight hash |
| Tool discovery/absolute paths/cwd/config/UTF-8/quoting | Task 4 launcher and groups 10/12 |
| Per-child failure, no false success, finite timeout | Calibrated real pipeline in group 11 |
| Fresh tests-OFF versus retained metadata | Actual ON/OFF synthetic configure in group 10 |
| No test bodies/capture; unchanged 15 Catch2 cases | Isolated marker fixture; no test registration or native consent changes |
| Proportionate Windows/Linux/Lua coverage; honest macOS limits | Task 5, no platform/version/profile Cartesian matrix |
| Safe retirement/docs and excluded commit content | Task 5; backups local, no auto-commit |

- [x] Re-read the approved spec against every row; inspect final file contents, not only test output.
- [x] Check no implementation section contains unresolved names, omitted code steps, or stale option names.
- [x] Keep 12 grouped scenarios, with table-driven variants; do not create separate test registrations per variant or add production test hooks.
- [x] Check first-party Lua syntax with the chosen interpreter/compiler, leaving no .luac artifacts in source.
- [x] Report actual commands, OS/runtime versions, results, and unavailable environments. Do not call the migration complete while required Linux/Lua 5.4 or pipeline calibration evidence is missing.
- [x] Implementation completion is not commit permission. Hand off changes and verification; tests/spec/plan/work records remain excluded from any subsequent explicitly authorized commit.

## References and execution handoff

- [CMake native process contract](https://cmake.org/cmake/help/latest/command/execute_process.html).
- [Host path operations](https://cmake.org/cmake/help/latest/command/cmake_path.html) and [program discovery](https://cmake.org/cmake/help/latest/command/find_program.html).
- [CTest 3.28 test-handler source](https://raw.githubusercontent.com/Kitware/CMake/v3.28.6/Source/CTest/cmCTestTestHandler.cxx): error flags stop ordinary JSON generation; the upstream-failure fixture must be calibrated as specified.
- [dkjson source](https://dkolf.de/dkjson-lua/dkjson-2.11.lua) and [published hashes](https://dkolf.de/downloads).
- [Lua stdio reader](https://www.lua.org/source/5.4/liolib.c.html#read_chars) and [Windows CRT text translation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/file-translation-constants?view=msvc-170): the stdin text boundary is not a raw binary transport guarantee.

Recommended execution is subagent-driven: fresh worker per task with a scoped file set, main-agent verification, then spec/code-quality review at each completed boundary. Alternatively execute these same checkpoints inline with executing-plans. Keep dependent tasks sequential; do not launch Task 4 against a changing Task 3 CLI/harness. No implementation or test execution is performed by writing this plan.

## Rolling review result (2026-09-08)

- Round 1, lua_tooling_plan_review_r1: Critical 0 / High 0 / Medium 1 / Low 1 in the original plan. Independently confirmed and corrected the timeout fixture's false-positive oracle. Also reproduced Windows Lua stdin CRLF/Ctrl+Z translation with small in-memory diagnostics and clarified that boundary in the plan, guide snippet, and spec; no native module or additional scenario group was introduced.
- Round 2, fresh lua_tooling_plan_review_r2: Critical 0 / High 0 / Medium 0 / Low 0 after a full review of all five tasks, proposed code, current source contracts, and both prior corrections. Ready for implementation-plan approval.

Both reviewers completed their work and were stopped before handoff; the available interface has no close/delete-thread operation, so this is not a claim of thread deletion or quota release. Main-agent checks confirmed all six proposed Lua blocks parse with local luac 5.5.1, while the Windows stdin probes checked only interpreter behavior. No production implementation, dependency vendoring, project build/tests, cross-platform execution, capture, staging, or commit was performed in this review. This result is plan-review evidence, not migration acceptance.

## Implementation result (2026-09-08)

All five tasks are implemented and passed separate spec/quality checkpoints.
Fresh final whole-migration reviewer lua_final_review_r1 approved with
Critical 0 / High 0 / Medium 0 / Low 0. Three confirmed CLI Low findings were
fixed with RED/GREEN evidence inside the existing scenario groups.

The final PowerShell-free 12-group harness passed on Windows with existing VS
CMake/CTest 4.3.1-msvc1 and Lua 5.5.1, and on Linux with CMake/CTest 3.28.3 and
Lua 5.4.6. macOS is unverified. Existing windows-policy inventory accepted 12
cases; the incomplete build/clangd inventory was correctly rejected without
reconfiguring it or running hardware cases.

The single old PowerShell script was retired only after parity, both host
checks and a verified recovery copy succeeded. See the
[execution record](../../verification/2026-09-08-lua-tooling-implementation.md)
for exact commands, environment diagnosis, review history and recovery path;
see the [developer guide](../../development/lua-tooling.md) for normal usage.
No staging, commit, push, package or publish operation was performed.
