# Task 4：Replica presentation resources 重用

起點：`e27448f77fa564be184519ac952233859156fbbf`，Task 3 完整 review 0 Critical／0 Medium、15/15 cases 與三組 checkpoint 已完成。

## Test-first 與 owner 接線

全新 implementation agent `task4_replica_pool_impl` 在既有 case 08 增加一個 SECTION，涵蓋容量、serial、transfer／retirement 與既有錯誤 classifier 對應。缺少 PresentUse API 的 compile RED 為 exit 2，log：`artifacts/vulkan-perf/task4-20260916/red-build.log`。後續 native／policy 接線沿用三筆固定 present records，owner 自己建立／回收 native handles。

Root 核對一項額外候選問題：若多筆 ReleaseRetained 同時等待同一 swapchain，第一筆 reset 失敗可能提前結束 cleanup loop。現有 command／submit／present 失敗皆同步關閉後續 admission，先前 enqueued slots 已移交 image ownership；root 與 agent 查核現有控制流後，該多筆序列缺少可達路徑。此次保留既有 loop，將修正範圍集中於新 pool 的 active ownership 在 fallible reset 前結算。

Implementation agent 交付後，root 逐項追蹤 reservation、partial bootstrap、same-ring／new-ring、submission／present proof、release／loss 與 reset failure。正常路徑保留 handles；例外路徑依原 classifier 處理完成且待消費的 signal。`graphics_retry_policy.hpp` 以單獨 69 行 patch 暫存新增契約，原有 Doxygen dirty changes 維持原狀。

- Agent targeted build 通過；case 08 1/1，hardware 14/15 2/2，logs `green-{build,policy,hardware}.log`。
- Root full build／boundaries 通過；CTest 15/15、hardware 3/3，4.09 秒；inventory 15，logs `full-build.log`、`full-ctest.log`。
- SDK 1.4.357.0 isolated validation cases 14/15：2/2，2.26 秒，VUID 0；loader 確認指定 DLL，`validation-357.log`。
- 全新 reviewer `task4_replica_pool_review_1`：0 Critical／0 Medium，獨立核對計畫契約、可達例外、bounded accounting、serial、proof／teardown 與分層。
- 五檔 staged tree `9d934c1e7e8d416622ef62c4b4e25067e2607db2`，252 insertions／108 deletions；精確 source 位於 `artifacts/vulkan-perf/task4-pool-20260916/source`，獨立 executable／diagnostics／boundary build 通過，`snapshot-build.log`。

## 固定 workload allocation 證據

固定 Task 1 fixture 連結上述 snapshot archives；binary SHA256 `943DA4509DBC411BD5E14335A34D4CF0E2CEE373D9131380DCEF5148C8886E7C`。B3／B4 各一次 10 秒暖機＋60 秒量測，fixture／collector complete、drain complete、exit 0、stderr 0；期間暫停 heavy build／hardware tests，explicit validation 關閉。

| Run | One-core CPU % | Presented FPS | p95 ms | Command buffers | Fences | Semaphores |
|---|---:|---:|---:|---:|---:|---:|
| B3 GPU interop | 3.1751 | 47.6197 | 4.8990 | 3 | 9 | 7 |
| B4 CPU | 7.7544 | 48.8956 | 5.3254 | 3 | 9 | 6 |

兩筆報告在 measurement window 內，command buffer／fence／semaphore 的 created 與 destroyed 六項 counters 均為 0。上表資源欄位為 owner high-water；GPU 額外一個 semaphore 由 device-scoped bridge timeline 擁有。

B3 collector admitted／submitted／retired／dropped = 2859／2858／2858／2；各 counter 按自身事件時間落入固定窗口，跨窗口事件與完整 drain 分別記錄。B4 四者為 2934／2934／2934／0。B4 CPU copy／clear／upload 各 24,335,769,600 bytes；padding 最佳化屬 Task 6。FPS 與 C4 checkpoint 接近；單次 CPU／latency 數字供階段檢查，先前延遲警戒繼續由 Task 7 交錯對照定位。

Task 4 已提交 `433810359f8c50e444c112fc3cd4d62dc4ff1463 perf(render): reuse retired replica presentation resources`。Task 5 依 fixed initial-capacity reuse／capacity migration 分成兩個可建置增量，各自 review／驗收後提交。Tests、spec、plan、verification、artifacts 保留工作樹。
