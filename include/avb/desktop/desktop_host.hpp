/// @file
/// @brief Controller-window infrastructure and the main-thread application loop.
///
/// This boundary owns the controller's presentation resources, not the replica's
/// capture or rendering resources. Its result tells the composition root whether
/// ordinary stack destruction is permitted after the event loop finishes.
#ifndef AVB0795_DESKTOP_DESKTOP_HOST_HPP
#define AVB0795_DESKTOP_DESKTOP_HOST_HPP

#include "avb/app/application.hpp"
#include "avb/app/commands.hpp"
#include "avb/app/controller_view_state.hpp"
#include "avb/desktop/controller_window_interaction.hpp"

#include <expected>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

struct GLFWwindow;

namespace avb::diagnostics { class PerformanceReport; }

namespace avb::desktop {

  /// @brief Separate the process result from permission to destroy retained owners.
  ///
  /// A nonzero exit code can accompany a completely drained controller. Conversely,
  /// abandon_process means possibly referenced resources must remain alive until
  /// a non-unwinding process exit. The caller must inspect that flag before local
  /// owners leave scope; logging an error and returning normally is not sufficient.
  struct HostRunResult final {
    int exit_code{};
    bool abandon_process{};
  };

  /// Enqueue an owning command value on the main thread. True acknowledges queue
  /// acceptance, not completion of the requested operation; the application
  /// processes the command in a subsequent step. Captured references must outlive
  /// the host, which retains this callable.
  using CommandSink = std::function<bool(app::AppCommand)>;
  /// @brief Native setup facts and an interaction owner transferred to the host.
  /// The interaction borrows the controller window for its lifetime. Backend
  /// shutdown precedes its destruction, and native window destruction follows it,
  /// preserving the input-hook chain on normal exit and constructor unwinding.
  struct ControllerWindowSetup {
    core::CaptureExclusionStatus exclusion{core::CaptureExclusionStatus::NotAttempted};
    std::unique_ptr<ControllerWindowInteraction> interaction;
  };

  /// Composition-supplied setup before graphics/UI initialization. Success moves
  /// the interaction owner into the host; an error aborts construction. The
  /// callable itself is synchronous and is released after startup.
  using ControllerWindowCreated = std::function<std::expected<ControllerWindowSetup, core::AppError>(GLFWwindow *)>;

  /// @brief Own the controller window, UI/font storage and private presentation context.
  ///
  /// The host drives Application through its public step/view/shutdown surface and
  /// emits command values. Replica resources and application service ports remain
  /// outside this owner. Sharing graphics-result policy does not share devices,
  /// submission records, or permission to destroy another owner's resources.
  ///
  /// Except for wake(), operations belong to the initialized GLFW main thread.
  /// Moving the wrapper preserves its implementation address, but callers must
  /// not move or destroy a wrapper while another thread is invoking its methods.
  /// A move assignment also destroys the previous implementation and therefore
  /// requires the same safe-retirement conditions as destruction.
  class DesktopHost final {
  public:
    /// @brief Construct a controller without submitting graphics work during startup.
    ///
    /// Take ownership of the font bytes for the atlas lifetime and retain the
    /// command sink. Invoke window_created once before UI setup; its error cancels
    /// construction, while its status is retained for later presentation.
    /// Startup failures release partial, never-submitted resources, so the caller
    /// receives either a complete movable host or a typed construction error.
    /// An enabled performance collector is borrowed through startup, run/drain,
    /// and destruction; composition must keep that collector alive throughout.
    /// @pre GLFW is initialized and this is its main thread; both callables exist.
    [[nodiscard]] static std::expected<DesktopHost, core::AppError>
    create(CommandSink command_sink, ControllerWindowCreated window_created, std::vector<std::byte> font_data,
           diagnostics::PerformanceReport *performance = nullptr);

    /// Destroy before run or after a non-abandon result; never wait for the GPU.
    /// Unresolved submitted work violates the implementation's destruction
    /// invariant and terminates instead of silently releasing referenced objects.
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
    /// @brief Route a confirmed global chord to a focused controller recorder.
    ///
    /// Query the controller's native focus at event time so a focus transition
    /// between rendered frames immediately restores the application recovery
    /// route. A ready recorder consumes the value; loss of focus or host
    /// shutdown cancels that local interaction and returns false to the caller.
    /// @pre Called on the initialized GLFW main thread while this host lives.
    [[nodiscard]] bool try_record_hotkey(core::HotkeyChord chord) noexcept;
    /// @brief Drive the controller until application shutdown and ownership disposition.
    ///
    /// Borrow application for the entire call. Each iteration pumps native events,
    /// progresses prior controller work and advances the application. UI cadence
    /// separately schedules snapshot rendering: active interaction uses a 16.667 ms
    /// period, idle visibility uses 100 ms, and hidden input is synchronized on
    /// demand. Commands emitted by a rendered frame force another pumped iteration.
    /// Application, graphics and UI deadlines contribute independently to the wait.
    /// Controller failure still drives application shutdown without requiring a
    /// drawable UI. The returned disposition, not merely the exit code, determines
    /// whether this host and the other composed owners may be destroyed.
    [[nodiscard]] HostRunResult run(app::Application &application) noexcept;

  private:
    class Impl;

    explicit DesktopHost(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
  };

}    // namespace avb::desktop

#endif    // AVB0795_DESKTOP_DESKTOP_HOST_HPP
