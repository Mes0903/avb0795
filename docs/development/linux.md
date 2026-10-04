# Linux 開發與桌面驗收

Linux 使用直接 X11 來源，沿用共用 Application、ImGui UI、CPU frame bridge 與 Vulkan renderer。 在 controller 重新整理來源清單、選取 Window 或 Display，再按 Start。 不需要系統分享選單。 重新整理與選取只觀察來源，Start 才啟動擷取

首要驗收環境為 Ubuntu／GNOME 的真正 Xorg session。 GLFW controller 與副本使用 X11。 在 Wayland session 中透過 XWayland 呈現，來源範圍限同一 X server 的 X 視窗。 原生 Wayland 視窗與整個 Wayland 桌面尚未支援。 真正 GNOME Xorg、使用者背景 Chrome 與完整操作矩陣仍待驗。 自有 X11 動畫視窗的實驗不代替這些結果

## Ubuntu 開發依賴

需要 CMake 3.28 以上、Ninja、pkg-config、Git，以及能編譯 C++23 的編譯器和標準函式庫。 建置實際使用 `std::expected`、`std::format` 與 `std::ios::noreplace`。 本機使用 GCC 15.2，其他工具鏈須各自驗證。 開發者在自己的環境安裝：

```bash
sudo apt install build-essential cmake ninja-build pkg-config git \
  libvulkan-dev glslc libfontconfig1-dev fonts-noto-cjk \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxext-dev \
  libxcb1-dev libxcb-composite0-dev libxcb-randr0-dev
```

Xlib／XKB、Xrandr、Xinerama、Xcursor、Xi 供 GLFW 使用。 Xfixes 與 Xext／Shape 供原生視窗互動使用。 來源 catalog／capture 使用獨立 XCB connections，pkg-config 模組為 `xcb`、`xcb-composite`、`xcb-randr`。 Window 需要真正 EWMH client list 與 Composite 0.2 以上

Display 讀 root，RandR 能力決定 monitor／layout 觀察。 原生 TrueColor 24／32-bit、支援的 32-bit 像素與 RGB masks 正規化為 opaque BGRA8。 不支援格式回報錯誤

執行需要有效的 X11 display／授權、相容的 Vulkan loader／driver，以及 Fontconfig 能找到的 Noto CJK 字型。 Wayland session 額外需要 XWayland，但這不擴大可擷取的來源範圍。 本產品接線不查找或連結 GIO／PipeWire，不啟動 ScreenCast Portal，也沒有 Portal fallback

Controller 與副本各自建立 Vulkan 1.3 device，要求實際啟用 `VK_KHR_swapchain_maintenance1` 或 `VK_EXT_swapchain_maintenance1` 的 present fences。 CPU transport 仍須符合此門檻。 Extension 列表、軟體 WSI 成功與實體 GPU 驗收各有不同證據範圍。 不能由套件版本推定實際選用的 device

固定第三方來源為 nlohmann/json 3.12.0、GLFW 3.4 與 Dear ImGui 1.92.9。 Catch2 3.8.1 僅供本機測試。 [Dependencies.cmake](../../cmake/Dependencies.cmake) 核對來源摘要與 patches。 本機 SDK 另行準備，透過 `PATH`、`CMAKE_PREFIX_PATH`、`LD_LIBRARY_PATH`、`PKG_CONFIG_PATH` 選用。 SDK `.pc` relocation 必須正確，不以廣泛 sysroot／include fallback 隱藏相依錯誤

## 產品建置與編輯器

本次新增測試與 tests CMake 接線未隨產品提交。 乾淨 checkout 明確關閉測試來建置產品：

```bash
cmake --preset linux-desktop \
  -DAVB0795_BUILD_TESTS=OFF \
  -DAVB0795_BUILD_LINUX_TESTS=OFF \
  -DAVB0795_ENABLE_LINUX_INTEGRATION=OFF
cmake --build --preset linux-desktop --target avb0795
cmake --build build/linux-desktop --target verify_source_boundaries
build/linux-desktop/src/avb0795
```

在使用者自己的桌面啟動，保留有效 `DISPLAY` 與 X11 授權。 Entry 在 GLFW init 前選定 X11。 缺少 display 明確回報啟動失敗。 本輪不切換 session，也不改用原生 Wayland backend

`linux-portable` 建立無 graphics 的共用函式庫，`linux-desktop` 建立 Linux 產品與共用 graphics，各有獨立 build tree。 編輯器執行 `cmake --preset linux-clangd` 與 `cmake --build --preset linux-clangd-generated`，實際 CDB／hover／導覽見 [clangd 指南](clangd.md)。 Packaging 尚待後續工作

## 責任與資源生命週期

UI 只發 commands／讀 view state，Application 決定來源與 session。 Linux catalog 保存 opaque SourceId 對應的不可變 observation 與 validity token，factory 在完整準備前後重新 resolve 同一 identity。 Capture worker 使用自己的 checked XCB watches，不依賴主執行緒及時刷新。 controller／replica 只依精確自有 drawable 排除，同 PID 的其他來源不一併隱藏

Window 使用 Composite automatic redirect／named pixmap。 Display 使用 root／monitor region。 每個 worker 最多一個 pending GetImage，reply 先經格式與 epoch 驗證，再寫三個 owning CPU slots。 Resize 要先退休舊 leases／writers、建立新 allocation 並得到 exact revision ACK，才恢復交付。 Bridge 同步複製到 renderer-owned image，沒有第二個 capture consumer。 SHM／DMA-BUF 留待後續設計

Stop 關閉 admission，tick 持續推進 producer callback／worker／wake 與獨立 GPU submission／presentation retirement。 只有 aggregate `Stopped` 才允許普通析構。 Deadline、命令接受或來源序號不提供 release proof。 停滯沿用保留 owners 的 non-unwinding process exit。 Shared renderer 不操作 X11 來源，frontend 不持原生硬體 authority

## 設定與操作限制

設定為 `$XDG_CONFIG_HOME/avb0795/avb0795.config`。 只有絕對 XDG 路徑有效，否則使用 `$HOME/.config/avb0795/avb0795.config`。 Entry 使用 `umask(0077)`，自有目錄 0700、新檔案 0600，並持 writer lock。 第二 writer 明確失敗。 Linux 不讀 Windows executable-relative legacy JSON

系統語言依 `LANGUAGE`、`LC_ALL`、`LC_MESSAGES`、`LANG` 的既定有效 locale 政策選取，C／POSIX 或未支援語言使用英文。 三語 UI／偏好 schema 共用，實際字形與重新啟動保存仍須桌面驗收

全域快捷鍵、擷取排除與 browser keep-alive 停用，但保留已保存偏好。 PassThrough 或 opacity 0 時由 controller 恢復 Editable 或 Stop。 不支援 opacity 時有效值為 100，不覆寫偏好。 Always-on-top、resize、穿透與 opacity 都由真正 WM 的能力／結果決定

背景被遮住、背景分頁、其他 workspace 與真正最小化各自不同。 不承諾最小化 Chrome 持續 live。 驗收保持使用者原有 Chrome／播放設定，不改 flags、不切換分頁、不重新播放、不重啟或關閉它

## 本機驗證與執行 gate

完整本機工作樹的預設清單為 22 個：六個原 core cases，加十三個 Linux 與三個 shared-render policy cases。 唯一 opt-in hardware case 加入後共 23 個，新增非 core 共十七個。 新測試及其登記不在產品 commit。 以下僅適用於保留它們的驗證工作樹：

```bash
cmake --preset linux-portable
cmake --build --preset linux-portable
ctest --test-dir build/linux-portable --output-on-failure
cmake -DBUILD_DIRECTORY=build/linux-portable -DINVENTORY_PROFILE=portable-core \
  -P cmake/VerifyTestInventory.cmake
cmake --preset linux-desktop
cmake --build --preset linux-desktop
ctest --test-dir build/linux-desktop --output-on-failure
```

Integration 登記預設 OFF，與 policy opt-in 獨立，但必須有 graphics。 執行另要求精確 `AVB0795_LINUX_HARDWARE_TESTS=1`。 缺少／其他值在 native startup 前回 77。 Catch discovery 不開視窗或 GPU。 77／純 skip 不算驗收通過：

```bash
cmake --preset linux-desktop -DAVB0795_ENABLE_LINUX_INTEGRATION=ON
cmake --build --preset linux-desktop
ctest --test-dir build/linux-desktop -N -L linux-hardware
AVB0795_LINUX_HARDWARE_TESTS=1 ctest --test-dir build/linux-desktop \
  -L linux-hardware --output-on-failure
```

Skip 需要獨立的能力缺少證據與正常 cleanup。 Native probe 的 false 或 factory `UnsupportedGraphics` 也可能包含查詢失敗、記憶體不足或 device loss，測試會明確 fail，不把模糊結果轉為 77；不另建 fault-injection framework。

唯一 case `linux hardware: owned X11 moving source and retained redraw` 在真正 WM 中建立自己的動畫來源，用真正 catalog／factory／runtime 擷取。 獨立讀自有 replica 像素，分開核對 publication、delivery／intake、移動 phases 與退休 presents，再驗證 resize、SourceLost 及 ordinary Stop／Stopped。 不讀使用者視窗，也不在使用者 root 安裝模擬 EWMH

Retained redraw 使用既有 caller-owned wake callback，單次延後返回不超過一秒，明確屬人為 notification 時序。 非零起點 crop 須匹配新 region，並排除目前 window geometry 下的 full-view 假設。 閉合 ticks 要求完整來源 stamp／identities 不變、獨立像素改變與 aggregate 退休數增加。 每條 cleanup 先解除延遲，再 Stop，保留 callback 借用直到 Stopped。 這不代替正常 App UI、背景 Chrome 或實體 GPU 的驗收

## GNOME session 與 Chrome 驗收

Xorg 與 Wayland／XWayland 分別記錄 Ubuntu、kernel、GNOME／Mutter、X server、實際 GPU／driver、來源 backend 與清理結果。 XWayland 結果只適用於同 server 的 X 視窗，不證明真正 Xorg、native Wayland 或完整 Wayland 螢幕。 Chrome backend 未獨立確認時記 unknown

原 Chrome 必須實際出現在 X11 catalog，經正常 ImGui 選取／Start。 非前景並由普通不透明視窗遮住後，觀察固定 steady-clock 60 秒／六十個一秒 bins。 至少 55 bins 同時有 publication 與 delivery 進展，最大 delivered intake 間隔 ≤5 秒。 instance／revision／graphics epoch 改變不能重設起點製造通過。 序號須搭配獨立動態內容比對，畫面、使用者像素與標題不存入紀錄

依序操作 resize、crop／Esc、FPS、zoom、border、opacity 0／50／100、PassThrough→controller Editable／Stop、Stop／Start 及 Quit。 命令接受不代替實體輸入／WM 結果。 Quit 必須正常完成 producer 與 GPU 退休，Chrome 持續開啟。 沒有真正 Xorg／可識別 X11 Chrome／Windows runner 時，各項保留待驗

原生責任見[共用 renderer](../architecture/shared-vulkan-renderer.md)、[桌面 host](../architecture/desktop-host.md)與[建置責任](../architecture/build-and-validation.md)。 Unit Doxygen 的目的、借用與 WHY 寫法見[註解指南](source-comments.md)
