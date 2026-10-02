/// @file
/// @brief Temporary capture-free services for the Linux controller milestone.
#ifndef AVB0795_LINUX_UNAVAILABLE_SERVICES_HPP
#define AVB0795_LINUX_UNAVAILABLE_SERVICES_HPP

#include "avb/app/ports.hpp"

namespace avb::linux_platform {
  /// @brief Supply an empty owning source snapshot without requesting authorization.
  /// Main-thread enumeration has no native observation, callback or worker owner,
  /// so refresh remains harmless while capture capabilities are false. Task 9
  /// replaces this temporary service with the real source registry/catalog and
  /// removes this header; an empty snapshot is not discovery failure or capture support.
  class LinuxUnavailableSourceCatalog final : public app::ISourceCatalog {
  public:
    [[nodiscard]] std::expected<std::vector<core::SourceDescriptor>, core::AppError>
    enumerate() override
    {
      return std::vector<core::SourceDescriptor>{};
    }
  };

  /// @brief Refuse runtime construction until actual Linux acquisition is composed.
  /// Compose on the main thread with both capture capabilities false and no
  /// acquisition port. Even a direct call returns a typed construction failure
  /// without retaining the borrowed source/settings or creating native owners.
  /// There is no partial runtime or wake borrower to retire on failure/destruction.
  /// Task 9 replaces this service and removes this temporary header.
  class LinuxUnavailableRuntimeFactory final : public app::IReplicaRuntimeFactory {
  public:
    [[nodiscard]] std::expected<std::unique_ptr<app::IReplicaRuntime>, core::AppError>
    create(const core::SourceDescriptor &, const core::ReplicaSettings &) override
    {
      return std::unexpected{core::AppError{core::AppErrorCode::RuntimeConstructionFailure}};
    }
  };
}    // namespace avb::linux_platform

#endif
