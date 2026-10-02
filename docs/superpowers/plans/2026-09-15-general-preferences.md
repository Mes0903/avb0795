# General Preferences Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 保存八類通用偏好，於重新啟動恢復，並使用 `avb0795.config` 相容讀取舊設定。

**Architecture:** 既有 UI typed command → Application 接受 → ConfigStore 保存；native apply 維持原 owner。V1 加入兩個 optional 欄位，啟動組裝提供新舊路徑，初始化讀取一次。

**Tech Stack:** C++23、nlohmann/json、Catch2、CMake/CTest、現有 Windows atomic replacement。

**Spec:** `docs/superpowers/specs/2026-09-15-general-preferences-design.md`，spec review 0 Critical / 0 Medium。

## 工作樹與驗證

Repo：`D:/document/MesRepo/Airelinna`；branch：`p4-application-release`。沿用目前 checkout，已有 rename／UX／opacity／crop 內容維持原樣。此次 16 個可能修改檔案的基準位於 `D:/document/MesRepo/5568ke/build/avb-general-preferences-baseline-20260915`。Baseline 非硬體 CTest 12/12。

測試與提交各自管理：tests、spec、plan 保留工作樹；production/doc 提交等待使用者確認既有 dirty baseline 整理方式。每個 task 保留獨立差異與 review 證據，index 維持原狀。

實作階段每完成一個 task，以全新 reviewer 核對 spec 與責任歸屬，主 agent 查證問題並修正，直到 0 Critical / 0 Medium。完成 task 後持續下一 task。

```powershell
cmd /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build/p4-release --target avb0795 avb0795_core_tests avb0795_windows_policy_tests'
& .\build\p4-release\tests\avb0795_core_tests.exe 'avb0795 V1 case 04:*'
& .\build\p4-release\tests\avb0795_core_tests.exe 'avb0795 V1 case 01:*'
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir build/p4-release -LE hardware --output-on-failure
```

所有命令於 repo 執行，建置使用既有 Release tree 與開發環境。這個 checkout 位於工具預設 writable root 外，建置／執行測試使用既有 escalation 機制。預期 build exit 0、focused cases 通過、CTest 12/12。

## Task 1：偏好格式與完整 Application 接線

**Files:**

- Modify `include/avb/app/config_store.hpp`, `src/app/config_store.cpp`：欄位、schema、legacy fallback。
- Modify `include/avb/app/application.hpp`, `src/app/application.cpp`：初始化、通用偏好接受與保存。
- Modify `include/avb/app/commands.hpp`：更新 SetMode／SetAutoRefresh Doxygen 契約。
- Modify `include/avb/core/replica_session.hpp`：更新 set_mode 註解，表達 Core 值提交與 Application 持久化的分工。
- Modify `include/avb/core/types.hpp`：同步 ReplicaSettings 的 mode／crop 保存範圍註解。
- Modify `main.cpp`：執行檔旁的新舊檔名。
- Modify `tests/app/config_store_test.cpp`, `tests/app/application_contract_test.cpp`：沿用 case 04／01 與既有 fixtures。

- [x] 先更新既有 mode 成功保存 assertion，加入約 6–8 個聚焦 SECTION；觀察舊行為導致紅燈。Config schema 可先透過既有 JSON API 輸入新欄位，驗證原值保留與序列化結果；Application 先驗證成功 mode／auto-refresh 命令的檔案副作用。Legacy 與新值型介面於可編譯後加入直接紅綠驗證。

```cpp
// Case 04: these new optional keys must preserve the other V1 values.
document["mode"] = "pass_through";
document["auto_refresh_enabled"] = true;
REQUIRE(write_text(config_path, document.dump()));
const auto loaded = store.load(config_path);
REQUIRE(loaded.has_value());
CHECK(loaded->fps_cap == FpsCap::Fps144);
// Case 01: replace the old successful-mode expectation of zero writes.
CHECK(harness.config.replace_count == 1);
```

- [x] 在 UserConfig 尾端加入 mode／auto-refresh，JSON keys 同步至九個。新增可讀的 mode parse／serialize，缺漏或格式錯誤分別回 Editable／false；save 的 invalid mode 在任何 I/O 前回 `InvalidValue`。

```cpp
core::OverlayMode mode{core::OverlayMode::Editable};
bool auto_refresh_enabled{};
// Canonical tokens: editable, pass_through.
// Missing or malformed optional values recover per field.
```

- [x] 以 optional legacy path 擴充 `ConfigStore::load`。有指定 legacy 時先查新檔；查驗成功且缺席就呼叫單路徑 load(legacy)，其餘沿用新檔讀取與既有錯誤分類。第二次呼叫採預設空 legacy，回到單檔讀取流程。Load 保留磁碟內容，save 始終使用呼叫者指定的新路徑。

```cpp
[[nodiscard]] std::expected<UserConfig, core::AppError>
load(const std::filesystem::path &path,
     const std::filesystem::path &legacy_path = {}) const;
```

- [x] Application 建構子在既有參數末端加入 `const std::filesystem::path &legacy_config_path = {}`，initialize 同步接收並傳給 ConfigStore；legacy 借用到初始化結束。`config_path_` 持續擁有新路徑。Main 的 resolver 回傳 `avb0795.config`，Application 最後參數為其 parent 下的 `airelinna.json`。

```cpp
// main: executable-relative write destination.
return executable.parent_path() / L"avb0795.config";
// Application initialization:
const auto loaded = config_store_.load(config_path_, legacy_config_path);
const auto initial_mode = capabilities_.pass_through
  ? config_.mode : core::OverlayMode::Editable;
(void) state_.set_mode(session_id, initial_mode);
```

- [x] 將 auto-refresh 的重複 bool member 收斂至 `config_.auto_refresh_enabled`；view projection、manual refresh 與 timer 讀同一欄。SetAutoRefresh 改值時重設 deadline／標 dirty，dirty 時 persist_config；同值可重試失敗保存。初始化後第一個 step 以既有 five-second scheduler 安排刷新。

```cpp
if (config_.auto_refresh_enabled != command.enabled) {
  config_.auto_refresh_enabled = command.enabled;
  source_refresh_deadline_.reset();
  config_dirty_ = true;
}
if (config_dirty_) {
  persist_config();
}
```

- [x] SetMode 先排除非法 enum，再經既有 session/capability/apply/domain 驗證；接受後保存 config.mode。RestoreActiveReplicaEditable 的 runtime apply 成功後也同步保存 Editable。保留其他設定值與框選 transient override，失敗維持先前 config；按鍵重複且值維持 Editable 時使用既有 dirty 狀態決定保存。

```cpp
// After runtime and domain acceptance:
if (config_.mode != command.mode) {
  config_.mode = command.mode;
  config_dirty_ = true;
}
if (config_dirty_) {
  persist_config();
}
// The global restore command uses the same accepted-value rule with Editable.
```

- [x] 補完精確驗證：缺漏與 invalid optional 值、mode invalid save、新檔／舊檔／兩檔缺席、格式錯誤新檔優先與 existing unreadable path；保存新檔保留舊檔。重建 Application 讀回八類偏好、初始來源與 crop 空值、auto-refresh +5s；mode apply 拒絕/stale/invalid 零保存，快捷鍵恢復與框選保留期望值，保存失敗後同值 retry。

```cpp
// Real ConfigStore with temporary files, existing replace fixture.
CHECK(store.load(new_path, legacy_path) == legacy_config);
REQUIRE(store.save(new_path, accepted).has_value());
CHECK(store.load(new_path, legacy_path) == accepted);
CHECK(store.load(legacy_path) == legacy_config);
// Existing Application harness, reopened after an idle preferences edit:
auto restarted = harness.make_application();
CHECK(restarted->view_state().auto_refresh_enabled);
CHECK(restarted->view_state().settings.mode == avb::core::OverlayMode::PassThrough);
CHECK_FALSE(restarted->view_state().selected_source_id);
CHECK_FALSE(restarted->view_state().settings.region);
restarted->step(now);
CHECK(restarted->next_deadline() == now + 5s);
```

- [x] 跑 build、focused cases、完整非硬體 tests。全新 reviewer 先核對 spec，再由全新 reviewer 核對 code quality；具體問題經 root 查證、修正、重驗後再開全新 reviewer。記錄 task delta，保持 index。

## Task 2：操作文件與最終驗證

**Files:** `README.md`, `SECURITY.md`, `docs/architecture/application-ui.md`, `docs/architecture/core.md`, `docs/architecture/desktop-host.md`, `docs/development/replica-controls.md`, `docs/verification/2026-09-15-general-preferences.md`。

- [x] 文件先說保存目的，再列八類欄位與檔案位置。說明新檔優先／新檔缺席時讀取舊檔、偏好保存時機、操作中的 transient 值、設定目錄權限及保存診斷。中文採繁體與肯定句；既有 inventory 路徑維持原樣。
- [x] 同步 schema JSON／Doxygen 和 auto-refresh／mode 舊 session-only 描述，確認 prose 與程式一致。
- [x] 執行 Release build、非硬體 CTest 12/12、以下邊界與 inventory 檢查，並確認執行檔時間。此輪使用 portable/policy tests 驗證 config，先前 graphics hardware 證據獨立保留。

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' -DAVB0795_SOURCE_ROOT=D:/document/MesRepo/Airelinna -P cmake/VerifySourceBoundaries.cmake
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' -DBUILD_DIRECTORY=D:/document/MesRepo/Airelinna/build/p4-release -DLUA_EXECUTABLE=C:/lua/lua.exe -P cmake/VerifyTestInventory.cmake
git -c core.safecrlf=false diff --check
git diff --cached --quiet
```

- [x] 全新 reviewer 核對完整 task delta 與文件至 0 Critical / 0 Medium。驗證紀錄分開記錄 build／tests／review、人工重啟操作與提交狀態。

## Plan self-review

八類通用偏好、源端暫態範圍、初始化、舊檔相容與保存失敗全部對應 Task 1；操作文件與完成證據對應 Task 2。原生 capture／renderer 與 host ownership 維持原有分層。

## Plan review

2026-09-15 round 1：全新 reviewer 對照 live code 與 case 01／04，0 Critical / 0 Medium。已修正 Task 2 的新檔缺席 fallback 用語。

## 完成紀錄

Task 1 規格符合性與完整品質／文件審查各由全新 reviewer 執行，均為 0 Critical / 0 Medium。Release build、12/12 非硬體 CTest、分層邊界、15 個 V1 cases 清單與差異格式檢查通過。實作與文件完成，人工重啟驗收與提交整理各自保留待確認，詳見 [驗證紀錄](../../verification/2026-09-15-general-preferences.md)。
