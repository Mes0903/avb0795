/// @file
/// @brief Installed Windows font loading for the independent controller UI.
/// See the matching header for the caller-facing lifetime and thread contract.

#include "platform/windows/controller_font.hpp"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>

namespace avb::platform::windows {

  std::expected<std::vector<std::byte>, core::AppError> load_controller_font()
  {
    try {
      std::array<wchar_t, 32768> windows_directory{};
      const auto length = GetWindowsDirectoryW(
        windows_directory.data(), static_cast<UINT>(windows_directory.size()));
      if (length == 0 || length >= windows_directory.size()) {
        return std::unexpected{core::AppError{
          core::AppErrorCode::RuntimeConstructionFailure}};
      }
      // Read the installed system font; the application does not redistribute font assets.
      const auto path = std::filesystem::path{
                          windows_directory.data(), windows_directory.data() + length} /
                        L"Fonts" / L"msjh.ttc";
      std::ifstream file{path, std::ios::binary | std::ios::ate};
      const auto size = file.tellg();
      // Bound allocation before converting file length to an unsigned buffer size.
      constexpr std::streamoff maximum_font_bytes{64 * 1024 * 1024};
      if (!file || size <= 0 || size > maximum_font_bytes) {
        return std::unexpected{core::AppError{
          core::AppErrorCode::RuntimeConstructionFailure}};
      }
      // The controller receives owned bytes and need not retain this open file.
      std::vector<std::byte> data(static_cast<std::size_t>(size));
      file.seekg(0);
      if (!file.read(reinterpret_cast<char *>(data.data()),
                     static_cast<std::streamsize>(data.size()))) {
        return std::unexpected{core::AppError{
          core::AppErrorCode::RuntimeConstructionFailure}};
      }
      return data;
    } catch (...) {
      return std::unexpected{core::AppError{
        core::AppErrorCode::RuntimeConstructionFailure}};
    }
  }

}    // namespace avb::platform::windows
