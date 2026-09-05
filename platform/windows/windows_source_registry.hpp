/// @file
/// @brief Generation-scoped source IDs, runtime leases, and shared Closed observations.
/// Native implementation details remain below the portable application ports.

#ifndef AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP
#define AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP

#include "airelinna/core/types.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace airelinna::platform::windows {

  namespace detail {
    class ItemObservationState;
    struct WindowsSourceRegistryControl;

    /// Produces opaque IDs whose prefix rejects selections from an old refresh.
    class WindowsSourceIdGeneration final {
    public:
      WindowsSourceIdGeneration();

      /// Issue the next ID in this snapshot; native handles are never encoded.
      [[nodiscard]] core::SourceId issue();
      /// Invalidate every previous ID before accepting another snapshot.
      void advance();
      /// Validate the generation only; the registry separately checks existence.
      [[nodiscard]] std::expected<void, core::AppError>
      validate(const core::SourceId &id) const noexcept;

    private:
      void rebuild_prefix();

      std::uint64_t generation_{};
      std::uint64_t next_ordinal_{};
      std::string prefix_;
    };

  }    // namespace detail

  class WindowsCaptureSource;

  /// Move-only runtime lease on the exact catalog-published item observation.
  /// The shared control block prevents refresh while any lease is retained.
  class WindowsSourceLease final {
  public:
    ~WindowsSourceLease();

    WindowsSourceLease(const WindowsSourceLease &) = delete;
    WindowsSourceLease &operator=(const WindowsSourceLease &) = delete;
    WindowsSourceLease(WindowsSourceLease &&other) noexcept;
    WindowsSourceLease &operator=(WindowsSourceLease &&other) noexcept;

  private:
    friend class WindowsCaptureSource;
    friend class WindowsSourceRegistry;
    [[nodiscard]] detail::ItemObservationState &observation() const noexcept;
    WindowsSourceLease(
      std::shared_ptr<detail::WindowsSourceRegistryControl> control,
      std::shared_ptr<detail::ItemObservationState> observation) noexcept;
    void release() noexcept;

    std::shared_ptr<detail::WindowsSourceRegistryControl> control_;
    std::shared_ptr<detail::ItemObservationState> observation_;
  };

  /// Owns one source snapshot; refresh and lookup are serialized by its control.
  /// COM identities stay here rather than being reconstructed from recycled HWNDs.
  class WindowsSourceRegistry final {
  public:
    /// Exclusive refresh transaction; destruction cancels an uncommitted refresh.
    class Refresh final {
    public:
      ~Refresh();

      Refresh(const Refresh &) = delete;
      Refresh &operator=(const Refresh &) = delete;
      Refresh(Refresh &&other) noexcept;
      Refresh &operator=(Refresh &&other) noexcept;

      /// Publish observations and return matching IDs in their input order.
      [[nodiscard]] std::expected<std::vector<core::SourceId>, core::AppError>
      commit(std::vector<std::shared_ptr<detail::ItemObservationState>>
               observations);

    private:
      friend class WindowsSourceRegistry;
      explicit Refresh(WindowsSourceRegistry &owner) noexcept;
      void cancel() noexcept;

      WindowsSourceRegistry *owner_{};
    };

    WindowsSourceRegistry();
    ~WindowsSourceRegistry();

    WindowsSourceRegistry(const WindowsSourceRegistry &) = delete;
    WindowsSourceRegistry &operator=(const WindowsSourceRegistry &) = delete;
    WindowsSourceRegistry(WindowsSourceRegistry &&) = delete;
    WindowsSourceRegistry &operator=(WindowsSourceRegistry &&) = delete;

    /// Reject refresh while a runtime lease or another refresh is active.
    [[nodiscard]] std::expected<Refresh, core::AppError> begin_refresh();
    /// Resolve a current ID into an owning lease without recreating its item.
    [[nodiscard]] std::expected<WindowsSourceLease, core::AppError>
    resolve(const core::SourceId &id) const;

  private:
    [[nodiscard]] std::expected<std::vector<core::SourceId>, core::AppError>
    commit_refresh(
      std::vector<std::shared_ptr<detail::ItemObservationState>> observations);
    void cancel_refresh() noexcept;

    std::shared_ptr<detail::WindowsSourceRegistryControl> control_;
  };

}    // namespace airelinna::platform::windows

#endif    // AIRELINNA_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP
