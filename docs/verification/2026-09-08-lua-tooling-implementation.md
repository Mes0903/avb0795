# Lua tooling implementation evidence

Date: 2026-09-08. Local work record; do not commit.

Status: Implementation and all review gates complete. Fresh whole-migration
review approved with Critical 0 / High 0 / Medium 0 / Low 0. Both required host
runs and legacy parity passed; macOS remains unverified. No commit was made.

## Scope and environment

The approved migration changes only developer inventory tooling and its active
documentation. Existing C++/i18n changes are preserved. No staging, commit,
application launch, hardware test, package, or publish operation is authorized.

| Host | Lua | CMake / CTest | Ninja |
| --- | --- | --- | --- |
| Windows, policy/CLI | C:/lua/lua.exe, PUC 5.5.1 | 4.0.0-rc2 | Visual Studio bundled 1.13.2, not on PATH |
| Windows, full launcher | Same Lua | Existing VS 4.3.1-msvc1; 12 groups passed | Same Ninja |
| Ubuntu WSL | /usr/bin/lua5.4, PUC 5.4.6 | 3.28.3 | /usr/bin/ninja, 1.11.1 |
| macOS | Not run | Not run | Not run |

WSL requires the environment's approved sandbox escalation. The same current
working-tree files are accessed at /mnt/d/document/MesRepo/Airelinna.
No interpreter or module installation was performed.

## Incremental verification

- Task 1: unmodified dkjson 2.11 SHA-256
  197cb50834c642f84b4cf99fe724932c50e6d9c92faec7ad89aa25e91df4d481.
  Narrow Git text attribute is unset. Actual Windows 5.5.1 and Linux 5.4.6
  parser smoke checks passed.
- Task 2 RED: policy harness exit 1 because test_inventory module was missing.
  GREEN: POLICY_OK 7 groups, exit 0, independently rerun on both hosts.
- Task 3 RED: policy passed, then actual Lua exit 1:
  cannot open verify_test_inventory.lua: No such file or directory.
  GREEN: TOOLING_OK 9 groups, exit 0, independently rerun on both hosts.
  Root Windows scratch: build/lua-tooling-214bd104aca9.
  Root Linux scratch: build/lua-tooling-1d7cbfc4dd8c.
- Lua syntax checks emit no .luac source artifacts.

Windows policy command:

    cmake -E chdir tools C:/lua/lua.exe -E ../tests/tooling/policy_test.lua

Windows CLI harness:

    cmake -DLUA_EXECUTABLE=C:/lua/lua.exe -P tests/tooling/run_tooling_tests.cmake

Linux CLI harness, invoked from Windows PowerShell:

    wsl.exe --distribution Ubuntu --cd /mnt/d/document/MesRepo/Airelinna --exec /usr/bin/cmake '-DLUA_EXECUTABLE=/usr/bin/lua5.4' -P tests/tooling/run_tooling_tests.cmake

The first Linux CLI-harness attempt used an unquoted -D argument. An actual
printf argv probe confirmed PowerShell split the Lua path into /usr/bin/lua5
and .4. Quoting the entire argument fixed invocation; no project-code change
was required.

## Independent reviews

| Task | Fresh spec reviewer | Fresh quality reviewer | Result |
| --- | --- | --- | --- |
| 1 | lua_task1_spec | lua_task1_quality | Critical/High/Medium/Low: all 0 |
| 2 | lua_task2_spec | lua_task2_quality | Critical/High/Medium/Low: all 0 |
| 3 | lua_task3_spec_r2 | lua_task3_quality_r3 | Final Critical/High/Medium/Low: all 0 |
| 4 | lua_task4_spec | lua_task4_quality | Critical/High/Medium/Low: all 0 |
| 5 | lua_task5_spec | lua_task5_quality | Critical/High/Medium/Low: all 0 |

Final whole-migration reviewer lua_final_review_r1 independently read the
complete scoped implementation, harness and docs, and approved all severities
0. It reran policy seven groups, six syntax checks, three-profile round trips,
selected malformed shapes and existing full/upstream/downstream launcher
fixtures. Failures remained nonzero without success banners (upstream
CTest=8/Lua=0; downstream CTest=0/Lua=1). The full cross-platform harness results
are root-run evidence above/below, not falsely attributed to that reviewer.

Task 3 initial spec review (lua_task3_spec) found Low 1, other severities 0:
fractional/NaN decoder positions reached string.sub and became internal exit 2
instead of rejected-input exit 1. The root independently reproduced it. The
same group 8 now table-drives exception, fractional, and NaN decoder faults;
RED observed expected 1/got 2, then an integer guard fixed it. Both hosts
passed all nine groups again (Windows scratch build/lua-tooling-ac36289aa763,
Linux scratch build/lua-tooling-a63900d5f9ef). Fresh full spec re-review
(lua_task3_spec_r2) passed with all severity counts 0 and independently reran
the Windows nine-group harness. Initial quality review (lua_task3_quality)
found another Low issue: a failed ordinary stderr write retained input error
code 1 rather than I/O code 2. Root reproduced it; an existing group 9 probe
went RED before the checked-write fix. Both hosts passed nine groups again
(Windows build/lua-tooling-1d9edc5fbe28, Linux build/lua-tooling-4ccaec73b29d).
Fresh full quality re-review (lua_task3_quality_r2) confirmed both fixes and
found Low 1: buffered stdout could fail only during exit flush and still
return 0. Root verified actual Linux stdout redirected to /dev/full returned
0 for valid input; stderr redirected there returned 2 for malformed input.
The existing group 9 regression first failed with expected 2/got 0. Explicit
flush checks now protect both success paths. Root reran both nine-group
harnesses and the actual Linux /dev/full probes: accepted inventory and help
now both return 2. Current scratches: Windows build/lua-tooling-05bf99a00915,
Linux build/lua-tooling-070d5a7e0559. Fresh full quality reviewer
lua_task3_quality_r3 approved with all severity counts 0 and independently
reran the Windows harness. All three confirmed Low findings are resolved.

The available collaboration interface has no close/delete-thread operation.
Completed/stopped reviewers are not claimed to have been manually deleted.

## Existing-build baseline and retirement safety

The initial ordinary PowerShell invocation was blocked before execution by
execution policy. The user subsequently approved process-only
-ExecutionPolicy Bypass for this retirement comparison. No permanent policy
setting is changed.

The old script's build/clangd Release check then ran and rejected the
inventory: only four Catch2 NOT_BUILT placeholder registrations, not the
required 15 cases. This is not a full-test or capture result.

A separate real CTest show-only query on build/browser-keep-alive-vs Release
returned 12 named cases. The later new-launcher profile check passed, as
recorded below.
Neither existing build was reconfigured or cleaned.

## Task 4 environment diagnosis

RED: the first real full launcher invocation failed because the launcher did
not exist, after all nine prior groups passed. During integration, groups 10
and 11 passed, then the PATH CMake 4.0.0-rc2 crashed on Unicode build paths
without a CMake diagnostic (exit -1073740791 / 0xc0000409).

Root isolated the crash to three lines in the retained
build/lua-cmake-unicode-probe-20260908.cmake, containing only the minimum
version, cmake_path SET with a Unicode string, and a success message. The
PATH binary crashed; the already-installed Visual Studio CMake 4.3.1-msvc1
ran the same file successfully. Its CTest also reports 4.3.1-msvc1.

Full Windows verification used this explicit existing executable:

    C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/CMake/bin/cmake.exe

No install or persistent PATH change was made. Experimental rc2-specific
fallbacks were removed before acceptance; production retains the approved
shared CMake path implementation and Unicode tests are not weakened.

Both full integration runs passed with TOOLING_OK 12 groups / exit 0:

- Root Windows stable run: build/lua-tooling-40958b75ef4b, including successful
  old/new full-profile parity and SHA-256-verified recovery copy.
- Root Linux CMake/CTest 3.28.3, Lua 5.4.6: build/lua-tooling-bbe21e9a8d30.
- Worker Windows stable run: build/lua-tooling-aa1597ef31cd. Additional direct
  timeout check observed 30 seconds, CTest timeout, Lua result missing, and
  nonzero launcher exit.

The downstream fault fixture replaces case 01's original LABELS assignment.
Appending another set_tests_properties had accumulated labels and retained
the required label; the calibrated fixture now requires CTest=0, Lua=1.
This correction changes only private test data, not policy or an oracle.

Root stable-Windows launcher on build/browser-keep-alive-vs Release accepted
windows-policy (12 cases, no hardware). The build/clangd default query still
correctly rejected four NOT_BUILT placeholders with CTest=0, Lua=1.

Retirement recovery copy (retained after the original's approved removal):

    build/lua-tooling-40958b75ef4b/VerifyTestInventory.ps1.backup

Root independently rechecked its SHA-256 against the unchanged original:
F73A005B192CF28273ED674DBFFD04675CC1601F834FF2231AB41271A20006EB.

## Post-retirement verification

After immediate original/backup hash rechecks, the worker removed only
cmake/VerifyTestInventory.ps1 with apply_patch and removed the temporary
VERIFY_LEGACY harness block. The original was untracked; recovery is via the
retained verified backup, not Git. No other data or scratch tree was deleted.

The final PowerShell-free harness passed all 12 groups / exit 0:

- Root Windows: build/lua-tooling-dd4041df637d.
- Worker Windows: build/lua-tooling-455ea50f7a3e.
- Root Linux: build/lua-tooling-624c97923144.

Final full Windows command (PowerShell, repository root):

    & 'C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/CMake/bin/cmake.exe' -DLUA_EXECUTABLE=C:/lua/lua.exe -DTOOLING_NINJA=C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake

Final full Linux command (invoked from Windows PowerShell):

    wsl.exe --distribution Ubuntu --cd /mnt/d/document/MesRepo/Airelinna --exec /usr/bin/cmake '-DLUA_EXECUTABLE=/usr/bin/lua5.4' -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake

Active-tool/reference searches find no first-party .ps1 outside ignored
build/tooling state, and no active inventory PowerShell dependency. Only the
pre-existing PowerShell code fences in docs/development/clangd.md remain.
All six first-party Lua files passed luac -p; no .luac artifact was created.
Approved Tasks 1-3 file hashes are unchanged. The 19 pre-existing modified
tracked files still match the initial baseline. Git diff --check passes
(ordinary LF/CRLF conversion warnings only); the staging index is empty.

The main agent checked the guide against real launcher options and corrected
the initial draft's first-supported/first-match wording and missing explicit
option names before independent Task 5 review. Application/runtime code,
top-level CMake and source-boundary allowlists remain untouched by this task.

Original untracked cmake/VerifyTestInventory.ps1 SHA-256:
F73A005B192CF28273ED674DBFFD04675CC1601F834FF2231AB41271A20006EB.
The original was retained until new-tool Windows/Linux acceptance, valid
synthetic full-profile parity, and an independently verified recovery copy
succeeded. It was then removed as described above.
