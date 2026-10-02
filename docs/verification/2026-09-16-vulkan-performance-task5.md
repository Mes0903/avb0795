# Task 5：Controller frame／present resources

Task 5 依初始容量重用（5A）與 image-count migration（5B）分成兩個可建置增量，各自驗證、全新 review 與提交。

## 5A：固定容量 native resources

起點 `433810359f8c50e444c112fc3cd4d62dc4ff1463`。Implementation agent `task5_controller_pool_impl` 將 DesktopHost 的 frame 與 present records 改為兩個容量 C 的永久容器。Frame 與 present 分別由 acquire／submission proof 與 maintenance1 present proof 回收；native handles 在正常退休保留。例外 signal 更換、ReleaseRetained、font staging 與 loss firewall 保持既有責任。

Root 查核 native-create error path 後，將 present record 的 output handles 改為成功後接管；create 失敗時 local output value 保持在 native 呼叫邊界。Ownership 在可能失敗的 reset 前結算，以維持單次 retirement accounting。Root 另確認 frame 命令仍參考 framebuffer 時，WSI retirement 維持該借用。

- 既有 case 08 同一個 pool SECTION 增加 C=2／3／4 資料；inventory 保持 15。
- 主工作樹 production／tests／source boundaries build 通過；CTest 15/15、hardware 3/3，4.01 秒。
- SDK 1.4.357.0 isolated layer cases 14/15：2/2，2.02 秒，VUID 0；原生 controller 另依下方 fixture 驗證。
- 全新 reviewer `task5a_controller_pool_review_1`：0 Critical／0 Medium。核對 partial construction、same-queue ImGui rotation、獨立 proofs、ReleaseRetained、reset failure、bounded accounting 與文件。
- Staged allowlist 為 `src/desktop/desktop_host.cpp`、`docs/architecture/controller-vulkan-resources.md`、`docs/architecture/shared-vulkan-renderer.md`；tree `cc0fa79eb5eb52304fe12c1803dd94566f08609d`。
- 精確 source snapshot：`artifacts/vulkan-perf/task5a-pool-20260916/source`。Production／diagnostics／source-boundary 獨立 build 通過。工作樹 tests、spec、plan、verification、artifacts 維持 unstaged。

Logs：`artifacts/vulkan-perf/task5a-20260916/{full-build,full-ctest,validation-357,snapshot-build}.log`。

### 原生 UI 與 validation

固定 Task 2 acceptance source SHA256 `92A0EFCEA6D3F3C7A6E1452801C0FFA1D4CA7DF7E61D0429B896AE901DB890CA` 連結 snapshot archives。前兩次 background Start-Process launch 分別在 checkbox／opacity 導覽失去 foreground；JSON 顯示停留在導覽 phase 0，host/source 正常退出、VUID 0。這些失敗保留於 `acceptance-native`、`acceptance-native-retry`。

Root 改以直接 cmd 呼叫維持相同互動程式，先跑既有 C4 control，再跑 5A candidate。兩者均 15/15 完成；candidate 的 `acceptance-native-direct` loader log 確認 isolated 1.4.357.0 layer、VUID 0，host/source exit 0。這證明 candidate 在成功取得前景的同一刺激下通過驗收；先前失敗列為 launch／foreground 環境限制。

容量證據：C=3、proof-held submissions=3；UI frame 308→312 且 checkbox 變更成功，submission 保持 308。釋放 proof 後 submission→311；三秒防護 timeout 前完成釋放。另涵蓋 native mouse drag、double-click／numeric edit、hotkey recording／focus cancellation、synthetic DPI resize、hidden input、minimized replica progress、restore 與 Quit drain。此 fixture 使用 test-owned windows 與合成輸入；physical monitor-DPI migration 維持另列範圍。

### Allocation checkpoints

B1 shipping executable SHA256 `E187A202831BDC6765A3CE9C5343C0F0EC0557D111471321F04DDF6D28A586C3`，10 秒暖機＋60 秒量測：598 UI builds／submissions／present retirements，六項 create／destroy counters 均 0。High-water 為 3 command buffers、9 fences、6 semaphores。520×720、FIFO、fresh config、validation disabled；complete／drain complete、exit 0、stderr 0。UI build p50/p95 = 0.1114／0.3192 ms。報告位於 `task5a-pool-20260916/checkpoint/B1/B1.json`。

B2 固定 Task 1 fixture binary SHA256 `C9E795011992B0A82502080DFCC36EA7F109FDC1E56E8F277E93D2BB414B426A`；controller 同樣 598 builds／submissions／retirements，六項 native counters 0，high-water 3／9／6。Fixture CPU 2.2635% of one logical CPU，static-source WGC presented 120，1.9981 FPS；source 配置與本輪 C4 static checkpoint 相同。Collector／fixture complete、drain complete、host/source exit 0。Report hash `F28A39EB552FB66FD3538155ED8B8764D06F948DEA06237CE9B94455407DB8D8`。

5A commit `5317b184b217f27e936fc651e594237fde97c5ce`，三檔 361 insertions／151 deletions。Root 再次核對 staged tree 與驗證 snapshot 相同，其他 dirty files 維持工作樹。

## 5B：Backend／pool capacity migration

Implementation agent `task5_controller_pool_impl` 以 `progress_backend_capacity` 銜接既有 WSI progress。Desired capacity 取自實際 swapchain images；mismatch 關閉 graphics admission，UI／Application 與 proof observation 持續。舊 frame、present、font users 全部退休後，拆除 Vulkan backend／font descriptor 與 pools，reset host 專用 descriptor pool，依新容量初始化 backend／pools，再綁定保留的 font image，最後發布 capacity。Context／GLFW callbacks 與 owner-wide serial 保留。

Root 直接檢查 pinned backend：Init 每次建立兩筆 sampler descriptor sets，而 Shutdown 對 host 提供的 descriptor pool 保留該配置。因此在全數完成 proof 後 reset 專用 pool；依 [vkResetDescriptorPool valid usage](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetDescriptorPool.html)，此處先完成全部 descriptor users，再回收 sets。Startup partial-prefix cleanup 與 runtime exception 的既有 non-unwinding firewall 延續原責任。

- Policy 的同一 SECTION 增加 3→4→2→4→3 migration／stale-key 資料；case 07／08 通過 2/2。
- Final desktop SHA256 `797722F7E999A2778256B5FD72B87840A77F78F79D15EB5FDEF036A6D4F91CB3`；controller doc `8D8F2D5F5C7418BCA9691284224C352FE9A1DE3DD31110097F50C525D1A50C99`。
- 全新 reviewer `task5b_capacity_review_1` 對 Task 5（5A＋5B）完整檢查：0 Critical／0 Medium。Root 同步查核 admission、proof、descriptor pool、font、partial failure 與 serial 路徑。
- 兩檔 staged tree `2621aff1336f29c4ca8d813c5876114b1b6754c9`；精確 snapshot 位於 `artifacts/vulkan-perf/task5b-capacity-20260916/{source,build}`，production／diagnostics／boundaries build 通過。
- 完整 regression targets build 通過。此工作樹另含 Task 6 prepared changes；5B 精確 snapshot 與 commit 排除 Task 6。

Root full CTest 15/15、hardware 3/3，4.07 秒；inventory 15。Cases14/15 isolated validation 2/2，1.96 秒；指定 DLL loader evidence 成立，VUID0。Logs：`artifacts/vulkan-perf/task5b-20260916/{full-build,full-ctest,validation-357}.log`。

### 真實 image-count migration

Artifact agent `task5b_capacity_acceptance_fixture` 建立單一真實 DesktopHost 的 bounded resize loop。Loader wrapper 先讀真實 surface capabilities（本機 min2／max8），接著把真正 `vkCreateSwapchainKHR` 的 minImageCount 改成 3／4，回讀原生 image count。Root 檢查 forwarding、pool 身分、font descriptor observation、命令及結束路徑。

最初 fixture 將 ImGui 自有 texture command buffer 計入 frame capacity，root／agent 將 owner scope 修正為 host 首先建立的 command pool，另外保留 all-command total。`red.json` 為該 observer 校正紀錄；產品對照採 `red-pool.json`。

- Frozen5A RED：real image count=4、host commands=3、samplers cumulative=2，持續呈現108次；8秒 capacity gate 精確失敗。Host正常退出／資源全回收；exit40 代表缺少 migration，shader／呈現維持有效。
- Frozen5B GREEN：initial＋20次3↔4遷移，21 checkpoints與21真實swapchains全部通過；host command capacity 每次等於實際image count。
- 73 host command buffers created/freed、42 samplers created/destroyed；70 submissions、presents與font descriptor binds。維持同一ImGui context與font atlas image view，Application設定命令持續生效。
- QueueWaitIdle／DeviceWaitIdle均0；teardown後all-command與sampler live均0。Root重新讀取RED／GREEN JSON、hash與layer logs，兩者isolated1.4.357DLL載入、VUID0。
- Fixture SHA256 `25858439718983AF695C24B675F93665764CD112C3F41BDDFC2A035D170A6FFE`；GREEN exe `2CCB968B7391B50FE2460D9C8C04D915036EC42C60C35995CF4AE078F5D6AFDB`；GREEN report `FC6C7AD2859C1E07D3EED46367B521CEB2A29875475B63C1C653F70CD848D859`。

資料位於 `artifacts/vulkan-perf/task5b-capacity-fixture-20260916/{red-pool,green}.json`。此驗證觀察真實GPU操作、字型image/descriptor連續性；pixel OCR與physical monitor移動屬另列範圍。

Root 另將固定native UI fixture連結5B精確snapshot，15/15操作完成，host/source exit0，isolatedvalidation VUID0；包含proof-held capacity下UI輸入、synthetic DPI/resize、minimize/restore與Quit drain。結果位於 `task5b-capacity-20260916/acceptance-native/`。

5B commit `6643827c0dacfb0102272981b809f981e2213a97 fix(desktop): migrate controller pools with swapchain capacity`，兩檔126 insertions／30 deletions；最終index tree與驗證snapshot相同。依使用者本輪停止指示，Task5完成後準備handoff，Task6 prepared changes繼續留在工作樹。
