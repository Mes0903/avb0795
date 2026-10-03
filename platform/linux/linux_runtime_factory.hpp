/// @file
/// @brief Publish a completely prepared Linux aggregate after atomic capture binding.
#ifndef AVB0795_LINUX_RUNTIME_FACTORY_HPP
#define AVB0795_LINUX_RUNTIME_FACTORY_HPP
#include "avb/app/ports.hpp"
#include "platform/linux/linux_source_registry.hpp"
#include <functional>
namespace avb::diagnostics {
  class PerformanceReport;
}
namespace avb::linux_platform {
  /// @brief Main-thread factory borrowing registry and host wake lifetimes.
  /// Construction is inactive. create borrows only a current legal prepared frame,
  /// constructs every fallible window/graphics/runtime owner before final commit,
  /// and schedules rejection cleanup without destructing a live producer on failure.
  /// Registry, wake target and optional diagnostics outlive every published runtime.
  class LinuxRuntimeFactory final : public app::IReplicaRuntimeFactory {
  public:
    LinuxRuntimeFactory(LinuxSourceRegistry &, std::function<void()> wake, diagnostics::PerformanceReport * = nullptr);
    [[nodiscard]] std::expected<std::unique_ptr<app::IReplicaRuntime>, core::AppError>
    create(const core::SourceDescriptor &, const core::ReplicaSettings &) override;

  private:
    LinuxSourceRegistry &registry_;
    std::function<void()> wake_;
    diagnostics::PerformanceReport *performance_{};
    std::uint64_t next_observation_instance_{1};
  };
}    // namespace avb::linux_platform
#endif
