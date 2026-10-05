cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_MULTIPUSHER "Enable building the obs-multipusher plugin" ON)
if(NOT ENABLE_MULTIPUSHER)
  target_disable(obs-multipusher)
  return()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil avformat)

add_library(obs-multipusher MODULE)
add_library(OBS::multipusher ALIAS obs-multipusher)

target_sources(
  obs-multipusher
  PRIVATE # cmake-format: sortable
          src/config-manager.cpp
          src/config-manager.hpp
          src/device-enumerator.cpp
          src/device-enumerator.hpp
          src/multipusher-dock.cpp
          src/multipusher-dock.hpp
          src/output-manager.cpp
          src/output-manager.hpp
          src/player-link.cpp
          src/player-link.hpp
          src/plugin-main.cpp
          src/plugin-main.hpp
          src/srt-auth.cpp
          src/srt-auth.hpp
          src/srt-pusher.cpp
          src/srt-pusher.hpp
          src/status-reporter.cpp
          src/status-reporter.hpp
          src/stream-ladder.cpp
          src/stream-ladder.hpp
          src/utils.cpp
          src/utils.hpp
)

target_include_directories(obs-multipusher PRIVATE src)

target_compile_definitions(obs-multipusher PRIVATE HAS_DOCK_UI=1)

target_link_libraries(
  obs-multipusher
  PRIVATE OBS::libobs
          OBS::frontend-api
          FFmpeg::avcodec
          FFmpeg::avutil
          FFmpeg::avformat
          Qt::Widgets
          Qt::Core
          $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
          $<$<PLATFORM_ID:Windows>:winhttp>
          $<$<PLATFORM_ID:Windows>:ws2_32>
)

set_target_properties_obs(
  obs-multipusher
  PROPERTIES FOLDER plugins
             PREFIX ""
             AUTOMOC ON
)
