include(FetchContent)

# Only the headers are needed; SOURCE_SUBDIR points nowhere so the loader is not built.
FetchContent_Declare(openxr_sdk
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG release-1.1.65
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR headers-only)
FetchContent_MakeAvailable(openxr_sdk)

set(LAYER_NAME XR_APILAYER_CONDOR3VRCOCKPIT_hands)

add_library(${LAYER_NAME} SHARED
  src/layer.cpp
  src/log.cpp
  src/layer.def)

target_include_directories(${LAYER_NAME} PRIVATE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_features(${LAYER_NAME} PRIVATE cxx_std_20)
target_compile_definitions(${LAYER_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(${LAYER_NAME} PRIVATE /W4 /permissive- /utf-8)
# Static CRT: users don't need a VC++ redistributable.
set_target_properties(${LAYER_NAME} PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The loader resolves library_path relative to the manifest, so keep them side by side.
add_custom_command(TARGET ${LAYER_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/manifest/${LAYER_NAME}.json
    $<TARGET_FILE_DIR:${LAYER_NAME}>/${LAYER_NAME}.json)
