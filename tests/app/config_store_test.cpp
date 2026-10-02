#include "avb/app/config_store.hpp"
#include "avb/i18n/language.hpp"

#ifdef _WIN32
#include "platform/windows/windows_atomic_replace.hpp"
#endif

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

  class TemporaryDirectory final {
  public:
    TemporaryDirectory() : path_(create_unique()) {}

    ~TemporaryDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
      return path_;
    }

  private:
    [[nodiscard]] static std::filesystem::path create_unique()
    {
      constexpr int max_attempts{64};
      const auto parent = std::filesystem::temp_directory_path();
      std::random_device random;
      for (int attempt{}; attempt < max_attempts; ++attempt) {
        const auto candidate =
          parent / ("avb0795-v1-case-04-" + std::to_string(random()) + "-" +
                    std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
          return candidate;
        }
        if (error) {
          throw std::filesystem::filesystem_error{"create temporary directory",
                                                  candidate, error};
        }
      }
      throw std::runtime_error{"could not create a unique temporary directory"};
    }

    std::filesystem::path path_;
  };

  [[nodiscard]] std::expected<void, avb::core::AppError>
  replace_file(const std::filesystem::path &temporary,
               const std::filesystem::path &destination)
  {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
      return std::unexpected{avb::core::AppError{
        avb::core::AppErrorCode::ConfigWriteFailure,
        static_cast<std::int64_t>(error.value())}};
    }
    return {};
  }

  [[nodiscard]] bool write_text(const std::filesystem::path &path,
                                std::string_view text)
  {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << text;
    output.close();
    return output.good();
  }

  [[nodiscard]] std::string read_text(const std::filesystem::path &path)
  {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
  }

  [[nodiscard]] std::filesystem::path
  temporary_path_for(std::filesystem::path destination)
  {
    destination += ".tmp";
    return destination;
  }

}    // namespace

TEST_CASE(
  "avb0795 V1 case 04: config defaults, validation, persistence, and "
  "atomic failure",
  "[avb0795-v1]")
{
  using avb::app::ConfigStore;
  using avb::app::UserConfig;
  using avb::core::AppErrorCode;
  using avb::core::BorderPreference;
  using avb::core::ExtensionSettings;
  using avb::core::FpsCap;
  using avb::core::HotkeyChord;
  using avb::core::HotkeyKey;
  using avb::core::HotkeyModifiers;
  using avb::core::OverlayMode;
  using avb::i18n::LanguagePreference;

  TemporaryDirectory temporary_directory;
  const auto config_path = temporary_directory.path() / "config.json";
  const UserConfig defaults{};

  SECTION("mode and automatic refresh preserve existing V1 preferences")
  {
    const ConfigStore store{replace_file};
    const nlohmann::json valid{
      {"version", 1}, {"hotkey", "Ctrl+Shift+F10"}, {"fps_cap", "144"},
      {"border", "borderless"}, {"language", "ja"}, {"opacity_percent", 25},
      {"extensions", {{"browser_capture_keep_alive", true}}},
    };
    const auto verify = [&](const nlohmann::json &document,
                            OverlayMode mode, bool refresh) {
      const auto serialized = document.dump();
      REQUIRE(write_text(config_path, serialized));
      const auto loaded = store.load(config_path);
      REQUIRE(loaded);
      CHECK(loaded->fps_cap == FpsCap::Fps144);
      CHECK(loaded->hotkey ==
            HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                        HotkeyKey::F10});
      CHECK(loaded->border == BorderPreference::Borderless);
      CHECK(loaded->language == LanguagePreference::Japanese);
      CHECK(loaded->opacity_percent == 25);
      CHECK(loaded->extensions.browser_capture_keep_alive);
      CHECK(loaded->mode == mode);
      CHECK(loaded->auto_refresh_enabled == refresh);
      CHECK(read_text(config_path) == serialized);
    };
    verify(valid, OverlayMode::Editable, false);
    for (const auto &[mode, token] :
         std::array{std::pair{OverlayMode::Editable, "editable"},
                    std::pair{OverlayMode::PassThrough, "pass_through"}}) {
      for (const bool refresh : {false, true}) {
        auto document = valid;
        document["mode"] = token;
        document["auto_refresh_enabled"] = refresh;
        verify(document, mode, refresh);
      }
    }
    const std::array invalid_modes{
      nlohmann::json(nullptr), nlohmann::json(true), nlohmann::json(1),
      nlohmann::json("PassThrough"), nlohmann::json::array(),
      nlohmann::json::object(),
    };
    for (const auto &invalid : invalid_modes) {
      auto document = valid;
      document["mode"] = invalid;
      document["auto_refresh_enabled"] = true;
      verify(document, OverlayMode::Editable, true);
    }
    const std::array invalid_refresh_values{
      nlohmann::json(nullptr), nlohmann::json(1), nlohmann::json(0.0),
      nlohmann::json("true"), nlohmann::json::array(),
      nlohmann::json::object(),
    };
    for (const auto &invalid : invalid_refresh_values) {
      auto document = valid;
      document["mode"] = "pass_through";
      document["auto_refresh_enabled"] = invalid;
      verify(document, OverlayMode::PassThrough, false);
    }
    auto mode_only = valid;
    mode_only["mode"] = "pass_through";
    verify(mode_only, OverlayMode::PassThrough, false);
    auto refresh_only = valid;
    refresh_only["auto_refresh_enabled"] = true;
    verify(refresh_only, OverlayMode::Editable, true);
  }

  SECTION("legacy fallback requires a confirmed missing primary")
  {
    const ConfigStore store{replace_file};
    const auto legacy_path = temporary_directory.path() / "legacy.json";
    auto legacy = defaults;
    legacy.fps_cap = FpsCap::Fps144;
    REQUIRE(store.load(config_path, legacy_path) == defaults);
    REQUIRE(store.save(legacy_path, legacy));
    CHECK(store.load(config_path, legacy_path) == legacy);
    REQUIRE(store.save(config_path, defaults));
    CHECK(store.load(config_path, legacy_path) == defaults);
    REQUIRE(write_text(config_path, "malformed primary"));
    CHECK(store.load(config_path, legacy_path) == defaults);
    CHECK(read_text(config_path) == "malformed primary");
    const auto unreadable = store.load(temporary_directory.path(), legacy_path);
    REQUIRE_FALSE(unreadable);
    CHECK(unreadable.error().code == AppErrorCode::ConfigReadFailure);
  }

  SECTION("legacy load writes nothing and explicit save targets only primary")
  {
    std::size_t replace_count{};
    const ConfigStore store{
      [&](const std::filesystem::path &temporary,
          const std::filesystem::path &destination) {
        ++replace_count;
        return replace_file(temporary, destination);
      }};
    const auto legacy_path = temporary_directory.path() / "legacy.json";
    const std::string original{
      R"({"version":1,"hotkey":"Ctrl+Shift+F10","fps_cap":"144","border":"borderless"})"};
    REQUIRE(write_text(legacy_path, original));
    const auto loaded = store.load(config_path, legacy_path);
    REQUIRE(loaded);
    CHECK(loaded->fps_cap == FpsCap::Fps144);
    CHECK(replace_count == 0);
    CHECK_FALSE(std::filesystem::exists(config_path));
    CHECK_FALSE(std::filesystem::exists(temporary_path_for(config_path)));
    CHECK(read_text(legacy_path) == original);
    REQUIRE(store.save(config_path, *loaded));
    CHECK(replace_count == 1);
    CHECK(store.load(config_path) == loaded);
    CHECK(read_text(legacy_path) == original);
  }

  SECTION("wheel zoom is an optional bounded integer with local default recovery")
  {
    const ConfigStore store{replace_file};
    CHECK(defaults.wheel_zoom_percent == 5);
    for (const int percent : {1, 5, 12, 25}) {
      CAPTURE(percent);
      auto config = defaults;
      config.wheel_zoom_percent = percent;
      REQUIRE(store.save(config_path, config));
      REQUIRE(store.load(config_path) == config);
      const auto emitted = nlohmann::json::parse(read_text(config_path));
      CHECK(emitted.at("wheel_zoom_percent").is_number_integer());
      CHECK(emitted.at("wheel_zoom_percent") == percent);
    }

    nlohmann::json document{
      {"version", 1}, {"hotkey", "Ctrl+Shift+F10"}, {"fps_cap", "144"},
      {"border", "borderless"}, {"language", "ja"}, {"opacity_percent", 75},
      {"extensions", {{"browser_capture_keep_alive", true}}},
    };
    const auto verify_document = [&] {
      const auto serialized = document.dump();
      REQUIRE(write_text(config_path, serialized));
      const auto loaded = store.load(config_path);
      REQUIRE(loaded);
      CHECK(loaded->wheel_zoom_percent == 5);
      CHECK(loaded->fps_cap == FpsCap::Fps144);
      CHECK(loaded->border == BorderPreference::Borderless);
      CHECK(loaded->language == LanguagePreference::Japanese);
      CHECK(loaded->opacity_percent == 75);
      CHECK(loaded->extensions.browser_capture_keep_alive);
      CHECK(loaded->hotkey ==
            HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                        HotkeyKey::F10});
      CHECK(read_text(config_path) == serialized);
    };
    verify_document();
    const std::array invalid_values{
      nlohmann::json(nullptr), nlohmann::json(true), nlohmann::json(12.0),
      nlohmann::json(-1), nlohmann::json(0), nlohmann::json(26),
      nlohmann::json("12"), nlohmann::json::array({12}),
      nlohmann::json{{"percent", 12}},
      nlohmann::json(std::numeric_limits<std::uint64_t>::max()),
      nlohmann::json(std::numeric_limits<std::int64_t>::min()),
    };
    for (const auto &invalid : invalid_values) {
      CAPTURE(invalid.dump());
      document["wheel_zoom_percent"] = invalid;
      verify_document();
    }

    bool replacement_called{};
    const ConfigStore rejecting_store{
      [&](const std::filesystem::path &, const std::filesystem::path &) {
        replacement_called = true;
        return std::expected<void, avb::core::AppError>{};
      }};
    for (const int percent : {0, 26}) {
      auto config = defaults;
      config.wheel_zoom_percent = percent;
      const auto absent_parent = temporary_directory.path() / "wheel-absent";
      const auto result = rejecting_store.save(absent_parent / "config.json", config);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == AppErrorCode::InvalidValue);
      CHECK_FALSE(replacement_called);
      CHECK_FALSE(std::filesystem::exists(absent_parent));
    }
  }

  SECTION("opacity is optional and valid integer percentages round trip")
  {
    const ConfigStore store{replace_file};
    CHECK(defaults.opacity_percent == 100);
    for (const int percent : {0, 1, 50, 99, 100}) {
      CAPTURE(percent);
      auto config = defaults;
      config.opacity_percent = percent;
      REQUIRE(store.save(config_path, config));
      REQUIRE(store.load(config_path) == config);
      const auto emitted = nlohmann::json::parse(read_text(config_path));
      CHECK(emitted.at("opacity_percent").is_number_integer());
      CHECK(emitted.at("opacity_percent") == percent);
    }
  }

  SECTION("invalid opacity recovers locally and preserves valid preferences")
  {
    const ConfigStore store{replace_file};
    const std::array invalid_values{
      nlohmann::json(nullptr), nlohmann::json(true), nlohmann::json(50.0),
      nlohmann::json(-1), nlohmann::json(101), nlohmann::json("50"),
      nlohmann::json(std::numeric_limits<std::uint64_t>::max()),
    };
    nlohmann::json document{
      {"version", 1}, {"hotkey", "Ctrl+Shift+F10"}, {"fps_cap", "144"},
      {"border", "borderless"}, {"language", "ja"},
      {"extensions", {{"browser_capture_keep_alive", true}}},
    };
    auto verify_document = [&] {
      const auto serialized = document.dump();
      REQUIRE(write_text(config_path, serialized));
      const auto loaded = store.load(config_path);
      REQUIRE(loaded);
      CHECK(loaded->opacity_percent == 100);
      CHECK(loaded->fps_cap == FpsCap::Fps144);
      CHECK(loaded->border == BorderPreference::Borderless);
      CHECK(loaded->language == LanguagePreference::Japanese);
      CHECK(loaded->extensions.browser_capture_keep_alive);
      CHECK(loaded->hotkey ==
            HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                        HotkeyKey::F10});
      CHECK(read_text(config_path) == serialized);
    };
    verify_document();
    for (const auto &invalid : invalid_values) {
      CAPTURE(invalid.dump());
      document["opacity_percent"] = invalid;
      verify_document();
    }
  }

  SECTION("invalid opacity save rejects before any filesystem mutation")
  {
    bool replacement_called{};
    const ConfigStore store{
      [&](const std::filesystem::path &, const std::filesystem::path &) {
        replacement_called = true;
        return std::expected<void, avb::core::AppError>{};
      }};
    for (const int percent : {-1, 101}) {
      auto config = defaults;
      config.opacity_percent = percent;
      const auto absent_parent = temporary_directory.path() / "absent";
      const auto result = store.save(absent_parent / "config.json", config);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == AppErrorCode::InvalidValue);
      CHECK_FALSE(replacement_called);
      CHECK_FALSE(std::filesystem::exists(absent_parent));
    }
  }

  SECTION("language and extensions are independently optional")
  {
    struct Row {
      bool has_extensions;
      bool has_language;
    };

    constexpr std::array rows{
      Row{false, false},
      Row{false, true},
      Row{true, false},
      Row{true, true},
    };
    const ConfigStore store{replace_file};

    for (const auto &row : rows) {
      CAPTURE(row.has_extensions, row.has_language);
      nlohmann::json document{
        {"version", 1},
        {"hotkey", "Ctrl+Shift+F10"},
        {"fps_cap", "144"},
        {"border", "borderless"},
      };
      if (row.has_extensions) {
        document["extensions"] = {{"browser_capture_keep_alive", true}};
      }
      if (row.has_language) {
        document["language"] = "ja";
      }
      const auto serialized = document.dump();
      REQUIRE(write_text(config_path, serialized));

      const auto loaded = store.load(config_path);
      REQUIRE(loaded.has_value());
      CHECK(loaded->version == 1);
      CHECK(loaded->hotkey ==
            HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                        HotkeyKey::F10});
      CHECK(loaded->fps_cap == FpsCap::Fps144);
      CHECK(loaded->border == BorderPreference::Borderless);
      CHECK(loaded->extensions.browser_capture_keep_alive ==
            row.has_extensions);
      CHECK(loaded->language == (row.has_language
                                   ? LanguagePreference::Japanese
                                   : LanguagePreference::System));
      CHECK(read_text(config_path) == serialized);
    }
  }

  SECTION("canonical language tokens round trip without resolving the request")
  {
    struct Row {
      LanguagePreference preference;
      std::string_view token;
    };

    constexpr std::array rows{
      Row{LanguagePreference::System, "system"},
      Row{LanguagePreference::English, "en"},
      Row{LanguagePreference::TraditionalChinese, "zh-TW"},
      Row{LanguagePreference::Japanese, "ja"},
    };
    const ConfigStore store{replace_file};

    for (const auto &row : rows) {
      CAPTURE(row.token);
      const UserConfig config{
        .version = 1,
        .hotkey = HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                              HotkeyKey::F12},
        .fps_cap = FpsCap::Fps144,
        .border = BorderPreference::Borderless,
        .extensions = ExtensionSettings{
          .browser_capture_keep_alive = true},
        .language = row.preference,
      };

      REQUIRE(store.save(config_path, config).has_value());
      REQUIRE(store.load(config_path) == config);
      const auto emitted = nlohmann::json::parse(read_text(config_path));
      CHECK(emitted.at("language").get<std::string>() == row.token);
    }
  }

  SECTION("an invalid language alone recovers locally without rewriting")
  {
    const ConfigStore store{replace_file};
    const nlohmann::json valid{
      {"version", 1},
      {"hotkey", "Ctrl+Shift+F10"},
      {"fps_cap", "144"},
      {"border", "borderless"},
      {"extensions", {{"browser_capture_keep_alive", true}}},
    };
    const std::array invalid_languages{
      nlohmann::json(nullptr),
      nlohmann::json(true),
      nlohmann::json(42),
      nlohmann::json::array({"ja"}),
      nlohmann::json{{"requested", "ja"}},
      nlohmann::json("ZH-TW"),
      nlohmann::json("unsupported"),
    };
    const UserConfig expected{
      .version = 1,
      .hotkey = HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                            HotkeyKey::F10},
      .fps_cap = FpsCap::Fps144,
      .border = BorderPreference::Borderless,
      .extensions = ExtensionSettings{
        .browser_capture_keep_alive = true},
      .language = LanguagePreference::System,
    };

    for (const auto &invalid_language : invalid_languages) {
      CAPTURE(invalid_language.dump());
      auto document = valid;
      document["language"] = invalid_language;
      const auto serialized = document.dump();
      REQUIRE(write_text(config_path, serialized));

      const auto loaded = store.load(config_path);
      REQUIRE(loaded.has_value());
      CHECK(*loaded == expected);
      CHECK(read_text(config_path) == serialized);
    }
  }

  SECTION("invalid enum and extra legacy or unknown root fields are rejected")
  {
    bool replacement_called{};
    const ConfigStore store{
      [&](const std::filesystem::path &, const std::filesystem::path &) {
        replacement_called = true;
        return std::expected<void, avb::core::AppError>{};
      }};
    for (const bool invalid_mode : {false, true}) {
      auto invalid_enum = defaults;
      if (invalid_mode) {
        invalid_enum.mode = static_cast<OverlayMode>(255);
      } else {
        invalid_enum.language = static_cast<LanguagePreference>(255);
      }
      const auto absent_parent = temporary_directory.path() / "absent";
      const auto invalid_path = absent_parent / "invalid-enum.json";
      const auto invalid_save = store.save(invalid_path, invalid_enum);
      REQUIRE_FALSE(invalid_save.has_value());
      CHECK(invalid_save.error().code == AppErrorCode::InvalidValue);
      CHECK_FALSE(replacement_called);
      CHECK_FALSE(std::filesystem::exists(absent_parent));
    }

    const nlohmann::json valid{
      {"version", 1},
      {"hotkey", "Ctrl+Shift+F10"},
      {"fps_cap", "144"},
      {"border", "borderless"},
      {"extensions", {{"browser_capture_keep_alive", true}}},
      {"language", "ja"},
    };
    auto invalid_legacy_value = valid;
    invalid_legacy_value["fps_cap"] = "120";
    const auto invalid_legacy_serialized = invalid_legacy_value.dump();
    REQUIRE(write_text(config_path, invalid_legacy_serialized));
    const auto invalid_legacy_load = store.load(config_path);
    REQUIRE(invalid_legacy_load.has_value());
    CHECK(*invalid_legacy_load == defaults);
    CHECK(read_text(config_path) == invalid_legacy_serialized);

    constexpr std::array extra_keys{"locale", "source_id"};
    for (const auto *const extra_key : extra_keys) {
      CAPTURE(extra_key);
      auto document = valid;
      document[extra_key] = "forbidden";
      const auto serialized = document.dump();
      REQUIRE(write_text(config_path, serialized));

      const auto loaded = store.load(config_path);
      REQUIRE(loaded.has_value());
      CHECK(*loaded == defaults);
      CHECK(read_text(config_path) == serialized);
    }
  }

  SECTION("legacy configuration preserves preferences with extensions off")
  {
    const ConfigStore store{replace_file};
    REQUIRE(write_text(
      config_path,
      R"({"version":1,"hotkey":"Ctrl+Shift+F10","fps_cap":"144","border":"borderless"})"));

    const auto loaded = store.load(config_path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->version == 1);
    CHECK(loaded->fps_cap == FpsCap::Fps144);
    CHECK(loaded->border == BorderPreference::Borderless);
    CHECK(loaded->hotkey ==
          HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                      HotkeyKey::F10});
    CHECK_FALSE(loaded->extensions.browser_capture_keep_alive);
  }

  SECTION("extension preference round trips both values and only permitted keys")
  {
    const ConfigStore store{replace_file};

    for (const bool enabled : {false, true}) {
      CAPTURE(enabled);
      const UserConfig config{
        .version = 1,
        .hotkey = HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                              HotkeyKey::F12},
        .fps_cap = FpsCap::Fps144,
        .border = BorderPreference::Borderless,
        .extensions = ExtensionSettings{
          .browser_capture_keep_alive = enabled},
        .mode = enabled ? OverlayMode::PassThrough : OverlayMode::Editable,
        .auto_refresh_enabled = enabled,
      };

      REQUIRE(store.save(config_path, config).has_value());
      REQUIRE(store.load(config_path) == config);

      const auto emitted = nlohmann::json::parse(read_text(config_path));
      std::set<std::string> emitted_keys;
      for (const auto &[key, value] : emitted.items()) {
        static_cast<void>(value);
        emitted_keys.insert(key);
      }
      CHECK(emitted_keys ==
            std::set<std::string>{
              "auto_refresh_enabled", "border", "extensions", "fps_cap", "hotkey",
              "language", "mode", "opacity_percent", "version", "wheel_zoom_percent"});
      CHECK(emitted.at("mode") == (enabled ? "pass_through" : "editable"));
      CHECK(emitted.at("auto_refresh_enabled").is_boolean());
      CHECK(emitted.at("auto_refresh_enabled") == enabled);

      REQUIRE(emitted.at("extensions").is_object());
      std::set<std::string> extension_keys;
      for (const auto &[key, value] : emitted.at("extensions").items()) {
        static_cast<void>(value);
        extension_keys.insert(key);
      }
      CHECK(extension_keys ==
            std::set<std::string>{"browser_capture_keep_alive"});
      CHECK(emitted.at("extensions").at("browser_capture_keep_alive") ==
            enabled);
    }
  }

  SECTION("malformed extension objects recover to complete defaults")
  {
    const ConfigStore store{replace_file};
    const nlohmann::json valid{
      {"version", 1},
      {"hotkey", "Ctrl+Shift+F10"},
      {"fps_cap", "144"},
      {"border", "borderless"},
      {"extensions", {{"browser_capture_keep_alive", true}}},
    };
    const std::array malformed_extensions{
      nlohmann::json(nullptr),
      nlohmann::json(true),
      nlohmann::json::object(),
      nlohmann::json{{"browser_capture_keep_alive", 1}},
      nlohmann::json{{"browser_capture_keep_alive", true},
                     {"source_id", "forbidden"}},
    };
    REQUIRE((malformed_extensions[0].is_null() &&
             malformed_extensions[1].is_boolean()));

    for (const auto &extension : malformed_extensions) {
      CAPTURE(extension.dump());
      auto malformed = valid;
      malformed["extensions"] = extension;
      REQUIRE(write_text(config_path, malformed.dump()));

      const auto loaded = store.load(config_path);
      REQUIRE(loaded.has_value());
      CHECK(*loaded == defaults);
    }
  }

  SECTION(
    "missing and unsupported configuration recover to complete defaults")
  {
    const ConfigStore store{replace_file};

    const auto missing = store.load(config_path);
    REQUIRE(missing.has_value());
    CHECK(*missing == defaults);

    const auto read_failure = store.load(temporary_directory.path());
    REQUIRE_FALSE(read_failure.has_value());
    CHECK(read_failure.error().code == AppErrorCode::ConfigReadFailure);
    CHECK(read_failure.error().retryable());

    REQUIRE(write_text(
      config_path,
      R"({"version":2,"hotkey":"Ctrl+Alt+R","fps_cap":"60","border":"bordered"})"));
    const auto unknown_version = store.load(config_path);
    REQUIRE(unknown_version.has_value());
    CHECK(*unknown_version == defaults);

    REQUIRE(write_text(
      config_path,
      R"({"version":1,"hotkey":"Ctrl+Escape","fps_cap":"144","border":"borderless"})"));
    const auto invalid_hotkey = store.load(config_path);
    REQUIRE(invalid_hotkey.has_value());
    CHECK(*invalid_hotkey == defaults);
  }

  SECTION(
    "round trip emits only the schema and replace failure preserves old "
    "data")
  {
    const ConfigStore store{replace_file};
    const UserConfig nondefault{
      .version = 1,
      .hotkey = HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                            HotkeyKey::F12},
      .fps_cap = FpsCap::Fps144,
      .border = BorderPreference::Borderless,
    };

    const auto saved = store.save(config_path, nondefault);
    REQUIRE(saved.has_value());
    const auto loaded = store.load(config_path);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == nondefault);

    auto unsupported_version = nondefault;
    unsupported_version.version = 2;
    const auto unsupported_version_path = temporary_path_for(config_path);
    const auto unsupported_save =
      store.save(unsupported_version_path, unsupported_version);
    REQUIRE_FALSE(unsupported_save.has_value());
    CHECK(unsupported_save.error().code == AppErrorCode::InvalidValue);
    CHECK_FALSE(unsupported_save.error().retryable());
    CHECK_FALSE(std::filesystem::exists(unsupported_version_path));
    CHECK_FALSE(
      std::filesystem::exists(temporary_path_for(unsupported_version_path)));

    const auto emitted = nlohmann::json::parse(read_text(config_path));
    std::set<std::string> emitted_keys;
    for (const auto &[key, value] : emitted.items()) {
      static_cast<void>(value);
      emitted_keys.insert(key);
    }
    CHECK(emitted_keys ==
          std::set<std::string>{
            "auto_refresh_enabled", "border", "extensions", "fps_cap", "hotkey",
            "language", "mode", "opacity_percent", "version", "wheel_zoom_percent"});
    REQUIRE(emitted.at("extensions").is_object());
    CHECK(emitted.at("extensions").size() == 1);
    CHECK(emitted.at("extensions").contains("browser_capture_keep_alive"));

    const auto failing_path = temporary_directory.path() / "replace-fails.json";
    const auto sibling_temporary = temporary_path_for(failing_path);
    REQUIRE(write_text(failing_path, "old destination"));
    bool replacement_called{};
    const ConfigStore failing_store{
      [&](const std::filesystem::path &temporary,
          const std::filesystem::path &destination) {
        replacement_called = true;
        CHECK(temporary == sibling_temporary);
        CHECK(destination == failing_path);
        CHECK(std::filesystem::exists(temporary));
        CHECK(read_text(destination) == "old destination");
        return std::unexpected{avb::core::AppError{
          AppErrorCode::PlatformOperationFailure, std::int64_t{1234}}};
      }};

    const auto failed_save = failing_store.save(failing_path, nondefault);
    REQUIRE_FALSE(failed_save.has_value());
    CHECK(failed_save.error().code == AppErrorCode::ConfigWriteFailure);
    CHECK(failed_save.error().native_detail == std::int64_t{1234});
    CHECK(failed_save.error().retryable());
    CHECK(replacement_called);
    CHECK(read_text(failing_path) == "old destination");
    CHECK_FALSE(std::filesystem::exists(sibling_temporary));
  }

#ifdef _WIN32
  SECTION(
    "Windows atomic replacement supports create, replace, and native "
    "failure detail")
  {
    const ConfigStore store{
      avb::platform::windows::windows_atomic_replace};
    const UserConfig nondefault{
      .version = 1,
      .hotkey = HotkeyChord{HotkeyModifiers{.control = true, .shift = true},
                            HotkeyKey::F12},
      .fps_cap = FpsCap::Fps144,
      .border = BorderPreference::Borderless,
    };

    REQUIRE(store.save(config_path, defaults).has_value());
    REQUIRE(store.load(config_path) == defaults);
    const auto replaced = store.save(config_path, nondefault);
    REQUIRE(replaced.has_value());
    REQUIRE(store.load(config_path) == nondefault);

    const auto native_destination =
      temporary_directory.path() / "native-existing.json";
    REQUIRE(write_text(native_destination, "old native destination"));
    const auto native_failure =
      avb::platform::windows::windows_atomic_replace(
        temporary_directory.path() / "missing.tmp", native_destination);
    REQUIRE_FALSE(native_failure.has_value());
    CHECK(native_failure.error().code == AppErrorCode::ConfigWriteFailure);
    REQUIRE(native_failure.error().native_detail.has_value());
    CHECK(*native_failure.error().native_detail != 0);
    CHECK(read_text(native_destination) == "old native destination");
  }
#endif
}
