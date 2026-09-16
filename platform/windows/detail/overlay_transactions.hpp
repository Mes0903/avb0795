/// @file
/// @brief Verified native-state transactions and factory-local attachment cleanup.
/// Native implementation details remain below the portable application ports.

#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_TRANSACTIONS_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_TRANSACTIONS_HPP

#include "avb/core/types.hpp"
#include "platform/windows/detail/overlay_policy.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>

namespace avb::platform::windows::detail {

  /// Observed property cleanup result; missing and still-bound are different failures.
  enum class OverlayPropertyRemoval {
    RemovedExpected,
    AbsentUnexpected,
    StillPresent,
  };

  /// Which procedure remains installed after a failed subclass installation attempt.
  enum class FailedWndProcInstallObservation {
    Original,
    Custom,
    Unavailable,
  };

  /// Whether callback reachability is safe or HWND must die while adapter state lives.
  struct FailedAttachmentCleanup final {
    bool native_binding_safe{};
    bool destroy_with_adapter_alive{};
  };

  /// Select conservative failure cleanup from actual procedure/property observations.
  [[nodiscard]] constexpr FailedAttachmentCleanup
  failed_attachment_cleanup(const FailedWndProcInstallObservation procedure,
                            const OverlayPropertyRemoval property) noexcept
  {
    if (procedure != FailedWndProcInstallObservation::Original) {
      return {.native_binding_safe = false, .destroy_with_adapter_alive = true};
    }
    return {
      .native_binding_safe = property != OverlayPropertyRemoval::StillPresent,
      .destroy_with_adapter_alive =
        property == OverlayPropertyRemoval::StillPresent,
    };
  }

  /// Exact restorable color-key/alpha/flags snapshot for an existing layered window.
  struct LayeredAttributes final {
    std::uint32_t color_key{};
    std::uint8_t alpha{};
    std::uint32_t flags{};

    friend bool operator==(const LayeredAttributes &,
                           const LayeredAttributes &) = default;
  };

  inline constexpr LayeredAttributes opaque_layered_attributes{
    .color_key = 0,
    .alpha = 255,
    .flags = layered_alpha_flag,
  };

  /// Complete instantaneous native state for verified rollback of one operation.
  struct NativeWindowState final {
    std::uintptr_t extended_style{};
    std::optional<LayeredAttributes> layered_attributes;

    friend bool operator==(const NativeWindowState &,
                           const NativeWindowState &) = default;
  };

  /// Construct fixed-size failure data suitable for callback/no-throw paths.
  [[nodiscard]] constexpr core::AppError
  overlay_transaction_error(const core::AppErrorCode code) noexcept
  {
    return {.code = code};
  }

  /// Commit the new DPI only after the suggested physical rectangle was applied.
  template <class ApplyRectangle>
  [[nodiscard]] bool apply_dpi_change_transaction(
    std::uint32_t &current_dpi, const std::uint32_t observed_dpi, const NativeRect rectangle, ApplyRectangle &&apply_rectangle) noexcept
  {
    const auto width =
      static_cast<std::int64_t>(rectangle.right) - rectangle.left;
    const auto height =
      static_cast<std::int64_t>(rectangle.bottom) - rectangle.top;
    if (width <= 0 || height <= 0 || width > (std::numeric_limits<int>::max)() ||
        height > (std::numeric_limits<int>::max)()) {
      return false;
    }
    try {
      if (!apply_rectangle(rectangle.left, rectangle.top, static_cast<int>(width),
                           static_cast<int>(height))) {
        return false;
      }
    } catch (...) {
      return false;
    }
    current_dpi = observed_dpi == 0 ? 96 : observed_dpi;
    return true;
  }

  /// Reject an unreadable layered snapshot before making any style mutation.
  template <class Operations>
  [[nodiscard]] std::expected<NativeWindowState, core::AppError>
  capture_native_window_state(Operations &operations) noexcept
  {
    try {
      const auto style = operations.read_extended_style();
      if (!style) {
        return std::unexpected{style.error()};
      }
      NativeWindowState state{.extended_style = *style};
      if (has_layered_style(*style)) {
        const auto attributes = operations.read_layered_attributes();
        if (!attributes) {
          return std::unexpected{attributes.error()};
        }
        state.layered_attributes = *attributes;
      }
      return state;
    } catch (...) {
      return std::unexpected{
        overlay_transaction_error(core::AppErrorCode::RuntimeStateLost)};
    }
  }

  /// Apply the complete snapshot, including opaque attributes when layered.
  template <class Operations>
  [[nodiscard]] bool
  apply_native_window_state(Operations &operations,
                            const NativeWindowState &state) noexcept
  {
    try {
      if (!operations.write_extended_style(state.extended_style)) {
        return false;
      }
      if (has_layered_style(state.extended_style)) {
        if (!state.layered_attributes ||
            !operations.write_layered_attributes(*state.layered_attributes)) {
          return false;
        }
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  /// Read native state back; a successful setter alone is not transaction proof.
  template <class Operations>
  [[nodiscard]] bool
  native_window_state_matches(Operations &operations,
                              const NativeWindowState &expected) noexcept
  {
    const auto observed = capture_native_window_state(operations);
    return observed && *observed == expected;
  }

  /// Distinguish verified rollback from RuntimeStateLost after unverified recovery.
  template <class Operations>
  [[nodiscard]] core::AppError
  rollback_native_window_state(Operations &operations,
                               const NativeWindowState &snapshot) noexcept
  {
    static_cast<void>(apply_native_window_state(operations, snapshot));
    const auto recovered = native_window_state_matches(operations, snapshot);
    return overlay_transaction_error(
      recovered ? core::AppErrorCode::PlatformOperationFailure
                : core::AppErrorCode::RuntimeStateLost);
  }

  /// Derive owned input/alpha state from current decoration bits. Percent is validated by callers.
  [[nodiscard]] constexpr NativeWindowState presentation_window_state(
    const std::uintptr_t current_style, const core::OverlayMode mode,
    const int opacity_percent) noexcept
  {
    NativeWindowState target{
      .extended_style = input_transparency_style(current_style, mode)};
    if (mode == core::OverlayMode::PassThrough || opacity_percent < 100) {
      target.extended_style |= layered_style_bit;
      target.layered_attributes = opaque_layered_attributes;
      target.layered_attributes->alpha =
        static_cast<std::uint8_t>((opacity_percent * 255 + 50) / 100);
    }
    return target;
  }

  /// @brief Apply input mode and whole-window alpha as one verified native transaction.
  /// Independent border EXSTYLE bits come from the current snapshot. Failure restores
  /// that exact snapshot, including color key/alpha flags; unverifiable rollback is
  /// RuntimeStateLost. Callers publish their cached mode and opacity only on success.
  template <class Operations>
  [[nodiscard]] std::expected<void, core::AppError>
  set_native_presentation(Operations &operations, const core::OverlayMode mode,
                          const int opacity_percent) noexcept
  {
    if (opacity_percent < 0 || opacity_percent > 100) {
      return std::unexpected{overlay_transaction_error(core::AppErrorCode::InvalidValue)};
    }
    const auto current = capture_native_window_state(operations);
    if (!current) {
      return std::unexpected{current.error()};
    }
    const auto target = presentation_window_state(current->extended_style, mode,
                                                  opacity_percent);
    if (*current == target) {
      return {};
    }
    if (!apply_native_window_state(operations, target) ||
        !native_window_state_matches(operations, target)) {
      return std::unexpected{rollback_native_window_state(operations, *current)};
    }
    return {};
  }

  /// @brief Commit stored settings only after the complete native change verifies.
  ///
  /// Operations supplies borrowed, synchronous access to the concrete owners.
  /// Normal failure attempts rollback and distinguishes verified restoration
  /// from RuntimeStateLost. Exceptions preserve the stronger state-lost result
  /// even if a best-effort rollback appears to succeed, because the failed
  /// operation may have left effects outside the captured proof.
  template <class Operations>
  [[nodiscard]] std::expected<void, core::AppError>
  apply_settings_transaction(Operations &operations) noexcept
  {
    using Snapshot = typename Operations::Snapshot;
    std::optional<Snapshot> snapshot;
    try {
      snapshot = operations.snapshot();
      if (!snapshot) {
        return std::unexpected{overlay_transaction_error(
          core::AppErrorCode::PlatformOperationFailure)};
      }
      const auto applied = operations.apply_mode() && operations.apply_border() &&
                           operations.apply_geometry() &&
                           operations.apply_extensions() &&
                           operations.verify_applied();
      if (applied) {
        operations.commit_scheduler();
        return {};
      }
      static_cast<void>(operations.rollback(*snapshot));
      const auto recovered = operations.verify_rollback(*snapshot);
      return std::unexpected{overlay_transaction_error(
        recovered ? core::AppErrorCode::PlatformOperationFailure
                  : core::AppErrorCode::RuntimeStateLost)};
    } catch (...) {
      if (snapshot) {
        try {
          static_cast<void>(operations.rollback(*snapshot));
          static_cast<void>(operations.verify_rollback(*snapshot));
        } catch (...) {
        }
      }
      return std::unexpected{
        overlay_transaction_error(core::AppErrorCode::RuntimeStateLost)};
    }
  }

  /// @brief Remove callback reachability before discarding its context property.
  ///
  /// Restoring the procedure happens first so native dispatch cannot observe the
  /// custom procedure without context. An unexpected removed property or replaced
  /// procedure requires window destruction with the adapter alive; a setter's
  /// successful return alone is insufficient evidence to free callback state.
  template <class Operations>
  [[nodiscard]] std::expected<void, core::AppError>
  detach_overlay_subclass(Operations &operations) noexcept
  {
    bool callback_detached{};
    try {
      const auto restored = operations.restore_window_proc();
      if (!restored) {
        return std::unexpected{restored.error()};
      }

      operations.publish_callback_detached();
      callback_detached = true;
      const auto property = operations.remove_and_observe_property();
      if (*restored && property == OverlayPropertyRemoval::RemovedExpected) {
        return {};
      }

      operations.require_window_destruction();
      return std::unexpected{overlay_transaction_error(
        *restored && property == OverlayPropertyRemoval::AbsentUnexpected
          ? core::AppErrorCode::PlatformOperationFailure
          : core::AppErrorCode::RuntimeStateLost)};
    } catch (...) {
      if (callback_detached) {
        try {
          operations.require_window_destruction();
        } catch (...) {
        }
      }
      return std::unexpected{
        overlay_transaction_error(core::AppErrorCode::RuntimeStateLost)};
    }
  }

  /// @brief Keep partial construction cleanup ordered around native callback reachability.
  ///
  /// The guard borrows an operations object and its overlay/adapter pair, all of
  /// which must outlive its scope. Failed restoration destroys the window while
  /// callback context is still valid. The guard follows ownership transferred
  /// into a still-local aggregate and is disarmed only after publication succeeds.
  template <class Operations>
  class FactoryOverlayAttachmentGuard final {
  public:
    explicit FactoryOverlayAttachmentGuard(Operations &operations) noexcept
        : operations_{operations} {}

    ~FactoryOverlayAttachmentGuard() noexcept
    {
      bool destroy_required{};
      try {
        destroy_required = operations_.destroy_with_adapter_alive_required();
      } catch (...) {
      }
      if (!armed_ && !destroy_required) {
        return;
      }
      bool detached{};
      if (!destroy_required) {
        try {
          detached = operations_.detach_adapter();
        } catch (...) {
        }
      }
      if (!detached) {
        bool destroyed{};
        try {
          operations_.destroy_overlay();
          destroyed = true;
        } catch (...) {
        }
        if (destroyed) {
          try {
            operations_.complete_adapter_after_overlay_destroyed();
          } catch (...) {
          }
        }
      }
    }

    FactoryOverlayAttachmentGuard(const FactoryOverlayAttachmentGuard &) = delete;
    FactoryOverlayAttachmentGuard &
    operator=(const FactoryOverlayAttachmentGuard &) = delete;
    FactoryOverlayAttachmentGuard(FactoryOverlayAttachmentGuard &&) = delete;
    FactoryOverlayAttachmentGuard &
    operator=(FactoryOverlayAttachmentGuard &&) = delete;

    /// Arm immediately after subclass installation, before any later fallible step.
    void arm() noexcept { armed_ = true; }
    /// Transfer cleanup responsibility only after the complete runtime is published.
    void disarm() noexcept { armed_ = false; }

  private:
    Operations &operations_;
    bool armed_{};
  };

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_OVERLAY_TRANSACTIONS_HPP
