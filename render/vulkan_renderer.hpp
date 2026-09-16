/// @file
/// @brief Concrete Vulkan image, submission and presentation owner.
#ifndef AVB0795_RENDER_VULKAN_RENDERER_HPP
#define AVB0795_RENDER_VULKAN_RENDERER_HPP

#include "avb/core/types.hpp"
#include "render/replica_presentation.hpp"
#include "render/vulkan_input.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

struct GLFWwindow;
namespace avb::diagnostics {
  class PerformanceReport;
}

namespace avb::render {

  /// @brief Accumulated facts for one ordered, main-thread renderer progress pass.
  /// Present retirement precedes producer delivery and submission retirement.
  /// finish_tick adds WSI recovery, at most one draw and the final ownership
  /// snapshot. Counts belong to renderer decisions; the facade adds producer
  /// admission/drop counts and independent producer failure facts.
  struct VulkanTickResult final {
    bool surface_recovery_started{};
    bool surface_recovery_completed{};
    bool vulkan_device_lost{};
    std::optional<core::AppError> terminal_error;
    bool made_progress{};
    bool drain_complete{};
    bool cleanup_pending{};
    bool presentation_suspended{};
    std::uint64_t presented_frames{};
    std::uint64_t dropped_frames{};
    std::size_t slots_in_flight{};
    std::size_t presentations_in_flight{};
    std::optional<std::chrono::steady_clock::time_point> next_deadline;
    std::optional<PresentationSnapshot> presentation;
  };

  /// @brief Synchronous diagnostic context supplied by the producer facade.
  /// Views live through observe_metadata(); renderer-owned device/WSI facts are
  /// merged into the existing report within that call. These values select no
  /// transport and carry no native ownership.
  struct VulkanObservationContext final {
    std::string_view os;
    std::string_view bridge;
    std::string_view bridge_reason;
    std::string_view requested_bridge;
    core::Extent content{};
    core::FpsCap fps{core::FpsCap::Fps60};
  };

  /// @brief Own one bounded sampled-image ring and its independent Vulkan WSI.
  /// All operations execute on the main thread. The borrowed GLFW window outlives
  /// this object. External image/timeline owners outlive their registrations and
  /// all native uses; detach is the explicit handoff that permits their release.
  /// Producers publish completion through typed values and keep native copy
  /// ownership outside this object. Canonical slot phase and retained redraw
  /// content belong exclusively to this renderer.
  class VulkanRenderer final {
  public:
    static constexpr std::size_t source_slot_capacity = 3;

    VulkanRenderer(GLFWwindow *window, std::uint64_t resource_generation, core::Extent framebuffer, const VulkanStartupRequest &startup, diagnostics::PerformanceReport *performance = nullptr, std::uint64_t observation_instance = 0);
    ~VulkanRenderer() noexcept;
    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;
    VulkanRenderer(VulkanRenderer &&) = delete;
    VulkanRenderer &operator=(VulkanRenderer &&) = delete;

    [[nodiscard]] VulkanDeviceBorrow device_borrow() const noexcept;

    /// @brief Begin an unpublished installation and issue its fresh slot identity.
    /// Requires an already released ring and representable positive allocation.
    /// Every attempted installation consumes a serial. The facade pairs this
    /// identity with its producer ring, prepares each slot, then publishes once.
    [[nodiscard]] std::expected<std::uint64_t, core::AppError>
    begin_ring(core::Extent allocation) noexcept;
    [[nodiscard]] std::expected<void, core::AppError>
    create_cpu_slot(SlotKey key);
    [[nodiscard]] std::expected<void, core::AppError>
    register_external_image(SlotKey key, ExternalSampledImageView image) noexcept;
    [[nodiscard]] std::expected<void, core::AppError>
    publish_ring(std::uint64_t expected_serial, std::uint64_t generation) noexcept;

    [[nodiscard]] bool ready_for_frame() const noexcept;
    [[nodiscard]] std::optional<SlotKey> reserve_source_slot() noexcept;
    /// @brief Cancel pre-adoption validation failure, restoring eligible redraw.
    /// The producer must have issued no native work for this reservation. A
    /// completed adopted source instead uses discard_completed_source().
    [[nodiscard]] std::expected<void, core::AppError>
    cancel_source_reservation(SlotKey key) noexcept;
    /// @brief Copy and flush mapped rows synchronously before committing CpuReady.
    /// Source bytes stay readable throughout the call and remain with the caller.
    [[nodiscard]] std::expected<void, core::AppError>
    complete_cpu_input(SlotKey key, CpuRowsView rows, FrameStamp stamp) noexcept;
    /// @brief Publish sampling readiness after host-observed producer completion.
    /// The supplied timeline wait is already satisfiable and remains alive until
    /// every submission using this registration has retired.
    [[nodiscard]] std::expected<void, core::AppError>
    complete_external_input(SlotKey key, core::Extent content, FrameStamp stamp, CompletedProducerWait wait) noexcept;
    [[nodiscard]] std::expected<void, core::AppError>
    discard_completed_source(SlotKey key) noexcept;

    /// @brief Withdraw image borrows after identity and image-user proof checks.
    /// A stale serial or live source/submission preserves the installation.
    /// Descriptor reset failure likewise preserves references for recovery.
    [[nodiscard]] std::expected<void, core::AppError>
    detach_ring(std::uint64_t expected_serial) noexcept;
    /// @brief Release allocation-local owners after successful detach and producer release.
    /// Devices, command buffers and independent present records survive this step.
    void release_ring_resources() noexcept;
    /// @brief Prepare final teardown after all healthy native users have retired.
    /// Destroys descriptor registrations directly, then clears image borrows.
    /// The caller releases external owners next and finally destroys this object.
    /// Outstanding native users terminate, preserving the established drain rule.
    void detach_for_shutdown() noexcept;
    [[nodiscard]] bool render_allocation_idle() const noexcept;
    /// @brief Discard completed queued content before ring replacement or fallback.
    /// Producer reservations and native submissions retain their existing proof.
    void discard_ready_inputs() noexcept;
    void set_source_admission(bool enabled) noexcept;

    void set_presentation(ReplicaPresentation presentation) noexcept;
    [[nodiscard]] bool has_redraw_work() const noexcept;
    void request_redraw() noexcept;
    void resize(core::Extent framebuffer) noexcept;
    void begin_drain() noexcept;
    /// @brief Apply confirmed Vulkan loss while preserving producer reservations.
    /// The facade continues producer progress and consumes its keyed completions.
    void begin_recovery_drain(bool device_lost) noexcept;
    /// Latch confirmed device loss immediately; the runtime requests drain next.
    void note_device_loss() noexcept;
    [[nodiscard]] bool vulkan_device_lost() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::uint64_t gpu_interop_frames_submitted() const noexcept;

    /// @brief Start a pass by observing present retirement and framebuffer changes.
    [[nodiscard]] VulkanTickResult begin_tick(core::Extent framebuffer,
                                              std::chrono::steady_clock::time_point now) noexcept;
    /// @brief Observe submission proof after the facade delivers producer completions.
    void retire_submissions(VulkanTickResult &result,
                            std::chrono::steady_clock::time_point now) noexcept;
    /// @brief Recover WSI and optionally draw once after facade fallback handling.
    /// allow_present expresses aggregate admission; retirement and recovery still
    /// progress when the facade suppresses presentation.
    void finish_tick(VulkanTickResult &result,
                     std::chrono::steady_clock::time_point now,
                     bool allow_present,
                     bool allow_recovery = true) noexcept;

    /// @brief Include externally owned timeline events in one replica resource ledger.
    /// The facade forwards each create/destroy exactly once while this owner lives.
    void observe_external_semaphore(bool created) noexcept;
    void observe_metadata(VulkanObservationContext context,
                          std::chrono::steady_clock::time_point now) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
  };
}    // namespace avb::render

#endif
