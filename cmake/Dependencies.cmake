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

if(WIN32)
  find_package(Git REQUIRED)
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
