# Source documentation work record — 2026-09-09

This local record is not a release report and is not intended for commit.

## Spec rolling review

- Artifact: `docs/superpowers/specs/2026-09-09-source-comment-contracts-design.md`.
- Round 1, fresh agent `comment_spec_review_r1`: APPROVED, Critical 0 / High 0 /
  Medium 0 / Low 0. Full spec plus representative working-tree source inspected.
- Reviewer verified ownership/drain constraints, layered responsibilities,
  comment-sensitive source gate, Lua/CMake separation, and absence of comment
  quotas. No finding required a correction or rejection.
- Root read the complete report and checked its cited source/constraint evidence.
- Reviewer stopped after reporting. No thread-deletion guarantee is claimed.

## Plan rolling review, round 1

- Fresh reviewer `comment_plan_review_r1`: Critical 0 / High 0 / Medium 1 / Low 0.
- Verified finding: Lua stripped chunks still contain function start/end line
  metadata. Root reproduced adding one leading comment with `string.dump(...,
  true)`: equality false, 103 bytes each, first changed byte 71 (1 to 2).
- Accepted the minimal fix: full contextual comment-only Lua diff, unchanged
  executable/literal text, `luac -p`, and existing tooling harness. No bytecode
  parser/framework added. Spec and plan changed together and need fresh review.
- No other inventory, layering, command, or scope finding was reported.

## Current progress

Spec round 2 approved by fresh `comment_spec_review_r2`: all severities zero;
reviewed the full revised spec and representative units; source scan returned 0.
Plan round 2 approved by fresh `comment_plan_review_r2`: all severities zero;
confirmed full A32/B44/C10/D4/E12 coverage, source contracts, raw lexer and Lua
syntax commands. No remaining significant spec or plan finding.

## Implementation baseline

- Scratch: `build/source-comments-b35174b7677e/`.
- `before/` contains all 102 current first-party source files, including untracked
  i18n/Lua files. `baseline.json` stores exact file hashes, 41 protected file
  hashes, index hash, Git status and branch/HEAD.
- Branch: `p4-application-release`; HEAD: `a955d2e110474ee339b5c3056a303eaf472b746d`.
- Index SHA-256: `7654B1B20FFDD15D19FFC43DBC26030C7EE2BF9C2684F50DE5D8604BAD79FE82`.
- Baseline is retained for verification, not automatically restored over work.
Preliminary source gate passed; Windows non-hardware tests passed 12/12. Stable
CMake build in the x64 VS child environment returned 0 (`ninja: no work to do`),
confirming the existing tree was up to date before source edits.


## Implemented coverage

All 102 sources were inspected; 91 changed and 11 retained sufficient existing
comments. Source changes are English comments only. No behavior fix was attempted.

| Batch | Inspected / changed | Contract emphasis |
|---|---|---|
| A: portable / app / i18n / UI | 32 / 32 | generation rejection, FIFO versus newest-only, deadline-grid pacing, borrowed dependencies, shutdown ownership, config recovery, requested/effective language, static text, stable UI IDs |
| B: Windows adapters / capture / runtime | 44 / 33 | identity leases, weak wake lifetime, epoch/generation, checkout versus COM view, callback and GPU retirement, readback/rollback, optional helper limits |
| C: replica renderer / shaders | 10 / 10 | capability versus selection, resource groups, independent copy/submit/present proof, rebuild barriers, retry/drain, coordinate/color assumptions |
| D: controller / composition | 4 / 4 | separate graphics owner, borrowed font bytes, startup exception cleanup, event/wake ordering, abandonment before stack unwind |
| E: Lua / CMake | 12 / 12 | pure inventory policy versus CLI/process orchestration, fixed profiles, diagnostics, metadata-only limits, generated-string safety, build target roles |

The render writer read the entire 4437-line renderer implementation. All writers
checked relevant definitions/callers and existing tests where needed; reading a
hardware fixture is not reported as executing it. Root read all D/E sources and
reviewed the integrated contracts and task-only deltas. Supplemental writer checks
found all non-full-comment lines identical for A32 and C10; compiler checks below
are the stronger combined source-token check.

Two misleading pre-existing comments were corrected without changing behavior:
the overlay geometry predicate compares exact dimensions rather than clamping;
the shader's texture scaling does not provide a half-texel padding-exclusion
guarantee. Neither is claimed here as a reproduced runtime bug.

The maintained guide is `docs/development/source-comments.md`, linked once from
`docs/architecture/README.md`. The existing localization index row was preserved.
The guide explains declaration contracts versus implementation WHY, useful
Doxygen commands, ownership/failure vocabulary, native Lua/CMake/GLSL syntax, and
validation limits using actual source examples.

### Per-file inspection ledger

Changed files inherit the concrete contract focus of their batch above. Retained
files list why the existing explanation was sufficient.

| Source | Batch | Result |
|---|---|---|
| `cmake/Dependencies.cmake` | E | Inspected / changed |
| `cmake/EmbedSpirv.cmake` | E | Inspected / changed |
| `cmake/Packaging.cmake` | E | Inspected / changed |
| `cmake/VerifyGlfwPatch.cmake` | E | Inspected / changed |
| `cmake/VerifySourceBoundaries.cmake` | E | Inspected / changed |
| `cmake/VerifyTestInventory.cmake` | E | Inspected / changed |
| `CMakeLists.txt` | E | Inspected / changed |
| `include/airelinna/app/app_command_queue.hpp` | A | Inspected / changed |
| `include/airelinna/app/application.hpp` | A | Inspected / changed |
| `include/airelinna/app/commands.hpp` | A | Inspected / changed |
| `include/airelinna/app/config_store.hpp` | A | Inspected / changed |
| `include/airelinna/app/controller_view_state.hpp` | A | Inspected / changed |
| `include/airelinna/app/localized_presentation.hpp` | A | Inspected / changed |
| `include/airelinna/app/ports.hpp` | A | Inspected / changed |
| `include/airelinna/core/application_state.hpp` | A | Inspected / changed |
| `include/airelinna/core/frame_scheduler.hpp` | A | Inspected / changed |
| `include/airelinna/core/hotkey.hpp` | A | Inspected / changed |
| `include/airelinna/core/latest_value_mailbox.hpp` | A | Inspected / changed |
| `include/airelinna/core/replica_session.hpp` | A | Inspected / changed |
| `include/airelinna/core/types.hpp` | A | Inspected / changed |
| `include/airelinna/desktop/desktop_host.hpp` | D | Inspected / changed |
| `include/airelinna/i18n/catalog.hpp` | A | Inspected / changed |
| `include/airelinna/i18n/language.hpp` | A | Inspected / changed |
| `include/airelinna/ui/controller_view.hpp` | A | Inspected / changed |
| `main.cpp` | D | Inspected / changed |
| `platform/windows/capture_exclusion.cpp` | B | Inspected / sufficient: ownership recheck, unsupported feedback, class compatibility |
| `platform/windows/capture_exclusion.hpp` | B | Inspected / changed |
| `platform/windows/controller_font.cpp` | B | Inspected / sufficient: installed-only discovery, bounded allocation, owned font bytes |
| `platform/windows/controller_font.hpp` | B | Inspected / changed |
| `platform/windows/detail/frame_intake_gate.hpp` | B | Inspected / changed |
| `platform/windows/detail/item_observation_state.hpp` | B | Inspected / changed |
| `platform/windows/detail/overlay_callback.hpp` | B | Inspected / changed |
| `platform/windows/detail/overlay_policy.cpp` | B | Inspected / sufficient: widened geometry arithmetic, resize bounds, drag anchor |
| `platform/windows/detail/overlay_policy.hpp` | B | Inspected / changed |
| `platform/windows/detail/overlay_transactions.hpp` | B | Inspected / changed |
| `platform/windows/detail/srw_lock.hpp` | B | Inspected / changed |
| `platform/windows/detail/ui_language_query.hpp` | B | Inspected / changed |
| `platform/windows/detail/wgc_capture_state.cpp` | B | Inspected / changed |
| `platform/windows/detail/wgc_capture_state.hpp` | B | Inspected / changed |
| `platform/windows/detail/windows_runtime_policy.hpp` | B | Inspected / changed |
| `platform/windows/features/browser_capture_keep_alive.cpp` | B | Inspected / changed |
| `platform/windows/features/browser_capture_keep_alive.hpp` | B | Inspected / changed |
| `platform/windows/features/detail/browser_capture_keep_alive_policy.hpp` | B | Inspected / changed |
| `platform/windows/overlay_window.cpp` | B | Inspected / sufficient: native allocation guard, readback, zero extent, single destruction |
| `platform/windows/overlay_window.hpp` | B | Inspected / changed |
| `platform/windows/process_dpi_awareness.cpp` | B | Inspected / sufficient: manifest/setter failure versus verified effective context |
| `platform/windows/process_dpi_awareness.hpp` | B | Inspected / changed |
| `platform/windows/wgc_capture_job.cpp` | B | Inspected / changed |
| `platform/windows/wgc_capture_job.hpp` | B | Inspected / changed |
| `platform/windows/win32_overlay_adapter.cpp` | B | Inspected / sufficient: subclass publication, exact rollback, geometry/DPI, NCDESTROY |
| `platform/windows/win32_overlay_adapter.hpp` | B | Inspected / changed |
| `platform/windows/windows_atomic_replace.cpp` | B | Inspected / sufficient: sibling rename versus store write/flush/cleanup |
| `platform/windows/windows_atomic_replace.hpp` | B | Inspected / changed |
| `platform/windows/windows_capture_source.cpp` | B | Inspected / sufficient: lease/raw observation, sticky cursor, close before unbind |
| `platform/windows/windows_capture_source.hpp` | B | Inspected / changed |
| `platform/windows/windows_captured_frame.cpp` | B | Inspected / sufficient: move assignment, COM view versus checkout, Close containment |
| `platform/windows/windows_captured_frame.hpp` | B | Inspected / changed |
| `platform/windows/windows_global_hotkey.cpp` | B | Inspected / changed |
| `platform/windows/windows_global_hotkey.hpp` | B | Inspected / changed |
| `platform/windows/windows_replica_runtime.cpp` | B | Inspected / changed |
| `platform/windows/windows_replica_runtime.hpp` | B | Inspected / changed |
| `platform/windows/windows_runtime_factory.cpp` | B | Inspected / sufficient: construction, attachment guard, allocation and publication |
| `platform/windows/windows_runtime_factory.hpp` | B | Inspected / changed |
| `platform/windows/windows_source_catalog.cpp` | B | Inspected / changed |
| `platform/windows/windows_source_catalog.hpp` | B | Inspected / changed |
| `platform/windows/windows_source_registry.cpp` | B | Inspected / sufficient: CAS stop/admit, final wake, preparation, unlocked revocation |
| `platform/windows/windows_source_registry.hpp` | B | Inspected / changed |
| `platform/windows/windows_ui_language.cpp` | B | Inspected / changed |
| `platform/windows/windows_ui_language.hpp` | B | Inspected / changed |
| `render/bridge_policy.cpp` | C | Inspected / changed |
| `render/bridge_policy.hpp` | C | Inspected / changed |
| `render/graphics_retry_policy.cpp` | C | Inspected / changed |
| `render/graphics_retry_policy.hpp` | C | Inspected / changed |
| `render/render_math.cpp` | C | Inspected / changed |
| `render/render_math.hpp` | C | Inspected / changed |
| `render/windows_render_stack.cpp` | C | Inspected / changed |
| `render/windows_render_stack.hpp` | C | Inspected / changed |
| `shaders/CMakeLists.txt` | E | Inspected / changed |
| `shaders/replica.frag` | C | Inspected / changed |
| `shaders/replica.vert` | C | Inspected / changed |
| `src/app/app_command_queue.cpp` | A | Inspected / changed |
| `src/app/application.cpp` | A | Inspected / changed |
| `src/app/application_language.cpp` | A | Inspected / changed |
| `src/app/config_store.cpp` | A | Inspected / changed |
| `src/app/controller_view_state.cpp` | A | Inspected / changed |
| `src/app/localized_presentation.cpp` | A | Inspected / changed |
| `src/CMakeLists.txt` | E | Inspected / changed |
| `src/core/application_state.cpp` | A | Inspected / changed |
| `src/core/frame_scheduler.cpp` | A | Inspected / changed |
| `src/core/hotkey.cpp` | A | Inspected / changed |
| `src/core/replica_session.cpp` | A | Inspected / changed |
| `src/core/types.cpp` | A | Inspected / changed |
| `src/desktop/controller_font.hpp` | D | Inspected / changed |
| `src/desktop/desktop_host.cpp` | D | Inspected / changed |
| `src/i18n/catalog.cpp` | A | Inspected / changed |
| `src/i18n/language.cpp` | A | Inspected / changed |
| `src/ui/controller_view.cpp` | A | Inspected / changed |
| `src/ui/language_selector.cpp` | A | Inspected / changed |
| `src/ui/language_selector.hpp` | A | Inspected / changed |
| `tools/lib/inventory_profiles.lua` | E | Inspected / changed |
| `tools/lib/test_inventory.lua` | E | Inspected / changed |
| `tools/verify_test_inventory.lua` | E | Inspected / changed |

## Post-edit preservation and execution evidence

- Installed Clang 22.1.6 raw lexer compared all 90 C++/GLSL files against their
  exact live baseline: 111418 ordered non-comment token kind/spelling pairs
  unchanged. The ignored `verify-comments.mjs` helper fails closed on lexer
  errors or unparsed output.
- Additional physical-line checks: 804 directive blocks, all backslash splices and
  2 clang-format pragmas unchanged; no `__LINE__`, `__FILE__`, `__COUNTER__`,
  `__BASE_FILE__`, `__INCLUDE_LEVEL__` or `source_location` usage in those sources.
- All 41 protected-file hashes and Git index SHA-256 unchanged.
- Root read every Lua/CMake source and every task-only changed line in full
  string context. All 12 files changed only genuine native comment lines;
  generated-content strings, regexes, literals and executable text stayed fixed.
  No naive comment-stripping or Lua bytecode-equality inference was used.
- `C:/lua/luac.exe -p` separately for all 3 first-party Lua files: exit 0,
  no emitted compiled output.
- Stable VS CMake source-boundary scan: exit 0. The checker itself changed only
  comments; its existing rules were not weakened.
- `git -c core.autocrlf=false diff --check`: exit 0; untracked source changes also
  covered by the working-tree baseline, not just Git's tracked diff.
- Existing Ninja/clang-cl Release build under x64 `VsDevCmd.bat`:
  62/62 steps complete, exit 0. Artifact:
  `build/browser-keep-alive-vs/src/Airelinna.exe` (1348096 bytes at verification).
- Post-build CTest `-C Release -LE hardware --output-on-failure`:
  12/12 existing non-hardware cases pass, 0 failures.
- Existing tooling harness with `RUN_LAUNCHER=ON`, explicit Lua and stable Ninja:
  `TOOLING_OK 12 groups`, exit 0. Scratch `build/lua-tooling-8611ea3dd467`.
  Repeated after the fresh Windows build: all 12 groups pass again, exit 0;
  scratch `build/lua-tooling-0de255ad6d5f`. All 3 Lua syntax checks also repeated
  with exit 0. Final source-boundary and tracked whitespace checks returned 0.
  No test cases were added or changed.

### Execution environment caveat

The first sandboxed rebuild configured successfully, then Ninja stayed idle with
no visible compiler process. Root interrupted only that own build session.
The same command outside the sandbox immediately progressed through all 62 steps.
Similarly, Node's direct Clang children returned EPERM inside the sandbox and
succeeded with approved escalation. No project configuration/source change was
used to work around either execution-environment restriction.

Stable CMake/CTest: Visual Studio's 4.3.1-msvc1 tools under
`C:/PROGRA~1/MICROS~1/18/COMMUN~1/Common7/IDE/COMMON~1/MICROS~1/CMake/CMake/bin`.
Lua/luac: 5.5.1 under `C:/lua`. Node: 22.16.0 under `C:/Program Files/nodejs`.

No application window, capture/hardware test, packaging/release operation,
Linux/macOS execution or Doxygen generation was performed. No new Doxyfile,
dependency, runtime abstraction, test framework, staging, commit or push.

## Independent implementation reviews

- Fresh `comments_impl_spec_r1_fresh`: APPROVED, Critical 0 / High 0 / Medium 0 /
  Low 0. Reviewed complete spec/plan/guide, all 91 task-only changed-file diffs,
  the 11 retained sources, and sensitive definitions/callers. Root checked the
  report against the manifest and inspected contract/source evidence. No finding
  required a correction. This reviewer did not claim to rerun root's tests.
- Fresh `comments_impl_quality_r1`: APPROVED, Critical 0 / High 0 / Medium 0 /
  Low 0. Independently confirmed the 102/91/11 inventory and read task-only
  changes, guide, relevant definitions and callers across every batch. Particular
  attention covered application precedence, exact config/language grammar,
  callback/publication and final wake, external checkouts, independent graphics
  evidence, helper retirement and controller/font lifetime. Root checked the
  report against its own source inspections and validation. No finding required
  correction. Large unchanged implementation bodies were cross-checked as needed,
  not represented as a new proof of all existing runtime behavior.
- Fresh `comments_final_review_r1`: APPROVED, Critical 0 / High 0 / Medium 0 /
  Low 0. Read complete spec/plan/guide/record and all 91 changed-source diffs;
  independently matched the live inventory and per-file ledger, baseline copies,
  41 protected hashes and index, CTest pass log and artifact size. Cross-checked
  risky ownership, callback, runtime and graphics assertions against definitions.
  Root verified the final report against its direct source and tool evidence;
  no finding required a correction or another implementation round.
- After final review, root repeated the complete raw-lexer/directive/protected/
  index check: 90 files / 111418 tokens / 804 directives / 2 pragmas unchanged,
  all 41 protected hashes and index unchanged, exit 0. Source inventory remains
  exactly 102. Source-boundary scan and whitespace check again returned 0.
- All reviews used distinct fresh agents; finished workers/reviewers were stopped.
  Available lifecycle tools interrupt execution, not guaranteed stored-thread
  deletion. No thread deletion is claimed.

## Completion

Spec and plan rolling reviews, implementation, independent spec/quality/final
reviews and preservation/build/test checks are complete. No remaining reported
Critical, High, Medium or Low finding. Changes remain in the original dirty
working tree; branch, HEAD and index are unchanged. No commit was created.
