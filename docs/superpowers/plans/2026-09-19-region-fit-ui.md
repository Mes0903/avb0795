# 框選與比例同步實作計畫

> **For agentic workers:** Use subagent-driven-development. Follow test-driven-development for implementation and verification-before-completion for each commit.

**Goal:** 以框選操作設定 crop，讓 crop／取消／全畫面切換同步副本比例，並提供可見的不透明度拉桿。

**Architecture:** UI 發送既有 typed commands；Application 驗證 domain；Windows runtime settings transaction 決定有效尺寸與 commit；Win32 adapter 擁有 HWND、work-area、frame insets 和 readback；純值 policy 共用 bounded-aspect math。

**Tech stack:** C++23、Dear ImGui、Win32、Vulkan、Catch2、CMake／clang-cl Release。工作目錄 `D:/document/MesRepo/Airelinna`，沿用 `p4-application-release`。

**Spec:** `docs/superpowers/specs/2026-09-19-region-fit-ui-design.md`；spec review r1 已確認 0 Critical／0 Medium。下列工作於 plan review 通過後依序實作。

## 共通驗證與提交

- 保存本次目標檔案原貌到 `artifacts/region-fit-20260919/before/`，保留原有 dirty edits。
- 使用 `build/p4-release`；每次一位 implementer 擁有 shared build slot。基線與最後執行 `ctest --test-dir build/p4-release -LE hardware -E "case 11:" --output-on-failure`。
- VS developer shell：`C:/Program Files/Microsoft Visual Studio/18/Community/Common7/Tools/VsDevCmd.bat -arch=x64 -host_arch=x64`；CMake／CTest 使用同安裝下 `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/`。
- 每階段：精準 regression RED → 最小 production 修正 → GREEN → fresh spec compliance reviewer → fresh quality reviewer；Medium／Critical finding 由 root 查證、修正後交全新 reviewer。
- 每階段允許清單 staging，驗證 `git diff --cached --check` 與 staged tree build；commit production 與本次 focused guide。Tests、spec、plan、verification、artifacts、其他 issue 和原有改動保留工作樹。
- Staged tree build 使用 `git checkout-index` 匯出至該階段 artifacts snapshot，以 NMake／clang-cl、Release、tests OFF 建置 `avb0795` 與 `verify_source_boundaries`。重用 `build/p4-release/_offline-src/{nlohmann_json,glfw}` 與 `artifacts/vulkan-perf/task2-20260916/patched-imgui` 的 pinned sources。commit 前確認 tree hash 一致。

## Task 1：框選操作與不透明度拉桿

**Status:** 完成，commit `a862457`。Spec compliance 與 quality 各經全新 reviewer 確認 0 Critical／0 Medium；low diagram timing 已澄清。RED 兩個 assertion，GREEN controller case 07 和 safe 11 cases，exact staged tree Release build 與 boundary check 通過。

**Files:**

- Modify `src/ui/controller_view.cpp`、`include/avb/ui/controller_view.hpp`。
- Modify `src/i18n/catalog.cpp`、`include/avb/i18n/catalog.hpp`。
- Modify local test `tests/app/controller_contract_test.cpp`。
- Update live guides `README.md`、`docs/development/replica-controls.md`、`docs/architecture/application-ui.md`、`docs/development/vulkan-performance.md`；各自保留既有 staged scope。
- Create focused committed guide `docs/development/region-selection.md`，說明本階段操作；Task 2 補充尺寸規則。

1. 先讀現有 opacity／wheel headless ImGui fixtures。將 numeric-region SECTION 改為 Select／Cancel／Reset typed commands 與 session／source identity 驗證；維持 missing-content cancel coverage。加入可見 slider 滑鼠互動、preview 與 end-of-edit commit 的精準 assertions，執行 controller case 07，確認 assertion RED。
2. 刪除 `RegionDraft`、XYWH widgets、ApplyRegion、六個専用 TextId（ApplyRegion／RegionX／RegionY／RegionWidth／RegionHeight／RegionInvalid）與 enum 對應三語 rows。widget scope 保留目前 session/source/extent 身分組合。保留 Select、Cancel、Reset、SourceContentSize 與 Help。
3. Opacity 改 `SliderInt(..., 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)`，沿用 `LiveEditOnInputScalar`、OpacityEditing、preview、完成保存、hidden completion 和 session 驗證。OpacityHelp 提示 Ctrl-click，RegionHelp 說明框選流程。
4. 清除此次失去用途的 includes；更新 docs 使用可見拉桿與框選操作的敘述。文件以目的、需求與責任解說，段落採正向敘述。
5. Build `avb0795 avb0795_core_tests verify_source_boundaries`；跑 controller focused case、safe baseline 與 inventory。依序 fresh spec review、quality review，查證並修正有效 finding。
6. Scoped staged snapshot build 後 commit `feat(ui): simplify region controls and expose opacity slider`。

## Task 2：視窗有效內容比例同步

**Status:** 實作與 spec compliance review 完成（全新 reviewer，0 Critical／0 Medium）。Final quality review 等待全新 thread 名額；多次建立皆回報 `agent thread limit reached`。六個目標檔案維持 staged，候選 tree 為 `d7172f236a987f06e6354048eae12bd953f501a7`，下一步為全新品質 reviewer → 查證 finding → 核對已驗證 tree → commit。

**Implementation evidence:** pure fit RED 83 assertions／23 failures；adapter RED 61／17。Native readback 另重現 Bordered 直式選區期望 client 寬 14、Windows 實際寬 120。Fit／restore 加入 `SWP_NOSENDCHANGING`，由 pure policy 與精確 readback 管理程式主動幾何，維持互動 sizing 的原有路徑；七個精準 SECTION 共 515 assertions 通過。獨立 safety review 確認新的 case 14 SECTION 安全隔離；真實 runtime RED 顯示 crop 後 height 480、預期 360，接入 runtime 後 root 再驗 176 assertions、exit 0。Safe CTest 11/11、15-case inventory、工作樹五個 build targets／boundaries、exact staged snapshot Release build／boundaries 皆通過。完整證據列於 `artifacts/region-fit-20260919/README.md`；實際拖曳框選由使用者操作驗收。

**Files:**

- Modify `platform/windows/detail/overlay_policy.hpp/.cpp`。
- Modify `platform/windows/win32_overlay_adapter.hpp/.cpp`。
- Modify `platform/windows/windows_replica_runtime.cpp`。
- Modify local tests `tests/windows/overlay_policy_test.cpp`、`tests/windows/windows_graphics_hardware_test.cpp`。
- Update focused guide `docs/development/region-selection.md` 和現有 live guides 的比例說明。

1. 在 case 11 加 pure fit SECTION：一般寬度、橫／直 crop、極端比例、工作區上限、中心平移、frame insets、tiny client 與 invalid geometry。先 stub `fit_content_rect(client_screen, source, insets, work_area)` 回傳原 rect，確認具體計算 assertion RED。
2. 從 `wheel_resized_rect` 抽出 bounded-aspect 計算；wheel 保留 notch factor 與 DPI 160×90 floor，fit 使用 factor 1 與至少一個 pixel。共享 validation、等比例 cap、rounded width/height、center anchor、work-area clamp。維持 wheel 的既有 regression。
3. Adapter 新增 `fit_client_geometry(baseline, source)`，回傳 `expected<OverlayClientGeometry, AppError>`。查目前 frame insets 與 nearest monitor work area，呼叫 pure policy（回傳 outer rect），SetWindowPos 使用 NOACTIVATE／NOZORDER，精確 client readback 後回傳 verified target。`restore_client_geometry` 增加 typed `GeometryRestoreBounds::{WorkArea, Exact}` 參數，預設 WorkArea 保持 border forward 的現有行為；交易 rollback 明確使用 Exact。Exact 直接以原 snapshot 和目前 frame insets 計算 outer rect，核對尺寸／座標可表示與 round-trip，SetWindowPos 後精確比對 snapshot；因此跨工作區邊界的原位置也能恢復。
4. Case 11 hidden HWND SECTION 驗證 fit 結果、border style、tiny width、後續 restore 原 rect，包含原 snapshot 部分位於工作區外的精確恢復。全程 hidden，真實輸入由使用者持續控制。保留 wheel 與現有交易失敗 stage tests。
5. Runtime 使用共用 internal `effective_presentation_extent(settings, content)` 取得 selecting/full 或 crop intersection。只有 region／selecting 變更且可用 presented content 時建立 fit target。`apply_border` 於 border 或 fit 需要時保存原 geometry；`apply_geometry` 執行 fit 或原 border restore；`verify_applied` 核對目標；rollback 使用同一原始 snapshot。成功後才由既有 commit 發布 settings／renderer／aspect。
6. 同時變更 border 與 region 依新 frame insets fit，原 client snapshot 保留 rollback。中斷期間 region-clear 保持現行 admission 與 geometry；source resize、首次 frame、每幀更新維持現行 aspect-only 職責。
7. Real runtime regression：將 case 14 原有完整 body 包入 legacy SECTION，新增同層 `region settings fit native client geometry without input` SECTION。既有 case 14 輸入、焦點、minimize／restore 維持於 legacy SECTION；case 15 保持現行 coverage。
8. 安全 fixture：source child 用 WS_EX_NOACTIVATE／SW_SHOWNOACTIVATE；GLFW_FOCUSED=false、FOCUS_ON_SHOW=false 於 main preflight 前設定；新 SECTION 再設一次與 MOUSE_PASSTHROUGH=true。新 runtime 用 PassThrough。測試全程 selecting_region=false，直接 apply positive crop／null reset，檢查 HWND rect、framebuffer presentation ratio、工作區限制與 tick 後幾何穩定。測試保留 source process／runtime RAII 與有界等待。
9. 新 runtime SECTION 首先在 production runtime wiring 前執行，確認 crop 後 rect assertion RED；完成 wiring 後 GREEN。設定 `AVB0795_WGC_HARDWARE_CONSENT=1`，直接執行 hardware binary 的 case 14 `--section "region settings fit native client geometry without input"`。此結果涵蓋真實 crop／reset 設定交易；框選拖曳焦點流程留給使用者操作驗收。
10. Build `avb0795 avb0795_core_tests avb0795_windows_policy_tests avb0795_windows_graphics_hardware_tests verify_source_boundaries`。執行 safe baseline、case 11 pure／hidden fit、wheel pure／hidden、DPI／border／rollback 專用 SECTION，以及上述安全 runtime SECTION。驗證 inventory 持續 15 case IDs；targeted assertions 與 test output 存 artifacts。
11. Fresh spec review → fresh quality review → root 驗證 finding，rolling 到 0 Critical／0 Medium。Scope staging overlay_policy.hpp 本次新增 hunk，保留既有 Doxygen changes；其餘明確 production 檔案及 focused guide。Staged snapshot build 後 commit `fix(windows): fit replica geometry when region settings change`。

## 完成交付

- 回報兩階段 commit、可執行檔位置與實際測試結果。
- 說明 opacity slider／Ctrl-click、框選／取消／reset 的比例規則。
- 硬體測試報告區分 runtime 設定交易與人工拖曳手勢。初始視窗偶發過小 issue 延續既有紀錄。
