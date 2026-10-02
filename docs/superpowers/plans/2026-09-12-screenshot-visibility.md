# Screenshot Visibility Implementation Plan

> For agentic workers: use executing-plans for this approved, single-scope change and an independent final reviewer.

**Goal:** Allow screenshots of the controller and floating replica.

**Architecture:** Keep native setup in Windows adapters. Controller validation returns NotAttempted; the replica factory initializes the same status. Keep the current-process eligibility filter and existing status interfaces.

**Tech Stack:** C++23, Win32, Catch2, CMake, clang-cl.

## Task 1: Red-green behavior change

- [x] Add a SECTION in tests/windows/overlay_policy_test.cpp using a hidden WS_EX_LAYERED STATIC HWND owned by a unique_ptr deleter. On DWM-capable hosts, require WDA_NONE before and after prepare_controller_window. Always assert NotAttempted and check nullptr returns RuntimeConstructionFailure. A reported unavailable DWM query leaves the setup-status check active on headless hosts. Existing case 11 covers class style eligibility.
- [x] Build airelinna_windows_policy_tests in build/p4-release and run case 11, confirming the new assertions fail on the previous exclusion behavior.
- [x] In platform/windows/capture_exclusion.cpp, preserve HWND and class checks and replace the controller return with `return core::CaptureExclusionStatus::NotAttempted;`.
- [x] In platform/windows/windows_runtime_factory.cpp replace the affinity call with `constexpr auto exclusion = core::CaptureExclusionStatus::NotAttempted;`, removing the now-unused capture_exclusion and GLFW native includes. Keep runtime construction and event publication intact.
- [x] Update the controller Doxygen contract in platform/windows/capture_exclusion.hpp. Run case 11 and case 09, then the existing non-hardware suite.

## Task 2: Documentation and verification

- [x] Update README.md, SECURITY.md, docs/architecture/desktop-host.md, docs/architecture/windows-runtime-overlay.md and related GLFW patch wording in build-and-validation.md. Update the local verification checklist. Explain default screenshot visibility and full-display recursion while preserving existing unrelated text.
- [x] Build Airelinna and verify_source_boundaries using `cmake --build build/p4-release --target Airelinna verify_source_boundaries` from the VS x64 developer environment.
- [x] Run `ctest --test-dir build/p4-release --output-on-failure -LE hardware`, language lint on changed Markdown, and scoped diff checks.
- [x] Have a fresh reviewer inspect the scoped patch against source, resolve genuine findings, then hand off the concrete executable path and validation limits. Preserve all existing edits and leave Git staging unchanged.

## Verification record: 2026-09-12

- Red: the focused new section failed on the previous behavior with Excluded versus NotAttempted and affinity 17 versus WDA_NONE (0).
- Green: the final focused section passed all 9 assertions, including native affinity checks on this host. Existing non-hardware CTest suite passed 12/12.
- Native Release build: Airelinna, airelinna_core_tests, airelinna_windows_policy_tests, and verify_source_boundaries completed with exit 0 using clang-cl in the VS x64 developer environment.
- Language lint: `C:/lua/lua.exe -E C:/Users/Mes/.codex/skills/zhTW-translocalizer-skills/scripts/lint.lua --profile final --format markdown README.md SECURITY.md docs/architecture/desktop-host.md docs/architecture/windows-runtime-overlay.md docs/architecture/build-and-validation.md` returned exit 0. The three Chinese architecture documents have zero warnings. The 17 warnings in the two English files concern existing paragraphs outside the edited text.
- Independent final review: 0 Critical / 0 Medium; one Low stale checklist phrase was confirmed against SECURITY.md and corrected. Review stopped at the requested severity threshold.
- Delivery: `build/p4-release/src/Airelinna.exe`. Product-window screenshots and the hardware capture suite remain manual acceptance steps; this run used an invisible owned HWND for policy checks and performed no screen capture.
- Changes remain local, with preexisting edits preserved and Git staging unchanged.
