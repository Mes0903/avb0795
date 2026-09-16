/// @file
/// @brief Replica draw requests and retired presentation geometry shared by graphics owners.
#ifndef AVB0795_RENDER_REPLICA_PRESENTATION_HPP
#define AVB0795_RENDER_REPLICA_PRESENTATION_HPP

#include "avb/core/types.hpp"
#include "render/render_math.hpp"

#include <cstdint>
#include <optional>

namespace avb::render {

  /// @brief Latest requested replica draw, independent of capture and native styles.
  /// Selection displays full content; its optional preview is expressed in source
  /// pixels. The runtime grants redraw admission through its existing FPS budget.
  struct ReplicaPresentation {
    std::optional<core::PixelRegion> region;
    bool selecting_region{};
    std::optional<core::PixelRegion> selection_preview;
    friend bool operator==(const ReplicaPresentation &, const ReplicaPresentation &) = default;
  };

  /// @brief Geometry copied from a draw whose successful presentation has retired.
  /// The runtime maps native client input through this value so selection follows
  /// the displayed image. A later retirement with an older sequence keeps the
  /// newer snapshot; geometry invalidation discards preceding in-flight mappings.
  struct PresentationSnapshot {
    core::Extent content;
    core::Extent framebuffer;
    Viewport viewport;
    core::PixelRegion region;
    bool selection_full_view{};
    std::uint64_t present_sequence{};
  };

}    // namespace avb::render

#endif    // AVB0795_RENDER_REPLICA_PRESENTATION_HPP
