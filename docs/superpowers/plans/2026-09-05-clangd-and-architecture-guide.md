# clangd and Architecture Guide Implementation Plan

> **For agentic workers:** Use executing-plans for the ordered tooling/verification steps and independently scoped parallel comment/documentation tasks. This task does not authorize commits or release operations.

**Goal:** Make the current Windows C++23 checkout navigable with clangd and explain its implemented architecture through extensive source comments and Mermaid documentation.

**Architecture:** Preserve every production declaration, expression, ownership rule and target dependency. Generate a real compilation database in a separate Ninja tooling build; document current components rather than inventing new abstractions.

**Tech Stack:** clangd/clang-cl 22, CMake/Ninja, Windows SDK, Markdown/Mermaid.

---

## Task 1: Capture baseline and configure clangd

- [x] Preserve the existing dirty worktree and snapshot first-party source tokens before comments are added.
- [x] Add `.clangd` with `CompileFlags.CompilationDatabase: build/clangd`, background indexing and strict unused-include diagnostics.
- [x] Add `CMakePresets.json` with a Windows Ninja/clang-cl tooling preset exporting `compile_commands.json`; do not change the existing `build/p4-release` generator.
- [x] Configure the tooling build in a VS developer environment, reusing the locally verified dependency sources for this checkout, and build generated shader headers.
- [x] Run clangd checks and an LSP declaration/definition check against real project files. Review include-cleaner diagnostics; do not blindly remove Windows/WinRT/GLFW ordering or umbrella includes.
- [x] Write `docs/development/clangd.md` with setup, database refresh, editor integration and measured include diagnostics/limitations.

## Task 2: Comment and document portable components

- [x] Add class/function contract and algorithm rationale comments to `include/airelinna/core`, `include/airelinna/app`, `include/airelinna/ui`, `src/core`, `src/app`, and `src/ui`.
- [x] Create `docs/architecture/core.md` and `docs/architecture/application-ui.md`, each with actual type/file links and Mermaid diagrams for internal responsibilities and control/data flow.

## Task 3: Comment and document native components

- [x] Add ownership, thread, publication and teardown comments throughout `platform/windows`, including private `detail` policies; preserve code tokens.
- [x] Create `docs/architecture/windows-capture.md` and `docs/architecture/windows-runtime-overlay.md`, with source identity, capture intake, runtime coordination, overlay transactions and hotkey/system-adapter diagrams.

## Task 4: Comment and document rendering and composition

- [x] Add comments to `render`, `shaders`, `include/airelinna/desktop`, `src/desktop`, and `main.cpp`, explaining the two independent presentation owners and CPU/GPU retirement paths.
- [x] Create `docs/architecture/renderer.md` and `docs/architecture/desktop-host.md`, including resource-owner diagrams and retirement/recovery sequences.
- [x] Create `docs/architecture/README.md` as the big-picture entry point and module index; add `docs/architecture/build-and-validation.md` for target graph, shader/dependency gates and the fixed 15-case inventory.

## Task 5: Verify and hand off

- [x] Compare first-party C++/shader tokens with the baseline, excluding comments and whitespace; intentional include edits, if any, must be separately reviewed.
- [x] Validate Mermaid syntax and local Markdown links against the actual files.
- [x] Run clang-format checks, `git diff --check`, full `cmake --build build/p4-release`, `verify_source_boundaries`, and `ctest --test-dir build/p4-release --output-on-failure -LE hardware`.
- [x] Confirm no new test cases and no staging/commits. Report tooling navigation evidence, include findings and any native/editor verification limitations.

## Verification record (2026-09-05)

- Tooling Ninja configure and `clangd-generated` shader build: passed. Local pinned dependency copies are inside `build/clangd/_offline-src`; no gate was weakened.
- clangd 22.1.6 LSP: 43 first-party translation units, 0 error diagnostics, 9 unused-include candidates and 17 unrelated tidy warnings. Four declaration/definition probes and Doxygen hover passed. Final server error log is empty after excluding resource-compiler background indexing.
- Source snapshot: all 75 first-party C++/shader files received comments; 101349 non-comment tokens match the pre-task dirty-worktree snapshot. Comment lines increased from 254 to 1517. No includes were removed.
- clang-format dry run: 73 C++ headers/sources, 0 failures. Shader compilation passed in the full build.
- Mermaid 11.17.2 parser: 47 diagrams across 9 documents; all 139 local document/file links exist. Syntax validation is not an IDE visual-preview inspection.
- Full `build/p4-release` build and `verify_source_boundaries`: passed. Textual boundary checks initially matched cross-layer names in new comments; those comments now describe responsibilities, while exact type relationships stay in docs. The gate itself was not modified.
- Non-hardware CTest: 12/12 passed. Inventory script: the existing 15 cases, with hardware cases 13-15. No cases added, no capture/GUI/hardware tests executed.
- Fresh tooling/docs reviewer: no Critical/Medium in the requested review scope. Both Low findings (resource-script indexing and missing Git prerequisite) verified and addressed.
- No editor-global settings, commits, staging, release operations or source uploads. Actual editor UI navigation remains for the user's client; direct local LSP navigation is verified.
