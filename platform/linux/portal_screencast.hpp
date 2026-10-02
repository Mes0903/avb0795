/// @file
/// @brief Native Portal request, session and remote ownership before frame preparation.
#ifndef AVB0795_LINUX_PORTAL_SCREENCAST_HPP
#define AVB0795_LINUX_PORTAL_SCREENCAST_HPP
#include "avb/app/source_acquisition.hpp"
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
namespace avb::linux_platform {
  namespace detail { struct PortalSessionState; class PortalWorker; }
  /// @brief Exactly one local descriptor owner; release transfers native ownership explicitly.
  /// This owner can be destroyed on either thread. It never closes a Portal session.
  class PortalUniqueFd final {
  public:
    explicit PortalUniqueFd(int fd = -1) noexcept : fd_{fd} {}
    ~PortalUniqueFd();
    PortalUniqueFd(const PortalUniqueFd &) = delete;
    PortalUniqueFd &operator=(const PortalUniqueFd &) = delete;
    PortalUniqueFd(PortalUniqueFd &&) noexcept;
    PortalUniqueFd &operator=(PortalUniqueFd &&) noexcept;
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept;
  private:
    int fd_;
  };
  /// @brief Lifetime-only session lease shared by consumers; it grants no Close authority.
  /// Closed leases retain terminal owned facts and no service, thread, transport or wake borrow.
  /// Validity is an observation: the native owner rechecks it before accepting a remote.
  class PortalSessionLease final {
  public:
    PortalSessionLease() = default;
    [[nodiscard]] bool valid() const noexcept;
    /// @brief Observe acknowledged session cleanup without retaining its native owner.
    /// Invalidity alone only forbids use: Close submission invalidates immediately.
    /// Retirement requires confirmed session closure, its Close callback and Closed
    /// subscription destroy acknowledgment. Uncertain Close or owner/transport loss
    /// additionally waits for actual GIO transport closure and those same borrows.
    /// Empty leases are retired; healthy retained sessions are not. True is monotonic.
    /// This atomic observation is safe on either thread and grants no Close authority;
    /// operation/capture retirement and service quiescence remain separate obligations.
    [[nodiscard]] bool retired() const noexcept;

  private:
    explicit PortalSessionLease(std::shared_ptr<detail::PortalSessionState> state) : state_{std::move(state)} {}
    std::shared_ptr<detail::PortalSessionState> state_;
    friend class detail::PortalWorker;
    friend class PortalScreencast;
  };
  /// @brief Registry-only explicit session cleanup authority, separate from consumer lifetime.
  /// request_close submits nonblocking cleanup to the owning worker. Destruction alone
  /// does not Close; service shutdown closes every retained session and acknowledges retirement.
  /// After terminal closure this value cannot reactivate the worker or borrow its wake.
  class PortalSessionCloseAuthority final {
  public:
    PortalSessionCloseAuthority() = default;
    PortalSessionCloseAuthority(const PortalSessionCloseAuthority &) = delete;
    PortalSessionCloseAuthority &operator=(const PortalSessionCloseAuthority &) = delete;
    PortalSessionCloseAuthority(PortalSessionCloseAuthority &&) noexcept = default;
    PortalSessionCloseAuthority &operator=(PortalSessionCloseAuthority &&) noexcept = default;
    void request_close() noexcept;
  private:
    PortalSessionCloseAuthority(std::weak_ptr<detail::PortalWorker>, std::shared_ptr<detail::PortalSessionState>);
    std::weak_ptr<detail::PortalWorker> worker_;
    std::shared_ptr<detail::PortalSessionState> session_;
    friend class detail::PortalWorker;
  };
  /// @brief A newly opened remote, with one FD and the started session's selected stream identity.
  /// This is native preparation input, not proof that any frame is available.
  struct PortalStreamConnection {
    PortalUniqueFd fd;
    std::uint32_t node_id{};
    std::optional<std::uint64_t> serial;
    PortalSessionLease session;
  };
  /// @brief Reopen only a valid already-started session; never select or Start again.
  struct PortalResumeTarget {
    PortalSessionLease session;
    std::uint32_t node_id{};
    std::optional<std::uint64_t> serial;
  };
  /// @brief Observed read-only protocol properties for later platform composition.
  /// Probing creates no requests, sessions or chooser and is separate from acquisition.
  struct PortalCapabilities { std::uint32_t version{}, available_source_types{}; };
  /// @brief Synchronous startup-only read of Portal capability properties, without a chooser.
  /// Resolves the session bus address only when no explicit address is supplied;
  /// an explicit address never falls back to another bus. A private connection is
  /// created, queried and synchronously closed/unreferenced before return. Callers
  /// must allow blocking address resolution, connection and retirement: the five
  /// second timeout applies to GetAll alone, not to this complete sequence.
  /// Resolution/connection failure, missing service and malformed or unsupported
  /// properties yield SourceUnavailable; AccessDenied yields PermissionDenied and
  /// other method failures yield PlatformOperationFailure, preserving native error
  /// detail when available. No acquisition owner or worker is created by this probe.
  [[nodiscard]] std::expected<PortalCapabilities, core::AppError>
  probe_portal_capabilities(const std::string &bus_address = {});
  /// @brief Bounded native facts consumed on the main thread, bound to the original request.
  /// A fresh result transfers distinct Close authority and lifetime lease. Publication
  /// retains its acquisition identity/admission while the service owns the remote;
  /// transfer or fully acknowledged revocation retires that original operation.
  /// Quiescence
  /// after shutdown proves async completions, subscription destroy notifications,
  /// commands, undelivered remote cleanup, transport, wake retirement and worker exit;
  /// external consumers of terminal leases do not delay it. No permission countdown exists.
  struct PortalProgress {
    app::AcquisitionId id;
    app::AcquisitionPhase phase{app::AcquisitionPhase::Idle};
    std::optional<core::AppError> error;
    bool cancelled{};
    std::optional<PortalStreamConnection> connection;
    std::optional<PortalSessionCloseAuthority> close_authority;
    std::optional<std::chrono::steady_clock::time_point> next_wake;
    bool made_progress{}, quiescent{};
  };
  /// @brief Composition-owned asynchronous Portal service with a private GIO context.
  /// Constructor is inactive. Retain the complete owner before main-thread begin.
  /// All native dispatch runs on one lazy worker; wake runs there and may only signal
  /// the host. The parent identifier and optional explicit bus address are copied;
  /// empty address resolves the session bus only upon begin or explicit probing.
  /// Main-thread methods never dispatch GIO or wait for permission. Cancel preserves
  /// obsolete identity while late effects are cleaned. Native completion alone does
  /// not release admission: an undelivered remote is still owned by this service.
  /// progress transfers it after its callbacks retire and permits immediate resume;
  /// cancel instead emits one original-ID acknowledgment after cleanup. Shutdown closes admission and
  /// retained sessions. Keep the wake target alive and poll until quiescent before
  /// destruction; destruction with a live worker is a contract violation, not a blocking
  /// RAII cleanup path. progress joins only a worker already acknowledged as finished.
  class PortalScreencast final {
  public:
    PortalScreencast(std::string parent_identifier, std::function<void()> wake, std::string bus_address = {});
    ~PortalScreencast();
    PortalScreencast(const PortalScreencast &) = delete;
    PortalScreencast &operator=(const PortalScreencast &) = delete;
    [[nodiscard]] std::expected<void, core::AppError> begin(const app::SourceAcquisitionRequest &, std::optional<PortalResumeTarget> resume = std::nullopt);
    void cancel(app::AcquisitionId) noexcept;
    void request_shutdown() noexcept;
    [[nodiscard]] PortalProgress progress(std::chrono::steady_clock::time_point) noexcept;
  private:
    std::shared_ptr<detail::PortalWorker> worker_;
  };
}
#endif
