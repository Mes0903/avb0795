/// @file Portable user intentions consumed only by Application on the main thread.
#ifndef AVB0795_APP_COMMANDS_HPP
#define AVB0795_APP_COMMANDS_HPP

#include "avb/core/types.hpp"

#include <variant>

namespace avb::app {

  /// Request a catalog refresh while no runtime is retained.
  struct RefreshSources {};

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

  /// Request an input-mode transaction; accepted mode changes are session-only.
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
  struct RestoreActiveReplicaEditable {};
  /// Stop command admission and begin application shutdown through ordinary drain.
  struct Quit {};

  /// Value-only command boundary: generation tokens prevent stale session mutations.
  /// No variant alternative carries a native handle, callback or mutable domain reference.
  using AppCommand =
    std::variant<RefreshSources, SelectSource, Start, Stop, SetMode, SetBorder, SetFps, SetExtensions, ReplaceHotkey, RestoreActiveReplicaEditable, Quit>;

}    // namespace avb::app

#endif    // AVB0795_APP_COMMANDS_HPP
