#pragma once

#ifdef __ANDROID__
#include <android/log.h>
#define YZ_LOG_TAG "YzDac"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, YZ_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, YZ_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, YZ_LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) (std::fprintf(stderr, "I/YzDac: " __VA_ARGS__), std::fputc('\n', stderr))
#define LOGW(...) (std::fprintf(stderr, "W/YzDac: " __VA_ARGS__), std::fputc('\n', stderr))
#define LOGE(...) (std::fprintf(stderr, "E/YzDac: " __VA_ARGS__), std::fputc('\n', stderr))
#endif
