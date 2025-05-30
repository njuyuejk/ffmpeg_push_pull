#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <ctime>
#include <sys/stat.h>
#include <dirent.h>
#include <atomic>

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

struct LogLocation {
    const char* file;
    int line;
    const char* function;

    LogLocation(const char* f, int l, const char* func)
            : file(f), line(l), function(func) {}
};

class Logger {
public:
    // 初始化日志系统
    static void init(bool logToFile = false, const std::string& logDir = "logs", LogLevel minLevel = LogLevel::INFO);

    // 关闭日志系统
    static void shutdown();

    // 第一阶段：准备关闭
    static void prepareShutdown();

    // 第二阶段：最终关闭
    static void finalizeShutdown();

    // 关闭过程中的日志
    static void shutdownMessage(const std::string& message);

    // 带位置信息的静态方法 - 这些方法会自动包含位置信息
    static void debug(const std::string& message, const LogLocation& location = LogLocation(__FILE__, __LINE__, __FUNCTION__));
    static void info(const std::string& message, const LogLocation& location = LogLocation(__FILE__, __LINE__, __FUNCTION__));
    static void warning(const std::string& message, const LogLocation& location = LogLocation(__FILE__, __LINE__, __FUNCTION__));
    static void error(const std::string& message, const LogLocation& location = LogLocation(__FILE__, __LINE__, __FUNCTION__));
    static void fatal(const std::string& message, const LogLocation& location = LogLocation(__FILE__, __LINE__, __FUNCTION__));

    // 不带位置信息的方法（用于特殊情况或向后兼容）
    static void debugPlain(const std::string& message);
    static void infoPlain(const std::string& message);
    static void warningPlain(const std::string& message);
    static void errorPlain(const std::string& message);
    static void fatalPlain(const std::string& message);

private:
    // 记录日志的主要方法
    static void log(LogLevel level, const std::string& message, const LogLocation* location = nullptr);

    // 获取当前日期的日志文件路径
    static std::string getCurrentLogFilePath();

    // 检查并清理旧的日志文件
    static void cleanupOldLogs();

    // 检查并切换日志文件（如果需要）
    static void checkAndRotateLogFile();

    // 创建目录（如果不存在）
    static bool createDirectory(const std::string& path);

    // 检查文件是否存在
    static bool fileExists(const std::string& filename);

    // 获取目录中的所有文件
    static std::vector<std::string> getFilesInDirectory(const std::string& directory);

    // 格式化位置信息
    static std::string formatLocation(const LogLocation& location);

    // 使用原子变量避免竞态条件
    static std::atomic<bool> isShuttingDown;
    static std::atomic<int> shutdownPhase; // 0=正常, 1=准备关闭, 2=最终关闭

    // 静态成员变量
    static std::mutex logMutex;
    static std::ofstream logFile;
    static bool initialized;
    static bool useFileOutput;
    static LogLevel minimumLevel;
    static std::string logDirectory;
    static std::string currentLogDate;
    static const int MAX_LOG_DAYS = 30;
};

// 可选的宏API - 使用更具体的名称避免冲突
#define LOG_DEBUG(message) Logger::debug(message, LogLocation(__FILE__, __LINE__, __FUNCTION__))
#define LOG_INFO(message) Logger::info(message, LogLocation(__FILE__, __LINE__, __FUNCTION__))
#define LOG_WARNING(message) Logger::warning(message, LogLocation(__FILE__, __LINE__, __FUNCTION__))
#define LOG_ERROR(message) Logger::error(message, LogLocation(__FILE__, __LINE__, __FUNCTION__))
#define LOG_FATAL(message) Logger::fatal(message, LogLocation(__FILE__, __LINE__, __FUNCTION__))

// 如果需要不带位置信息的版本，可以定义这个宏
#ifdef LOGGER_NO_LOCATION
// 在这种模式下，所有调用都重定向到Plain版本
#define REDIRECT_TO_PLAIN
#endif

#ifdef REDIRECT_TO_PLAIN
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERROR
#undef LOG_FATAL
#define LOG_DEBUG(message) Logger::debugPlain(message)
#define LOG_INFO(message) Logger::infoPlain(message)
#define LOG_WARNING(message) Logger::warningPlain(message)
#define LOG_ERROR(message) Logger::errorPlain(message)
#define LOG_FATAL(message) Logger::fatalPlain(message)
#endif

#endif // LOGGER_H