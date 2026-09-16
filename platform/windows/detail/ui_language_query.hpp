/// @file
/// @brief Bounded parsing of the native preferred-language multi-string response.
#ifndef AVB0795_PLATFORM_WINDOWS_DETAIL_UI_LANGUAGE_QUERY_HPP
#define AVB0795_PLATFORM_WINDOWS_DETAIL_UI_LANGUAGE_QUERY_HPP

#include "avb/i18n/language.hpp"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace avb::platform::windows::detail {

  /// @brief Query and select a supported tag without exposing native buffer lifetime.
  ///
  /// Query is invoked synchronously for size and fill; it may borrow the supplied
  /// output buffer only during that call. Size is bounded before allocation and
  /// revalidated after filling, including the terminating double NUL. Unsupported
  /// or non-ASCII tags are skipped in preference order. Query, format, and
  /// allocation failures return English without propagating an exception.
  template <class Query>
  [[nodiscard]] i18n::Language read_ui_language(Query &&query) noexcept
  {
    constexpr auto fallback = i18n::Language::English;
    try {
      ULONG count{};
      ULONG size{};
      if (!query(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size < 2 ||
          size > 32'768) {
        return fallback;
      }

      std::vector<wchar_t> buffer(size);
      // Preferences may change between probe and fill. Trust only a successful
      // response whose reported extent still fits the allocated buffer.
      if (!query(MUI_LANGUAGE_NAME, &count, buffer.data(), &size) || size < 2 ||
          size > buffer.size() || buffer[size - 1] != L'\0' ||
          buffer[size - 2] != L'\0') {
        return fallback;
      }

      auto begin = buffer.cbegin();
      const auto end = begin + size;
      while (begin != end && *begin != L'\0') {
        const auto terminator = std::find(begin, end, L'\0');
        if (terminator == end) {
          return fallback;
        }

        std::string tag;
        bool ascii = true;
        for (auto it = begin; it != terminator; ++it) {
          if (*it > 0x7f) {
            ascii = false;
            break;
          }
          tag.push_back(static_cast<char>(*it));
        }
        if (ascii) {
          if (const auto language = i18n::match_language_tag(tag)) {
            return *language;
          }
        }
        begin = terminator + 1;
      }
    } catch (...) {
      return fallback;
    }
    return fallback;
  }

}    // namespace avb::platform::windows::detail

#endif    // AVB0795_PLATFORM_WINDOWS_DETAIL_UI_LANGUAGE_QUERY_HPP
