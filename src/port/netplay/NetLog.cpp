#include "NetLog.h"

#include <cstdarg>
#include <cstdio>

#include <spdlog/spdlog.h>

void NetLog(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    printf("[Netplay] %s\n", message);
    // The game exits with _Exit(), which skips flushing stdio.
    fflush(stdout);
    SPDLOG_INFO("[Netplay] {}", message);
}
