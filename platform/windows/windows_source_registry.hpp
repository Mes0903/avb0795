/// @file
/// @brief Generation-scoped source IDs, runtime leases, and shared Closed observations.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP
#define AVB0795_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP

#include "avb/core/types.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace avb::platform::windows {

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

  /// @brief Retain the exact item observation selected from one catalog snapshot.
  ///
  /// Ownership transfers on move; destruction releases both the observation and
  /// its contribution to the registry's active-lease count. This count prevents
  /// refresh from replacing identities while capture or recovery still uses one.
  /// Keeping the control block alive does not keep the registry wrapper alive.
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

  /// @brief Own a snapshot of opaque IDs and their canonical capture identities.
  ///
  /// Refresh and lookup serialize through shared control. IDs belong to this
  /// registry's current generation; titles are display metadata, and recycled
  /// native window handles must never be used to reconstruct a selected item.
  /// Existing leases retain their observations independently of wrapper lifetime.
  class WindowsSourceRegistry final {
  public:
    /// @brief Exclude new lookups while a replacement snapshot is prepared.
    ///
    /// This move-only guard borrows its registry, which must outlive the guard.
    /// Destruction cancels an uncommitted refresh and restores lookup access to
    /// the previous snapshot; successful commit consumes the guard.
    class Refresh final {
    public:
      ~Refresh();

      Refresh(const Refresh &) = delete;
      Refresh &operator=(const Refresh &) = delete;
      Refresh(Refresh &&other) noexcept;
      Refresh &operator=(Refresh &&other) noexcept;

      /// @brief Replace the snapshot and return IDs in observation input order.
      ///
      /// Replacement containers are prepared before old observations retire.
      /// Commit advances the generation, so every ID from the previous snapshot
      /// becomes unusable even if a display label or native handle is unchanged.
      /// Allocation failures may throw; the guard cancels its refresh on failure.
      /// @pre Each observation is non-null and already has its close watcher.
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
    /// @brief Acquire a lease on an existing entry in the current generation.
    ///
    /// Refresh in progress, stale generations, and absent entries are rejected.
    /// Successful lookup preserves identity, but does not promise the source is
    /// still open: the capture owner must inspect the observation's sticky close.
    [[nodiscard]] std::expected<WindowsSourceLease, core::AppError>
    resolve(const core::SourceId &id) const;

  private:
    [[nodiscard]] std::expected<std::vector<core::SourceId>, core::AppError>
    commit_refresh(
      std::vector<std::shared_ptr<detail::ItemObservationState>> observations);
    void cancel_refresh() noexcept;

    std::shared_ptr<detail::WindowsSourceRegistryControl> control_;
  };

}    // namespace avb::platform::windows

#endif    // AVB0795_PLATFORM_WINDOWS_WINDOWS_SOURCE_REGISTRY_HPP
