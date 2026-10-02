/// @file
/// @brief Feature-local helper visibility transactions and retirement policy.

#ifndef AVB0795_PLATFORM_WINDOWS_FEATURES_DETAIL_BROWSER_CAPTURE_KEEP_ALIVE_POLICY_HPP
#define AVB0795_PLATFORM_WINDOWS_FEATURES_DETAIL_BROWSER_CAPTURE_KEEP_ALIVE_POLICY_HPP

#include "avb/core/types.hpp"

#include <chrono>
#include <concepts>
#include <expected>
#include <optional>

namespace avb::platform::windows::features::detail {

  using Clock = std::chrono::steady_clock;
  inline constexpr auto helper_retirement_grace = std::chrono::milliseconds{250};

  /// @brief Runtime eligibility facts independent of native helper ownership.
  ///
  /// User preference alone is insufficient: a published tick owner, connected
  /// window source, and nonterminal runtime are required before showing a helper.
  struct HelperActivationFacts final {
    bool requested{};
    bool window_capture{};
    bool published_tick_ready{};
    bool source_connected{};
    bool stopping_or_terminal{};
  };

  [[nodiscard]] constexpr bool
  helper_should_be_active(const HelperActivationFacts &facts) noexcept
  {
    return facts.requested && facts.window_capture && facts.published_tick_ready &&
           facts.source_connected && !facts.stopping_or_terminal;
  }

  /// @brief Preserve visibility history needed to authorize delayed retirement.
  ///
  /// Even a show whose readback failed counts as ever shown. Only a verified
  /// hide establishes the timestamp from which the full grace interval runs;
  /// losing hidden-state evidence prevents time alone from permitting release.
  struct HelperRetirementState final {
    bool ever_shown{};
    bool hidden_verified{true};
    std::optional<Clock::time_point> last_hide;
  };

  [[nodiscard]] constexpr std::optional<Clock::time_point>
  helper_retirement_deadline(const HelperRetirementState &state) noexcept
  {
    if (!state.ever_shown || !state.last_hide) {
      return std::nullopt;
    }
    return *state.last_hide + helper_retirement_grace;
  }

  [[nodiscard]] constexpr bool helper_may_retire(
    const HelperRetirementState &state, Clock::time_point now) noexcept
  {
    if (!state.hidden_verified) {
      return false;
    }
    if (!state.ever_shown) {
      return true;
    }
    const auto deadline = helper_retirement_deadline(state);
    return deadline && now >= *deadline;
  }

  /// @brief Apply a visibility change and distinguish verified rollback from state loss.
  ///
  /// The operations seam borrows the native owner synchronously. Its no-throw
  /// return types still carry failure; each mutation needs explicit readback.
  /// Restoring the previous visible state is appropriate for a settings change,
  /// while terminal hiding uses the separate deactivation policy below.
  template <typename Operations>
    requires requires(Operations &operations, bool active) {
      { operations.observe_active() } noexcept -> std::same_as<std::expected<bool, core::AppError>>;
      { operations.prepare_hidden() } noexcept -> std::same_as<std::expected<void, core::AppError>>;
      { operations.write_active(active) } noexcept -> std::same_as<std::expected<void, core::AppError>>;
      { operations.matches_active(active) } noexcept -> std::same_as<bool>;
    }
  [[nodiscard]] std::expected<void, core::AppError>
  set_helper_active(Operations &operations, bool active) noexcept
  {
    const auto previous = operations.observe_active();
    if (!previous) {
      return std::unexpected{previous.error()};
    }
    if (active) {
      const auto prepared = operations.prepare_hidden();
      if (!prepared) {
        return std::unexpected{prepared.error()};
      }
    }
    const auto applied = operations.write_active(active);
    if (applied && operations.matches_active(active)) {
      return {};
    }
    (void) operations.write_active(*previous);
    const bool recovered = operations.matches_active(*previous);
    return std::unexpected{core::AppError{
      .code = recovered ? core::AppErrorCode::PlatformOperationFailure
                        : core::AppErrorCode::RuntimeStateLost}};
  }

  /// @brief Require verified hiding without restoring visibility after failure.
  ///
  /// Stop and source loss cannot safely roll back to an active helper. Any failed
  /// hide or readback therefore reports state loss and leaves ownership retained.
  template <typename Operations>
    requires requires(Operations &operations) {
      { operations.write_active(false) } noexcept -> std::same_as<std::expected<void, core::AppError>>;
      { operations.matches_active(false) } noexcept -> std::same_as<bool>;
    }
  [[nodiscard]] std::expected<void, core::AppError>
  deactivate_helper(Operations &operations) noexcept
  {
    const auto hidden = operations.write_active(false);
    if (hidden && operations.matches_active(false)) {
      return {};
    }
    return std::unexpected{core::AppError{
      .code = core::AppErrorCode::RuntimeStateLost,
      .native_detail = hidden ? std::nullopt : hidden.error().native_detail}};
  }

  struct HelperRetirementResult final {
    bool made_progress{};
    bool ownership_uncertain{};
  };

  /// @brief Retire native resources only after hidden-state and grace proofs hold.
  ///
  /// Destroying the window must precede unregistering its class. Progress is a
  /// decreased owned-resource count, allowing partial release to be distinguished
  /// from retries; unsuccessful destruction or class cleanup retains uncertainty.
  template <typename Operations>
    requires requires(Operations &operations) {
      { operations.resource_count() } noexcept -> std::same_as<unsigned>;
      { operations.destroy_window() } noexcept -> std::same_as<bool>;
      { operations.unregister_class() } noexcept -> std::same_as<bool>;
    }
  [[nodiscard]] HelperRetirementResult progress_helper_retirement(
    Operations &operations, const HelperRetirementState &state, Clock::time_point now) noexcept
  {
    if (!helper_may_retire(state, now)) {
      return {};
    }
    const auto before = operations.resource_count();
    if (!operations.destroy_window()) {
      return {.ownership_uncertain = true};
    }
    const bool class_released = operations.unregister_class();
    return {.made_progress = operations.resource_count() < before,
            .ownership_uncertain = !class_released};
  }

}    // namespace avb::platform::windows::features::detail

#endif
