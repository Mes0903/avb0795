#ifndef AIRELINNA_APP_COMMANDS_HPP
#define AIRELINNA_APP_COMMANDS_HPP

#include "airelinna/core/types.hpp"

#include <variant>

namespace airelinna::app {

  struct RefreshSources {};

  struct SelectSource {
    core::SessionId session_id;
    core::SourceId source_id;
  };

  struct Start {
    core::SessionId session_id;
  };

  struct Stop {
    core::SessionId session_id;
  };

  struct SetMode {
    core::SessionId session_id;
    core::OverlayMode mode{core::OverlayMode::Editable};
  };

  struct SetBorder {
    core::SessionId session_id;
    core::BorderPreference border{core::BorderPreference::Bordered};
  };

  struct SetFps {
    core::SessionId session_id;
    core::FpsCap fps_cap{core::FpsCap::Fps60};
  };

  struct ReplaceHotkey {
    core::SessionId session_id;
    core::HotkeyChord hotkey;
  };

  struct RestoreActiveReplicaEditable {};
  struct Quit {};

  using AppCommand =
    std::variant<RefreshSources, SelectSource, Start, Stop, SetMode, SetBorder, SetFps, ReplaceHotkey, RestoreActiveReplicaEditable, Quit>;

}    // namespace airelinna::app

#endif    // AIRELINNA_APP_COMMANDS_HPP
