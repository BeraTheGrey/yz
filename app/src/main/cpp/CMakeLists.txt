cmake_minimum_required(VERSION 3.22.1)
project(yzdac C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- libusb -----------------------------------------------------------------
# Normally provided as a git submodule (app/src/main/cpp/libusb). If the
# checkout was made without --recursive, it is downloaded at configure time.
set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libusb)
if(NOT EXISTS ${LIBUSB_DIR}/libusb/core.c)
    include(FetchContent)
    FetchContent_Declare(libusb_src
            GIT_REPOSITORY https://github.com/libusb/libusb.git
            GIT_TAG v1.0.27
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libusb_src)
    set(LIBUSB_DIR ${libusb_src_SOURCE_DIR})
endif()

add_library(usb1 STATIC
        ${LIBUSB_DIR}/libusb/core.c
        ${LIBUSB_DIR}/libusb/descriptor.c
        ${LIBUSB_DIR}/libusb/hotplug.c
        ${LIBUSB_DIR}/libusb/io.c
        ${LIBUSB_DIR}/libusb/sync.c
        ${LIBUSB_DIR}/libusb/strerror.c
        ${LIBUSB_DIR}/libusb/os/linux_usbfs.c
        ${LIBUSB_DIR}/libusb/os/linux_netlink.c
        ${LIBUSB_DIR}/libusb/os/events_posix.c
        ${LIBUSB_DIR}/libusb/os/threads_posix.c)
target_include_directories(usb1
        PRIVATE ${LIBUSB_DIR}/android ${LIBUSB_DIR}/libusb/os
        PUBLIC ${LIBUSB_DIR}/libusb)
target_compile_options(usb1 PRIVATE -fvisibility=hidden -Wno-unused-parameter)

# ---- yzdac ------------------------------------------------------------------
add_library(yzdac SHARED
        uac/descriptors.cpp
        uac/control.cpp
        dsp/pcm.cpp
        dsp/resampler.cpp
        engine/iso_stream.cpp
        engine/dac_device.cpp
        jni/jni_bridge.cpp)
target_compile_options(yzdac PRIVATE -Wall -Wextra -O2)
find_library(log-lib log)
target_link_libraries(yzdac usb1 ${log-lib})
# 16 KB page size support (Android 15+ devices)
target_link_options(yzdac PRIVATE "-Wl,-z,max-page-size=16384")
