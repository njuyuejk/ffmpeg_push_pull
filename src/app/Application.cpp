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
          autoRestartStreams_(true) {

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

    // 注册信号处理器
    std::signal(SIGINT, signalHandlerWrapper);
    std::signal(SIGTERM, signalHandlerWrapper);

    // 创建流管理器
    streamManager_ = std::make_unique<MultiStreamManager>(AppConfig::getThreadPoolSize());

    running_ = true;

    Logger::info("Application initialized with config: " + configFilePath_);
    return true;
}

// 运行应用
int Application::run() {
    // 主循环 - 监控所有流
    Logger::info("Starting main loop, press Ctrl+C to exit");

//    std::mutex mtx;

    // 启动所有配置的流
    startAllStreams();

    // 启动一个线程等待用户输入来决定何时停止
//    std::thread control_thread([&](){
//        std::string cmd;
//        while (true) {
//            std::cin >> cmd;
//            if (cmd == "q" || cmd == "quit") {
//                {
//                    std::lock_guard<std::mutex> lock(mtx);
//                    running_ = false;
//                }
//                break;
//            }
//        }
//    });

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

    // 停止所有流
    if (streamManager_) {
        stopAllStreams();
    }

    // 清理流管理器
    streamManager_.reset();

    // 关闭日志系统
    Logger::shutdown();

    running_ = false;
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

    // 查找并重启出错的流
    std::vector<std::string> streams = streamManager_->listStreams();
    for (const auto& streamId : streams) {
        printf("attempt restart stream\n");
        printf("%d\n", streamManager_->hasStreamError(streamId));
        printf("%d\n", !streamManager_->isStreamRunning(streamId));
        if (streamManager_->hasStreamError(streamId) && !streamManager_->isStreamRunning(streamId)) {
            printf("start stream\n");
            Logger::warning("Detected error in stream: " + streamId + ", attempting restart");

            // 停止流
            streamManager_->stopStream(streamId);

            // 获取配置并重新启动
            StreamConfig config = AppConfig::findStreamConfigById(streamId);
            if (config.id == streamId) {
                try {
                    streamManager_->startStream(config);
                    Logger::info("Restarted stream: " + streamId);
                } catch (const FFmpegException& e) {
                    Logger::error("Failed to restart stream " + streamId + ": " + e.what());
                }
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
                autoStart = value.c_str();
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

    Logger::info("Started " + std::to_string(successCount) + " of " +
                 std::to_string(configs.size()) + " streams");
}

// 停止所有流
void Application::stopAllStreams() {
    if (!streamManager_) return;

    streamManager_->stopAll();
    Logger::info("Stopped all streams");
}