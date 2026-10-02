# Vulkan performance Task 3 實作驗證

日期：2026-09-16，Asia/Taipei。起點 `01c32d6`，repo `D:/document/MesRepo/Airelinna`。

## 邊界分類階段

目的：讓共用 Vulkan 的資料契約與 Windows 擷取政策分別位於自己的責任層。Presentation structs、SlotKey／FrameStamp／CPU rows／external image／completed wait 組成共用輸入值；Windows bridge selection、D3 loss／completion 與 capture recovery 位於 platform/windows/detail。

Production commit `bbbc3c8`，staged tree `36958250fb5d4ae7964603985ba05a7ef3c067f8`。13 files，290 insertions／147 deletions；兩份 bridge_policy 檔案以 rename 保留。原 graphics_retry_policy.hpp 的其餘註解編修保持 unstaged。

- 全套 build 成功；15/15 cases，hardware 3/3，CTest 3.97 秒；inventory 15，source/target boundaries 通過。
- 第一輪全新 reviewer `task3a_boundary_review_1`：0 Critical／1 Medium。Facade exception 使用 basename，會接受巢狀同名檔案。
- Root 以 boundary-negative fixture 重現：原 checker exit 0。修正為 relative source path 的 anchored exact-file exception 後，negative exit 1、positive exact facade exit 0、實際 tree exit 0。
- 第二輪全新 reviewer `task3a_boundary_review_2`：0 Critical／0 Medium，獨立比對 declarations／function bodies，重跑 checker 正向／反向資料。
- `artifacts/vulkan-perf/task3-contracts-20260916` 匯出精確 index，使用自己的 first-party source 建置 Release executable、diagnostics 與 source boundaries，exit 0。

## FrameStamp 與 ring identity

目的：在後續 owner handoff 中保留來源觀測時間與已接納順序，並以每次 installation 的 identity 驗證槽位。

FrameStamp 取代 monolith 的兩個同義欄位，所有 selection／retained redraw／intake-to-submit measurement 保留原使用時機。`next_ring_serial` 保留零為 detached，溢位回傳 empty optional；`matches_slot_key` 檢查 active serial、identity 與 index bounds。Concrete phase validation 與真正跨 owner handoff 排在 producer integration 階段。

- Case 08 同一新增 SECTION：首次缺 symbol 編譯失敗；最小 stub 成功編譯後，兩個 successor 與兩個 valid-key assertions 產生 runtime RED。
- 真實 implementation 後 case 08 GREEN，0.08 秒；測試 inventory 維持 15。
- FrameStamp 接線後 production、policy、graphics hardware targets 與 boundary 通過；cases 08／14／15 為 3/3，1.90 秒。
- 最後全套 CTest 15/15、hardware 3/3，4.04 秒；inventory 保持 01–15。Task 2 十二份 baseline/candidate report fingerprints 再次核對相符。
- 全新 reviewer `task3a_stamp_identity_review_1`：0 Critical／0 Medium，核對 primitive 的邊界與完整 FrameStamp 機械差異。`task3-stamp-identity-20260916` 的精確 staged tree `54ce404c146e3039f771be805d9d33307a626725` 建置 executable／diagnostics／boundaries 通過。Production commit `c017a40`，2 files，34 insertions／9 deletions。

## WindowsFrameBridge 草稿與安全審核

為了將原生 producer proof 集中於 Windows 層，草稿 `platform/windows/windows_frame_bridge.hpp/.cpp` 定義 D3 device/context、三個 producer records、query/fence proof、WGC checkout、同步 Map consumer 與 bounded completion batch。獨立 clang-cl 語法檢查通過。這兩份檔案目前保留於工作樹，production target 與 monolith 維持既有擷取路徑。

工具的自動安全審核擋下兩次 native integration patch：第一次為整檔形式的大幅 patch；第二次為會暫時留下部分 API 尚待接線的中間步驟。兩份 patch 皆維持拒絕狀態，root 保留原 native control flow，後續接線需要新的核准。原始碼草稿可供核對設計與生命週期；整合後 build、hardware、rollback／loss review 與 Task 3 B2–B4 measurement 屬接續工作。

## Native integration 續作核准

使用者在上述安全審核說明後明確回覆「ok繼續」，核准繼續 D3 資源與同步生命週期搬移。續作沿用同一 branch、明確檔案責任、分段 commit、全新 reviewer 與實際 CPU/GPU 硬體驗證。

Root 在既有 case 14 加入同 generation 的真實 producer ring 7→8 replacement：stale release／create、index 3 越界、prepared slot 重複 query、serial reuse 與 cleanup 後 pending 狀態。首次 build 的 RED 為 WindowsFrameBridge 尚待 target 接線的七個 undefined symbols，log：`artifacts/vulkan-perf/task3-20260916/producer-contract-red.log`。整體 inventory 保持 15。

Tests、spec、plan、verification 與 artifacts 保存於工作樹。

## Producer ownership 接線完成

Production commit `e6266a0`，staged tree `5aa4aab629b88bc2ce0be4896e61f69f317459b1`。六個 production／architecture 檔案，1103 insertions／279 deletions。

- Bridge 已進入 production target：D3 device/context、checkout、query／fence proof、staging Map／Unmap 由 bridge 管理；renderer-role caller 持有 canonical phase、CPU upload 與 Vulkan submission／presentation。
- 既有 case 14 的 native ring 7→8 contract 轉為 GREEN。完整 build、source boundaries、15/15 CTest 與 inventory 通過；hardware 3/3，CTest 4.09 秒。Logs：`artifacts/vulkan-perf/task3-20260916/producer-build.log`、`producer-ctest.log`。
- 全新 reviewer `task3b_producer_review_1`：0 Critical／0 Medium。核對 Signal failure 的 query proof、query error 的 checkout 保留、CPU consumer／native error provenance、same-generation serial、retained invalid-input preservation、partial rollback 與 ForceCpuBaseline 的實際 handle probes。
- 精確 index 匯出至 `artifacts/vulkan-perf/task3-producer-20260916/source`，獨立建置 executable、diagnostics、source boundaries 通過，exit 0。
- `ExternalCopyBorrow` 屬於此 increment 的暫時介面；external import owner 遷移接續進行。Native failure 分支的驗證以原碼因果審查為主，hardware cases 涵蓋當前主機可執行的 CPU／GPU 路徑。
- 固定 Task 1 fixture source 連結上述 snapshot archives，短版 B2／B3／B4 三秒 smoke 皆 exit 0、fixture complete、drain complete、stderr 0 bytes；presented 分別為 1／148／148。Binary SHA256 `AEAC7705D4439311C297C66581D588A6821C06AA486EAA8F9F113C8A9D368217`。Results：`artifacts/vulkan-perf/task3-producer-20260916/smoke/*-verified/`。這組 smoke 的 collector window 長度低於正式 10+60 秒，用途為功能與關閉驗證。最初 B2 runner 使用 WaitForExit 前缺少 process handle，退出碼回傳 null；該次 sidecar 正常完成，後續修正 runner 並以全新路徑重跑。

## Renderer handoff 契約釐清

對照 `e6266a0` 時確認：source validation／texture projection 失敗發生於 adoption 之前，原路徑保留 retained redraw；成功 adoption 的 SafeDiscard 則淘汰該 slot 舊 retained 內容。Spec §4.2 與 Plan 3C 加入單一 reservation rollback marker 與 cancel／discard 的分流，CPU completion 直接驗證交付 metadata，完成 row copy／flush 後提交 readiness。

全新 reviewer `task3c_contract_clarification_review` 先核對 spec，再核對 plan 與 immutable HEAD 的行為：0 Critical／0 Medium。此結果驗證契約釐清；後續 typed transition 實作仍需通過自己的 tests／review。

## External import ownership 接線完成

Production commit `a547a49`，staged tree `2b303ccdbcb5dad5be43d935d916aaa7e882cc90`。五個 production／architecture 檔案，977 insertions／721 deletions。

- Bridge 接管 shared texture／fence、NT handles、Vulkan image／view／memory／timeline 與 Win32 capability probes。Renderer-role caller 保存 borrowed image／completed wait，startup request 與 device borrow 使用共用 Vulkan header。
- Root 在 case 15 加入安全 capture reconfigure → ring replacement → 新 generation frame retirement。先連結固定 `e6266a0` archives 執行 characterization，118 assertions 通過；imports 遷移後 cases 14／15 通過 2.01 秒。
- Root 確認實作者提出的 allocation-error／cleanup-error precedence 問題，保留先前已觀察的 `VK_ERROR_DEVICE_LOST`；完整重建後 15/15，hardware 3/3，4.01 秒，inventory 15。Logs：`import-build-final.log`、`import-ctest-final.log`。
- 全新 reviewer `task3b_import_review_1`：0 Critical／0 Medium；包含 ForceCpuBaseline 真實 probes、GPU ring replacement support 保留、capability facts、descriptor detach／reset failure、partial teardown、timeline counters 與錯誤 precedence。
- `artifacts/vulkan-perf/task3-import-20260916/source` 的精確 staged snapshot 獨立建置 executable／diagnostics／boundaries 通過，exit 0。
- 下一個 increment 建立 canonical typed handoff；完整共用 Vulkan renderer、Task 3 正式 measurement 與最終 ownership review 接續進行。

## Construction guard 前置契約

新增本機 `graphics_construction_guard.hpp` 與 case 08 一個 SECTION，採固定 ledger 驗證 partial bootstrap／ring prefixes、detach → bridge → renderer 順序及 disarm。獨立 harness 先以 stub 得到 runtime RED（exit 1，calls=0 expected=3），正式 guard GREEN；detach-failure terminate handler 的預期 exit 為 73。輸入與 logs 位於 `artifacts/vulkan-perf/task3-guard-prerequisite-20260916/`。

Guard 尚待 production bootstrap／ring operations 接線，保持 unstaged；case 08 已隨上述 full suite 通過，總 test cases 維持 15。

## Import commit checkpoint 與延遲警戒

固定 `a547a49` snapshot archives 連結相同 Task 1 fixture source。Binary SHA256 `A0460D512645168D178CA48E3A247CB0455BBE41373AF27533F275C37628D35D`；`task3-import-20260916/checkpoint/` 的 B2／B3／B4 各完成一次 10 秒暖機＋60 秒量測，exit 0、collector complete、drain complete、stderr 0 bytes。Power plan 現查為 Balanced `381b4222-f694-41f0-9685-ff5bb260df2e`，報告 GPU／raw driver 與基線一致。

| Checkpoint | One-core CPU % | Presented FPS | UI builds | Intake-to-submit p95 ms |
|---|---:|---:|---:|---:|
| B2 | 1.4837 | 0 | 599 | null |
| B3 | 3.8790 | 47.3349 | 0 | 5.6502 |
| B4 | 7.2860 | 48.6457 | 0 | 5.5549 |

B3 collector admitted 2842／submitted 2841／present-retired 2841／dropped 1；B4 為 2919／2919／2919／0。B4 的 D3 copy、CPU copy、CPU clear、Vulkan upload bytes 各為 `2919 × 1920 × 1080 × 4`，符合目前完整 allocation clear 路徑。Replica semaphore／fence 仍逐次呈現配置，Task 4 處理其重用。

相較早先 Task 1 三次基線，B3／B4 的 latency 觸發警戒。Root 隨即驗證固定 Task 1／Task 2 binary hashes，以當前環境各重跑 B3（`latency-diagnosis/`），兩份 report 均完整：

| 固定版本 | One-core CPU % | Presented FPS | p95 ms |
|---|---:|---:|---:|
| Task 1 | 12.8259 | 47.8527 | 4.2193 |
| Task 2 | 4.7640 | 47.5166 | 5.0344 |

目前資料同時呈現環境時段差異與 cadence 後的 latency 差異；單次順序量測支援後續定位，Task 7 的交錯三次 baseline／candidate 量測持續追蹤這個警戒。這些 checkpoint 尚屬中間 commit，完整 Task 3 與最終效能驗收接續完成。

## Typed handoff 與 construction guard 接線

七項 typed transitions 已進入 canonical slot 呼叫點：reserve、cancel、registration、CPU completion、external completion、discard 與 serial-aware detach。CPU metadata 在 copy／flush 成功後發布；adoption 前 cancel 恢復符合條件的 retained reference，adopted SafeDiscard 使用獨立 discard。Bootstrap／ring operations 共用已測試的短小 guard，ring ready state 在安裝完成後發布。

全新 reviewer `task3c_handoff_review_1`：0 Critical／0 Medium。Root 獨立核對 retained restoration、completion metadata 與 detach-before-release。Device-loss reset 疑點經 Khronos 官方 [vkResetDescriptorPool return codes](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetDescriptorPool.html) 與 [lost-device contract](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device) 排除：reset 的合法錯誤碼集合與可回報 device loss 的 commands 分別定義，現有有效 lifetime cleanup 成立。

完整 build／boundary 成功，log `artifacts/vulkan-perf/task3-20260916/handoff-build.log`。目前 working tree CTest 15/15，hardware 3/3，4.07 秒；inventory 15。Exact staged tree `f33806480531b244e605fc315db63936b0bfafef` 匯出至 `task3-handoff-20260916/source`，獨立 executable／diagnostics／boundary build 成功，exit 0；四個 production／architecture 檔案進入 `e56a8d3 refactor(render): make source handoff and rollback explicit` commit。

## Shared owner 拆分進行中

Root 先收緊 render source 邊界，舊 monolith 的平台 include 觸發預期 RED exit 1，log `task3-20260916/shared-boundary-red.log`。四個限定 fixture 驗證 neutral Vulkan header 通過，platform include／HRESULT／Win32 Vulkan import type 各自失敗，位於 `task3-boundary-20260916/`。Target allowlist 將 shared render 與 Windows graphics 的 source roots 分離，desktop 明確連結共用 policy owner。

既有 case 14 補上真實 VulkanRenderer 安裝、partial publish、CPU completion、stale key／serial 與 safe detach 契約；維持原 test case inventory。此時 implementation 尚在進行，以上新增驗證等待完整 owner 接線後執行。

Owner 接線後初次 cases 14/15 通過 2.10 秒；root 於完整原碼追蹤確認一個 terminal-drain gate 問題：producer terminal 之後，healthy ReleaseRetained 仍需要 WSI draining 分支合法銷毀 swapchain 以釋放 image ownership。Facade 的 recovery admission 修正為 `!interrupted && (draining_ || !terminal_error_)`，維持同 pass exception short-circuit，同時開放後續 drain cleanup。此異常 native 分支使用原碼因果審查；當前硬體 cases 驗證可執行的正常與 resize／drain 路徑。

Working-tree build／boundaries 通過，CTest 15/15、hardware 3/3，4.09 秒，inventory 15。Logs：`shared-owner-build-final.log`、`shared-owner-ctest-final.log`。新公開 header 的獨立 C++23 `/W4 /WX` syntax check 通過，位於 `task3-boundary-20260916/neutral-header.log`。Exact staged tree `a719aaa036af944e5ce5aa5f4f5adc06df5221eb` 匯出至 `task3-shared-20260916/source`，獨立 build 成功；全新 reviewer `task3c_shared_owner_review_1` 回報 0 Critical／0 Medium。

## Acquire proof 與 validation 版本核實

額外 correctness run 使用既有 SDK 1.4.304.1，cases 14/15 通過時仍產生 `VUID-vkResetFences-pFences-01123`。Root 依 Khronos [signal operation ordering](https://docs.vulkan.org/spec/latest/chapters/synchronization.html) 核實根因：acquire fence signal 位於同次 semaphore signal 之後，等待 semaphore 的 submission fence 與 acquire fence 具有各自完成時間。Replica retirement 與 controller frame retirement 都補上 acquire fence observation，再 reset／destroy frame owners。NOT_READY 沿用既有 poll deadline。

修正後以舊 layer 重跑，`01123` 歸零；external timeline 仍有兩筆 `03268`。全新診斷 agent `task3c_external_wait_diagnosis` 找到 Khronos [issue #10211](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/10211) 與 [commit 8f3509a](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/8f3509a0aee2533c249374989d5dbcfd09723aca)。Root 透過官方 API 直接核對 patch：layer 的 dependency scan 改以 external scope 區分其可觀察的 signal records。Production 保留 D3 Signal／Flush／GetCompletedValue proof、imported timeline wait 與 binary present wait。

隔離驗證使用官方 SDK 1.4.357.0 archive 的兩個檔案：`VkLayer_khronos_validation.dll`、JSON manifest。Archive Authenticode 為 Valid，signer LunarG, Inc.，SHA256 `81F474711E9042F4CD22B31B2F7A8870DB2E428B21586FB43DD80150BE97310D`。檔案置於 `artifacts/vulkan-perf/validation-layer-1.4.357.0/extracted/Bin`，process-local `VK_LAYER_PATH`／`VK_INSTANCE_LAYERS` 控制測試。既有 SDK、registry 與全機 PATH 保持原設定。

- 新 layer cases 14/15：2/2，2.09 秒，VUID 0，loader log 顯示上述 isolated DLL 實際載入；`shared-proof-validation-357.log`。
- 固定 Task 1 fixture source 連結目前 production archives，B2 controller／replica 三秒 smoke：exit 0、fixture complete、drain complete、VUID 0；`controller-validation-357/`。其 collector measurement window 屬短測，`collector_complete=false` 按 smoke 定義記錄。
- 完整 rebuild／boundaries：成功；CTest 15/15、hardware 3/3，4.30 秒，inventory 15；`shared-proof-build-final.log`、`shared-proof-ctest-final.log`。
- 全新 reviewer `task3c_shared_owner_review_2`：0 Critical／0 Medium；核對 ownership、rollback、loss／terminal drain 及兩處 acquire proof。文件補上雙 fence retirement 條件。
- 最終 staged tree `d500df3bc5513827c39739c38486e5a0261d5609` 匯出至 `task3-shared-final-20260916/source`，獨立 executable／diagnostics／boundary build 成功，exit 0。C2 commit `3b68ede refactor(render): establish shared Vulkan renderer ownership`，11 個 production／architecture 檔案，1867 insertions／1331 deletions。

## Device translation unit 搬移

C3 將同一 `VulkanRenderer::Impl` 的定義移入 `render/detail/vulkan_renderer_impl.hpp`，並將八個 device／common-resource methods 搬入 `render/vulkan_device.cpp`。Root 以固定 C2 snapshot 逐個比較方法 bodies，八者在 whitespace normalization 後完全一致。Constructor、aggregate teardown、canonical transitions、WSI 與雙 fence proof 保留既有執行順序。

- Working-tree build／boundaries 通過；15/15 CTest、hardware 3/3，4.03 秒；`device-build-final.log`、`device-ctest-final.log`。
- SDK 1.4.357.0 隔離 layer correctness run：cases 14/15 通過，2.00 秒，VUID 0；`device-validation-357.log`。Inventory 保持 15。
- 五檔 staged tree `c0dee9d8dce1b3d3b993272c1e1f2550270ff319` 匯出至 `task3-device-20260916/source`，獨立 executable／diagnostics／boundary build 成功，exit 0。
- C3 fresh reviewer 建立遇到工具 thread 上限；既有 agents 已完成仍佔用 thread，當前工具提供 interrupt 與列舉，缺少 close/delete 介面。Root 保留 staged tree，review gate 通過後提交。Task 3 整體、C4 與 Tasks 4–7 持續列為待完成。

使用者續作後，全新 reviewer `task3c_device_review_retry` 成功建立，完整 staged increment review 回報 0 Critical／0 Medium／0 Low。Reviewer 獨立確認八個方法與兩個 helper 原文一致，Impl 成員順序／初始化保持，renderer 剩餘方法保持原樣，ODR／CMake／文件符合分層。Root 重跑完整 build／boundaries、15/15 tests（hardware 3/3，4.13 秒）、新版 layer cases 14/15（2/2，2.10 秒、VUID 0）與同一精確 snapshot build，全部通過；logs：`device-review-resume-{build,ctest,validation,snapshot}.log`。

C3 已提交為 `972c804 refactor(render): isolate Vulkan device methods`，對應上述五檔 staged tree。

## WSI translation unit 搬移與 Task 3 收尾

全新實作 agent `task3c_wsi_extraction` 將 19 個 WSI／recovery methods 搬入 `render/vulkan_wsi.cpp`，其餘 49 個 Impl methods 保持在 renderer。Root 從 C3 精確 source snapshot 重新擷取、比對全部 68 個 definitions，signature 與 body 原文一致；兩個共享 helper 移至 private Impl header。`try_present` 保持完整，單一 owner 與各 proof 的執行次序保持原樣。

- Working-tree executable／test targets／boundaries build 通過；CTest 15/15、hardware 3/3，4.18 秒，inventory 15；`wsi-build-final.log`、`wsi-ctest-final.log`。
- SDK 1.4.357.0 isolated layer cases 14/15：2/2，2.02 秒，VUID 0；loader 確認載入指定 DLL，`wsi-validation-357.log`。
- C4 五檔 staged tree `dc89ae7dc049fee6700a3234a2814efa0f3d65b0` 匯出至 `task3-wsi-20260916/source`；獨立 executable／diagnostics／boundary build 成功，exit 0，`wsi-snapshot-build.log`。
- 全新 reviewer `task3c_wsi_full_review` 同時審查此 increment 與 Task 3 整體 ownership／boundary，0 Critical／0 Medium。Reviewer 獨立核對全部 68 個方法，以及 ring identity、rollback、independent loss、雙 fence proof、ReleaseRetained drain 與 target/source boundary；source scan exit 0。Native device-loss／allocation-failure 分支以 policy tests 和因果 code review 為證據，實體裝置故障注入屬後續驗證範圍。
- 固定 Task 1 fixture source SHA256 `C01D4D84531D89CF6D4CBE78C4A7F41CCE0220AF90220357A41A56444491D3D0` 連結 C4 snapshot archives；候選 binary SHA256 `ED2C17497994E426965C8EB3EF697EAB73B103BFBD5E6153BE0CA0C0B4C2C469`，build log `wsi-fixture-build.log`。B2/B3/B4 checkpoint 各完成一次固定 10 秒暖機＋60 秒量測，fixture／collector complete、drain complete、exit 0、stderr 0 bytes。

| C4 checkpoint | One-core CPU % | Presented FPS | UI builds | Intake-to-submit p95 ms |
|---|---:|---:|---:|---:|
| B2 | 2.4729 | 1.9992 | 598 | 3.2113 |
| B3 | 3.9841 | 47.5633 | 0 | 5.1247 |
| B4 | 8.2796 | 48.8905 | 0 | 5.3495 |

B2 source report 為 paint_calls=1、scheduled_updates=0；本次 WGC admitted／submitted／present-retired 各 120，與先前靜態 checkpoint 的 0 幀不同。這筆數據保存為實際本次工作量，Task 7 採同時段配對判讀。B3 collector 為 2859／2854／2854／5（admitted／submitted／retired／dropped）；B4 為 2933／2933／2932／0，量測窗口末端跨界的退休與 fixture 的完整 drain 分別記錄。B4 copy／clear／upload 各 24,327,475,200 bytes，仍為 full allocation 路徑。

測量期間保持 build／hardware tests 暫停；電源 Balanced、RTX 3070 Ti、raw driver 2559541248 與既有 checkpoint 相符，explicit validation layer 關閉。先前 p95 警戒持續列為 Task 7 交錯量測與定位項目。

C4 commit `e27448f refactor(render): isolate Vulkan presentation methods`，五個檔案、1072 insertions／1034 deletions，主要為既有方法群搬移。Tests／spec／plan／verification／artifacts 持續保留工作樹。

提交後重用既有 Task 2 `acceptance.cpp`，經 `shared-controller-fixture-20260916` 僅更新 archive link graph，連結同一 C4 snapshot。`task3-wsi-20260916/acceptance-native/` 通過 native 15/15 checks、host/source exit 0、stderr 0。三筆 controller submissions 的成功 proof 暫存期間，UI frame 291→295 且 checkbox command 生效，submitted 保持 290；釋放後 submitted 293，早於三秒界限。Synthetic DPI callback 1.5 scale、resize、hidden sync、minimize／restore／Quit drain 均通過。Hardware 執行時序與 Task 4 的 build／tests 明確協調；真實 HID 與物理 monitor DPI migration 持續另列範圍。
