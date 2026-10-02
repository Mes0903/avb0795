# High-density source documentation design

Status: Revised validation contract approved by fresh reviewer
`comment_spec_review_r2` on 2026-09-09: Critical 0 / High 0 / Medium 0 / Low 0.
The prior Lua stripped-chunk false-failure requirement has been removed.

## 1. Intent and authorization

Document the current Airelinna working tree so a reader can understand a unit's
purpose, correct use, important effects, and design constraints from its comments
without first reconstructing its implementation. This is a documentation change,
not an architecture refactor or a behavior fix.

The user requests detailed Doxygen-style C++ comments, explanations of why, and
high information density rather than line-by-line narration or a repeated list of
every parameter. They additionally authorize spec review, then plan review, then
implementation without another approval pause. Each review round uses a new
independent agent and continues until no verified critical/high/medium issue
remains. Findings must be checked against the source before acceptance.

No staging, commit, push, release operation, dependency installation, or permanent
machine configuration change is authorized. Existing uncommitted work belongs to
the user; the baseline is the current working tree, not HEAD.

## 2. Scope

Inspect all first-party production C++ under `include/`, `src/`,
`platform/windows/`, `render/`, and `main.cpp`. Add or improve documentation where
the existing comments do not explain a meaningful contract or a non-obvious
implementation decision. Existing accurate, sufficient comments remain valid;
there is no quota of lines or requirement to expand trivial accessors.

Also inspect first-party `shaders/`, `tools/` excluding `tools/vendor/`, root and
subdirectory production `CMakeLists.txt`, and `cmake/`. Use their native comment
syntax with the same intent/contract discipline. They are supporting developer
code, not C++ APIs. Document the Lua tool's test-inventory role without implying it
builds the application, runs tests, or proves release readiness.

Tests are read-only evidence for existing contracts in this pass. Their helpers
and cases are not expanded or mechanically annotated. Do not edit third-party
code, vendored Lua, patches, generated shader headers, build output, JSON data,
resources, persisted user preferences, or publish documents. Scratch validation
artifacts may be created in a uniquely named ignored `build/` subdirectory.

Create one maintained developer guide at
`docs/development/source-comments.md` explaining the conventions and examples;
link it from the architecture index. Spec, plan, and review records remain local
work records. No Doxygen dependency, Doxyfile, documentation generation target,
new abstraction, or new product test is required.

## 3. Approach and alternatives

Use declaration-centered contracts plus selective implementation rationale.
Compared with documenting declarations alone, this also explains shutdown
barriers, callback handshakes, rollback, and graphics retirement. Compared with
annotating every statement or copying the same API text above each definition,
it avoids maintenance duplication and preserves the visibility of actual code.

Comments describe the implementation that exists, not an idealized future design.
If reading reveals a possible behavior bug, report it separately; do not silently
repair behavior or write an assurance the code does not provide.

## 4. C++ documentation contract

Use English for source comments, consistent with the current source. The guide
may use Traditional Chinese with English symbols. Do not introduce Simplified
Chinese or duplicate every paragraph in two languages.

Use `/// @file` followed by a separate `/// @brief` for a file-level overview;
the argument to `@file` must not accidentally become prose. Use contiguous `///`
blocks with `@brief` for substantive public types/functions. Existing correct
Doxygen forms can remain when reformatting would add no information. Keep normal
`//` comments for internal algorithm rationale and namespace/guard markers.

For a meaningful interface, explain the relevant subset of:

- responsibility and what remains the caller's or another layer's responsibility;
- inputs in semantic terms: identity, units, admissible states, generation, size;
- ownership transfer versus borrowing, view lifetime, and destruction conditions;
- thread affinity, synchronization boundary, and callback reentrancy when present;
- results and observable effects, including consumption, coalescing, or persistence;
- failure behavior: `std::expected` errors, containment, partial mutation, rollback,
  or fail-fast conditions supported by the code;
- reason for non-obvious policy and what the guarantee explicitly does not prove.

Use `@pre`, `@post`, `@return`, `@throws`, `@note`, or `@warning` only when their
content adds a concrete contract. Do not list obvious `@param` entries. Document a
parameter when its semantic domain, lifetime, or side effect is otherwise unclear.
Do not repeat deleted copy/move members, default destructors, obvious equality,
enum spellings, or every data member with name/type paraphrases. Document value
groups where their relationship matters.

Public contracts belong at declarations; definitions explain mechanisms and why
an ordering or trade-off exists. Internal-only types/helpers can carry Doxygen
contracts near their definitions. Avoid duplicating whole paragraphs across files.

Modern C++ intent means explaining RAII, move-only ownership, value snapshots,
borrowed references, and typed failure where those mechanisms already exist. It
does not authorize introducing attributes, qualifiers, interfaces, or refactors.
In particular, `noexcept` does not itself prove success, nonblocking execution,
allocation freedom, safe destruction, or thread safety.

## 5. Architectural topics to make legible

| Area | Reader should be able to establish |
|---|---|
| Core | Value/state invariants; transition rejection; scheduler pacing and mailbox replacement; no platform or localization ownership |
| Application | Ordered commands, synchronous runtime facts, lifecycle precedence, retained owner/drain rules, typed config and language policy |
| UI / localization | Immutable presentation input and emitted command boundary; borrowed catalog text, fallback policy, stable widget identity |
| Native capture | Source identity/leases, generation/epoch checks, callback admission and quiescence, checkout ownership and source loss |
| Native runtime / overlay | Publication and rollback, stop/recovery ordering, window interaction policy and isolated optional browser helper |
| Renderer / shaders | Separate copy, submit, and presentation completion evidence; CPU/GPU transport, bounded slots, resource retirement, coordinate/color assumptions |
| Desktop / composition | Controller-only graphics ownership, event-loop wake/deadline behavior, font lifetime, construction/destruction order |
| Developer tooling | CMake orchestration versus Lua pure inventory policy, process results and input limits, build boundary/patch/shader helpers |

The existing source boundary checker scans comment text too. Comments must not
introduce prohibited native type names or forbidden owner/include names into a
portable layer. Explain those exclusions using layer-level terms rather than
weakening the boundary gate or bypassing it. Do not publish private window titles,
local user settings, or environment-specific secrets in comments.

## 6. Validation and acceptance

Before source edits, capture scoped source bytes and Git index state in an ignored
scratch baseline. Include untracked first-party production sources, not just Git
diffs. Preserve all pre-existing work. Each implementation batch records the files
read, changed, and intentionally left unchanged with sufficient comments.

Acceptance requires:

1. All in-scope modules have been inspected; public boundaries and non-obvious
   state/lifetime mechanisms have substantive, source-accurate explanations.
2. No code token, literal, directive, signature, build logic, test body, or
   vendored byte changes attributable to this task. Verify against the live
   pre-edit baseline, not against the old commit. For C++/shaders, compare lexical
   non-comment tokens with the installed compiler; separately review directive
   layout and line-sensitive constructs. For Lua and CMake, inspect each task-only
   delta in full context to confirm only genuine comments outside strings changed;
   preserve all executable text, quoted/long-bracket literals, and operators.
   Run Lua syntax checks and the existing tooling harness. Do not require Lua
   stripped-chunk byte equality: function line metadata survives stripping.
3. Source-boundary scan passes unchanged. Applicable existing Windows build and
   non-hardware tests pass; Lua syntax/policy/launcher checks pass if those files
   are edited. No manual capture or hardware tests are needed to validate comments.
4. Changes receive an independent accuracy/scope review; verified significant
   findings are fixed, with another fresh reviewer when re-review is required.
5. No new tests/framework/toolchain or unrequested repository mutations. Report
   actual commands/results and any unverified environment honestly.

Token comparison and contextual diff review check accidental code edits; they do
not prove prose is true or replace accuracy review. Source locations and debug
line metadata may naturally shift; no behavioral contract relies on them here.
Doxygen generation is not claimed if no generator
is installed or run. Native build availability is assessed from the live cache;
do not repair unrelated stale build configurations to make this task look green.

## 7. Review lifecycle

Spec and implementation plan each receive a full independent review of their own
artifact, checked against this user request and current code. Root verifies every
finding, records accepted/rejected reasoning, and revises the artifact as needed.
A fresh reviewer reviews each revised round, until zero critical/high/medium.
Low findings are either cheaply corrected or explicitly recorded, never hidden.

Stop old reviewers after their response using the available lifecycle tool before
starting their replacement. If this environment lacks a thread-delete/close
operation, report that limitation accurately; do not claim that interrupting a
completed agent deletes its thread. Keep concurrency bounded and avoid reusing an
old reviewer for a new rolling-review round.
