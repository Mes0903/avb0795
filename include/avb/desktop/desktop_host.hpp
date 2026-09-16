/// @file
/// Controller-window infrastructure and the main-thread application event loop.
#ifndef AVB0795_DESKTOP_DESKTOP_HOST_HPP
#define AVB0795_DESKTOP_DESKTOP_HOST_HPP

#include "avb/app/application.hpp"
#include "avb/app/commands.hpp"
#include "avb/app/controller_view_state.hpp"

#include <expected>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

struct GLFWwindow;

namespace avb::desktop {

  /// Process handoff from run(). abandon_process requires non-unwinding exit
  /// while owners remain alive; exit_code alone is not cleanup permission.
  struct HostRunResult final {
    int exit_code{};
    bool abandon_process{};
  };

  /// Enqueue one portable command on the main thread; true means accepted.
  using CommandSink = std::function<bool(app::AppCommand)>;
  /// Composition-supplied setup of the newly created controller window.
  /// Native platform details stay in that callable, outside DesktopHost.
  using ControllerWindowCreated = std::function<std::expected<core::CaptureExclusionStatus, core::AppError>(GLFWwindow *)>;

  /// Own the controller GLFW window, ImGui state/font bytes, and private Vulkan
  /// presentation context. Calls Application's host surface and emits commands;
  /// it never owns or directly calls the replica runtime or application ports.
  class DesktopHost final {
  public:
    /// Construct on the initialized GLFW main thread. Take ownership of font
    /// bytes for the atlas lifetime and invoke window_created before UI setup.
    /// Partial startup failure cleans only resources that were never submitted.
    [[nodiscard]] static std::expected<DesktopHost, core::AppError>
    create(CommandSink command_sink, ControllerWindowCreated window_created, std::vector<std::byte> font_data);

    /// Destroy before run or after a non-abandon result; never wait for the GPU.
    ~DesktopHost();

    DesktopHost(const DesktopHost &) = delete;
    DesktopHost &operator=(const DesktopHost &) = delete;
    DesktopHost(DesktopHost &&) noexcept;
    DesktopHost &operator=(DesktopHost &&) noexcept;

    /// Return the startup capture-exclusion result for application projection.
    [[nodiscard]] core::CaptureExclusionStatus
    controller_exclusion() const noexcept;
    /// Post GLFW's thread-safe empty event. Callback owners must keep the host
    /// alive until their admitted wake invocations have returned.
    void wake() const noexcept;
    /// Drive commands, controller presentation and deadline-aware shutdown on
    /// the main thread; contain failures and return an explicit exit disposition.
    [[nodiscard]] HostRunResult run(app::Application &application) noexcept;

  private:
    class Impl;

    explicit DesktopHost(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
  };

}    // namespace avb::desktop

#endif    // AVB0795_DESKTOP_DESKTOP_HOST_HPP
