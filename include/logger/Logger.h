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
    static void init(bool logToFile = true, const std::string& logDir = "logs", LogLevel minLevel = LogLevel::INFO);

    // 关闭日志系统
    static void shutdown();

    // 日志方法
    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warning(const std::string& message);
    static void error(const std::string& message);
    static void fatal(const std::string& message);

private:
    // 记录日志的主要方法
    static void log(LogLevel level, const std::string& message);

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

#endif // LOGGER_H