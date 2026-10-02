/// @file
/// @brief Composition-owned native Portal admission, first-frame preparation and retirement.
#ifndef AVB0795_LINUX_SOURCE_ACQUISITION_HPP
#define AVB0795_LINUX_SOURCE_ACQUISITION_HPP
#include "avb/app/source_acquisition.hpp"
#include "platform/linux/linux_source_registry.hpp"
namespace avb::linux_platform {
  /// @brief Own the inactive Portal service and prepare concrete streams in the registry.
  /// Main-thread calls never wait for native loops or permission. Worker wake callbacks
  /// may only signal the host, which must remain alive until tick reports quiescent.
  /// Cancellation retains obsolete native owners through acknowledgments, preserves a
  /// healthy prior selection, and cannot authorize Ready. The ten-second deadline starts
  /// only at PreparingStream; permission waiting has no countdown. Shutdown closes idle
  /// sessions and excludes externally transferred consumer/GPU obligations from this clock.
  class LinuxSourceAcquisition final : public app::ISourceAcquisition {
  public:
    LinuxSourceAcquisition(LinuxSourceRegistry &, std::string parent_identifier, std::function<void()> wake);
    ~LinuxSourceAcquisition() override;
    std::expected<void, core::AppError> begin(const app::SourceAcquisitionRequest &) override;
    void cancel(app::AcquisitionId) noexcept override;
    void request_shutdown() noexcept override;
    app::SourceAcquisitionTickResult tick(std::chrono::steady_clock::time_point) noexcept override;

  private:
    LinuxSourceRegistry &registry_;
    std::string parent_;
    std::function<void()> wake_;
    std::unique_ptr<PortalScreencast> portal_;
    std::optional<app::SourceAcquisitionRequest> request_;
    LinuxSourceRegistry::Record *preparing_{};
    std::optional<std::chrono::steady_clock::time_point> first_frame_deadline_;
    std::optional<core::AppError> failure_;
    PortalSessionLease discarded_session_;
    app::AcquisitionPhase phase_{app::AcquisitionPhase::Idle};
    std::optional<app::AcquisitionPhase> announced_phase_;
    bool operation_retired_{}, cancel_submitted_{};
    bool cancelled_{}, shutdown_{}, portal_quiescent_{true}, portal_started_{};
  };
}    // namespace avb::linux_platform
#endif
