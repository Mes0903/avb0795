# Opacity and source region implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 提供 replica 即時不透明度、直接框選、來源像素微調與全畫面還原。

**Architecture:** ImGui 送出值型 AppCommand，Application 驗證並接受 ReplicaSettings，Windows runtime 組合 native input/alpha 與 renderer region presentation。核心資料與數學保持 portable，所有 GPU/Win32 操作保留在具體 owner。

**Tech Stack:** C++23、Catch2、ImGui DragInt、GLFW/Win32、WGC、Vulkan、既有 CMake/Lua tooling。

**Spec:** `docs/superpowers/specs/2026-09-15-opacity-region-design.md`。

## 2026-09-15 執行進度

Tasks 1–4 的功能、文件與自動驗證已完成；最終全新 reviewer 回報 0 Critical / 0 Medium。Release build、非硬體 CTest 12/12、source boundaries、15-case inventory 與自有視窗 graphics hardware cases 14/15 均通過。

完整結果與 review 查證見 `docs/verification/2026-09-15-opacity-region.md`。各 task 的 scoped commit 等待既有 dirty rename／UX 的提交整理選擇；index 維持空白。人工視覺與跨 DPI 操作保留於驗收清單。

實作細節收斂：presentation setter、record 與 enqueue 由同一 main-thread owner 串行執行，dirty 在成功 enqueue 清除，後續 setter 重新標示；因此採用現有值與 dirty 狀態追蹤。退休 snapshot 的 sequence 專門處理畫面幾何與晚到 present，維持兩者明確職責。

## 工作樹與驗證環境

實際專案路徑為 `D:/document/MesRepo/Airelinna`，branch 為 `p4-application-release`，基準 HEAD 為 `a955d2e`。avb 改名等既有修改保持原狀。開始實作前保存 touched-file baseline 至 build 下獨立備份，以 task delta 做 review。Git 提交範圍依使用者對既有 dirty baseline 的選擇執行；tests、spec、plan 排除。

使用既有 `build/p4-release`，Visual Studio 18 Community x64 developer shell 與 VS 內附 stable CMake。以下各 Task 的紅綠驗證使用同一組命令：

```powershell
cmd /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build/p4-release --target avb0795 avb0795_core_tests avb0795_windows_policy_tests'
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir build/p4-release -LE hardware --output-on-failure
```

紅燈需指向新增行為缺少或舊值；修正後既有群組綠燈。新增約 10–20 個針對行為的 SECTION，沿用既有 test case inventory。

## 共用介面定案

新增資料全部具值語意；合併設定走既有 `IReplicaRuntime::apply`，維持單一交易入口。

```cpp
// include/avb/core/types.hpp
struct PixelRegion {
  std::uint32_t x{}, y{}, width{}, height{};
  friend bool operator==(const PixelRegion&, const PixelRegion&) = default;
};
// ReplicaSettings appended fields:
int opacity_percent{100};
std::optional<PixelRegion> region{};
bool selecting_region{}; // main-thread transient presentation request, config excludes it

// include/avb/app/commands.hpp
struct SetOpacity { core::SessionId session_id; int percent{100}; bool commit{}; };
struct SetRegion { core::SessionId session_id; core::SourceId source_id; std::optional<core::PixelRegion> region; };
struct SetRegionSelection { core::SessionId session_id; core::SourceId source_id; bool active{}; };

// include/avb/app/ports.hpp
struct CaptureContentChanged { core::Extent extent; };
struct RegionSelectionCompleted { std::optional<core::PixelRegion> region; };
// region == nullopt means cancel; Reset is SetRegion{session, source, nullopt}.
// Fixed RuntimeTickResult capacity grows from 8 to 10: 3 status + 5 lifecycle + 2 region facts.
```

`ReplicaSession` 與 `ApplicationState` 加入對應 set_opacity/set_region/set_region_selection，保留 generation 檢查。`RuntimeFeedback` 增加有效 content extent；`ControllerControls` 增加 opacity/region；ControllerViewState.settings 提供 selecting_region 與已接受 region，selected_source_id 提供已選來源的 owned optional SourceId。

選区結果由目前 runtime 的同步 tick 產生，先做 lifecycle 仲裁。失敗、關閉、source loss、recovery/resize 優先取消 selection。每次開始/結束 selection 清空 adapter pending gesture；取消 capture 時先清 gesture state，再 ReleaseCapture，以處理 WM_CAPTURECHANGED 重入。

## Task 1：透明度完整垂直切片

**Files:**

- Modify `include/avb/core/types.hpp`, `include/avb/core/replica_session.hpp`, `src/core/replica_session.cpp`, `include/avb/core/application_state.hpp`, `src/core/application_state.cpp`：opacity 值與 domain mutation。
- Modify `include/avb/app/commands.hpp`, `include/avb/app/application.hpp`, `src/app/application.cpp`, `include/avb/app/config_store.hpp`, `src/app/config_store.cpp`, `include/avb/app/controller_view_state.hpp`, `src/app/controller_view_state.cpp`：preview/commit admission 與持久化。
- Modify `platform/windows/detail/overlay_transactions.hpp`, `platform/windows/win32_overlay_adapter.hpp`, `.cpp`, `platform/windows/windows_replica_runtime.cpp`：統一原生 input/alpha transaction。
- Modify `platform/windows/windows_runtime_factory.cpp`：在 capture publication 前套用初始 native mode/opacity，沿用已 armed attachment guard 的失敗清理。
- Modify `include/avb/i18n/catalog.hpp`, `src/i18n/catalog.cpp`, `src/ui/controller_view.cpp`：控制項與語系文字。
- Tests `tests/app/config_store_test.cpp`, `tests/app/application_contract_test.cpp`, `tests/windows/overlay_policy_test.cpp`, `tests/app/controller_contract_test.cpp`。

- [x] 寫約 5 個精確 SECTION：legacy config 與錯誤 opacity local default、preview 至 commit 的磁碟呼叫次數、非法值/stale token、mode/border/opacity round trip、native failure rollback。先跑新增 SECTION 看見紅燈。
- [x] Config optional field 採 `is_number_integer` 加 0–100 驗證；初始化/選來源/替代 session 複製 config opacity；save 時驗證 accepted range。
- [x] Factory 在 StartCapture/publication 前使用 adapter 的 set_presentation 套用 settings.mode/settings.opacity_percent；失敗經現有 factory error/guard 清理。Task3 同一位置初始化 renderer crop 與 display aspect。驗證非 100% opacity 的第一次 Start，以及 Stop→Start 還原偏好。
- [x] SetOpacity 用 settings_allowed + token 驗證，candidate apply 成功再接受值。preview 設 dirty，commit 保存；失敗保持舊值。Stop 與選區開始保存 pending preview，Quit 維持現有 drain 規則。
- [x] 原生 target 從目前 style 重新計算本 owner 的 layered/transparent bits，alpha 取整至 0–255；目前其他 EXSTYLE bits 保留。set_mode 與新 set_opacity 共用 set_presentation(mode, percent) 交易，readback 驗證/rollback 沿用現有 helper。

```cpp
const auto alpha = static_cast<std::uint8_t>((percent * 255 + 50) / 100);
const auto effective_mode = requested.selecting_region
  ? core::OverlayMode::Editable : requested.mode;
const auto effective_opacity = requested.selecting_region ? 100 : requested.opacity_percent;
// SettingsOperations: apply_mode uses set_presentation(effective_mode, effective_opacity).
// verify_applied/rollback verify the same pair; helper/border/geometry follow existing transaction.
if (requested.fps_cap != owner.settings_.fps_cap)
  owner.scheduler_ = core::FrameScheduler{requested.fps_cap, now};
owner.settings_ = requested;
```

- [x] UI 使用 DragInt 與 AlwaysClamp，發送 live 與 final commit；UI local draft 在 active gesture 維持連續值，結束後以 accepted snapshot 同步。

```cpp
if (ImGui::DragInt(label.c_str(), &percent, 1.0F, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp))
  sink(app::SetOpacity{state.session_id, percent, false});
if (ImGui::IsItemDeactivatedAfterEdit())
  sink(app::SetOpacity{state.session_id, percent, true});
```

- [ ] 跑 build、focused tests、完整非硬體 tests；全新 subagent 先核對 spec 再 review quality，root 驗證問題並修正，Critical/Medium 清空後記錄 task delta / scoped commit。

## Task 2：選區值型契約、數學與 renderer

**Files:**

- Modify `include/avb/core/types.hpp`, `src/core/types.cpp`：PixelRegion valid/intersection，計算使用 subtraction 避開溢位。
- Modify `render/render_math.hpp`, `.cpp`：來源 content crop、UV offset/scale/clamp、目的像素往來源矩形。
- Modify `render/windows_render_stack.hpp`, `.cpp`, `shaders/replica.vert`, `shaders/replica.frag`：crop presentation 與選框 push constants；retained redraw。
- Tests `tests/render/render_policy_test.cpp` 與既有自有 graphics hardware fixture。

- [x] 加約 4 個數學 SECTION，先驗證紅燈：有效/越界與 uint32 上限、四方向拖曳和 letterbox、allocation padding 的 offset/scale、裁切 texel-center 邊界與 1×1。

```cpp
CHECK(core::intersect_region({90, 20, 40, 30}, {100, 100}) ==
      core::PixelRegion{90, 20, 10, 30});
CHECK_FALSE(core::intersect_region({100, 0, 1, 1}, {100, 100}));
// Source 200x100 aspect-fits 400x400 into (0,100,400,200).
// client drag (100,150) -> (300,250) maps to (50,25,100,50).
```

- [x] 新 renderer setter 接受 region 與 optional selection preview；record_draw 對 slot.content 求 effective crop，aspect_fit 使用 crop extent；UV transform 使用 slot allocation 分母。
- [x] push constant 放大為明確 align/layout 的 float 結構，vertex/fragment stage flags 同步更新；fragment 以 clamp 到 texel centers 的 UV 取樣，selection color stroke 疊在完整來源。
- [x] setter 於值改變時保留 presentation dirty，將 latest candidate 存為 renderer 值。renderer 提供 `has_redraw_work()`（dirty 且 retained image 可用）與 `request_redraw()`（給本次呈現許可）。runtime 使用 `scheduler_.poll(now, has_frame, render_stack_->has_redraw_work())`，schedule.render 後才呼叫 request_redraw；capture submit 沿用同次 admission。GPU/WSI retirement/recovery 每 tick 持續，present completion 清除已提交 revision 的 dirty，更新 revision 保留後續 demand。加入靜態來源、30FPS、連續 preview 合併的 deterministic 情境。
- [x] 為實際顯示與互動對齊新增 `PresentationSnapshot` 值：content、framebuffer、viewport、effective region、selection_full_view、present_sequence。recording 時複製至既有 bounded PresentRecord，成功 retire 後發布最新 sequence，晚 retire 的舊值保持較新 snapshot。rebuild/loss/zero framebuffer 使 mapping 失效，retained/source frame 再次成功呈現後恢復。欄位由 renderer 擁有並回傳 owned optional value。
- [x] 驗證 shader build、數學 SECTION、非硬體 tests。自有來源 fixture 以靜態 frame 驗證 crop-only 更新增加 retired presents；GPU/CPU 路徑依可用能力記錄。
- [ ] 全新 subagent spec/quality rolling review，root 驗證後修正並整理 task delta / scoped commit。

## Task 3：框選互動與 Application/UI 整合

**Files:**

- Modify domain settings/mutation files from Task 1：region/selecting_region。
- Modify `include/avb/app/ports.hpp`, `commands.hpp`, `application.hpp`, `src/app/application.cpp`, `controller_view_state.hpp`, `src/app/controller_view_state.cpp`：bounded facts 與 admission。
- Modify `platform/windows/win32_overlay_adapter.hpp`, `.cpp`, `platform/windows/windows_replica_runtime.hpp`, `.cpp`：native drag、已顯示來源 content、完成事實與停用處理。
- Modify `include/avb/ui/controller_view.hpp`, `src/ui/controller_view.cpp`, i18n catalog files：numeric draft、選取/取消/套用/還原 controls。
- Tests `tests/windows/overlay_policy_test.cpp`, `tests/app/application_contract_test.cpp`, `tests/app/controller_contract_test.cpp`。

- [x] 先寫約 5 個 SECTION：選區 admission/來源 reset、完成與取消結果、lifecycle+result 同 tick precedence、resize intersection 與空交集、UI整個 XYWH draft command。驗證新增行為紅燈。
- [x] Application SetRegionSelection(true) / SetRegion（含 reset）需要 Capturing + 當前 runtime 已呈現有效 content + SessionId/SourceId 一致；candidate 保留既有 region 並選取中。SetRegionSelection(false) 保留 region，取消可在 cleanup 開始前執行。SetRegion 檢查完整四元組，退出框選，套用成功後更新 domain。Source loss/改選/source resize/recovery/start/replacement 清除可用尺寸，第一張新有效呈現重新發布。
- [x] Runtime 記錄 renderer 成功 retire 的 PresentationSnapshot content，與 catalog/allocation 分離；CaptureContentChanged 於有效尺寸改變/失效發 edge（零 extent 代表失效）。框選使用相同 snapshot viewport 與 physical client/framebuffer 比例；尺寸或呈現幾何改變終止舊 gesture。
- [x] Adapter 提供少量主執行緒方法 `set_region_selection(bool)`、`take_region_selection_result()`、`region_selection_preview()`。WndProc 接收 WM_LBUTTONDOWN/MOVE/UP、WM_SETCURSOR、WM_KEYDOWN(Esc)、WM_KILLFOCUS/WM_CAPTURECHANGED；state 清理先於 ReleaseCapture。所有 callback 保持原本 guarded prepare/commit/delegate safety。
- [x] 進入 selection 的操作順序：native effective Editable+100 成功、等待完整 selection 畫面的 pending state、必要 focus/focus observation、經 scheduler 許可 full-view redraw、matching full-view snapshot 成功 retire、armed drag。gesture 保存當時 mapping，幾何相異時取消；present_sequence 增加但映射相同可持續拖曳。視窗 activation 限於使用者點選選區的命令。取消/完成經 typed facts 交回 App，App 套用原偏好與 region。中途 mode/border/opacity 編輯保存最新偏好，selection override 持續到退出。加「舊 crop 尚顯示時等待 full view」的明確測試。
- [x] Runtime 每次接受 region/取消/還原/首次啟動/來源 content resize/recovery 呈現時，使用有效 region 寬高更新 adapter->update_source_extent；selection 中使用完整 content 寬高。現有以 allocation 更新 resize constraint 的兩處改為此 presentation extent 規則；WM_SIZING 沿用既有 adapter constraint。測試 crop 與 cancel 回復的長寬比。
- [x] tick 每次只取最新 preview/result；recording geometry 或 source resize 取消 gesture。RuntimeEvent 容量 10 與註解/測試證明同步維護，Failed/Stopped 事件保持可達。SourceLost/recovery/Stop 清 transient；取消 native override 失敗按 apply error 分類，終端失敗 drain。
- [x] UI 一次提交 region 四元組：signed int draft 驗證非負/正大小後轉 uint32，顯示實際 content 大小；SourceId/SessionId/extent 改變重建草稿。SetRegion/SetRegionSelection 都帶目前 SourceId，Application 同時驗證來源與 session；新增同尺寸來源切換回歸情境。語系 key 與 widget stable ID 分離。doubleclick 與 typing 屬 UI local editing。
- [ ] build + focused/full nonhardware tests + owner-window hardware interactions；全新 subagent spec/quality rolling review，查證修正後整理 task delta / scoped commit。

## Task 4：使用文件、完整驗證與收尾

**Files:** `README.md`, `docs/architecture/application-ui.md`, `docs/architecture/windows-runtime-overlay.md`, `docs/architecture/renderer.md`, `docs/verification/2026-09-15-opacity-region.md`。

- [x] 以繁體中文、肯定句、目的→需求→結構順序更新操作與架構；新 public contracts 使用高資訊密度 Doxygen，註明座標空間、暫時 override、ownership、失败恢復與成本。
- [x] 跑 Release build、非硬體 CTest、boundary/inventory；原有硬體 fixture 依明確自有視窗做 opacity/readback、region/retained redraw。記錄自動與人工驗證區別。
- [x] 全新 reviewer 對本次整體 delta 做最終完整 review；實證確認問題、最小修正、驗證、再開全新 reviewer，直到 Critical/Medium 為零。
- [ ] 檢查 task changes 與既有 baseline 分離、tests/spec/plan 排除提交；回報 exe 路徑、操作方式、驗證結果與尚需使用者實機操作的項目。

## Plan self-review 與 rolling review

Spec coverage 對應 Tasks 1–4；名稱、candidate settings、事件數量、來源像素與物理 client 座標、無新來源 frame 的 redraw 已逐項檢查。各 task 保留完整測試/實作/驗證/review 流程。

Round 1：0 Critical / 4 Medium。已逐項查證並修正 redraw FPS admission、publication 前的初始 presentation、native resize aspect 同步、retired presentation mapping/armed drag 的時序。新增測試整合既有 SECTION。

Round 2：全新 reviewer 完整核對，0 Critical / 0 Medium，plan gate 通過。實作時涵蓋 resize/recovery 前的舊 present late retirement 尺寸失效情境。
