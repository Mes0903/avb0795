/// @file
/// @brief Pure operation identity and protocol-result rules, independent of dispatch.
#ifndef AVB0795_LINUX_ACQUISITION_POLICY_HPP
#define AVB0795_LINUX_ACQUISITION_POLICY_HPP
#include "avb/app/source_acquisition.hpp"
#include <optional>
namespace avb::linux_platform::detail {
  /// @brief Response 1 acknowledges a user cancellation; other failures are not evidence of denial.
  enum class ResponseDisposition { Success, Cancelled, Failure };
  [[nodiscard]] ResponseDisposition response_disposition(unsigned) noexcept;
  /// @brief Serial identity is usable only when the observed protocol version supports it.
  [[nodiscard]] std::optional<std::uint64_t> stream_serial(unsigned, std::optional<std::uint64_t>) noexcept;
  /// @brief Worker-owned admission rule retaining cancelled identity until native retirement.
  /// No callback may accept an obsolete operation, a closed session, or shutdown work.
  /// Retirement is supplied by the native owner after cleanup, never inferred here.
  class AcquisitionPolicy {
  public:
    void begin(app::AcquisitionId id) noexcept { id_ = id; active_ = true; cancelled_ = false; }
    void cancel(app::AcquisitionId id) noexcept { if (active_ && id == id_) cancelled_ = true; }
    void shutdown() noexcept { stopping_ = true; }
    void retire() noexcept { active_ = false; }
    [[nodiscard]] bool accepts(app::AcquisitionId id, bool live) const noexcept
    { return active_ && !cancelled_ && !stopping_ && id == id_ && live; }
    [[nodiscard]] app::AcquisitionId identity() const noexcept { return id_; }
  private:
    app::AcquisitionId id_{};
    bool active_{}, cancelled_{}, stopping_{};
  };
}
#endif
