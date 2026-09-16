# These targets encode compile/link ownership, not just convenient source groups.
# Portable values and language policy can be used without native graphics. The UI
# consumes public command/view contracts but never links the application owner.
# Concrete Windows services and the controller host meet at the executable below.
add_library(avb0795_diagnostics
  diagnostics/performance_report.cpp
)

target_include_directories(avb0795_diagnostics
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_features(avb0795_diagnostics PUBLIC cxx_std_23)
target_link_libraries(avb0795_diagnostics PRIVATE nlohmann_json::nlohmann_json)

add_library(avb0795_i18n
  i18n/catalog.cpp
  i18n/language.cpp
)

target_include_directories(avb0795_i18n
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(avb0795_i18n PUBLIC cxx_std_23)

if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(avb0795_i18n PRIVATE /utf-8)
endif()

add_library(avb0795_core
  core/application_state.cpp
  core/frame_scheduler.cpp
  core/hotkey.cpp
  core/replica_session.cpp
  core/types.cpp
)

target_include_directories(avb0795_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(avb0795_core PUBLIC cxx_std_23)

add_library(avb0795_app
  app/app_command_queue.cpp
  app/application.cpp
  app/application_language.cpp
  app/config_store.cpp
  app/controller_view_state.cpp
  app/localized_presentation.cpp
)

target_link_libraries(avb0795_app
  PUBLIC
    avb0795_core
    avb0795_i18n
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(avb0795_app PUBLIC cxx_std_23)

add_library(avb0795_ui
  ui/controller_view.cpp
  ui/language_selector.cpp
)

target_link_libraries(avb0795_ui
  PUBLIC
    avb0795_core
    avb0795_i18n
  PRIVATE
    imgui
)

target_compile_features(avb0795_ui PUBLIC cxx_std_23)

# Native capture/system adapters are separate from the replica graphics aggregate.
# Only the latter needs both capture and presentation dependencies; a frontend
# target must not inherit these dependencies as a shortcut to native operations.
if(WIN32)
  # This owner consumes Vulkan/GLFW and neutral frame values. Current dependency
  # configuration enables graphics on Windows; the renderer itself has a separate
  # source/link boundary from native capture and the Windows composition facade.
  add_library(avb0795_vulkan_renderer
    ${PROJECT_SOURCE_DIR}/render/graphics_retry_policy.cpp
    ${PROJECT_SOURCE_DIR}/render/render_math.cpp
    ${PROJECT_SOURCE_DIR}/render/vulkan_device.cpp
    ${PROJECT_SOURCE_DIR}/render/vulkan_renderer.cpp
    ${AVB0795_REPLICA_SHADER_SOURCE}
  )

  add_dependencies(avb0795_vulkan_renderer avb0795_replica_shaders)

  target_include_directories(avb0795_vulkan_renderer
    PUBLIC
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
    PRIVATE
      ${PROJECT_BINARY_DIR}/generated
  )

  target_link_libraries(avb0795_vulkan_renderer
    PUBLIC
      avb0795_core
      Vulkan::Vulkan
    PRIVATE
      avb0795_diagnostics
      glfw
  )
  target_compile_features(avb0795_vulkan_renderer PUBLIC cxx_std_23)

  add_library(avb0795_windows
    ${PROJECT_SOURCE_DIR}/platform/windows/capture_exclusion.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/controller_font.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/detail/wgc_capture_state.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/features/browser_capture_keep_alive.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/process_dpi_awareness.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_atomic_replace.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_captured_frame.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_capture_source.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_controller_window.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_global_hotkey.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_source_catalog.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_source_registry.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_ui_language.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/wgc_capture_job.cpp
  )

  target_include_directories(avb0795_windows
    PUBLIC
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(avb0795_windows
    PRIVATE
      avb0795_core
      avb0795_app
      avb0795_i18n
      user32
      comctl32
      d3d11
      dxgi
      windowsapp
  )

  target_compile_definitions(avb0795_windows
    PRIVATE
      NOMINMAX
      WIN32_LEAN_AND_MEAN
  )

  target_compile_features(avb0795_windows PUBLIC cxx_std_23)

  add_library(avb0795_windows_graphics
    ${PROJECT_SOURCE_DIR}/platform/windows/detail/bridge_policy.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_frame_bridge.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_render_stack.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/detail/overlay_policy.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/overlay_window.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/win32_overlay_adapter.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_replica_runtime.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_runtime_factory.cpp
  )

  target_include_directories(avb0795_windows_graphics
    PUBLIC
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(avb0795_windows_graphics
    PRIVATE
      avb0795_core
      avb0795_app
      avb0795_windows
      avb0795_vulkan_renderer
      avb0795_diagnostics
      glfw
      Vulkan::Vulkan
      d3d11
      dxgi
      user32
  )

  target_compile_definitions(avb0795_windows_graphics
    PRIVATE
      NOMINMAX
      WIN32_LEAN_AND_MEAN
  )

  target_compile_features(avb0795_windows_graphics PUBLIC cxx_std_23)

  # The controller has its own backend/device lifetime. Compiling these backend
  # sources here does not make the replica renderer their owner or consumer.
  add_library(avb0795_desktop
    desktop/desktop_host.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  )

  target_include_directories(avb0795_desktop
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${imgui_SOURCE_DIR}/backends
  )

  target_link_libraries(avb0795_desktop
    PRIVATE
      avb0795_app
      avb0795_ui
      avb0795_diagnostics
      avb0795_vulkan_renderer
      glfw
      Vulkan::Vulkan
      imgui
  )

  target_compile_features(avb0795_desktop PUBLIC cxx_std_23)

  add_executable(avb0795
    ${PROJECT_SOURCE_DIR}/main.cpp
  )

  target_link_libraries(avb0795
    PRIVATE
      avb0795_app
      avb0795_desktop
      avb0795_windows
      avb0795_windows_graphics
      avb0795_diagnostics
      glfw
      windowsapp
  )

  target_compile_definitions(avb0795
    PRIVATE
      NOMINMAX
      WIN32_LEAN_AND_MEAN
  )

  target_compile_features(avb0795 PRIVATE cxx_std_23)
endif()
