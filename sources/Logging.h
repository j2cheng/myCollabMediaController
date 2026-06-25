// Internal logging helpers for mediacontroller .cpp files.
//
// Each .cpp must #define LOG_TAG to a string literal BEFORE including:
//
//     #define LOG_TAG "MyComponent"
//     #include "Logging.h"
//
// Then use the existing macros (call initLogging() once before logging):
//
//     initLogging();
//     LogInfo (logCategory, "msg fmt=%d", 42);
//     LogWarning(logCategory, "...");
//     LogError(logCategory, "...");
//
// When USE_FOUNDATION is defined, logs go through Foundation::Log under a
// category named LOG_TAG. Otherwise they go to std::cout / std::cerr with a
// "[LEVEL] [LOG_TAG] ..." prefix.
//
// This header is .cpp-only: it relies on the anonymous namespace + macros and
// must not be included from any public header.
#pragma once

#ifndef LOG_TAG
#error "LOG_TAG must be defined as a string literal before including Logging.h"
#endif

#ifdef USE_FOUNDATION

#include "Logging/Log.hpp"
#include <atomic>

namespace {
::Foundation::Log::Category logCategory;
::std::atomic<bool> logInitialized{false};
inline void initLogging() {
    if (!logInitialized.exchange(true)) {
        logCategory = ::Foundation::Log::GetInstance().AddCategory(LOG_TAG);
    }
}
} // namespace

#else // !USE_FOUNDATION

#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <ostream>

namespace {
inline void mediacontrollerFallbackLog(std::ostream& os, const char* level,
                                       const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    os << "[" << level << "] [" << LOG_TAG << "] " << buf << std::endl;
}
inline void initLogging() {}
} // namespace

#define LogInfo(cat, ...)    mediacontrollerFallbackLog(::std::cout, "INFO",  __VA_ARGS__)
#define LogWarning(cat, ...) mediacontrollerFallbackLog(::std::cout, "WARN",  __VA_ARGS__)
#define LogError(cat, ...)   mediacontrollerFallbackLog(::std::cerr, "ERROR", __VA_ARGS__)

#endif // USE_FOUNDATION
