#include "platform/windows/detail/bridge_policy.hpp"
#include "platform/windows/detail/graphics_construction_guard.hpp"
#include "platform/windows/detail/windows_graphics_policy.hpp"
#include "render/cpu_row_copy.hpp"
#include "render/graphics_retry_policy.hpp"
#include "render/render_math.hpp"
#include "render/vulkan_input.hpp"
#include "avb/diagnostics/performance_report.hpp"

#include "platform/windows/detail/wgc_capture_state.hpp"
#include "platform/windows/detail/windows_runtime_policy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

  namespace capture_detail = avb::platform::windows::detail;
  namespace render = avb::render;
  using avb::core::BridgeMode;
  using avb::core::BridgeReason;
  using avb::core::Extent;
  using namespace std::chrono_literals;

  /// Own only files created inside a unique directory for report contract tests.
  class PerformanceTestDirectory final {
  public:
    PerformanceTestDirectory()
    {
      std::random_device random;
      const auto parent = std::filesystem::temp_directory_path();
      for (int attempt{}; attempt < 64; ++attempt) {
        const auto candidate = parent / ("avb-performance-test-" +
          std::to_string(random()) + "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
          path_ = candidate;
          return;
        }
      }
      throw std::runtime_error{"cannot create report test directory"};
    }
    ~PerformanceTestDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
    PerformanceTestDirectory(const PerformanceTestDirectory &) = delete;
    PerformanceTestDirectory &operator=(const PerformanceTestDirectory &) = delete;
    [[nodiscard]] std::filesystem::path file(std::string_view name) const
    { return path_ / name; }
  private:
    std::filesystem::path path_;
  };

  [[nodiscard]] nlohmann::json read_performance_report(const std::filesystem::path &path)
  {
    std::ifstream input{path};
    return nlohmann::json::parse(input);
  }

  TEST_CASE("avb0795 V1 case 06: aspect fit and present mode selection",
            "[avb0795-v1][render][policy]")
  {
    SECTION("CPU tight rows copy every pixel with zero clearing")
    {
      std::array<std::byte, 18> source{};
      source.fill(std::byte{0xE1});
      for (std::size_t index{}; index < 16; ++index) {
        source[index + 1] = static_cast<std::byte>(index + 1);
      }
      const auto original = source;
      std::array<std::byte, 18> guarded{};
      guarded.fill(std::byte{0xA5});
      const auto pixels = std::span{source}.subspan(1, 16);
      const auto destination = std::span{guarded}.subspan(1, 16);
      const auto copied = render::copy_bgra_rows({pixels, 8, {2, 2}, {2, 2}}, destination);
      REQUIRE(copied);
      CHECK(copied->copied_bytes == 16);
      CHECK(copied->cleared_bytes == 0);
      CHECK(std::ranges::equal(destination, pixels));
      CHECK(source == original);
      CHECK(guarded.front() == std::byte{0xA5});
      CHECK(guarded.back() == std::byte{0xA5});
    }

    SECTION("CPU padding uses source pitch and clears pixels exposed by shrink")
    {
      std::array<std::byte, 24> source{};
      source.fill(std::byte{0xE1});
      std::fill_n(source.begin(), 8, std::byte{0x31});
      std::fill_n(source.begin() + 12, 8, std::byte{0x72});
      const auto original = source;
      std::array<std::byte, 26> guarded{};
      guarded.fill(std::byte{0xA5});
      const auto destination = std::span{guarded}.subspan(1, 24);
      // The final row ends at its last pixel: source trailing pitch padding is optional.
      const auto padded = render::copy_bgra_rows(
        {std::span{source}.first(20), 12, {2, 2}, {3, 2}}, destination);
      REQUIRE(padded);
      CHECK(padded->copied_bytes == 16);
      CHECK(padded->cleared_bytes == 8);
      for (std::size_t index{}; index < destination.size(); ++index) {
        const auto expected = index % 12 < 8
          ? (index < 12 ? std::byte{0x31} : std::byte{0x72}) : std::byte{};
        CHECK(destination[index] == expected);
      }
      REQUIRE(render::copy_bgra_rows({source, 12, {3, 2}, {3, 2}}, destination));
      const auto shrunk = render::copy_bgra_rows(
        {std::span{source}.first(8), (std::numeric_limits<std::size_t>::max)(),
         {2, 1}, {3, 2}}, destination);
      REQUIRE(shrunk);
      CHECK(shrunk->copied_bytes == 8);
      CHECK(shrunk->cleared_bytes == 16);
      for (std::size_t index{}; index < destination.size(); ++index) {
        CHECK(destination[index] == (index < 8 ? std::byte{0x31} : std::byte{}));
      }
      CHECK(source == original);
      CHECK(guarded.front() == std::byte{0xA5});
      CHECK(guarded.back() == std::byte{0xA5});
    }

    SECTION("CPU invalid layout returns before modifying destination bytes")
    {
      using Error = render::RowCopyError;
      std::array<std::byte, 24> source{};
      std::array<std::byte, 27> guarded{};
      guarded.fill(std::byte{0xA5});
      const auto original = guarded;
      constexpr auto size_max = (std::numeric_limits<std::size_t>::max)();
      constexpr auto dimension_max = (std::numeric_limits<std::uint32_t>::max)();
      struct InvalidRow final {
        render::CpuRowsView input;
        std::size_t destination_bytes;
        Error error;
      };
      const std::array invalid{
        InvalidRow{{source, 12, {0, 2}, {3, 2}}, 24, Error::InvalidExtent},
        InvalidRow{{source, 12, {2, 0}, {3, 2}}, 24, Error::InvalidExtent},
        InvalidRow{{source, 12, {4, 2}, {3, 2}}, 24, Error::InvalidExtent},
        InvalidRow{{source, 12, {2, 3}, {3, 2}}, 24, Error::InvalidExtent},
        InvalidRow{{source, 7, {2, 2}, {3, 2}}, 24, Error::InvalidPitch},
        InvalidRow{{{}, 12, {2, 2}, {3, 2}}, 24, Error::InvalidSpan},
        InvalidRow{{std::span{source}.first(19), 12, {2, 2}, {3, 2}}, 24, Error::InvalidSpan},
        InvalidRow{{source, 12, {2, 2}, {3, 2}}, 23, Error::InvalidSpan},
        InvalidRow{{source, 12, {2, 2}, {3, 2}}, 25, Error::InvalidSpan},
        InvalidRow{{source, size_max, {1, 2}, {1, 2}}, 8, Error::Overflow},
        InvalidRow{{source, size_max, {1, 3}, {1, 3}}, 12, Error::Overflow},
        InvalidRow{{source, 4, {1, 1}, {dimension_max, dimension_max}}, 4, Error::Overflow},
      };
      for (const auto &row : invalid) {
        CAPTURE(row.input.content.width, row.input.content.height,
                row.input.row_pitch, row.destination_bytes);
        const auto copied = render::copy_bgra_rows(
          row.input, std::span{guarded}.subspan(1, row.destination_bytes));
        REQUIRE_FALSE(copied);
        CHECK(copied.error() == row.error);
        CHECK(guarded == original);
      }
    }

    SECTION("one admission shares dirty redraw with an adopted capture")
    {
      using Phase = render::RenderSlotPhase;
      std::array candidates{render::RenderFrameCandidate{Phase::Empty, 1, true},
                            render::RenderFrameCandidate{Phase::ProducerPending, 2, false}};
      const auto replay = capture_detail::admit_retained_redraw(true, true);
      CHECK_FALSE(replay);
      CHECK_FALSE(render::select_render_frame(candidates, 1, replay));
      candidates[1].phase = Phase::CpuReady;
      CHECK(render::select_render_frame(candidates, 1, replay) == 1);
      CHECK(capture_detail::admit_retained_redraw(true, false));
      CHECK_FALSE(capture_detail::admit_retained_redraw(false, true));
    }

    SECTION("crop geometry covers dragged pixels and isolates texture boundaries")
    {
      using avb::core::PixelRegion;
      using avb::core::valid_region;
      using avb::core::intersect_region;
      constexpr Extent source{200, 100};
      CHECK(valid_region({0, 0, 200, 100}, source));
      CHECK_FALSE(valid_region({199, 99, 2, 1}, source));
      CHECK_FALSE(valid_region({0, 0, 0, 1}, source));
      CHECK_FALSE(valid_region({1, 1, UINT32_MAX, UINT32_MAX}, source));
      CHECK(intersect_region({190, 90, 50, 50}, source) == PixelRegion{190, 90, 10, 10});
      CHECK_FALSE(intersect_region({200, 0, 1, 1}, source));
      constexpr render::Viewport viewport{0, 100, 400, 200};
      CHECK(render::selection_region(source, viewport, {100, 150}, {300, 250}) ==
            PixelRegion{50, 25, 100, 50});
      CHECK(render::selection_region(source, viewport, {300, 250}, {100, 150}) ==
            PixelRegion{50, 25, 100, 50});
      CHECK(render::selection_region(source, viewport, {100, 150}, {600, 500}) ==
            PixelRegion{50, 25, 150, 75});
      CHECK_FALSE(render::selection_region(source, viewport, {10, 50}, {300, 250}));
      CHECK_FALSE(render::selection_region(source, viewport, {100, 150}, {100, 150}));
      const auto sampling = render::region_sampling(PixelRegion{50, 25, 100, 50},
                                                     source, {256, 128}, {400, 400});
      REQUIRE(sampling);
      CHECK(sampling->viewport == viewport);
      CHECK(sampling->texture_offset == std::array{50.0F / 256, 25.0F / 128});
      CHECK(sampling->texture_scale == std::array{100.0F / 256, 50.0F / 128});
      CHECK(sampling->texture_min == std::array{50.5F / 256, 25.5F / 128});
      CHECK(sampling->texture_max == std::array{149.5F / 256, 74.5F / 128});
      const auto pixel = render::region_sampling(PixelRegion{199, 99, 1, 1},
                                                 source, {256, 128}, {400, 400});
      REQUIRE(pixel);
      CHECK(pixel->texture_min == pixel->texture_max);
      CHECK_FALSE(render::region_sampling({}, source, {128, 128}, {400, 400}));
    }

    struct AspectRow final {
      Extent source;
      Extent destination;
      std::optional<render::Viewport> expected;
    };

    constexpr auto maximum = (std::numeric_limits<std::uint32_t>::max)();
    constexpr std::array aspect_rows{
      AspectRow{
        {1920, 1080},
        {1000, 1000},
        render::Viewport{.x = 0, .y = 219, .width = 1000, .height = 562}},
      AspectRow{
        {1080, 1920},
        {1000, 500},
        render::Viewport{.x = 359, .y = 0, .width = 281, .height = 500}},
      AspectRow{{640, 480},
                {640, 480},
                render::Viewport{.x = 0, .y = 0, .width = 640, .height = 480}},
      AspectRow{{2, 1},
                {7, 4},
                render::Viewport{.x = 0, .y = 0, .width = 7, .height = 3}},
      AspectRow{{1, 2},
                {4, 7},
                render::Viewport{.x = 0, .y = 0, .width = 3, .height = 7}},
      AspectRow{
        {maximum, maximum - 1},
        {maximum - 1, maximum},
        render::Viewport{
          .x = 0, .y = 1, .width = maximum - 1, .height = maximum - 2}},
      AspectRow{{1, maximum},
                {1, 1},
                render::Viewport{.x = 0, .y = 0, .width = 1, .height = 1}},
      AspectRow{{0, 1}, {1, 1}, std::nullopt},
      AspectRow{{1, 0}, {1, 1}, std::nullopt},
      AspectRow{{1, 1}, {0, 1}, std::nullopt},
      AspectRow{{1, 1}, {1, 0}, std::nullopt},
    };

    for (const auto &row : aspect_rows) {
      CAPTURE(row.source.width, row.source.height, row.destination.width,
              row.destination.height);
      CHECK(render::aspect_fit(row.source, row.destination) == row.expected);
    }

    struct PresentModeRow final {
      std::vector<VkPresentModeKHR> advertised;
      VkPresentModeKHR expected;
    };
    const std::array present_mode_rows{
      PresentModeRow{{VK_PRESENT_MODE_FIFO_KHR}, VK_PRESENT_MODE_FIFO_KHR},
      PresentModeRow{{VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR},
                     VK_PRESENT_MODE_MAILBOX_KHR},
      PresentModeRow{
        {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR},
        VK_PRESENT_MODE_MAILBOX_KHR},
      PresentModeRow{{VK_PRESENT_MODE_IMMEDIATE_KHR}, VK_PRESENT_MODE_FIFO_KHR},
      PresentModeRow{{}, VK_PRESENT_MODE_FIFO_KHR},
    };
    for (const auto &row : present_mode_rows) {
      CAPTURE(row.advertised.size());
      CHECK(render::select_present_mode(row.advertised) == row.expected);
      CHECK(render::select_present_mode(row.advertised) !=
            VK_PRESENT_MODE_IMMEDIATE_KHR);
    }

    SECTION("SDR surface format preserves captured bytes")
    {
      constexpr VkSurfaceFormatKHR unorm{
        VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
      constexpr VkSurfaceFormatKHR srgb{
        VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
      const std::array supported{
        std::vector{unorm}, std::vector{srgb, unorm},
        std::vector{unorm, srgb}};
      for (const auto &advertised : supported) {
        CAPTURE(advertised.size(), advertised.front().format);
        const auto selected = render::select_sdr_surface_format(advertised);
        REQUIRE(selected.has_value());
        CHECK(selected->format == VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(selected->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
      }

      constexpr std::array unsupported{
        srgb,
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                           VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM,
                           VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED,
                           VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
      for (const auto format : unsupported) {
        CAPTURE(format.format, format.colorSpace);
        const std::array advertised{format};
        CHECK_FALSE(render::select_sdr_surface_format(advertised));
      }
      CHECK_FALSE(render::select_sdr_surface_format({}));
    }
  }

  TEST_CASE("avb0795 V1 case 08: render ownership and recovery policies",
            "[avb0795-v1][render][policy]")
  {
    SECTION("bounded present uses retain independent proof and reject stale reuse")
    {
      using Phase = render::PresentUsePhase;
      std::array<render::PresentUseState, 3> uses{};
      std::array<render::PresentUseKey, 3> keys{};
      std::uint64_t last_serial{};
      for (std::size_t index{}; index < uses.size(); ++index) {
        const auto key = render::reserve_present_use(uses[index], index, last_serial);
        REQUIRE(key);
        keys[index] = *key;
      }
      REQUIRE(render::enqueue_present_use(uses[0], keys[0], 0));
      CHECK(std::ranges::count_if(uses, [](const auto &use) {
        return use.phase != Phase::Free;
      }) == 3);
      for (std::size_t index{}; index < uses.size(); ++index) {
        CHECK_FALSE(render::reserve_present_use(uses[index], index, last_serial));
      }
      CHECK(last_serial == 3);

      // Submission proof recycles its slot, but cannot release a transferred
      // present use. Only the independent present fence permits this transition.
      CHECK(render::proof_fence_for_slot(render::RenderSlotPhase::Submitted) ==
            render::SlotProofFence::Submission);
      CHECK_FALSE(render::release_present_use(uses[0], keys[0], 0, Phase::Reserved));
      CHECK(uses[0].phase == Phase::PresentPending);
      REQUIRE(render::release_present_use(uses[0], keys[0], 0, Phase::PresentPending));
      CHECK_FALSE(render::release_present_use(uses[0], keys[0], 0, Phase::PresentPending));
      const auto reused = render::reserve_present_use(uses[0], 0, last_serial);
      REQUIRE(reused);
      CHECK(reused->serial == 4);
      CHECK_FALSE(render::enqueue_present_use(uses[0], keys[0], 0));
      CHECK_FALSE(render::release_present_use(uses[0], keys[0], 0, Phase::Reserved));
      CHECK_FALSE(render::matches_present_use(uses[0], *reused, 1, Phase::Reserved));
      CHECK_FALSE(render::matches_present_use(uses[0], {0, 0}, 0, Phase::Reserved));
      CHECK_FALSE(render::release_present_use(uses[0], *reused, 0, Phase::Free));
      CHECK(uses[0].phase == Phase::Reserved);

      // A timeout-zero acquire owns no image or signal, so its reservation can
      // be cancelled. Ring/WSI replacement must not rewind the owner serial.
      REQUIRE(render::classify_acquire_result(VK_TIMEOUT, false).ownership ==
              render::AcquireOwnership::Unchanged);
      REQUIRE(render::release_present_use(uses[0], *reused, 0, Phase::Reserved));
      const auto after_replacement = render::reserve_present_use(uses[0], 0, last_serial);
      REQUIRE(after_replacement);
      CHECK(after_replacement->serial > reused->serial);
      CHECK_FALSE(render::enqueue_present_use(uses[0], *reused, 0));

      // Success, WSI rejection and uncertainty all retain the same bounded
      // present record until a present proof (or device-loss cleanup) arrives.
      for (const auto native : {VK_SUCCESS, VK_ERROR_OUT_OF_DATE_KHR, VK_TIMEOUT}) {
        CAPTURE(native);
        render::PresentUseState use;
        const auto key = render::reserve_present_use(use, 2, last_serial);
        REQUIRE(key);
        const auto present = render::classify_present_result(native);
        REQUIRE(present.presentation_pending);
        REQUIRE(render::enqueue_present_use(use, *key, 2));
        CHECK_FALSE(render::enqueue_present_use(use, *key, 2));
        CHECK_FALSE(render::reserve_present_use(use, 2, last_serial));
        REQUIRE(render::release_present_use(use, *key, 2, Phase::PresentPending));
        CHECK(use.phase == Phase::Free);
      }

      const auto submit_oom = render::slot_phase_after_submit(
        render::classify_submit_result(VK_ERROR_OUT_OF_DEVICE_MEMORY));
      const auto submit_uncertain = render::slot_phase_after_submit(
        render::classify_submit_result(VK_TIMEOUT));
      REQUIRE(submit_oom);
      REQUIRE(submit_uncertain);
      const auto present_oom = render::classify_present_result(VK_ERROR_OUT_OF_HOST_MEMORY);
      REQUIRE(present_oom.ownership == render::PresentOwnership::NotEnqueued);
      for (const auto phase : {render::RenderSlotPhase::AcquireProofPending,
                               *submit_oom, *submit_uncertain,
                               render::slot_phase_after_present(present_oom.ownership)}) {
        for (const auto native : {VK_SUCCESS, VK_ERROR_SURFACE_LOST_KHR,
                                  VK_ERROR_VALIDATION_FAILED_EXT}) {
          CAPTURE(phase, native);
          render::PresentUseState use;
          const auto key = render::reserve_present_use(use, 1, last_serial);
          REQUIRE(key);
          CHECK(render::proof_fence_for_slot(phase) != render::SlotProofFence::None);
          CHECK(use.phase == Phase::Reserved);
          const auto release = render::plan_slot_release(
            phase, render::classify_release_result(native));
          CHECK(release.destroy_acquire_semaphore ==
                (phase == render::RenderSlotPhase::AcquireProofPending));
          CHECK(release.destroy_render_finished_semaphore ==
                (phase != render::RenderSlotPhase::AcquireProofPending));
          if (!release.clear_acquired_ownership) {
            REQUIRE(release.next_phase == render::RenderSlotPhase::ReleaseRetained);
            CHECK_FALSE(render::reserve_present_use(use, 1, last_serial));
            // Reserved capacity remains occupied, but owes no present fence:
            // safe WSI destruction can discharge the retained acquired image.
            CHECK(render::retired_swapchain_destroyable({
              .present_fence_pending = use.phase == Phase::PresentPending,
              .release_retained = true}));
          }
          REQUIRE(render::release_present_use(use, *key, 1, Phase::Reserved));
          CHECK(use.phase == Phase::Free);
        }
      }
      render::PresentUseState exhausted;
      auto maximum = (std::numeric_limits<std::uint64_t>::max)();
      CHECK_FALSE(render::reserve_present_use(exhausted, 0, maximum));
      CHECK(exhausted.phase == Phase::Free);
      CHECK(exhausted.serial == 0);
      CHECK(maximum == (std::numeric_limits<std::uint64_t>::max)());

      // The controller uses its backend image count, rather than the replica's
      // fixed three slots. Acquire reservations and release-retained uses occupy
      // the same capacity as independently pending presentation.
      for (const std::size_t capacity : {2U, 3U, 4U}) {
        CAPTURE(capacity);
        std::vector<render::PresentUseState> controller_uses(capacity);
        std::vector<render::PresentUseKey> controller_keys;
        std::uint64_t controller_serial{};
        for (std::size_t index{}; index < capacity; ++index) {
          const auto key = render::reserve_present_use(
            controller_uses[index], index, controller_serial);
          REQUIRE(key);
          controller_keys.push_back(*key);
          if (index != 0) {
            REQUIRE(render::enqueue_present_use(controller_uses[index], *key, index));
          }
        }
        CHECK(std::ranges::count_if(controller_uses, [](const auto &use) {
          return use.phase != Phase::Free;
        }) == capacity);
        for (std::size_t index{}; index < capacity; ++index) {
          CHECK_FALSE(render::reserve_present_use(
            controller_uses[index], index, controller_serial));
        }
        // A frame/submission proof does not release a presentation lease.
        CHECK_FALSE(render::release_present_use(
          controller_uses[1], controller_keys[1], 1, Phase::Reserved));
        REQUIRE(render::release_present_use(
          controller_uses[1], controller_keys[1], 1, Phase::PresentPending));
        const auto replacement = render::reserve_present_use(
          controller_uses[1], 1, controller_serial);
        REQUIRE(replacement);
        CHECK(replacement->serial > controller_keys.back().serial);
        CHECK_FALSE(render::release_present_use(
          controller_uses[1], controller_keys[1], 1, Phase::Reserved));
        CHECK(controller_uses[0].phase == Phase::Reserved);
        CHECK(controller_uses[1].phase == Phase::Reserved);
      }

      // Capacity migration rebuilds storage after every old use is proven free,
      // while the device owner's serial survives both shrinking and expanding.
      std::uint64_t migration_serial{};
      std::vector<render::PresentUseKey> prior_keys;
      for (const std::size_t capacity : {3U, 4U, 2U, 4U, 3U}) {
        CAPTURE(capacity);
        std::vector<render::PresentUseState> rebuilt(capacity);
        std::vector<render::PresentUseKey> current_keys;
        for (std::size_t index{}; index < capacity; ++index) {
          const auto key = render::reserve_present_use(
            rebuilt[index], index, migration_serial);
          REQUIRE(key);
          current_keys.push_back(*key);
          for (const auto prior : prior_keys) {
            CHECK_FALSE(render::matches_present_use(
              rebuilt[index], prior, index, Phase::Reserved));
            CHECK_FALSE(render::release_present_use(
              rebuilt[index], prior, index, Phase::Reserved));
          }
          REQUIRE(render::enqueue_present_use(rebuilt[index], *key, index));
        }
        for (std::size_t index{}; index < capacity; ++index) {
          REQUIRE(render::release_present_use(
            rebuilt[index], current_keys[index], index, Phase::PresentPending));
        }
        CHECK(std::ranges::all_of(rebuilt, [](const auto &use) {
          return use.phase == Phase::Free;
        }));
        prior_keys.insert(prior_keys.end(), current_keys.begin(), current_keys.end());
        CHECK(migration_serial == prior_keys.size());
      }
    }

    SECTION("partial bootstrap and ring construction roll back before device release")
    {
      struct ConstructionFailure final {};
      struct Operations final {
        bool ring_scope{}, device_alive{}, references{}, safe{true};
        int imports{}, count{};
        std::array<int, 3> order{};
        bool detach_renderer_references() noexcept
        { order[count++] = 1; references = false; return true; }
        void release_bridge_resources() noexcept
        {
          order[count++] = 2;
          safe = !references && (imports == 0 || device_alive);
          imports = 0;
        }
        void release_renderer_resources() noexcept
        { order[count++] = 3; device_alive = ring_scope; }
      };
      using Guard = capture_detail::GraphicsConstructionGuard<Operations>;
      STATIC_CHECK_FALSE(std::copy_constructible<Guard>);
      STATIC_CHECK_FALSE(std::move_constructible<Guard>);
      for (const bool ring_scope : {false, true}) {
        for (int prefix{}; prefix <= 7; ++prefix) {
          for (const bool transfer : {false, true}) {
            CAPTURE(ring_scope, prefix, transfer);
            Operations operations{.ring_scope = ring_scope};
            try {
              Guard guard{operations};
              operations.device_alive = ring_scope || prefix > 0;
              operations.imports = prefix / 2;
              operations.references = prefix >= 3;
              if (!transfer) {
                throw ConstructionFailure{};
              }
              guard.disarm();
            } catch (const ConstructionFailure &) {
            }
            CHECK(operations.safe);
            CHECK(operations.count == (transfer ? 0 : 3));
            CHECK(operations.order ==
                  (transfer ? std::array<int, 3>{} : std::array{1, 2, 3}));
            CHECK(operations.device_alive ==
                  (ring_scope || (transfer && prefix > 0)));
            CHECK(operations.imports == (transfer ? prefix / 2 : 0));
            CHECK(operations.references == (transfer && prefix >= 3));
          }
        }
      }
    }

    SECTION("ring installation identity rejects stale zero and out-of-range keys")
    {
      CHECK(render::next_ring_serial(0) == 1);
      CHECK(render::next_ring_serial(41) == 42);
      CHECK_FALSE(render::next_ring_serial((std::numeric_limits<std::uint64_t>::max)()));
      CHECK(render::matches_slot_key({42, 0}, 42, 3));
      CHECK(render::matches_slot_key({42, 2}, 42, 3));
      CHECK_FALSE(render::matches_slot_key({42, 3}, 42, 3));
      CHECK_FALSE(render::matches_slot_key({41, 0}, 42, 3));
      CHECK_FALSE(render::matches_slot_key({0, 0}, 0, 3));
      CHECK_FALSE(render::matches_slot_key({42, 0}, 42, 0));
    }

    SECTION("performance timestamps counters quantiles empty samples and overflow")
    {
      namespace perf = avb::diagnostics;
      using Report = perf::PerformanceReport;
      using Counter = perf::PerformanceCounter;
      using Sample = perf::PerformanceSample;
      constexpr auto replica = perf::PerformanceOwner::Replica;
      constexpr auto controller = perf::PerformanceOwner::Controller;
      const Report::TimePoint epoch{100s};
      PerformanceTestDirectory directory;
      Report report{perf::PerformanceScope::Replica};
      report.observe_runtime({4, 8}, epoch);
      report.observe_metadata(replica, {}, epoch);
      report.observe_first_ui(epoch);
      report.observe_first_present(epoch + 2s);
      report.observe_counter(replica, Counter::Submitted, 99, epoch + 11s);
      report.observe_sample(Sample::IntakeToSubmit, 999ns, epoch + 11s);
      report.observe_sample(Sample::PresentRetirementInterval, 999ns, epoch + 12s);
      report.observe_counter(replica, Counter::Submitted, 1, epoch + 12s);
      report.observe_counter(controller, Counter::Submitted, 3, epoch + 12s);
      for (const auto duration : {4ns, 1ns, 3ns, 2ns}) {
        report.observe_sample(Sample::IntakeToSubmit, duration, epoch + 13s);
      }
      report.observe_sample(Sample::PresentRetirementInterval, 7ns, epoch + 13s);
      report.observe_sample(Sample::PresentRetirementInterval, 8ns, epoch + 14s);
      report.observe_high_water(replica, {.slots = 3, .presents = 2}, epoch + 14s);
      report.observe_high_water(replica, {.slots = 1, .presents = 3}, epoch + 15s);
      report.observe_counter(replica, Counter::Submitted, 1, epoch + 72s - 1ns);
      report.observe_counter(replica, Counter::Submitted, 1000, epoch + 72s);
      report.observe_sample(Sample::IntakeToSubmit, 1000ns, epoch + 72s);
      report.observe_runtime({9, 9}, epoch + 73s);
      report.observe_high_water(replica, {.slots = 99}, epoch + 73s);
      report.finish(epoch + 74s);
      const auto path = directory.file("quantiles.json");
      REQUIRE(report.write_new(path).has_value());
      const auto json = read_performance_report(path);
      CHECK(json.at("complete") == true);
      CHECK(json.at("run_id") == "quantiles");
      CHECK(json.at("scope") == "replica");
      CHECK(json.at("window").at("warmup_start_ns") == 2'000'000'000LL);
      CHECK(json.at("window").at("measurement_start_ns") == 12'000'000'000LL);
      CHECK(json.at("window").at("measurement_end_ns") == 72'000'000'000LL);
      CHECK(json.at("owners").at("replica").at("counters").at("submitted") == 2);
      CHECK(json.at("owners").at("controller").at("counters").at("submitted") == 3);
      CHECK(json.at("owners").at("replica").at("high_water").at("slots") == 3);
      CHECK(json.at("owners").at("replica").at("high_water").at("presents") == 3);
      CHECK(json.at("samples").at("intake_to_submit").at("count") == 4);
      CHECK(json.at("samples").at("intake_to_submit").at("p50_ns") == 2);
      CHECK(json.at("samples").at("intake_to_submit").at("p95_ns") == 4);
      CHECK(json.at("samples").at("present_retirement_interval").at("count") == 2);
      CHECK(json.at("samples").at("present_retirement_interval").at("p50_ns") == 7);
      CHECK(json.at("samples").at("ui_build").at("p50_ns").is_null());
      CHECK(json.at("samples").at("ui_build").at("p95_ns").is_null());
      CHECK(json.at("runtime").at("instance") == 4);
      CHECK(json.at("runtime").at("generation") == 8);

      Report overflow{perf::PerformanceScope::Controller};
      overflow.observe_metadata(controller, {}, epoch);
      overflow.observe_first_ui(epoch);
      for (std::size_t index{}; index < 65'537; ++index) {
        overflow.observe_sample(Sample::UiBuild, 1ns, epoch + 10s);
      }
      overflow.finish(epoch + 70s);
      const auto overflow_path = directory.file("overflow.json");
      REQUIRE(overflow.write_new(overflow_path).has_value());
      const auto overflow_json = read_performance_report(overflow_path);
      CHECK(overflow_json.at("complete") == false);
      CHECK(overflow_json.at("samples").at("ui_build").at("count") == 65'536);
      CHECK(overflow_json.at("samples").at("ui_build").at("overflow") == 1);
      CHECK(overflow_json.at("incomplete_reasons").get<std::vector<std::string>>() ==
            std::vector<std::string>{"sample_overflow"});

      Report invalid{perf::PerformanceScope::Controller};
      invalid.observe_metadata(controller, {}, epoch);
      invalid.observe_first_ui(epoch);
      invalid.observe_counter(controller, Counter::CpuCopyBytes, UINT64_MAX, epoch + 10s);
      invalid.observe_counter(controller, Counter::CpuCopyBytes, 1, epoch + 10s);
      invalid.observe_sample(Sample::UiBuild, -1ns, epoch + 10s);
      invalid.finish(epoch + 70s);
      const auto invalid_path = directory.file("invalid.json");
      REQUIRE(invalid.write_new(invalid_path).has_value());
      const auto invalid_json = read_performance_report(invalid_path);
      CHECK(invalid_json.at("owners").at("controller").at("counters").at("cpu_copy_bytes") == UINT64_MAX);
      CHECK(invalid_json.at("samples").at("ui_build").at("count") == 0);
      CHECK(invalid_json.at("complete") == false);
    }

    SECTION("performance warmup identity metadata shutdown and exclusive report writes")
    {
      namespace perf = avb::diagnostics;
      using Report = perf::PerformanceReport;
      constexpr auto replica = perf::PerformanceOwner::Replica;
      const Report::TimePoint epoch{100s};
      PerformanceTestDirectory directory;
      const perf::PerformanceMetadata initial{
        .gpu = "adapter", .driver = "driver", .os = "system", .bridge = "cpu",
        .bridge_reason = "forced", .present_mode = "fifo", .ui_policy = "on-demand", .requested_bridge = "cpu",
        .content_width = 1920, .content_height = 1080,
        .allocation_width = 2048, .allocation_height = 1080,
        .framebuffer_width = 1280, .framebuffer_height = 720, .fps_cap = 60};
      Report report{perf::PerformanceScope::Replica};
      report.observe_runtime({1, 1}, epoch);
      report.observe_metadata(replica, initial, epoch);
      report.observe_first_ui(epoch);
      report.advance(epoch + 90s);
      report.observe_first_present(epoch + 100s);
      auto warmed = initial;
      warmed.allocation_width = 1920;
      report.observe_metadata(replica, warmed, epoch + 101s);
      report.advance(epoch + 110s);
      auto changed = warmed;
      changed.framebuffer_width = 640;
      report.observe_metadata(replica, changed, epoch + 111s);
      report.observe_runtime({1, 2}, epoch + 112s);
      report.observe_runtime({2, 1}, epoch + 113s);
      report.finish(epoch + 170s);
      const auto path = directory.file("identity.json");
      REQUIRE(report.write_new(path).has_value());
      const auto json = read_performance_report(path);
      CHECK(json.at("complete") == false);
      CHECK(json.at("window").at("warmup_start_ns") == 100'000'000'000LL);
      CHECK(json.at("owners").at("replica").at("metadata").at("allocation_width") == 1920);
      CHECK(json.at("owners").at("replica").at("metadata").at("framebuffer_width") == 1280);
      CHECK(json.at("owners").at("replica").at("metadata").at("requested_bridge") == "cpu");
      const auto reasons = json.at("incomplete_reasons").get<std::vector<std::string>>();
      CHECK(std::ranges::find(reasons, "metadata_changed") != reasons.end());
      CHECK(std::ranges::find(reasons, "runtime_changed") != reasons.end());
      const auto second_write = report.write_new(directory.file("second.json"));
      REQUIRE_FALSE(second_write.has_value());
      CHECK(second_write.error() == perf::PerformanceWriteError::AlreadyWritten);
      CHECK_FALSE(std::filesystem::exists(directory.file("second.json")));

      Report early{perf::PerformanceScope::Controller};
      early.observe_first_ui(epoch);
      early.finish(epoch + 11s, perf::PerformanceDrainState::Stalled);
      early.observe_counter(replica, perf::PerformanceCounter::Admitted, 99, epoch + 12s);
      early.finish(epoch + 80s);
      const auto early_path = directory.file("early.json");
      REQUIRE(early.write_new(early_path).has_value());
      const auto early_json = read_performance_report(early_path);
      CHECK(early_json.at("complete") == false);
      CHECK(early_json.at("drain") == "stalled");
      CHECK(early_json.at("owners").at("replica").at("counters").at("admitted") == 0);
      const auto early_reasons = early_json.at("incomplete_reasons").get<std::vector<std::string>>();
      CHECK(std::ranges::find(early_reasons, "early_shutdown") != early_reasons.end());
      CHECK(std::ranges::find(early_reasons, "drain_incomplete") != early_reasons.end());

      Report collision{perf::PerformanceScope::Controller};
      collision.finish(epoch);
      const auto collision_result = collision.write_new(path);
      REQUIRE_FALSE(collision_result.has_value());
      CHECK(collision_result.error() == perf::PerformanceWriteError::TargetExists);
      CHECK(read_performance_report(path) == json);
      Report inaccessible{perf::PerformanceScope::Controller};
      inaccessible.finish(epoch);
      const auto inaccessible_result = inaccessible.write_new(directory.file("missing/report.json"));
      REQUIRE_FALSE(inaccessible_result.has_value());
      CHECK(inaccessible_result.error() == perf::PerformanceWriteError::OpenFailed);
      CHECK_FALSE(std::filesystem::exists(directory.file("missing")));

      Report missing{perf::PerformanceScope::Controller};
      missing.observe_first_ui(epoch);
      missing.finish(epoch + 70s);
      const auto missing_path = directory.file("missing-metadata.json");
      REQUIRE(missing.write_new(missing_path).has_value());
      const auto missing_json = read_performance_report(missing_path);
      CHECK(missing_json.at("complete") == false);
      CHECK(missing_json.at("incomplete_reasons").get<std::vector<std::string>>() ==
            std::vector<std::string>{"metadata_missing"});
      CHECK(missing_json.at("owners").at("controller").at("high_water").at("transient_bytes").is_null());

      Report never_started{perf::PerformanceScope::Replica};
      never_started.observe_first_ui(epoch);
      never_started.finish(epoch + 100s);
      const auto never_path = directory.file("never-started.json");
      REQUIRE(never_started.write_new(never_path).has_value());
      const auto never_json = read_performance_report(never_path);
      CHECK(never_json.at("complete") == false);
      CHECK(never_json.at("window").at("measurement_start_ns").is_null());
      CHECK(never_json.at("stop_reason") == "scope_not_started");

      struct StopRow {
        perf::PerformanceScope scope;
        perf::PerformanceOwner owner;
        Report::Duration after;
        bool complete;
      };
      const std::array stop_rows{
        StopRow{perf::PerformanceScope::Controller, perf::PerformanceOwner::Controller, 69900ms, false},
        StopRow{perf::PerformanceScope::Controller, perf::PerformanceOwner::Controller, 70s, true},
        StopRow{perf::PerformanceScope::Controller, replica, 69900ms, true},
        StopRow{perf::PerformanceScope::Replica, replica, 69900ms, false},
        StopRow{perf::PerformanceScope::Replica, perf::PerformanceOwner::Controller, 69900ms, false}};
      for (std::size_t index{}; index < stop_rows.size(); ++index) {
        const auto &row = stop_rows[index];
        CAPTURE(index);
        Report stopping{row.scope};
        stopping.observe_metadata(perf::PerformanceOwner::Controller, initial, epoch);
        stopping.observe_metadata(replica, initial, epoch);
        stopping.observe_first_ui(epoch);
        stopping.observe_first_present(epoch);
        stopping.observe_stop(row.owner, epoch + row.after);
        stopping.observe_counter(replica, perf::PerformanceCounter::Submitted, 1, epoch + row.after + 1ms);
        stopping.finish(epoch + 70100ms);
        const auto stop_path = directory.file("stop-" + std::to_string(index) + ".json");
        REQUIRE(stopping.write_new(stop_path).has_value());
        const auto stop_json = read_performance_report(stop_path);
        CHECK(stop_json.at("complete") == row.complete);
        if (!row.complete) {
          CHECK(stop_json.at("stop_reason") == "early_shutdown");
          CHECK(stop_json.at("window").at("stopped_ns") == row.after.count());
          CHECK(stop_json.at("window").at("measurement_end_ns") == 70'000'000'000LL);
          CHECK(stop_json.at("owners").at("replica").at("counters").at("submitted") == 0);
        }
      }
    }

    SECTION("Vulkan classifier tables")
    {
      using Action = render::AcquireAction;
      using Ownership = render::AcquireOwnership;
      struct AcquireRow final {
        VkResult result;
        bool full_screen_exclusive_enabled;
        Action action;
        Ownership ownership;
        bool recreate_after_safe_use;
      };
      constexpr std::array acquire_rows{
        AcquireRow{VK_SUCCESS, false, Action::UseImage,
                   Ownership::ImageAndSyncAcquired, false},
        AcquireRow{VK_SUBOPTIMAL_KHR, false, Action::UseImage,
                   Ownership::ImageAndSyncAcquired, true},
        AcquireRow{VK_NOT_READY, false, Action::Wait, Ownership::Unchanged,
                   false},
        AcquireRow{VK_TIMEOUT, false, Action::Wait, Ownership::Unchanged,
                   false},
        AcquireRow{VK_ERROR_OUT_OF_DATE_KHR, false, Action::RecreateSwapchain,
                   Ownership::Unchanged, false},
        AcquireRow{VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT, true,
                   Action::RecreateSwapchain, Ownership::Unchanged, false},
        AcquireRow{VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT, false,
                   Action::Terminal, Ownership::Unchanged, false},
        AcquireRow{VK_ERROR_SURFACE_LOST_KHR, false, Action::RecreateSurface,
                   Ownership::Unchanged, false},
        AcquireRow{VK_ERROR_DEVICE_LOST, false, Action::RebuildDevice,
                   Ownership::Unchanged, false},
        AcquireRow{VK_ERROR_OUT_OF_HOST_MEMORY, false, Action::Terminal,
                   Ownership::Unchanged, false},
        AcquireRow{VK_ERROR_OUT_OF_DEVICE_MEMORY, false, Action::Terminal,
                   Ownership::Unchanged, false},
        AcquireRow{VK_ERROR_UNKNOWN, false, Action::Terminal,
                   Ownership::Unchanged, false},
      };

      for (const auto &row : acquire_rows) {
        CAPTURE(row.result, row.full_screen_exclusive_enabled);
        const auto classified = render::classify_acquire_result(
          row.result, row.full_screen_exclusive_enabled);
        CHECK(classified.action == row.action);
        CHECK(classified.ownership == row.ownership);
        CHECK(classified.recreate_after_safe_use == row.recreate_after_safe_use);
      }

      struct SubmitRow final {
        VkResult result;
        render::SubmitOutcome expected;
      };
      constexpr std::array submit_rows{
        SubmitRow{VK_SUCCESS, render::SubmitOutcome::Enqueued},
        SubmitRow{VK_ERROR_OUT_OF_HOST_MEMORY,
                  render::SubmitOutcome::NotEnqueued},
        SubmitRow{VK_ERROR_OUT_OF_DEVICE_MEMORY,
                  render::SubmitOutcome::NotEnqueued},
        SubmitRow{VK_ERROR_DEVICE_LOST, render::SubmitOutcome::DeviceLost},
        SubmitRow{VK_TIMEOUT, render::SubmitOutcome::Uncertain},
      };
      for (const auto &row : submit_rows) {
        CAPTURE(row.result);
        CHECK(render::classify_submit_result(row.result) == row.expected);
      }

      using PresentOwnership = render::PresentOwnership;
      using PresentRecovery = render::PresentRecovery;
      struct PresentRow final {
        VkResult result;
        render::PresentFeatureFacts features;
        PresentOwnership ownership;
        PresentRecovery recovery;
        bool presentation_pending;
      };
      constexpr std::array present_rows{
        PresentRow{VK_SUCCESS,
                   {},
                   PresentOwnership::Enqueued,
                   PresentRecovery::None,
                   true},
        PresentRow{VK_SUBOPTIMAL_KHR,
                   {},
                   PresentOwnership::Enqueued,
                   PresentRecovery::RecreateSwapchain,
                   true},
        PresentRow{VK_ERROR_OUT_OF_DATE_KHR,
                   {},
                   PresentOwnership::Enqueued,
                   PresentRecovery::RecreateSwapchain,
                   true},
        PresentRow{VK_ERROR_SURFACE_LOST_KHR,
                   {},
                   PresentOwnership::Enqueued,
                   PresentRecovery::RecreateSurface,
                   true},
        PresentRow{VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT,
                   {.full_screen_exclusive_enabled = true},
                   PresentOwnership::Enqueued,
                   PresentRecovery::RecreateSwapchain,
                   true},
        PresentRow{VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT,
                   {.full_screen_exclusive_enabled = false},
                   PresentOwnership::Uncertain,
                   PresentRecovery::Terminal,
                   true},
        PresentRow{VK_ERROR_OUT_OF_HOST_MEMORY,
                   {},
                   PresentOwnership::NotEnqueued,
                   PresentRecovery::Terminal,
                   false},
        PresentRow{VK_ERROR_OUT_OF_DEVICE_MEMORY,
                   {},
                   PresentOwnership::NotEnqueued,
                   PresentRecovery::Terminal,
                   false},
        PresentRow{VK_ERROR_DEVICE_LOST,
                   {},
                   PresentOwnership::Uncertain,
                   PresentRecovery::RebuildDevice,
                   true},
        PresentRow{VK_TIMEOUT,
                   {},
                   PresentOwnership::Uncertain,
                   PresentRecovery::Terminal,
                   true},
      };
      for (const auto &row : present_rows) {
        CAPTURE(row.result);
        const auto classified =
          render::classify_present_result(row.result, row.features);
        CHECK(classified.ownership == row.ownership);
        CHECK(classified.recovery == row.recovery);
        CHECK(classified.presentation_pending == row.presentation_pending);
      }

#if defined(VK_EXT_present_timing)
      const auto timing_full =
        render::classify_present_result(VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT,
                                        {.present_timing_enabled = true});
      CHECK(timing_full.ownership == PresentOwnership::Enqueued);
      CHECK(timing_full.recovery == PresentRecovery::None);
      CHECK(timing_full.presentation_pending);
#endif

      struct ReleaseRow final {
        VkResult result;
        render::ReleaseOwnership ownership;
        render::ReleaseRecovery recovery;
      };
      constexpr std::array release_rows{
        ReleaseRow{VK_SUCCESS, render::ReleaseOwnership::Released,
                   render::ReleaseRecovery::None},
        ReleaseRow{VK_ERROR_SURFACE_LOST_KHR,
                   render::ReleaseOwnership::Retained,
                   render::ReleaseRecovery::SurfaceLost},
        ReleaseRow{VK_ERROR_VALIDATION_FAILED_EXT,
                   render::ReleaseOwnership::Uncertain,
                   render::ReleaseRecovery::Terminal},
        ReleaseRow{VK_TIMEOUT, render::ReleaseOwnership::Uncertain,
                   render::ReleaseRecovery::Terminal},
        ReleaseRow{VK_ERROR_UNKNOWN, render::ReleaseOwnership::Uncertain,
                   render::ReleaseRecovery::Terminal},
      };
      for (const auto &row : release_rows) {
        CAPTURE(row.result);
        const auto classified = render::classify_release_result(row.result);
        CHECK(classified.ownership == row.ownership);
        CHECK(classified.recovery == row.recovery);
      }
    }

    SECTION("classifier-derived ownership traces")
    {
      // Newest frame selection and one requested retained redraw.
      {
        using Phase = render::RenderSlotPhase;
        using Candidate = render::RenderFrameCandidate;
        const std::array queued{
          Candidate{Phase::CpuReady, 1}, Candidate{Phase::GpuReady, 2},
          Candidate{Phase::CpuReady, 3}};
        CHECK(render::select_render_frame(queued, 0, false) == 2);
        CHECK_FALSE(render::select_render_frame(queued, 3, false));

        // Source loss leaves no capture checkout and no native submission. Its
        // last completed slot is usable only when a replacement needs repainting.
        const std::array retained{
          Candidate{Phase::Empty, 3, true}, Candidate{}, Candidate{}};
        CHECK_FALSE(render::select_render_frame(retained, 3, false));
        CHECK(render::select_render_frame(retained, 3, true) == 0);
        CHECK_FALSE(render::select_render_frame(retained, 4, true));

        const std::array fresh{
          Candidate{Phase::Empty, 3, true}, Candidate{Phase::GpuReady, 4},
          Candidate{Phase::CpuReady, 2}};
        CHECK(render::select_render_frame(fresh, 3, true) == 1);
        const std::array in_flight{
          Candidate{Phase::Submitted, 4, true},
          Candidate{Phase::ProducerPending, 5}, Candidate{Phase::GpuReady, 2}};
        CHECK_FALSE(render::select_render_frame(in_flight, 4, true));
        CHECK_FALSE(render::select_render_frame(
          std::array{Candidate{}, Candidate{}, Candidate{}}, 0, true));
      }

      const render::RuntimeDrainPolicy drain_policy;
      constexpr std::array release_results{VK_SUCCESS, VK_ERROR_SURFACE_LOST_KHR,
                                           VK_ERROR_VALIDATION_FAILED_EXT};

      // Trace 1: an acquired image is known not to have been enqueued after OOM,
      // but maintenance release remains unavailable until the acquire fence
      // proves acquisition complete.
      const auto acquired = render::classify_acquire_result(VK_SUCCESS, false);
      REQUIRE(acquired.ownership ==
              render::AcquireOwnership::ImageAndSyncAcquired);
      const auto submit_oom =
        render::classify_submit_result(VK_ERROR_OUT_OF_DEVICE_MEMORY);
      REQUIRE(submit_oom == render::SubmitOutcome::NotEnqueued);
      const auto acquire_proof_phase =
        render::slot_phase_after_submit(submit_oom);
      REQUIRE(acquire_proof_phase ==
              render::RenderSlotPhase::AcquireProofPending);
      CHECK(render::proof_fence_for_slot(*acquire_proof_phase) ==
            render::SlotProofFence::Acquire);
      const auto before_acquire_proof = drain_policy.evaluate(
        {.pending_ownership =
           render::slot_has_vulkan_work(*acquire_proof_phase),
         .ownership_uncertain = false,
         .progress = render::DrainProgress::Poll,
         .elapsed_no_progress = 0ms});
      CHECK_FALSE(before_acquire_proof.may_complete);
      CHECK_FALSE(before_acquire_proof.restart_no_progress_interval);
      const auto acquire_proof_observed = drain_policy.evaluate(
        {.pending_ownership = true,
         .ownership_uncertain = false,
         .progress = render::DrainProgress::TrackedRetirement,
         .elapsed_no_progress = 50ms});
      CHECK_FALSE(acquire_proof_observed.may_complete);
      CHECK(acquire_proof_observed.restart_no_progress_interval);
      for (const auto result : release_results) {
        CAPTURE(result);
        const auto released = render::classify_release_result(result);
        const auto release_plan =
          render::plan_slot_release(*acquire_proof_phase, released);
        CHECK(release_plan.destroy_acquire_semaphore);
        CHECK_FALSE(release_plan.destroy_render_finished_semaphore);
        const auto drain = drain_policy.evaluate(
          {.pending_ownership =
             render::slot_has_vulkan_work(release_plan.next_phase),
           .ownership_uncertain =
             released.ownership == render::ReleaseOwnership::Uncertain,
           .progress = release_plan.clear_acquired_ownership
                         ? render::DrainProgress::ProvenSafeRelease
                         : render::DrainProgress::Poll,
           .elapsed_no_progress = 0ms});
        if (release_plan.clear_acquired_ownership) {
          CHECK(release_plan.next_phase == render::RenderSlotPhase::Empty);
          CHECK(drain.may_complete);
          CHECK_FALSE(drain.poll_after.has_value());
        }
        else {
          CHECK(release_plan.next_phase ==
                render::RenderSlotPhase::ReleaseRetained);
          CHECK_FALSE(drain.may_complete);
          REQUIRE(drain.poll_after.has_value());
          CHECK(*drain.poll_after > 0ms);
        }
      }

      // Trace 2: an uncertain submit remains unresolved until an owner records a
      // proof; without that proof, the same ownership reaches Stalled.
      const auto uncertain_submit = render::classify_submit_result(VK_TIMEOUT);
      REQUIRE(uncertain_submit == render::SubmitOutcome::Uncertain);
      const auto uncertain_phase =
        render::slot_phase_after_submit(uncertain_submit);
      REQUIRE(uncertain_phase == render::RenderSlotPhase::SubmitUncertain);
      CHECK(render::proof_fence_for_slot(*uncertain_phase) ==
            render::SlotProofFence::Submission);
      const auto before_proof = drain_policy.evaluate(
        {.pending_ownership = render::slot_has_vulkan_work(*uncertain_phase),
         .ownership_uncertain =
           uncertain_submit == render::SubmitOutcome::Uncertain,
         .progress = render::DrainProgress::Poll,
         .elapsed_no_progress = 0ms});
      CHECK_FALSE(before_proof.may_complete);
      REQUIRE(before_proof.poll_after.has_value());
      const auto proof_release = render::plan_slot_release(
        *uncertain_phase, render::classify_release_result(VK_SUCCESS));
      CHECK_FALSE(proof_release.destroy_acquire_semaphore);
      CHECK(proof_release.destroy_render_finished_semaphore);
      CHECK(proof_release.clear_acquired_ownership);
      const auto after_proof = drain_policy.evaluate(
        {.pending_ownership =
           render::slot_has_vulkan_work(proof_release.next_phase),
         .ownership_uncertain = false,
         .progress = render::DrainProgress::ProvenSafeRelease,
         .elapsed_no_progress = 9999ms});
      CHECK(after_proof.may_complete);
      CHECK_FALSE(after_proof.poll_after.has_value());
      const auto without_proof = drain_policy.evaluate(
        {.pending_ownership = render::slot_has_vulkan_work(*uncertain_phase),
         .ownership_uncertain =
           uncertain_submit == render::SubmitOutcome::Uncertain,
         .progress = render::DrainProgress::Retry,
         .elapsed_no_progress = 10s});
      CHECK(without_proof.status == avb::core::RuntimeDrainStatus::Stalled);
      CHECK_FALSE(without_proof.may_complete);
      REQUIRE(without_proof.poll_after.has_value());
      CHECK(*without_proof.poll_after > 0ms);
      CHECK(*without_proof.poll_after <= 50ms);

      // Trace 3: a known-not-enqueued present keeps the acquired image owned
      // after submission completion; the later release classification controls
      // whether draining may complete.
      const auto submitted = render::classify_submit_result(VK_SUCCESS);
      REQUIRE(submitted == render::SubmitOutcome::Enqueued);
      const auto submitted_phase = render::slot_phase_after_submit(submitted);
      REQUIRE(submitted_phase == render::RenderSlotPhase::Submitted);
      const auto presented =
        render::classify_present_result(VK_ERROR_OUT_OF_DEVICE_MEMORY);
      REQUIRE(presented.ownership == render::PresentOwnership::NotEnqueued);
      REQUIRE_FALSE(presented.presentation_pending);
      const auto present_phase =
        render::slot_phase_after_present(presented.ownership);
      REQUIRE(present_phase == render::RenderSlotPhase::PresentNotEnqueued);
      CHECK(render::proof_fence_for_slot(present_phase) ==
            render::SlotProofFence::Submission);
      const auto submission_completed = drain_policy.evaluate(
        {.pending_ownership = render::slot_has_vulkan_work(present_phase),
         .ownership_uncertain =
           presented.ownership == render::PresentOwnership::Uncertain,
         .progress = render::DrainProgress::TrackedRetirement,
         .elapsed_no_progress = 50ms});
      CHECK(submission_completed.restart_no_progress_interval);
      CHECK_FALSE(submission_completed.may_complete);
      for (const auto result : release_results) {
        CAPTURE(result);
        const auto released = render::classify_release_result(result);
        const auto release_plan =
          render::plan_slot_release(present_phase, released);
        CHECK_FALSE(release_plan.destroy_acquire_semaphore);
        CHECK(release_plan.destroy_render_finished_semaphore);
        const auto drain = drain_policy.evaluate(
          {.pending_ownership =
             render::slot_has_vulkan_work(release_plan.next_phase),
           .ownership_uncertain =
             released.ownership == render::ReleaseOwnership::Uncertain,
           .progress = release_plan.clear_acquired_ownership
                         ? render::DrainProgress::ProvenSafeRelease
                         : render::DrainProgress::Poll,
           .elapsed_no_progress = 0ms});
        CHECK(drain.may_complete == release_plan.clear_acquired_ownership);
        if (!drain.may_complete) {
          REQUIRE(drain.poll_after.has_value());
          CHECK(*drain.poll_after > 0ms);
        }
      }
    }

    SECTION("retired swapchains and surface-only recovery")
    {
      // A queued present may outlive same-surface replacement. It prevents
      // destruction of the retired swapchain, not use of the active handle as
      // oldSwapchain after render/acquire ownership has retired.
      constexpr render::SwapchainOwnershipFacts presenting_only{
        .native_image_use_pending = false,
        .present_fence_pending = true,
        .release_retained = false,
      };
      const auto replacement =
        render::plan_same_surface_swapchain_create(true, presenting_only);
      CHECK(replacement.may_create);
      CHECK(replacement.retire_active_before_create);
      CHECK(replacement.use_active_as_old);
      CHECK_FALSE(render::retired_swapchain_destroyable(presenting_only));

      constexpr render::SwapchainOwnershipFacts render_use_pending{
        .native_image_use_pending = true,
      };
      CHECK_FALSE(
        render::plan_same_surface_swapchain_create(true, render_use_pending)
          .may_create);
      CHECK_FALSE(render::retired_swapchain_destroyable(render_use_pending));

      // Once the old present fence retires, the old state becomes destroyable;
      // if the first creation failed, the now-empty active slot makes the next
      // production decision a fresh create with VK_NULL_HANDLE.
      constexpr render::SwapchainOwnershipFacts fully_retired{};
      CHECK(render::retired_swapchain_destroyable(fully_retired));
      const auto retry =
        render::plan_same_surface_swapchain_create(false, fully_retired);
      CHECK(retry.may_create);
      CHECK_FALSE(retry.retire_active_before_create);
      CHECK_FALSE(retry.use_active_as_old);

      // A retained release forbids same-surface replacement, while a surface
      // teardown may legally clear it only after native use and present fences
      // have both retired.
      constexpr render::SwapchainOwnershipFacts release_surface_lost{
        .native_image_use_pending = false,
        .present_fence_pending = false,
        .release_retained = true,
      };
      const auto blocked =
        render::plan_same_surface_swapchain_create(true, release_surface_lost);
      CHECK_FALSE(blocked.may_create);
      CHECK_FALSE(blocked.retire_active_before_create);
      CHECK_FALSE(blocked.use_active_as_old);
      CHECK(render::retired_swapchain_destroyable(release_surface_lost));

      const auto surface_scope = render::graphics_recovery_scope(
        render::GraphicsFailureCategory::Surface);
      CHECK(surface_scope.recreate_surface);
      CHECK(surface_scope.recreate_swapchain);
      CHECK(render::render_stack_local_recovery(surface_scope));

      const render::GraphicsRetryPolicy retry_policy;
      const auto surface = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Surface,
         .attempt = 0,
         .current_generation = 12,
         .elapsed_no_progress = 0ms});
      CHECK(surface.action == render::GraphicsRetryAction::SurfaceRecreate);
      REQUIRE(surface.candidate_generation.has_value());
      CHECK(*surface.candidate_generation == 12);

      auto runtime_recovery = capture_detail::observe_runtime_recovery_events(
        {}, {.started = true, .completed = true});
      CHECK(runtime_recovery.pending_started);
      CHECK(runtime_recovery.pending_completed);
      runtime_recovery =
        capture_detail::commit_runtime_recovery_events(runtime_recovery);
      CHECK_FALSE(runtime_recovery.published_active);
      CHECK_FALSE(runtime_recovery.pending_started);
      CHECK_FALSE(runtime_recovery.pending_completed);
    }

    SECTION("source-size reconfiguration handshake and signal merge")
    {
      using PixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
      constexpr auto format = PixelFormat::B8G8R8A8UIntNormalized;
      constexpr Extent old_extent{640, 480};
      constexpr Extent new_extent{1024, 768};
      capture_detail::FrameCallbackPolicy policy{3, old_extent, 7, format, [] {}};
      policy.open_current_epoch();
      CHECK(policy.observe_frame({7, old_extent, {800, 600}, format}).accepted);
      CHECK(policy.observe_frame({7, old_extent, new_extent, format}).accepted);
      const auto merged = policy.drain_signals();
      REQUIRE(merged.resize.has_value());
      CHECK(*merged.resize == new_extent);

      const auto runtime_facts =
        capture_detail::merge_runtime_capture_signals(true, merged);
      CHECK(runtime_facts.source_closed);
      CHECK_FALSE(runtime_facts.capture_failed);
      REQUIRE(runtime_facts.resize.has_value());
      CHECK(*runtime_facts.resize == new_extent);

      const auto stale_epoch = policy.current_epoch();
      const auto current_epoch = policy.close_for_reconfigure();
      CHECK(current_epoch != stale_epoch);
      CHECK(policy.frame_callbacks_quiescent());
      CHECK_FALSE(policy.admission_open());
      CHECK_FALSE(policy.enter(stale_epoch).has_value());
      REQUIRE(policy.prepare_current_epoch(new_extent, 8).has_value());
      CHECK_FALSE(policy.admission_open());

      capture_detail::FrameCallbackPolicy stale_generation{3, new_extent, 8,
                                                           format, [] {}};
      stale_generation.open_current_epoch();
      CHECK_FALSE(
        stale_generation.observe_frame({7, new_extent, new_extent, format})
          .accepted);

      using Phase = capture_detail::SourceReconfigurationPhase;
      using Action = capture_detail::SourceReconfigurationAction;
      CHECK(capture_detail::next_source_reconfiguration_action(
              {.phase = Phase::WaitingForCallbacks}) == Action::Wait);
      CHECK(capture_detail::next_source_reconfiguration_action(
              {.phase = Phase::WaitingForCallbacks,
               .callbacks_quiescent = true}) == Action::DiscardFinalFrame);
      CHECK(capture_detail::next_source_reconfiguration_action(
              {.phase = Phase::WaitingForRender}) == Action::Wait);
      CHECK(capture_detail::next_source_reconfiguration_action(
              {.phase = Phase::WaitingForRender, .render_idle = true}) ==
            Action::Prepare);
      CHECK(capture_detail::next_source_reconfiguration_action(
              {.phase = Phase::Prepared, .source_open = true}) ==
            Action::Commit);
      policy.open_current_epoch();
      CHECK(policy.admission_open());
      CHECK(policy.current_epoch() == current_epoch);
      CHECK(capture_detail::next_source_reconfiguration_action(
              {.phase = Phase::Prepared, .source_open = false}) ==
            Action::Cancel);
      CHECK(capture_detail::next_source_reconfiguration_action(
              {.phase = Phase::WaitingForCallbacks,
               .callbacks_quiescent = true,
               .source_open = true,
               .capture_failed = true}) == Action::Cancel);
    }

    SECTION("D3 completed-value classifier")
    {
      CHECK(render::classify_d3_completed_value(40, 41) ==
            render::D3CompletionStatus::Pending);
      CHECK(render::classify_d3_completed_value(41, 41) ==
            render::D3CompletionStatus::Complete);
      CHECK(render::classify_d3_completed_value(42, 41) ==
            render::D3CompletionStatus::Complete);
      CHECK(render::classify_d3_completed_value(
              (std::numeric_limits<std::uint64_t>::max)(), 1) ==
            render::D3CompletionStatus::DeviceRemoved);

      const auto pre_submit_cancellation = render::plan_device_recovery_slot(
        render::DeviceLossKind::D3DeviceRemoved,
        render::RenderSlotPhase::ProducerPending);
      CHECK(pre_submit_cancellation ==
            render::DeviceRecoverySlotAction::ClearWithoutWait);

      REQUIRE(render::classify_d3_completed_value(41, 41) ==
              render::D3CompletionStatus::Complete);
      REQUIRE(render::classify_submit_result(VK_SUCCESS) ==
              render::SubmitOutcome::Enqueued);
      CHECK(render::plan_device_recovery_slot(
              render::DeviceLossKind::D3DeviceRemoved,
              render::RenderSlotPhase::Submitted) ==
            render::DeviceRecoverySlotAction::AwaitVulkanRetirement);
      const render::RuntimeDrainPolicy drain_policy;
      const auto submitted =
        drain_policy.evaluate({.pending_ownership = true,
                               .progress = render::DrainProgress::Poll,
                               .elapsed_no_progress = 0ms});
      CHECK_FALSE(submitted.may_complete);
      const auto retired = drain_policy.evaluate(
        {.pending_ownership = false,
         .progress = render::DrainProgress::TrackedRetirement,
         .elapsed_no_progress = 1ms});
      CHECK(retired.may_complete);
    }

    SECTION("device-loss classification and bridge selection")
    {
      const auto vulkan_loss =
        render::classify_submit_result(VK_ERROR_DEVICE_LOST);
      const auto d3_removal = render::classify_d3_completed_value(
        (std::numeric_limits<std::uint64_t>::max)(), 0);
      CHECK(vulkan_loss == render::SubmitOutcome::DeviceLost);
      CHECK(d3_removal == render::D3CompletionStatus::DeviceRemoved);

      const auto vulkan_loss_during_d3 = render::plan_device_recovery_slot(
        render::DeviceLossKind::VulkanDeviceLost,
        render::RenderSlotPhase::ProducerPending);
      CHECK(vulkan_loss_during_d3 ==
            render::DeviceRecoverySlotAction::AwaitD3Completion);

      const auto vulkan_loss_after_submit = render::plan_device_recovery_slot(
        render::DeviceLossKind::VulkanDeviceLost,
        render::RenderSlotPhase::Submitted);
      CHECK(vulkan_loss_after_submit ==
            render::DeviceRecoverySlotAction::ClearWithoutWait);

      const auto d3_loss_before_submit = render::plan_device_recovery_slot(
        render::DeviceLossKind::D3DeviceRemoved,
        render::RenderSlotPhase::GpuReady);
      CHECK(d3_loss_before_submit ==
            render::DeviceRecoverySlotAction::ClearWithoutWait);

      constexpr render::BridgeCapabilityFacts all_true{true, true, true, true,
                                                       true, true, true};
      STATIC_CHECK_FALSE(std::default_initializable<render::BridgeSelection>);
      CHECK(render::select_bridge(all_true) ==
            render::BridgeSelection{BridgeMode::GpuInterop, BridgeReason::None});

      constexpr std::array reasons{
        BridgeReason::AdapterLuidUnavailableOrMismatch,
        BridgeReason::ExternalImageUnsupported,
        BridgeReason::RequiredD3d11InterfacesUnavailable,
        BridgeReason::SharedFenceOrTimelineSemaphoreUnavailable,
        BridgeReason::TimelineFenceImportUnavailable,
        BridgeReason::Win32ImportProcedureUnavailable,
        BridgeReason::CompatibleMemoryTypeUnavailable,
      };
      for (std::size_t failed{}; failed < reasons.size(); ++failed) {
        auto facts = all_true;
        auto &[luid, image, interfaces, fence, timeline, procedure, memory] =
          facts;
        std::array<bool *, 7> ordered{&luid, &image, &interfaces, &fence,
                                      &timeline, &procedure, &memory};
        *ordered[failed] = false;
        if (failed + 1 < ordered.size()) {
          *ordered[failed + 1] = false;
        }
        CAPTURE(failed);
        CHECK(render::select_bridge(facts) ==
              render::BridgeSelection{BridgeMode::CpuCopy, reasons[failed]});
      }

      const auto setup_fallback = render::select_interop_fallback(
        render::InteropFallbackPoint::PreSubmitSetupFailure);
      REQUIRE(setup_fallback.has_value());
      CHECK(*setup_fallback ==
            render::BridgeSelection{BridgeMode::CpuCopy,
                                    BridgeReason::InteropOperationFailure});

      CHECK_FALSE(render::select_interop_fallback(
                    render::InteropFallbackPoint::RuntimeWorkPending)
                    .has_value());
      const auto retired_fallback = render::select_interop_fallback(
        render::InteropFallbackPoint::RuntimeWorkRetired);
      REQUIRE(retired_fallback.has_value());
      CHECK(*retired_fallback ==
            render::BridgeSelection{BridgeMode::CpuCopy,
                                    BridgeReason::InteropOperationFailure});

      const render::BridgeSelection gpu{BridgeMode::GpuInterop,
                                        BridgeReason::None};
      const render::BridgeSelection cpu{BridgeMode::CpuCopy,
                                        BridgeReason::InteropOperationFailure};
      CHECK_FALSE(render::should_probe_interop_generation(true, cpu, 7, 7));
      CHECK(render::should_probe_interop_generation(true, cpu, 7, 8));
      CHECK_FALSE(render::should_probe_interop_generation(false, cpu, 7, 8));
      CHECK(render::should_probe_interop_generation(true, gpu, 7, 7));
      auto runtime_bridge = capture_detail::observe_runtime_bridge_selection(
        capture_detail::RuntimeBridgeEventState{}, gpu);
      REQUIRE(runtime_bridge.pending.has_value());
      runtime_bridge =
        capture_detail::commit_runtime_bridge_selection(runtime_bridge);
      CHECK(runtime_bridge.published == gpu);
      runtime_bridge =
        capture_detail::observe_runtime_bridge_selection(runtime_bridge, cpu);
      REQUIRE(runtime_bridge.pending == cpu);
      runtime_bridge =
        capture_detail::observe_runtime_bridge_selection(runtime_bridge, gpu);
      CHECK_FALSE(runtime_bridge.pending.has_value());

      CHECK(render::vulkan_instance_create_error_code(
              VK_ERROR_INCOMPATIBLE_DRIVER) ==
            avb::core::AppErrorCode::UnsupportedGraphics);
      CHECK(render::vulkan_instance_create_error_code(
              VK_ERROR_INITIALIZATION_FAILED) ==
            avb::core::AppErrorCode::RuntimeConstructionFailure);
    }

    SECTION("scripted retry and whole-runtime drain timeline")
    {
      const render::GraphicsRetryPolicy retry_policy;
      constexpr std::array expected_delays{0ms, 100ms, 500ms};
      for (std::uint32_t attempt{}; attempt < expected_delays.size(); ++attempt) {
        const auto decision = retry_policy.evaluate(
          {.category = render::GraphicsFailureCategory::Swapchain,
           .attempt = attempt,
           .current_generation = 9,
           .elapsed_no_progress = 0ms});
        CAPTURE(attempt);
        CHECK(decision.action == render::GraphicsRetryAction::Retry);
        REQUIRE(decision.candidate_generation.has_value());
        CHECK(*decision.candidate_generation == 9);
        REQUIRE(decision.retry_after.has_value());
        CHECK(*decision.retry_after == expected_delays[attempt]);
      }
      const auto exhausted = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Swapchain,
         .attempt = 3,
         .current_generation = 9,
         .elapsed_no_progress = 500ms});
      CHECK(exhausted.action == render::GraphicsRetryAction::Terminal);
      CHECK_FALSE(exhausted.candidate_generation.has_value());

      const auto late_retry = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Swapchain,
         .attempt = 1,
         .current_generation = 9,
         .elapsed_no_progress = 700ms});
      REQUIRE(late_retry.retry_after == 0ms);
      CHECK(render::strictly_future_retry_delay(*late_retry.retry_after) == 1ms);
      CHECK(render::strictly_future_retry_delay(75ms) == 75ms);

      // Coordinator-owned episode state advances on construction attempts. A
      // constructed replacement remains in the same episode until one of its
      // successful presents retires; construction by itself cannot reset it.
      render::GraphicsRecoveryEpisodeState episode;
      const auto first_construction = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Swapchain,
         .attempt = episode.attempt,
         .current_generation = 9,
         .elapsed_no_progress = 0ms});
      REQUIRE(first_construction.retry_after == 0ms);
      episode = render::advance_graphics_recovery_episode(
        episode, render::GraphicsRecoveryObservation::ConstructionAttempted);
      episode = render::advance_graphics_recovery_episode(
        episode, render::GraphicsRecoveryObservation::ReplacementConstructed);
      CHECK(episode.attempt == 1);
      CHECK(episode.awaiting_successful_present);
      const auto repeated_before_present = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Swapchain,
         .attempt = episode.attempt,
         .current_generation = 9,
         .elapsed_no_progress = 25ms});
      REQUIRE(repeated_before_present.retry_after.has_value());
      CHECK(*repeated_before_present.retry_after == 75ms);
      episode = render::advance_graphics_recovery_episode(
        episode, render::GraphicsRecoveryObservation::SuccessfulPresentRetired);
      CHECK(episode.attempt == 0);
      CHECK_FALSE(episode.awaiting_successful_present);
      const auto unrelated_old_present =
        render::advance_graphics_recovery_episode(
          {.attempt = 2, .awaiting_successful_present = false},
          render::GraphicsRecoveryObservation::SuccessfulPresentRetired);
      CHECK(unrelated_old_present.attempt == 2);
      CHECK_FALSE(unrelated_old_present.awaiting_successful_present);
      const auto after_successful_present = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Swapchain,
         .attempt = episode.attempt,
         .current_generation = 9,
         .elapsed_no_progress = 0ms});
      REQUIRE(after_successful_present.retry_after.has_value());
      CHECK(*after_successful_present.retry_after == 0ms);

      const auto intrinsically_terminal = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Terminal,
         .attempt = 0,
         .current_generation = 9,
         .elapsed_no_progress = 0ms});
      CHECK(intrinsically_terminal.action ==
            render::GraphicsRetryAction::Terminal);
      CHECK_FALSE(intrinsically_terminal.candidate_generation.has_value());
      CHECK_FALSE(intrinsically_terminal.retry_after.has_value());

      const auto device = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Device,
         .attempt = 1,
         .current_generation = 9,
         .elapsed_no_progress = 40ms});
      CHECK(device.action == render::GraphicsRetryAction::DeviceRebuild);
      REQUIRE(device.candidate_generation.has_value());
      CHECK(*device.candidate_generation == 10);
      REQUIRE(device.retry_after.has_value());
      CHECK(*device.retry_after == 60ms);
      const auto generation_overflow = retry_policy.evaluate(
        {.category = render::GraphicsFailureCategory::Device,
         .attempt = 0,
         .current_generation = (std::numeric_limits<std::uint64_t>::max)(),
         .elapsed_no_progress = 0ms});
      CHECK(generation_overflow.action == render::GraphicsRetryAction::Terminal);
      CHECK_FALSE(generation_overflow.candidate_generation.has_value());

      const render::RuntimeDrainPolicy drain_policy;
      const auto pending =
        drain_policy.evaluate({.pending_ownership = true,
                               .ownership_uncertain = false,
                               .progress = render::DrainProgress::Poll,
                               .elapsed_no_progress = 1s});
      CHECK(pending.status == avb::core::RuntimeDrainStatus::Normal);
      CHECK_FALSE(pending.may_complete);
      CHECK_FALSE(pending.restart_no_progress_interval);
      REQUIRE(pending.poll_after.has_value());
      CHECK(*pending.poll_after == 50ms);
      CHECK(*pending.poll_after > 0ms);

      const auto near_stall =
        drain_policy.evaluate({.pending_ownership = true,
                               .ownership_uncertain = false,
                               .progress = render::DrainProgress::Warning,
                               .elapsed_no_progress = 9975ms});
      REQUIRE(near_stall.poll_after.has_value());
      CHECK(*near_stall.poll_after == 25ms);
      CHECK(*near_stall.poll_after > 0ms);

      const auto progress = drain_policy.evaluate(
        {.pending_ownership = true,
         .ownership_uncertain = false,
         .progress = render::DrainProgress::TrackedRetirement,
         .elapsed_no_progress = 9999ms});
      CHECK(progress.status == avb::core::RuntimeDrainStatus::Normal);
      CHECK(progress.restart_no_progress_interval);
      REQUIRE(progress.poll_after.has_value());
      CHECK(*progress.poll_after == 50ms);
      const auto proven_safe = drain_policy.evaluate(
        {.pending_ownership = true,
         .ownership_uncertain = false,
         .progress = render::DrainProgress::ProvenSafeRelease,
         .elapsed_no_progress = 9999ms});
      CHECK(proven_safe.restart_no_progress_interval);

      const auto stalled =
        drain_policy.evaluate({.pending_ownership = true,
                               .ownership_uncertain = true,
                               .progress = render::DrainProgress::NewPending,
                               .elapsed_no_progress = 10s});
      CHECK(stalled.status == avb::core::RuntimeDrainStatus::Stalled);
      CHECK_FALSE(stalled.may_complete);
      REQUIRE(stalled.poll_after.has_value());
      CHECK(*stalled.poll_after == 50ms);
      CHECK(*stalled.poll_after > 0ms);

      const auto complete =
        drain_policy.evaluate({.pending_ownership = false,
                               .ownership_uncertain = false,
                               .progress = render::DrainProgress::None,
                               .elapsed_no_progress = 10s});
      CHECK(complete.may_complete);
      CHECK_FALSE(complete.poll_after.has_value());

      const auto exceptional_pending =
        drain_policy.evaluate({.pending_ownership = true,
                               .ownership_uncertain = true,
                               .progress = render::DrainProgress::Retry,
                               .elapsed_no_progress = 500ms});
      CHECK_FALSE(exceptional_pending.may_complete);
      REQUIRE(exceptional_pending.poll_after.has_value());
      CHECK(*exceptional_pending.poll_after <= 50ms);
      CHECK(*exceptional_pending.poll_after > 0ms);

      capture_detail::CaptureJobStopState failed_stop;
      failed_stop.request_stop([] { throw 42; }, [] {});
      const auto cleanup_failure = failed_stop.cleanup_failure();
      REQUIRE(cleanup_failure.has_value());
      const auto cleanup_uncertain =
        capture_detail::capture_cleanup_is_uncertain(cleanup_failure);
      CHECK(cleanup_uncertain);
      CHECK_FALSE(failed_stop.stop_quiescent(true, true));
      const auto failed_cleanup_drain = capture_detail::advance_runtime_drain(
        {}, true, cleanup_uncertain, render::DrainProgress::Retry,
        std::chrono::steady_clock::time_point{16s});
      CHECK_FALSE(failed_cleanup_drain.may_complete);
      REQUIRE(failed_cleanup_drain.next_deadline.has_value());

      const auto timeline_start = std::chrono::steady_clock::time_point{17s};
      const auto first_exception = capture_detail::observe_runtime_exception(
        {}, capture_detail::RuntimeExceptionPhase::Running, timeline_start);
      CHECK(first_exception.emit_failure);
      CHECK(first_exception.retain_owners);
      CHECK_FALSE(first_exception.drain.may_complete);
      REQUIRE(first_exception.drain.next_deadline.has_value());
      CHECK(*first_exception.drain.next_deadline > timeline_start);
      CHECK(*first_exception.drain.next_deadline == timeline_start + 50ms);

      const auto second_exception = capture_detail::observe_runtime_exception(
        first_exception.drain.state,
        capture_detail::RuntimeExceptionPhase::Stopping, timeline_start + 10s);
      CHECK_FALSE(second_exception.emit_failure);
      CHECK(second_exception.retain_owners);
      CHECK_FALSE(second_exception.drain.may_complete);
      REQUIRE(second_exception.drain.status_change.has_value());
      CHECK(*second_exception.drain.status_change ==
            avb::core::RuntimeDrainStatus::Stalled);
      REQUIRE(second_exception.drain.next_deadline.has_value());
      CHECK(*second_exception.drain.next_deadline > timeline_start + 10s);

      const auto resumed = capture_detail::advance_runtime_drain(
        second_exception.drain.state, true, false,
        render::DrainProgress::TrackedRetirement, timeline_start + 10001ms);
      REQUIRE(resumed.status_change.has_value());
      CHECK(*resumed.status_change ==
            avb::core::RuntimeDrainStatus::Normal);
      CHECK_FALSE(resumed.may_complete);
      REQUIRE(resumed.next_deadline.has_value());
      CHECK(*resumed.next_deadline > timeline_start + 10001ms);

      const auto proven_after_exception = capture_detail::advance_runtime_drain(
        resumed.state, false, false, render::DrainProgress::ProvenSafeRelease,
        timeline_start + 10002ms);
      CHECK(proven_after_exception.may_complete);
      CHECK_FALSE(proven_after_exception.next_deadline.has_value());
    }
  }

}    // namespace
