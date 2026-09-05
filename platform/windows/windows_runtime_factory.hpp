#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP

#include "airelinna/app/ports.hpp"
#include "platform/windows/windows_source_registry.hpp"
#include "render/windows_render_stack.hpp"

#include <functional>

namespace airelinna::platform::windows {

class WindowsRuntimeFactory final : public app::IReplicaRuntimeFactory {
public:
  using WakeMainLoop = std::function<void()>;

  WindowsRuntimeFactory(WindowsSourceRegistry &registry,
                        WakeMainLoop wake_main_loop,
                        render::WindowsBridgePreference bridge_preference =
                            render::WindowsBridgePreference::Automatic);

  [[nodiscard]] std::expected<std::unique_ptr<app::IReplicaRuntime>,
                              core::AppError>
  create(const core::SourceDescriptor &source,
         const core::ReplicaSettings &settings) override;

private:
  WindowsSourceRegistry &registry_;
  WakeMainLoop wake_main_loop_;
  render::WindowsBridgePreference bridge_preference_;
};

} // namespace airelinna::platform::windows

#endif // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_RUNTIME_FACTORY_HPP
