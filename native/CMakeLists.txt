cmake_minimum_required(VERSION 3.24)
project(HomePanelNative VERSION 2.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
  message(FATAL_ERROR "HomePanel native build requires MSVC and the Windows SDK")
endif()

option(HOMEPANEL_CI_STRICT "Treat compiler and linker warnings as errors" OFF)
option(HOMEPANEL_CI_STATIC_ANALYSIS "Run MSVC native code analysis" OFF)




file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(HOMEPANEL_RADAR_SATELLITE "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ui/radar-satellite.png")
set(HOMEPANEL_RADAR_MAP "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ui/radar-map.png")
if(NOT EXISTS "${HOMEPANEL_RADAR_SATELLITE}" OR NOT EXISTS "${HOMEPANEL_RADAR_MAP}")
  message(FATAL_ERROR "Bundled radar layers are missing from scripts/ui")
endif()
file(GLOB HOMEPANEL_WEATHER_ICON_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ui/weather-icons/*.png")
list(SORT HOMEPANEL_WEATHER_ICON_FILES)
if(HOMEPANEL_WEATHER_ICON_FILES STREQUAL "")
  message(FATAL_ERROR "Bundled weather icons are missing from scripts/ui/weather-icons")
endif()

set(HOMEPANEL_WEATHER_ICON_RCDATA "")
set(HOMEPANEL_WEATHER_ICON_ASSETS "constexpr RuntimeAsset kWeatherIconAssets[] = {\n")
set(HOMEPANEL_WEATHER_ICON_ID 2000)
foreach(icon_file IN LISTS HOMEPANEL_WEATHER_ICON_FILES)
  get_filename_component(icon_name "${icon_file}" NAME)
  file(TO_CMAKE_PATH "${icon_file}" icon_resource_path)
  string(APPEND HOMEPANEL_WEATHER_ICON_RCDATA "${HOMEPANEL_WEATHER_ICON_ID} RCDATA \"${icon_resource_path}\"\n")
  string(APPEND HOMEPANEL_WEATHER_ICON_ASSETS "    {${HOMEPANEL_WEATHER_ICON_ID}, L\"weather-icons/${icon_name}\"},\n")
  math(EXPR HOMEPANEL_WEATHER_ICON_ID "${HOMEPANEL_WEATHER_ICON_ID} + 1")
endforeach()
string(APPEND HOMEPANEL_WEATHER_ICON_ASSETS "};\n")

set(HOMEPANEL_GENERATED_HOME_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/HomePanel.rc")
set(HOMEPANEL_GENERATED_UPDATER_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/HomePanelUpdater.rc")
set(HOMEPANEL_GENERATED_VERSION_H "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")
set(HOMEPANEL_GENERATED_WEATHER_ICON_ASSETS_H "${CMAKE_CURRENT_BINARY_DIR}/generated/weather_icon_assets.h")
set(HOMEPANEL_GENERATED_WEATHER_ICON_RESOURCES_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/weather_icon_resources.rc")
file(TO_CMAKE_PATH "${HOMEPANEL_GENERATED_WEATHER_ICON_RESOURCES_RC}" HOMEPANEL_WEATHER_ICON_RC_INCLUDE)
file(WRITE "${HOMEPANEL_GENERATED_WEATHER_ICON_ASSETS_H}" "${HOMEPANEL_WEATHER_ICON_ASSETS}")
file(WRITE "${HOMEPANEL_GENERATED_WEATHER_ICON_RESOURCES_RC}" "${HOMEPANEL_WEATHER_ICON_RCDATA}")

add_custom_target(homepanel_version_files ALL
  COMMAND "${CMAKE_COMMAND}"
          -DHOMEPANEL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DINPUT_HOME_RC=${CMAKE_CURRENT_SOURCE_DIR}/resources/HomePanel.rc.in
          -DINPUT_UPDATER_RC=${CMAKE_CURRENT_SOURCE_DIR}/resources/HomePanelUpdater.rc.in
          -DINPUT_VERSION_H=${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
          -DOUTPUT_HOME_RC=${HOMEPANEL_GENERATED_HOME_RC}
          -DOUTPUT_UPDATER_RC=${HOMEPANEL_GENERATED_UPDATER_RC}
          -DOUTPUT_VERSION_H=${HOMEPANEL_GENERATED_VERSION_H}
          -DHOMEPANEL_RADAR_SATELLITE=${HOMEPANEL_RADAR_SATELLITE}
          -DHOMEPANEL_RADAR_MAP=${HOMEPANEL_RADAR_MAP}
          -DHOMEPANEL_WEATHER_ICON_RC_INCLUDE=${HOMEPANEL_WEATHER_ICON_RC_INCLUDE}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version_files.cmake"
  BYPRODUCTS
    "${HOMEPANEL_GENERATED_HOME_RC}"
    "${HOMEPANEL_GENERATED_UPDATER_RC}"
    "${HOMEPANEL_GENERATED_VERSION_H}"
    "${HOMEPANEL_GENERATED_WEATHER_ICON_ASSETS_H}"
    "${HOMEPANEL_GENERATED_WEATHER_ICON_RESOURCES_RC}"
  VERBATIM)

set(WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packages/Microsoft.Web.WebView2.1.0.4022.49" CACHE PATH "Extracted Microsoft WebView2 NuGet package")
set(WEBVIEW2_NATIVE_DIR "${WEBVIEW2_DIR}/build/native/x64")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h" OR NOT EXISTS "${WEBVIEW2_NATIVE_DIR}/WebView2Loader.dll.lib" OR NOT EXISTS "${WEBVIEW2_NATIVE_DIR}/WebView2Loader.dll")
  message(FATAL_ERROR "WebView2 SDK not found. Run: nuget install Microsoft.Web.WebView2 -Version 1.0.4022.49 -OutputDirectory packages")
endif()

function(homepanel_optimize_target target)
  set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  target_compile_options(${target} PRIVATE
    /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /sdl /guard:cf
    $<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>
    $<$<CONFIG:Debug>:/Zi /Od /FS>)
  target_link_options(${target} PRIVATE
    /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /CETCOMPAT
    $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>
    $<$<CONFIG:Release>:/INCREMENTAL:NO> $<$<CONFIG:Debug>:/INCREMENTAL>)
  if(HOMEPANEL_CI_STRICT)
    target_compile_options(${target} PRIVATE /WX)
    target_link_options(${target} PRIVATE /WX)
  endif()
  if(HOMEPANEL_CI_STATIC_ANALYSIS)
    target_compile_options(${target} PRIVATE /analyze /analyze:external-)
  else()
    target_compile_options(${target} PRIVATE /MP)
  endif()
endfunction()

function(homepanel_configure_common_target target)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_include_directories(${target} SYSTEM PRIVATE "${WEBVIEW2_DIR}/build/native/include")
  target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_precompile_headers(${target} PRIVATE src/common.h)
  homepanel_optimize_target(${target})
endfunction()

set(HOMEPANEL_CORE_SOURCES
  src/main.cpp
  src/app.cpp
  src/config.cpp
  src/cloud_config.cpp
  src/logger.cpp
  src/cloud_client.cpp
  src/dashboard_data.cpp
  src/update_client.cpp
  src/sensors.cpp)

set(HOMEPANEL_STATIONHEAD_SOURCES
  src/shared_webview_environment.cpp
  src/stationhead_window_layout.cpp
  src/sh.cpp
  src/secondary_sh.cpp
  src/secondary_sh_audio.cpp)

set(HOMEPANEL_RENDERER_SOURCES
  src/renderer_core.cpp
  src/renderer_panels.cpp
  src/renderer_radar_ui.cpp)

set(HOMEPANEL_RESOURCE_SOURCES
  "${HOMEPANEL_GENERATED_HOME_RC}"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/HomePanel.manifest")
set_source_files_properties("${HOMEPANEL_GENERATED_HOME_RC}" PROPERTIES
  OBJECT_DEPENDS "${HOMEPANEL_RADAR_SATELLITE};${HOMEPANEL_RADAR_MAP};${HOMEPANEL_WEATHER_ICON_FILES}")

add_executable(HomePanel WIN32
  ${HOMEPANEL_CORE_SOURCES}
  ${HOMEPANEL_STATIONHEAD_SOURCES}
  ${HOMEPANEL_RENDERER_SOURCES}
  ${HOMEPANEL_RESOURCE_SOURCES})
homepanel_configure_common_target(HomePanel)
add_dependencies(HomePanel homepanel_version_files)
target_link_libraries(HomePanel PRIVATE
  "${WEBVIEW2_NATIVE_DIR}/WebView2Loader.dll.lib"
  windowscodecs winhttp bcrypt wintrust setupapi psapi shell32
  ole32 oleaut32 user32 gdi32 msimg32 shlwapi windowsapp runtimeobject crypt32 iphlpapi)
add_custom_command(TARGET HomePanel POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WEBVIEW2_NATIVE_DIR}/WebView2Loader.dll" "$<TARGET_FILE_DIR:HomePanel>/WebView2Loader.dll"
  VERBATIM)

set(HOMEPANEL_UPDATER_SOURCES
  src/updater_entry.cpp
  src/update_client.cpp
  src/config.cpp
  src/logger.cpp
  src/cloud_client.cpp
  "${HOMEPANEL_GENERATED_UPDATER_RC}"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/HomePanel.manifest")

add_executable(HomePanelUpdater WIN32 ${HOMEPANEL_UPDATER_SOURCES})
homepanel_configure_common_target(HomePanelUpdater)
add_dependencies(HomePanelUpdater homepanel_version_files)
target_link_libraries(HomePanelUpdater PRIVATE winhttp bcrypt wintrust crypt32 shell32 ole32 user32 windowsapp runtimeobject iphlpapi)

install(TARGETS HomePanel HomePanelUpdater RUNTIME DESTINATION .)
install(FILES config.example.json DESTINATION .)
