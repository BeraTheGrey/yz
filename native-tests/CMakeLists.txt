# Host build of the device-independent native code + unit tests.
cmake_minimum_required(VERSION 3.16)
project(yzdac_native_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)
add_executable(yzdac_tests
        tests.cpp
        ${SRC}/uac/descriptors.cpp
        ${SRC}/uac/control.cpp
        ${SRC}/dsp/pcm.cpp
        ${SRC}/dsp/resampler.cpp)
target_include_directories(yzdac_tests PRIVATE ${SRC})
target_compile_options(yzdac_tests PRIVATE -Wall -Wextra -O2 -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(yzdac_tests PRIVATE -fsanitize=address,undefined)
enable_testing()
add_test(NAME yzdac_tests COMMAND yzdac_tests)
