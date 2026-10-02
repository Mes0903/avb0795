# avb0795 1.0.0

avb0795 V1 is a Windows 11 desktop utility that displays one explicitly
selected display or window in a movable always-on-top replica window.

## Supported-hardware floor

Both the controller presentation device and the replica presentation device
must expose present fences through `VK_KHR_swapchain_maintenance1` or
`VK_EXT_swapchain_maintenance1`. Startup or runtime construction reports a
typed unsupported-graphics error when this requirement is unavailable.

CPU-copy fallback applies only to the D3D-to-Vulkan frame bridge. It does not
remove the present-fence requirement from either Vulkan presentation owner.

## Runtime requirements

The ZIP does not bundle system DLLs. It relies on Windows 11 system components,
an installed Vulkan 1.3 loader/GPU driver, the Universal C Runtime, and the
Microsoft Visual C++ runtime (`MSVCP140.dll`, `VCRUNTIME140.dll`, and
`VCRUNTIME140_1.dll`). GLFW and Dear ImGui are statically linked.

## V1 scope and limitations

- Windows 11 only; one active replica session.
- SDR rendering only. Detected HDR and unknown color status are shown as
  distinct warnings rather than silently claiming HDR support.
- Capture starts only after source selection and **Start**.
- Controller and replica capture exclusion is best effort and is visibly
  reported when unsupported or failed.
- The portable ZIP has no installer, updater, telemetry, analytics, or signing
  service.

See `SECURITY.md` for local data and privacy behavior and
`THIRD_PARTY_NOTICES.md` for pinned dependency versions and licenses.
