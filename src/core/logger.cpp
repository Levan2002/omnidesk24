#include "core/logger.h"
#include <chrono>
#include <ctime>

namespace omnidesk {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < level_) return;

    static const char* levelNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&time));

    char msgBuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    // Extract just filename from path
    const char* basename = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') basename = p + 1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    fprintf(stderr, "[%s.%03d] [%s] %s:%d: %s\n",
            timeBuf, static_cast<int>(ms.count()),
            levelNames[static_cast<int>(level)],
            basename, line, msgBuf);
}

} // namespace omnidesk
