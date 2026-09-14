cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Native OBS properties already follow the OBS theme, so no custom Qt UI is
# needed. Keep both off for the simplest, most portable build.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Svg SvgWidgets)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg Qt6::SvgWidgets)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PLUGIN_VERSION="${_version}")

# FFmpeg (from obs-deps, already on CMAKE_PREFIX_PATH) - used by the disk backend.
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
find_library(AVFORMAT_LIBRARY avformat REQUIRED)
find_library(AVCODEC_LIBRARY avcodec REQUIRED)
find_library(AVUTIL_LIBRARY avutil REQUIRED)
find_library(SWSCALE_LIBRARY swscale REQUIRED)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY}
)

# (Censure + AI removed from Broadcast Delay; ONNX no longer needed. The delay
# engine exposes the bd_* C API + live frame tap so Broadcast Censure does the
# AI/censure on the live feed.)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp src/dse-source.cpp src/core/dse-globals.cpp src/core/dse-status.cpp src/core/bd-api.cpp src/gfx/gfx-util.cpp src/config/dse-properties.cpp src/config/dse-hotkeys.cpp src/warp/warp-transport.cpp src/warp/dse-dock-scene.cpp src/audio/dse-audio.cpp src/audio/device-capture.cpp src/render/dse-render.cpp src/capture/dse-buffers.cpp src/capture/dse-capture.cpp src/delay/disk-buffer.cpp src/delay/png-stb.cpp src/ui/dock.cpp src/ui/preview-dock.cpp src/ui/consumption-dock.cpp src/ui/mixer-dock.cpp src/ui/kz-delay-dock.cpp src/ui/vu-meter.hpp src/vendor/miniaudio/miniaudio_impl.c
)

# Let every module include siblings as "<folder>/<header>.hpp" from the src root,
# and the public C API header as "broadcast_delay_api.h".
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
