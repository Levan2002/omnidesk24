#include "core/logger.h"
#include <chrono>
#include <ctime>
#include <cstdio>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace omnidesk {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

// ---- platform log sink -------------------------------------------------------

#ifdef _WIN32
// Returns a persistent FILE* to %APPDATA%\omnidesk24\omnidesk24.log.
// Falls back to stderr if the file cannot be opened.
static FILE* getLogFile() {
    static FILE* logFile = nullptr;
    static bool  tried   = false;
    if (tried) return logFile;
    tried = true;

    char appdata[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\omnidesk24", appdata);
        CreateDirectoryA(dir, nullptr);   // no-op if it already exists

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\omnidesk24.log", dir);
        logFile = fopen(path, "a");
    }
    return logFile;
}
#endif // _WIN32

// -----------------------------------------------------------------------------

void Logger::log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < level_) return;

    static const char* levelNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) % 1000;

    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&time));

    char msgBuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    // Extract just the filename from the full path
    const char* basename = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') basename = p + 1;
    }

    char lineBuf[1280];
    snprintf(lineBuf, sizeof(lineBuf), "[%s.%03d] [%s] %s:%d: %s\n",
             timeBuf, static_cast<int>(ms.count()),
             levelNames[static_cast<int>(level)],
             basename, line, msgBuf);

    std::lock_guard<std::mutex> lock(mutex_);

#ifdef _WIN32
    // On Windows, write to a log file (and also OutputDebugString so logs
    // are visible in debuggers / DebugView). We do NOT use stderr here
    // because writing to stderr in a WIN32/GUI subsystem app causes Windows
    // to allocate a console window.
    OutputDebugStringA(lineBuf);

    FILE* lf = getLogFile();
    if (lf) {
        fputs(lineBuf, lf);
        fflush(lf);
    }
#else
    fputs(lineBuf, stderr);
#endif
}

} // namespace omnidesk
