include(FetchContent)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(json)

add_executable(condorhands
  src/main.cpp
  src/audio.cpp
  src/cockpit.cpp
  src/ovr_hands.cpp
  src/profile.cpp
  src/vjoy.cpp)

target_link_libraries(condorhands PRIVATE nlohmann_json::nlohmann_json winmm ole32 shell32)
target_compile_features(condorhands PRIVATE cxx_std_20)
target_compile_definitions(condorhands PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(condorhands PRIVATE /W4 /permissive- /utf-8)
set_target_properties(condorhands PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Profile templates next to the exe; user copies are made on first use.
add_custom_command(TARGET condorhands POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${PROJECT_SOURCE_DIR}/profiles $<TARGET_FILE_DIR:condorhands>/profiles)
