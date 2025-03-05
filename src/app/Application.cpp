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
    Logger::info("Cleaning up application resources...");
    running_ = false;  // 确保主循环终止

    // 记录清理开始时间（用于超时）
    auto cleanupStart = std::chrono::steady_clock::now();
    bool cleanupCompleted = false;

    // 使用单独的线程执行清理，防止潜在的阻塞
    std::thread cleanupThread([this, &cleanupCompleted]() {
        try {
            // 停止看门狗
            if (watchdog_) {
                try {
                    watchdog_->stop();
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    watchdog_.reset();
                    Logger::info("Watchdog stopped and cleaned up");
                } catch (const std::exception& e) {
                    Logger::error("Error stopping watchdog: " + std::string(e.what()));
                } catch (...) {
                    Logger::error("Unknown error stopping watchdog");
                }
            }

            // 等待确保所有线程都注意到应用程序已停止
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // 停止所有流
            if (streamManager_) {
                try {
                    stopAllStreams();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    streamManager_.reset();
                    Logger::info("Stream manager cleaned up");
                } catch (const std::exception& e) {
                    Logger::error("Error cleaning up stream manager: " + std::string(e.what()));
                } catch (...) {
                    Logger::error("Unknown error cleaning up stream manager");
                }
            }

            cleanupCompleted = true;
        } catch (const std::exception& e) {
            Logger::error("Exception during cleanup process: " + std::string(e.what()));
        } catch (...) {
            Logger::error("Unknown exception during cleanup process");
        }
    });

    // 带超时地等待清理完成
    const int CLEANUP_TIMEOUT_SEC = 10;
    auto waitEndTime = cleanupStart + std::chrono::seconds(CLEANUP_TIMEOUT_SEC);

    while (!cleanupCompleted && std::chrono::steady_clock::now() < waitEndTime) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 处理清理超时
    if (!cleanupCompleted) {
        Logger::warning("Cleanup operation timed out, proceeding with forced exit");

        if (cleanupThread.joinable()) {
            cleanupThread.detach();
        }
    } else {
        if (cleanupThread.joinable()) {
            cleanupThread.join();
        }
    }

    // 关闭日志系统
    try {
        Logger::info("Application exit");
        Logger::shutdown();
    } catch (...) {
        std::cerr << "Error shutting down logger system" << std::endl;
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
    const int MAX_RESTART_ATTEMPTS = 20;  // 最大重启尝试次数，超过后将采用更长的间隔
    const int LONG_RETRY_INTERVAL = 60;   // 长重试间隔（秒）

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

        // 检查看门狗状态
        bool unhealthyInWatchdog = false;
        if (watchdog_ && !watchdog_->isTargetHealthy("stream_" + streamId)) {
            int failCount = watchdog_->getTargetFailCount("stream_" + streamId);
            Logger::warning("Stream " + streamId + " unhealthy according to watchdog (failures: " +
                            (failCount >= 0 ? std::to_string(failCount) : "unknown") + ")");
            unhealthyInWatchdog = true;
        }

        // 流有错误、未运行或在看门狗中不健康，尝试重启
        if (hasError || !isRunning || unhealthyInWatchdog) {
            // 初始化重启计数器（如果需要）
            if (restartAttempts.find(streamId) == restartAttempts.end()) {
                restartAttempts[streamId] = 0;
            }

            // 如果超过最大尝试次数，使用更长的重试间隔
            bool usingLongRetryInterval = (restartAttempts[streamId] >= MAX_RESTART_ATTEMPTS);

            // 计算重启延迟（指数退避但有上限）
            int backoffSeconds;
            if (usingLongRetryInterval) {
                // 尝试多次，使用较长的固定间隔
                backoffSeconds = LONG_RETRY_INTERVAL;

                // 仅在进入长重试模式时记录一次
                if (restartAttempts[streamId] == MAX_RESTART_ATTEMPTS) {
                    Logger::warning("Stream " + streamId + " entered long retry mode after " +
                                    std::to_string(MAX_RESTART_ATTEMPTS) + " attempts");
                }
            } else {
                // 标准指数退避，上限为60秒
                backoffSeconds = std::min(60, 1 << std::min(restartAttempts[streamId], 10));
            }

            // 检查上次重启时间，避免过于频繁的重启
            int64_t currentTime = time(nullptr);
            if (lastRestartTime.find(streamId) != lastRestartTime.end() &&
                currentTime - lastRestartTime[streamId] < backoffSeconds) {
                continue; // 还没到重试时间
            }

            // 记录重连尝试
            if (usingLongRetryInterval) {
                Logger::info("Periodic retry for stream: " + streamId + " (in long retry mode)");
            } else {
                Logger::warning("Attempting to reconnect stream: " + streamId + " (attempt "
                                + std::to_string(restartAttempts[streamId] + 1) + ")");
            }

            // 使用异步重连方法替代原有的重启逻辑
            streamManager_->asyncReconnectStream(streamId);

            // 更新重连状态
            lastRestartTime[streamId] = time(nullptr);
            restartAttempts[streamId]++;
        }
    }

    // 处理所有流的周期性重连（包括失败的流）
    if (periodicReconnectInterval_ > 0) {
        int64_t currentTime = time(nullptr);
        if (currentTime - lastPeriodicReconnectTime_ > periodicReconnectInterval_) {
            Logger::info("Performing periodic reconnect check");

            // 保存有错误的流
            std::vector<std::string> errorStreams;
            for (const auto& streamId : streams) {
                if (streamManager_->hasStreamError(streamId)) {
                    errorStreams.push_back(streamId);
                }
            }

            // 重启每个失败的流（使用线程池异步方式）
            for (const auto& streamId : errorStreams) {
                Logger::info("Periodic reconnect for stream with error: " + streamId);
                streamManager_->asyncReconnectStream(streamId);
            }

            lastPeriodicReconnectTime_ = currentTime;
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
