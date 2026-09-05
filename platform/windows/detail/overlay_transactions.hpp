#ifndef AIRELINNA_PLATFORM_WINDOWS_DETAIL_OVERLAY_TRANSACTIONS_HPP
#define AIRELINNA_PLATFORM_WINDOWS_DETAIL_OVERLAY_TRANSACTIONS_HPP

#include "airelinna/core/types.hpp"
#include "platform/windows/detail/overlay_policy.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>

namespace airelinna::platform::windows::detail {

  enum class OverlayPropertyRemoval {
    RemovedExpected,
    AbsentUnexpected,
    StillPresent,
  };

  enum class FailedWndProcInstallObservation {
    Original,
    Custom,
    Unavailable,
  };

  struct FailedAttachmentCleanup final {
    bool native_binding_safe{};
    bool destroy_with_adapter_alive{};
  };

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

  struct NativeWindowState final {
    std::uintptr_t extended_style{};
    std::optional<LayeredAttributes> layered_attributes;

    friend bool operator==(const NativeWindowState &,
                           const NativeWindowState &) = default;
  };

  [[nodiscard]] constexpr core::AppError
  overlay_transaction_error(const core::AppErrorCode code) noexcept
  {
    return {.code = code};
  }

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

  template <class Operations>
  [[nodiscard]] bool
  native_window_state_matches(Operations &operations,
                              const NativeWindowState &expected) noexcept
  {
    const auto observed = capture_native_window_state(operations);
    return observed && *observed == expected;
  }

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

  template <class Operations>
  [[nodiscard]] std::expected<NativeWindowState, core::AppError>
  enter_pass_through(Operations &operations) noexcept
  {
    const auto snapshot = capture_native_window_state(operations);
    if (!snapshot) {
      return std::unexpected{snapshot.error()};
    }
    NativeWindowState target{
      .extended_style = input_transparency_style(
        snapshot->extended_style, core::OverlayMode::PassThrough),
      .layered_attributes = opaque_layered_attributes,
    };
    if (!apply_native_window_state(operations, target) ||
        !native_window_state_matches(operations, target)) {
      return std::unexpected{rollback_native_window_state(operations, *snapshot)};
    }
    return *snapshot;
  }

  template <class Operations>
  [[nodiscard]] std::expected<void, core::AppError>
  leave_pass_through(Operations &operations,
                     const NativeWindowState &snapshot) noexcept
  {
    const auto current = capture_native_window_state(operations);
    if (!current) {
      return std::unexpected{current.error()};
    }
    if (!apply_native_window_state(operations, snapshot) ||
        !native_window_state_matches(operations, snapshot)) {
      return std::unexpected{rollback_native_window_state(operations, *current)};
    }
    return {};
  }

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

    void arm() noexcept { armed_ = true; }
    void disarm() noexcept { armed_ = false; }

  private:
    Operations &operations_;
    bool armed_{};
  };

}    // namespace airelinna::platform::windows::detail

#endif    // AIRELINNA_PLATFORM_WINDOWS_DETAIL_OVERLAY_TRANSACTIONS_HPP
