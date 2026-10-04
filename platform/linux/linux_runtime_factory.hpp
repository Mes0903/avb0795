/// @file
/// @brief Prepare an inactive X11 aggregate before nonthrowing capture publication.
#ifndef AVB0795_LINUX_RUNTIME_FACTORY_HPP
#define AVB0795_LINUX_RUNTIME_FACTORY_HPP
#include "avb/app/ports.hpp"
#include "platform/linux/x11_source_catalog.hpp"
#include <functional>
namespace avb::diagnostics {
  class PerformanceReport;
}
namespace avb::linux_platform {
  namespace detail {
    /// @brief Reject malformed complete settings before catalog or native work.
    /// Crop dimensions must be positive; current source bounds are certified later
    /// by retired presentation. Saved extension preferences remain accepted values.
    [[nodiscard]] bool valid_initial_settings(const core::ReplicaSettings &) noexcept;
  }
  /// @brief Main-thread factory borrowing the catalog and host wake lifetimes.
  /// create resolves the exact observation, prepares its independently watched
  /// inactive capture and all window/graphics/runtime storage, acknowledges the
  /// exact initial ring, then resolves the SAME identity before activation. The
  /// original watched target is never replaced by a second lookup. Once start
  /// succeeds, only nonthrowing publication remains; worker failures belong to
  /// runtime tick/drain. Catalog, wake target and diagnostics outlive the runtime.
  class LinuxRuntimeFactory final : public app::IReplicaRuntimeFactory {
  public:
    LinuxRuntimeFactory(X11SourceCatalog &, std::function<void()> wake, diagnostics::PerformanceReport * = nullptr);
    [[nodiscard]] std::expected<std::unique_ptr<app::IReplicaRuntime>, core::AppError>
    create(const core::SourceDescriptor &, const core::ReplicaSettings &) override;

  private:
    X11SourceCatalog &catalog_;
    std::function<void()> wake_;
    diagnostics::PerformanceReport *performance_{};
    std::uint64_t next_observation_instance_{1};
  };
}    // namespace avb::linux_platform
#endif
