# Configure Windows resource ownership and optional distribution targets.
# Inclusion attaches the application resource and generates build metadata;
# producing an archive still requires explicitly invoking the packaging tooling.
# Nothing here signs, uploads, or proves the application's release readiness.
include_guard(GLOBAL)

if(NOT WIN32 OR NOT TARGET avb0795)
  message(FATAL_ERROR "avb0795 packaging requires the Windows product target")
endif()

if(NOT PROJECT_VERSION STREQUAL "1.0.0")
  message(FATAL_ERROR "avb0795 V1 package metadata must use version 1.0.0")
endif()

foreach(static_target glfw imgui)
  get_target_property(target_type "${static_target}" TYPE)
  if(NOT target_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "${static_target} must be statically linked into avb0795")
  endif()
endforeach()

enable_language(RC)
set(avb0795_resource_dir "${PROJECT_SOURCE_DIR}/resources/windows")
set(avb0795_resource_file "${avb0795_resource_dir}/avb0795.rc")
set_property(
  SOURCE "${avb0795_resource_file}"
  TARGET_DIRECTORY avb0795
  PROPERTY OBJECT_DEPENDS
    "${avb0795_resource_dir}/avb0795.ico;${avb0795_resource_dir}/avb0795.manifest"
)
target_sources(avb0795 PRIVATE "${avb0795_resource_file}")

# The resource script embeds the complete application manifest as resource 1.
# Disable the MSVC-compatible linker's generated manifest to avoid two owners
# for that resource ID.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  target_link_options(avb0795 PRIVATE /MANIFEST:NO)
endif()

# Record the actual configured toolchain separately for each configuration.
# SDK discovery uses generator/compiler/environment evidence in that order and
# reports unknown rather than inventing a version when none is available.
set(build_environment_dir "${PROJECT_BINARY_DIR}/package/$<CONFIG>")
set(windows_sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(windows_sdk_version STREQUAL "" AND CMAKE_RC_COMPILER)
  get_filename_component(rc_arch_dir "${CMAKE_RC_COMPILER}" DIRECTORY)
  get_filename_component(rc_version_dir "${rc_arch_dir}" DIRECTORY)
  get_filename_component(rc_sdk_candidate "${rc_version_dir}" NAME)
  if(rc_sdk_candidate MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(windows_sdk_version "${rc_sdk_candidate}")
  endif()
endif()
if(windows_sdk_version STREQUAL "" AND NOT "$ENV{WindowsSDKVersion}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{WindowsSDKVersion}" windows_sdk_version)
  string(REGEX REPLACE "/+$" "" windows_sdk_version "${windows_sdk_version}")
endif()
if(windows_sdk_version STREQUAL "")
  set(windows_sdk_version "unknown")
endif()
file(GENERATE
  OUTPUT "${build_environment_dir}/BUILD_ENVIRONMENT.txt"
  CONTENT
"Product: avb0795 ${PROJECT_VERSION}
Configuration: $<CONFIG>
Target: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION} (${CMAKE_SYSTEM_PROCESSOR})
Generator: ${CMAKE_GENERATOR}
C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}
Windows SDK: ${windows_sdk_version}
CMake: ${CMAKE_VERSION}
Vulkan headers: ${Vulkan_VERSION}
Pinned GLFW: 3.4 (altered source)
Pinned Dear ImGui: 1.92.9 (altered Vulkan backend source)
Pinned nlohmann/json: 3.12.0
")

install(TARGETS avb0795 RUNTIME DESTINATION .)
install(
  FILES
    "${PROJECT_SOURCE_DIR}/SECURITY.md"
    "${PROJECT_SOURCE_DIR}/RELEASE_NOTES.md"
    "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${build_environment_dir}/BUILD_ENVIRONMENT.txt"
  DESTINATION .
)

set(CPACK_GENERATOR ZIP)
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_NAME avb0795)
set(CPACK_PACKAGE_VENDOR "avb0795 contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "A local Windows 11 display and window replica utility")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "avb0795-${PROJECT_VERSION}-windows-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_VERBATIM_VARIABLES YES)

include(CPack)
