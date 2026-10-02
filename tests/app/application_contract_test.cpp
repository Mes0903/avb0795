#include "avb/app/application.hpp"

#include "avb/app/app_command_queue.hpp"
#include "avb/app/config_store.hpp"
#include "avb/app/localized_presentation.hpp"
#include "avb/app/ports.hpp"
#include "avb/i18n/catalog.hpp"
#include "src/desktop/ui_cadence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  using avb::app::AppCommandQueue;
  using avb::app::Application;
  using avb::app::RuntimeEvent;
  using avb::app::RuntimeTickResult;
  using avb::core::AppError;
  using avb::core::AppErrorCode;
  using avb::core::CaptureExclusionStatus;
  using avb::core::SourceDescriptor;

  constexpr avb::core::PlatformCapabilities all_capabilities{
    .display_capture = true,
    .window_capture = true,
    .pass_through = true,
    .borderless_resize = true,
    .global_hotkey = true,
    .capture_exclusion = true,
    .browser_capture_keep_alive = true,
  };

  [[nodiscard]] SourceDescriptor make_source(std::string id = "display-1")
  {
    return {
      .id = {.value = std::move(id)},
      .kind = avb::core::SourceKind::Display,
      .display_label = "Display 1",
      .extent = {.width = 1920, .height = 1080},
      .color_support = avb::core::ColorSupport::Sdr,
    };
  }

  [[nodiscard]] RuntimeTickResult
  make_tick(std::initializer_list<RuntimeEvent> events,
            std::optional<Clock::time_point> next_wake = std::nullopt)
  {
    RuntimeTickResult result{.next_wake = next_wake};
    std::size_t index{};
    for (const auto &event : events) {
      REQUIRE(index < result.events.size());
      result.events[index++] = event;
    }
    return result;
  }

  [[nodiscard]] RuntimeTickResult make_tick_at(
    std::initializer_list<std::pair<std::size_t, RuntimeEvent>> events,
    std::optional<Clock::time_point> next_wake = std::nullopt)
  {
    RuntimeTickResult result{.next_wake = next_wake};
    for (const auto &[index, event] : events) {
      REQUIRE(index < result.events.size());
      result.events[index] = event;
    }
    return result;
  }

  enum class RecordedCall { ConfigReplace,
                            HotkeyClear,
                            RuntimeStop };

  struct CallTrace {
    void record(RecordedCall call) noexcept
    {
      if (count < calls.size()) {
        calls[count] = call;
      }
      ++count;
    }

    void clear() noexcept { count = 0; }

    std::array<RecordedCall, 32> calls{};
    std::size_t count{};
  };

  struct RuntimeProbe {
    std::deque<std::expected<void, AppError>> apply_results;
    std::deque<RuntimeTickResult> ticks;
    std::array<avb::core::ReplicaSettings, 16> applied{};
    std::size_t apply_count{};
    std::size_t tick_count{};
    std::size_t request_stop_count{};
    std::size_t destruction_count{};
    std::optional<Clock::time_point> last_tick;
    CallTrace *trace{};
  };

  class RecordingRuntime final : public avb::app::IReplicaRuntime {
  public:
    explicit RecordingRuntime(std::shared_ptr<RuntimeProbe> probe)
        : probe_{std::move(probe)}
    {
    }

    ~RecordingRuntime() override { ++probe_->destruction_count; }

    std::expected<void, AppError>
    apply(const avb::core::ReplicaSettings &settings) noexcept override
    {
      if (probe_->apply_count < probe_->applied.size()) {
        probe_->applied[probe_->apply_count] = settings;
      }
      ++probe_->apply_count;
      if (probe_->apply_results.empty()) {
        return {};
      }
      auto result = probe_->apply_results.front();
      probe_->apply_results.pop_front();
      return result;
    }

    RuntimeTickResult tick(Clock::time_point now) noexcept override
    {
      ++probe_->tick_count;
      probe_->last_tick = now;
      if (probe_->ticks.empty()) {
        return {};
      }
      auto result = std::move(probe_->ticks.front());
      probe_->ticks.pop_front();
      return result;
    }

    void request_stop() noexcept override
    {
      ++probe_->request_stop_count;
      if (probe_->trace) {
        probe_->trace->record(RecordedCall::RuntimeStop);
      }
    }

  private:
    std::shared_ptr<RuntimeProbe> probe_;
  };

  class RecordingCatalog final : public avb::app::ISourceCatalog {
  public:
    std::deque<std::expected<std::vector<SourceDescriptor>, AppError>> results;
    std::vector<SourceDescriptor> fallback{make_source()};
    std::size_t enumerate_count{};

    std::expected<std::vector<SourceDescriptor>, AppError> enumerate() override
    {
      ++enumerate_count;
      if (results.empty()) {
        return fallback;
      }
      auto result = std::move(results.front());
      results.pop_front();
      return result;
    }
  };

  struct FactoryPlan {
    std::shared_ptr<RuntimeProbe> probe;
    std::optional<AppError> error;
    bool return_null{};
  };

  class RecordingFactory final : public avb::app::IReplicaRuntimeFactory {
  public:
    std::deque<FactoryPlan> plans;
    std::vector<SourceDescriptor> sources;
    std::vector<avb::core::ReplicaSettings> settings;
    std::shared_ptr<RuntimeProbe> last_probe;

    std::expected<std::unique_ptr<avb::app::IReplicaRuntime>, AppError>
    create(const SourceDescriptor &source,
           const avb::core::ReplicaSettings &requested) override
    {
      sources.push_back(source);
      settings.push_back(requested);
      FactoryPlan plan;
      if (!plans.empty()) {
        plan = std::move(plans.front());
        plans.pop_front();
      }
      if (plan.error) {
        return std::unexpected(*plan.error);
      }
      if (plan.return_null) {
        return std::unique_ptr<avb::app::IReplicaRuntime>{};
      }
      last_probe = plan.probe ? std::move(plan.probe)
                              : std::make_shared<RuntimeProbe>();
      last_probe->trace = trace;
      return std::make_unique<RecordingRuntime>(last_probe);
    }

    CallTrace *trace{};
  };

  class RecordingHotkey final : public avb::app::IGlobalHotkey {
  public:
    std::deque<std::expected<void, AppError>> results;
    std::vector<avb::core::HotkeyChord> replacements;
    std::size_t clear_count{};

    std::expected<void, AppError>
    replace(const avb::core::HotkeyChord &hotkey) override
    {
      replacements.push_back(hotkey);
      if (throw_on_replace_call == replacements.size()) {
        throw std::runtime_error{"scripted hotkey exception"};
      }
      if (results.empty()) {
        return {};
      }
      auto result = results.front();
      results.pop_front();
      return result;
    }

    void clear() noexcept override
    {
      ++clear_count;
      if (trace) {
        trace->record(RecordedCall::HotkeyClear);
      }
    }

    std::optional<std::size_t> throw_on_replace_call;
    CallTrace *trace{};
  };

  class ConfigFixture final {
  public:
    ConfigFixture()
        : directory_{create_unique_directory()},
          path_{directory_ / "config.json"},
          store_{[this](const std::filesystem::path &temporary,
                        const std::filesystem::path &destination) {
            ++replace_count;
            if (trace) {
              trace->record(RecordedCall::ConfigReplace);
            }
            if (!replace_succeeds) {
              return std::expected<void, avb::core::AppError>{
                std::unexpected{avb::core::AppError{
                  avb::core::AppErrorCode::ConfigWriteFailure}}};
            }
            std::error_code error;
            std::filesystem::remove(destination, error);
            error.clear();
            std::filesystem::rename(temporary, destination, error);
            if (error) {
              return std::expected<void, avb::core::AppError>{
                std::unexpected{avb::core::AppError{
                  avb::core::AppErrorCode::ConfigWriteFailure,
                  static_cast<std::int64_t>(error.value())}}};
            }
            return std::expected<void, avb::core::AppError>{};
          }}
    {
    }

    ~ConfigFixture()
    {
      std::error_code ignored;
      std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
      return path_;
    }

    [[nodiscard]] const std::filesystem::path &directory() const noexcept
    {
      return directory_;
    }

    [[nodiscard]] avb::app::ConfigStore &store() noexcept
    {
      return store_;
    }

    bool replace_succeeds{true};
    std::size_t replace_count{};
    CallTrace *trace{};

  private:
    [[nodiscard]] static std::filesystem::path create_unique_directory()
    {
      constexpr int max_attempts{64};
      const auto parent = std::filesystem::temp_directory_path();
      std::random_device random;
      for (int attempt{}; attempt < max_attempts; ++attempt) {
        const auto candidate =
          parent / ("avb0795-v1-case-01-" + std::to_string(random()) + "-" +
                    std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
          return candidate;
        }
        if (error) {
          throw std::filesystem::filesystem_error{
            "create temporary directory", candidate, error};
        }
      }
      throw std::runtime_error{"could not create a unique temporary directory"};
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
    avb::app::ConfigStore store_;
  };

  struct Harness {
    Harness()
    {
      factory.trace = &trace;
      hotkey.trace = &trace;
      config.trace = &trace;
    }

    CallTrace trace;
    AppCommandQueue queue;
    RecordingCatalog catalog;
    RecordingFactory factory;
    RecordingHotkey hotkey;
    ConfigFixture config;
    avb::app::SystemLanguageQuery system_language_query = [] {
      return avb::i18n::Language::English;
    };

    [[nodiscard]] std::unique_ptr<Application> make_application(
      std::optional<std::filesystem::path> config_path = std::nullopt,
      avb::core::PlatformCapabilities capabilities = all_capabilities,
      const std::filesystem::path &legacy_config_path = {})
    {
      return std::make_unique<Application>(
        queue, catalog, factory, hotkey, config.store(),
        config_path.value_or(config.path()),
        capabilities, CaptureExclusionStatus::Excluded,
        system_language_query, legacy_config_path);
    }
  };

  void select_and_start(Harness &harness, Application &application, const std::shared_ptr<RuntimeProbe> &probe, Clock::time_point now)
  {
    const auto session_id = application.view_state().session_id;
    harness.factory.plans.push_back(FactoryPlan{.probe = probe});
    REQUIRE(harness.queue.push(avb::app::SelectSource{
      session_id, harness.catalog.fallback.front().id}));
    REQUIRE(harness.queue.push(avb::app::Start{session_id}));
    application.step(now);
    REQUIRE(application.view_state().status_text ==
            avb::i18n::text(
              avb::i18n::TextId::Capturing,
              application.view_state().effective_language));
  }

  [[nodiscard]] avb::core::HotkeyChord
  hotkey(avb::core::HotkeyKey key)
  {
    return {.modifiers = {.control = true, .shift = true}, .key = key};
  }

  void check_notice(const Application &application, AppErrorCode code)
  {
    REQUIRE(application.view_state().application_notice_text.has_value());
    CHECK(*application.view_state().application_notice_text ==
          avb::app::error_text(
            code, application.view_state().effective_language));
  }

}    // namespace

TEST_CASE("avb0795 V1 case 01: Application orchestration contract",
          "[avb0795-v1]")
{
  const auto now = Clock::time_point{1s};

  SECTION("host wait reenters application for UI commands before any deadline")
  {
    using avb::desktop::detail::choose_host_wait;
    Harness harness;
    auto application = harness.make_application();
    application->step(now);
    const auto session = application->view_state().session_id;
    const auto source_id = harness.catalog.fallback.front().id;
    REQUIRE_FALSE(application->view_state().selected_source_id.has_value());

    // This is the queue boundary used by the controller sink after its UI build.
    const bool command_emitted = harness.queue.push(
      avb::app::SelectSource{session, source_id});
    REQUIRE(command_emitted);
    const auto command_wait = choose_host_wait(
      command_emitted, now + 50ms, now + 2ms, now + 100ms);
    REQUIRE(command_wait.immediate_reentry);
    CHECK_FALSE(command_wait.deadline.has_value());
    CHECK(choose_host_wait(true, {}, {}, {}).immediate_reentry);
    application->step(now);
    CHECK(application->view_state().selected_source_id == source_id);

    const auto progress_wait = choose_host_wait(
      false, now + 50ms, now + 2ms, now + 100ms);
    CHECK_FALSE(progress_wait.immediate_reentry);
    CHECK(progress_wait.deadline == now + 2ms);
    CHECK(choose_host_wait(false, now + 1ms, now + 2ms,
                           now + 100ms).deadline == now + 1ms);
    CHECK(choose_host_wait(false, now + 50ms, now + 2ms,
                           now + 1ms).deadline == now + 1ms);
    // A hidden controller contributes an empty UI deadline; progress still wakes.
    CHECK(choose_host_wait(false, {}, now + 2ms, {}).deadline == now + 2ms);
    const auto idle_wait = choose_host_wait(false, {}, {}, {});
    CHECK_FALSE(idle_wait.immediate_reentry);
    CHECK_FALSE(idle_wait.deadline.has_value());
  }

  SECTION("restart restores eight general preferences without source or crop")
  {
    for (const bool pass_through_supported : {false, true}) {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session = application->view_state().session_id;
      REQUIRE(harness.queue.push(avb::app::SetLanguage{
        avb::i18n::LanguagePreference::Japanese}));
      REQUIRE(harness.queue.push(avb::app::ReplaceHotkey{
        session, hotkey(avb::core::HotkeyKey::F10)}));
      REQUIRE(harness.queue.push(avb::app::SetFps{
        session, avb::core::FpsCap::Fps144}));
      REQUIRE(harness.queue.push(avb::app::SetBorder{
        session, avb::core::BorderPreference::Borderless}));
      REQUIRE(harness.queue.push(avb::app::SetMode{
        session, avb::core::OverlayMode::PassThrough}));
      REQUIRE(harness.queue.push(avb::app::SetExtensions{
        session, {.browser_capture_keep_alive = true}}));
      REQUIRE(harness.queue.push(avb::app::SetAutoRefresh{true}));
      REQUIRE(harness.queue.push(avb::app::SetOpacity{session, 25, false}));
      REQUIRE(harness.queue.push(avb::app::SetOpacity{session, 25, true}));
      probe->ticks.push_back(make_tick({avb::app::CaptureContentChanged{{100, 80}}}));
      application->step(now + 1ms);
      REQUIRE(harness.queue.push(avb::app::SetRegion{
        session, {"display-1"}, avb::core::PixelRegion{10, 10, 50, 40}}));
      application->step(now + 2ms);
      REQUIRE(application->view_state().settings.region);
      application->shutdown();
      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 3ms);
      REQUIRE(application->exit_disposition() == avb::core::ExitDisposition::Clean);
      application.reset();

      const auto original = harness.config.store().load(harness.config.path());
      REQUIRE(original);
      const auto primary = harness.config.directory() / "avb0795.config";
      auto capabilities = all_capabilities;
      capabilities.pass_through = pass_through_supported;
      const auto saves = harness.config.replace_count;
      Harness restarted;
      application = restarted.make_application(primary, capabilities, harness.config.path());
      const auto &view = application->view_state();
      CHECK(view.language_preference == avb::i18n::LanguagePreference::Japanese);
      CHECK(restarted.hotkey.replacements.back() == hotkey(avb::core::HotkeyKey::F10));
      CHECK(view.settings.fps_cap == avb::core::FpsCap::Fps144);
      CHECK(view.settings.border == avb::core::BorderPreference::Borderless);
      CHECK(view.settings.mode == (pass_through_supported
        ? avb::core::OverlayMode::PassThrough : avb::core::OverlayMode::Editable));
      CHECK(view.settings.extensions.browser_capture_keep_alive);
      CHECK(view.auto_refresh_enabled);
      CHECK(view.settings.opacity_percent == 25);
      CHECK_FALSE(view.settings.region);
      CHECK_FALSE(view.settings.selecting_region);
      for (const auto &source : view.sources) {
        CHECK_FALSE(source.selected);
      }
      CHECK(harness.factory.sources.size() == 1);
      CHECK(restarted.factory.sources.empty());
      CHECK(harness.config.replace_count == saves);
      CHECK(restarted.config.replace_count == 0);
      CHECK_FALSE(std::filesystem::exists(primary));
      CHECK_FALSE(application->next_deadline());
      application->step(now + 10s);
      CHECK(application->next_deadline() == now + 15s);
      REQUIRE(restarted.queue.push(avb::app::SetAutoRefresh{false}));
      application->step(now + 11s);
      const auto migrated = harness.config.store().load(primary);
      REQUIRE(migrated);
      auto expected = *original;
      expected.auto_refresh_enabled = false;
      CHECK(*migrated == expected);
      CHECK(migrated->mode == avb::core::OverlayMode::PassThrough);
      CHECK(harness.config.store().load(harness.config.path()) == original);
      application.reset();
    }
  }

  SECTION("mode and auto refresh retry failed saves on the same accepted value")
  {
    for (const bool automatic_refresh : {false, true}) {
      Harness harness;
      REQUIRE(harness.config.store().save(harness.config.path(), {}));
      auto application = harness.make_application();
      const auto session = application->view_state().session_id;
      const auto enqueue = [&] {
        if (automatic_refresh) {
          REQUIRE(harness.queue.push(avb::app::SetAutoRefresh{true}));
        } else {
          REQUIRE(harness.queue.push(avb::app::SetMode{
            session, avb::core::OverlayMode::PassThrough}));
        }
      };
      harness.config.replace_count = 0;
      harness.config.replace_succeeds = false;
      enqueue();
      application->step(now);
      CHECK(harness.config.replace_count == 1);
      check_notice(*application, AppErrorCode::ConfigWriteFailure);
      CHECK(harness.config.store().load(harness.config.path()) == avb::app::UserConfig{});
      harness.config.replace_succeeds = true;
      enqueue();
      application->step(now + 1s);
      CHECK(harness.config.replace_count == 2);
      CHECK_FALSE(application->view_state().application_notice_text);
      const auto saved = harness.config.store().load(harness.config.path());
      REQUIRE(saved);
      CHECK(saved->auto_refresh_enabled == automatic_refresh);
      CHECK(saved->mode == (automatic_refresh
        ? avb::core::OverlayMode::Editable : avb::core::OverlayMode::PassThrough));
      enqueue();
      application->step(now + 2s);
      CHECK(harness.config.replace_count == 2);
      if (automatic_refresh) {
        CHECK(application->next_deadline() == now + 5s);
      }
    }
  }

  SECTION("global Editable restoration persists during interrupted and graphics recovery")
  {
    for (const auto &event : std::array<RuntimeEvent, 2>{
           avb::app::SourceLost{}, avb::app::GraphicsRecoveryStarted{}}) {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      REQUIRE(harness.queue.push(avb::app::SetMode{
        application->view_state().session_id, avb::core::OverlayMode::PassThrough}));
      probe->ticks.push_back(make_tick({event}));
      application->step(now + 1ms);
      REQUIRE(harness.config.replace_count == 1);
      probe->apply_results.push_back(std::unexpected(
        AppError{AppErrorCode::PlatformOperationFailure}));
      REQUIRE(harness.queue.push(avb::app::RestoreActiveReplicaEditable{}));
      application->step(now + 2ms);
      CHECK(harness.config.replace_count == 1);
      CHECK(application->view_state().settings.mode == avb::core::OverlayMode::PassThrough);
      harness.config.replace_succeeds = false;
      REQUIRE(harness.queue.push(avb::app::RestoreActiveReplicaEditable{}));
      application->step(now + 3ms);
      CHECK(harness.config.replace_count == 2);
      CHECK(application->view_state().settings.mode == avb::core::OverlayMode::Editable);
      check_notice(*application, AppErrorCode::ConfigWriteFailure);
      harness.config.replace_succeeds = true;
      REQUIRE(harness.queue.push(avb::app::RestoreActiveReplicaEditable{}));
      application->step(now + 4ms);
      CHECK(harness.config.replace_count == 3);
      REQUIRE(harness.config.store().load(harness.config.path()));
      CHECK(harness.config.store().load(harness.config.path())->mode ==
            avb::core::OverlayMode::Editable);
      REQUIRE(harness.queue.push(avb::app::RestoreActiveReplicaEditable{}));
      application->step(now + 5ms);
      CHECK(harness.config.replace_count == 3);
    }
  }

  SECTION("region commands require presented bounds and retain only the current source crop")
  {
    Harness harness;
    harness.catalog.fallback.push_back(make_source("display-2"));
    auto application = harness.make_application();
    const auto probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, probe, now);
    const auto session = application->view_state().session_id;
    const avb::core::SourceId source{"display-1"};
    const avb::core::PixelRegion crop{10, 20, 70, 40};
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, source, crop}));
    application->step(now + 1ms);
    CHECK(probe->apply_count == 0);
    CHECK_FALSE(application->view_state().controls.region.enabled);
    probe->ticks.push_back(make_tick({avb::app::CaptureContentChanged{{100, 80}}}));
    application->step(now + 2ms);
    REQUIRE(application->view_state().content_extent == avb::core::Extent{100, 80});
    REQUIRE(application->view_state().controls.region.enabled);
    REQUIRE(harness.queue.push(avb::app::SetRegion{{session.value + 1}, source, crop}));
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, {"display-2"}, crop}));
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, source, avb::core::PixelRegion{90, 0, 11, 1}}));
    application->step(now + 3ms);
    CHECK(probe->apply_count == 0);
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, source, crop}));
    application->step(now + 4ms);
    REQUIRE(application->view_state().settings.region == crop);
    CHECK(harness.config.replace_count == 0);
    REQUIRE(harness.queue.push(avb::app::Stop{session}));
    probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
    application->step(now + 5ms);
    REQUIRE(harness.queue.push(avb::app::Start{session}));
    application->step(now + 6ms);
    CHECK(harness.factory.settings.back().region == crop);
    CHECK_FALSE(application->view_state().content_extent);
    harness.factory.last_probe->ticks.push_back(make_tick({avb::app::SourceLost{}}));
    application->step(now + 7ms);
    REQUIRE(harness.queue.push(avb::app::SelectSource{session, {"display-2"}}));
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, {"display-2"}, crop}));
    application->step(now + 8ms);
    CHECK_FALSE(application->view_state().settings.region);
    CHECK_FALSE(application->view_state().content_extent);
    CHECK(harness.factory.last_probe->apply_count == 0);
    REQUIRE(harness.queue.push(avb::app::Start{session}));
    harness.factory.last_probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
    application->step(now + 9ms);
    CHECK(application->view_state().session_id != session);
    CHECK_FALSE(harness.factory.settings.back().region);
  }

  SECTION("region selection commits complete results and cancellation preserves accepted settings")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, probe, now);
    const auto session = application->view_state().session_id;
    const avb::core::SourceId source{"display-1"};
    const avb::core::PixelRegion crop{20, 10, 60, 40};
    probe->ticks.push_back(make_tick({avb::app::CaptureContentChanged{{100, 80}}}));
    application->step(now + 1ms);
    REQUIRE(harness.queue.push(avb::app::SetMode{session, avb::core::OverlayMode::PassThrough}));
    REQUIRE(harness.queue.push(avb::app::SetOpacity{session, 25, false}));
    REQUIRE(harness.queue.push(avb::app::SetRegionSelection{session, source, true}));
    application->step(now + 2ms);
    REQUIRE(application->view_state().settings.selecting_region);
    CHECK(probe->applied[2].mode == avb::core::OverlayMode::PassThrough);
    CHECK(probe->applied[2].opacity_percent == 25);
    CHECK(harness.config.replace_count == 2);
    const auto saved = harness.config.store().load(harness.config.path());
    REQUIRE(saved);
    CHECK(saved->mode == avb::core::OverlayMode::PassThrough);
    CHECK(saved->opacity_percent == 25);
    probe->ticks.push_back(make_tick({avb::app::RegionSelectionCompleted{crop}}));
    application->step(now + 3ms);
    REQUIRE(application->view_state().settings.region == crop);
    REQUIRE_FALSE(application->view_state().settings.selecting_region);
    CHECK(application->next_deadline() == now + 3ms);
    REQUIRE(harness.queue.push(avb::app::SetRegionSelection{session, source, true}));
    application->step(now + 4ms);
    probe->ticks.push_back(make_tick({avb::app::RegionSelectionCompleted{std::nullopt}}));
    application->step(now + 5ms);
    CHECK(application->view_state().settings.region == crop);
    CHECK_FALSE(application->view_state().settings.selecting_region);
    probe->apply_results.push_back(std::unexpected(AppError{AppErrorCode::PlatformOperationFailure}));
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, source, std::nullopt}));
    application->step(now + 6ms);
    CHECK(application->view_state().settings.region == crop);
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, source, std::nullopt}));
    application->step(now + 7ms);
    CHECK_FALSE(application->view_state().settings.region);
    CHECK(harness.config.replace_count == 2);
    CHECK(harness.config.store().load(harness.config.path()) == saved);
    probe->ticks.push_back(make_tick({avb::app::RegionSelectionCompleted{crop}}));
    application->step(now + 8ms);
    CHECK_FALSE(application->view_state().settings.region);
  }

  SECTION("region results lose to lifecycle and content invalidation regardless of slot order")
  {
    for (const auto event : std::array<RuntimeEvent, 5>{
           avb::app::SourceLost{}, avb::app::GraphicsRecoveryStarted{},
           avb::app::OverlayCloseRequested{}, avb::app::CaptureContentChanged{{}},
           avb::app::Failed{AppError{AppErrorCode::RuntimeStateLost}}}) {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session = application->view_state().session_id;
      const avb::core::SourceId source{"display-1"};
      probe->ticks.push_back(make_tick({avb::app::CaptureContentChanged{{100, 80}}}));
      application->step(now + 1ms);
      REQUIRE(harness.queue.push(avb::app::SetRegionSelection{session, source, true}));
      application->step(now + 2ms);
      REQUIRE(application->view_state().settings.selecting_region);
      probe->ticks.push_back(make_tick_at({
        {0, avb::app::RegionSelectionCompleted{avb::core::PixelRegion{0, 0, 10, 10}}},
        {8, avb::app::CaptureContentChanged{{100, 80}}}, {9, event}}));
      application->step(now + 3ms);
      CHECK_FALSE(application->view_state().settings.region);
      CHECK_FALSE(application->view_state().settings.selecting_region);
      CHECK_FALSE(application->view_state().content_extent);
      CHECK_FALSE(application->view_state().controls.region.enabled);
    }
  }

  SECTION("presented source resize intersects crop and cancels stale drags")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, probe, now);
    const auto session = application->view_state().session_id;
    const avb::core::SourceId source{"display-1"};
    probe->ticks.push_back(make_tick({avb::app::CaptureContentChanged{{200, 100}}}));
    application->step(now + 1ms);
    REQUIRE(harness.queue.push(avb::app::SetRegion{session, source, avb::core::PixelRegion{90, 20, 40, 30}}));
    REQUIRE(harness.queue.push(avb::app::SetRegionSelection{session, source, true}));
    application->step(now + 2ms);
    probe->ticks.push_back(make_tick({
      avb::app::RegionSelectionCompleted{avb::core::PixelRegion{0, 0, 5, 5}},
      avb::app::CaptureContentChanged{{100, 40}}}));
    application->step(now + 3ms);
    CHECK(application->view_state().settings.region == avb::core::PixelRegion{90, 20, 10, 20});
    CHECK_FALSE(application->view_state().settings.selecting_region);
    CHECK(application->view_state().content_extent == avb::core::Extent{100, 40});
    CHECK(application->next_deadline() == now + 3ms);
    probe->ticks.push_back(make_tick({avb::app::CaptureContentChanged{{80, 40}}}));
    application->step(now + 4ms);
    CHECK_FALSE(application->view_state().settings.region);
    CHECK(application->view_state().controls.region.enabled);
    CHECK(harness.config.replace_count == 0);
  }

  SECTION("wheel zoom commits only accepted preferences across runtime generations")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto session_id = application->view_state().session_id;
    CHECK(application->view_state().settings.wheel_zoom_percent == 5);
    CHECK(application->view_state().controls.wheel_zoom.enabled);
    REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{session_id, 12}));
    application->step(now);
    CHECK(application->view_state().settings.wheel_zoom_percent == 12);
    auto saved = harness.config.store().load(harness.config.path());
    REQUIRE(saved);
    CHECK(saved->wheel_zoom_percent == 12);
    const auto restarted = harness.make_application();
    CHECK(restarted->view_state().settings.wheel_zoom_percent == 12);

    const auto probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, probe, now + 1ms);
    REQUIRE(harness.factory.settings.front().wheel_zoom_percent == 12);
    CHECK(application->view_state().controls.wheel_zoom.enabled);
    for (const int percent : {1, 25}) {
      REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{session_id, percent}));
      application->step(now + 2ms);
      CHECK(application->view_state().settings.wheel_zoom_percent == percent);
      saved = harness.config.store().load(harness.config.path());
      REQUIRE(saved);
      CHECK(saved->wheel_zoom_percent == percent);
    }
    REQUIRE(probe->apply_count == 2);
    CHECK(probe->applied[0].wheel_zoom_percent == 1);
    CHECK(probe->applied[1].wheel_zoom_percent == 25);
    CHECK(probe->applied[1].opacity_percent == 100);

    for (const int percent : {0, 26}) {
      REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{session_id, percent}));
      application->step(now + 3ms);
      check_notice(*application, AppErrorCode::InvalidValue);
    }
    REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{
      avb::core::SessionId{session_id.value + 1}, 12}));
    application->step(now + 4ms);
    check_notice(*application, AppErrorCode::InvalidCommand);
    CHECK(probe->apply_count == 2);
    CHECK(harness.config.replace_count == 3);

    harness.config.replace_succeeds = false;
    REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{session_id, 12}));
    application->step(now + 5ms);
    CHECK(application->view_state().settings.wheel_zoom_percent == 12);
    CHECK(probe->apply_count == 3);
    CHECK(harness.config.replace_count == 4);
    check_notice(*application, AppErrorCode::ConfigWriteFailure);
    harness.config.replace_succeeds = true;
    REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{session_id, 12}));
    application->step(now + 6ms);
    CHECK(probe->apply_count == 3);
    CHECK(harness.config.replace_count == 5);
    CHECK_FALSE(application->view_state().application_notice_text);

    probe->ticks.push_back(make_tick({avb::app::SourceLost{}}));
    application->step(now + 7ms);
    const auto replacement = std::make_shared<RuntimeProbe>();
    harness.factory.plans.push_back(FactoryPlan{.probe = replacement});
    REQUIRE(harness.queue.push(avb::app::Start{session_id}));
    application->step(now + 8ms);
    CHECK_FALSE(application->view_state().controls.wheel_zoom.enabled);
    REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{session_id, 25}));
    application->step(now + 9ms);
    check_notice(*application, AppErrorCode::InvalidCommand);
    CHECK(probe->apply_count == 3);
    CHECK(harness.config.replace_count == 5);
    probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
    application->step(now + 10ms);
    REQUIRE(application->view_state().session_id != session_id);
    REQUIRE(harness.factory.settings.size() == 2);
    CHECK(harness.factory.settings.back().wheel_zoom_percent == 12);
    CHECK(application->view_state().settings.wheel_zoom_percent == 12);
    REQUIRE(harness.queue.push(avb::app::SetWheelZoomPercent{session_id, 25}));
    application->step(now + 11ms);
    CHECK(replacement->apply_count == 0);
    check_notice(*application, AppErrorCode::InvalidCommand);

    for (const auto error : {AppErrorCode::PlatformOperationFailure,
                             AppErrorCode::RuntimeStateLost}) {
      CAPTURE(error);
      Harness failure_harness;
      auto failure_application = failure_harness.make_application();
      const auto failure_id = failure_application->view_state().session_id;
      REQUIRE(failure_harness.queue.push(avb::app::SetWheelZoomPercent{failure_id, 12}));
      failure_application->step(now);
      const auto failure_probe = std::make_shared<RuntimeProbe>();
      select_and_start(failure_harness, *failure_application, failure_probe, now + 1ms);
      failure_probe->apply_results.push_back(std::unexpected(AppError{error}));
      REQUIRE(failure_harness.queue.push(avb::app::SetWheelZoomPercent{failure_id, 25}));
      failure_application->step(now + 2ms);
      REQUIRE(failure_probe->apply_count == 1);
      CHECK(failure_probe->applied[0].wheel_zoom_percent == 25);
      CHECK(failure_application->view_state().settings.wheel_zoom_percent == 12);
      CHECK(failure_harness.config.replace_count == 1);
      const auto preserved = failure_harness.config.store().load(failure_harness.config.path());
      REQUIRE(preserved);
      CHECK(preserved->wheel_zoom_percent == 12);
      CHECK(failure_probe->request_stop_count ==
            (error == AppErrorCode::RuntimeStateLost ? 1 : 0));
    }
  }

  SECTION("opacity previews apply complete candidates and commit only once")
  {
    Harness harness;
    auto stored = avb::app::UserConfig{};
    stored.opacity_percent = 75;
    stored.fps_cap = avb::core::FpsCap::Fps144;
    stored.border = avb::core::BorderPreference::Borderless;
    REQUIRE(harness.config.store().save(harness.config.path(), stored));
    harness.config.replace_count = 0;
    auto application = harness.make_application();
    CHECK(application->view_state().settings.opacity_percent == 75);
    const auto probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, probe, now);
    REQUIRE(harness.factory.settings.front().opacity_percent == 75);
    const auto session_id = application->view_state().session_id;

    for (const int percent : {50, 0, 100, 35}) {
      REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, percent}));
    }
    application->step(now + 1ms);
    REQUIRE(probe->apply_count == 4);
    CHECK(probe->applied[1].opacity_percent == 0);
    CHECK(probe->applied[2].opacity_percent == 100);
    CHECK(probe->applied[3].fps_cap == stored.fps_cap);
    CHECK(probe->applied[3].border == stored.border);
    CHECK(application->view_state().settings.opacity_percent == 35);
    CHECK(harness.config.replace_count == 0);
    REQUIRE(harness.config.store().load(harness.config.path()) == stored);

    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 35, true}));
    application->step(now + 2ms);
    CHECK(probe->apply_count == 4);
    CHECK(harness.config.replace_count == 1);
    stored.opacity_percent = 35;
    REQUIRE(harness.config.store().load(harness.config.path()) == stored);

    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 35, true}));
    application->step(now + 3ms);
    CHECK(probe->apply_count == 4);
    CHECK(harness.config.replace_count == 1);
  }

  SECTION("failed opacity applies preserve the last accepted value")
  {
    for (const auto error : {AppErrorCode::PlatformOperationFailure,
                             AppErrorCode::RuntimeStateLost}) {
      CAPTURE(error);
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;
      REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 65}));
      application->step(now + 1ms);
      probe->apply_results.push_back(std::unexpected(AppError{error}));
      REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 20}));
      application->step(now + 2ms);

      REQUIRE(probe->apply_count == 2);
      CHECK(probe->applied[1].opacity_percent == 20);
      CHECK(application->view_state().settings.opacity_percent == 65);
      CHECK(harness.config.replace_count == 0);
      CHECK(probe->request_stop_count ==
            (error == AppErrorCode::RuntimeStateLost ? 1 : 0));
      CHECK(probe->destruction_count == 0);
      if (error == AppErrorCode::PlatformOperationFailure) {
        REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 20, true}));
        application->step(now + 3ms);
        CHECK(probe->apply_count == 2);
        CHECK(harness.config.replace_count == 1);
        const auto saved = harness.config.store().load(harness.config.path());
        REQUIRE(saved);
        CHECK(saved->opacity_percent == 65);
      }
    }
  }

  SECTION("opacity rejects invalid stale and stopping commands before apply")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, probe, now);
    const auto session_id = application->view_state().session_id;
    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, -1, true}));
    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 101, true}));
    application->step(now + 1ms);
    check_notice(*application, AppErrorCode::InvalidValue);
    REQUIRE(harness.queue.push(avb::app::SetOpacity{
      avb::core::SessionId{session_id.value + 1}, 20, true}));
    application->step(now + 2ms);
    check_notice(*application, AppErrorCode::InvalidCommand);
    REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 20, true}));
    application->step(now + 3ms);
    CHECK(probe->apply_count == 0);
    CHECK(harness.config.replace_count == 0);
    CHECK(application->view_state().settings.opacity_percent == 100);
    check_notice(*application, AppErrorCode::InvalidCommand);
  }

  SECTION("failed opacity save keeps accepted state and commit retries it")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto session_id = application->view_state().session_id;
    harness.config.replace_succeeds = false;
    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 45}));
    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 45, true}));
    application->step(now);
    CHECK(application->view_state().settings.opacity_percent == 45);
    CHECK(harness.config.replace_count == 1);
    check_notice(*application, AppErrorCode::ConfigWriteFailure);
    harness.config.replace_succeeds = true;
    REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 45, true}));
    application->step(now + 1ms);
    CHECK(harness.config.replace_count == 2);
    const auto saved = harness.config.store().load(harness.config.path());
    REQUIRE(saved);
    CHECK(saved->opacity_percent == 45);
    CHECK_FALSE(application->view_state().application_notice_text);
  }

  SECTION("Stop flushes pending opacity while Quit retains the no-I/O drain")
  {
    for (const bool quit : {false, true}) {
      CAPTURE(quit);
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;
      REQUIRE(harness.queue.push(avb::app::SetOpacity{session_id, 30}));
      application->step(now + 1ms);
      REQUIRE(harness.config.replace_count == 0);
      if (quit) {
        REQUIRE(harness.queue.push(avb::app::Quit{}));
      } else {
        REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      }
      application->step(now + 2ms);
      CHECK(harness.config.replace_count == (quit ? 0 : 1));
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);
      CHECK(application->view_state().settings.opacity_percent == 30);
      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 3ms);
      CHECK(probe->destruction_count == 1);
      CHECK(harness.config.replace_count == (quit ? 0 : 1));
      if (quit) {
        CHECK(application->exit_disposition() == avb::core::ExitDisposition::Clean);
      } else {
        const auto saved = harness.config.store().load(harness.config.path());
        REQUIRE(saved);
        CHECK(saved->opacity_percent == 30);
      }
    }
  }

  SECTION("replacement sessions preserve preview opacity and reject old commits")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto old_probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, old_probe, now);
    const auto old_id = application->view_state().session_id;
    REQUIRE(harness.queue.push(avb::app::SetOpacity{old_id, 55}));
    application->step(now + 1ms);
    old_probe->ticks.push_back(make_tick({avb::app::SourceLost{}}));
    application->step(now + 2ms);
    const auto replacement = std::make_shared<RuntimeProbe>();
    harness.factory.plans.push_back(FactoryPlan{.probe = replacement});
    REQUIRE(harness.queue.push(avb::app::Start{old_id}));
    application->step(now + 3ms);
    old_probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
    application->step(now + 4ms);
    REQUIRE(application->view_state().session_id != old_id);
    REQUIRE(harness.factory.settings.size() == 2);
    CHECK(harness.factory.settings.back().opacity_percent == 55);
    CHECK(application->view_state().settings.opacity_percent == 55);
    REQUIRE(harness.queue.push(avb::app::SetOpacity{old_id, 20, true}));
    application->step(now + 5ms);
    CHECK(replacement->apply_count == 0);
    CHECK(harness.config.replace_count == 0);
    check_notice(*application, AppErrorCode::InvalidCommand);
  }

  SECTION("automatic discovery obeys deadlines and preserves selection")
  {
    Harness harness;
    auto application = harness.make_application();
    application->step(now);
    CHECK_FALSE(application->view_state().auto_refresh_enabled);
    CHECK_FALSE(application->next_deadline());
    REQUIRE(harness.queue.push(avb::app::SetAutoRefresh{true}));
    application->step(now);
    CHECK(application->view_state().auto_refresh_enabled);
    application.reset();
    harness.catalog.enumerate_count = 0;
    application = harness.make_application();
    CHECK(application->view_state().auto_refresh_enabled);
    CHECK_FALSE(application->next_deadline());
    application->step(now);
    CHECK(application->next_deadline() == now + 5s);
    application->step(now + 4999ms);
    CHECK(harness.catalog.enumerate_count == 1);
    application->step(now + 5s);
    CHECK(harness.catalog.enumerate_count == 2);
    application->step(now + 30s);
    CHECK(harness.catalog.enumerate_count == 3);
    CHECK(application->next_deadline() == now + 35s);

    // Both successful and failed manual attempts start a fresh full interval.
    REQUIRE(harness.queue.push(avb::app::RefreshSources{}));
    application->step(now + 31s);
    CHECK(harness.catalog.enumerate_count == 4);
    CHECK(application->next_deadline() == now + 36s);
    const auto retained = application->view_state().sources.front().id;
    harness.catalog.results.push_back(std::unexpected(
      AppError{AppErrorCode::SourceCatalogFailure}));
    REQUIRE(harness.queue.push(avb::app::RefreshSources{}));
    application->step(now + 32s);
    CHECK(harness.catalog.enumerate_count == 5);
    CHECK(application->next_deadline() == now + 37s);
    CHECK(application->view_state().sources.front().id == retained);
    check_notice(*application, AppErrorCode::SourceCatalogFailure);

    // A queued selection wins over a deadline that is due in this same step.
    REQUIRE(harness.queue.push(avb::app::SelectSource{
      application->view_state().session_id, retained}));
    application->step(now + 37s);
    CHECK(harness.catalog.enumerate_count == 5);
    CHECK(application->view_state().sources.front().selected);
    CHECK(application->view_state().auto_refresh_paused);
    CHECK_FALSE(application->next_deadline());
    const auto probe = std::make_shared<RuntimeProbe>();
    harness.factory.plans.push_back(FactoryPlan{.probe = probe});
    REQUIRE(harness.queue.push(avb::app::Start{
      application->view_state().session_id}));
    application->step(now + 38s);
    application->step(now + 60s);
    CHECK(harness.catalog.enumerate_count == 5);
    REQUIRE(harness.queue.push(avb::app::Stop{
      application->view_state().session_id}));
    application->step(now + 61s);
    CHECK(application->view_state().auto_refresh_paused);
    probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
    application->step(now + 62s);
    // A manual new catalog can clear the selection, enabling scheduling again.
    harness.catalog.fallback = {make_source("new-generation")};
    REQUIRE(harness.queue.push(avb::app::RefreshSources{}));
    application->step(now + 63s);
    CHECK_FALSE(application->view_state().auto_refresh_paused);
    CHECK(application->next_deadline() == now + 68s);
    REQUIRE(harness.queue.push(avb::app::SetAutoRefresh{false}));
    application->step(now + 64s);
    CHECK_FALSE(application->next_deadline());
    REQUIRE(harness.queue.push(avb::app::SetAutoRefresh{true}));
    application->step(now + 65s);
    application->shutdown();
    CHECK_FALSE(application->view_state().controls.auto_refresh.enabled);
    CHECK(application->exit_disposition() == avb::core::ExitDisposition::Clean);
  }

  SECTION("startup and source ownership")
  {
    {
      Harness harness;
      const avb::app::UserConfig stored{
        .hotkey = hotkey(avb::core::HotkeyKey::F10),
        .fps_cap = avb::core::FpsCap::Fps144,
        .border = avb::core::BorderPreference::Borderless,
      };
      REQUIRE(harness.config.store().save(harness.config.path(), stored));
      harness.config.replace_count = 0;
      harness.trace.clear();

      auto application = harness.make_application();

      CHECK(harness.catalog.enumerate_count == 1);
      REQUIRE(harness.hotkey.replacements.size() == 1);
      CHECK(harness.hotkey.replacements.front() == stored.hotkey);
      CHECK(application->view_state().sources.size() == 1);
      CHECK(application->view_state().settings.fps_cap == stored.fps_cap);
      CHECK(application->view_state().settings.border == stored.border);
      CHECK(application->view_state().hotkey_display_text == "Ctrl+Shift+F10");
      CHECK(application->view_state().hotkey_binding_confirmed);
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Continue);
    }

    {
      Harness harness;
      auto application = harness.make_application(harness.config.directory());
      CHECK(harness.catalog.enumerate_count == 1);
      CHECK(harness.hotkey.replacements.size() == 1);
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Continue);
      check_notice(*application, AppErrorCode::ConfigReadFailure);
    }

    {
      Harness harness;
      harness.catalog.results.push_back(std::unexpected(
        AppError{AppErrorCode::SourceCatalogFailure}));
      auto application = harness.make_application();
      CHECK(application->view_state().sources.empty());
      CHECK(harness.hotkey.replacements.size() == 1);
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Continue);
      check_notice(*application, AppErrorCode::SourceCatalogFailure);
    }

    {
      Harness harness;
      harness.hotkey.results.push_back(
        std::unexpected(AppError{AppErrorCode::HotkeyConflict}));
      auto application = harness.make_application();
      CHECK_FALSE(application->view_state().hotkey_binding_confirmed);
      CHECK(application->view_state().sources.size() == 1);
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Continue);
      check_notice(*application, AppErrorCode::HotkeyConflict);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto session_id = application->view_state().session_id;
      const avb::core::SessionId stale_id{session_id.value + 1};
      REQUIRE(harness.queue.push(avb::app::SelectSource{
        stale_id, harness.catalog.fallback.front().id}));
      REQUIRE(harness.queue.push(avb::app::Start{stale_id}));
      application->step(now);
      CHECK(harness.factory.sources.empty());
      CHECK_FALSE(application->view_state().sources.front().selected);
      check_notice(*application, AppErrorCode::InvalidCommand);

      REQUIRE(harness.queue.push(avb::app::SelectSource{
        session_id, avb::core::SourceId{"missing"}}));
      application->step(now + 1ms);
      CHECK(harness.factory.sources.empty());
      check_notice(*application, AppErrorCode::SourceUnavailable);
    }

    enum class FactoryFailure { TypedError,
                                NullRuntime };
    for (const auto failure :
         {FactoryFailure::TypedError, FactoryFailure::NullRuntime}) {
      CAPTURE(failure);
      Harness harness;
      auto application = harness.make_application();
      const auto session_id = application->view_state().session_id;
      FactoryPlan failed_plan;
      if (failure == FactoryFailure::TypedError) {
        failed_plan.error = AppError{AppErrorCode::PermissionDenied};
      }
      else {
        failed_plan.return_null = true;
      }
      harness.factory.plans.push_back(std::move(failed_plan));
      REQUIRE(harness.queue.push(avb::app::SelectSource{
        session_id, harness.catalog.fallback.front().id}));
      REQUIRE(harness.queue.push(avb::app::Start{session_id}));
      application->step(now);
      CHECK(application->view_state().status_text == "Error");
      CHECK(harness.factory.sources.size() == 1);
      check_notice(*application,
                   failure == FactoryFailure::TypedError
                     ? AppErrorCode::PermissionDenied
                     : AppErrorCode::RuntimeConstructionFailure);

      const auto retry_probe = std::make_shared<RuntimeProbe>();
      harness.factory.plans.push_back(FactoryPlan{.probe = retry_probe});
      REQUIRE(harness.queue.push(avb::app::Start{session_id}));
      application->step(now + 1ms);
      CHECK(application->view_state().status_text == "Capturing");
      CHECK(harness.factory.sources.size() == 2);
      CHECK(retry_probe->tick_count == 1);
      CHECK_FALSE(application->view_state().application_notice_text);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      const auto session_id = application->view_state().session_id;
      harness.factory.plans.push_back(FactoryPlan{.probe = probe});
      REQUIRE(harness.queue.push(avb::app::SelectSource{
        session_id, harness.catalog.fallback.front().id}));
      REQUIRE(harness.queue.push(avb::app::Start{session_id}));
      REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      application->step(now);
      CHECK(harness.factory.sources.size() == 1);
      CHECK(probe->request_stop_count == 1);
      CHECK(application->view_state().status_text == "Stopping");
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto old_probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, old_probe, now);
      const auto old_session_id = application->view_state().session_id;
      old_probe->ticks.push_back(make_tick({avb::app::SourceLost{}}));
      application->step(now + 1ms);
      REQUIRE(application->view_state().status_text == "Capture interrupted");

      const auto enumerations = harness.catalog.enumerate_count;
      harness.catalog.results.push_back(
        std::vector<SourceDescriptor>{make_source("display-2")});
      REQUIRE(harness.queue.push(avb::app::RefreshSources{}));
      application->step(now + 2ms);
      CHECK(harness.catalog.enumerate_count == enumerations);
      CHECK(application->view_state().status_text == "Capture interrupted");

      const auto replacement_probe = std::make_shared<RuntimeProbe>();
      harness.factory.plans.push_back(
        FactoryPlan{.probe = replacement_probe});
      REQUIRE(harness.queue.push(avb::app::Start{old_session_id}));
      application->step(now + 3ms);
      CHECK(application->view_state().status_text == "Stopping");
      CHECK(old_probe->request_stop_count == 1);
      CHECK(harness.factory.sources.size() == 1);
      CHECK(replacement_probe->tick_count == 0);

      application->step(now + 4ms);
      CHECK(harness.factory.sources.size() == 1);
      old_probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 5ms);
      CHECK(old_probe->destruction_count == 1);
      CHECK(harness.factory.sources.size() == 2);
      CHECK(replacement_probe->tick_count == 0);
      CHECK(application->view_state().status_text == "Capturing");
      CHECK(application->view_state().session_id != old_session_id);
    }
  }

  SECTION("language startup resolves injected system policy and safe fallbacks")
  {
    {
      Harness harness;
      std::size_t query_count{};
      harness.system_language_query = [&query_count] {
        ++query_count;
        return avb::i18n::Language::Japanese;
      };

      const auto application = harness.make_application();

      CHECK(query_count == 1);
      CHECK(application->view_state().language_preference ==
            avb::i18n::LanguagePreference::System);
      CHECK(application->view_state().effective_language ==
            avb::i18n::Language::Japanese);
      CHECK(application->view_state().status_text ==
            avb::i18n::text(avb::i18n::TextId::Idle,
                                  avb::i18n::Language::Japanese));
    }

    {
      Harness harness;
      auto stored = avb::app::UserConfig{};
      stored.language = avb::i18n::LanguagePreference::English;
      REQUIRE(harness.config.store().save(harness.config.path(), stored));
      std::size_t query_count{};
      harness.system_language_query = [&query_count] {
        ++query_count;
        return avb::i18n::Language::Japanese;
      };

      const auto application = harness.make_application();

      CHECK(query_count == 0);
      CHECK(application->view_state().language_preference == stored.language);
      CHECK(application->view_state().effective_language ==
            avb::i18n::Language::English);
    }

    for (const bool throw_query : {false, true}) {
      Harness harness;
      std::size_t query_count{};
      if (throw_query) {
        harness.system_language_query = [&query_count]()
          -> avb::i18n::Language {
          ++query_count;
          throw std::runtime_error{"scripted language query failure"};
        };
      }
      else {
        harness.system_language_query = [&query_count] {
          ++query_count;
          return static_cast<avb::i18n::Language>(255);
        };
      }

      const auto application = harness.make_application();

      CAPTURE(throw_query);
      CHECK(query_count == 1);
      CHECK(application->view_state().effective_language ==
            avb::i18n::Language::English);
      CHECK(application->view_state().status_text == "Idle");
      CHECK_FALSE(application->view_state().application_notice_text);
    }
  }

  SECTION("system language can be reselected without becoming a manual preference")
  {
    Harness harness;
    REQUIRE(harness.config.store().save(harness.config.path(),
                                        avb::app::UserConfig{}));
    harness.config.replace_count = 0;
    auto system_language = avb::i18n::Language::English;
    std::size_t query_count{};
    harness.system_language_query = [&] {
      ++query_count;
      return system_language;
    };
    auto application = harness.make_application();
    REQUIRE(query_count == 1);

    system_language = avb::i18n::Language::Japanese;
    REQUIRE(harness.queue.push(avb::app::SetLanguage{
      avb::i18n::LanguagePreference::System}));
    application->step(now + 1ms);

    CHECK(query_count == 2);
    CHECK(harness.config.replace_count == 0);
    CHECK(application->view_state().language_preference ==
          avb::i18n::LanguagePreference::System);
    CHECK(application->view_state().effective_language ==
          avb::i18n::Language::Japanese);
    CHECK(application->view_state().status_text ==
          avb::i18n::text(avb::i18n::TextId::Idle,
                                avb::i18n::Language::Japanese));
    const auto unchanged = harness.config.store().load(harness.config.path());
    REQUIRE(unchanged);
    CHECK(unchanged->language ==
          avb::i18n::LanguagePreference::System);

    REQUIRE(harness.queue.push(avb::app::SetLanguage{
      avb::i18n::LanguagePreference::TraditionalChinese}));
    application->step(now + 2ms);
    CHECK(query_count == 2);
    CHECK(harness.config.replace_count == 1);
    CHECK(application->view_state().effective_language ==
          avb::i18n::Language::TraditionalChinese);

    Harness notice_harness;
    notice_harness.catalog.results.push_back(std::unexpected{AppError{
      AppErrorCode::SourceCatalogFailure}});
    auto notice_application = notice_harness.make_application();
    check_notice(*notice_application, AppErrorCode::SourceCatalogFailure);
    REQUIRE(notice_harness.queue.push(avb::app::SetLanguage{
      avb::i18n::LanguagePreference::Japanese}));
    notice_application->step(now + 3ms);
    CHECK(notice_application->view_state().effective_language ==
          avb::i18n::Language::Japanese);
    check_notice(*notice_application, AppErrorCode::SourceCatalogFailure);
  }

  SECTION("failed language save preserves a live capture and retries the whole config")
  {
    Harness harness;
    const avb::app::UserConfig stored{
      .hotkey = hotkey(avb::core::HotkeyKey::F10),
      .fps_cap = avb::core::FpsCap::Fps144,
      .border = avb::core::BorderPreference::Borderless,
      .extensions = {.browser_capture_keep_alive = true},
      .language = avb::i18n::LanguagePreference::English,
    };
    REQUIRE(harness.config.store().save(harness.config.path(), stored));
    harness.config.replace_count = 0;
    auto application = harness.make_application();
    const auto probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, probe, now);
    const auto before = application->view_state();
    const auto factory_count = harness.factory.sources.size();
    const auto apply_count = probe->apply_count;
    const auto stop_count = probe->request_stop_count;
    const auto destruction_count = probe->destruction_count;
    const auto tick_count = probe->tick_count;
    const auto enumerate_count = harness.catalog.enumerate_count;
    const auto hotkey_count = harness.hotkey.replacements.size();
    const auto clear_count = harness.hotkey.clear_count;

    harness.config.replace_succeeds = false;
    REQUIRE(harness.queue.push(avb::app::SetLanguage{
      avb::i18n::LanguagePreference::Japanese}));
    application->step(now + 1ms);

    const auto &failed = application->view_state();
    CHECK(failed.language_preference ==
          avb::i18n::LanguagePreference::Japanese);
    CHECK(failed.effective_language == avb::i18n::Language::Japanese);
    CHECK(failed.status_text == avb::i18n::text(
                                  avb::i18n::TextId::Capturing,
                                  avb::i18n::Language::Japanese));
    CHECK(failed.session_id == before.session_id);
    CHECK(failed.settings == before.settings);
    REQUIRE(failed.sources.size() == before.sources.size());
    for (std::size_t index{}; index < failed.sources.size(); ++index) {
      CHECK(failed.sources[index].id == before.sources[index].id);
      CHECK(failed.sources[index].label == before.sources[index].label);
      CHECK(failed.sources[index].selected == before.sources[index].selected);
    }
    CHECK(harness.factory.sources.size() == factory_count);
    CHECK(probe->apply_count == apply_count);
    CHECK(probe->request_stop_count == stop_count);
    CHECK(probe->destruction_count == destruction_count);
    CHECK(probe->tick_count == tick_count + 1);
    CHECK(harness.catalog.enumerate_count == enumerate_count);
    CHECK(harness.hotkey.replacements.size() == hotkey_count);
    CHECK(harness.hotkey.clear_count == clear_count);
    check_notice(*application, AppErrorCode::ConfigWriteFailure);

    harness.config.replace_succeeds = true;
    REQUIRE(harness.queue.push(avb::app::SetLanguage{
      avb::i18n::LanguagePreference::Japanese}));
    application->step(now + 2ms);
    const auto saved = harness.config.store().load(harness.config.path());
    REQUIRE(saved);
    CHECK(*saved == avb::app::UserConfig{
                      .hotkey = stored.hotkey,
                      .fps_cap = stored.fps_cap,
                      .border = stored.border,
                      .extensions = stored.extensions,
                      .language =
                        avb::i18n::LanguagePreference::Japanese});

    application->shutdown();
    probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
    application->step(now + 3ms);
    CHECK(application->exit_disposition() ==
          avb::core::ExitDisposition::Clean);
    CHECK(probe->destruction_count == 1);
  }

  SECTION("language remains session agnostic during replacement and error drain")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      const auto old_probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, old_probe, now);
      const auto old_session_id = application->view_state().session_id;
      const auto old_settings = application->view_state().settings;
      const auto old_source_id = harness.catalog.fallback.front().id;
      old_probe->ticks.push_back(make_tick({avb::app::SourceLost{}}));
      application->step(now + 1ms);

      const auto replacement_probe = std::make_shared<RuntimeProbe>();
      harness.factory.plans.push_back(FactoryPlan{.probe = replacement_probe});
      REQUIRE(harness.queue.push(avb::app::Start{old_session_id}));
      REQUIRE(harness.queue.push(avb::app::SetLanguage{
        avb::i18n::LanguagePreference::Japanese}));
      application->step(now + 2ms);

      CHECK(application->view_state().session_id == old_session_id);
      CHECK(application->view_state().settings == old_settings);
      REQUIRE(application->view_state().sources.size() == 1);
      CHECK(application->view_state().sources.front().id == old_source_id);
      CHECK(application->view_state().sources.front().selected);
      CHECK(application->view_state().effective_language ==
            avb::i18n::Language::Japanese);
      CHECK(harness.factory.sources.size() == 1);
      CHECK(old_probe->request_stop_count == 1);
      CHECK(old_probe->destruction_count == 0);

      old_probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 3ms);
      CHECK(application->view_state().session_id != old_session_id);
      CHECK(application->view_state().settings == old_settings);
      CHECK(application->view_state().effective_language ==
            avb::i18n::Language::Japanese);
      CHECK(harness.factory.sources.size() == 2);
      CHECK(harness.factory.sources.back().id == old_source_id);
      CHECK(old_probe->destruction_count == 1);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      probe->ticks.push_back(make_tick({avb::app::Failed{
        AppError{AppErrorCode::RuntimeStateLost}}}));
      application->step(now + 1ms);
      const auto apply_count = probe->apply_count;
      const auto stop_count = probe->request_stop_count;

      REQUIRE(harness.queue.push(avb::app::SetLanguage{
        avb::i18n::LanguagePreference::TraditionalChinese}));
      application->step(now + 2ms);

      CHECK(application->view_state().effective_language ==
            avb::i18n::Language::TraditionalChinese);
      CHECK(application->view_state().status_text ==
            avb::i18n::text(avb::i18n::TextId::Error,
                                  avb::i18n::Language::TraditionalChinese));
      CHECK(probe->apply_count == apply_count);
      CHECK(probe->request_stop_count == stop_count);
      CHECK(probe->destruction_count == 0);
      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 3ms);
      CHECK(probe->destruction_count == 1);
    }
  }

  SECTION("quit ordering closes language commands and invalid preferences are rejected")
  {
    {
      Harness harness;
      std::size_t query_count{};
      harness.system_language_query = [&query_count] {
        ++query_count;
        return avb::i18n::Language::English;
      };
      auto application = harness.make_application();
      REQUIRE(query_count == 1);
      REQUIRE(harness.queue.push(avb::app::Quit{}));
      REQUIRE(harness.queue.push(avb::app::SetLanguage{
        avb::i18n::LanguagePreference::System}));
      application->step(now + 1ms);
      CHECK(query_count == 1);
      CHECK(harness.config.replace_count == 0);
      CHECK(application->view_state().language_preference ==
            avb::i18n::LanguagePreference::System);
      CHECK_FALSE(harness.queue.push(avb::app::SetLanguage{
        avb::i18n::LanguagePreference::Japanese}));
    }

    {
      Harness harness;
      std::size_t query_count{};
      harness.system_language_query = [&query_count] {
        ++query_count;
        return avb::i18n::Language::Japanese;
      };
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto before = application->view_state();
      const auto apply_count = probe->apply_count;
      const auto stop_count = probe->request_stop_count;
      const auto saves = harness.config.replace_count;
      const auto queries = query_count;

      REQUIRE(harness.queue.push(avb::app::SetLanguage{
        static_cast<avb::i18n::LanguagePreference>(255)}));
      application->step(now + 1ms);

      CHECK(application->view_state().language_preference ==
            before.language_preference);
      CHECK(application->view_state().effective_language ==
            before.effective_language);
      CHECK(application->view_state().settings == before.settings);
      CHECK(query_count == queries);
      CHECK(harness.config.replace_count == saves);
      CHECK(probe->apply_count == apply_count);
      CHECK(probe->request_stop_count == stop_count);
      check_notice(*application, AppErrorCode::InvalidValue);
    }
  }

  SECTION("direct shutdown localizes a dirty config notice without another save")
  {
    Harness harness;
    auto stored = avb::app::UserConfig{};
    stored.language = avb::i18n::LanguagePreference::English;
    REQUIRE(harness.config.store().save(harness.config.path(), stored));
    harness.config.replace_count = 0;
    auto application = harness.make_application();
    harness.config.replace_succeeds = false;
    REQUIRE(harness.queue.push(avb::app::SetLanguage{
      avb::i18n::LanguagePreference::Japanese}));
    application->step(now + 1ms);
    REQUIRE(harness.config.replace_count == 1);
    CHECK(application->view_state().effective_language ==
          avb::i18n::Language::Japanese);

    application->shutdown();

    CHECK(harness.config.replace_count == 1);
    check_notice(*application, AppErrorCode::ConfigWriteFailure);
    CHECK_FALSE(application->view_state().controls.language.enabled);
    CHECK(application->view_state().controls.language.explanation ==
          avb::i18n::text(
            avb::i18n::TextId::LanguageChangesClosed,
            avb::i18n::Language::Japanese));
  }

  SECTION("runtime feedback and lifecycle arbitration")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);

      probe->ticks.push_back(make_tick_at({
        {6,
         avb::app::BridgeModeChanged{
           .mode = avb::core::BridgeMode::CpuCopy,
           .reason = avb::core::BridgeReason::InteropOperationFailure}},
        {3, avb::app::OverlayCaptureExclusionChanged{
              CaptureExclusionStatus::Failed}},
        {1, avb::app::RuntimeDrainStatusChanged{avb::core::RuntimeDrainStatus::Stalled}},
      }));
      application->step(now + 1ms);
      REQUIRE(application->view_state().bridge.has_value());
      CHECK(application->view_state().bridge->reason ==
            avb::core::BridgeReason::InteropOperationFailure);
      REQUIRE(application->view_state().overlay_exclusion_warning.has_value());
      REQUIRE(application->view_state().drain_warning.has_value());

      probe->ticks.push_back(make_tick_at({
        {5, avb::app::BridgeModeChanged{
              .mode = avb::core::BridgeMode::GpuInterop,
              .reason = avb::core::BridgeReason::None}},
        {2, avb::app::OverlayCaptureExclusionChanged{CaptureExclusionStatus::Excluded}},
        {7, avb::app::RuntimeDrainStatusChanged{avb::core::RuntimeDrainStatus::Normal}},
      }));
      application->step(now + 2ms);
      REQUIRE(application->view_state().bridge.has_value());
      CHECK(application->view_state().bridge->mode ==
            avb::core::BridgeMode::GpuInterop);
      CHECK_FALSE(
        application->view_state().overlay_exclusion_warning.has_value());
      CHECK_FALSE(application->view_state().drain_warning.has_value());
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);

      probe->ticks.push_back(
        make_tick({avb::app::GraphicsRecoveryStarted{}}));
      application->step(now + 1ms);
      REQUIRE(application->view_state().status_text == "Recovering graphics");

      probe->ticks.push_back(
        make_tick({avb::app::GraphicsRecovered{}}));
      application->step(now + 2ms);
      REQUIRE(application->view_state().status_text == "Capturing");

      probe->ticks.push_back(
        make_tick({avb::app::GraphicsRecoveryStarted{}}));
      application->step(now + 3ms);
      REQUIRE(application->view_state().status_text == "Recovering graphics");
      probe->ticks.push_back(make_tick({avb::app::SourceLost{}}));
      application->step(now + 4ms);
      CHECK(application->view_state().status_text == "Capture interrupted");

      probe->ticks.push_back(make_tick_at({
        {7, avb::app::GraphicsRecovered{}},
        {2, avb::app::GraphicsRecoveryStarted{}},
      }));
      application->step(now + 5ms);
      CHECK(application->view_state().status_text == "Capture interrupted");
      CHECK(probe->request_stop_count == 0);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      probe->ticks.push_back(make_tick_at({
        {7, avb::app::GraphicsRecovered{}},
        {1, avb::app::GraphicsRecoveryStarted{}},
      }));
      application->step(now + 1ms);
      CHECK(application->view_state().status_text == "Capturing");
    }

    for (const auto mode : {avb::core::OverlayMode::Editable,
                            avb::core::OverlayMode::PassThrough}) {
      CAPTURE(mode);
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;
      REQUIRE(harness.queue.push(avb::app::SetMode{session_id, mode}));
      REQUIRE(harness.queue.push(avb::app::SetBorder{
        session_id, mode == avb::core::OverlayMode::Editable
                      ? avb::core::BorderPreference::Bordered
                      : avb::core::BorderPreference::Borderless}));
      application->step(now + 1ms);
      probe->ticks.push_back(
        make_tick({avb::app::OverlayCloseRequested{}}));
      application->step(now + 2ms);
      CHECK(application->view_state().status_text == "Stopping");
      CHECK(probe->request_stop_count == 1);
    }

    for (const bool reverse_order : {false, true}) {
      CAPTURE(reverse_order);
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      probe->ticks.push_back(reverse_order
                               ? make_tick({avb::app::SourceLost{},
                                            avb::app::OverlayCloseRequested{}})
                               : make_tick({avb::app::OverlayCloseRequested{},
                                            avb::app::SourceLost{}}));
      application->step(now + 1ms);
      CHECK(application->view_state().status_text == "Stopping");
      CHECK(probe->request_stop_count == 1);
    }

    for (const bool failure_last : {false, true}) {
      CAPTURE(failure_last);
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto failure = avb::app::Failed{
        AppError{AppErrorCode::RuntimeStateLost}};
      probe->ticks.push_back(
        failure_last
          ? make_tick({avb::app::SourceLost{},
                       avb::app::OverlayCloseRequested{}, failure})
          : make_tick({failure, avb::app::OverlayCloseRequested{},
                       avb::app::SourceLost{}}));
      application->step(now + 1ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);

      probe->ticks.push_back(make_tick({
        avb::app::BridgeModeChanged{
          .mode = avb::core::BridgeMode::CpuCopy,
          .reason = avb::core::BridgeReason::InteropOperationFailure},
        avb::app::OverlayCaptureExclusionChanged{
          CaptureExclusionStatus::Failed},
        avb::app::RuntimeDrainStatusChanged{
          avb::core::RuntimeDrainStatus::Stalled},
        avb::app::SourceLost{},
        avb::app::OverlayCloseRequested{},
        avb::app::GraphicsRecoveryStarted{},
        avb::app::GraphicsRecovered{},
      }));
      application->step(now + 2ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->request_stop_count == 1);
      REQUIRE(application->view_state().bridge.has_value());
      REQUIRE(application->view_state().overlay_exclusion_warning.has_value());
      REQUIRE(application->view_state().drain_warning.has_value());

      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 3ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->destruction_count == 1);
      CHECK(application->view_state().controls.start.enabled);
      CHECK_FALSE(application->view_state().bridge.has_value());
      CHECK_FALSE(
        application->view_state().overlay_exclusion_warning.has_value());
      CHECK_FALSE(application->view_state().drain_warning.has_value());
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 1ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->destruction_count == 1);
      CHECK(application->view_state().controls.start.enabled);
    }
  }

  SECTION("settings persistence")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      probe->apply_results.push_back(std::unexpected(
        AppError{AppErrorCode::PlatformOperationFailure}));
      REQUIRE(harness.queue.push(avb::app::SetMode{
        session_id, avb::core::OverlayMode::PassThrough}));
      application->step(now + 1ms);

      REQUIRE(probe->apply_count == 1);
      CHECK(probe->applied[0].mode ==
            avb::core::OverlayMode::PassThrough);
      CHECK(application->view_state().settings.mode ==
            avb::core::OverlayMode::Editable);
      CHECK(application->view_state().status_text == "Capturing");
      CHECK(probe->request_stop_count == 0);
      CHECK(probe->destruction_count == 0);
      CHECK(harness.config.replace_count == 0);
      check_notice(*application, AppErrorCode::PlatformOperationFailure);

      REQUIRE(harness.queue.push(avb::app::SetMode{
        session_id, avb::core::OverlayMode::PassThrough}));
      application->step(now + 2ms);

      REQUIRE(probe->apply_count == 2);
      CHECK(application->view_state().settings.mode ==
            avb::core::OverlayMode::PassThrough);
      CHECK(harness.config.replace_count == 1);
      CHECK_FALSE(application->view_state().application_notice_text);
      const auto saved = harness.config.store().load(harness.config.path());
      REQUIRE(saved);
      CHECK(saved->mode == avb::core::OverlayMode::PassThrough);
      for (const auto command : std::array{
             avb::app::SetMode{avb::core::SessionId{0}, avb::core::OverlayMode::Editable},
             avb::app::SetMode{session_id, static_cast<avb::core::OverlayMode>(255)}}) {
        REQUIRE(harness.queue.push(command));
        application->step(now + 3ms);
        CHECK(probe->apply_count == 2);
        CHECK(harness.config.replace_count == 1);
        CHECK(harness.config.store().load(harness.config.path()) == saved);
      }
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      probe->apply_results.push_back(
        std::unexpected(AppError{AppErrorCode::RuntimeStateLost}));
      REQUIRE(harness.queue.push(avb::app::SetBorder{
        session_id, avb::core::BorderPreference::Borderless}));
      application->step(now + 1ms);

      REQUIRE(probe->apply_count == 1);
      CHECK(probe->applied[0].border ==
            avb::core::BorderPreference::Borderless);
      CHECK(application->view_state().settings.border ==
            avb::core::BorderPreference::Bordered);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);
      CHECK(harness.config.replace_count == 0);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      REQUIRE(harness.queue.push(avb::app::SetMode{
        session_id, avb::core::OverlayMode::PassThrough}));
      REQUIRE(harness.queue.push(avb::app::SetFps{
        session_id, avb::core::FpsCap::Fps144}));
      REQUIRE(harness.queue.push(avb::app::SetBorder{
        session_id, avb::core::BorderPreference::Borderless}));
      application->step(now + 1ms);

      REQUIRE(probe->apply_count == 3);
      CHECK(probe->applied[0] == avb::core::ReplicaSettings{
                                   .mode = avb::core::OverlayMode::PassThrough,
                                   .border = avb::core::BorderPreference::Bordered,
                                   .fps_cap = avb::core::FpsCap::Fps60});
      CHECK(probe->applied[1] == avb::core::ReplicaSettings{
                                   .mode = avb::core::OverlayMode::PassThrough,
                                   .border = avb::core::BorderPreference::Bordered,
                                   .fps_cap = avb::core::FpsCap::Fps144});
      CHECK(probe->applied[2] == avb::core::ReplicaSettings{
                                   .mode = avb::core::OverlayMode::PassThrough,
                                   .border = avb::core::BorderPreference::Borderless,
                                   .fps_cap = avb::core::FpsCap::Fps144});
      CHECK(application->view_state().settings == probe->applied[2]);
      CHECK(harness.config.replace_count == 3);

      const auto saved = harness.config.store().load(harness.config.path());
      REQUIRE(saved);
      CHECK(saved->hotkey == avb::app::UserConfig{}.hotkey);
      CHECK(saved->fps_cap == avb::core::FpsCap::Fps144);
      CHECK(saved->border ==
            avb::core::BorderPreference::Borderless);
    }
  }

  SECTION("browser keep-alive saves while idle and initializes the next session")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto session_id = application->view_state().session_id;

    REQUIRE(harness.queue.push(avb::app::SetExtensions{
      session_id, {.browser_capture_keep_alive = true}}));
    application->step(now);

    CHECK(application->view_state()
            .settings.extensions.browser_capture_keep_alive);
    CHECK(harness.config.replace_count == 1);
    const auto saved = harness.config.store().load(harness.config.path());
    REQUIRE(saved);
    CHECK(saved->extensions.browser_capture_keep_alive);

    application.reset();
    application = harness.make_application();
    CHECK(application->view_state()
            .settings.extensions.browser_capture_keep_alive);
    CHECK(harness.config.replace_count == 1);
  }

  SECTION("browser keep-alive live apply commits only verified runtime acceptance")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;
      const auto saves = harness.config.replace_count;

      probe->apply_results.push_back(std::unexpected(
        AppError{AppErrorCode::PlatformOperationFailure}));
      REQUIRE(harness.queue.push(avb::app::SetExtensions{
        session_id, {.browser_capture_keep_alive = true}}));
      application->step(now + 1ms);

      REQUIRE(probe->apply_count == 1);
      CHECK(probe->applied[0].extensions.browser_capture_keep_alive);
      CHECK_FALSE(application->view_state()
                    .settings.extensions.browser_capture_keep_alive);
      CHECK(harness.config.replace_count == saves);
      CHECK(probe->request_stop_count == 0);
      check_notice(*application, AppErrorCode::PlatformOperationFailure);

      harness.config.replace_succeeds = false;
      REQUIRE(harness.queue.push(avb::app::SetExtensions{
        session_id, {.browser_capture_keep_alive = true}}));
      application->step(now + 2ms);

      REQUIRE(probe->apply_count == 2);
      CHECK(probe->applied[1].extensions.browser_capture_keep_alive);
      CHECK(application->view_state()
              .settings.extensions.browser_capture_keep_alive);
      CHECK(harness.config.replace_count == saves + 1);
      CHECK(probe->request_stop_count == 0);
      check_notice(*application, AppErrorCode::ConfigWriteFailure);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      probe->apply_results.push_back(
        std::unexpected(AppError{AppErrorCode::RuntimeStateLost}));
      REQUIRE(harness.queue.push(avb::app::SetExtensions{
        session_id, {.browser_capture_keep_alive = true}}));
      application->step(now + 1ms);

      REQUIRE(probe->apply_count == 1);
      CHECK(probe->applied[0].extensions.browser_capture_keep_alive);
      CHECK_FALSE(application->view_state()
                    .settings.extensions.browser_capture_keep_alive);
      CHECK(harness.config.replace_count == 0);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);

      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 2ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->destruction_count == 1);
    }
  }

  SECTION("browser keep-alive rejects stale stopping and unsupported commands")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;
      const avb::core::SessionId stale_id{session_id.value + 1};

      REQUIRE(harness.queue.push(avb::app::SetExtensions{
        stale_id, {.browser_capture_keep_alive = true}}));
      application->step(now + 1ms);
      CHECK(probe->apply_count == 0);
      CHECK(harness.config.replace_count == 0);
      check_notice(*application, AppErrorCode::InvalidCommand);

      REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      REQUIRE(harness.queue.push(avb::app::SetExtensions{
        session_id, {.browser_capture_keep_alive = true}}));
      application->step(now + 2ms);
      CHECK(application->view_state().status_text == "Stopping");
      CHECK(probe->apply_count == 0);
      CHECK(probe->request_stop_count == 1);
      CHECK(harness.config.replace_count == 0);
      check_notice(*application, AppErrorCode::InvalidCommand);
    }

    {
      Harness harness;
      auto capabilities = all_capabilities;
      capabilities.browser_capture_keep_alive = false;
      auto application = harness.make_application(std::nullopt, capabilities);
      const auto session_id = application->view_state().session_id;

      REQUIRE(harness.queue.push(avb::app::SetExtensions{
        session_id, {.browser_capture_keep_alive = true}}));
      application->step(now);

      CHECK_FALSE(application->view_state()
                    .settings.extensions.browser_capture_keep_alive);
      CHECK_FALSE(application->view_state()
                    .controls.browser_capture_keep_alive.enabled);
      CHECK(harness.config.replace_count == 0);
      check_notice(*application, AppErrorCode::InvalidCommand);
    }
  }

  SECTION("interrupted replacement preserves accepted browser keep-alive preference")
  {
    Harness harness;
    auto application = harness.make_application();
    const auto old_probe = std::make_shared<RuntimeProbe>();
    select_and_start(harness, *application, old_probe, now);
    const auto old_session_id = application->view_state().session_id;

    REQUIRE(harness.queue.push(avb::app::SetExtensions{
      old_session_id, {.browser_capture_keep_alive = true}}));
    application->step(now + 1ms);
    REQUIRE(application->view_state()
              .settings.extensions.browser_capture_keep_alive);
    const auto saves = harness.config.replace_count;

    old_probe->ticks.push_back(make_tick({avb::app::SourceLost{}}));
    application->step(now + 2ms);
    REQUIRE(application->view_state().status_text == "Capture interrupted");

    const auto replacement_probe = std::make_shared<RuntimeProbe>();
    harness.factory.plans.push_back(
      FactoryPlan{.probe = replacement_probe});
    REQUIRE(harness.queue.push(avb::app::Start{old_session_id}));
    application->step(now + 3ms);
    REQUIRE(application->view_state().status_text == "Stopping");

    old_probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
    application->step(now + 4ms);

    const auto new_session_id = application->view_state().session_id;
    CHECK(new_session_id != old_session_id);
    REQUIRE(harness.factory.settings.size() == 2);
    CHECK(harness.factory.settings.back()
            .extensions.browser_capture_keep_alive);
    CHECK(application->view_state()
            .settings.extensions.browser_capture_keep_alive);

    REQUIRE(harness.queue.push(avb::app::SetExtensions{
      old_session_id, {.browser_capture_keep_alive = false}}));
    application->step(now + 5ms);
    CHECK(replacement_probe->apply_count == 0);
    CHECK(harness.config.replace_count == saves);
    CHECK(application->view_state()
            .settings.extensions.browser_capture_keep_alive);
    check_notice(*application, AppErrorCode::InvalidCommand);
  }

  SECTION("hotkey truth and application notices")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      REQUIRE(harness.queue.push(
        avb::app::RestoreActiveReplicaEditable{}));
      application->step(now);
      CHECK(application->view_state().status_text == "Idle");
      CHECK(application->view_state().settings.mode ==
            avb::core::OverlayMode::Editable);
      CHECK(harness.config.replace_count == 0);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      REQUIRE(harness.queue.push(avb::app::SetMode{
        session_id, avb::core::OverlayMode::PassThrough}));
      application->step(now + 1ms);
      REQUIRE(harness.queue.push(
        avb::app::RestoreActiveReplicaEditable{}));
      application->step(now + 2ms);

      REQUIRE(probe->apply_count == 2);
      CHECK(probe->applied[1].mode ==
            avb::core::OverlayMode::Editable);
      CHECK(application->view_state().settings.mode ==
            avb::core::OverlayMode::Editable);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      REQUIRE(harness.queue.push(avb::app::SetMode{
        session_id, avb::core::OverlayMode::PassThrough}));
      application->step(now + 1ms);
      REQUIRE(probe->apply_count == 1);

      REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      REQUIRE(harness.queue.push(
        avb::app::RestoreActiveReplicaEditable{}));
      application->step(now + 2ms);

      CHECK(application->view_state().status_text == "Stopping");
      CHECK(application->view_state().settings.mode ==
            avb::core::OverlayMode::PassThrough);
      CHECK(probe->apply_count == 1);
      CHECK(probe->request_stop_count == 1);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto session_id = application->view_state().session_id;
      const auto initial_text =
        application->view_state().hotkey_display_text;
      const auto replacement = hotkey(avb::core::HotkeyKey::F9);
      const auto rejected = hotkey(avb::core::HotkeyKey::F10);

      REQUIRE(application->view_state().hotkey_binding_confirmed);
      REQUIRE(harness.queue.push(
        avb::app::ReplaceHotkey{session_id, replacement}));
      application->step(now);

      REQUIRE(application->view_state().hotkey_binding_confirmed);
      const auto replacement_text =
        application->view_state().hotkey_display_text;
      CHECK(replacement_text != initial_text);
      CHECK(harness.config.replace_count == 1);
      CHECK_FALSE(application->view_state().application_notice_text);

      harness.hotkey.results.push_back(
        std::unexpected(AppError{AppErrorCode::HotkeyConflict}));
      REQUIRE(harness.queue.push(
        avb::app::ReplaceHotkey{session_id, rejected}));
      application->step(now + 1ms);
      CHECK(application->view_state().hotkey_binding_confirmed);
      CHECK(application->view_state().hotkey_display_text ==
            replacement_text);
      CHECK(harness.config.replace_count == 1);
      check_notice(*application, AppErrorCode::HotkeyConflict);

      harness.hotkey.results.push_back(
        std::unexpected(AppError{AppErrorCode::HotkeyCleanupFailure}));
      REQUIRE(harness.queue.push(
        avb::app::ReplaceHotkey{session_id, rejected}));
      application->step(now + 2ms);
      CHECK(application->view_state().hotkey_binding_confirmed);
      CHECK(application->view_state().hotkey_display_text ==
            replacement_text);
      check_notice(*application, AppErrorCode::HotkeyCleanupFailure);

      harness.hotkey.results.push_back(
        std::unexpected(AppError{AppErrorCode::HotkeyRegistrationLost}));
      REQUIRE(harness.queue.push(
        avb::app::ReplaceHotkey{session_id, rejected}));
      application->step(now + 3ms);
      CHECK_FALSE(application->view_state().hotkey_binding_confirmed);
      CHECK(application->view_state().hotkey_display_text ==
            replacement_text);
      check_notice(*application, AppErrorCode::HotkeyRegistrationLost);

      harness.hotkey.results.push_back(
        std::unexpected(AppError{AppErrorCode::HotkeyCleanupFailure}));
      REQUIRE(harness.queue.push(
        avb::app::ReplaceHotkey{session_id, rejected}));
      application->step(now + 4ms);
      CHECK_FALSE(application->view_state().hotkey_binding_confirmed);
      CHECK(application->view_state().hotkey_display_text ==
            replacement_text);
      check_notice(*application, AppErrorCode::HotkeyCleanupFailure);

      harness.config.replace_succeeds = false;
      REQUIRE(harness.queue.push(
        avb::app::ReplaceHotkey{session_id, rejected}));
      application->step(now + 5ms);
      REQUIRE(application->view_state().hotkey_binding_confirmed);
      const auto accepted_text = application->view_state().hotkey_display_text;
      CHECK(accepted_text != replacement_text);
      CHECK(harness.config.replace_count == 2);
      check_notice(*application, AppErrorCode::ConfigWriteFailure);

      REQUIRE(harness.queue.push(
        avb::app::RestoreActiveReplicaEditable{}));
      application->step(now + 6ms);
      CHECK(application->view_state().hotkey_display_text == accepted_text);
      check_notice(*application, AppErrorCode::ConfigWriteFailure);

      harness.config.replace_succeeds = true;
      REQUIRE(harness.queue.push(
        avb::app::ReplaceHotkey{session_id, rejected}));
      application->step(now + 7ms);

      CHECK(application->view_state().hotkey_binding_confirmed);
      CHECK(application->view_state().hotkey_display_text == accepted_text);
      CHECK(harness.config.replace_count == 3);
      CHECK_FALSE(application->view_state().application_notice_text);
      const auto saved = harness.config.store().load(harness.config.path());
      REQUIRE(saved);
      CHECK(saved->hotkey == rejected);
    }
  }

  SECTION("stop and shutdown retain ownership until Stopped")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      application->step(now + 1ms);
      CHECK(application->view_state().status_text == "Stopping");
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);

      REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      application->step(now + 2ms);
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);

      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 3ms);
      CHECK(application->view_state().status_text == "Idle");
      CHECK(probe->destruction_count == 1);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      probe->ticks.push_back(make_tick({avb::app::Failed{
        AppError{AppErrorCode::RuntimeStateLost}}}));
      application->step(now + 1ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->request_stop_count == 1);

      REQUIRE(harness.queue.push(avb::app::Stop{session_id}));
      application->step(now + 2ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);

      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 3ms);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->destruction_count == 1);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      harness.config.replace_succeeds = false;
      REQUIRE(harness.queue.push(avb::app::SetFps{
        session_id, avb::core::FpsCap::Fps144}));
      application->step(now + 1ms);
      REQUIRE(harness.config.replace_count == 1);
      check_notice(*application, AppErrorCode::ConfigWriteFailure);

      harness.trace.clear();
      application->shutdown();
      application->shutdown();

      REQUIRE(harness.trace.count == 2);
      CHECK(harness.trace.calls[0] == RecordedCall::HotkeyClear);
      CHECK(harness.trace.calls[1] == RecordedCall::RuntimeStop);
      CHECK(harness.hotkey.clear_count == 1);
      CHECK(probe->request_stop_count == 1);
      CHECK(harness.config.replace_count == 1);
      check_notice(*application, AppErrorCode::ConfigWriteFailure);
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Continue);
      CHECK(probe->destruction_count == 0);

      probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      application->step(now + 2ms);
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Clean);
      CHECK(probe->destruction_count == 1);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);

      probe->ticks.push_back(make_tick(
        {avb::app::RuntimeDrainStatusChanged{
          avb::core::RuntimeDrainStatus::Stalled}}));
      application->step(now + 1ms);
      CHECK(application->view_state().drain_status ==
            avb::core::RuntimeDrainStatus::Stalled);
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Continue);

      application->shutdown();
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::AbandonProcess);
      CHECK(harness.hotkey.clear_count == 1);
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);

      application->shutdown();
      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::Continue);
      probe->ticks.push_back(make_tick(
        {avb::app::RuntimeDrainStatusChanged{
          avb::core::RuntimeDrainStatus::Stalled}}));
      application->step(now + 1ms);

      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::AbandonProcess);
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);
    }
  }

  SECTION("scheduling combines runtime and immediate work")
  {
    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      CHECK_FALSE(application->next_deadline());

      const auto close_time = now + 1ms;
      probe->ticks.push_back(
        make_tick({avb::app::OverlayCloseRequested{}}));
      application->step(close_time);
      CHECK(application->next_deadline() == close_time);
      CHECK(probe->request_stop_count == 1);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      CHECK_FALSE(application->next_deadline());

      const auto failure_time = now + 1ms;
      probe->ticks.push_back(make_tick({avb::app::Failed{
        AppError{AppErrorCode::RuntimeStateLost}}}));
      application->step(failure_time);
      CHECK(application->next_deadline() == failure_time);
      CHECK(application->view_state().status_text == "Error");
      CHECK(probe->request_stop_count == 1);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      CHECK_FALSE(application->next_deadline());

      const auto before_shutdown = Clock::now();
      application->shutdown();
      const auto after_shutdown = Clock::now();
      REQUIRE(application->next_deadline());
      CHECK(*application->next_deadline() >= before_shutdown);
      CHECK(*application->next_deadline() <= after_shutdown);
    }

    {
      Harness harness;
      auto application = harness.make_application();
      const auto old_probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, old_probe, now);
      const auto old_session_id = application->view_state().session_id;

      old_probe->ticks.push_back(
        make_tick({avb::app::SourceLost{}}));
      application->step(now + 1ms);
      REQUIRE(application->view_state().status_text == "Capture interrupted");
      CHECK_FALSE(application->next_deadline());

      const auto replacement_probe = std::make_shared<RuntimeProbe>();
      harness.factory.plans.push_back(
        FactoryPlan{.probe = replacement_probe});
      old_probe->ticks.push_back(make_tick({avb::app::Stopped{}}));
      REQUIRE(
        harness.queue.push(avb::app::Start{old_session_id}));
      const auto replacement_time = now + 2ms;
      application->step(replacement_time);

      CHECK(application->view_state().status_text == "Capturing");
      CHECK(application->view_state().session_id != old_session_id);
      CHECK(harness.factory.sources.size() == 2);
      CHECK(replacement_probe->tick_count == 0);
      CHECK(application->next_deadline() == replacement_time);
    }

    {
      Harness harness;
      auto stored = avb::app::UserConfig{};
      stored.language = avb::i18n::LanguagePreference::Japanese;
      REQUIRE(harness.config.store().save(harness.config.path(), stored));
      harness.config.replace_count = 0;
      std::size_t query_count{};
      harness.system_language_query = [&query_count] {
        ++query_count;
        return avb::i18n::Language::TraditionalChinese;
      };
      auto application = harness.make_application();
      const auto probe = std::make_shared<RuntimeProbe>();
      select_and_start(harness, *application, probe, now);
      const auto session_id = application->view_state().session_id;

      harness.hotkey.throw_on_replace_call = 2;
      REQUIRE(harness.queue.push(avb::app::ReplaceHotkey{
        session_id, hotkey(avb::core::HotkeyKey::F9)}));
      application->step(now + 1ms);

      CHECK(application->exit_disposition() ==
            avb::core::ExitDisposition::AbandonProcess);
      CHECK(harness.hotkey.clear_count == 1);
      CHECK(probe->request_stop_count == 1);
      CHECK(probe->destruction_count == 0);
      CHECK(query_count == 0);
      CHECK(harness.config.replace_count == 0);
      check_notice(*application, AppErrorCode::PlatformOperationFailure);
      CHECK_FALSE(application->view_state().controls.language.enabled);
      CHECK(application->view_state().controls.language.explanation ==
            avb::i18n::text(
              avb::i18n::TextId::LanguageChangesClosed,
              avb::i18n::Language::Japanese));
    }
  }
}
