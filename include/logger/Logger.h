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

    // 原有的日志方法（保持不变）
    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warning(const std::string& message);
    static void error(const std::string& message);
    static void fatal(const std::string& message);

    // 新增：带位置信息的日志方法
    static void debugWithLocation(const std::string& message, const char* file, int line, const char* function);
    static void infoWithLocation(const std::string& message, const char* file, int line, const char* function);
    static void warningWithLocation(const std::string& message, const char* file, int line, const char* function);
    static void errorWithLocation(const std::string& message, const char* file, int line, const char* function);
    static void fatalWithLocation(const std::string& message, const char* file, int line, const char* function);

private:
    // 记录日志的主要方法
    static void log(LogLevel level, const std::string& message);

    // 新增：带位置信息的日志记录方法
    static void logWithLocation(LogLevel level, const std::string& message,
                                const char* file, int line, const char* function);

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

    // 提取文件名（去除路径）
    static std::string extractFilename(const char* filepath);

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

// 带位置信息的日志宏定义（使用安全的前缀避免与OpenCV冲突）
#define LOGGER_DEBUG(msg) \
    Logger::debugWithLocation(msg, __FILE__, __LINE__, __FUNCTION__)

#define LOGGER_INFO(msg) \
    Logger::infoWithLocation(msg, __FILE__, __LINE__, __FUNCTION__)

#define LOGGER_WARNING(msg) \
    Logger::warningWithLocation(msg, __FILE__, __LINE__, __FUNCTION__)

#define LOGGER_ERROR(msg) \
    Logger::errorWithLocation(msg, __FILE__, __LINE__, __FUNCTION__)

#define LOGGER_FATAL(msg) \
    Logger::fatalWithLocation(msg, __FILE__, __LINE__, __FUNCTION__)

// 支持格式化字符串的宏（需要C++20或使用sprintf）
#define LOGGER_DEBUG_FMT(fmt, ...) do { \
    char buffer[1024]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    Logger::debugWithLocation(std::string(buffer), __FILE__, __LINE__, __FUNCTION__); \
} while(0)

#define LOGGER_INFO_FMT(fmt, ...) do { \
    char buffer[1024]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    Logger::infoWithLocation(std::string(buffer), __FILE__, __LINE__, __FUNCTION__); \
} while(0)

#define LOGGER_WARNING_FMT(fmt, ...) do { \
    char buffer[1024]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    Logger::warningWithLocation(std::string(buffer), __FILE__, __LINE__, __FUNCTION__); \
} while(0)

#define LOGGER_ERROR_FMT(fmt, ...) do { \
    char buffer[1024]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    Logger::errorWithLocation(std::string(buffer), __FILE__, __LINE__, __FUNCTION__); \
} while(0)

#define LOGGER_FATAL_FMT(fmt, ...) do { \
    char buffer[1024]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    Logger::fatalWithLocation(std::string(buffer), __FILE__, __LINE__, __FUNCTION__); \
} while(0)

// 条件日志宏
#define LOGGER_DEBUG_IF(condition, msg) \
    do { if (condition) LOGGER_DEBUG(msg); } while(0)

#define LOGGER_INFO_IF(condition, msg) \
    do { if (condition) LOGGER_INFO(msg); } while(0)

#define LOGGER_WARNING_IF(condition, msg) \
    do { if (condition) LOGGER_WARNING(msg); } while(0)

#define LOGGER_ERROR_IF(condition, msg) \
    do { if (condition) LOGGER_ERROR(msg); } while(0)

#define LOGGER_FATAL_IF(condition, msg) \
    do { if (condition) LOGGER_FATAL(msg); } while(0)

// 便捷的进入/退出函数日志宏
#define LOGGER_FUNCTION_ENTER() \
    LOGGER_DEBUG("Function entered")

#define LOGGER_FUNCTION_EXIT() \
    LOGGER_DEBUG("Function exited")

// RAII风格的函数跟踪
class FunctionTracker {
public:
    FunctionTracker(const char* file, int line, const char* function)
            : file_(file), line_(line), function_(function) {
        Logger::debugWithLocation("Function entered", file_, line_, function_);
    }

    ~FunctionTracker() {
        Logger::debugWithLocation("Function exited", file_, line_, function_);
    }

private:
    const char* file_;
    int line_;
    const char* function_;
};

#define LOGGER_FUNCTION_TRACE() \
    FunctionTracker __func_tracker(__FILE__, __LINE__, __FUNCTION__)

#endif // LOGGER_H