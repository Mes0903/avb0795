/// @file
/// @brief Ordered rollback for unpublished graphics construction.
#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_GRAPHICS_CONSTRUCTION_GUARD_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_GRAPHICS_CONSTRUCTION_GUARD_HPP

#include <exception>

namespace avb::platform::windows::detail {

  /// @brief Roll back partial bootstrap or ring resources in dependency order.
  ///
  /// Operations and its borrowed owners must outlive the guard. All three
  /// operations must be noexcept and accept partial construction. Detachment
  /// must prove renderer references are gone before bridge resources are freed;
  /// failure preserves the existing fail-closed termination invariant.
  template <class Operations>
  class GraphicsConstructionGuard final {
  public:
    explicit GraphicsConstructionGuard(Operations &operations) noexcept
        : operations_{operations} {}
    ~GraphicsConstructionGuard() noexcept
    {
      if (!armed_) {
        return;
      }
      if (!operations_.detach_renderer_references()) {
        std::terminate();
      }
      operations_.release_bridge_resources();
      operations_.release_renderer_resources();
    }
    GraphicsConstructionGuard(const GraphicsConstructionGuard &) = delete;
    GraphicsConstructionGuard &operator=(const GraphicsConstructionGuard &) = delete;
    GraphicsConstructionGuard(GraphicsConstructionGuard &&) = delete;
    GraphicsConstructionGuard &operator=(GraphicsConstructionGuard &&) = delete;

    /// Transfer cleanup only after the construction transaction succeeds.
    void disarm() noexcept { armed_ = false; }

  private:
    Operations &operations_;
    bool armed_{true};
  };

}    // namespace avb::platform::windows::detail

#endif
