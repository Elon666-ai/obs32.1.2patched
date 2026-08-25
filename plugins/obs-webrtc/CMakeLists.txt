cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_WEBRTC "Enable WebRTC Output support" ON)
if(NOT ENABLE_WEBRTC)
  target_disable(obs-webrtc)
  return()
endif()

find_package(LibDataChannel 0.20 REQUIRED)
find_package(CURL REQUIRED)
# avcodec/swscale: quality-score.cpp decodes the stream's own top layer
# back and compares it against the program feed.
find_package(FFmpeg REQUIRED avcodec avutil swscale)

add_library(obs-webrtc MODULE)
add_library(OBS::webrtc ALIAS obs-webrtc)

target_sources(
  obs-webrtc
  PRIVATE obs-webrtc.cpp whip-output.cpp whip-output.h whip-service.cpp whip-service.h whip-utils.h
          degrade-client.cpp degrade-client.h motion-roi.cpp motion-roi.h quality-score.cpp quality-score.h
)

target_compile_definitions(obs-webrtc PRIVATE WHIP_DEGRADE_ACTIVE)

target_link_libraries(
  obs-webrtc
  PRIVATE OBS::libobs
          LibDataChannel::LibDataChannel
          CURL::libcurl
          FFmpeg::avcodec
          FFmpeg::avutil
          FFmpeg::swscale
)

set_target_properties_obs(obs-webrtc PROPERTIES FOLDER plugins PREFIX "")
