add_library(airelinna_core
  core/application_state.cpp
  core/frame_scheduler.cpp
  core/hotkey.cpp
  core/replica_session.cpp
  core/types.cpp
)

target_include_directories(airelinna_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(airelinna_core PUBLIC cxx_std_23)

add_library(airelinna_app
  app/config_store.cpp
)

target_link_libraries(airelinna_app
  PUBLIC
    airelinna_core
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(airelinna_app PUBLIC cxx_std_23)

if(WIN32)
  add_library(airelinna_windows
    ${PROJECT_SOURCE_DIR}/platform/windows/capture_exclusion.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/detail/wgc_capture_state.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_captured_frame.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_capture_source.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_global_hotkey.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_source_catalog.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/windows_source_registry.cpp
    ${PROJECT_SOURCE_DIR}/platform/windows/wgc_capture_job.cpp
  )

  target_include_directories(airelinna_windows
    PUBLIC
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(airelinna_windows
    PRIVATE
      airelinna_core
      airelinna_app
      user32
      d3d11
      dxgi
      windowsapp
  )

  target_compile_definitions(airelinna_windows
    PRIVATE
      NOMINMAX
      WIN32_LEAN_AND_MEAN
  )

  target_compile_features(airelinna_windows PUBLIC cxx_std_23)
endif()
