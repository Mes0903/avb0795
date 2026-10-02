# Screenshot visibility

Approved by the user on 2026-09-12: controller and replica windows use the default capturable window policy. Windows window-class validation and current-process source filtering stay in their existing adapters. The controller reports CaptureExclusionStatus::NotAttempted after validation; the replica factory supplies the same initial status. Existing exclusion helper and result types remain available for explicit callers.

A full-display source includes visible replica content and can produce recursive imagery. The README and window architecture documentation explain placement on another display or selection of a source window. Scope includes targeted source, comments, documentation and existing tests. Existing worktree changes remain preserved. Tests, this spec and the plan remain local; commits require scoped authorization for this repository.

Acceptance: a hidden layered controller fixture retains WDA_NONE and reports NotAttempted, invalid HWND validation remains active, the replica startup omits its exclusion request, existing own-process filtering passes, and the native Release executable builds. Verification reads window metadata and uses controlled fixtures; desktop image capture stays outside the automated checks.
