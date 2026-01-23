cmake_minimum_required(VERSION 3.16)
project(videowall_monitor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WVM_USE_RGA "Use Rockchip RGA for preprocessing" ON)
option(WVM_USE_MOSQUITTO "Use libmosquitto for MQTT" OFF)

include_directories(include)

add_executable(videowall_monitor
  src/main.cpp
  src/config.cpp
  src/logger.cpp
  src/v4l2_capture.cpp
  src/rga_preproc.cpp
  src/roi.cpp
  src/rknn_mobilenet.cpp
  src/reference_db.cpp
  src/matcher.cpp
  src/anomaly.cpp
  src/mqtt.cpp
  src/pipeline.cpp
)

target_compile_definitions(videowall_monitor PRIVATE
  $<$<BOOL:${WVM_USE_RGA}>:WVM_USE_RGA>
  $<$<BOOL:${WVM_USE_MOSQUITTO}>:WVM_USE_MOSQUITTO>
)

# RKNN
target_link_libraries(videowall_monitor PRIVATE rknn_api)

# V4L2
target_link_libraries(videowall_monitor PRIVATE pthread)

# RGA (optional)
if(WVM_USE_RGA)
  target_link_libraries(videowall_monitor PRIVATE rga)
endif()

# MQTT (optional)
if(WVM_USE_MOSQUITTO)
  target_link_libraries(videowall_monitor PRIVATE mosquitto)
endif()
