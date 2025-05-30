#include "logger/Logger.h"
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(dir) _mkdir(dir)
#define PATH_SEPARATOR "\\"
#else
#include <sys/stat.h>
    #include <unistd.h>
    #define MKDIR(dir) mkdir(dir, 0755)
    #define PATH_SEPARATOR "/"
#endif

// 静态成员初始化
std::mutex Logger::logMutex;
std::ofstream Logger::logFile;
bool Logger::initialized = false;
bool Logger::useFileOutput = false;
LogLevel Logger::minimumLevel = LogLevel::INFO;
std::string Logger::logDirectory = "logs";
std::string Logger::currentLogDate = "";
std::atomic<bool> Logger::isShuttingDown(false);
std::atomic<int> Logger::shutdownPhase(0);

void Logger::init(bool logToFile, const std::string& logDir, LogLevel minLevel) {
    std::lock_guard<std::mutex> lock(logMutex);

    if (initialized) {
        // 如果已经初始化，先关闭以前的文件
        if (useFileOutput && logFile.is_open()) {
            logFile.close();
        }
    }

    useFileOutput = logToFile;
    minimumLevel = minLevel;
    logDirectory = logDir;

    if (useFileOutput) {
        // 确保日志目录存在
        if (!createDirectory(logDirectory)) {
            std::cerr << "Failed to create log directory: " << logDirectory << std::endl;
            useFileOutput = false;
        } else {
            // 清理旧日志
            cleanupOldLogs();

            // 初始化日志文件
            std::string logFilePath = getCurrentLogFilePath();
            logFile.open(logFilePath, std::ios::out | std::ios::app);
            if (!logFile.is_open()) {
                std::cerr << "Failed to open log file: " << logFilePath << std::endl;
                useFileOutput = false;
            }
            else {
                // 记录当前日期
                time_t now = time(nullptr);
                char dateStr[64];
                strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", localtime(&now));
                currentLogDate = dateStr;
            }
        }
    }

    initialized = true;

//    info("Logger initialized");
}

void Logger::shutdown() {
    prepareShutdown();
    finalizeShutdown();
}

bool Logger::createDirectory(const std::string& path) {
    // 对于多级目录，我们需要逐级创建
    std::string currentPath;
    std::string dirPath = path;

    // 替换Windows路径分隔符为标准格式
    std::replace(dirPath.begin(), dirPath.end(), '\\', '/');

    // 分割路径并创建每个目录
    size_t pos = 0;
    while ((pos = dirPath.find('/', pos)) != std::string::npos) {
        currentPath = dirPath.substr(0, pos);
        if (!currentPath.empty() && !fileExists(currentPath)) {
            if (MKDIR(currentPath.c_str()) != 0 && errno != EEXIST) {
                return false;
            }
        }
        pos++;
    }

    // 创建最后一级目录
    if (!dirPath.empty() && !fileExists(dirPath)) {
        if (MKDIR(dirPath.c_str()) != 0 && errno != EEXIST) {
            return false;
        }
    }

    return true;
}

bool Logger::fileExists(const std::string& filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

std::vector<std::string> Logger::getFilesInDirectory(const std::string& directory) {
    std::vector<std::string> files;
    DIR* dir;
    struct dirent* ent;

    if ((dir = opendir(directory.c_str())) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            // 跳过 . 和 ..
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }

            // 获取文件名
            std::string filename = ent->d_name;

            // 检查是否是常规文件（不是目录）
            std::string fullPath = directory + PATH_SEPARATOR + filename;
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                files.push_back(fullPath);
            }
        }
        closedir(dir);
    }

    return files;
}

std::string Logger::getCurrentLogFilePath() {
    time_t now = time(nullptr);
    char dateStr[64];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", localtime(&now));

    // 日志文件格式：logs/log_YYYY-MM-DD.txt
    return logDirectory + PATH_SEPARATOR + "log_" + std::string(dateStr) + ".log";
}

void Logger::cleanupOldLogs() {
    try {
        // 获取日志目录中的所有文件
        std::vector<std::string> allFiles = getFilesInDirectory(logDirectory);

        // 筛选符合日志文件命名格式的文件
        std::vector<std::string> logFiles;
        for (const auto& filePath : allFiles) {
            // 提取文件名
            size_t lastSeparator = filePath.find_last_of("/\\");
            std::string filename = filePath.substr(lastSeparator + 1);

            // 检查文件名是否符合格式：log_YYYY-MM-DD.txt
            if (filename.size() >= 15 && filename.substr(0, 4) == "log_" &&
                filename.substr(filename.size() - 4) == ".txt") {
                logFiles.push_back(filePath);
            }
        }

        // 如果日志文件数量超过最大值，删除最旧的文件
        if (logFiles.size() > MAX_LOG_DAYS) {
            // 按文件名排序，因为使用了YYYY-MM-DD格式，字母序和时间序是一致的
            std::sort(logFiles.begin(), logFiles.end());

            // 删除最旧的日志文件
            size_t filesToDelete = logFiles.size() - MAX_LOG_DAYS;
            for (size_t i = 0; i < filesToDelete; ++i) {
                // 删除文件
                if (remove(logFiles[i].c_str()) != 0) {
                    std::cerr << "Error deleting file: " << logFiles[i] << std::endl;
                } else {
                    // 提取文件名
                    size_t lastSeparator = logFiles[i].find_last_of("/\\");
                    std::string removedFile = logFiles[i].substr(lastSeparator + 1);
                    std::cerr << "Removed old log file: " << removedFile << std::endl;
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error during log cleanup: " << e.what() << std::endl;
    }
}

void Logger::checkAndRotateLogFile() {
    if (!useFileOutput) return;

    // 获取当前日期
    time_t now = time(nullptr);
    char dateStr[64];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", localtime(&now));
    std::string todayDate(dateStr);

    // 如果日期变了，需要切换日志文件
    if (todayDate != currentLogDate) {
        std::lock_guard<std::mutex> lock(logMutex);

        // 关闭当前日志文件
        if (logFile.is_open()) {
            logFile.close();
        }

        // 更新当前日期
        currentLogDate = todayDate;

        // 清理旧日志
        cleanupOldLogs();

        // 打开新的日志文件
        std::string logFilePath = getCurrentLogFilePath();
        logFile.open(logFilePath, std::ios::out | std::ios::app);
        if (!logFile.is_open()) {
            std::cerr << "Failed to open log file: " << logFilePath << std::endl;
            useFileOutput = false;
        }
    }
}

void Logger::log(LogLevel level, const std::string& message) {

    // 如果日志系统正在关闭，只允许致命错误和通过shutdownMessage方法发送的消息
    if (isShuttingDown && shutdownPhase >= 2 && level != LogLevel::FATAL) {
        return; // 在最终关闭阶段，丢弃普通消息
    }

    // 检查日志级别
    if (level < minimumLevel) {
        return;
    }

    // 检查是否需要切换日志文件
    checkAndRotateLogFile();

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

// 添加一个专门用于关闭过程的消息记录方法
void Logger::shutdownMessage(const std::string& message) {
    // 这个方法即使在关闭过程中也会工作
    char timeStr[64];
    time_t now = time(nullptr);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));

    std::string formattedMessage = std::string("[") + timeStr + "][INFO] " + message;

    // 总是打印到控制台
    std::cout << formattedMessage << std::endl;

    // 只有在初始化阶段和准备关闭阶段尝试写入文件
    // 在最终关闭阶段，文件操作由finalizeShutdown直接处理
    if (shutdownPhase < 2) {
        std::unique_lock<std::mutex> lock(logMutex, std::try_to_lock);
        if (lock.owns_lock() && initialized && useFileOutput && logFile.is_open()) {
            logFile << formattedMessage << std::endl;
            logFile.flush();
        }
    }
}

// 第一阶段：准备关闭
void Logger::prepareShutdown() {
    bool expected = false;
    if (isShuttingDown.compare_exchange_strong(expected, true)) {
        shutdownPhase = 1; // 设置为准备关闭阶段

        // 使用新的方法记录关闭准备消息
        shutdownMessage("Logger preparing to shut down - will only accept critical messages");
    }
}

// 第二阶段：最终关闭
void Logger::finalizeShutdown() {
    shutdownPhase = 2; // 设置为最终关闭阶段

    // 使用尝试锁定避免死锁
    std::unique_lock<std::mutex> lock(logMutex, std::try_to_lock);

    if (initialized && useFileOutput && logFile.is_open()) {
        if (lock.owns_lock()) {
            // 先记录关闭开始的消息
            char timeStr[64];
            time_t now = time(nullptr);
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));

            std::string startMsg = std::string("[") + timeStr + "][INFO] Logger finalizing shutdown";
            std::cout << startMsg << std::endl;
            logFile << startMsg << std::endl;

            // 短暂等待，确保所有缓冲区内容都能写入
            logFile.flush();

            // 再记录完成消息
            now = time(nullptr);
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));

            std::string completeMsg = std::string("[") + timeStr + "][INFO] Logger finalizing shutdown completed";
            std::cout << completeMsg << std::endl;
            logFile << completeMsg << std::endl;

            // 最终刷新和关闭
            logFile.flush();
            logFile.close();
        } else {
            // 如果无法获取锁，只打印到控制台
            std::cerr << "WARNING: Could not acquire log mutex for clean shutdown" << std::endl;
            std::cerr << "Logger finalizing shutdown completed (console only)" << std::endl;

            // 等待短暂时间后强制关闭
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            logFile.close(); // 不安全但必要的关闭
        }
    } else {
        // 文件未打开，只记录到控制台
        std::cout << "Logger finalizing shutdown completed (console only)" << std::endl;
    }

    initialized = false;
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