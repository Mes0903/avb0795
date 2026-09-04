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
