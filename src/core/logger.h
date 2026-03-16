#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace omnidesk {

enum class LogLevel : uint8_t {
    DBG,
    INFO,
    WARN,
    ERR,
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    void log(LogLevel level, const char* file, int line, const char* fmt, ...);

private:
    Logger() = default;
    LogLevel level_ = LogLevel::INFO;
    std::mutex mutex_;
};

#define LOG_DEBUG(...) ::omnidesk::Logger::instance().log(::omnidesk::LogLevel::DBG,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::omnidesk::Logger::instance().log(::omnidesk::LogLevel::INFO, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::omnidesk::Logger::instance().log(::omnidesk::LogLevel::WARN, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::omnidesk::Logger::instance().log(::omnidesk::LogLevel::ERR,  __FILE__, __LINE__, __VA_ARGS__)

} // namespace omnidesk
