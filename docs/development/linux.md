# Linux 開發與桌面驗收

Linux 版本沿用共用 Application、UI 與 Vulkan renderer。 來源由 ScreenCast Portal 授權，再經 PipeWire 的 CPU BGRA／BGRx 路線送入副本。 Controller 與副本固定使用 GLFW X11，在 GNOME Wayland session 中透過 XWayland 呈現

目前已完成 Linux 產品實作及無桌面政策驗證。 真實 Portal 到副本的完整擷取、背景 Chrome 的新影格與移動內容、實體視窗操作，以及真正 GNOME Xorg session 的驗收仍待完成。 建置成功與跳過硬體測試各自保留原本的證據範圍

## Ubuntu 開發依賴

需要 CMake 3.28 以上、Ninja、pkg-config 與能編譯 C++23 的編譯器和標準函式庫。 建置會實際使用 `std::expected`、`std::format` 與 `std::ios::noreplace`。 本機已使用 GCC 15.2 建置，其他工具鏈仍須各自驗證

完整開發套件清單如下，安裝由開發者在自己的環境執行：

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libvulkan-dev glslc libglib2.0-dev libpipewire-0.3-dev \
  libfontconfig1-dev fonts-noto-cjk \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxext-dev
```

`libx11-dev` 提供 X11 與 XKB headers。 Xrandr、Xinerama、Xcursor、Xi 是固定 GLFW 3.4 的開發依賴。 Xfixes 與 Xext／Shape 供 Linux 原生互動使用。 GIO 2.0／gio-unix 2.0 由 GLib 開發套件提供，PipeWire 的 pkg-config 模組名為 `libpipewire-0.3`

桌面執行另需可用的 `xdg-desktop-portal`、GNOME Portal backend、PipeWire、XWayland，以及相容的 Vulkan loader／GPU driver。 Fontconfig 必須找到涵蓋英文、繁體中文與日文 UI 字元的 Noto CJK 字型，程式會把 TTC face index 與 owning bytes 一起交給共用 host

Controller 與副本各自建立 Vulkan 1.3 device，並要求實際啟用 `VK_KHR_swapchain_maintenance1` 或 `VK_EXT_swapchain_maintenance1` 的 present fences。 CPU 傳輸仍需符合這個 GPU／driver 門檻。 列出 extension、找到 runtime library 或使用軟體裝置，各自都不足以證明實體 GPU 呈現成功

第三方來源固定為 nlohmann/json 3.12.0、GLFW 3.4、Dear ImGui 1.92.9。 Catch2 3.8.1 僅供本機測試使用。 來源摘要與 patch 檢查由 [Dependencies.cmake](../../cmake/Dependencies.cmake) 保持一致

若依賴由本機 SDK 提供，須讓 `PATH`、`CMAKE_PREFIX_PATH`、`LD_LIBRARY_PATH` 與 `PKG_CONFIG_PATH` 指向該 SDK。 沿用已查核的 SDK `.pc` relocation 與 `PKG_CONFIG_ARGN=--dont-define-prefix`，不設定涵蓋系統 GIO 的 `PKG_CONFIG_SYSROOT_DIR`。 本機 SDK 與其環境腳本由開發者另行準備

## 產品建置與編輯器

提交的產品來源排除了本次新增的全部測試與其 CMake 接線。 乾淨 checkout 用以下命令建立產品，避免把不存在的本機測試清單當成執行成功：

```bash
cmake --preset linux-desktop \
  -DAVB0795_BUILD_TESTS=OFF \
  -DAVB0795_BUILD_LINUX_TESTS=OFF \
  -DAVB0795_ENABLE_LINUX_INTEGRATION=OFF
cmake --build --preset linux-desktop --target avb0795
cmake --build build/linux-desktop --target verify_source_boundaries
build/linux-desktop/src/avb0795
```

啟動須使用目前使用者自己的桌面 session、有效的 `DISPLAY` 與 session D-Bus。 GNOME Wayland 也需要 XWayland。 程式在 GLFW init 前明確選擇 X11，缺少 display 時回報啟動診斷。 不得藉由切換桌面或自動改用未完成的原生 Wayland 視窗來略過缺少的能力

`linux-portable` 建立無 graphics 的共用函式庫，`linux-desktop` 建立 Linux 產品及共用 graphics，兩者使用獨立 Ninja Debug build trees。 編輯器使用 `cmake --preset linux-clangd` 與 `cmake --build --preset linux-clangd-generated`，詳見 [clangd 指南](clangd.md)。 Linux packaging 仍屬後續工作

## 設定與操作限制

設定路徑為 `$XDG_CONFIG_HOME/avb0795/avb0795.config`。 只有絕對的 `XDG_CONFIG_HOME` 有效，其他情況使用 `$HOME/.config/avb0795/avb0795.config`。 程式使用 `umask(0077)`，自有目錄為 0700，新檔案為 0600，並持有 writer lock。 第二個 writer 明確失敗。 Linux 不讀 Windows 執行檔旁的 legacy JSON

跟隨系統語言時，Linux 使用有效 locale 與 `LANGUAGE`、`LC_ALL`、`LC_MESSAGES`、`LANG` 的既定優先序，C／POSIX 或無支援語言回英文。 偏好 JSON schema 與三語 UI 共用，實際字形及重新啟動後的保存仍須桌面驗收

首版停用全域快捷鍵、擷取排除及 browser keep-alive，保留已保存偏好。 副本為 PassThrough 或 opacity 0 時，使用 controller 恢復 Editable 或 Stop。 不支援視窗 opacity 時，有效 opacity 為 100，保存的偏好不被覆寫

被遮住的視窗、背景分頁與真正最小化各自有不同條件。 首版不承諾真正最小化的 Chrome 持續 live。 驗收保持使用者既有 Chrome 視窗與播放設定，不改 flags、不切換分頁、不重新播放，也不自動最小化或關閉它

## 本機驗證與硬體同意

完整本機工作樹保留六個 portable cases、18 個新增 Linux 政策 cases，以及唯一的自有來源硬體 case，共 25 個。 這些新增測試與登記接線未隨產品提交。 以下命令只適用於保留該本機測試內容的驗證工作樹：

```bash
cmake --preset linux-portable
cmake --build --preset linux-portable
ctest --test-dir build/linux-portable --output-on-failure
cmake -DBUILD_DIRECTORY=build/linux-portable -DINVENTORY_PROFILE=portable-core \
  -P cmake/VerifyTestInventory.cmake
cmake --preset linux-desktop
cmake --build --preset linux-desktop
ctest --test-dir build/linux-desktop -LE linux-hardware --output-on-failure
```

硬體登記預設 OFF，只有顯式 `AVB0795_ENABLE_LINUX_INTEGRATION=ON` 才出現 `linux-hardware` case。 執行前另要求精確的 `AVB0795_LINUX_HARDWARE_TESTS=1`，缺少、空值或其他值皆在原生啟動前回傳 77。 Discovery／列清單不開啟視窗、bus、chooser 或 GPU

```bash
cmake --preset linux-desktop -DAVB0795_ENABLE_LINUX_INTEGRATION=ON
cmake --build --preset linux-desktop
ctest --test-dir build/linux-desktop -N -L linux-hardware
AVB0795_LINUX_HARDWARE_TESTS=1 ctest --test-dir build/linux-desktop \
  -L linux-hardware --output-on-failure
```

唯一硬體 case 要求在真正 Portal 中選取它自己的動畫視窗，暫時比對已知變動像素、接受來源序號／intake 與成功退休的 presents。 停止並確認 producer quiescent 後，以 renderer 已複製的影像裁切重畫，確認新呈現不增加來源序號。 它涵蓋底層 production owners，完整 Application／controller 與 Chrome 操作另行驗收

取消或缺少能力只在證明清理完成後記為 skipped。 Catch 的純 skip 4 轉為 77，assertion failure 保留失敗。 CTest 的「100% tests passed」可能包含 skipped，必須查閱實際 case 結果。 Runner timeout、強制終止或 abandoned 都不提供正常 drain 證據

## GNOME session 與 Chrome 驗收

Wayland／XWayland 與真正 GNOME Xorg 各自記錄 Ubuntu、kernel、GNOME／Mutter、Portal frontend／backend、PipeWire、XWayland、實際建立的 GPU／driver、來源 backend 與清理結果。 套件版本屬安裝資料，GPU 身分須來自實際建立的 device。 Chrome backend 未獨立確認時記為 unknown

Chrome 必須經真正的 window chooser 選到使用者既有播放視窗。 確認副本呈現移動內容、來源非前景且被普通不透明視窗遮住後，開始獨立 steady-clock 60 秒觀察，固定 60 個一秒區間。 事先固定至少 55 個區間同時有來源發布與 delivery 進展，最大 delivered intake 間隔不超過 5 秒。 來源 instance、producer revision 或 graphics epoch 改變即記為不連續，不能重設起點製造成功

來源發布 sequence、bridge delivery 的 `FrameStamp` 與退休 presents 分別記錄。 前兩者證明新來源及實際交付，presents 包含 retained redraw。 序號進展還須搭配人工移動內容比對，畫面、像素樣本與視窗標題不寫入紀錄。 這個 60 秒觀察與 performance report 的 10 秒 warmup 加 60 秒量測各自獨立

接著逐項操作來源 resize、crop／Esc、FPS、opacity 0／50／100、border、wheel zoom、Editable／PassThrough／controller 恢復、普通 Stop／Start 與 Quit。 命令接受不能代替實體輸入及 compositor 視覺結果。 Quit 須取得 acquisition quiescence 與 runtime／GPU 的獨立退休證據，Chrome 持續開啟

目前沒有真正 GNOME Xorg 與 Windows runner 的完整紀錄，保留待驗。 XWayland 不替代 Xorg。 若真正 Xorg Portal 擷取失敗，先記錄失敗並停止 X11 支援聲明，再另行設計 Xorg capture adapter

原生責任可沿[共用 renderer](../architecture/shared-vulkan-renderer.md)、[桌面 host](../architecture/desktop-host.md) 與[建置責任](../architecture/build-and-validation.md)閱讀
