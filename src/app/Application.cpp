#include "app/Application.h"
#include "logger/Logger.h"
#include "ffmpeg_base/FFmpegException.h"

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <csignal>

// 全局应用程序引用，用于信号处理
Application* g_applicationInstance = nullptr;

// 信号处理函数
void signalHandlerWrapper(int signal) {
    if (g_applicationInstance) {
        g_applicationInstance->handleSignal(signal);
    }
}

// 单例实现
Application& Application::getInstance() {
    static Application instance;
    return instance;
}

// 构造函数
Application::Application()
        : running_(false),
          configFilePath_("D:/project/C++/my/ffmpeg-new-pull-push/config.ini"),
          monitorIntervalSeconds_(30),
          autoRestartStreams_(true),
          useWatchdog_(true),
          watchdogIntervalSeconds_(5) {

    g_applicationInstance = this;
}

// 初始化应用
bool Application::initialize(const std::string& configFilePath) {
    // 设置配置文件路径
    configFilePath_ = configFilePath;

    // 加载配置
    if (!AppConfig::loadFromFile(configFilePath_)) {
        std::cerr << "Failed to load config file: " << configFilePath_ << std::endl;
        std::cerr << "Creating a default config file." << std::endl;

        // 创建默认配置并保存
        StreamConfig defaultConfig = StreamConfig::createDefault();
        AppConfig::addStreamConfig(defaultConfig);
        AppConfig::saveToFile(configFilePath_);
    }

    // 读取周期性重连设置
    periodicReconnectInterval_ = 0; // 默认禁用
    for (const auto& [key, value] : AppConfig::getExtraOptions()) {
        if (key == "periodicReconnectInterval") {
            periodicReconnectInterval_ = std::stoi(value);
        }
    }

    if (periodicReconnectInterval_ > 0) {
        Logger::info("Periodic reconnect enabled, interval: " +
                     std::to_string(periodicReconnectInterval_) + " seconds");
    }

    // 初始化日志系统
    LogLevel logLevel = static_cast<LogLevel>(AppConfig::getLogLevel());
    Logger::init(AppConfig::getLogToFile(), AppConfig::getLogFilePath(), logLevel);

    // 获取应用程序配置
    monitorIntervalSeconds_ = 30; // 默认30秒

    // 读取高级配置
    for (const auto& [key, value] : AppConfig::getExtraOptions()) {
        if (key == "monitorInterval") {
            monitorIntervalSeconds_ = std::stoi(value);
        } else if (key == "autoRestartStreams") {
            autoRestartStreams_ = (value == "true" || value == "1");
        }
    }

    // 读取看门狗配置
    useWatchdog_ = AppConfig::getUseWatchdog();
    watchdogIntervalSeconds_ = AppConfig::getWatchdogInterval();

    Logger::info("Configuration loaded - watchdog: " + std::string(useWatchdog_ ? "enabled" : "disabled") +
                 ", interval: " + std::to_string(watchdogIntervalSeconds_) + "s");

    // 注册信号处理器
    std::signal(SIGINT, signalHandlerWrapper);
    std::signal(SIGTERM, signalHandlerWrapper);

    // 创建流管理器
    streamManager_ = std::make_unique<MultiStreamManager>(AppConfig::getThreadPoolSize());

    // 设置看门狗
    if (useWatchdog_) {
        try {
            watchdog_ = std::make_unique<Watchdog>(watchdogIntervalSeconds_ * 1000);
            watchdog_->start();

            // 将看门狗设置到流管理器
            streamManager_->setWatchdog(watchdog_.get());

            Logger::info("Watchdog started with interval: " + std::to_string(watchdogIntervalSeconds_) + "s");
        } catch (const std::exception& e) {
            Logger::error("Failed to setup watchdog: " + std::string(e.what()));
            useWatchdog_ = false;
            watchdog_.reset();
        }
    }

    running_ = true;

    Logger::info("Application initialized with config: " + configFilePath_);
    return true;
}

// 运行应用
int Application::run() {
    // 主循环 - 监控所有流
    Logger::info("Starting main loop, press Ctrl+C to exit");

    // 启动所有配置的流
    startAllStreams();

    lastPeriodicReconnectTime_ = time(nullptr);

    // 监控循环
    while (running_) {
        monitorStreams();

        // 延迟
        for (int i = 0; i < monitorIntervalSeconds_ && running_; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    return 0;
}

// 处理信号
void Application::handleSignal(int signal) {
    Logger::info("Received signal: " + std::to_string(signal));
    running_ = false;
}

// 清理应用
void Application::cleanup() {
    Logger::info("Cleaning up application...");
    running_ = false;  // 首先确保主循环终止

    // 停止看门狗
    if (watchdog_) {
        try {
            watchdog_->stop();
            // 停止看门狗后等待片刻，确保其线程有时间终止
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            watchdog_.reset();
            Logger::info("Watchdog stopped and cleaned up");
        } catch (const std::exception& e) {
            Logger::error("Error stopping watchdog: " + std::string(e.what()));
        } catch (...) {
            Logger::error("Unknown error stopping watchdog");
        }
    }

    // 等待一小段时间确保所有线程都注意到应用程序已停止
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 停止所有流
    if (streamManager_) {
        try {
            stopAllStreams();
            // 在释放指针前等待片刻，确保所有后台操作完成
            std::this_thread::sleep_for(std::chrono::seconds(1));
            streamManager_.reset();
            Logger::info("Stream manager cleaned up");
        } catch (const std::exception& e) {
            Logger::error("Error cleaning up stream manager: " + std::string(e.what()));
        } catch (...) {
            Logger::error("Unknown error cleaning up stream manager");
        }
    }

    // 关闭日志系统
    try {
        Logger::shutdown();
    } catch (...) {
        std::cerr << "Error shutting down logger" << std::endl;
    }
}

// 列出所有流
void Application::listStreams() {
    if (!streamManager_) return;

    std::vector<std::string> streams = streamManager_->listStreams();

    Logger::info("Running streams (" + std::to_string(streams.size()) + "):");
    for (const auto& streamId : streams) {
        std::string status = streamManager_->hasStreamError(streamId) ? "ERROR" :
                             (streamManager_->isStreamRunning(streamId) ? "RUNNING" : "STOPPED");

        StreamConfig config = streamManager_->getStreamConfig(streamId);
        Logger::info("  - " + streamId + " [" + status + "] " +
                     config.inputUrl + " -> " + config.outputUrl);
    }
}

// 监控流状态
void Application::monitorStreams() {
    // 显示当前状态
    listStreams();

    // 如果不需要自动重启流，则直接返回
    if (!autoRestartStreams_) {
        return;
    }

    static std::map<std::string, int> restartAttempts;  // 跟踪每个流的重启尝试次数
    static std::map<std::string, int64_t> lastRestartTime;  // 上次重启时间
    const int RESTART_RESET_TIME = 300;   // 5分钟无错误后重置重启计数器

    // 查找并重启出错的流
    std::vector<std::string> streams = streamManager_->listStreams();
    for (const auto& streamId : streams) {
        bool hasError = streamManager_->hasStreamError(streamId);
        bool isRunning = streamManager_->isStreamRunning(streamId);

        // 检查是否应该重置重启计数器
        if (!hasError && isRunning) {
            int64_t currentTime = time(nullptr);
            if (lastRestartTime.find(streamId) != lastRestartTime.end() &&
                currentTime - lastRestartTime[streamId] > RESTART_RESET_TIME) {
                restartAttempts[streamId] = 0;
                Logger::debug("Reset restart counter for stream: " + streamId);
            }
            continue; // 流正常运行，无需处理
        }

        // 检查看门狗状态 (新增)
        bool unhealthyInWatchdog = false;
        if (watchdog_ && !watchdog_->isTargetHealthy("stream_" + streamId)) {
            int failCount = watchdog_->getTargetFailCount("stream_" + streamId);
            Logger::warning("Stream " + streamId + " unhealthy according to watchdog (failures: " +
                            (failCount >= 0 ? std::to_string(failCount) : "unknown") + ")");
            unhealthyInWatchdog = true;
        }

        // 流出错、未运行或在看门狗中不健康，尝试重启
        if (hasError || !isRunning || unhealthyInWatchdog) {
            // 初始化重启计数器
            if (restartAttempts.find(streamId) == restartAttempts.end()) {
                restartAttempts[streamId] = 0;
            }

            // 计算重启延迟（使用指数退避策略，但有上限）
            int backoffSeconds = std::min(60, 1 << std::min(restartAttempts[streamId], 10));

            // 检查最后一次重启时间，确保不会太频繁重启
            int64_t currentTime = time(nullptr);
            if (lastRestartTime.find(streamId) != lastRestartTime.end() &&
                currentTime - lastRestartTime[streamId] < backoffSeconds) {
                continue; // 还没到重试时间
            }

            Logger::warning("Attempting to reconnect stream: " + streamId + " (attempt "
                            + std::to_string(restartAttempts[streamId] + 1) + ")");

            // 停止流
            streamManager_->stopStream(streamId);

            // 等待指定的重启延迟
            std::this_thread::sleep_for(std::chrono::seconds(10));

            // 获取配置并重新启动
            StreamConfig config = AppConfig::findStreamConfigById(streamId);
            if (config.id == streamId) {
                try {
                    streamManager_->startStream(config);
                    Logger::info("Restarted stream: " + streamId);
                    lastRestartTime[streamId] = time(nullptr);
                    restartAttempts[streamId]++;
                } catch (const FFmpegException& e) {
                    Logger::error("Failed to restart stream " + streamId + ": " + e.what());
                    lastRestartTime[streamId] = time(nullptr);
                    restartAttempts[streamId]++;
                }
            } else {
                Logger::error("Could not find configuration for stream: " + streamId);
            }
        }
    }
}

// 启动所有流
void Application::startAllStreams() {
    if (!streamManager_) return;

    const std::vector<StreamConfig>& configs = AppConfig::getStreamConfigs();
    int successCount = 0;

    for (const auto& config : configs) {
        // 检查流是否需要自动启动
        bool autoStart = true;
        for (const auto& [key, value] : config.extraOptions) {
            if (key == "autoStart") {
                autoStart = (value == "true" || value == "1");
                break;
            }
        }

        if (!autoStart) {
            Logger::info("Skipping auto-start for stream: " + config.id + " (autoStart=false)");
            continue;
        }

        try {
            streamManager_->startStream(config);
            Logger::info("Started stream: " + config.id);
            successCount++;
        } catch (const FFmpegException& e) {
            Logger::error("Failed to start stream " + config.id + ": " + e.what());
        }
    }

    // 添加周期性喂养看门狗的代码
    if (watchdog_) {
        watchdog_->feedTarget("application");
    }

    Logger::info("Started " + std::to_string(successCount) + " of " +
                 std::to_string(configs.size()) + " streams");
}

// 停止所有流
void Application::stopAllStreams() {
    if (!streamManager_) return;

    streamManager_->stopAll();
    Logger::info("Stopped all streams");
}
