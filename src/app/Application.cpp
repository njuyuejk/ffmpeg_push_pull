#include "app/Application.h"
#include "logger/Logger.h"
#include "ffmpeg_base/FFmpegException.h"
#include "opencv2/opencv.hpp"
#include "common/opencv2avframe.h"
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

// 示例：计算视频帧亮度并保存到文件
void calculateFrameBrightness(const std::string& streamId, const AVFrame* frame, int64_t pts) {
    // 检查帧是否有效
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    cv::Mat dstMat = AVFrameToMat(frame);

    Logger::info("cv mat status is: "+ std::to_string(dstMat.cols) + " height is: " + std::to_string(dstMat.rows));

//    cv::imwrite("D:\\project\\C++\\my\\ffmpeg_push_pull\\cmake-build-debug/test.jpg", dstMat);

    // 计算亮度（使用Y平面的平均值）
    double totalLuma = 0.0;
    int pixelCount = 0;

    // 打印结果
//    Logger::info("Stream " + streamId + " frame at " + std::to_string(pts) +
//                 "ms, average brightness: " + std::to_string(frame->format));

    // 只处理YUV格式的帧
    if (frame->format == AV_PIX_FMT_YUV420P ||
        frame->format == AV_PIX_FMT_YUV422P ||
        frame->format == AV_PIX_FMT_YUV444P ||
        frame->format == AV_PIX_FMT_NV12) {

        // Y平面是亮度
        for (int y = 0; y < frame->height; y++) {
            for (int x = 0; x < frame->width; x++) {
                totalLuma += frame->data[0][y * frame->linesize[0] + x];
                pixelCount++;
            }
        }
    }

    if (pixelCount > 0) {
        double avgLuma = totalLuma / pixelCount;

        // 打印结果
        Logger::info("Stream " + streamId + " frame at " + std::to_string(pts) +
                      "ms, average brightness: " + std::to_string(avgLuma));

        // 将结果保存到文件（追加模式）
        static std::ofstream outFile("brightness_" + streamId + ".csv", std::ios::app);
        if (outFile.is_open()) {
            outFile << pts << "," << avgLuma << std::endl;
        }
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
          configFilePath_("./config.json"),
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

    // 确定配置文件扩展名（支持.ini或.json）
    std::string extension;
    size_t lastDot = configFilePath_.find_last_of(".");
    if (lastDot != std::string::npos) {
        extension = configFilePath_.substr(lastDot);
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    }

    // 加载配置
    if (!AppConfig::loadFromFile(configFilePath_)) {
        std::cerr << "Failed to load config file: " << configFilePath_ << std::endl;
        std::cerr << "Creating a default config file." << std::endl;

        // 创建默认配置并保存
        StreamConfig defaultConfig = StreamConfig::createDefault();
        AppConfig::addStreamConfig(defaultConfig);

        // 根据扩展名选择保存格式
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

    // 初始化http相关
    httpClient_ = std::make_unique<httplib::Client>("127.0.0.1", 9000);
    httpClient_->set_connection_timeout(5);
    httpClient_->set_read_timeout(5);

    if (!httpClient_->Head("/")) {
        Logger::error("http server connect failed");
    }

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

    // 初始化MQTT客户端
    if (!initializeMQTTClients()) {
        Logger::warning("Failed to initialize MQTT clients, continuing without MQTT support");
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

    // 设置自定义帧处理
    setupCustomFrameProcessing();

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

    // 清理mqtt连接
    cleanupMQTTClients();

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
    streams.clear();
    std::vector<std::string>().swap(streams);
}

// 监控流状态
void Application::monitorStreams() {
// 显示当前状态
    listStreams();

    if (!httpClient_->Head("/")) {
        Logger::info("reconnect http server");
        httpClient_ = std::make_unique<httplib::Client>("127.0.0.1", 9000);
        httpClient_->set_connection_timeout(5);
        httpClient_->set_read_timeout(5);
        if (!httpClient_->Head("/")) {
            Logger::error("reconnect http server failed");
        } else {
            Logger::info("reconnect http server success");
        }
    }

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
    streams.clear();
    restartAttempts.clear();
    lastRestartTime.clear();
    std::vector<std::string>().swap(streams);
    std::map<std::string, int>().swap(restartAttempts);
    std::map<std::string, int64_t >().swap(lastRestartTime);
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

void Application::setupCustomFrameProcessing() {
    if (!streamManager_) return;

    std::vector<std::string> streamIds = streamManager_->listStreams();

    for (const auto& streamId : streamIds) {
        // 获取流配置
        StreamConfig config = streamManager_->getStreamConfig(streamId);

        // 如果是仅拉流模式，设置自定义处理
        if (!config.pushEnabled) {
            // 获取StreamProcessor指针
            auto processor = streamManager_->getStreamProcessor(streamId);
            if (processor) {
                // 设置视频帧回调
                processor->setVideoFrameCallback([streamId](const AVFrame* frame, int64_t pts) {
                    calculateFrameBrightness(streamId, frame, pts);
                });

                Logger::info("Custom video frame processing set up for stream: " + streamId);
            }
        }
    }
}

// MQTT客户端初始化
bool Application::initializeMQTTClients() {
    Logger::info("Initializing MQTT clients...");

    // 获取MQTT服务器配置
    const auto& mqttServers = AppConfig::getMQTTServers();

    if (mqttServers.empty()) {
        Logger::info("No MQTT servers configured, skipping MQTT initialization");
        return true;
    }

    // 初始化每个MQTT客户端
    for (const auto& serverConfig : mqttServers) {
        try {
            Logger::info("Initializing MQTT client for server: " + serverConfig.name);

            if (serverConfig.name.empty() || serverConfig.brokerUrl.empty()) {
                Logger::warning("MQTT server name or URL is empty, skipping");
                continue;
            }

            // 如果未提供客户端ID，则生成一个
            std::string clientId = serverConfig.clientId;
            if (clientId.empty()) {
                // 生成随机客户端ID
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(1000, 9999);
                clientId = "ffmpeg_" + std::to_string(dis(gen));
                Logger::info("Generated random client ID: " + clientId);
            }

            // 创建MQTT客户端
            auto client = std::make_shared<MQTTClientWrapper>(
                    serverConfig.brokerUrl,
                    clientId,
                    serverConfig.username,
                    serverConfig.password,
                    serverConfig.cleanSession,
                    serverConfig.keepAliveInterval
            );

            // 设置连接断开回调函数
            client->setConnectionLostCallback([this, serverName = serverConfig.name](const std::string& cause) {
                Logger::warning("MQTT connection lost for server " + serverName + ": " + cause);
                // 自动重连由客户端处理
            });

            // 连接到代理服务器
            if (!client->connect()) {
                Logger::warning("Failed to connect to MQTT broker " + serverConfig.name +
                                " at " + serverConfig.brokerUrl + ", will retry later");
            }

            // 订阅主题
            for (const auto& sub : serverConfig.subscriptions) {
                client->subscribe(sub.topic, sub.qos,
                                  [this, serverName = serverConfig.name, handlerId = sub.handlerId]
                                          (const std::string& topic, const std::string& payload) {
                                      // 处理消息
                                      handleMQTTMessage(serverName, topic, payload);
                                  }
                );
                Logger::info("Subscribed to topic " + sub.topic + " on server " + serverConfig.name);
            }

            // 存储客户端
            mqttClients[serverConfig.name] = client;
            Logger::info("MQTT client for server " + serverConfig.name + " initialized");

        } catch (const std::exception& e) {
            Logger::error("Failed to initialize MQTT client for server " + serverConfig.name + ": " + e.what());
            // Continue with other servers
        }
    }

    return true;
}

void Application::handleMQTTMessage(const std::string& serverName, const std::string& topic, const std::string& payload) {
    Logger::debug("Received MQTT message from server " + serverName + " on topic " + topic);

    try {
        // 尝试解析JSON消息（如果适用）
        nlohmann::json message;
        bool isJson = false;
        try {
            message = nlohmann::json::parse(payload);
            isJson = true;
        } catch (...) {
            // 不是JSON，按纯文本处理
        }

        // 处理不同的主题
        if (topic == "stream/control") {
            // 处理流控制命令
            if (isJson && message.contains("action") && message["action"].is_string()) {
                std::string action = message["action"];

                if (action == "start" && message.contains("stream_id")) {
                    std::string streamId = message["stream_id"];
                    Logger::info("Received command to start stream: " + streamId);

                    // 查找流配置
                    StreamConfig config = AppConfig::findStreamConfigById(streamId);
                    if (config.id.empty()) {
                        Logger::warning("Stream ID not found: " + streamId);
                    } else if (streamManager_) {
                        try {
                            streamManager_->startStream(config);
                            Logger::info("Stream started: " + streamId);
                        } catch (const std::exception& e) {
                            Logger::error("Failed to start stream: " + std::string(e.what()));
                        }
                    }
                }
                else if (action == "stop" && message.contains("stream_id")) {
                    std::string streamId = message["stream_id"];
                    Logger::info("Received command to stop stream: " + streamId);

                    if (streamManager_) {
                        if (streamManager_->stopStream(streamId)) {
                            Logger::info("Stream stopped: " + streamId);
                        } else {
                            Logger::warning("Failed to stop stream: " + streamId);
                        }
                    }
                }
                else if (action == "restart" && message.contains("stream_id")) {
                    std::string streamId = message["stream_id"];
                    Logger::info("Received command to restart stream: " + streamId);

                    if (streamManager_) {
                        streamManager_->asyncReconnectStream(streamId);
                        Logger::info("Stream restart initiated: " + streamId);
                    }
                }
                else if (action == "list_streams") {
                    Logger::info("Received command to list streams");

                    // 发布流列表
                    if (streamManager_) {
                        nlohmann::json response;
                        response["action"] = "stream_list";

                        nlohmann::json streams = nlohmann::json::array();
                        for (const auto& streamId : streamManager_->listStreams()) {
                            nlohmann::json streamInfo;
                            streamInfo["id"] = streamId;
                            streamInfo["running"] = streamManager_->isStreamRunning(streamId);
                            streamInfo["error"] = streamManager_->hasStreamError(streamId);

                            StreamConfig config = streamManager_->getStreamConfig(streamId);
                            streamInfo["input"] = config.inputUrl;
                            streamInfo["output"] = config.outputUrl;

                            streams.push_back(streamInfo);
                        }

                        response["streams"] = streams;

                        // 查找客户端并发布
                        auto it = mqttClients.find(serverName);
                        if (it != mqttClients.end()) {
                            it->second->publish("stream/status", response.dump(), 1);
                            Logger::info("Published stream list to server " + serverName);
                        }
                    }
                }
                else {
                    Logger::warning("Unknown action in stream control message: " + action);
                }
            }
        }
        else if (topic == "system/status") {
            // 处理系统状态请求
            Logger::info("Received system status request");
            publishSystemStatus(serverName);
        }
        else {
            // 处理其他主题或使用通用处理
            Logger::debug("No specific handler for topic: " + topic);
        }
    } catch (const std::exception& e) {
        Logger::error("Error handling MQTT message: " + std::string(e.what()));
    }
}

void Application::publishSystemStatus(const std::string& serverName) {
    // 查找客户端
    auto it = mqttClients.find(serverName);
    if (it == mqttClients.end()) {
        Logger::error("Cannot publish system status: MQTT client not found for server " + serverName);
        return;
    }

    try {
        // 创建状态消息
        nlohmann::json statusMsg;
        statusMsg["timestamp"] = time(nullptr);
        statusMsg["status"] = "running";

        // 添加流信息
        nlohmann::json streams = nlohmann::json::array();
        if (streamManager_) {
            for (const auto& streamId : streamManager_->listStreams()) {
                nlohmann::json streamInfo;
                streamInfo["id"] = streamId;
                streamInfo["running"] = streamManager_->isStreamRunning(streamId);
                streamInfo["error"] = streamManager_->hasStreamError(streamId);

                StreamConfig config = streamManager_->getStreamConfig(streamId);
                streamInfo["input"] = config.inputUrl;
                streamInfo["output"] = config.outputUrl;

                streams.push_back(streamInfo);
            }
        }
        statusMsg["streams"] = streams;

        // 发布状态
        it->second->publish("system/status/response", statusMsg.dump(), 1);
        Logger::info("Published system status to server " + serverName);

    } catch (const std::exception& e) {
        Logger::error("Error publishing system status: " + std::string(e.what()));
    }
}

bool Application::publishToAllServers(const std::string& topic, const std::string& payload, int qos) {
    bool success = true;

    for (auto& [name, client] : mqttClients) {
        try {
            if (client->isConnected()) {
                if (!client->publish(topic, payload, qos)) {
                    Logger::error("Failed to publish message to server " + name);
                    success = false;
                }
            } else {
                Logger::warning("Cannot publish to server " + name + ": not connected");
                success = false;
            }
        } catch (const std::exception& e) {
            Logger::error("Error publishing to server " + name + ": " + std::string(e.what()));
            success = false;
        }
    }

    return success;
}

void Application::cleanupMQTTClients() {
    Logger::info("Cleaning up MQTT clients...");

    for (auto& [name, client] : mqttClients) {
        try {
            client->disconnect();
            client->cleanup();
            Logger::info("MQTT client " + name + " cleaned up");
        } catch (const std::exception& e) {
            Logger::error("Error cleaning up MQTT client " + name + ": " + e.what());
        }
    }

    mqttClients.clear();
}