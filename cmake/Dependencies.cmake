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

set(imgui_patch_arguments)
if(WIN32)
  find_package(Git REQUIRED)
  set(imgui_patch_arguments
    PATCH_COMMAND
      "${CMAKE_COMMAND}" -E chdir "${PROJECT_SOURCE_DIR}"
      "${GIT_EXECUTABLE}" apply --verbose --unsafe-paths
      "--directory=<SOURCE_DIR>"
      "${PROJECT_SOURCE_DIR}/third_party/patches/imgui-1.92.9-vulkan-safe-failure.patch"
  )
endif()

FetchContent_Declare(
  imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
  URL_HASH SHA256=af97ed649182c39314320514a672b82008ab462b9293fe23d37b30bfa5d05519
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  ${imgui_patch_arguments}
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(imgui)
unset(imgui_patch_arguments)

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
  file(SHA256
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
    imgui_vulkan_backend_hash
  )
  if(NOT imgui_vulkan_backend_hash STREQUAL
      "435e04fba9de28ff2c88e3cccb883c24ae387d4cea052648b0a0447921027ec0")
    message(FATAL_ERROR
      "Dear ImGui 1.92.9 Vulkan backend is missing Airelinna's safe-failure patch")
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
      "${CMAKE_COMMAND}" -E chdir "${PROJECT_SOURCE_DIR}"
      "${GIT_EXECUTABLE}" apply --verbose --unsafe-paths
      "--directory=<SOURCE_DIR>"
      "${PROJECT_SOURCE_DIR}/third_party/patches/glfw-3.4-win32-no-own-dc.patch"
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(glfw)

  include(${PROJECT_SOURCE_DIR}/cmake/VerifyGlfwPatch.cmake)
  airelinna_verify_glfw_patch("${glfw_SOURCE_DIR}")
endif()

if(AIRELINNA_BUILD_TESTS)
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
