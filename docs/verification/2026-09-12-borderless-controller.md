# Borderless controller verification — 2026-09-12

Scope: the approved full-client ImGui controller in the existing dirty Airelinna checkout, branch `p4-application-release`. Tests, work spec, plan and this record remain local. Pre-edit snapshots are in `D:/document/MesRepo/5568ke/build/airelinna-borderless-baseline-20260912`.

## Automated evidence

- Red/green, existing case 07: the full-client regression failed against the former floating root (10 expected assertions), then passed after the layout change. A focused interaction section passes 19 assertions for minimize, Quit, fixed-title scrolling, active-press and popup handling.
- Red/green, existing case 11: missing native attachment initially produced two expected failures. The tiny-client fixture exposed three overlapping-edge failures; clamping each band to half its extent resolves them. Final case 11 passes 380 assertions. The detach assertion compares the original window procedure's observed result, preserving the fixture's native STATIC behavior.
- Latest Release product, both existing test executables and source-boundary target: build exit 0. Final CTest: 12/12 non-hardware cases pass, 0 failures. Test inventory remains twelve cases.
- Boundary rejection probes: portable `src/ui/probe.hpp` including `<windowsx.h>` fails with the native-header boundary diagnostic; Windows `probe.hpp` including `desktop/desktop_host.hpp` fails with the disallowed-desktop-include diagnostic. Both expected exit 1. The production scanner passes with the exact portable interaction-port exception.
- Scoped `git diff --check`: exit 0; Git emits existing LF/CRLF conversion notices.

Build from the repository root with the native x64 developer environment:

```powershell
cmd /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build/p4-release --target Airelinna airelinna_core_tests airelinna_windows_policy_tests verify_source_boundaries'
ctest --test-dir build/p4-release --output-on-failure -LE hardware
```

Artifact: `build/p4-release/src/Airelinna.exe` (1,351,168 bytes at this verification).

## Documentation checks

```powershell
C:/lua/lua.exe -E C:/Users/Mes/.codex/skills/zhTW-translocalizer-skills/scripts/lint.lua --profile final --format markdown docs/architecture/README.md docs/architecture/desktop-host.md docs/architecture/application-ui.md docs/architecture/windows-runtime-overlay.md README.md
```

Result: 5 files scanned, 15 warnings. Baseline comparison confirms all 15 are preexisting: 12 in English README prose, and 3 at unchanged application-ui lines (one enumeration heuristic and two existing wording suggestions). This change adds zero warnings. New prose, source links and Mermaid data/control edges were inspected against the implementation.

## Fresh reviews

- Spec: `borderless_spec_review_1`, 0 Critical / 0 Medium.
- Plan: `borderless_plan_review_1`, 0 Critical / 0 Medium.
- Implementation spec/architecture: `borderless_implementation_spec_review_1`, 0 Critical / 0 Medium. Reviewer checked actual GLFW/ImGui callback chaining and constructor cleanup.
- Final code-quality review: `borderless_final_quality_review_1`, 0 Critical / 0 Medium. Fresh reviewer checked all scoped source, tests, build rules and documentation against pre-edit snapshots. Each reviewer stopped after completing its review. Manual acceptance remains separately pending.

## Manual acceptance

Status: **Not run**. Automated native fixtures create hidden test-owned windows; they exercise messages and the current window DPI. Physical mouse drag, mixed-DPI monitor movement, minimize/taskbar restore, screenshot appearance, Alt+F4 and capture-active close/drain require the manual controller-frame checks in [windows-v1-checklist.md](windows-v1-checklist.md). This task launched no product GUI or capture session.
