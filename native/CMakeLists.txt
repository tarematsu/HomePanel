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

set(HOMEPANEL_BUILD_VERSION "$ENV{HOMEPANEL_BUILD_VERSION}")
if(NOT HOMEPANEL_BUILD_VERSION)
  set(HOMEPANEL_BUILD_VERSION "${PROJECT_VERSION}")
endif()
if(HOMEPANEL_BUILD_VERSION MATCHES "^([0-9]{2})([0-9]{2})([0-9]{2})([0-9]{4})$")
  set(HOMEPANEL_VERSION_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},${CMAKE_MATCH_4}")
elseif(HOMEPANEL_BUILD_VERSION MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
  string(REPLACE "." "," HOMEPANEL_VERSION_COMMA "${HOMEPANEL_BUILD_VERSION}")
  set(HOMEPANEL_VERSION_COMMA "${HOMEPANEL_VERSION_COMMA},0")
else()
  message(FATAL_ERROR "HOMEPANEL_BUILD_VERSION must use yymmddhhmm or numeric major.minor.patch format")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

find_program(HOMEPANEL_POWERSHELL NAMES pwsh powershell REQUIRED)
set(HOMEPANEL_SCRIPT_OUTPUT "")
function(homepanel_run_script description script)
  cmake_parse_arguments(HP "" "" "ARGS" ${ARGN})
  execute_process(
    COMMAND "${HOMEPANEL_POWERSHELL}" -NoLogo -NoProfile -ExecutionPolicy Bypass
            -File "${script}" ${HP_ARGS}
    RESULT_VARIABLE HP_RESULT
    OUTPUT_VARIABLE HP_OUTPUT
    ERROR_VARIABLE HP_ERROR)
  if(NOT HP_RESULT EQUAL 0)
    message(FATAL_ERROR "${description} failed.\nstdout: ${HP_OUTPUT}\nstderr: ${HP_ERROR}")
  endif()
  set(HOMEPANEL_SCRIPT_OUTPUT "${HOMEPANEL_SCRIPT_OUTPUT}${HP_OUTPUT}" PARENT_SCOPE)
endfunction()

set(HOMEPANEL_RADAR_SATELLITE "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ui/radar-satellite.png")
set(HOMEPANEL_RADAR_MAP "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ui/radar-map.png")
if(NOT EXISTS "${HOMEPANEL_RADAR_SATELLITE}" OR NOT EXISTS "${HOMEPANEL_RADAR_MAP}")
  message(FATAL_ERROR "Bundled radar layers are missing from scripts/ui")
endif()

configure_file(resources/HomePanel.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/HomePanel.rc" @ONLY)
configure_file(resources/HomePanelUpdater.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/HomePanelUpdater.rc" @ONLY)
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

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
  src/radar.cpp
  src/sensors.cpp)

set(HOMEPANEL_SPOTIFY_SOURCES
  src/cloud_spotify_auth.cpp
  src/secondary_spotify_api.cpp)

set(HOMEPANEL_STATIONHEAD_SOURCES
  src/shared_webview_environment.cpp
  src/mn_automation.cpp
  src/sh_player.cpp
  src/sh_audio.cpp
  src/sh_selection.cpp
  src/sh_state.cpp
  src/secondary_sh.cpp
  src/secondary_sh_audio.cpp)

set(HOMEPANEL_RENDERER_SOURCES
  src/renderer_core.cpp
  src/renderer_visuals.cpp
  src/renderer_animation.cpp
  src/renderer_button.cpp
  src/renderer_panel.cpp
  src/renderer_status.cpp
  src/renderer_dashboard_base.cpp
  src/renderer_news.cpp
  src/renderer_clock_weather.cpp
  src/renderer_metrics.cpp
  src/renderer_co2_graph.cpp
  src/renderer_spotify.cpp
  src/renderer_energy.cpp
  src/renderer_switchbot_panel.cpp
  src/renderer_radar_ui.cpp
  src/renderer_diagnostics_ui.cpp
  src/renderer_runtime.cpp)

set(HOMEPANEL_RESOURCE_SOURCES
  "${CMAKE_CURRENT_BINARY_DIR}/generated/HomePanel.rc"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/HomePanel.manifest")

add_executable(HomePanel WIN32
  ${HOMEPANEL_CORE_SOURCES}
  ${HOMEPANEL_SPOTIFY_SOURCES}
  ${HOMEPANEL_STATIONHEAD_SOURCES}
  ${HOMEPANEL_RENDERER_SOURCES}
  ${HOMEPANEL_RESOURCE_SOURCES})
homepanel_configure_common_target(HomePanel)
target_compile_definitions(HomePanel PRIVATE HOMEPANEL_ENABLE_SPOTIFY_RUNTIME)
target_link_libraries(HomePanel PRIVATE
  "${WEBVIEW2_NATIVE_DIR}/WebView2Loader.dll.lib"
  d2d1 dwrite d3d11 dcomp dxgi windowscodecs winhttp bcrypt wintrust setupapi psapi shell32
  ole32 oleaut32 user32 gdi32 shlwapi windowsapp runtimeobject powrprof crypt32 iphlpapi)
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
  "${CMAKE_CURRENT_BINARY_DIR}/generated/HomePanelUpdater.rc"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/HomePanel.manifest")

add_executable(HomePanelUpdater WIN32 ${HOMEPANEL_UPDATER_SOURCES})
homepanel_configure_common_target(HomePanelUpdater)
target_link_libraries(HomePanelUpdater PRIVATE winhttp bcrypt wintrust crypt32 shell32 ole32 user32 windowsapp runtimeobject iphlpapi)

install(TARGETS HomePanel HomePanelUpdater RUNTIME DESTINATION .)
install(FILES config.example.json "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md" DESTINATION .)
