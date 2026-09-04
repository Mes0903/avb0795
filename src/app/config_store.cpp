#include "airelinna/app/config_store.hpp"

#include "airelinna/core/hotkey.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace airelinna::app {
namespace {

constexpr std::uint32_t config_version{1};

[[nodiscard]] core::AppError read_failure() noexcept {
  return core::AppError{core::AppErrorCode::ConfigReadFailure};
}

[[nodiscard]] core::AppError write_failure() noexcept {
  return core::AppError{core::AppErrorCode::ConfigWriteFailure};
}

[[nodiscard]] std::filesystem::path
temporary_path_for(std::filesystem::path destination) {
  destination += ".tmp";
  return destination;
}

void remove_temporary(const std::filesystem::path &path) noexcept {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

[[nodiscard]] std::optional<core::FpsCap>
parse_fps_cap(std::string_view text) noexcept {
  if (text == "30") {
    return core::FpsCap::Fps30;
  }
  if (text == "60") {
    return core::FpsCap::Fps60;
  }
  if (text == "144") {
    return core::FpsCap::Fps144;
  }
  if (text == "unlimited") {
    return core::FpsCap::Unlimited;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<core::BorderPreference>
parse_border(std::string_view text) noexcept {
  if (text == "bordered") {
    return core::BorderPreference::Bordered;
  }
  if (text == "borderless") {
    return core::BorderPreference::Borderless;
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view serialize_fps_cap(core::FpsCap fps_cap) {
  switch (fps_cap) {
  case core::FpsCap::Fps30:
    return "30";
  case core::FpsCap::Fps60:
    return "60";
  case core::FpsCap::Fps144:
    return "144";
  case core::FpsCap::Unlimited:
    return "unlimited";
  }
  return {};
}

[[nodiscard]] std::string_view serialize_border(core::BorderPreference border) {
  switch (border) {
  case core::BorderPreference::Bordered:
    return "bordered";
  case core::BorderPreference::Borderless:
    return "borderless";
  }
  return {};
}

[[nodiscard]] bool has_exact_keys(const nlohmann::json &document) {
  return document.is_object() && document.size() == 4 &&
         document.contains("version") && document.contains("hotkey") &&
         document.contains("fps_cap") && document.contains("border");
}

[[nodiscard]] std::optional<UserConfig>
parse_config(const nlohmann::json &document) {
  if (!has_exact_keys(document) ||
      !document.at("version").is_number_integer() ||
      !document.at("hotkey").is_string() ||
      !document.at("fps_cap").is_string() ||
      !document.at("border").is_string()) {
    return std::nullopt;
  }

  const auto version = document.at("version").get<std::int64_t>();
  if (version != config_version) {
    return std::nullopt;
  }

  const auto hotkey = core::parse_hotkey_chord(
      document.at("hotkey").get_ref<const std::string &>());
  const auto fps_cap =
      parse_fps_cap(document.at("fps_cap").get_ref<const std::string &>());
  const auto border =
      parse_border(document.at("border").get_ref<const std::string &>());
  if (!hotkey || !fps_cap || !border) {
    return std::nullopt;
  }

  return UserConfig{.version = config_version,
                    .hotkey = *hotkey,
                    .fps_cap = *fps_cap,
                    .border = *border};
}

[[nodiscard]] nlohmann::json serialize_config(const UserConfig &config) {
  return nlohmann::json{{"version", config.version},
                        {"hotkey", core::serialize_hotkey_chord(config.hotkey)},
                        {"fps_cap", serialize_fps_cap(config.fps_cap)},
                        {"border", serialize_border(config.border)}};
}

} // namespace

ConfigStore::ConfigStore(AtomicReplaceFn atomic_replace)
    : atomic_replace_(std::move(atomic_replace)) {}

std::expected<UserConfig, core::AppError>
ConfigStore::load(const std::filesystem::path &path) const {
  std::ifstream input{path, std::ios::binary};
  if (!input.is_open()) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (!error && !exists) {
      return UserConfig{};
    }
    return std::unexpected{read_failure()};
  }

  std::error_code size_error;
  const auto file_size = std::filesystem::file_size(path, size_error);
  if (size_error || file_size > std::numeric_limits<std::size_t>::max() ||
      file_size > static_cast<std::uintmax_t>(
                      std::numeric_limits<std::streamsize>::max())) {
    return std::unexpected{read_failure()};
  }

  std::string contents(static_cast<std::size_t>(file_size), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size()) ||
        input.bad()) {
      return std::unexpected{read_failure()};
    }
  }

  char extra{};
  input.read(&extra, 1);
  if (input.gcount() != 0 || input.bad() || !input.eof()) {
    return std::unexpected{read_failure()};
  }

  const auto document = nlohmann::json::parse(contents, nullptr, false);
  if (document.is_discarded()) {
    return UserConfig{};
  }

  try {
    const auto config = parse_config(document);
    return config.value_or(UserConfig{});
  } catch (const nlohmann::json::exception &) {
    return UserConfig{};
  }
}

std::expected<void, core::AppError>
ConfigStore::save(const std::filesystem::path &path,
                  const UserConfig &config) const {
  if (config.version != config_version) {
    return std::unexpected{core::AppError{core::AppErrorCode::InvalidValue}};
  }

  std::string serialized;
  try {
    serialized = serialize_config(config).dump(2);
    serialized.push_back('\n');
  } catch (...) {
    return std::unexpected{write_failure()};
  }

  const auto temporary_path = temporary_path_for(path);
  std::ofstream output{temporary_path, std::ios::binary | std::ios::trunc};
  if (!output.is_open()) {
    remove_temporary(temporary_path);
    return std::unexpected{write_failure()};
  }

  output << serialized;
  output.flush();
  bool failed = output.fail();
  output.close();
  failed = failed || output.fail();
  if (failed) {
    remove_temporary(temporary_path);
    return std::unexpected{write_failure()};
  }

  bool replaced{};
  try {
    replaced = atomic_replace_(temporary_path, path);
  } catch (...) {
    replaced = false;
  }
  if (!replaced) {
    remove_temporary(temporary_path);
    return std::unexpected{write_failure()};
  }

  return {};
}

} // namespace airelinna::app
