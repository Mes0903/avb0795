#include "platform/windows/windows_source_catalog.hpp"

#include "platform/windows/detail/item_observation_state.hpp"

#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.h>

#include <dxgi1_6.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace airelinna::platform::windows {

bool is_window_source_eligible(const WindowEligibilityFacts &facts) noexcept {
  return facts.visible && facts.cloak_query_succeeded && !facts.cloaked &&
         facts.has_nonzero_area &&
         (!facts.tool_window || facts.tool_window_intended_for_selection) &&
         !facts.owned_by_current_process && facts.has_usable_title;
}

core::ColorSupport classify_color_support(
    std::optional<DXGI_COLOR_SPACE_TYPE> color_space) noexcept {
  if (!color_space) {
    return core::ColorSupport::UnknownAssumeSdr;
  }
  if (*color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
    return core::ColorSupport::HdrDetectedUnsupported;
  }
  if (*color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
    return core::ColorSupport::Sdr;
  }
  return core::ColorSupport::UnknownAssumeSdr;
}

namespace {

using GraphicsCaptureItem =
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem;

struct PendingSource {
  core::SourceDescriptor descriptor;
  std::shared_ptr<detail::ItemObservationState> observation;
};

struct OutputColorSnapshot {
  HMONITOR monitor{};
  RECT desktop_coordinates{};
  std::optional<DXGI_COLOR_SPACE_TYPE> color_space;
};

[[nodiscard]] std::int64_t overlap_area(const RECT &left,
                                        const RECT &right) noexcept {
  const auto overlap_width =
      static_cast<std::int64_t>(std::min(left.right, right.right)) -
      static_cast<std::int64_t>(std::max(left.left, right.left));
  const auto overlap_height =
      static_cast<std::int64_t>(std::min(left.bottom, right.bottom)) -
      static_cast<std::int64_t>(std::max(left.top, right.top));
  if (overlap_width <= 0 || overlap_height <= 0) {
    return 0;
  }
  return overlap_width * overlap_height;
}

class DxgiColorReader final {
public:
  DxgiColorReader() { refresh(); }

  [[nodiscard]] std::optional<DXGI_COLOR_SPACE_TYPE>
  for_monitor(HMONITOR monitor) const noexcept {
    for (const auto &output : outputs_) {
      if (output.monitor == monitor) {
        return output.color_space;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<DXGI_COLOR_SPACE_TYPE>
  for_window(const RECT &bounds) const noexcept {
    const OutputColorSnapshot *best{};
    std::int64_t greatest_overlap{};
    for (const auto &output : outputs_) {
      const auto area = overlap_area(bounds, output.desktop_coordinates);
      if (area > greatest_overlap) {
        greatest_overlap = area;
        best = &output;
      }
    }
    return best == nullptr ? std::nullopt : best->color_space;
  }

private:
  void refresh() {
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(
            CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
      return;
    }

    for (UINT adapter_index{};; ++adapter_index) {
      winrt::com_ptr<IDXGIAdapter1> adapter;
      const auto adapter_result =
          factory->EnumAdapters1(adapter_index, adapter.put());
      if (adapter_result == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(adapter_result)) {
        return;
      }

      for (UINT output_index{};; ++output_index) {
        winrt::com_ptr<IDXGIOutput> output;
        const auto output_result =
            adapter->EnumOutputs(output_index, output.put());
        if (output_result == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        if (FAILED(output_result)) {
          return;
        }

        DXGI_OUTPUT_DESC description{};
        if (FAILED(output->GetDesc(&description))) {
          continue;
        }

        std::optional<DXGI_COLOR_SPACE_TYPE> color_space;
        if (const auto output6 = output.try_as<IDXGIOutput6>()) {
          DXGI_OUTPUT_DESC1 description1{};
          if (SUCCEEDED(output6->GetDesc1(&description1))) {
            color_space = description1.ColorSpace;
          }
        }
        outputs_.push_back(OutputColorSnapshot{
            .monitor = description.Monitor,
            .desktop_coordinates = description.DesktopCoordinates,
            .color_space = color_space,
        });
      }
    }
  }

  std::vector<OutputColorSnapshot> outputs_;
};

class CaptureItemCreator final {
public:
  CaptureItemCreator() {
    try {
      interop_ = winrt::get_activation_factory<GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();
    } catch (const winrt::hresult_error &) {
    }
  }

  [[nodiscard]] std::optional<GraphicsCaptureItem>
  for_window(HWND window) const noexcept {
    if (!interop_) {
      return std::nullopt;
    }
    GraphicsCaptureItem item{nullptr};
    const auto result = interop_->CreateForWindow(
        window,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item));
    if (FAILED(result)) {
      return std::nullopt;
    }
    return item;
  }

  [[nodiscard]] std::optional<GraphicsCaptureItem>
  for_monitor(HMONITOR monitor) const noexcept {
    if (!interop_) {
      return std::nullopt;
    }
    GraphicsCaptureItem item{nullptr};
    const auto result = interop_->CreateForMonitor(
        monitor,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item));
    if (FAILED(result)) {
      return std::nullopt;
    }
    return item;
  }

private:
  winrt::com_ptr<IGraphicsCaptureItemInterop> interop_;
};

class CloakReader final {
public:
  CloakReader() noexcept
      : module_{LoadLibraryExW(L"dwmapi.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_SYSTEM32)} {
    if (module_ != nullptr) {
      get_window_attribute_ = reinterpret_cast<GetWindowAttribute>(
          GetProcAddress(module_, "DwmGetWindowAttribute"));
    }
  }

  ~CloakReader() {
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  CloakReader(const CloakReader &) = delete;
  CloakReader &operator=(const CloakReader &) = delete;
  CloakReader(CloakReader &&) = delete;
  CloakReader &operator=(CloakReader &&) = delete;

  [[nodiscard]] std::optional<bool> is_cloaked(HWND window) const noexcept {
    if (get_window_attribute_ == nullptr) {
      return std::nullopt;
    }
    DWORD cloaked{};
    const auto result =
        get_window_attribute_(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (FAILED(result)) {
      return std::nullopt;
    }
    return cloaked != 0;
  }

private:
  using GetWindowAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);

  HMODULE module_{};
  GetWindowAttribute get_window_attribute_{};
};

[[nodiscard]] std::wstring trim(std::wstring value) {
  const auto not_space = [](wchar_t character) {
    return std::iswspace(static_cast<std::wint_t>(character)) == 0;
  };
  const auto first = std::find_if(value.begin(), value.end(), not_space);
  const auto last =
      std::find_if(value.rbegin(), value.rend(), not_space).base();
  if (first >= last) {
    return {};
  }
  return std::wstring{first, last};
}

[[nodiscard]] std::wstring window_label(HWND window) {
  const auto title_length = GetWindowTextLengthW(window);
  if (title_length > 0) {
    std::wstring title(static_cast<std::size_t>(title_length) + 1, L'\0');
    const auto copied =
        GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (copied > 0) {
      title.resize(static_cast<std::size_t>(copied));
      title = trim(std::move(title));
      if (!title.empty()) {
        return title;
      }
    }
  }

  std::array<wchar_t, 256> class_name{};
  const auto copied = GetClassNameW(window, class_name.data(),
                                    static_cast<int>(class_name.size()));
  if (copied <= 0) {
    return {};
  }
  return trim(
      std::wstring{class_name.data(), static_cast<std::size_t>(copied)});
}

[[nodiscard]] std::string utf8(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const auto size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), size,
                          nullptr, nullptr) != size) {
    return {};
  }
  return result;
}

[[nodiscard]] std::optional<core::Extent>
extent_from_rect(const RECT &bounds) noexcept {
  const auto width = static_cast<std::int64_t>(bounds.right) - bounds.left;
  const auto height = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  if (width <= 0 || height <= 0 || width > maximum || height > maximum) {
    return std::nullopt;
  }
  return core::Extent{.width = static_cast<std::uint32_t>(width),
                      .height = static_cast<std::uint32_t>(height)};
}

[[nodiscard]] std::shared_ptr<detail::ItemObservationState>
observe(std::optional<GraphicsCaptureItem> item) {
  if (!item) {
    return {};
  }
  try {
    return detail::ItemObservationState::create(std::move(*item));
  } catch (const winrt::hresult_error &) {
    return {};
  }
}

[[nodiscard]] constexpr bool
is_active_desktop_display(bool metadata_query_succeeded,
                          DWORD state_flags) noexcept {
  return metadata_query_succeeded &&
         (state_flags & DISPLAY_DEVICE_ACTIVE) != 0 &&
         (state_flags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 &&
         (state_flags & DISPLAY_DEVICE_MIRRORING_DRIVER) == 0;
}

static_assert(!is_active_desktop_display(false, DISPLAY_DEVICE_ACTIVE));
static_assert(is_active_desktop_display(true, DISPLAY_DEVICE_ACTIVE));
static_assert(!is_active_desktop_display(true, 0));
static_assert(!is_active_desktop_display(true,
                                         DISPLAY_DEVICE_ACTIVE |
                                             DISPLAY_DEVICE_MIRRORING_DRIVER));

[[nodiscard]] constexpr bool
display_device_names_match(std::wstring_view left,
                           std::wstring_view right) noexcept {
  return left == right;
}

static_assert(display_device_names_match(LR"(\\.\DISPLAY1)",
                                         LR"(\\.\DISPLAY1)"));
static_assert(!display_device_names_match(LR"(\\.\DISPLAY1)",
                                          LR"(\\.\DISPLAY2)"));

[[nodiscard]] std::optional<DWORD> find_display_adapter_state_flags(
    std::wstring_view monitor_device_name) noexcept {
  constexpr DWORD maximum_adapter_queries{256};
  for (DWORD adapter_index{}; adapter_index < maximum_adapter_queries;
       ++adapter_index) {
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, adapter_index, &adapter, 0)) {
      return std::nullopt;
    }
    if (display_device_names_match(adapter.DeviceName, monitor_device_name)) {
      return adapter.StateFlags;
    }
  }
  return std::nullopt;
}

struct MonitorEnumerationContext {
  CaptureItemCreator &capture_items;
  const DxgiColorReader &color_reader;
  std::vector<PendingSource> &pending;
  std::exception_ptr failure;

  void visit(HMONITOR monitor, const RECT &bounds) {
    MONITORINFOEXW monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(monitor,
                         reinterpret_cast<MONITORINFO *>(&monitor_info))) {
      return;
    }

    const auto adapter_state_flags =
        find_display_adapter_state_flags(monitor_info.szDevice);
    if (!adapter_state_flags ||
        !is_active_desktop_display(true, *adapter_state_flags)) {
      return;
    }

    const auto extent = extent_from_rect(bounds);
    const auto label = utf8(monitor_info.szDevice);
    if (!extent || label.empty()) {
      return;
    }

    auto observation = observe(capture_items.for_monitor(monitor));
    if (!observation) {
      return;
    }
    pending.push_back(PendingSource{
        .descriptor =
            core::SourceDescriptor{
                .id = {},
                .kind = core::SourceKind::Display,
                .display_label = label,
                .extent = *extent,
                .color_support =
                    classify_color_support(color_reader.for_monitor(monitor)),
            },
        .observation = std::move(observation),
    });
  }
};

BOOL CALLBACK monitor_callback(HMONITOR monitor, HDC, LPRECT bounds,
                               LPARAM parameter) noexcept {
  auto &context = *reinterpret_cast<MonitorEnumerationContext *>(parameter);
  try {
    context.visit(monitor, *bounds);
    return TRUE;
  } catch (...) {
    context.failure = std::current_exception();
    return FALSE;
  }
}

struct WindowEnumerationContext {
  CaptureItemCreator &capture_items;
  const DxgiColorReader &color_reader;
  const CloakReader &cloak_reader;
  std::vector<PendingSource> &pending;
  DWORD current_process_id;
  std::exception_ptr failure;

  void visit(HWND window) {
    RECT bounds{};
    const auto has_bounds = GetWindowRect(window, &bounds) != FALSE;
    const auto extent = has_bounds ? extent_from_rect(bounds) : std::nullopt;
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    const auto extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const auto label = utf8(window_label(window));
    const auto cloaked = cloak_reader.is_cloaked(window);

    const WindowEligibilityFacts facts{
        .visible = IsWindowVisible(window) != FALSE,
        .cloak_query_succeeded = cloaked.has_value(),
        .cloaked = cloaked.value_or(true),
        .has_nonzero_area = extent.has_value(),
        .tool_window = (extended_style & WS_EX_TOOLWINDOW) != 0,
        .tool_window_intended_for_selection =
            (extended_style & WS_EX_APPWINDOW) != 0,
        .owned_by_current_process = process_id == current_process_id,
        .has_usable_title = !label.empty(),
    };
    if (!is_window_source_eligible(facts)) {
      return;
    }

    auto observation = observe(capture_items.for_window(window));
    if (!observation) {
      return;
    }
    pending.push_back(PendingSource{
        .descriptor =
            core::SourceDescriptor{
                .id = {},
                .kind = core::SourceKind::Window,
                .display_label = label,
                .extent = *extent,
                .color_support =
                    classify_color_support(color_reader.for_window(bounds)),
            },
        .observation = std::move(observation),
    });
  }
};

BOOL CALLBACK window_callback(HWND window, LPARAM parameter) noexcept {
  auto &context = *reinterpret_cast<WindowEnumerationContext *>(parameter);
  try {
    context.visit(window);
    return TRUE;
  } catch (...) {
    context.failure = std::current_exception();
    return FALSE;
  }
}

[[nodiscard]] core::AppError catalog_failure() noexcept {
  return core::AppError{.code = core::AppErrorCode::SourceCatalogFailure};
}

} // namespace

WindowsSourceCatalog::WindowsSourceCatalog(
    WindowsSourceRegistry &registry) noexcept
    : registry_{registry} {}

std::expected<std::vector<core::SourceDescriptor>, core::AppError>
WindowsSourceCatalog::enumerate() {
  auto refresh = registry_.begin_refresh();
  if (!refresh) {
    return std::unexpected(refresh.error());
  }

  try {
    CaptureItemCreator capture_items;
    const DxgiColorReader color_reader;
    const CloakReader cloak_reader;
    std::vector<PendingSource> pending;

    MonitorEnumerationContext monitor_context{
        .capture_items = capture_items,
        .color_reader = color_reader,
        .pending = pending,
    };
    if (!EnumDisplayMonitors(nullptr, nullptr, monitor_callback,
                             reinterpret_cast<LPARAM>(&monitor_context)) ||
        monitor_context.failure) {
      return std::unexpected(catalog_failure());
    }

    WindowEnumerationContext window_context{
        .capture_items = capture_items,
        .color_reader = color_reader,
        .cloak_reader = cloak_reader,
        .pending = pending,
        .current_process_id = GetCurrentProcessId(),
    };
    if (!EnumWindows(window_callback,
                     reinterpret_cast<LPARAM>(&window_context)) ||
        window_context.failure) {
      return std::unexpected(catalog_failure());
    }

    std::vector<std::shared_ptr<detail::ItemObservationState>> observations;
    observations.reserve(pending.size());
    std::vector<core::SourceDescriptor> descriptors;
    descriptors.reserve(pending.size());
    for (auto &source : pending) {
      observations.push_back(std::move(source.observation));
    }

    auto ids = refresh->commit(std::move(observations));
    if (!ids) {
      return std::unexpected(ids.error());
    }
    for (std::size_t index{}; index < pending.size(); ++index) {
      pending[index].descriptor.id = std::move((*ids)[index]);
      descriptors.push_back(std::move(pending[index].descriptor));
    }
    return descriptors;
  } catch (...) {
    return std::unexpected(catalog_failure());
  }
}

} // namespace airelinna::platform::windows
