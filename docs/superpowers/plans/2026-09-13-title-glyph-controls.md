# Integrated Title Glyph Controls Implementation Plan

> Execute inline using executing-plans. User-approved self-review replaces fresh subagents for this follow-up because the environment's thread limit blocks creation. Keep work in the existing dirty branch, preserving previous work. Tests/spec/plan remain uncommitted; this task includes no commit/push.

**Goal:** Integrate the minus/cross controls visually with the title while retaining their existing input and command contracts.

**Architecture:** One UI-private glyph enum/helper uses the public ImGui Button to own interaction, and draw-list lines to own appearance. ControllerFrame, host, Windows adapter and renderer remain unchanged.

**Tech Stack:** C++23, pinned ImGui 1.92.9, existing Catch2 case 07 and native clang-cl Release CMake build.

## Task 1: Regression, implementation and delivery

**Files:**

- Modify `src/ui/controller_view.cpp`: private title helper and two call sites.
- Extend `tests/app/controller_contract_test.cpp`: one section in case 07.
- Update `docs/architecture/application-ui.md`: one high-information paragraph about drawing/input responsibility.

- [x] Capture pre-edit copies of these three files in the existing workspace build directory; inspect current source and spec. Spec self-review confirms bounded geometry, scoped styles, stable IDs and unchanged ownership.
- [x] Add a real ImGui frame test using distinctive Button/hover/active/border colors. Scope vertex inspection to each title-control rectangle. At rest, check absence of idle background/border and presence of solid white-atlas glyph vertices. Hover and hold each control through real mouse events and check the matching themed background. Check style restoration and reuse existing action/drag/popup/scrolling tests.

The vertex predicate uses actual UI draw geometry, not implementation function names:

```cpp
vertex.pos.x >= left && vertex.pos.x <= left + button_width &&
vertex.pos.y >= margin && vertex.pos.y <= margin + button_width &&
vertex.col == expected_color
```

Glyph evidence additionally requires `vertex.uv == io.Fonts->TexUvWhitePixel` component-wise. This distinguishes vector lines from text-atlas glyphs without raster snapshots, exact vertex counts or a new rendering harness.

- [x] Build the core test target and run the new section against old production code. Record assertion failures caused by the old opaque/text controls, then continue to production edits.
- [x] Add `enum class TitleGlyph { Minimize, Close };` and `title_button(const char* id, TitleGlyph glyph, float size)` in the anonymous namespace. Add high-information Doxygen explaining the one-item contract, scoped styles and font-independent glyphs. Interaction core:

```cpp
ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
const bool pressed = ImGui::Button(id, {size, size});
ImGui::PopStyleVar();
ImGui::PopStyleColor();
```

Read item min/max after Button. Use their midpoint as glyph center; use `max(1.0F, GetFontSize() * 0.35F)` as half extent and `max(1.0F, GetFontSize() / 18.0F)` as stroke thickness. Use GetColorU32(ImGuiCol_Text), one horizontal AddLine for Minimize and two diagonal AddLine calls for Close. Return the original pressed value. Keep all declarations private and public ImGui API-only.

- [x] Replace the two calls with `title_button("###controller_minimize", TitleGlyph::Minimize, button_width)` and `title_button("###controller_close", TitleGlyph::Close, button_width)`. Retain the existing result/emit_if, tooltips and geometry assignments.
- [x] Run the new section and existing case 07, then the product, all existing non-hardware tests and source boundary verification. Build/run commands from Airelinna root:

```powershell
cmd /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build/p4-release --target Airelinna airelinna_core_tests verify_source_boundaries'
& ./build/p4-release/tests/airelinna_core_tests.exe '*case 07*' --section 'title glyphs share the title background and retain themed feedback'
ctest --test-dir build/p4-release --output-on-failure -LE hardware
```

- [x] Update the architecture paragraph using purpose/requirement/implementation order. Self-review the three scoped diffs against the spec and plan for Critical/Medium defects, especially input identity, style leakage, clipping, glyph geometry and responsibility boundaries. Verify whitespace and rebuilt artifact. Report automated evidence separately from manual visual inspection.

## Plan review

2026-09-13 self-review: the change uses existing UI behavior and the existing twelve-case inventory. All native operation owners remain outside the edited files. No Critical/Medium issue identified before implementation. Manual GUI appearance and mixed-DPI interaction remain explicit acceptance limits.

## Completion evidence

- Spec, plan and scoped implementation self-review completed under the user's 2026-09-13 exception: 0 Critical / 0 Medium. No fresh-subagent review is claimed.
- Production diff: 37-line private helper plus two call-site replacements in controller_view.cpp. Public interfaces, host/native/renderer sources and command routes are unchanged. Title IDs retain their existing ### suffixes. Helper creates one Button item and restores its two style overrides before drawing glyphs.
- Existing case 07 gains one 15-assertion draw-data section. Fixture disables optional textured line anti-aliasing so white-atlas UV checks identify vector geometry consistently. RED against old production: 6 expected failures (idle fill, border, vector glyph for each of two controls); GREEN: all 15 pass. Existing input/layout tests remain in the same case.
- Release product/core-test build and verify_source_boundaries: exit 0. CTest non-hardware suite: 12/12 pass, zero failed cases.
- Rebuilt artifact: build/p4-release/src/Airelinna.exe, 1,352,192 bytes, verified 2026-09-13.
- Scoped baseline-to-working-file whitespace checks produced zero diagnostics. Git index remains empty; all changes stay uncommitted.
- Architecture documentation updated under the zhTW writing guidance. `C:/lua/lua.exe -E C:/Users/Mes/.codex/skills/zhTW-translocalizer-skills/scripts/lint.lua --profile final --format markdown docs/architecture/application-ui.md` reports 3 warnings, all at unchanged baseline lines 106/189/222; zero new warnings.
- Manual GUI appearance and physical/mixed-DPI interaction were not exercised. The tests check generated draw geometry and queued input rather than screen pixels. User can relaunch the rebuilt executable to inspect the integrated title controls.
