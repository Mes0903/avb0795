# Production 基線分階段驗證

日期：2026-09-16。分支：`p4-application-release`。起始 HEAD：`a955d2e`。

使用者已授權將既有 production 變更整理成可建置的小階段，接續提交 Vulkan performance 工作。Git index 每次保存該階段的完整內容，工作樹保留現行功能。Tests、spec、plan、review records、個人設定與 benchmark artifacts 保留於本機。

## 已驗證階段

| 階段 | Commit／候選 tree | 獨立 snapshot | Review |
| --- | --- | --- | --- |
| Product／namespace 改名 | `206cd2c`；tree `fd775527c03943667ae899e2fa31c5727c902adb` | `artifacts/vulkan-perf/baseline-rename-20260916/` | `rename_and_wgc_fixture_review_1`：0 Critical／0 Medium |
| Core 裁切／透明度值契約 | `282414e`；tree `9763247737bfc8d850c920914225240b9b6cd5fe` | `artifacts/vulkan-perf/baseline-core-20260916/` | `baseline_core_review_1`：0 Critical／0 Medium |
| Renderer 幾何／shader／presentation | `5cea211`；tree `423740cef65bb05335f7c593d308989b76414a57` | `artifacts/vulkan-perf/baseline-render-20260916/` | `baseline_renderer_review_1`：0 Critical／0 Medium |
| Native opacity／region selection | `31864bc`；tree `da5bfc811d2230680a83a21180dabc006d45a9cd` | `artifacts/vulkan-perf/baseline-native-r2-20260916/` | `baseline_native_review_2`：0 Critical／0 Medium |
| Portable i18n foundation | `c29d221`；tree `8581e07b9f3d64c52712ed873f4d770e974170a7` | `artifacts/vulkan-perf/baseline-i18n-20260916/` | `baseline_i18n_review_1`：0 Critical／0 Medium |

上述五個 snapshot 均以自身匯出的 first-party sources 獨立 configure，設定 Release、clang-cl 22.1.6、C++23、NMake、`AVB0795_BUILD_TESTS=OFF`。`avb0795` 與 `verify_source_boundaries` targets 皆成功建置；i18n 階段另明確建置 `avb0795_i18n`。Pinned dependencies 使用既有 offline sources，ImGui／GLFW patch checks 同步通過。

Native 第一版 snapshot 的建置與 reviewer 共同發現 `admit_retained_redraw()` 所屬 policy header 缺漏；第二版納入該相依後通過獨立建置與全新完整 review。

Core 階段保留原本 `AppError::user_message()` 的宣告與定義，以維持尚待遷移的 Application 相容性。Localization integration 階段接續移轉使用者文字的責任。

## 已準備的後續 snapshots

為了讓正式效能量測維持穩定，這些候選先進行 source-only review，量測結束後逐一建置、驗證及提交。Preferences 使用真實 index；其後候選使用各 artifact 目錄的獨立 `candidate.index`，production working files 保留原樣。

| 階段 | 完整候選 tree | Snapshot 根目錄 | Source review／build |
| --- | --- | --- | --- |
| Preferences／localized projection | `c2f7bbc4a315af3b9ca28a8a71e955c8715fd8b5` | `baseline-preferences-20260916` | `baseline_preferences_review_1` 0/0；build 通過；commit `31a5785` |
| Native helpers／capture policy | `9ea562a3e16415f1566c0bf856c4082c07cf221f` | `baseline-native-helpers-20260916` | `baseline_native_helpers_review_1` 0/0；build 通過；commit `abadaa1` |
| Application／commands／settings | `3288f22a8d638558421d936183aa2ff6d9d7294c` | `baseline-application-20260916` | `baseline_application_review_1` 0/0；build 通過；commit `3b49669` |
| Controller UI／host／hotkey wiring | `1f70b6004fc9d6222f48052504172723187401e9` | `baseline-controller-20260916` | `baseline_controller_review_1` 0/0；build 通過；commit `2f64894` |
| Task 1 collector foundation | `f0eeebb87dff9b66e068a3bff972c6c4e7e570fb` | `diagnostics-foundation-20260916` | `task1_frozen_full_review_1` 0/0；build 通過；commit `a15baf6` |
| Task 1 observations／CLI wiring | `c092b0748facdf3956135519bcb13dd82a6d9917` | `diagnostics-wiring-20260916` | `task1_frozen_full_review_1` 0/0；build 通過；commit `10f0df9` |

Snapshot 根目錄皆位於 `artifacts/vulkan-perf/`。每個 source-only reviewer 都核對該階段完整 diff、相依呼叫端與 exported blobs。六個候選的獨立 configure、`avb0795` 與 source boundaries targets 均通過，diagnostics 兩階段另外明確建置 `avb0795_diagnostics`。各次 commit 前重新核對完整 tree hash、staged allowlist 與 `git diff --cached --check`；提交後的 tree 與候選一致。各 snapshot 的 `build.log` 保存建置結果。

## Pre-diagnostics 的精準復原

為了將既有功能與 Task 1 觀測分成各自的 commits，依原始 Task 1 patch 記錄在 artifact copies 反向還原十一個檔案。Root 獨立核對每個副本，11/11 SHA256 與 `source-before-task1.json` 相符。

- 副本：`artifacts/vulkan-perf/task1-20260916-4eec1ed5/pre-diagnostics-source/`。
- 雜湊清單：同層 `pre-diagnostics-reconstruction.json`。
- 現行 production working files 維持原內容，包含已驗證的效能觀測實作。

後續依 renderer、native adapter/runtime、localization/controller/preferences、packaging/tooling 與文件分階段驗證及提交。
