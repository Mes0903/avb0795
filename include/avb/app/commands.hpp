/// @file
/// @brief Portable user intentions consumed only by Application on the main thread.
#ifndef AVB0795_APP_COMMANDS_HPP
#define AVB0795_APP_COMMANDS_HPP

#include "avb/core/types.hpp"
#include "avb/i18n/language.hpp"

#include <variant>

namespace avb::app {

  /// Request a catalog refresh while no runtime is retained.
  struct RefreshSources {};
  /// Persist automatic discovery preference; Application owns its transient deadline.
  struct SetAutoRefresh { bool enabled{}; };

  /// Select a descriptor from the current snapshot without starting capture.
  struct SelectSource {
    core::SessionId session_id;
    core::SourceId source_id;
  };

  /// Start the selected source, or queue replacement of an Interrupted runtime.
  struct Start {
    core::SessionId session_id;
  };

  /// Request asynchronous stopping; a repeated stop cancels a pending replacement.
  struct Stop {
    core::SessionId session_id;
  };

  /// Request an input-mode transaction; Application persists the accepted preference.
  struct SetMode {
    core::SessionId session_id;
    core::OverlayMode mode{core::OverlayMode::Editable};
  };

  /// Request a border transaction and persist the accepted preference.
  struct SetBorder {
    core::SessionId session_id;
    core::BorderPreference border{core::BorderPreference::Bordered};
  };

  /// Request a render-cap transaction and persist the accepted preference.
  struct SetFps {
    core::SessionId session_id;
    core::FpsCap fps_cap{core::FpsCap::Fps60};
  };

  /// Preview an opacity percentage, or commit the current accepted preferences.
  /// A commit never reapplies percent, including a rejected final preview.
  struct SetOpacity {
    core::SessionId session_id;
    int percent{100};
    bool commit{};
  };

  /// @brief Apply one complete source-pixel rectangle, or reset to full view.
  /// Application validates both identities, Capturing and current presented bounds
  /// before applying the whole candidate. A successful change also ends selection.
  struct SetRegion {
    core::SessionId session_id;
    core::SourceId source_id;
    std::optional<core::PixelRegion> region;
  };

  /// Begin full-source selection or cancel it while retaining the accepted crop.
  /// Begin requires current presented bounds; cancellation remains reachable before drain.
  struct SetRegionSelection {
    core::SessionId session_id;
    core::SourceId source_id;
    bool active{};
  };

  /// Request an experimental-settings transaction and persist accepted preferences.
  struct SetExtensions {
    core::SessionId session_id;
    core::ExtensionSettings extensions;
  };

  /// Replace the OS binding before committing/persisting this typed desired chord.
  struct ReplaceHotkey {
    core::SessionId session_id;
    core::HotkeyChord hotkey;
  };

  /// Session-agnostic hotkey intent resolved against the current live runtime.
  /// It is harmless when no runtime exists or stopping has begun.
  /// Accepted restoration also persists Editable, including during graphics recovery.
  struct RestoreActiveReplicaEditable {};
  /// Stop command admission and begin application shutdown through ordinary drain.
  struct Quit {};
  /// @brief Change the requested presentation language independently of session generation.
  /// Re-selecting System refreshes the system query even when the preference is
  /// unchanged. The request, not its resolved language, is persisted; save failure
  /// retains the accepted in-memory preference. Quit closes further command processing.
  struct SetLanguage {
    i18n::LanguagePreference preference{i18n::LanguagePreference::System};
  };

  /// @brief Owning intentions whose effects occur only when Application processes them.
  /// Generation-bearing alternatives copy the token from their originating view;
  /// stale tokens are rejected before session mutation. A current token still needs
  /// lifecycle and capability admission. Global commands resolve against current
  /// state without a token. No alternative carries a callback or mutable domain reference.
  using AppCommand =
    std::variant<RefreshSources, SelectSource, Start, Stop, SetMode, SetBorder, SetFps, SetExtensions, ReplaceHotkey, RestoreActiveReplicaEditable, Quit, SetLanguage, SetAutoRefresh, SetOpacity, SetRegion, SetRegionSelection>;

}    // namespace avb::app

#endif    // AVB0795_APP_COMMANDS_HPP
