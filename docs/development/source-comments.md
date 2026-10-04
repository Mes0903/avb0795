# 原始碼註解：契約、原因與使用界線

[架構索引](../architecture/README.md) · [clangd 與 hover](clangd.md)

讀註解應能回答：這個單元負責什麼、怎麼正確使用、什麼時候不能使用，
以及為何保留這個限制。註解描述目前實作；不替未實作的功能、尚未完成的
工作或未量測的效能背書

## 1. 先說明語意差異，再解釋機制

[語言政策](../../include/avb/i18n/language.hpp) 有兩種容易混淆的輸入：
系統語言 tag 和設定檔 token。前者做語言家族匹配；後者只接受精確的
`system`、`en`、`zh-TW`、`ja`。註解需要把這個差異寫出來：

```cpp
/// @brief Parse only the exact persistence tokens system, en, zh-TW and ja.
/// Matching is case-sensitive and does not trim whitespace or accept family aliases.
/// Invalid input returns no value so the persistence layer can choose its recovery policy.
```

這段讓呼叫端知道哪些輸入有效、失敗怎麼表示，以及 fallback 不由 parser
決定。不用再替 `token` 寫一行「the token to parse」

系統 tag 的匹配則要說明：接受的 `zh` 家族都使用目前提供的繁體中文 catalog，
不會依 script／region 切換成另一份中文翻譯目錄；小型語法檢查也不是完整的
language-tag validator。這些是產品政策，不能只從函式名稱推知

## 2. Declaration 放契約，implementation 放原因

公開 header 是使用入口，說明責任、允許狀態、輸入語意、結果與副作用
實作內的註解解釋難以直接看出的原因，例如先保存 owner 再提交原生工作、
為什麼不能在錯誤路徑直接釋放資源、為什麼新的 UI command 要跳過本輪 wait
不要把 header 的整段文字再貼到 definition

[DesktopHost](../../include/avb/desktop/desktop_host.hpp) 的結果契約不只
是「回傳 exit code」：`abandon_process` 決定能否正常展開 owner stack
非零 exit code 不代表不能清理；要求 abandon 時，即使只想回報錯誤也不能
先讓區域物件解構

相對地，[主迴圈實作](../../src/desktop/desktop_host.cpp) 解釋 command 為何在
下一輪處理，以及為何不能只拿 submission fence 當成 presentation 已完成
這些說明緊貼作出決定的位置，讓改程式的人看得到不可省略的理由

## 3. Ownership 註解要寫出時間界線

[WgcCaptureJob](../../platform/windows/wgc_capture_job.hpp) 的生命週期需要兩種
證據：內部 callback／mailbox／native cleanup 已完成，且已交出的 frame 和
其 GPU 使用都已退休。只讀 `stop_quiescent()` 的名字，容易誤認為它同時證明
外部使用者都已結束；因此註解明寫範圍與呼叫端義務：

```cpp
/// Internal quiescence covers callbacks, mailbox, and native cleanup;
/// it cannot count checkouts already transferred to external consumers.
/// @pre A published job has stopped, stop_quiescent() is true, and all
/// externally held frames and their GPU uses have retired.
```

這是契約摘要；完整條件以該 header 和實作為準。對不同 C++ 機制，應寫的資訊也不同：

- `unique_ptr`／move-only value：何時轉移所有權、失敗或拒收時是否仍消耗輸入
- reference／`span`／`string_view`：借用誰的儲存、可用到何時、是否允許修改
- callback：在哪個執行緒呼叫、借用目標如何存活、何時才可停止或銷毀目標
- mutex：哪些操作受保護、可能呼叫使用者程式的 move／destructor 是否在鎖內
- `std::expected`：哪些失敗變成 error value，是否有部分修改或 rollback 限制

`noexcept` 只描述例外不會離開函式，不等於成功、不阻塞、不配置記憶體、
thread-safe 或可安全解構。例如 [mailbox](../../include/avb/core/latest_value_mailbox.hpp)
明確區分「停止接受新值」與「所有並行呼叫都已結束」

### Linux 來源與 retirement 的 unit 範例

[X11Capture](../../platform/linux/x11_capture.hpp) 的註解集中解釋 inactive preparation、private checked connection、exact revision ACK、最多一個 native reply，以及 callback／worker 完成前保留 owner 的原因。 [LinuxRuntimeFactory](../../platform/linux/linux_runtime_factory.hpp) 說明 catalog／wake 借用與完整 preparation 後才能 activation；[runtime](../../platform/linux/linux_replica_runtime.hpp) 把 publication、delivery stamp、retired presents 與 aggregate Stopped 分開。 讀者能由這些契約判斷可否接受影格、啟動或釋放資源，無須逐欄翻譯型別

## 4. Doxygen 格式保持簡單

第一方 C++ 使用英文，避免每段中英並列造成重複維護。檔案說明分開寫：

```cpp
/// @file
/// @brief Controller-window infrastructure and the main-thread application loop.
///
/// This boundary owns the controller's presentation resources, not the replica's.
```

不要把描述誤放成 `@file` 的檔名參數。實質 type／API 使用 `/// @brief` 和
必要段落；implementation 的 WHY 使用普通 `//`。已有準確且充分的註解
可以保留，不需要為了統一外觀而改寫

`@pre`、`@post`、`@return`、`@throws`、`@note`、`@warning` 只在帶來明確
使用資訊時使用。參數有非顯然的單位、合法範圍或生命週期才值得特別解釋
避免以下只有重述名稱、沒有契約的內容：

```cpp
/// @param path The path.
/// @return The result.
```

不用替 deleted copy/move members、預設 destructor、每個 enum 值或 accessor
湊註解數量。需要連在一起理解的資料欄位，優先說明整組 invariant

## 5. Lua、CMake、shader 使用各自的語法

[Lua inventory tool](lua-tooling.md) 使用 `--` 說明模組輸入、diagnostics、
exit categories 和 metadata-only 限制。它不是 application runtime、build system，
也不能證明測試已通過。CMake 使用真正位於字串外的 `#` 註解；shader 使用
`//` 說明 stage、座標與格式假設，不把它們假裝成 C++ declaration

CMake 的 quoted／bracket argument 可能包含要產生的 C++。例如
[EmbedSpirv.cmake](../../cmake/EmbedSpirv.cmake) 的 `header_contents` 是輸出內容，
其中看起來像註解的文字也屬於字串資料，不是可以隨意改動的 CMake 註解

## 6. 審查時核對的事情

先對照 definition、caller 與既有測試，確認敘述是真實契約，再檢查：

- 是否說清楚 responsibility、ownership、失敗與重要限制，而非翻譯名稱？
- 是否誇大同步、rollback、completion 或效能保證？
- 是否重複別處已有的說明，或把未來設計寫成現有功能？
- 是否只改了註解，沒有順手改 include、literal、directive、tool pragma 或邏輯？

[Source-boundary checker](../../cmake/VerifySourceBoundaries.cmake) 也會掃描
註解。跨層說明使用角色用語，不把禁止出現的 native type／owner／include
名稱塞進 portable 層，也不為註解放寬 gate

驗證要比較修改前的 working-tree snapshot，包含未追蹤來源；不能拿較早的
HEAD 當成本次改動的起點。C++／shader token 比對仍需搭配 directive 與
line-sensitive 檢查；Lua／CMake 要在完整字串上下文中核對，再跑既有工具
Lua 的 stripped bytecode 仍保留部分行號，不能要求加註解前後逐 byte 相同

本次沒有新增 Doxygen dependency、Doxyfile、HTML site 或 CI target。原始碼
保留 Doxygen 格式，配合既有 clangd tooling 閱讀；未執行生成器就不宣稱
文件生成已驗證
