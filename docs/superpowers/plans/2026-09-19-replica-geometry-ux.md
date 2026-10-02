# Replica geometry UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Each task uses fresh spec and quality review; root verifies findings.

**Goal:** 提供可調來源欄寬、Editable 滾輪縮放與可保存的 5% 預設步幅；初始尺寸問題保留待重現紀錄。

**Architecture:** ControllerView 送 typed command；Application 驗證、套用、提交 accepted preferences；ConfigStore 保存。Windows adapter 查詢原生幾何，純值政策計算 resize，adapter 執行 SetWindowPos；現有 runtime／renderer 處理 framebuffer 更新。

**Tech Stack:** C++23、Dear ImGui 1.92.9、GLFW／Win32、JSON ConfigStore、現有 Catch2 15-case inventory。

---

## 0. Scope、工作樹與驗證

- [x] Spec `../specs/2026-09-19-replica-geometry-ux-design.md` 經兩次全新 reviewer；目前 scope review 0 Critical／0 Medium。
- [x] 使用者要求等待真實小副本重現，紀錄位於 `other-issue.md`；初始尺寸 production 行為維持原樣。
- [x] 使用者允許啟動應用程式與測試視窗；滑鼠鍵盤輸入與主動焦點切換留給使用者。測試使用 headless ImGui、純值 policies、hidden test-owned native windows。
- [x] 在 `D:/document/MesRepo/Airelinna`、branch `p4-application-release` 原地實作。每個 task 保存 touched-file 的 before snapshot，保留全部既有 dirty edits。Index 僅加入本 task production/docs hunks；tests/spec/plan/issues/artifacts/verification 維持 unstaged。
- [x] 每次提交前 `git diff --cached --check`、allowlist、`git write-tree`；checkout-index 到新 artifact source，tests OFF、既有 pinned dependency overrides，獨立 build `avb0795 verify_source_boundaries`。提交採小階段，沿 task 次序。

共用驗證命令（PowerShell／x64 developer shell；compiler clang-cl、NMake、Release）：

```powershell
cmd /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build/p4-release --target avb0795 avb0795_core_tests avb0795_windows_policy_tests verify_source_boundaries'
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir build/p4-release -LE hardware -E 'case 11:' --output-on-failure
& ./build/p4-release/tests/avb0795_windows_policy_tests.exe 'avb0795 V1 case 11:*' -c 'DPI band and aspect sizing preserve opposite anchors'
& ./build/p4-release/tests/avb0795_windows_policy_tests.exe 'avb0795 V1 case 11:*' -c 'border geometry converts content without owning decoration bits'
& ./build/p4-release/tests/avb0795_windows_policy_tests.exe 'avb0795 V1 case 11:*' -c 'settings transaction restores every failed stage'
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' -DBUILD_DIRECTORY=D:/document/MesRepo/Airelinna/build/p4-release -DLUA_EXECUTABLE=C:/lua/lua.exe -DINVENTORY_PROFILE=windows-v1-full -P cmake/VerifyTestInventory.cmake
```

預期 build／boundary／inventory exit0；CTest 的安全子集為 11/11，case11 額外依上述三個 SECTION allowlist 執行。現有 case11 的兩組 `native region` SECTION 包含 SetCursor／SetCapture／focus 操作，保留為使用者桌面驗收。新增 wheel 的兩個 SECTION 固定命名為 `wheel geometry preserves aspect within work area` 與 `native hidden wheel changes editable geometry`，root 審核 callback 路徑後以同一 `-c` 方式分別執行。Native wheel test 使用 hidden HWND、WM_MOUSEWHEEL 與 SWP_NOACTIVATE；框選阻擋測試採 set_region_selection(true) 和 wheel，保持 pointer gesture idle。所有下文的非互動 regression 指本段安全子集，完整15-case inventory 照常查核。Hardware／真實桌面操作分別記錄。

## Task 1. Typed wheel preference 與 config

2026-09-19 完成，commit `f2b591f`。RED／GREEN、安全 11/11、inventory、boundary、精確 staged snapshot build 通過；fresh spec 與 quality review 各 0/0/0。逐項證據見 `docs/verification/2026-09-19-replica-geometry-ux.md`。

**Files:** `include/avb/core/{types,replica_session,application_state}.hpp`、`src/core/{replica_session,application_state}.cpp`、`include/avb/app/{commands,application,config_store,controller_view_state}.hpp`、`src/app/{application,config_store,controller_view_state}.cpp`。Tests：現有 `tests/app/application_contract_test.cpp`、`tests/app/config_store_test.cpp`，沿既有 owner fixture 擴充。

- [x] 新測試先驗證完整行為：default5、設定12後 view/config 相同、Start 將12交給 runtime、apply failure 保留舊值、stale token／0／26 拒絕；config 合法 round-trip、舊欄位缺省／字串／0／26／過大 JSON integer 局部回復5。約兩組 SECTION；先以缺 API 得到 compiler feedback，再最小宣告／空處理取得真實 assertion RED，保存 logs。

```cpp
inline constexpr int default_wheel_zoom_percent = 5;
[[nodiscard]] constexpr bool valid_wheel_zoom_percent(int value) noexcept {
  return value >= 1 && value <= 25;
}
// ReplicaSettings and UserConfig:
int wheel_zoom_percent{core::default_wheel_zoom_percent};
// Within namespace core omit the core:: qualifier.
struct SetWheelZoomPercent { core::SessionId session_id; int percent{5}; };
```

- [x] 沿用 session setter 與 ApplicationState generation gate，加入 `set_wheel_zoom_percent`。`AppCommand` variant 增加 command，Application handler 依下列順序接線，settings admission 沿現有 `settings_allowed()`，範圍驗證使用共用 constexpr 函式。

```cpp
// After session/admission/range validation:
auto candidate = state_.current_session().settings();
candidate.wheel_zoom_percent = command.percent;
if (!apply_settings(candidate, now)) return;
const auto updated = state_.set_wheel_zoom_percent(command.session_id, command.percent);
if (!updated) { reject(updated.error()); return; }
config_.wheel_zoom_percent = command.percent;
config_dirty_ = true;
clear_notice(NoticeOperation::Command);
persist_config();
```

- [x] Initialize 與 pending replacement 的完整 settings clone 接線；`ControllerControls::wheel_zoom` 沿 settings availability 投影。Existing accepted value 重送可早退並重試既有 config dirty 保存。
- [x] Config V1 exact-key 集合加 optional `wheel_zoom_percent`。整數解析先驗證 JSON number type／寬範圍，再轉 int；回復5保留其他欄位。Serialize 寫入此欄位；save validation 在 I/O 前拒絕越界。
- [x] Doxygen 說明 accepted preference、local default recovery 與 runtime apply 前後的責任。跑對應 case GREEN、第0節安全regression子集、boundary、inventory。
- [x] Fresh spec reviewer→root驗證→fresh quality reviewer，修正後每輪使用新 agent。0/0 後精確 staged snapshot，commit `feat(settings): persist replica wheel zoom increment`。

## Task 2. 原生 wheel input 與有界比例幾何

2026-09-19 完成，commit `522c326`。幾何／native／既有安全 SECTION、boundary、inventory、精確 snapshot 通過。Quality r1 的小工作區測試假設已確認並修正；全新 r2 review 0 Critical／0 Medium。初始擷取尺寸維持既有行為。

**Files:** `platform/windows/detail/overlay_policy.hpp/.cpp`、`platform/windows/win32_overlay_adapter.hpp/.cpp`、`platform/windows/windows_runtime_factory.cpp`、`platform/windows/windows_replica_runtime.cpp`。Tests：`tests/windows/overlay_policy_test.cpp`，約兩個 SECTION。

- [x] RED tests：5% 一格放大／縮小、25% 多格、120 分割為30+30+60、source/crop aspect、有框insets、DPI、負座標monitor、minimum與work-area上限、invalid rect／extent／step／overflow；native hidden fixture 驗證 Editable變更、PassThrough／selection保持原樣、mode／step／selection reset remainder。保持既有 case inventory。
- [x] 幾何只新增一個純值函式，使用現有 `NativeRect`、`FrameInsets`：

```cpp
[[nodiscard]] std::optional<NativeRect> wheel_resized_rect(
  NativeRect client_screen, core::Extent source, FrameInsets insets,
  NativeRect work_area, std::uint32_t dpi, int notches, int step_percent) noexcept;
```

輸入先檢查正值 source／rect、非負 insets、有效 step、正 DPI，以及 SetWindowPos 的 int dimensions 可表示性。`notches=0` 由 adapter 保留 remainder，略過 native resize。尺寸計算使用 widened/double 中間值，轉整數前限制範圍：

```text
ratio = source.width / source.height
available = work_area extent - frame insets
upper_width = min(available.width, available.height * ratio)
lower_width = min(upper_width, max(160*dpi/96, 90*dpi/96*ratio))
w = clamp(current_client_width * pow(1 + step_percent/100, notches), lower_width, upper_width)
h = w / ratio
```

Round 至可表示的正整數且保持 available bounds；極端比例採至少1pixel的一軸，比例誤差依pixel precision。以原client中心定位，加入insets後平移到work area內。輸出invalid為nullopt。檢查大notches的pow結果與尺寸上下界，bounded saturation具有相同方向語意。
- [x] Adapter 加 `wheel_zoom_percent_{5}`、`wheel_remainder_{}` 與 `set_wheel_zoom_percent(int)`。Setter由已驗證settings呼叫，值變更清remainder。Mode改變與開始selection亦清除；native geometry resize本身保持mode/border/opacity。
- [x] `prepare_message(WM_MOUSEWHEEL)` 在Editable／一般預覽／游標位於client時查詢geometry、insets、workarea；使用 `GET_WHEEL_DELTA_WPARAM`。累積數值由bounded remainder加signed16-bit delta，算notches與remainder；prepared message 保存候選rect/remainder。Fractional-only結果在commit保存remainder；完整notch成功SetWindowPos後保存remainder。失敗保持原值；callback firewall沿現有政策。

```cpp
const int total = wheel_remainder_ + GET_WHEEL_DELTA_WPARAM(word_parameter);
const int notches = total / WHEEL_DELTA;
const int remainder = total % WHEEL_DELTA;
// commit: SetWindowPos(window_, nullptr, left, top, width, height,
//                     SWP_NOACTIVATE | SWP_NOZORDER)
```

- [x] Factory 在adapter建立後、native publication前安裝validated step；runtime `apply` 加範圍check，SettingsOperations 的 `commit_scheduler()` 在fallible native transaction已驗證後安裝step。建構與recovery沿既有settings保存。
- [x] Native failure若可由disabled／invalid自身fixture穩定引發，加入精確assertion；以既有 transaction style 把 remainder commit gating 做成可測的小型 policy 亦可，維持單一幾何 helper及一個native callback分支。
- [x] GREEN／第0節安全regression子集／boundary；fresh spec+quality rolling0/0，root核對 lifetime、framebuffer自動resize與crop比例。精確staged snapshot後commit `feat(windows): resize editable replicas with the mouse wheel`。

## Task 3. ImGui 欄界與縮放步幅控制

2026-09-19 完成，commit `d21d299`。兩組 headless SECTION、完整 build、root 最終安全 11/11 與五組原生 allowlist、inventory、boundary、精確 snapshot 通過；fresh spec／quality 各 0 Critical／0 Medium／0 Low。操作與責任文件已隨本階段提交。

文件交付補充：現有 `replica-controls.md` 為既有 untracked 文件，`windows-runtime-overlay.md` 已有較大 dirty rewrite。本階段另以 `docs/development/replica-wheel-zoom.md` 提交精簡且完整的操作／責任文件；原有文件保留工作樹並加入交叉入口，避免夾帶既有改動。

**Files:** `src/ui/controller_view.cpp`、`include/avb/ui/controller_view.hpp`、`include/avb/i18n/catalog.hpp`、`src/i18n/catalog.cpp`、`docs/development/replica-controls.md`、`docs/architecture/windows-runtime-overlay.md`。Tests：`tests/app/controller_contract_test.cpp` 約兩個SECTION。

- [x] RED：headless ImGui 以 `io.AddMousePosEvent`／`AddMouseButtonEvent` 拖兩個table欄界，後frame寬度保留，sink維持空；slider改draft期間sink空，手勢結束送一次當session設定command；Ctrl-click numeric、hidden-input synchronization後完成、session變更／disabled取消draft。這些事件只進入測試的ImGui context。
- [x] Source table flags 加Resizable，既有三欄Fixed／Stretch／Fixed與顏色literaltext保持原樣。

```cpp
constexpr auto table_flags = ImGuiTableFlags_RowBg |
  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersH |
  ImGuiTableFlags_Resizable;
```

- [x] 增加TextId與English／繁體中文／Japanese對應label／help；`SliderInt`範圍1..25，格式`"%d%%"`，AlwaysClamp；scoped `LiveEditOnInputScalar`讓numeric draft沿既有ImGui patch保存。
- [x] 新增小型WheelZoomEditing `{SessionId,int percent,bool changed}` draft，綁session與controls；`has_active_interaction()`涵蓋draft。渲染時使用draft值，手勢結束時一次emit command。沿opacity現有跨hidden-frame completion做法，以當前widget active與draft obligation判定，避免依賴單frame的deactivationflag。UI重啟與下一手勢讀accepted view值。
- [x] 文件以中文目的→需求→結構說明欄界、Ctrl-click、5%預設／1..25%、正負倍率、Editable與框選行為、config。新增Doxygen描述draft與nativeowner責任，保持資訊密度。
- [x] GREEN／第0節安全regression子集／inventory／boundary、production完整build；fresh spec+quality rolling0/0，精確snapshot、commit `feat(ui): expose wheel zoom settings and resizable source columns`。實際滑鼠鍵盤驗收交由使用者，本輪列出可直接執行的Release artifact與測試步驟。

## Spec coverage 與測試預算

| Requirement | Task | Evidence |
|---|---|---|
| 三欄可拖曳 | 3 | headless兩個欄界實際輸入／寬度 |
| 小副本待重現紀錄 | 0 | other-issue.md／WireGuard唯讀數值 |
| Editable wheel／border／crop | 2 | 純值與hidden native fixture |
| 預設5、UI可調、config保存 | 1、3 | roundtrip／typedadmission／gesture |
| 分層與簡潔 | 全部 | boundary／freshrolling／沿用15cases |
| 避免搶滑鼠鍵盤 | 全部 | headless／hidden／直接native message |

約六個必要SECTION，表格資料各自對應已列出的production invariant。Spec／plan／tests／問題紀錄保存於工作樹；三筆production commit分段可建置，最後完整UX驗收。
