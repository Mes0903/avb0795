# Two complementary architecture checks share this file: configure-time target
# source/link allowlists and a standalone textual source scan. The scan is not a
# C++ parser: comments and inactive preprocessor branches are examined too. Keep
# cross-layer explanations in role terms rather than weakening the rules for prose.
include_guard(GLOBAL)

set(_AVB0795_BOUNDARY_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(_avb0795_boundary_failure detail)
  message(FATAL_ERROR "avb0795 architecture boundary violation: ${detail}")
endfunction()

# Unwrap only the generator-expression forms admitted by current target wiring.
# This is not a general generator-expression evaluator; other forms retain their
# spelling and must pass the explicit allowlist instead of hiding dependencies.
function(_avb0795_normalize_link output link)
  set(value "${link}")
  if(value MATCHES "^\\$<LINK_ONLY:(.*)>$")
    set(value "${CMAKE_MATCH_1}")
  elseif(value MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
    set(value "${CMAKE_MATCH_1}")
  endif()
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_avb0795_assert_links target)
  set(allowed ${ARGN})
  foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(links "${target}" "${property}")
    if(NOT links OR links MATCHES "-NOTFOUND$")
      continue()
    endif()

    foreach(link IN LISTS links)
      _avb0795_normalize_link(normalized "${link}")
      if(normalized IN_LIST allowed)
        continue()
      endif()
      _avb0795_boundary_failure(
        "${target} has disallowed direct link '${normalized}' in ${property}")
    endforeach()
  endforeach()
endfunction()

function(_avb0795_assert_sources target)
  set(allowed_roots ${ARGN})
  get_target_property(source_dir "${target}" SOURCE_DIR)
  get_target_property(sources "${target}" SOURCES)
  if(NOT sources OR sources MATCHES "-NOTFOUND$")
    _avb0795_boundary_failure("${target} has no sources")
  endif()

  foreach(source IN LISTS sources)
    if(source MATCHES "^\\$<")
      _avb0795_boundary_failure(
        "${target} uses an unchecked generator-expression source '${source}'")
    endif()

    if(IS_ABSOLUTE "${source}")
      set(absolute_source "${source}")
    else()
      set(absolute_source "${source_dir}/${source}")
    endif()
    cmake_path(NORMAL_PATH absolute_source)

    set(source_allowed FALSE)
    foreach(root IN LISTS allowed_roots)
      if(root MATCHES "^REGEX:(.*)$")
        if(absolute_source MATCHES "${CMAKE_MATCH_1}")
          set(source_allowed TRUE)
        endif()
      else()
        set(absolute_root "${root}")
        cmake_path(ABSOLUTE_PATH absolute_root
                   BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)
        cmake_path(IS_PREFIX absolute_root "${absolute_source}" NORMALIZE
                   source_allowed)
      endif()
      if(source_allowed)
        break()
      endif()
    endforeach()

    if(NOT source_allowed)
      _avb0795_boundary_failure(
        "${target} owns source outside its layer: ${absolute_source}")
    endif()
  endforeach()
endfunction()

# Follow first-party source directories while excluding the build/dependency tree
# and external source roots. The root invokes this before adding test targets, so
# a new production owner cannot evade review merely by using an unfamiliar name.
function(_avb0795_collect_project_targets output directory)
  get_property(
    directory_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  set(project_targets ${directory_targets})

  get_property(
    subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(subdirectory IN LISTS subdirectories)
    cmake_path(IS_PREFIX PROJECT_BINARY_DIR "${subdirectory}" NORMALIZE
               is_binary_subdirectory)
    if(is_binary_subdirectory)
      continue()
    endif()

    cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${subdirectory}" NORMALIZE
               is_project_subdirectory)
    if(NOT is_project_subdirectory)
      continue()
    endif()

    _avb0795_collect_project_targets(child_targets "${subdirectory}")
    list(APPEND project_targets ${child_targets})
  endforeach()

  set(${output} ${project_targets} PARENT_SCOPE)
endfunction()

function(_avb0795_assert_known_production_targets)
  set(known_targets
      avb0795_diagnostics
      avb0795_i18n
      avb0795_core
      avb0795_app
      avb0795_ui
      avb0795_vulkan_renderer
      avb0795_windows
      avb0795_linux
      avb0795_linux_graphics
      avb0795_linux_cpu_frames
      avb0795_linux_x11_catalog
      avb0795_linux_x11_capture
      avb0795_windows_graphics
      avb0795_desktop
      avb0795
      imgui)
  _avb0795_collect_project_targets(project_targets "${PROJECT_SOURCE_DIR}")
  list(REMOVE_DUPLICATES project_targets)

  foreach(target IN LISTS project_targets)
    if(target IN_LIST known_targets)
      continue()
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "UTILITY")
      continue()
    endif()
    _avb0795_boundary_failure(
      "${target} is a project production target without an explicit layer allowlist")
  endforeach()
endfunction()

function(avb0795_assert_target_boundaries)
  _avb0795_assert_known_production_targets()

  if(TARGET avb0795_diagnostics)
    _avb0795_assert_sources(avb0795_diagnostics "src/diagnostics")
    _avb0795_assert_links(avb0795_diagnostics nlohmann_json::nlohmann_json)
  endif()

  if(TARGET avb0795_i18n)
    _avb0795_assert_sources(avb0795_i18n "src/i18n")
    _avb0795_assert_links(avb0795_i18n)
  endif()

  if(TARGET avb0795_core)
    _avb0795_assert_sources(avb0795_core "src/core")
    _avb0795_assert_links(avb0795_core)
  endif()

  if(TARGET avb0795_app)
    _avb0795_assert_sources(avb0795_app "src/app")
    _avb0795_assert_links(
      avb0795_app
      avb0795_core
      avb0795_i18n
      nlohmann_json::nlohmann_json)
  endif()

  if(TARGET avb0795_ui)
    _avb0795_assert_sources(avb0795_ui "src/ui")
    _avb0795_assert_links(
      avb0795_ui avb0795_core avb0795_i18n imgui)
  endif()

  if(TARGET avb0795_linux_cpu_frames)
    _avb0795_assert_sources(avb0795_linux_cpu_frames
      "platform/linux/linux_captured_frame.cpp" "platform/linux/detail/cpu_frame_pool.cpp")
    _avb0795_assert_links(avb0795_linux_cpu_frames avb0795_core)
  endif()

  if(TARGET avb0795_linux_x11_catalog)
    _avb0795_assert_sources(avb0795_linux_x11_catalog "platform/linux/x11_source_catalog.cpp")
    _avb0795_assert_links(avb0795_linux_x11_catalog avb0795_core PkgConfig::AVB0795_XCB)
  endif()

  if(TARGET avb0795_linux_x11_capture)
    _avb0795_assert_sources(avb0795_linux_x11_capture
      "platform/linux/x11_capture.cpp" "platform/linux/detail/x11_image_policy.cpp")
    _avb0795_assert_links(avb0795_linux_x11_capture
      avb0795_core avb0795_linux_cpu_frames avb0795_linux_x11_catalog
      PkgConfig::AVB0795_XCB Threads::Threads)
  endif()

  if(TARGET avb0795_linux)
    _avb0795_assert_sources(avb0795_linux
      "platform/linux/linux_preferences.cpp"
      "platform/linux/linux_atomic_replace.cpp"
      "platform/linux/linux_ui_language.cpp"
      "platform/linux/controller_font.cpp")
    _avb0795_assert_links(avb0795_linux
      avb0795_core avb0795_i18n PkgConfig::AVB0795_FONTCONFIG)
  endif()

  if(TARGET avb0795_linux_graphics)
    _avb0795_assert_sources(avb0795_linux_graphics "platform/linux/linux_replica_runtime.cpp" "platform/linux/linux_runtime_factory.cpp" "platform/linux/linux_cpu_frame_bridge.cpp" "platform/linux/detail/overlay_policy.cpp" "platform/linux/x11_controller_window.cpp" "platform/linux/x11_overlay_adapter.cpp")
    _avb0795_assert_links(avb0795_linux_graphics avb0795_vulkan_renderer avb0795_linux avb0795_linux_cpu_frames avb0795_linux_x11_capture avb0795_linux_x11_catalog glfw X11::X11 X11::Xext)
  endif()

  if(TARGET avb0795_windows)
    _avb0795_assert_sources(avb0795_windows "platform/windows")
    _avb0795_assert_links(
      avb0795_windows
      avb0795_core
      avb0795_app
      avb0795_i18n
      user32
      comctl32
      d3d11
      dxgi
      windowsapp)
  endif()

  if(TARGET avb0795_vulkan_renderer)
    _avb0795_assert_sources(
      avb0795_vulkan_renderer
      "render"
      "${PROJECT_BINARY_DIR}/generated/avb/render")
    _avb0795_assert_links(
      avb0795_vulkan_renderer avb0795_core avb0795_diagnostics glfw Vulkan::Vulkan)
  endif()

  if(TARGET avb0795_windows_graphics)
    _avb0795_assert_sources(
      avb0795_windows_graphics
      "platform/windows")
    _avb0795_assert_links(
      avb0795_windows_graphics
      avb0795_core
      avb0795_app
      avb0795_windows
      avb0795_vulkan_renderer
      avb0795_diagnostics
      glfw
      Vulkan::Vulkan
      d3d11
      dxgi
      user32)
  endif()

  if(TARGET avb0795_desktop)
    if(NOT DEFINED imgui_SOURCE_DIR)
      _avb0795_boundary_failure(
        "avb0795_desktop requires the pinned imgui source directory")
    endif()
    _avb0795_assert_sources(
      avb0795_desktop
      "src/desktop"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
    _avb0795_assert_links(
      avb0795_desktop
      avb0795_app
      avb0795_ui
      avb0795_diagnostics
      avb0795_vulkan_renderer
      glfw
      Vulkan::Vulkan
      imgui
      imgui_impl_glfw
      imgui_impl_vulkan)
  endif()

  foreach(product_target avb0795)
    if(TARGET "${product_target}")
      get_target_property(product_type "${product_target}" TYPE)
      if(NOT product_type STREQUAL "EXECUTABLE")
        _avb0795_boundary_failure(
          "${product_target} must be the executable composition root")
      endif()
      if(WIN32)
        _avb0795_assert_sources(
          "${product_target}"
          "main.cpp"
          "resources/windows/avb0795.rc"
          "resources/windows/avb0795.manifest")
        _avb0795_assert_links(
          "${product_target}"
          avb0795_app
          avb0795_ui
          avb0795_desktop
          avb0795_windows
          avb0795_windows_graphics
          avb0795_diagnostics
          glfw
          windowsapp)
      elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        _avb0795_assert_sources("${product_target}" "platform/linux/main.cpp")
        _avb0795_assert_links("${product_target}"
          avb0795_app avb0795_desktop avb0795_linux avb0795_linux_graphics avb0795_linux_x11_catalog glfw Vulkan::Vulkan)
      else()
        _avb0795_boundary_failure("${product_target} has no supported platform composition")
      endif()
     endif()
   endforeach()
endfunction()

function(avb0795_add_source_boundary_target)
  if(TARGET verify_source_boundaries)
    _avb0795_boundary_failure(
      "verify_source_boundaries may be registered only once")
  endif()

  add_custom_target(
    verify_source_boundaries
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAVB0795_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
      -P "${_AVB0795_BOUNDARY_SCRIPT}"
    COMMENT "Verifying avb0795 source-layer boundaries"
    VERBATIM)
endfunction()

function(_avb0795_collect_sources output)
  set(files)
  foreach(relative_path IN LISTS ARGN)
    set(path "${AVB0795_SOURCE_ROOT}/${relative_path}")
    if(IS_DIRECTORY "${path}")
      file(
        GLOB_RECURSE found
        LIST_DIRECTORIES FALSE
        "${path}/*.c"
        "${path}/*.cc"
        "${path}/*.cpp"
        "${path}/*.cxx"
        "${path}/*.h"
        "${path}/*.hpp"
        "${path}/*.inc"
        "${path}/*.inl"
        "${path}/*.ipp"
        "${path}/*.tpp")
      list(APPEND files ${found})
    elseif(EXISTS "${path}")
      list(APPEND files "${path}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES files)
  set(${output} "${files}" PARENT_SCOPE)
endfunction()

function(_avb0795_assert_no_match file contents pattern description)
  if("${contents}" MATCHES "${pattern}")
    _avb0795_boundary_failure("${file}: ${description}")
  endif()
endfunction()

function(_avb0795_assert_app_includes file contents)
  set(allowed ${ARGN})
  string(
    REGEX MATCHALL
    "(avb/)?app/[A-Za-z0-9_./-]+\\.(hpp|h)"
    app_includes
    "${contents}")
  foreach(app_include IN LISTS app_includes)
    string(REGEX REPLACE "^app/" "avb/app/" normalized
           "${app_include}")
    if(NOT normalized IN_LIST allowed)
      _avb0795_boundary_failure(
        "${file}: disallowed application include '${normalized}'")
    endif()
  endforeach()
endfunction()

function(_avb0795_assert_layer_includes file contents prefix)
  set(allowed ${ARGN})
  string(
    REGEX MATCHALL
    "(avb/)?${prefix}/[A-Za-z0-9_./-]+\\.(hpp|h)"
    layer_includes
    "${contents}")
  foreach(layer_include IN LISTS layer_includes)
    string(REGEX REPLACE "^avb/" "" normalized "${layer_include}")
    if(NOT normalized IN_LIST allowed)
      _avb0795_boundary_failure(
        "${file}: disallowed ${prefix} include '${normalized}'")
    endif()
  endforeach()
endfunction()

# Apply the category's include and ownership vocabulary constraints to raw text.
# Include paths are normalized separately from case-sensitive C++ symbols. The
# explicit exceptions describe existing seams; absence of a match is not proof of
# runtime ownership correctness, which still requires contracts and code review.
function(_avb0795_scan_files category)
  set(files ${ARGN})
  set(portable_native_include
      "#[ \t]*include[ \t]*[<\"][^>\"]*(windows([./]|\\.h)|win[a-z0-9_]*\\.h[>\"]|winrt/|wrl/|d3d[^>\"]*\\.h|dxgi[^>\"]*\\.h|dxcore\\.h|dwmapi\\.h|shellapi\\.h|shobjidl[^>\"]*\\.h|combaseapi\\.h|roapi\\.h|unknwn\\.h|objbase\\.h|objidl[^>\"]*\\.h|vulkan/|glfw/|imgui_impl_(glfw|vulkan)\\.h|platform/(windows|linux)/)")
  set(portable_native_symbol
      "(^|[^A-Za-z0-9_])(HANDLE|HDC|HINSTANCE|HMONITOR|HWND|IUnknown|IInspectable|ID3D[A-Za-z0-9_]*|IDXGI[A-Za-z0-9_]*|Vk[A-Z][A-Za-z0-9_]*)([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])winrt::")
  set(desktop_forbidden_include
      "#[ \t]*include[ \t]*[<\"][^>\"]*(windows([./]|\\.h)|win[a-z0-9_]*\\.h[>\"]|winrt/|wrl/|d3d[^>\"]*\\.h|dxgi[^>\"]*\\.h|dxcore\\.h|dwmapi\\.h|shellapi\\.h|shobjidl[^>\"]*\\.h|combaseapi\\.h|roapi\\.h|platform/(windows|linux)/)")

  foreach(file IN LISTS files)
    file(READ "${file}" contents)
    string(TOLOWER "${contents}" lower_contents)
    string(REPLACE "\\" "/" normalized_lower_contents "${lower_contents}")

    if(category STREQUAL "diagnostics" OR category STREQUAL "i18n" OR category STREQUAL "core" OR
       category STREQUAL "app" OR category STREQUAL "ui")
      _avb0795_assert_no_match(
        "${file}" "${normalized_lower_contents}" "${portable_native_include}"
        "portable source includes a native graphics/platform header")
      _avb0795_assert_no_match(
        "${file}"
        "${normalized_lower_contents}"
        "#[ \t]*include[ \t]+[^<\" \t\r\n]"
        "portable source uses a non-literal include")
      _avb0795_assert_no_match(
        "${file}" "${contents}" "${portable_native_symbol}"
        "portable source mentions a native graphics/platform type")
    endif()

    if(category STREQUAL "diagnostics" OR category STREQUAL "i18n" OR
       category STREQUAL "core" OR category STREQUAL "app" OR
       category STREQUAL "ui" OR category STREQUAL "desktop" OR category STREQUAL "render")
      _avb0795_assert_layer_includes("${file}" "${contents}" "platform/linux")
      _avb0795_assert_no_match("${file}" "${normalized_lower_contents}"
        "#[ \t]*include[ \t]*[<\"](fontconfig/|gio/|glib[^>\"]*|pipewire/|spa/|x11/|xcb/|sys/(file|stat)\\.h|unistd\\.h|fcntl\\.h)"
        "shared source includes a Linux native header")
      _avb0795_assert_no_match("${file}" "${contents}"
        "(^|[^A-Za-z0-9_])(FcConfig|FcPattern|FcCharSet|XEvent|GDBusConnection|pw_stream|spa_buffer|xcb_[A-Za-z0-9_]+)([^A-Za-z0-9_]|$)"
        "shared source mentions a Linux native contract")
    endif()

    if(category STREQUAL "diagnostics" OR category STREQUAL "i18n" OR category STREQUAL "core" OR
       category STREQUAL "app")
      _avb0795_assert_no_match(
        "${file}"
        "${normalized_lower_contents}"
        "#[ \t]*include[ \t]*[<\"][^>\"]*imgui(_internal|_stdlib)?\\.h"
        "portable source includes a UI header")
    endif()

    if(category STREQUAL "i18n" OR category STREQUAL "core" OR
       category STREQUAL "app" OR category STREQUAL "ui")
      _avb0795_assert_layer_includes("${file}" "${contents}" "diagnostics")
    endif()

    if(category STREQUAL "diagnostics")
      foreach(layer app core ui desktop render platform i18n)
        _avb0795_assert_layer_includes("${file}" "${contents}" "${layer}")
      endforeach()
    elseif(category STREQUAL "i18n")
      _avb0795_assert_layer_includes("${file}" "${contents}" "app")
      _avb0795_assert_layer_includes("${file}" "${contents}" "core")
      _avb0795_assert_layer_includes("${file}" "${contents}" "ui")
      _avb0795_assert_layer_includes("${file}" "${contents}" "desktop")
      _avb0795_assert_layer_includes("${file}" "${contents}" "render")
      _avb0795_assert_layer_includes("${file}" "${contents}" "platform")
      _avb0795_assert_no_match(
        "${file}"
        "${normalized_lower_contents}"
        "#[ \t]*include[ \t]*[<\"](filesystem|fstream)[>\"]"
        "language policy includes filesystem I/O")
    elseif(category STREQUAL "core")
      _avb0795_assert_app_includes("${file}" "${contents}")
      _avb0795_assert_layer_includes("${file}" "${contents}" "i18n")
      _avb0795_assert_layer_includes("${file}" "${contents}" "ui")
      _avb0795_assert_layer_includes("${file}" "${contents}" "desktop")
      _avb0795_assert_layer_includes("${file}" "${contents}" "render")
      _avb0795_assert_layer_includes(
        "${file}" "${contents}" "platform/windows")
    elseif(category STREQUAL "app")
      _avb0795_assert_layer_includes("${file}" "${contents}" "ui")
      _avb0795_assert_layer_includes("${file}" "${contents}" "desktop")
      _avb0795_assert_layer_includes("${file}" "${contents}" "render")
      _avb0795_assert_layer_includes(
        "${file}" "${contents}" "platform/windows")
    elseif(category STREQUAL "ui")
      _avb0795_assert_layer_includes("${file}" "${contents}" "render")
      _avb0795_assert_layer_includes("${file}" "${contents}" "desktop")
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "(avb/)?core/(application_state|replica_session)\\.hpp"
        "UI includes mutable domain state")
      _avb0795_assert_app_includes(
        "${file}"
        "${contents}"
        avb/app/commands.hpp
        avb/app/controller_view_state.hpp)
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "(^|[^A-Za-z0-9_])(Application|ApplicationState|ReplicaSession|IReplicaRuntime|ISourceAcquisition|SourceAcquisitionRequest|IGlobalHotkey|ConfigStore|OverlayWindow|WindowsRenderStack|WindowsFrameBridge|VulkanRenderer)([^A-Za-z0-9_]|$)"
        "UI crosses the frontend command/view-state boundary")
    elseif(category STREQUAL "desktop")
      _avb0795_assert_layer_includes("${file}" "${contents}" "i18n")
      _avb0795_assert_layer_includes(
        "${file}"
        "${contents}"
        "render"
        render/graphics_retry_policy.hpp)
      _avb0795_assert_no_match(
        "${file}"
        "${lower_contents}"
        "${desktop_forbidden_include}"
        "desktop shell includes replica-native/platform ownership")
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "(avb/)?core/(application_state|replica_session)\\.hpp|(^|[^A-Za-z0-9_])(ApplicationState|ReplicaSession)([^A-Za-z0-9_]|$)"
        "desktop shell reaches mutable domain state")
      _avb0795_assert_app_includes(
        "${file}"
        "${contents}"
        avb/app/application.hpp
        avb/app/commands.hpp
        avb/app/controller_view_state.hpp)
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "(^|[^A-Za-z0-9_])(ISourceCatalog|IReplicaRuntimeFactory|IReplicaRuntime|IGlobalHotkey|ConfigStore|OverlayWindow|WindowsRenderStack|WindowsFrameBridge|VulkanRenderer|WgcCaptureJob|WindowsCaptureSource|WindowsCapturedFrame)([^A-Za-z0-9_]|$)"
        "desktop shell reaches a port, replica runtime, config store, or native replica owner")
    elseif(category STREQUAL "linux")
      file(RELATIVE_PATH linux_relative "${AVB0795_SOURCE_ROOT}" "${file}")
      set(allowed_linux_app_headers)
      if(linux_relative MATCHES "^platform/linux/(portal_screencast|linux_source_acquisition|detail/acquisition_policy)\\.(hpp|cpp)$")
        list(APPEND allowed_linux_app_headers avb/app/source_acquisition.hpp)
      endif()
      if(linux_relative MATCHES "^platform/linux/(linux_(disabled_hotkey|source_registry|replica_runtime|runtime_factory)|x11_source_catalog)\\.hpp$")
        list(APPEND allowed_linux_app_headers avb/app/ports.hpp)
      endif()
      _avb0795_assert_app_includes("${file}" "${contents}" ${allowed_linux_app_headers})
      if(linux_relative MATCHES "^platform/linux/x11_(controller_window|overlay_adapter)\\.(hpp|cpp)$")
        _avb0795_assert_no_match("${file}" "${contents}"
          "[.>]source_selection_method[ \t\r\n]*="
          "Linux window interaction adapter assigns source-selection authority")
      endif()
      if(NOT linux_relative STREQUAL "platform/linux/x11_source_catalog.cpp" AND
         NOT linux_relative STREQUAL "platform/linux/x11_capture.cpp")
        _avb0795_assert_no_match("${file}" "${contents}"
          "#[ \t]*include[ \t]*[<\"]xcb/"
          "XCB native protocol headers belong only to the concrete catalog and capture implementations")
      endif()
      if(linux_relative MATCHES "^platform/linux/(detail/(cpu_frame_pool|x11_image_policy)|linux_captured_frame)\\.(hpp|cpp)$")
        _avb0795_assert_no_match("${file}" "${normalized_lower_contents}"
          "#[ \t]*include[ \t]*[<\"](gio/|glib|pipewire/|spa/|xcb/|x11/|glfw/|vulkan/)"
          "CPU frame storage or image layout depends on native protocol or graphics headers")
      endif()
      if(NOT linux_relative MATCHES "^platform/linux/(x11_controller_window|x11_overlay_adapter)\\.cpp$")
        _avb0795_assert_no_match("${file}" "${contents}"
          "#[ \t]*include[ \t]*[<\"](X11/|GLFW/glfw3native\\.h)"
          "Linux native window headers belong only to the concrete X11 implementations")
      endif()

      _avb0795_assert_layer_includes("${file}" "${contents}" "ui")
      set(allowed_linux_render_headers)
      if(linux_relative MATCHES "^platform/linux/linux_(replica_runtime|runtime_factory)\\.(hpp|cpp)$")
        list(APPEND allowed_linux_render_headers render/graphics_retry_policy.hpp render/replica_presentation.hpp render/vulkan_renderer.hpp)
      endif()
      if(linux_relative MATCHES "^platform/linux/linux_cpu_frame_bridge\\.(hpp|cpp)$")
        list(APPEND allowed_linux_render_headers render/vulkan_input.hpp render/vulkan_renderer.hpp)
      endif()
      if(linux_relative MATCHES "^platform/linux/(detail/overlay_policy|x11_overlay_adapter)\\.hpp$")
        list(APPEND allowed_linux_render_headers render/replica_presentation.hpp)
      endif()
      _avb0795_assert_layer_includes("${file}" "${contents}" "render" ${allowed_linux_render_headers})
      _avb0795_assert_layer_includes("${file}" "${contents}" "platform/windows")
      set(allowed_linux_i18n_headers)
      set(allowed_linux_desktop_headers)
      if(linux_relative MATCHES "^platform/linux/linux_ui_language\\.(hpp|cpp)$")
        list(APPEND allowed_linux_i18n_headers i18n/language.hpp)
      elseif(linux_relative MATCHES "^platform/linux/controller_font\\.(hpp|cpp)$")
        list(APPEND allowed_linux_i18n_headers i18n/catalog.hpp)
        list(APPEND allowed_linux_desktop_headers desktop/controller_font_data.hpp)
      endif()
      _avb0795_assert_layer_includes("${file}" "${contents}" "i18n" ${allowed_linux_i18n_headers})
      if(linux_relative MATCHES "^platform/linux/x11_controller_window\\.hpp$")
        list(APPEND allowed_linux_desktop_headers desktop/controller_window_interaction.hpp)
      endif()
      _avb0795_assert_layer_includes("${file}" "${contents}" "desktop" ${allowed_linux_desktop_headers})
      _avb0795_assert_no_match("${file}" "${contents}"
        "(avb/)?core/(application_state|replica_session)|(avb/)?ui/|(^|[^A-Za-z0-9_])(Application|ApplicationState|ReplicaSession|ControllerView|DesktopHost)([^A-Za-z0-9_]|$)"
        "Linux adapter reaches an owner outside its boundary")
    elseif(category STREQUAL "windows")
      get_filename_component(file_name "${file}" NAME)
      set(allowed_windows_app_headers avb/app/ports.hpp)
      set(allowed_windows_i18n_headers)
      set(allowed_windows_desktop_headers)
      file(RELATIVE_PATH windows_relative "${AVB0795_SOURCE_ROOT}" "${file}")
      string(REPLACE "\\" "/" windows_relative "${windows_relative}")
      if(windows_relative MATCHES
         "^platform/windows/windows_controller_window\\.(hpp|cpp)$")
        list(APPEND allowed_windows_desktop_headers
             desktop/controller_window_interaction.hpp)
      endif()
      _avb0795_assert_layer_includes(
        "${file}" "${contents}" "desktop"
        ${allowed_windows_desktop_headers})
      if(windows_relative MATCHES
         "^platform/windows/windows_ui_language\\.(c|cc|cpp|cxx|h|hpp|inc|inl|ipp|tpp)$" OR
         windows_relative STREQUAL
         "platform/windows/detail/ui_language_query.hpp")
        list(APPEND allowed_windows_i18n_headers i18n/language.hpp)
      endif()
      _avb0795_assert_layer_includes(
        "${file}"
        "${contents}"
        "i18n"
        ${allowed_windows_i18n_headers})
      set(is_hotkey_file FALSE)
      if(file_name MATCHES
         "^windows_global_hotkey\\.(c|cc|cpp|cxx|h|hpp)$")
        set(is_hotkey_file TRUE)
        list(APPEND
             allowed_windows_app_headers avb/app/commands.hpp)
      endif()
      _avb0795_assert_app_includes(
        "${file}"
        "${contents}"
        ${allowed_windows_app_headers})
      if(NOT is_hotkey_file)
        _avb0795_assert_no_match(
          "${file}"
          "${contents}"
          "(^|[^A-Za-z0-9_])(AppCommand|RestoreActiveReplicaEditable)([^A-Za-z0-9_]|$)|windows_global_hotkey"
          "non-hotkey Windows adapter reaches the command path")
      endif()
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "(avb/)?core/(application_state|replica_session)|(avb/)?ui/"
        "Windows adapter includes domain state or frontend/desktop code")
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "(^|[^A-Za-z0-9_])(Application|ApplicationState|ReplicaSession|ControllerView|DesktopHost)([^A-Za-z0-9_]|$)"
        "Windows adapter mentions an owner outside its boundary")
    elseif(category STREQUAL "render")
      _avb0795_assert_app_includes("${file}" "${contents}")
      _avb0795_assert_layer_includes("${file}" "${contents}" "i18n")
      _avb0795_assert_layer_includes(
        "${file}" "${contents}" "platform")
      _avb0795_assert_no_match(
        "${file}" "${normalized_lower_contents}" "${desktop_forbidden_include}"
        "shared renderer includes a native capture/platform header")
      _avb0795_assert_no_match(
        "${file}" "${contents}"
        "(^|[^A-Za-z0-9_])(HANDLE|HDC|HINSTANCE|HMONITOR|HWND|HRESULT|IUnknown|IInspectable|ID3D[A-Za-z0-9_]*|IDXGI[A-Za-z0-9_]*|D3D[A-Za-z0-9_]*|DXGI_[A-Za-z0-9_]*|Windows[A-Za-z0-9_]*|Wgc[A-Za-z0-9_]*|DeviceLossKind|BridgeCapabilityFacts|BridgeSelection)([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])winrt::|VK_[A-Z0-9_]*WIN32|[A-Za-z0-9_]*Win32[A-Za-z0-9_]*"
        "shared renderer mentions a platform capture/interop contract")
      _avb0795_assert_no_match(
        "${file}" "${normalized_lower_contents}"
        "#[ \t]*include[ \t]+[^<\" \t\r\n]"
        "shared renderer uses a non-literal include")
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "(avb/)?core/(application_state|replica_session)|(avb/)?(ui|desktop)/"
        "render source includes application/domain/frontend ownership")
    endif()
  endforeach()
endfunction()

# Discover current production files directly, including untracked additions; do
# not rely on an older compilation database or Git index to define this boundary.
# Per-layer rules run first, followed by feature-isolation and removed-contract
# checks that apply across the complete production set.
function(_avb0795_run_source_boundary_scan)
  if(NOT DEFINED AVB0795_SOURCE_ROOT OR
     NOT IS_DIRECTORY "${AVB0795_SOURCE_ROOT}")
    _avb0795_boundary_failure(
      "AVB0795_SOURCE_ROOT must name the project source directory")
  endif()

  _avb0795_collect_sources(
    diagnostics_files "include/avb/diagnostics" "src/diagnostics")
  _avb0795_scan_files(diagnostics ${diagnostics_files})

  _avb0795_collect_sources(
    i18n_files "include/avb/i18n" "src/i18n")
  _avb0795_scan_files(i18n ${i18n_files})

  _avb0795_collect_sources(
    core_files "include/avb/core" "src/core")
  _avb0795_scan_files(core ${core_files})

  _avb0795_collect_sources(
    app_files "include/avb/app" "src/app")
  _avb0795_scan_files(app ${app_files})

  _avb0795_collect_sources(ui_files "include/avb/ui" "src/ui")
  _avb0795_scan_files(ui ${ui_files})

  _avb0795_collect_sources(
    desktop_files "include/avb/desktop" "src/desktop")
  _avb0795_scan_files(desktop ${desktop_files})

  _avb0795_collect_sources(windows_files "platform/windows")
  _avb0795_scan_files(windows ${windows_files})

  _avb0795_collect_sources(linux_files "platform/linux")
  # Only this exact file owns Linux composition. All other adapters retain their
  # narrow include/owner rules, including registry, acquisition and concrete runtime owners.
  list(REMOVE_ITEM linux_files "${AVB0795_SOURCE_ROOT}/platform/linux/main.cpp")
  _avb0795_scan_files(linux ${linux_files})

  _avb0795_collect_sources(render_files "render")
  _avb0795_scan_files(render ${render_files})

  _avb0795_collect_sources(main_files "main.cpp" "platform/linux/main.cpp")
  foreach(file IN LISTS main_files)
    file(READ "${file}" contents)
    if(file STREQUAL "${AVB0795_SOURCE_ROOT}/platform/linux/main.cpp")
      _avb0795_assert_app_includes("${file}" "${contents}"
        avb/app/app_command_queue.hpp avb/app/application.hpp avb/app/config_store.hpp)
      _avb0795_assert_layer_includes("${file}" "${contents}" "core" core/types.hpp)
      _avb0795_assert_layer_includes("${file}" "${contents}" "desktop" desktop/desktop_host.hpp)
      _avb0795_assert_layer_includes("${file}" "${contents}" "ui")
      _avb0795_assert_layer_includes("${file}" "${contents}" "render")
      _avb0795_assert_layer_includes("${file}" "${contents}" "platform/windows")
      _avb0795_assert_layer_includes("${file}" "${contents}" "platform/linux"
        platform/linux/controller_font.hpp platform/linux/linux_atomic_replace.hpp
        platform/linux/linux_disabled_hotkey.hpp platform/linux/linux_preferences.hpp
        platform/linux/linux_ui_language.hpp platform/linux/x11_source_catalog.hpp
        platform/linux/linux_runtime_factory.hpp
        platform/linux/x11_controller_window.hpp platform/linux/x11_overlay_adapter.hpp)
      _avb0795_assert_no_match("${file}" "${contents}"
        "#[ \t]*include[ \t]*[<\"](X11/|xcb/|GLFW/glfw3native\\.h)"
        "Linux composition reaches native window protocol outside its adapters")
    endif()
    _avb0795_assert_no_match(
      "${file}"
      "${contents}"
      "(^|[^A-Za-z0-9_])(ApplicationState|ReplicaSession|RuntimeEvent|AppCommand|ControllerView|WgcCaptureJob|WindowsRenderStack|WindowsFrameBridge|VulkanRenderer)([^A-Za-z0-9_]|$)|[.>](tick|start|request_stop|apply|take_latest|drain_signals|enumerate|step|shutdown|create|view_state|next_deadline|exit_disposition|render)[ \t\r\n]*\\(|ImGui::|vk[A-Z][A-Za-z0-9_]*[ \t\r\n]*\\(|GraphicsCapture"
      "composition root contains session/runtime behavior")
  endforeach()

  set(all_files
      ${diagnostics_files}
      ${i18n_files}
      ${core_files}
      ${app_files}
      ${ui_files}
      ${desktop_files}
      ${windows_files}
      ${linux_files}
      ${render_files}
      ${main_files})
  foreach(file IN LISTS all_files)
    file(READ "${file}" contents)
    file(RELATIVE_PATH feature_relative "${AVB0795_SOURCE_ROOT}" "${file}")
    string(REPLACE "\\" "/" feature_relative "${feature_relative}")
    if(NOT feature_relative MATCHES "^platform/windows/features/")
      _avb0795_assert_no_match(
        "${file}"
        "${contents}"
        "TaskListThumbnailWnd|MultitaskingViewFrame"
        "browser workaround constants escape their feature module")
      if(NOT feature_relative MATCHES
         "^platform/windows/windows_(replica_runtime|runtime_factory)\\.(hpp|cpp)$")
        _avb0795_assert_no_match(
          "${file}"
          "${contents}"
          "platform/windows/features/|(^|[^A-Za-z0-9_])BrowserCaptureKeepAlive([^A-Za-z0-9_]|$)"
          "non-runtime consumer depends on the browser helper module")
      endif()
    endif()
    _avb0795_assert_no_match(
      "${file}"
      "${contents}"
      "(^|[^A-Za-z0-9_])(CapturedFrame|RenderFrame|IFrameBridge|IVulkanDriver)([^A-Za-z0-9_]|$)|void[ \t\r\n]*\\*[ \t\r\n]*native_resource|(std::)?shared_ptr[ \t\r\n]*<[ \t\r\n]*void[ \t\r\n]*>"
      "removed generic frame/native-resource contract reappeared")
  endforeach()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  _avb0795_run_source_boundary_scan()
endif()
