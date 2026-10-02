# Vulkan performance 工作交接（2026-09-16）

## 停止點與工作規則

使用者於本輪要求「現在的東西做完之後停一下，準備交接」。收尾範圍為 Task 5B；Task 6 保留已準備的工作樹內容，Task 7 留待後續。此文件與 tests／spec／plan／verification／artifacts 維持未提交。

實際 repo：`D:\document\MesRepo\Airelinna`，產品 avb0795、namespace `avb::`，branch `p4-application-release`。工具預設 cwd `D:\document\MesRepo\5568ke` 是另一個 repo；每個命令都指定正確 workdir。Build／Git／建立 artifact parent 需要環境的 escalated execution；文字修改使用 apply_patch。

每階段維持 fresh reviewer → root 驗證 findings → 修正 → 全新 reviewer，直到 0 Critical／0 Medium。完成 reviewer 保持 inactive；工具提供 interrupt，缺少 close/delete。小階段 commit、保留 unrelated dirty files；push 由另行指示決定。

## 已完成與提交

- Task 3 C3 `972c804`：Vulkan device methods 拆分；C4 `e27448f`：WSI methods 拆分。完整 review 0/0、15 tests／hardware3、validation VUID0、exact snapshot 通過。
- Task 4 `4338103`：replica permanent presentation resources。Review0/0、full15／hardware3、VUID0、snapshot；B3/B4 warmup 後六項 native create/destroy counters 全部0。
- Task 5A `5317b184b217f27e936fc651e594237fde97c5ce`：controller fixed-capacity frame／present resources。Full15、native15、VUID0、exact tree `cc0fa79eb5eb52304fe12c1803dd94566f08609d` 通過。B1/B2 60秒各598 UI builds／submissions／retirements；所有native create/destroy為0，high-water3cmd／9fences／6semaphores。
- Task 5B `6643827c0dacfb0102272981b809f981e2213a97`，tree `2621aff1336f29c4ca8d813c5876114b1b6754c9`：capacity migration。Full Task5 fresh review0/0、精確snapshot、full15／hardware3、nativeUI15與20次真實3↔4migration全部通過，VUID0。

詳細證據入口：同目錄 `2026-09-16-vulkan-performance-task3.md`、`task4.md`、`task5.md`。Approved spec／plan 位於 `docs/superpowers/{specs,plans}/2026-09-15-vulkan-portability-performance*.md`。

## Task 6：保留未提交的 prepared changes

- 新 `render/cpu_row_copy.hpp/.cpp`；`render/vulkan_renderer.cpp` 的 completion callsite；`src/CMakeLists.txt` 一行 source。
- `docs/architecture/shared-vulkan-renderer.md` CPU rows H2；`docs/development/vulkan-performance.md` byte cost 段落。
- `tests/render/render_policy_test.cpp` 既有 case06 三個 SECTION：tight、padding/shrink、invalid/overflow。該檔同時含先前 Task4/5 的未提交測試。
- 真正 RED 55 failed／155 passed，GREEN case06/08 2/2。新 reviewer `task6_cpu_rows_review_1` code review0/0；其 review 早於 root 的兩份文件補充。
- Root 已讀取 code、caller Map/flush/discard 與 span/overflow 計算。Task5B收尾的主工作樹full15／hardware3／isolatedvalidation同時包含Task6 prepared code且通過；Task6的frozen commit build、B4/B5/B6及包含文件的final review待後續。
- 日誌 `artifacts/vulkan-perf/task6-20260916/`；說明 `docs/verification/2026-09-16-vulkan-performance-task6.md`。

後續從核對此 diff 與當前 HEAD 開始。Task6 commit allowlist 為上面四個 code/CMake 檔及兩份 documents；tests 持續 unstaged。按計畫完成驗收後提交 `perf(render): clear CPU upload padding selectively`。

## Task 7：後續驗收與延遲警戒

Task7 尚待執行：完整 V1–V6、native acceptance、paired baseline/candidate B1–B6 每組3次（10秒warmup＋60秒measurement）、summary／文件／全新整體review。

先前量測出現 intake-to-submit p95 警戒（同時 >5% 與 >0.5ms），持續追蹤。早晨baseline B3p95 2.8421ms／B4 4.1263ms；同日較晚使用凍結binary重測，B3 baseline4.2193ms、Task2 5.0344ms，顯示桌面環境漂移與可能cadence影響需區分。Task4單次B3 4.8990ms、B4 5.3254ms。以同時段交錯runs確認，維持實際工作量、drop、source skipped-periods 等證據；整體效能驗收仍為待辦。

Static B2 本輪有 WGC約2FPS，source仍paint_calls1／scheduled_updates0；早晨baseline static穩態0FPS。對照時依每run實際來源／capture資料解讀。

固定fixture source：`artifacts/vulkan-perf/task1-20260916-4eec1ed5/fixture/fixture.cpp`，SHA256 `C01D4D84531D89CF6D4CBE78C4A7F41CCE0220AF90220357A41A56444491D3D0`。Baseline fixture executable SHA256 `B6D20CC537D85214C24C91AADFEB3BBB26CC11A93E1E4F1648D199B0A7D26106`。保留這些歷史來源與binary；新candidate透過 `artifacts/vulkan-perf/shared-renderer-fixture-20260916/CMakeLists.txt` 連結自己的frozen production archives。

## 可重複的驗證環境

- x64 VS18 Community developer environment，clang-cl22.1.6，NMake Release C++23。CMake／CTest：`C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/`。
- 主build `build/p4-release`；`AVB0795_WGC_HARDWARE_CONSENT=1` 後跑full CTest，共15cases（hardware13–15）。Inventory：CMake script `cmake/VerifyTestInventory.cmake`，profile `windows-v1-full`，Lua `C:/lua/lua.exe`。
- Compile SDK1.4.304.1；correctness 使用 isolated SDK1.4.357.0 validation DLL：`artifacts/vulkan-perf/validation-layer-1.4.357.0/extracted/Bin`，環境 `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`、`VK_LAYER_PATH=<該目錄>`、`VK_LOADER_DEBUG=layer`。確認actual layer path及VUID0。正式benchmark移除這三個process環境變數。
- 舊304 layer外部timeline `03268`為已確認上游false positive；acquire fence `01123`曾是真實問題，已以submission＋acquire雙proof修正。保留GPUexternalwait與各自completion責任。
- Windows25H2 build26200.9445，RTX3070Ti rawdriver2559541248（Windows32.0.16.1062），Balanced。正式measurement期間暫停heavy build／hardware tests。
- 每commit明確stage allowlist→diffcachedcheck→gitwrite-tree→checkout-index到唯一source→testsOFF獨立build→再比tree→commit。依賴overrides：nlohmann/glfw使用`build/p4-release/_offline-src/`，ImGui使用`artifacts/vulkan-perf/task2-20260916/patched-imgui`。
- Native15fixture：`shared-controller-fixture-20260916` 連結對應snapshot。固定acceptance source為Task2，SHA256 `92A0EFCEA6D3F3C7A6E1452801C0FFA1D4CA7DF7E61D0429B896AE901DB890CA`。直接cmd呼叫 `--acceptance <new absolute prefix> --send-input`，stdout/stderr在cmd內redirect。Background Start-Process曾失去foreground；同時段direct baseline/candidate各15/15。
- Fresh prefix與獨立working directory保存report/config；測試目標限fixture自有視窗。Physical monitor DPI／GPU實體故障另列範圍。

## 本輪最終停止狀態

最終HEAD為 `6643827c0dacfb0102272981b809f981e2213a97`；Task5兩份production/doc檔為clean，index為empty。Task6六份code/docs改動以及既有unrelated dirty files保留原處。所有本輪subagents已completed；全部build、benchmark與nativefixtureprocess均已結束。

已提交Task5的獨立可執行檔為 `artifacts/vulkan-perf/task5b-capacity-20260916/build/src/avb0795.exe`。主build `build/p4-release/src/avb0795.exe` 包含未提交Task6 prepared code；測試目前已提交版本時，使用前者。

使用者要求本輪於此停止。下一輪先確認Git與檔案fingerprints，再接Task6驗收與commit，之後進Task7；此交接與相關verification保持本機未提交。
