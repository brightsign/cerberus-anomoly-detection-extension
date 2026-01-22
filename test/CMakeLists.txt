cmake_minimum_required(VERSION 3.10)
project(mobilenet_test)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

# Set C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set target platform
if(NOT DEFINED TARGET_PLATFORM)
    set(TARGET_PLATFORM "RK3588")
endif()

message(STATUS "Building for platform: ${TARGET_PLATFORM}")

# Platform-specific paths
if(TARGET_PLATFORM STREQUAL "RK3588")
    set(RKNN_API_PATH "${CMAKE_SOURCE_DIR}/../toolkit/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api")
elseif(TARGET_PLATFORM STREQUAL "RK3568")
    set(RKNN_API_PATH "${CMAKE_SOURCE_DIR}/../toolkit/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api")
elseif(TARGET_PLATFORM STREQUAL "RK3576")
    set(RKNN_API_PATH "${CMAKE_SOURCE_DIR}/../toolkit/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api")
else()
    message(FATAL_ERROR "Unsupported platform: ${TARGET_PLATFORM}")
endif()

# Include directories
include_directories(
    ${RKNN_API_PATH}/include
    ${CMAKE_SOURCE_DIR}/../toolkit/rknn_model_zoo/3rdparty/stb_image
)

# Link directories
link_directories(
    ${CMAKE_SYSROOT}/usr/lib
)

# Source files
set(SOURCES
    test_mobilenet.cpp
)

# Create executable
add_executable(test_mobilenet ${SOURCES})

# Link libraries
target_link_libraries(test_mobilenet
    rknnrt
    m
)

# Install
install(TARGETS test_mobilenet DESTINATION .)
