# Lua inventory tooling

The inventory tooling validates metadata from a separately configured, trusted
build tree. It checks discovered test names, labels, and command metadata; it
does not inspect test bodies, prove metadata freshness or commandability,
attest captured output, or claim that a test passed.

## Prerequisites

- A stable CMake and CTest 3.28 or newer.
- A stock PUC Lua 5.4.x or 5.5.x interpreter.

Lua is an optional development utility, not an ordinary build or application
runtime dependency. The tooling uses the checked-in, pure-Lua dkjson copy and
does not download packages or require LuaRocks, LPeg, or PATH changes.

The locally tested PATH installation of CMake 4.0.0-rc2 has a confirmed crash
when `cmake_path` handles a Unicode path. Select a stable CMake executable
instead of adding a platform workaround. Visual Studio's CMake 4.3.1 and Linux
CMake 3.28.3 have been tested; macOS has not been verified. This does not imply
that all CMake 4.x releases are affected or change global prerequisites.

## Public launcher

Run from the repository root. The following path is illustrative; this command
does not create `build/windows-full`:

```text
cmake -DBUILD_DIRECTORY=build/windows-full -DCONFIGURATION=Release -DLUA_EXECUTABLE=C:/lua/lua.exe -P cmake/VerifyTestInventory.cmake
```

`BUILD_DIRECTORY` is required and must name an existing build tree containing
CTest metadata. `CONFIGURATION` is optional; supply the intended configuration
for a multi-config tree because the launcher does not guess `Release`.

When supplied, `LUA_EXECUTABLE` must be an absolute native path. Otherwise
`find_program` searches the host PATH for `lua`, `lua5.5`, then `lua5.4`. The
first match is selected and fails immediately if its version is unsupported;
the launcher does not continue searching for a later supported match. When
supplied, `CTEST_EXECUTABLE` must also be absolute; otherwise the launcher tries
the sibling of CMake and then PATH.

The default strict profile is `windows-v1-full`: cases 01-15, with `hardware`
on exactly cases 13-15. Explicit smaller profiles are:

| Profile | Required case IDs | Hardware cases |
|---|---|---|
| `windows-policy` | 01-12 | none |
| `portable-core` | 01, 02, 03, 04, 05, 07 | none |

Select a smaller profile with `INVENTORY_PROFILE`. A smaller profile is never
full acceptance. Unknown profile names and blank supplied documented values
fail rather than falling back to a default.

```text
cmake -DBUILD_DIRECTORY=build/windows-policy -DINVENTORY_PROFILE=windows-policy -P cmake/VerifyTestInventory.cmake
cmake -DBUILD_DIRECTORY=build/portable-core -DINVENTORY_PROFILE=portable-core -P cmake/VerifyTestInventory.cmake
```

Paths are relative to the invoking working directory. When invoking from
outside the repository, pass the absolute path to the repository's
`cmake/VerifyTestInventory.cmake`. Quote the whole `-DNAME=value` argument as
required by the shell when a value contains spaces or `&`. Semicolons and CR/LF
are rejected. Unicode path handling stays at the CMake boundary, while command
strings from CTest are opaque data.

## Results and trust boundary

The public launcher returns zero only when both CTest and Lua return normally
with zero status and the selected profile validates. Process probes time out
after 5 seconds and the inventory query after 30 seconds. Any failure returns
nonzero and prints no success banner.

The validator never executes JSON `command` entries. CTest still interprets
metadata and discovery from the trusted configured build tree; this is not an
untrusted-build sandbox. The internal stdin-only Lua CLI does not attest CTest
success, so use the public launcher end to end. For CLI option details only,
run `cd tools` and then `lua -E verify_test_inventory.lua --help`.

Input is limited to 4 MiB, and the trailing-input check applies to Lua-visible
text rather than raw binary bytes. The Windows CRT translates CRLF to LF and
treats Ctrl-Z as end-of-file, which can hide later bytes. The trusted CTest
producer escapes control characters, and the tool loads no native binary
module. The pure validator's ASCII definition of nonblank uses six ASCII
whitespace characters; U+00A0 and U+3000 may remain opaque command arguments or
extra labels. Required labels are still matched exactly. These details prevent
treating a PowerShell text-pipeline comparison as byte parity.

A build reconfigured from tests enabled to tests disabled can retain stale
metadata and pass. Use a fresh build tree; the launcher does not audit or clean
the cache. A fresh tests-disabled tree with no metadata fails. This portable
tooling does not implement Linux or macOS application capture.

## Developer harness

The local, uncommitted harness can be run from the repository root:

```text
cmake -DLUA_EXECUTABLE=C:/lua/lua.exe -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake
```

Use an absolute Lua path on other hosts. Ninja is required only for the
harness's synthetic configure test; `TOOLING_NINJA` is an optional harness-only
absolute override and is not a production launcher option.

The harness has exactly 12 tooling groups, not Catch2 cases. A complete run
uses one CPU core and takes about 30 seconds because of a timeout fixture. Each
run creates a unique scratch tree under `build/`; it retains that tree and
never deletes it automatically. Because the tests are not committed, a clean
checkout may not contain them. Treat actual versions and results as local
verification records, and do not infer macOS coverage from them.

Responsibility is narrow: CMake owns native process invocation, Lua owns CLI
parsing and diagnostics, and pure Lua policy plus fixed profile data owns
inventory validation. No application runtime target links this tooling.
