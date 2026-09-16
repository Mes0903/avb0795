/// @file
/// @brief Construction and publication of the complete Windows replica aggregate.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP

#include "avb/app/ports.hpp"
#include "platform/windows/windows_source_registry.hpp"
#include "platform/windows/detail/bridge_policy.hpp"

#include <functional>

namespace avb::diagnostics { class PerformanceReport; }

namespace avb::platform::windows {

  /// @brief Assemble a complete native runtime around one resolved catalog identity.
  ///
  /// The factory borrows its registry and copies a wake callable into produced
  /// owners. The registry must outlive factory use; anything the callable borrows
  /// must survive each published runtime's full drain. Partial owners stay local
  /// until native capture start commits the aggregate to published lifetime rules.
  class WindowsRuntimeFactory final : public app::IReplicaRuntimeFactory {
  public:
    using WakeMainLoop = std::function<void()>;

    /// Retain a wake callable, not an application event sink or session reference.
    WindowsRuntimeFactory(WindowsSourceRegistry &registry,
                          WakeMainLoop wake_main_loop,
                          render::WindowsBridgePreference bridge_preference =
                            render::WindowsBridgePreference::Automatic,
                          diagnostics::PerformanceReport *performance = nullptr);

    /// @brief Resolve once, construct in dependency order, and publish after start.
    ///
    /// The descriptor's extent is only an initial window-sizing hint: capture
    /// allocation is read again from the retained item before graphics slots are
    /// built. Recoverable construction failures return typed errors and unwind
    /// local owners under their destruction invariants; the attachment guard
    /// keeps callback state alive if window destruction is needed. Success
    /// transfers the complete runtime, not a promise of first-frame
    /// delivery or continued source availability.
    [[nodiscard]] std::expected<std::unique_ptr<app::IReplicaRuntime>,
                                core::AppError>
    create(const core::SourceDescriptor &source,
           const core::ReplicaSettings &settings) override;

  private:
    WindowsSourceRegistry &registry_;
    WakeMainLoop wake_main_loop_;
    render::WindowsBridgePreference bridge_preference_;
    // Borrowed from composition through every produced runtime's full drain.
    diagnostics::PerformanceReport *performance_{};
    std::uint64_t next_observation_instance_{1};
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP
