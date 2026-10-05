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

# Platform validation (the compiled binary is identical across SoCs; the platform
# only labels the install path).
if(NOT (TARGET_PLATFORM STREQUAL "RK3588" OR TARGET_PLATFORM STREQUAL "RK3568" OR TARGET_PLATFORM STREQUAL "RK3576"))
    message(FATAL_ERROR "Unsupported platform: ${TARGET_PLATFORM}")
endif()

# rknn_api.h comes from the repo include/ (fetched by scripts/prep.sh); stb_image
# comes from the RKNN model zoo in the shared cache toolkit. build_test.sh resolves
# and passes TOOLKIT_DIR; librknnrt is linked from the SDK sysroot below.
if(NOT DEFINED TOOLKIT_DIR)
    message(FATAL_ERROR "TOOLKIT_DIR not set -- invoke this via ./build_test.sh, which resolves the shared cache")
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/../include
    ${TOOLKIT_DIR}/rknn_model_zoo/3rdparty/stb_image
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
