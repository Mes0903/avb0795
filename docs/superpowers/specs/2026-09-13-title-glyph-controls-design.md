# Integrated title glyph controls

## Approved outcome

The Airelinna controller keeps its existing full-client title row and native host. Minimize and close show a centered line-drawn minus and cross against the shared title background at rest. Hover and held states retain the current ImGui Button feedback. This follows the integrated close-control appearance observed in REFramework, whose panel uses ImGui::Begin(name, &is_open).

## Responsibility and behavior

- Implement a small private helper in src/ui/controller_view.cpp using the public ImGui Button and draw-list APIs. A transparent idle Button color and zero FrameBorderSize apply within that helper. Existing hover/active colors, dimensions and stable controller_minimize/controller_close IDs remain intact.
- Draw the minus/cross from the returned item rectangle and current font size, preserving stable alignment across languages. The helper emits exactly one ImGui item so tooltip, focus and active-item checks retain their existing meaning. Restore style changes before returning.
- Retain ControllerFrame, its drag-region boundaries and its one-frame minimize request. Close continues through app::Quit. DesktopHost, Win32 adapter, Vulkan resources, resize margins, popup suppression, scrolling layout, i18n catalog and capture behavior keep their existing contracts.
- Keep this change local to the two title controls. Content buttons continue using the shared theme. The reference's menu-hide behavior maps to Airelinna's existing application Quit semantics, as approved.

## Verification and scope

Extend existing case 07 with one focused real-ImGui draw-data section: idle has glyph geometry with transparent control background, hover and held show themed feedback, and local style overrides restore the caller's colors/border. Use a distinctive palette to identify title-control vertices and bounded item regions. Reuse existing mouse action, Quit, popup, scrolling and layout checks. Keep the twelve-case inventory and avoid screenshot baselines or a new test framework.

Record runtime RED before production edits, then build the native Release product/tests, run all twelve non-hardware tests and source-boundary checks. On 2026-09-13 the user approved self-review for this follow-up after fresh reviewer creation hit the environment's agent thread limit. Spec, plan and implementation self-review stop at zero Critical/Medium. Manual visual appearance remains separately reported until a GUI check is performed.

Production scope: src/ui/controller_view.cpp, plus the usage paragraph in docs/architecture/application-ui.md. Tests, this spec, implementation plan and verification records remain local. Preserve the existing dirty checkout and previous borderless/capture work. No commit or push is included in this small follow-up.

## Reference

- https://github.com/praydog/REFramework/blob/master/src/REFramework.cpp#L1783-L1784
- https://github.com/ocornut/imgui/blob/85b2fe8486190fa9326565a2fb5fccb6caea4396/imgui_widgets.cpp#L817-L848

REFramework borrows the game's HWND and draws into its DirectX back buffer. Airelinna continues owning its standalone borderless host; this task concerns title-control presentation.
