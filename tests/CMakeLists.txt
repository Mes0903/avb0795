add_executable(avb0795_core_tests
  core/latest_value_mailbox_test.cpp
  core/frame_scheduler_test.cpp
  app/application_contract_test.cpp
  app/config_store_test.cpp
  app/controller_contract_test.cpp
  core/hotkey_test.cpp
)

target_link_libraries(avb0795_core_tests
  PRIVATE
    avb0795_app
    avb0795_core
    avb0795_i18n
    avb0795_ui
    imgui
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)

target_include_directories(avb0795_core_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}
)

if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(avb0795_core_tests PRIVATE /utf-8)
endif()

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(avb0795_core_tests ADD_TAGS_AS_LABELS)

if(WIN32)
  target_link_libraries(avb0795_core_tests
    PRIVATE
      avb0795_windows
  )

  add_executable(avb0795_windows_policy_tests
    render/render_policy_test.cpp
    windows/overlay_policy_test.cpp
    windows/windows_policy_test.cpp
  )

  target_include_directories(avb0795_windows_policy_tests
    PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(avb0795_windows_policy_tests
    PRIVATE
      glfw
      avb0795_windows
      avb0795_windows_graphics
      avb0795_core
      avb0795_i18n
      avb0795_diagnostics
      nlohmann_json::nlohmann_json
      Catch2::Catch2WithMain
      Vulkan::Vulkan
  )

  catch_discover_tests(avb0795_windows_policy_tests ADD_TAGS_AS_LABELS)

  if(AVB0795_ENABLE_WINDOWS_INTEGRATION)
    add_executable(avb0795_wgc_hardware_tests
      windows/wgc_hardware_test.cpp
    )

    target_link_libraries(avb0795_wgc_hardware_tests
      PRIVATE
        avb0795_windows
        avb0795_core
        Catch2::Catch2
        d3d11
        user32
        windowsapp
    )

    target_compile_definitions(avb0795_wgc_hardware_tests
      PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    catch_discover_tests(avb0795_wgc_hardware_tests
      ADD_TAGS_AS_LABELS
      SKIP_IS_FAILURE
      PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
    )

    add_executable(avb0795_windows_graphics_hardware_tests
      windows/windows_graphics_hardware_test.cpp
    )

    target_include_directories(avb0795_windows_graphics_hardware_tests
      PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(avb0795_windows_graphics_hardware_tests
      PRIVATE
        avb0795_windows_graphics
        avb0795_windows
        avb0795_core
        Catch2::Catch2
        glfw
        Vulkan::Vulkan
        d3d11
        dxgi
        user32
        windowsapp
    )

    target_compile_definitions(avb0795_windows_graphics_hardware_tests
      PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    catch_discover_tests(avb0795_windows_graphics_hardware_tests
      ADD_TAGS_AS_LABELS
      SKIP_IS_FAILURE
      PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
    )
  endif()
endif()
