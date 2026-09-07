include_guard(GLOBAL)

set(_AIRELINNA_BOUNDARY_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(_airelinna_boundary_failure detail)
  message(FATAL_ERROR "Airelinna architecture boundary violation: ${detail}")
endfunction()

function(_airelinna_normalize_link output link)
  set(value "${link}")
  if(value MATCHES "^\\$<LINK_ONLY:(.*)>$")
    set(value "${CMAKE_MATCH_1}")
  elseif(value MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
    set(value "${CMAKE_MATCH_1}")
  endif()
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_airelinna_assert_links target)
  set(allowed ${ARGN})
  foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(links "${target}" "${property}")
    if(NOT links OR links MATCHES "-NOTFOUND$")
      continue()
    endif()

    foreach(link IN LISTS links)
      _airelinna_normalize_link(normalized "${link}")
      if(normalized IN_LIST allowed)
        continue()
      endif()
      _airelinna_boundary_failure(
        "${target} has disallowed direct link '${normalized}' in ${property}")
    endforeach()
  endforeach()
endfunction()

function(_airelinna_assert_sources target)
  set(allowed_roots ${ARGN})
  get_target_property(source_dir "${target}" SOURCE_DIR)
  get_target_property(sources "${target}" SOURCES)
  if(NOT sources OR sources MATCHES "-NOTFOUND$")
    _airelinna_boundary_failure("${target} has no sources")
  endif()

  foreach(source IN LISTS sources)
    if(source MATCHES "^\\$<")
      _airelinna_boundary_failure(
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
      _airelinna_boundary_failure(
        "${target} owns source outside its layer: ${absolute_source}")
    endif()
  endforeach()
endfunction()

function(_airelinna_collect_project_targets output directory)
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

    _airelinna_collect_project_targets(child_targets "${subdirectory}")
    list(APPEND project_targets ${child_targets})
  endforeach()

  set(${output} ${project_targets} PARENT_SCOPE)
endfunction()

function(_airelinna_assert_known_production_targets)
  set(known_targets
      airelinna_core
      airelinna_app
      airelinna_ui
      airelinna_windows
      airelinna_windows_graphics
      airelinna_desktop
      airelinna
      Airelinna
      imgui)
  _airelinna_collect_project_targets(project_targets "${PROJECT_SOURCE_DIR}")
  list(REMOVE_DUPLICATES project_targets)

  foreach(target IN LISTS project_targets)
    if(target IN_LIST known_targets)
      continue()
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "UTILITY")
      continue()
    endif()
    _airelinna_boundary_failure(
      "${target} is a project production target without an explicit layer allowlist")
  endforeach()
endfunction()

function(airelinna_assert_target_boundaries)
  _airelinna_assert_known_production_targets()

  if(TARGET airelinna_core)
    _airelinna_assert_sources(airelinna_core "src/core")
    _airelinna_assert_links(airelinna_core)
  endif()

  if(TARGET airelinna_app)
    _airelinna_assert_sources(airelinna_app "src/app")
    _airelinna_assert_links(
      airelinna_app airelinna_core nlohmann_json::nlohmann_json)
  endif()

  if(TARGET airelinna_ui)
    _airelinna_assert_sources(airelinna_ui "src/ui")
    _airelinna_assert_links(airelinna_ui airelinna_core imgui)
  endif()

  if(TARGET airelinna_windows)
    _airelinna_assert_sources(airelinna_windows "platform/windows")
    _airelinna_assert_links(
      airelinna_windows
      airelinna_core
      airelinna_app
      user32
      d3d11
      dxgi
      windowsapp)
  endif()

  if(TARGET airelinna_windows_graphics)
    _airelinna_assert_sources(
      airelinna_windows_graphics
      "platform/windows"
      "render"
      "${PROJECT_BINARY_DIR}/generated/airelinna/render")
    _airelinna_assert_links(
      airelinna_windows_graphics
      airelinna_core
      airelinna_app
      airelinna_windows
      glfw
      Vulkan::Vulkan
      d3d11
      dxgi
      user32)
  endif()

  if(TARGET airelinna_desktop)
    if(NOT DEFINED imgui_SOURCE_DIR)
      _airelinna_boundary_failure(
        "airelinna_desktop requires the pinned imgui source directory")
    endif()
    _airelinna_assert_sources(
      airelinna_desktop
      "src/desktop"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
    _airelinna_assert_links(
      airelinna_desktop
      airelinna_app
      airelinna_ui
      glfw
      Vulkan::Vulkan
      imgui
      imgui_impl_glfw
      imgui_impl_vulkan)
  endif()

  foreach(product_target airelinna Airelinna)
    if(TARGET "${product_target}")
      get_target_property(product_type "${product_target}" TYPE)
      if(NOT product_type STREQUAL "EXECUTABLE")
        _airelinna_boundary_failure(
          "${product_target} must be the executable composition root")
      endif()
      _airelinna_assert_sources(
        "${product_target}"
        "main.cpp"
        "resources/windows/airelinna.rc"
        "resources/windows/airelinna.manifest")
      _airelinna_assert_links(
        "${product_target}"
        airelinna_app
         airelinna_ui
         airelinna_desktop
         airelinna_windows
         airelinna_windows_graphics
         glfw
         windowsapp)
     endif()
   endforeach()
endfunction()

function(airelinna_add_source_boundary_target)
  if(TARGET verify_source_boundaries)
    _airelinna_boundary_failure(
      "verify_source_boundaries may be registered only once")
  endif()

  add_custom_target(
    verify_source_boundaries
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAIRELINNA_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
      -P "${_AIRELINNA_BOUNDARY_SCRIPT}"
    COMMENT "Verifying Airelinna source-layer boundaries"
    VERBATIM)
endfunction()

function(_airelinna_collect_sources output)
  set(files)
  foreach(relative_path IN LISTS ARGN)
    set(path "${AIRELINNA_SOURCE_ROOT}/${relative_path}")
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

function(_airelinna_assert_no_match file contents pattern description)
  if("${contents}" MATCHES "${pattern}")
    _airelinna_boundary_failure("${file}: ${description}")
  endif()
endfunction()

function(_airelinna_assert_app_includes file contents)
  set(allowed ${ARGN})
  string(
    REGEX MATCHALL
    "(airelinna/)?app/[A-Za-z0-9_./-]+\\.(hpp|h)"
    app_includes
    "${contents}")
  foreach(app_include IN LISTS app_includes)
    string(REGEX REPLACE "^app/" "airelinna/app/" normalized
           "${app_include}")
    if(NOT normalized IN_LIST allowed)
      _airelinna_boundary_failure(
        "${file}: disallowed application include '${normalized}'")
    endif()
  endforeach()
endfunction()

function(_airelinna_assert_layer_includes file contents prefix)
  set(allowed ${ARGN})
  string(
    REGEX MATCHALL
    "(airelinna/)?${prefix}/[A-Za-z0-9_./-]+\\.(hpp|h)"
    layer_includes
    "${contents}")
  foreach(layer_include IN LISTS layer_includes)
    string(REGEX REPLACE "^airelinna/" "" normalized "${layer_include}")
    if(NOT normalized IN_LIST allowed)
      _airelinna_boundary_failure(
        "${file}: disallowed ${prefix} include '${normalized}'")
    endif()
  endforeach()
endfunction()

function(_airelinna_scan_files category)
  set(files ${ARGN})
  set(portable_native_include
      "#[ \t]*include[ \t]*[<\"][^>\"]*(windows([./]|\\.h)|win[a-z0-9_]*\\.h|winrt/|wrl/|d3d[^>\"]*\\.h|dxgi[^>\"]*\\.h|dxcore\\.h|dwmapi\\.h|shellapi\\.h|shobjidl[^>\"]*\\.h|combaseapi\\.h|roapi\\.h|unknwn\\.h|objbase\\.h|objidl[^>\"]*\\.h|vulkan/|glfw/|imgui_impl_(glfw|vulkan)\\.h|platform/windows/)")
  set(portable_native_symbol
      "(^|[^A-Za-z0-9_])(HANDLE|HDC|HINSTANCE|HMONITOR|HWND|IUnknown|IInspectable|ID3D[A-Za-z0-9_]*|IDXGI[A-Za-z0-9_]*|Vk[A-Z][A-Za-z0-9_]*)([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])winrt::")
  set(desktop_forbidden_include
      "#[ \t]*include[ \t]*[<\"][^>\"]*(windows([./]|\\.h)|win[a-z0-9_]*\\.h|winrt/|wrl/|d3d[^>\"]*\\.h|dxgi[^>\"]*\\.h|dxcore\\.h|dwmapi\\.h|shellapi\\.h|shobjidl[^>\"]*\\.h|combaseapi\\.h|roapi\\.h|platform/windows/)")

  foreach(file IN LISTS files)
    file(READ "${file}" contents)
    string(TOLOWER "${contents}" lower_contents)
    string(REPLACE "\\" "/" normalized_lower_contents "${lower_contents}")

    if(category STREQUAL "core" OR category STREQUAL "app" OR
       category STREQUAL "ui")
      _airelinna_assert_no_match(
        "${file}" "${normalized_lower_contents}" "${portable_native_include}"
        "portable source includes a native graphics/platform header")
      _airelinna_assert_no_match(
        "${file}"
        "${normalized_lower_contents}"
        "#[ \t]*include[ \t]+[^<\" \t\r\n]"
        "portable source uses a non-literal include")
      _airelinna_assert_no_match(
        "${file}" "${contents}" "${portable_native_symbol}"
        "portable source mentions a native graphics/platform type")
    endif()

    if(category STREQUAL "core" OR category STREQUAL "app")
      _airelinna_assert_no_match(
        "${file}"
        "${normalized_lower_contents}"
        "#[ \t]*include[ \t]*[<\"][^>\"]*imgui(_internal|_stdlib)?\\.h"
        "portable source includes a UI header")
    endif()

    if(category STREQUAL "core")
      _airelinna_assert_app_includes("${file}" "${contents}")
      _airelinna_assert_layer_includes("${file}" "${contents}" "ui")
      _airelinna_assert_layer_includes("${file}" "${contents}" "desktop")
      _airelinna_assert_layer_includes("${file}" "${contents}" "render")
      _airelinna_assert_layer_includes(
        "${file}" "${contents}" "platform/windows")
    elseif(category STREQUAL "app")
      _airelinna_assert_layer_includes("${file}" "${contents}" "ui")
      _airelinna_assert_layer_includes("${file}" "${contents}" "desktop")
      _airelinna_assert_layer_includes("${file}" "${contents}" "render")
      _airelinna_assert_layer_includes(
        "${file}" "${contents}" "platform/windows")
    elseif(category STREQUAL "ui")
      _airelinna_assert_layer_includes("${file}" "${contents}" "render")
      _airelinna_assert_layer_includes("${file}" "${contents}" "desktop")
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "(airelinna/)?core/(application_state|replica_session)\\.hpp"
        "UI includes mutable domain state")
      _airelinna_assert_app_includes(
        "${file}"
        "${contents}"
        airelinna/app/commands.hpp
        airelinna/app/controller_view_state.hpp)
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "(^|[^A-Za-z0-9_])(Application|ApplicationState|ReplicaSession|IReplicaRuntime|IGlobalHotkey|ConfigStore|OverlayWindow|WindowsRenderStack)([^A-Za-z0-9_]|$)"
        "UI crosses the frontend command/view-state boundary")
    elseif(category STREQUAL "desktop")
      _airelinna_assert_layer_includes(
        "${file}"
        "${contents}"
        "render"
        render/graphics_retry_policy.hpp)
      _airelinna_assert_no_match(
        "${file}"
        "${lower_contents}"
        "${desktop_forbidden_include}"
        "desktop shell includes replica-native/platform ownership")
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "(airelinna/)?core/(application_state|replica_session)\\.hpp|(^|[^A-Za-z0-9_])(ApplicationState|ReplicaSession)([^A-Za-z0-9_]|$)"
        "desktop shell reaches mutable domain state")
      _airelinna_assert_app_includes(
        "${file}"
        "${contents}"
        airelinna/app/application.hpp
        airelinna/app/commands.hpp
        airelinna/app/controller_view_state.hpp)
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "(^|[^A-Za-z0-9_])(ISourceCatalog|IReplicaRuntimeFactory|IReplicaRuntime|IGlobalHotkey|ConfigStore|OverlayWindow|WindowsRenderStack|WgcCaptureJob|WindowsCaptureSource|WindowsCapturedFrame)([^A-Za-z0-9_]|$)"
        "desktop shell reaches a port, replica runtime, config store, or native replica owner")
    elseif(category STREQUAL "windows")
      get_filename_component(file_name "${file}" NAME)
      set(allowed_windows_app_headers airelinna/app/ports.hpp)
      set(is_hotkey_file FALSE)
      if(file_name MATCHES
         "^windows_global_hotkey\\.(c|cc|cpp|cxx|h|hpp)$")
        set(is_hotkey_file TRUE)
        list(APPEND
             allowed_windows_app_headers airelinna/app/commands.hpp)
      endif()
      _airelinna_assert_app_includes(
        "${file}"
        "${contents}"
        ${allowed_windows_app_headers})
      if(NOT is_hotkey_file)
        _airelinna_assert_no_match(
          "${file}"
          "${contents}"
          "(^|[^A-Za-z0-9_])(AppCommand|RestoreActiveReplicaEditable)([^A-Za-z0-9_]|$)|windows_global_hotkey"
          "non-hotkey Windows adapter reaches the command path")
      endif()
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "(airelinna/)?core/(application_state|replica_session)|(airelinna/)?(ui|desktop)/"
        "Windows adapter includes domain state or frontend/desktop code")
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "(^|[^A-Za-z0-9_])(Application|ApplicationState|ReplicaSession|ControllerView|DesktopHost)([^A-Za-z0-9_]|$)"
        "Windows adapter mentions an owner outside its boundary")
    elseif(category STREQUAL "render")
      _airelinna_assert_app_includes("${file}" "${contents}")
      _airelinna_assert_layer_includes(
        "${file}"
        "${contents}"
        "platform/windows"
        platform/windows/overlay_window.hpp
        platform/windows/windows_captured_frame.hpp)
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "(airelinna/)?core/(application_state|replica_session)|(airelinna/)?(ui|desktop)/"
        "render source includes application/domain/frontend ownership")
    endif()
  endforeach()
endfunction()

function(_airelinna_run_source_boundary_scan)
  if(NOT DEFINED AIRELINNA_SOURCE_ROOT OR
     NOT IS_DIRECTORY "${AIRELINNA_SOURCE_ROOT}")
    _airelinna_boundary_failure(
      "AIRELINNA_SOURCE_ROOT must name the project source directory")
  endif()

  _airelinna_collect_sources(
    core_files "include/airelinna/core" "src/core")
  _airelinna_scan_files(core ${core_files})

  _airelinna_collect_sources(
    app_files "include/airelinna/app" "src/app")
  _airelinna_scan_files(app ${app_files})

  _airelinna_collect_sources(ui_files "include/airelinna/ui" "src/ui")
  _airelinna_scan_files(ui ${ui_files})

  _airelinna_collect_sources(
    desktop_files "include/airelinna/desktop" "src/desktop")
  _airelinna_scan_files(desktop ${desktop_files})

  _airelinna_collect_sources(windows_files "platform/windows")
  _airelinna_scan_files(windows ${windows_files})

  _airelinna_collect_sources(render_files "render")
  _airelinna_scan_files(render ${render_files})

  _airelinna_collect_sources(main_files "main.cpp")
  foreach(file IN LISTS main_files)
    file(READ "${file}" contents)
    _airelinna_assert_no_match(
      "${file}"
      "${contents}"
      "(^|[^A-Za-z0-9_])(ApplicationState|ReplicaSession|RuntimeEvent|AppCommand|ControllerView|WgcCaptureJob|WindowsRenderStack)([^A-Za-z0-9_]|$)|[.>](tick|start|request_stop|apply|take_latest|drain_signals|enumerate|step|shutdown|create|view_state|next_deadline|exit_disposition|render)[ \t\r\n]*\\(|ImGui::|vk[A-Z][A-Za-z0-9_]*[ \t\r\n]*\\(|GraphicsCapture"
      "composition root contains session/runtime behavior")
  endforeach()

  set(all_files
      ${core_files}
      ${app_files}
      ${ui_files}
      ${desktop_files}
      ${windows_files}
      ${render_files}
      ${main_files})
  foreach(file IN LISTS all_files)
    file(READ "${file}" contents)
    file(RELATIVE_PATH feature_relative "${AIRELINNA_SOURCE_ROOT}" "${file}")
    string(REPLACE "\\" "/" feature_relative "${feature_relative}")
    if(NOT feature_relative MATCHES "^platform/windows/features/")
      _airelinna_assert_no_match(
        "${file}"
        "${contents}"
        "TaskListThumbnailWnd|MultitaskingViewFrame"
        "browser workaround constants escape their feature module")
      if(NOT feature_relative MATCHES
         "^platform/windows/windows_(replica_runtime|runtime_factory)\\.(hpp|cpp)$")
        _airelinna_assert_no_match(
          "${file}"
          "${contents}"
          "platform/windows/features/|(^|[^A-Za-z0-9_])BrowserCaptureKeepAlive([^A-Za-z0-9_]|$)"
          "non-runtime consumer depends on the browser helper module")
      endif()
    endif()
    _airelinna_assert_no_match(
      "${file}"
      "${contents}"
      "(^|[^A-Za-z0-9_])(CapturedFrame|RenderFrame|IFrameBridge|IVulkanDriver)([^A-Za-z0-9_]|$)|void[ \t\r\n]*\\*[ \t\r\n]*native_resource|(std::)?shared_ptr[ \t\r\n]*<[ \t\r\n]*void[ \t\r\n]*>"
      "removed generic frame/native-resource contract reappeared")
  endforeach()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  _airelinna_run_source_boundary_scan()
endif()
