/// @file
/// @brief Construction and publication of the complete Windows replica aggregate.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP

#include "airelinna/app/ports.hpp"
#include "platform/windows/windows_source_registry.hpp"
#include "render/bridge_policy.hpp"

#include <functional>

namespace airelinna::platform::windows {

  /// Composition adapter that borrows discovery identity and constructs one runtime.
  /// Partial native owners remain local until StartCapture guarantees publication.
  class WindowsRuntimeFactory final : public app::IReplicaRuntimeFactory {
  public:
    using WakeMainLoop = std::function<void()>;

    /// Retain a wake callable, not an application event sink or session reference.
    WindowsRuntimeFactory(WindowsSourceRegistry &registry,
                          WakeMainLoop wake_main_loop,
                          render::WindowsBridgePreference bridge_preference =
                            render::WindowsBridgePreference::Automatic);

    /// Resolve once, assemble in dependency order, then publish the complete owner.
    /// The factory-local guard preserves subclass state during failure unwind.
    [[nodiscard]] std::expected<std::unique_ptr<app::IReplicaRuntime>,
                                core::AppError>
    create(const core::SourceDescriptor &source,
           const core::ReplicaSettings &settings) override;

  private:
    WindowsSourceRegistry &registry_;
    WakeMainLoop wake_main_loop_;
    render::WindowsBridgePreference bridge_preference_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP
