#include "logger/Logger.h"

// 静态成员初始化
std::mutex Logger::logMutex;
std::ofstream Logger::logFile;
bool Logger::initialized = true;
bool Logger::useFileOutput = true;
LogLevel Logger::minimumLevel = LogLevel::INFO;

void Logger::init(bool logToFile, const std::string& logFilePath, LogLevel minLevel) {
    std::lock_guard<std::mutex> lock(logMutex);

    if (initialized) {
        // 如果已经初始化，先关闭以前的文件
        if (useFileOutput && logFile.is_open()) {
            logFile.close();
        }
    }
    printf("3333, %s\n", logFilePath.c_str());

    useFileOutput = logToFile;
    minimumLevel = minLevel;

    if (useFileOutput) {
        logFile.open(logFilePath, std::ios::out | std::ios::app);
        if (!logFile.is_open()) {
            std::cerr << "Failed to open log file: " << logFilePath << std::endl;
            useFileOutput = false;
        }
    }

    printf("2222222222\n");

    initialized = true;

//    Logger::info("Logger initialized");
    printf("5555555\n");
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(logMutex);

    if (initialized && useFileOutput && logFile.is_open()) {
        info("Logger shutting down");
        logFile.close();
    }

    initialized = false;
}

void Logger::log(LogLevel level, const std::string& message) {
    // 检查日志级别
    if (level < minimumLevel) {
        return;
    }

    std::lock_guard<std::mutex> lock(logMutex);

    // 获取当前时间
    char timeStr[64];
    time_t now = time(nullptr);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));

    // 转换日志级别为字符串
    const char* levelStr;
    switch (level) {
        case LogLevel::DEBUG:   levelStr = "DEBUG"; break;
        case LogLevel::INFO:    levelStr = "INFO"; break;
        case LogLevel::WARNING: levelStr = "WARNING"; break;
        case LogLevel::ERROR:   levelStr = "ERROR"; break;
        case LogLevel::FATAL:   levelStr = "FATAL"; break;
        default:                levelStr = "UNKNOWN";
    }

    // 格式化日志消息
    std::string formattedMessage =
            std::string("[") + timeStr + "][" + levelStr + "] " + message;

    // 输出到控制台
    if (level == LogLevel::ERROR || level == LogLevel::FATAL) {
        std::cerr << formattedMessage << std::endl;
    } else {
        std::cout << formattedMessage << std::endl;
    }

    // 输出到文件
    if (initialized && useFileOutput && logFile.is_open()) {
        logFile << formattedMessage << std::endl;
        logFile.flush();
    }
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void Logger::fatal(const std::string& message) {
    log(LogLevel::FATAL, message);
}