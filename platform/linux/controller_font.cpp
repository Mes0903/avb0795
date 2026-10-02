/// @file
/// @brief Private Fontconfig matching and UTF-8 catalog coverage for controller startup.
#include "platform/linux/controller_font.hpp"
#include "avb/i18n/catalog.hpp"

#include <fontconfig/fontconfig.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

namespace avb::linux_platform {
  namespace {
    /// @brief Decode catalog literals strictly so invalid text cannot silently omit coverage.
    bool append_text(FcCharSet &characters, std::string_view text)
    {
      std::size_t index{};
      while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index++]);
        std::uint32_t codepoint{};
        unsigned trailing{};
        std::uint32_t minimum{};
        if (lead < 0x80)
          codepoint = lead;
        else if (lead >= 0xC2 && lead <= 0xDF) {
          codepoint = lead & 0x1F;
          trailing = 1;
          minimum = 0x80;
        }
        else if (lead >= 0xE0 && lead <= 0xEF) {
          codepoint = lead & 0x0F;
          trailing = 2;
          minimum = 0x800;
        }
        else if (lead >= 0xF0 && lead <= 0xF4) {
          codepoint = lead & 0x07;
          trailing = 3;
          minimum = 0x10000;
        }
        else
          return false;
        for (unsigned count{}; count < trailing; ++count) {
          if (index == text.size()) return false;
          const auto byte = static_cast<unsigned char>(text[index++]);
          if ((byte & 0xC0) != 0x80) return false;
          codepoint = (codepoint << 6) | (byte & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
        if (codepoint >= 0x20 && codepoint != 0x7F && !FcCharSetAddChar(&characters, codepoint)) return false;
      }
      return true;
    }
  }    // namespace
  std::expected<desktop::ControllerFontData, core::AppError> load_controller_font()
  {
    const auto failure = [] {
      return std::unexpected{core::AppError{core::AppErrorCode::RuntimeConstructionFailure}};
    };
    try {
      // A private config lifetime avoids retaining registry state in the host and
      // lets startup fail cleanly before any presentation resources exist.
      const std::unique_ptr<FcConfig, decltype(&FcConfigDestroy)> config{FcInitLoadConfigAndFonts(), FcConfigDestroy};
      const std::unique_ptr<FcCharSet, decltype(&FcCharSetDestroy)> required{FcCharSetCreate(), FcCharSetDestroy};
      const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> request{FcPatternCreate(), FcPatternDestroy};
      if (!config || !required || !request) return failure();
      for (const auto language : {i18n::Language::English, i18n::Language::TraditionalChinese, i18n::Language::Japanese}) {
        for (std::size_t index{}; index < i18n::text_count; ++index) {
          if (!append_text(*required, i18n::text(static_cast<i18n::TextId>(index), language))) return failure();
        }
      }
      if (!FcPatternAddString(request.get(), FC_FAMILY, reinterpret_cast<const FcChar8 *>("Noto Sans CJK TC")) ||
          !FcPatternAddInteger(request.get(), FC_WEIGHT, FC_WEIGHT_REGULAR) ||
          !FcPatternAddCharSet(request.get(), FC_CHARSET, required.get()) ||
          !FcConfigSubstitute(config.get(), request.get(), FcMatchPattern)) return failure();
      FcDefaultSubstitute(request.get());
      FcResult result{};
      const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> match{FcFontMatch(config.get(), request.get(), &result), FcPatternDestroy};
      if (!match || result != FcResultMatch) return failure();
      FcChar8 *filename{};
      FcCharSet *actual{};
      int face{};
      if (FcPatternGetString(match.get(), FC_FILE, 0, &filename) != FcResultMatch ||
          FcPatternGetInteger(match.get(), FC_INDEX, 0, &face) != FcResultMatch || face < 0 ||
          FcPatternGetCharSet(match.get(), FC_CHARSET, 0, &actual) != FcResultMatch ||
          !FcCharSetIsSubset(required.get(), actual)) return failure();
      // Family preference can yield a fallback: verify its actual coverage and
      // retain its exact index instead of assuming the first face in a collection.
      std::ifstream file{std::filesystem::path{reinterpret_cast<const char *>(filename)}, std::ios::binary | std::ios::ate};
      const auto size = file.tellg();
      constexpr std::streamoff maximum_font_bytes{64 * 1024 * 1024};
      if (!file || size <= 0 || size > maximum_font_bytes) return failure();
      desktop::ControllerFontData data{std::vector<std::byte>(static_cast<std::size_t>(size)), face};
      file.seekg(0);
      if (!file.read(reinterpret_cast<char *>(data.bytes.data()), static_cast<std::streamsize>(data.bytes.size()))) return failure();
      return data;
    } catch (...) {
      return failure();
    }
  }
}    // namespace avb::linux_platform
