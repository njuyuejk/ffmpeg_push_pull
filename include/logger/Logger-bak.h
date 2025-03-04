#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <mutex>
#include <iostream>
#include <fstream>
#include <ctime>

/**
 * @brief 日志级别枚举
 */
enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

/**
 * @brief 日志系统类
 * 提供线程安全的日志记录功能，支持多个日志级别和文件输出
 */
class Logger {
public:
    /**
     * @brief 初始化日志系统
     * @param logToFile 是否输出到文件
     * @param logFilePath 日志文件路径
     * @param minLevel 最小日志级别
     */
    static void init(bool logToFile = false, const std::string& logFilePath = "application.log", LogLevel minLevel = LogLevel::INFO);

    /**
     * @brief 关闭日志系统
     */
    static void shutdown();

    /**
     * @brief 记录日志
     * @param level 日志级别
     * @param message 日志消息
     */
    static void log(LogLevel level, const std::string& message);

    /**
     * @brief 记录调试级别日志
     * @param message 日志消息
     */
    static void debug(const std::string& message);

    /**
     * @brief 记录信息级别日志
     * @param message 日志消息
     */
    static void info(const std::string& message);

    /**
     * @brief 记录警告级别日志
     * @param message 日志消息
     */
    static void warning(const std::string& message);

    /**
     * @brief 记录错误级别日志
     * @param message 日志消息
     */
    static void error(const std::string& message);

    /**
     * @brief 记录致命错误级别日志
     * @param message 日志消息
     */
    static void fatal(const std::string& message);

private:
    static std::mutex logMutex;
    static std::ofstream logFile;
    static bool initialized;
    static bool useFileOutput;
    static LogLevel minimumLevel;
};

#endif // LOGGER_H