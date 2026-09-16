/// @file
/// @brief Scalar performance collection and exclusive report serialization.
#include "avb/diagnostics/performance_report.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace avb::diagnostics {

  namespace {
    using Json = nlohmann::json;
    constexpr std::size_t sample_capacity{65'536};
    constexpr std::size_t owner_count{2};
    constexpr std::size_t sample_count{3};
    constexpr std::array counter_names{
      "ui_builds", "hidden_syncs", "graphics_only_wakes", "admitted", "submitted",
      "present_retired", "dropped", "retained_redraws", "d3_copy_bytes", "cpu_copy_bytes",
      "cpu_clear_bytes", "vulkan_upload_bytes", "command_buffers_created",
      "command_buffers_destroyed", "fences_created", "fences_destroyed",
      "semaphores_created", "semaphores_destroyed"};
    constexpr std::array owner_names{"controller", "replica"};
    constexpr std::array sample_names{"ui_build", "intake_to_submit",
                                      "present_retirement_interval"};
    constexpr std::array text_names{"gpu", "driver", "os", "bridge", "bridge_reason",
                                    "present_mode", "ui_policy", "requested_bridge"};
    constexpr std::array dimension_names{
      "content_width", "content_height", "allocation_width", "allocation_height",
      "framebuffer_width", "framebuffer_height", "fps_cap"};
    constexpr auto warmup_duration = std::chrono::seconds{10};
    constexpr auto measurement_duration = std::chrono::seconds{60};

    enum class IncompleteReason {
      SampleOverflow,
      CounterOverflow,
      InvalidDuration,
      MetadataTruncated,
      RuntimeChanged,
      MetadataChanged,
      EarlyShutdown,
      ScopeNotStarted,
      DrainIncomplete,
      NotFinished,
      MetadataMissing,
    };
    constexpr std::array reason_names{
      "sample_overflow", "counter_overflow", "invalid_duration", "metadata_truncated",
      "runtime_changed", "metadata_changed", "early_shutdown", "scope_not_started",
      "drain_incomplete", "not_finished", "metadata_missing"};

    /// Fixed owned text keeps caller lifetimes out of the hot observation path.
    struct StoredText {
      std::array<char, 256> bytes{};
      std::size_t length{};
      [[nodiscard]] std::string_view view() const noexcept
      { return {bytes.data(), length}; }
      friend bool operator==(const StoredText &, const StoredText &) = default;
    };

    struct StoredMetadata {
      std::array<StoredText, text_names.size()> text{};
      std::array<std::uint32_t, dimension_names.size()> dimensions{};
      friend bool operator==(const StoredMetadata &, const StoredMetadata &) = default;
    };

    [[nodiscard]] StoredMetadata store_metadata(const PerformanceMetadata &input,
                                                bool &truncated) noexcept
    {
      StoredMetadata result;
      const std::array strings{input.gpu, input.driver, input.os, input.bridge,
                               input.bridge_reason, input.present_mode, input.ui_policy,
                               input.requested_bridge};
      for (std::size_t index{}; index < strings.size(); ++index) {
        auto &target = result.text[index];
        target.length = (std::min) (strings[index].size(), target.bytes.size() - 1);
        std::copy_n(strings[index].begin(), target.length, target.bytes.begin());
        truncated = truncated || target.length != strings[index].size();
      }
      result.dimensions = {input.content_width, input.content_height,
                           input.allocation_width, input.allocation_height, input.framebuffer_width,
                           input.framebuffer_height, input.fps_cap};
      return result;
    }

    void merge_high_water(PerformanceHighWater &target, PerformanceHighWater value) noexcept
    {
      target.command_buffers = (std::max) (target.command_buffers, value.command_buffers);
      target.fences = (std::max) (target.fences, value.fences);
      target.semaphores = (std::max) (target.semaphores, value.semaphores);
      target.transient_bytes = (std::max) (target.transient_bytes, value.transient_bytes);
      target.slots = (std::max) (target.slots, value.slots);
      target.presents = (std::max) (target.presents, value.presents);
    }

    [[nodiscard]] std::string_view drain_name(PerformanceDrainState drain) noexcept
    {
      switch (drain) {
      case PerformanceDrainState::NotStarted: return "not_started";
      case PerformanceDrainState::Complete: return "complete";
      case PerformanceDrainState::Stalled: return "stalled";
      case PerformanceDrainState::Abandoned: return "abandoned";
      }
      return "unknown";
    }
  }    // namespace

  struct PerformanceReport::Impl {
    struct Samples {
      std::array<std::int64_t, sample_capacity> values{};
      std::size_t count{};
      std::uint64_t overflow{};
    };
    struct Owner {
      std::array<std::uint64_t, counter_names.size()> counters{};
      StoredMetadata current_metadata;
      StoredMetadata measured_metadata;
      PerformanceHighWater current_occupancy;
      PerformanceHighWater high_water;
      bool metadata_observed{};
    };

    explicit Impl(PerformanceScope selected_scope) : scope(selected_scope) {}

    PerformanceScope scope;
    std::array<Samples, sample_count> samples{};
    std::array<Owner, owner_count> owners{};
    std::array<bool, reason_names.size()> incomplete{};
    std::optional<TimePoint> epoch;
    std::optional<TimePoint> warmup_start;
    std::optional<TimePoint> stopped_at;
    TimePoint measurement_start{};
    TimePoint measurement_end{};
    TimePoint finished_at{};
    RuntimeTag current_runtime{};
    RuntimeTag measured_runtime{};
    PerformanceDrainState drain{PerformanceDrainState::NotStarted};
    bool measurement_started{};
    bool frozen{};
    bool finished{};
    bool write_attempted{};
    bool saw_measurement_present{};

    void mark(IncompleteReason reason) noexcept
    { incomplete[static_cast<std::size_t>(reason)] = true; }

    /// Snapshot before accepting the first event in the half-open interval.
    void advance(TimePoint now) noexcept
    {
      if (finished) {
        return;
      }
      if (!epoch) {
        epoch = now;
      }
      if (!warmup_start || frozen || now < measurement_start) {
        return;
      }
      if (!measurement_started) {
        measurement_started = true;
        const auto selected = scope == PerformanceScope::Controller ? 0U : 1U;
        if (!owners[selected].metadata_observed) {
          mark(IncompleteReason::MetadataMissing);
        }
        for (auto &owner : owners) {
          owner.measured_metadata = owner.current_metadata;
          owner.high_water = owner.current_occupancy;
        }
      }
      if (now >= measurement_end) {
        frozen = true;
      }
    }

    [[nodiscard]] bool measuring(TimePoint now) const noexcept
    { return measurement_started && !frozen && now >= measurement_start; }

    void start(PerformanceScope event_scope, TimePoint now) noexcept
    {
      advance(now);
      if (scope != event_scope || warmup_start || frozen || finished) {
        return;
      }
      warmup_start = now;
      measurement_start = now + warmup_duration;
      measurement_end = measurement_start + measurement_duration;
      measured_runtime = current_runtime;
    }

    [[nodiscard]] std::int64_t relative(TimePoint time) const noexcept
    { return std::chrono::duration_cast<Duration>(time - *epoch).count(); }

    /// All sorting and JSON allocation is deferred until explicit serialization.
    [[nodiscard]] Json serialize(const std::filesystem::path &path)
    {
      const auto run_id = path.stem().u8string();
      Json report{{"schema_version", 1},
                  {"run_id", std::string{run_id.begin(), run_id.end()}},
                  {"scope", scope == PerformanceScope::Controller ? "controller" : "replica"},
                  {"drain", drain_name(drain)},
                  {"runtime", {{"instance", measured_runtime.instance}, {"generation", measured_runtime.generation}}}};
      auto reasons = Json::array();
      for (std::size_t index{}; index < incomplete.size(); ++index) {
        if (incomplete[index]) {
          reasons.push_back(reason_names[index]);
        }
      }
      report["complete"] = reasons.empty() && finished && frozen;
      report["incomplete_reasons"] = std::move(reasons);
      const auto ended_early = incomplete[static_cast<std::size_t>(IncompleteReason::EarlyShutdown)];
      if (!finished) {
        report["stop_reason"] = "not_finished";
      }
      else if (!warmup_start) {
        report["stop_reason"] = "scope_not_started";
      }
      else {
        report["stop_reason"] = ended_early ? "early_shutdown" : "measurement_complete";
      }
      report["window"] = {
        {"epoch", "first_observation"},
        {"warmup_duration_ns", std::chrono::duration_cast<Duration>(warmup_duration).count()},
        {"measurement_duration_ns", std::chrono::duration_cast<Duration>(measurement_duration).count()},
        {"warmup_start_ns", warmup_start ? Json(relative(*warmup_start)) : Json{}},
        {"measurement_start_ns", warmup_start ? Json(relative(measurement_start)) : Json{}},
        {"measurement_end_ns", warmup_start ? Json(relative(measurement_end)) : Json{}},
        {"stopped_ns", stopped_at ? Json(relative(*stopped_at)) : Json{}},
        {"finished_ns", finished && epoch ? Json(relative(finished_at)) : Json{}}};
      report["samples"] = Json::object();
      for (std::size_t index{}; index < samples.size(); ++index) {
        auto &sample = samples[index];
        std::sort(sample.values.begin(), sample.values.begin() + sample.count);
        // Integer nearest-rank arithmetic is exact for the bounded sample count.
        const auto quantile = [&](std::size_t percentile) -> Json {
          if (sample.count == 0) {
            return nullptr;
          }
          return sample.values[(percentile * sample.count + 99) / 100 - 1];
        };
        report["samples"][sample_names[index]] = {
          {"count", sample.count}, {"capacity", sample_capacity}, {"overflow", sample.overflow}, {"p50_ns", quantile(50)}, {"p95_ns", quantile(95)}};
      }
      report["owners"] = Json::object();
      for (std::size_t index{}; index < owners.size(); ++index) {
        const auto &owner = owners[index];
        auto &output = report["owners"][owner_names[index]];
        output["counters"] = Json::object();
        for (std::size_t counter{}; counter < owner.counters.size(); ++counter) {
          output["counters"][counter_names[counter]] = owner.counters[counter];
        }
        const auto &high = owner.high_water;
        output["high_water"] = {
          {"command_buffers", high.command_buffers}, {"fences", high.fences}, {"semaphores", high.semaphores}, {"transient_bytes", high.transient_bytes ? Json(high.transient_bytes) : Json{}}, {"slots", high.slots}, {"presents", high.presents}};
        const auto &metadata = measurement_started ? owner.measured_metadata : owner.current_metadata;
        output["metadata"] = Json::object();
        for (std::size_t field{}; field < text_names.size(); ++field) {
          output["metadata"][text_names[field]] = metadata.text[field].view();
        }
        for (std::size_t field{}; field < dimension_names.size(); ++field) {
          output["metadata"][dimension_names[field]] = metadata.dimensions[field];
        }
      }
      return report;
    }
  };

  PerformanceReport::PerformanceReport(PerformanceScope scope)
      : impl_(std::make_unique<Impl>(scope)) {}
  PerformanceReport::~PerformanceReport() = default;

  void PerformanceReport::observe_counter(PerformanceOwner owner, PerformanceCounter counter, std::uint64_t value, TimePoint now) noexcept
  {
    impl_->advance(now);
    if (!impl_->measuring(now)) {
      return;
    }
    auto &total = impl_->owners[static_cast<std::size_t>(owner)].counters[static_cast<std::size_t>(counter)];
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (value > maximum - total) {
      total = maximum;
      impl_->mark(IncompleteReason::CounterOverflow);
      return;
    }
    total += value;
  }

  void PerformanceReport::observe_sample(PerformanceSample kind, Duration duration, TimePoint now) noexcept
  {
    impl_->advance(now);
    if (!impl_->measuring(now)) {
      return;
    }
    if (kind == PerformanceSample::PresentRetirementInterval && !impl_->saw_measurement_present) {
      impl_->saw_measurement_present = true;
      return;
    }
    if (duration < Duration::zero()) {
      impl_->mark(IncompleteReason::InvalidDuration);
      return;
    }
    auto &sample = impl_->samples[static_cast<std::size_t>(kind)];
    if (sample.count == sample.values.size()) {
      if (sample.overflow != (std::numeric_limits<std::uint64_t>::max)()) {
        ++sample.overflow;
      }
      impl_->mark(IncompleteReason::SampleOverflow);
      return;
    }
    sample.values[sample.count++] = duration.count();
  }

  void PerformanceReport::observe_runtime(RuntimeTag tag, TimePoint now) noexcept
  {
    impl_->advance(now);
    if (impl_->frozen) {
      return;
    }
    if (impl_->warmup_start && tag != impl_->measured_runtime) {
      impl_->mark(IncompleteReason::RuntimeChanged);
    }
    impl_->current_runtime = tag;
  }

  void PerformanceReport::observe_first_present(TimePoint now) noexcept
  { impl_->start(PerformanceScope::Replica, now); }

  void PerformanceReport::observe_first_ui(TimePoint now) noexcept
  { impl_->start(PerformanceScope::Controller, now); }

  void PerformanceReport::observe_metadata(PerformanceOwner owner,
                                           const PerformanceMetadata &metadata,
                                           TimePoint now) noexcept
  {
    impl_->advance(now);
    if (impl_->frozen) {
      return;
    }
    bool truncated{};
    const auto stored = store_metadata(metadata, truncated);
    if (truncated) {
      impl_->mark(IncompleteReason::MetadataTruncated);
    }
    auto &destination = impl_->owners[static_cast<std::size_t>(owner)];
    if (impl_->measurement_started && destination.measured_metadata != stored) {
      impl_->mark(IncompleteReason::MetadataChanged);
    }
    destination.current_metadata = stored;
    destination.metadata_observed = true;
  }

  void PerformanceReport::observe_high_water(PerformanceOwner owner,
                                             PerformanceHighWater values,
                                             TimePoint now) noexcept
  {
    impl_->advance(now);
    if (impl_->frozen) {
      return;
    }
    auto &destination = impl_->owners[static_cast<std::size_t>(owner)];
    destination.current_occupancy = values;
    if (impl_->measuring(now)) {
      merge_high_water(destination.high_water, values);
    }
  }

  void PerformanceReport::advance(TimePoint now) noexcept
  { impl_->advance(now); }

  void PerformanceReport::observe_stop(PerformanceOwner owner, TimePoint now) noexcept
  {
    impl_->advance(now);
    if (impl_->finished || impl_->stopped_at ||
        (owner == PerformanceOwner::Replica && impl_->scope == PerformanceScope::Controller)) {
      return;
    }
    impl_->stopped_at = now;
    if (!impl_->warmup_start || now < impl_->measurement_end) {
      impl_->mark(IncompleteReason::EarlyShutdown);
      impl_->frozen = true;
    }
  }

  void PerformanceReport::finish(TimePoint now, PerformanceDrainState drain) noexcept
  {
    if (impl_->finished) {
      return;
    }
    impl_->advance(now);
    if (!impl_->warmup_start) {
      impl_->mark(IncompleteReason::ScopeNotStarted);
    }
    if (!impl_->warmup_start || now < impl_->measurement_end) {
      impl_->mark(IncompleteReason::EarlyShutdown);
    }
    if (drain != PerformanceDrainState::Complete) {
      impl_->mark(IncompleteReason::DrainIncomplete);
    }
    impl_->drain = drain;
    impl_->finished_at = now;
    impl_->finished = true;
    impl_->frozen = true;
  }

  std::expected<void, PerformanceWriteError>
  PerformanceReport::write_new(const std::filesystem::path &path)
  {
    if (impl_->write_attempted) {
      return std::unexpected{PerformanceWriteError::AlreadyWritten};
    }
    impl_->write_attempted = true;
    if (!impl_->finished) {
      impl_->mark(IncompleteReason::NotFinished);
    }
    auto stage = PerformanceWriteError::SerializationFailed;
    try {
      // Truncation can split a multi-byte scalar; replacement keeps the bounded
      // report writable while its metadata_truncated reason rejects comparison.
      const auto serialized = impl_->serialize(path).dump(2, ' ', false,
                                                          Json::error_handler_t::replace);
      stage = PerformanceWriteError::OpenFailed;
      std::ofstream output{path, std::ios::out | std::ios::binary | std::ios::noreplace};
      if (!output.is_open()) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
          return std::unexpected{PerformanceWriteError::TargetExists};
        }
        return std::unexpected{stage};
      }
      stage = PerformanceWriteError::WriteFailed;
      output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
      output.put('\n');
      if (!output) {
        return std::unexpected{stage};
      }
      stage = PerformanceWriteError::FlushFailed;
      output.flush();
      if (!output) {
        return std::unexpected{stage};
      }
      stage = PerformanceWriteError::CloseFailed;
      output.close();
      if (!output) {
        return std::unexpected{stage};
      }
      return {};
    } catch (...) {
      return std::unexpected{stage};
    }
  }
}    // namespace avb::diagnostics
