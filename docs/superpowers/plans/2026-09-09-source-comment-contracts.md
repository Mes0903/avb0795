# High-density Source Documentation Implementation Plan

> **For agentic workers:** Use `subagent-driven-development` for the documentation
> batches, with independent spec-compliance then quality review. Root owns the
> baseline and integration. The user authorizes execution after separate spec and
> plan rolling reviews; do not pause for an execution-choice question.

**Goal:** Make current first-party source understandable through source-accurate,
high-density comments, without changing program behavior.

**Architecture:** Preserve every layer and owner. Put C++ Doxygen contracts on
declarations and selective WHY explanations at implementation decisions.
Supporting Lua/CMake/GLSL use native comments; no new runtime module or framework.

**Tech Stack:** Existing C++23/Clang, CMake/CTest, Lua 5.5.1, GLSL, Markdown.

**Spec:** `docs/superpowers/specs/2026-09-09-source-comment-contracts-design.md`
(revised spec review round 2: Critical/High/Medium/Low all zero).

**Status:** Revised plan approved by fresh `comment_plan_review_r2` on 2026-09-09:
Critical 0 / High 0 / Medium 0 / Low 0. Implementation and independent
spec-compliance, quality and final reviews completed on 2026-09-09, each with
zero findings. Preservation checks, fresh Windows build, 12 existing non-hardware
cases and 12 tooling groups passed. No commit; see the verification record.

## Global constraints

- Work against the dirty working tree, including untracked i18n and Lua sources.
  Do not reset, stash, stage, commit, push, or change branch/worktree.
- Only comments and named documentation/records may change. Preserve code tokens,
  includes, literals, directives, signatures, compiler/tool pragmas, build behavior,
  generated-content strings, vendored bytes, and test bodies.
- Source comments are English. The guide may use Traditional Chinese.
- Use `/// @file` with separate `/// @brief`, meaningful Doxygen contracts, and
  ordinary internal WHY comments. No accessor quota, parameter/type/name catalog,
  deleted-member boilerplate, or duplicated explanations.
- Read implementations/callers before promising ownership, exception behavior,
  thread safety, rollback, completion, or fallback. Report existing bugs separately.
- Read `cmake/VerifySourceBoundaries.cmake`: its regex scans comments too. Never
  relax it to accommodate prohibited cross-layer names in prose.
- This is documentation-only: no artificial red/green product test is added.
  Token/text preservation, existing tests and independent prose review replace
  feature-development TDD. Do not install Doxygen or another tool.
- Stop a finished agent before replacement. Each rolling-review round is a new
  agent; interruption is not represented as guaranteed thread deletion.

## Task 1: Freeze and verify the live baseline

**Read:** The source inventory below; protection set `tests/`, `tools/vendor/`,
`.clang-format`, `.gitattributes`, and Git index.
**Create:** Only ignored `build/source-comments-<unique-id>/` scratch artifacts.
Never open `imgui.ini` or user config data.

- [x] Record branch, HEAD, Git status and index SHA-256. Confirm the current
  `p4-application-release` checkout. Do not branch, stash or export from HEAD.
- [x] Enumerate exact source paths (currently 102). Copy their current bytes under
  scratch `before/` preserving relative paths, including untracked files. Record
  the manifest and protection-set hashes for final comparison.

```powershell
$commentFiles = @(rg --files include src platform render shaders tools cmake -g '*.hpp' -g '*.cpp' -g '*.vert' -g '*.frag' -g '*.lua' -g '*.cmake' -g 'CMakeLists.txt' -g '!tools/vendor/**') + @('main.cpp','CMakeLists.txt')
git status --short
git branch --show-current
git rev-parse HEAD
git ls-files --stage
```

Use `New-Item`/`Copy-Item -LiteralPath` for baseline copies. Create helper/record
content with `apply_patch`; no `.ps1` is added. Snapshotting does not authorize
overwriting current source. Hash the protected set without editing it.

- [x] Run baseline checks, inspecting each exit code separately:

```powershell
$commentCmake = 'C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/CMake/bin/cmake.exe'
$commentCtest = 'C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/CMake/bin/ctest.exe'
& $commentCmake -DAIRELINNA_SOURCE_ROOT=D:/document/MesRepo/Airelinna -P cmake/VerifySourceBoundaries.cmake
& $commentCtest --test-dir build/browser-keep-alive-vs -C Release -LE hardware --output-on-failure
```

Expected: scan exit 0 and 12 existing non-hardware cases pass. Cached binaries
alone do not prove current-source buildability; Task 4 requires a fresh build.
Use existing VS stable CMake rather than the locally faulty PATH 4.0.0-rc2.
Do not run capture/hardware tests or open windows.

## Task 2: Document source by ownership boundary

**Files:** Exact Task 1 manifest, partitioned below. Each writer stays inside its
partition; root supplies the full requirements/context. Review the task-only
baseline diff, not unrelated Git changes from HEAD.

This is one source-documentation task with bounded module batches, integrated
before independent spec-compliance then accuracy/quality review. Independent
non-overlapping batches may run concurrently within available slots; never place
two writers on one file. With one available slot, use the same scope sequentially.

### Batch A: Portable contracts, application, localization and UI

Inspect `include/airelinna/core/*.hpp`, `src/core/*.cpp`,
`include/airelinna/app/*.hpp`, `src/app/*.cpp`,
`include/airelinna/i18n/*.hpp`, `src/i18n/*.cpp`,
`include/airelinna/ui/controller_view.hpp`, and `src/ui/*.{hpp,cpp}`.

- [x] Read definitions/callers and relevant existing tests. Explain FIFO versus
  latest-value handoff, stale identity rejection, state transitions, FPS pacing,
  config defaults/optional fallbacks/atomic replacement, runtime retention and
  synchronous event precedence.
- [x] Document requested versus effective language, exact persistence tokens
  versus family matching, borrowed static catalog text, fallback, view lifetime,
  stable widget identity and command-only UI responsibility.
- [x] Add contracts and selected mechanism explanations; retain sufficient
  comments. Example anchor verified against `src/i18n/language.cpp`:

```cpp
/// @brief Match a supported language family without querying the environment.
///
/// Matching is ASCII case-insensitive for the two-letter primary subtag.
/// Hyphen-separated suffixes must be nonempty ASCII alphanumeric groups of at
/// most eight characters; this is deliberately not a full language-tag parser.
/// Every accepted zh family selects the available Traditional Chinese catalog,
/// including tags whose region would normally prefer another Chinese script.
/// @return A supported language, or no value for malformed/unsupported input.
/// The caller, not this matcher, decides fallback and system-query policy.
```

Do not duplicate that paragraph at the definition or document every catalog row.

### Batch B: Native capture, runtime, adapters and optional feature

Inspect every `.hpp`/`.cpp` under `platform/windows/`, including `detail/` and
`features/`. This writer owns no `render/` or `src/desktop/` files.

- [x] Explain source identity/observation leases, callback wake borrowing,
  generation/epoch rejection, intake coalescing, checkout retirement, and what
  internal quiescence does not prove about external frame owners.
- [x] Document runtime/factory publication, rollback/loss of verified state,
  independent stop barriers and retained uncertainty; overlay interaction and
  transactions; hotkey, exclusion, DPI, file replacement, language/font adapters.
- [x] Explain the browser helper's narrow lifetime and failure reporting without
  inventing guaranteed browser behavior, minimized capture or other-platform support.
- [x] Consolidate substantial API contracts into coherent Doxygen blocks. Explain
  important internal transitions rather than narrating every native API call.

### Batch C: Replica renderer and shaders

Inspect all `render/*.hpp`, `render/*.cpp`, `shaders/replica.vert`,
`shaders/replica.frag`. Shader CMake belongs to Batch E.

- [x] Explain bridge capability/selection, aspect fit, format/color conventions,
  bounded slot phases and newest-frame selection.
- [x] Read full `windows_render_stack.cpp`; explain resource groups, separate
  copy/acquire/submission/presentation proof, independent device loss, CPU versus
  GPU transport, allocation barriers, retained redraw and safe destruction.
- [x] Explain retry-episode reset and drain evidence. No invented throughput,
  allocation, synchronization, driver or completion guarantee.
- [x] Add GLSL stage/input/output/coordinate comments with `//`, preserving
  `#version` and all literals/statements.

### Batch D: Controller host and composition

Inspect `include/airelinna/desktop/desktop_host.hpp`,
`src/desktop/controller_font.hpp`, `src/desktop/desktop_host.cpp`, `main.cpp`.

- [x] Document controller-only graphics ownership, borrowed font bytes,
  startup atlas configuration and upload lifetime.
- [x] Explain event-loop command/deadline/wake ordering, hidden/zero-size handling,
  controller recovery/retirement, terminal disposition, composition lifetime and
  non-unwinding abandon. Do not put lifecycle policy into `main.cpp`.

### Batch E: Developer tools and build helpers

Inspect `tools/verify_test_inventory.lua`, `tools/lib/inventory_profiles.lua`,
`tools/lib/test_inventory.lua`, `CMakeLists.txt`, `src/CMakeLists.txt`,
`shaders/CMakeLists.txt`, and all six `cmake/*.cmake` files:
`Dependencies`, `EmbedSpirv`, `Packaging`, `VerifyGlfwPatch`,
`VerifySourceBoundaries`, `VerifyTestInventory`.

- [x] Explain module inputs/results, trusted profiles, tagged JSON containers,
  diagnostic order, bounded input and CLI exit categories using native `--`.
- [x] Explain CMake process/path orchestration, dual status checks, timeout and
  metadata-only limitations, build target roles, pins/patches, shader generation
  and opt-in packaging. Do not execute release operations.
- [x] Freeze generated header/source strings in `EmbedSpirv.cmake`, all regexes,
  options and literals. Add only real CMake `#` comments outside quoted/bracket
  arguments; a `#` inside a generated-source string is not a CMake comment.

### Batch integration and review

- [x] Record every file as inspected/changed or inspected/sufficient, with concrete
  contract additions rather than a comment-line quota.
- [x] Inspect each task-only diff for inaccurate assurances, duplication, code
  changes or boundary violations; run the unchanged boundary scan after C++ edits.
- [x] After integration, a fresh spec-compliance reviewer checks coverage/scope.
  After approval, a different fresh quality reviewer checks comment accuracy.
  Root verifies findings against source before corrections; revised significant
  findings require another fresh review. No commit follows either approval.

## Task 3: Explain the conventions once in maintained documentation

**Create:** `docs/development/source-comments.md`.
**Modify:** `docs/architecture/README.md` (one guide link).
**Record:** `docs/verification/2026-09-09-source-comments.md` (local only).

- [x] Write purpose, declaration/definition placement, useful Doxygen commands,
  ownership/failure vocabulary, non-C++ conventions, boundary caveat and checklist.
- [x] Include accurate language-matching and lifetime examples plus a short
  name/type-paraphrase counterexample. Link real source, not a duplicate API index.
- [x] State that no generator/CI/HTML site was added or installed.
- [x] Record spec/plan rounds, verified fixes/rejections, per-file coverage and
  exact validation results. Check guide scope/accuracy in the final review.

## Task 4: Verify preservation and hand off

- [x] Compare all source against scratch `before/`, not HEAD. An ignored throwaway
  helper may orchestrate already-installed tools; it is not a new project
  framework/dependency. Fail closed on tool errors or unparsed lexer output.
  For C++/GLSL use the installed Clang raw lexer and compare ordered token
  kind/spelling after removing only comment and whitespace tokens:

```powershell
& 'C:/Program Files/LLVM/bin/clang.exe' -cc1 -std=c++23 -dump-raw-tokens -x c++ include/airelinna/i18n/language.hpp
```

Capture native stderr without PowerShell error-record formatting when automating
(native process API or existing Node process support). Ignore relocated token
locations, not diagnostics or other unknown tokens. Separately inspect every
directive/continuation, tool pragma and `__LINE__`/`__FILE__`/`__COUNTER__` use:
token equality alone cannot prove line-sensitive preprocessing equivalence.

- [x] Inspect each changed Lua line in full baseline/current context: only genuine
  comments outside strings may change. Preserve every operator, executable token,
  quoted/long-bracket literal and other program text. Do not strip `--` with a
  naive regex or require stripped-bytecode equality: Lua retains function line
  metadata even with `-s`. Source/debug line movement is expected for comments.
  Check syntax using `C:/lua/luac.exe -p <source-file>` separately for each of the
  three first-party Lua files; each must return 0 and emit no compiled output.
- [x] Inspect every changed CMake line in context: only genuine comments outside
  strings may change; all non-comment arguments remain byte-equivalent. Do not
  strip `#` with a regex that corrupts quoted/bracket strings.
- [x] Confirm protected hashes/index unchanged. Run `git diff --check` and inspect
  untracked task-only diffs. No whole-project formatting pass.
- [x] Build using stable tools and existing x64 VS environment, then repeat scan
  and 12 non-hardware tests:

```powershell
& $commentCmake --build build/browser-keep-alive-vs --config Release --parallel 4
& $commentCtest --test-dir build/browser-keep-alive-vs -C Release -LE hardware --output-on-failure
```

Use installed `VsDevCmd.bat` in a child shell if compiler environment setup is
needed. The tree's actual generator is Ninja/clang-cl despite its historical name.
Do not modify global PATH, repair unrelated caches, weaken gates, fetch needlessly,
run packaging targets or launch the application.

- [x] If Lua/CMake tooling changed, run the existing full 12-group harness:

```powershell
& $commentCmake -DLUA_EXECUTABLE=C:/lua/lua.exe -DTOOLING_NINJA=C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe -DRUN_LAUNCHER=ON -P tests/tooling/run_tooling_tests.cmake
```

Expected: all 12 groups pass; no new cases. Do not claim Linux/macOS execution
from Windows evidence. Do not claim Doxygen generation unless an existing usable
generator was actually run without adding dependencies.

- [x] A fresh final reviewer checks the task-only changes, guide and evidence.
  Verify/correct real findings and use fresh re-review when required. Record any
  unrelated build/tool limitation honestly; mark tasks complete only with evidence.
- [x] Report meaningful coverage, actual verification, limitations and no commit.
