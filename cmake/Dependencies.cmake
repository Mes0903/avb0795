# Populate pinned dependency sources and expose only the targets used below.
# Archive hashes constrain downloaded inputs; Windows patch verification also
# checks the populated sources, including local source overrides that can bypass
# FetchContent's download/patch step. This is build setup, not runtime updating.
include(FetchContent)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(nlohmann_json)

# Scalar text and subsequent drag gestures share an ID. The common widgets patch
# ties text export to the active editor on every UI platform. Windows additionally
# uses the Vulkan backend patch to preserve ownership after native failures.
find_package(Git REQUIRED)
# FetchContent archives are source trees rather than Git worktrees. Bound Git's
# discovery above each archive so an enclosing checkout cannot filter patches by
# its current-directory prefix. Patch files and patched sources use LF regardless
# of the developer's global Git line-ending configuration.
set(imgui_patch_files
  "${PROJECT_SOURCE_DIR}/third_party/patches/imgui-1.92.9-active-temp-input.patch")
if(WIN32)
  list(APPEND imgui_patch_files
    "${PROJECT_SOURCE_DIR}/third_party/patches/imgui-1.92.9-vulkan-safe-failure.patch")
endif()

FetchContent_Declare(
  imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
  URL_HASH SHA256=af97ed649182c39314320514a672b82008ab462b9293fe23d37b30bfa5d05519
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND
    "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=<SOURCE_DIR>/.."
    "${GIT_EXECUTABLE}" -c core.autocrlf=false -c core.eol=lf
    -C "<SOURCE_DIR>" apply --verbose ${imgui_patch_files}
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(imgui)
unset(imgui_patch_files)

# Verify offline source overrides against the same post-patch input as downloads.
file(SHA256 "${imgui_SOURCE_DIR}/imgui_widgets.cpp" imgui_widgets_hash)
if(NOT imgui_widgets_hash STREQUAL
    "d36894ba583c4a976a1f227b8721473142aa8ff669d9708173c2c2d25c339e3f")
  message(FATAL_ERROR
    "Dear ImGui 1.92.9 widgets are missing avb0795's active-temp-input patch")
endif()
unset(imgui_widgets_hash)

add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
)

target_include_directories(imgui
  PUBLIC
    $<BUILD_INTERFACE:${imgui_SOURCE_DIR}>
)

if(WIN32)
  # Do not infer patch presence from a successful dependency population step.
  # The expected post-patch file hash also rejects drift in an offline override.
  file(SHA256
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
    imgui_vulkan_backend_hash
  )
  if(NOT imgui_vulkan_backend_hash STREQUAL
      "435e04fba9de28ff2c88e3cccb883c24ae387d4cea052648b0a0447921027ec0")
    message(FATAL_ERROR
      "Dear ImGui 1.92.9 Vulkan backend is missing avb0795's safe-failure patch")
  endif()
  unset(imgui_vulkan_backend_hash)

  find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

  if(NOT TARGET Vulkan::glslc)
    message(FATAL_ERROR
      "Vulkan was found without the required Vulkan::glslc imported target")
  endif()

  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
  FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND
      "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=<SOURCE_DIR>/.."
      "${GIT_EXECUTABLE}" -c core.autocrlf=false -c core.eol=lf
      -C "<SOURCE_DIR>" apply --verbose
      "${PROJECT_SOURCE_DIR}/third_party/patches/glfw-3.4-win32-no-own-dc.patch"
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(glfw)

  include(${PROJECT_SOURCE_DIR}/cmake/VerifyGlfwPatch.cmake)
  avb0795_verify_glfw_patch("${glfw_SOURCE_DIR}")
endif()

# Test-only dependencies are absent when tests are disabled. Lua inventory
# validation remains a separately invoked developer utility, not a build input.
if(AVB0795_BUILD_TESTS)
  set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "" FORCE)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(Catch2)
endif()
