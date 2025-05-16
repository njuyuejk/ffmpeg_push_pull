#include "app/Application.h"
#include "logger/Logger.h"
#include "ffmpeg_base/FFmpegException.h"
#include "opencv2/opencv.hpp"
#include "common/opencv2avframe.h"
#include "common/utils.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <csignal>
#include <uuid/uuid.h>

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

    const HTTPServerConfig& httpConfig = AppConfig::getHTTPServerConfig();
    httpClient_ = std::make_unique<httplib::Client>(httpConfig.host, httpConfig.port);
    httpClient_->set_connection_timeout(httpConfig.connectionTimeout);
    isHttpConnect_ = true;

    if (!httpClient_->Head("/")) {
        Logger::error("HTTP server connect failed: " + httpConfig.host + ":" +
                      std::to_string(httpConfig.port));
        isHttpConnect_ = false;
    }

    Logger::info("HTTP server connect info: " + httpConfig.host + ":" + std::to_string(httpConfig.port));

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

    modelPools_.clear();
    std::vector<std::unique_ptr<ModelPoolEntry>>().swap(modelPools_);

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

    singleModelPools_.clear();
    std::vector<std::unique_ptr<SingleModelEntry>>().swap(singleModelPools_);

    // 关闭日志系统
    try {
        // 第一阶段：准备关闭日志
        Logger::prepareShutdown();

        // 使用专门的关闭消息方法记录最后的日志
        Logger::shutdownMessage("Application exit - cleanup completed");

        // 短暂延迟
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 第二阶段：完成日志关闭
        Logger::finalizeShutdown();
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

//    publishSystemStatus("main_server");

    if (!httpClient_->Head("/")) {
        isHttpConnect_ = false;
    }

    if (!isHttpConnect_) {
        Logger::info("reconnect http server");
        const HTTPServerConfig& httpConfig = AppConfig::getHTTPServerConfig();
        httpClient_ = std::make_unique<httplib::Client>(httpConfig.host, httpConfig.port);
        httpClient_->set_connection_timeout(httpConfig.connectionTimeout);
        if (!httpClient_->Head("/")) {
            Logger::error("reconnect http server failed: " + httpConfig.host + ":" +
                          std::to_string(httpConfig.port));
        } else {
            Logger::info("reconnect http server success: " + httpConfig.host + ":" +
                         std::to_string(httpConfig.port));
            isHttpConnect_ = true;
        }
    }

    // 监控MQTT连接
    monitorMQTTConnections();

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

//    test_model();

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
        if (!config.pushEnabled && config.aiEnabled) {
            // 获取StreamProcessor指针
            auto processor = streamManager_->getStreamProcessor(streamId);

            // 模型池子，针对实时检测情况
//            std::unique_ptr<ModelPoolEntry> aiPool = std::make_unique<ModelPoolEntry>();
//            aiPool->pool_ = std::make_unique<ThreadPool>(6);
//            aiPool->streamId = streamId;
//            aiPool->first_ok = true;
//            aiPool->count = 0;
//            aiPool->rknnFlag = 0;
//            aiPool->modelType = config.modelType;
//            aiPool->rkpool = initModel(config.modelType);
//            modelPools_.push_back(std::move(aiPool));

            // 单个模型，针对不需要实时性
            std::unique_ptr<SingleModelEntry> aiPool = std::make_unique<SingleModelEntry>();
            aiPool->streamId = streamId;
            aiPool->count = 0;
            aiPool->modelType = config.modelType;
            aiPool->isEnabled = true;
            aiPool->singleRKModel = initSingleModel(config.modelType);
            singleModelPools_.push_back(std::move(aiPool));

            if (processor) {
                // 设置视频帧回调
                processor->setVideoFrameCallback([this, streamId](const AVFrame* frame, int64_t pts, int fps) {
                    processDelayFrameAI(streamId, frame, pts, fps);
//                    processFrameAI(streamId, frame, pts);
                });

                Logger::info("Custom video frame processing set up for stream: " + streamId);
            }
        }
    }
}

// MQTT客户端初始化
bool Application::initializeMQTTClients() {
    Logger::info("Initializing MQTT clients using MQTTClientManager...");

    // 获取 MQTT 服务器配置
    const auto& mqttServers = AppConfig::getMQTTServers();

    if (mqttServers.empty()) {
        Logger::info("No MQTT servers configured, skipping MQTT initialization");
        return true;
    }

    int successCount = 0;

    // 初始化每个 MQTT 客户端
    for (const auto& serverConfig : mqttServers) {
        try {
            Logger::info("Initializing MQTT client for server: " + serverConfig.name);

            if (serverConfig.name.empty() || serverConfig.brokerUrl.empty()) {
                Logger::warning("MQTT server name or URL is empty, skipping");
                continue;
            }

            // 如果未提供客户端 ID，则生成一个
            std::string clientId = serverConfig.clientId;
            if (clientId.empty()) {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(1000, 9999);
                clientId = "ffmpeg_" + std::to_string(dis(gen));
                Logger::info("Generated random client ID: " + clientId);
            }

            // 使用管理器创建 MQTT 客户端
            bool clientCreated = mqttManager.createClient(
                    serverConfig.name,
                    serverConfig.brokerUrl,
                    clientId,
                    serverConfig.username,
                    serverConfig.password,
                    serverConfig.cleanSession,
                    serverConfig.keepAliveInterval
            );

            if (!clientCreated) {
                Logger::warning("Failed to create MQTT client for server " + serverConfig.name);
                continue;
            }

            // 获取客户端
            auto client = mqttManager.getClient(serverConfig.name);
            if (!client) {
                Logger::warning("Failed to get MQTT client for server " + serverConfig.name);
                continue;
            }

            // 设置连接断开回调函数
            client->setConnectionLostCallback([this, serverName = serverConfig.name](const std::string& cause) {
                Logger::warning("MQTT connection lost for server " + serverName + ": " + cause);
                // 客户端包装器自动处理重连
            });

            // 订阅主题
            for (const auto& sub : serverConfig.subscriptions) {
                // 创建一个将调用我们处理程序的消息回调
                auto messageCallback = [this, serverName = serverConfig.name](
                        const std::string& topic, const std::string& payload) {
                    // 查找并调用适当的处理程序
                    this->handleMQTTMessage(serverName, topic, payload);
                };

                // 订阅主题
                if (client->subscribe(sub.topic, sub.qos, messageCallback)) {
                    Logger::info("Subscribed to topic " + sub.topic + " on server " + serverConfig.name);
                    registerTopicHandler(serverConfig.name, sub.topic,
                                         [this](const std::string& serverName, const std::string& topic, const std::string& payload) {
                                             this->handlePTZControl(serverName, topic, payload);
                                         });
                }
                else {
                    Logger::warning("Failed to subscribe to topic " + sub.topic + " on server " + serverConfig.name);
                }
            }

            successCount++;
            Logger::info("MQTT client for server " + serverConfig.name + " initialized successfully");
        }
        catch (const std::exception& e) {
            Logger::error("Failed to initialize MQTT client for server " + serverConfig.name + ": " + e.what());
            // 继续处理其他服务器
        }
    }

    Logger::info("Initialized " + std::to_string(successCount) + " of " +
                 std::to_string(mqttServers.size()) + " MQTT clients");

    configureMQTTMonitoring();

    return successCount > 0 || mqttServers.empty();
}

// 注册主题处理程序的辅助方法
void Application::registerTopicHandler(const std::string& serverName, const std::string& topic,
                                       std::function<void(const std::string&, const std::string&, const std::string&)> handler) {
    topicHandlers.push_back({serverName, topic, handler});
}

void Application::registerTopicHandler(const std::string& serverName, const std::string& topic,
                          std::function<void(const std::string&, const std::string&, MQTTClient_message&)> handler) {
    topicMsgHandlers.push_back({serverName, topic, handler});
}

void Application::handleMQTTMessage(const std::string& serverName, const std::string& topic, MQTTClient_message& message) {
    Logger::debug("Received MQTT message from server " + serverName + " on topic " + topic);

    try {
        // 查找此特定服务器和主题的处理程序
        bool handlerFound = false;
        for (const auto& handler : topicMsgHandlers) {
            if (handler.serverName == serverName && handler.topic == topic) {
                handler.handler(serverName, topic, message);
                handlerFound = true;
                break;
            }
        }

        // 如果没有找到特定处理程序，进行通用处理
        if (!handlerFound) {
            // 根据需要添加更多通用处理
            Logger::debug("No specific handler found for topic: " + topic);
        }
    }
    catch (const std::exception& e) {
        Logger::error("Error handling MQTT message: " + std::string(e.what()));
    }
}

void Application::handleMQTTMessage(const std::string& serverName, const std::string& topic, const std::string& payload) {
    Logger::debug("Received MQTT message from server " + serverName + " on topic " + topic);

    try {
        // 查找此特定服务器和主题的处理程序
        bool handlerFound = false;
        for (const auto& handler : topicHandlers) {
            if (handler.serverName == serverName && handler.topic == topic) {
                handler.handler(serverName, topic, payload);
                handlerFound = true;
                break;
            }
        }

        // 如果没有找到特定处理程序，进行通用处理
        if (!handlerFound) {
            // 根据需要添加更多通用处理
            Logger::debug("No specific handler found for topic: " + topic);
        }
    }
    catch (const std::exception& e) {
        Logger::error("Error handling MQTT message: " + std::string(e.what()));
    }
}

void Application::handlePTZControl(const std::string& serverName, const std::string& topic, const std::string& payload) {
    try {
        Logger::info("start process ptz message");

        // 模型启停控制
        if (topic == "model/aiControl") {
            for (auto &model : singleModelPools_) {
                if (model->modelType == 1) {
                    model->isEnabled = false;
                }
            }
        }

        if (topic == "test/airesult") {
            AIDataResponse::aiDataResponse message;
            message.ParseFromString(payload);



            std::vector<uchar> imgData(message.data().begin(), message.data().end());
            cv::Mat image = cv::imdecode(imgData, cv::IMREAD_COLOR);

//            cv::Mat rgbImage;
//            cv::cvtColor(image, rgbImage, cv::COLOR_RGB2BGR);

            cv::imwrite("./test_ai.jpg", image);

        }

        // 解析 protobuf 消息
//        PTZControl::PTZControl ptzControl;
//        // 解析 JSON 消息
//        nlohmann::json message;
        try {
//            message = nlohmann::json::parse(payload.c_str());
//            ptzControl.ParseFromString(payload);
//            PTZControl::PTZControl::Control commandValue = ptzControl.control();
//            std::string type_name = PTZControl::PTZControl::Control_Name(commandValue);
//            Logger::info("payload message ptz command is: " + std::to_string(ptzControl.control()) + " name is: " + type_name);
//            std::string path = "/api/front-end/ptz/" + std::string("34020000001320000003") + "/" + std::string("34020000001320000002") +
//                    "?command=" + type_name + "&horizonSpeed=" + std::to_string(ptzControl.horizonspeed()) +
//                    "&verticalSpeed=" + std::to_string(ptzControl.verticalspeed()) + "&zoomSpeed=" + std::to_string(ptzControl.zoomspeed());
//            std::string path = "/user/" + std::string(message["deviceId"]);

            // 处理云台控制相关话题
            std::string path;
            if (topic == "wubarobot/terminal_to_logic_ptz_control") {
                path = PTZControlHandler(payload);
            } else if (topic == "wubarobot/terminal_to_logic_ptz_iris_control") {
                path = IRISControlHandler(payload);
            } else if (topic == "wubarobot/terminal_to_logic_ptz_focus_control") {
                path = FocusControlHandler(payload);
            }

//            if (isHttpConnect_) {
//                auto res = httpClient_->Get(path.c_str());
//                if (res && res->status == 200) {
//                    Logger::info("ptz control request success, response message is: " + res->body);
//                } else {
//                    Logger::info("ptz control request failed");
//                    isHttpConnect_ = false;
//                }
//            }
        }
        catch (...) {
            Logger::warning("Failed to parse JSON payload for stream control message");
            return;
        }

    }
    catch (const std::exception& e) {
        Logger::error("Error processing PTZ control message: " + std::string(e.what()));
    }
}

void Application::handlePTZControl(const std::string& serverName, const std::string& topic, MQTTClient_message& message) {
    try {
        Logger::info("start process ptz message");
        // 解析 JSON 消息
        PTZControl::PTZControl ptzControl;
        try {
            ptzControl.ParseFromArray(message.payload, message.payloadlen);
            Logger::info("payload message is: " + std::to_string(ptzControl.horizonspeed()));
//            std::string path = "/api/front-end/ptz/" + std::string(message["deviceId"]) + "/" + std::string(message["channelId"]) +
//                               "?command=" + std::string(message["command"]) + "&horizonSpeed=" + std::string(message["horizonSpeed"]) +
//                               "&verticalSpeed=" + std::string(message["verticalSpeed"]) + "&zoomSpeed=" + std::string(message["zoomSpeed"]);
            std::string path = "/user/" + std::to_string(ptzControl.control());
            auto res = httpClient_->Get(path.c_str());
            if (res && res->status == 200 ) {
                Logger::info("ptz control request success, response message is: " + res->body);
            } else {
                Logger::info("ptz control request failed");
            }
        }
        catch (...) {
            Logger::warning("Failed to parse JSON payload for stream control message");
            return;
        }

    }
    catch (const std::exception& e) {
        Logger::error("Error processing PTZ control message: " + std::string(e.what()));
    }
}

void Application::publishSystemStatus(const std::string& serverName, const std::string& payload) {
    auto client = mqttManager.getClient(serverName);
    if (!client) {
        Logger::error("Cannot publish system status: MQTT client not found for server " + serverName);
        return;
    }

    try {
//        std::vector<uchar> jpgBuffer;
//        std::vector<int> compression_params;
//        compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
//        compression_params.push_back(90); // JPEG质量(0-100)
//
//        cv::imencode(".jpg", data, jpgBuffer, compression_params);
//
//        AIDataResponse::aiDataResponse aiResult;
//        aiResult.set_name("识别结果");
//        aiResult.set_format("jpg");
//        aiResult.set_data(jpgBuffer.data(), jpgBuffer.size());
//        aiResult.set_airesult("皖B18060");
//
//        std::string serialized_message;
//        if (!aiResult.SerializeToString(&serialized_message)) {
//            Logger::error("序列化消息失败");
//        }

        // 创建状态消息
//        nlohmann::json statusMsg;
//        statusMsg["deviceId"] = "HikVision2";
//        statusMsg["channelId"] = "2"; // 替换为实际版本
//        statusMsg["command"] = "zoomin";
//        statusMsg["horizonSpeed"] = 64;
//        statusMsg["verticalSpeed"] = 64;
//        statusMsg["zoomSpeed"] = 8;

//        Logger::info("publish message is: " + statusMsg.dump());

        // 发布状态
//        client->publish("system/status/response", statusMsg.dump(), 1);
//        client->publish("test/ptz", statusMsg.dump(), 2);
//        client->publish("test/airesult", serialized_message, 2);
        client->publish("wubarobot/inner/ai_event", payload, 2);
        Logger::info("Published system status to server " + serverName);
    }
    catch (const std::exception& e) {
        Logger::error("Error publishing system status: " + std::string(e.what()));
    }
}

bool Application::publishToAllServers(const std::string& topic, const std::string& payload, int qos) {
    bool success = true;
    auto clientNames = mqttManager.listClients();

    for (const auto& name : clientNames) {
        auto client = mqttManager.getClient(name);
        if (client) {
            try {
                if (client->isConnected()) {
                    if (!client->publish(topic, payload, qos)) {
                        Logger::error("Failed to publish message to server " + name);
                        success = false;
                    }
                }
                else {
                    Logger::warning("Cannot publish to server " + name + ": not connected");
                    success = false;
                }
            }
            catch (const std::exception& e) {
                Logger::error("Error publishing to server " + name + ": " + std::string(e.what()));
                success = false;
            }
        }
    }

    return success;
}

void Application::cleanupMQTTClients() {
    Logger::info("Cleaning up MQTT clients...");

    // 停止MQTT监控
    if (mqttMonitoringEnabled_) {
        mqttManager.stopPeriodicHealthCheck();
    }

    mqttManager.cleanup();
}

void Application::configureMQTTMonitoring() {
    // 获取MQTT健康检查参数（可以从配置文件中获取）
    for (const auto& [key, value] : AppConfig::getExtraOptions()) {
        if (key == "mqttHealthCheckInterval") {
            try {
                mqttHealthCheckInterval_ = std::stoi(value);
                Logger::info("MQTT health check interval set to " + value + " seconds");
            } catch (...) {
                Logger::warning("Invalid MQTT health check interval: " + value + ", using default");
            }
        } else if (key == "mqttMonitoringEnabled") {
            mqttMonitoringEnabled_ = (value == "true" || value == "1");
            Logger::info("MQTT monitoring " + std::string(mqttMonitoringEnabled_ ? "enabled" : "disabled"));
        }
    }

    // 配置MQTT客户端管理器
    if (mqttMonitoringEnabled_) {
        // 设置健康检查间隔
        mqttManager.setHealthCheckInterval(mqttHealthCheckInterval_);

        // 设置全局重连策略
        // 启用自动重连, 最大尝试次数不限, 初始延迟2秒, 最大延迟2分钟
        mqttManager.setGlobalReconnectPolicy(true, 0, 2000, 120000);

        // 启动周期性健康检查
        mqttManager.startPeriodicHealthCheck();

        Logger::info("MQTT monitoring configured and started");
    }
}

void Application::monitorMQTTConnections() {
    if (!mqttMonitoringEnabled_) {
        return;
    }

    int64_t currentTime = std::time(nullptr);

    // 如果上次检查时间距离现在超过了检查间隔，执行一次检查
    if (lastMQTTHealthCheckTime_ == 0 || (currentTime - lastMQTTHealthCheckTime_) >= mqttHealthCheckInterval_) {
        // 获取所有MQTT客户端名称
        auto clientNames = mqttManager.listClients();

        // 统计断开连接的客户端
        std::vector<std::string> disconnectedClients;

        // 检查每个客户端的连接状态
        for (const auto& name : clientNames) {
            auto client = mqttManager.getClient(name);
            if (client && !client->isConnected()) {
                disconnectedClients.push_back(name);

                // 获取断开状态持续时间
                int64_t disconnectedTime = client->getStatusDuration();

                // 如果断开时间过长，记录警告
                if (disconnectedTime > 300) { // 5分钟
                    Logger::warning("MQTT client " + name + " disconnected for " +
                                    std::to_string(disconnectedTime) + " seconds");
                }
            }
        }

        // 如果有断开连接的客户端，尝试进行一次健康检查恢复
        if (!disconnectedClients.empty()) {
            Logger::info("Found " + std::to_string(disconnectedClients.size()) +
                         " disconnected MQTT clients, attempting recovery");
            mqttManager.checkAndRecoverClients();
        }

        // 更新最后检查时间
        lastMQTTHealthCheckTime_ = currentTime;
    }
}

std::vector<std::unique_ptr<rknn_lite>> Application::initModel(int modelType) {

    char *model_name;

    if (modelType == 1) {
        model_name = "./model/yolov8-plate.rknn";
    } else if (modelType == 3) {
        model_name = "./model/yolov8n-fire-smoke.rknn";
    } else if (modelType == 4) {
        model_name = "./model/yolov8_relu_person_best.rknn";
    } else if (modelType == 5) {
        model_name = "./model/yolov8n-p2-uav.rknn";
    }

    Logger::info("init model :" + std::string(model_name));

    std::vector<std::unique_ptr<rknn_lite>> rkpool;

    for (int i = 0; i < 6; ++i) {
        auto rknn_ptr = std::make_unique<rknn_lite>(model_name, i % 3, modelType);
        rkpool.push_back(std::move(rknn_ptr));
    }

    return rkpool;
}

std::unique_ptr<rknn_lite> Application::initSingleModel(int modelType) {

    char *model_name;

    if (modelType == 1) {
        model_name = "./model/yolov8-plate.rknn";
    } else if (modelType == 3) {
        model_name = "./model/yolov8n-fire-smoke.rknn";
    } else if (modelType == 4) {
        model_name = "./model/yolov8_relu_person_best.rknn";
    } else if (modelType == 5) {
        model_name = "./model/yolov8n-p2-uav.rknn";
    } else if (modelType == 6) {
        model_name = "./model/yolov8n-meter.rknn";
    }

    Logger::info("init single model :" + std::string(model_name));

    std::unique_ptr<rknn_lite> rknn_ptr = std::make_unique<rknn_lite>(model_name, modelType % 3, modelType);

    return rknn_ptr;
}

// 示例：计算视频帧亮度并保存到文件
void Application::processFrameAI(const std::string& streamId, const AVFrame* frame, int64_t pts, int fps) {

//    return;
    // 检查帧是否有效
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    try {
        cv::Mat srcMat = AVFrameToMat(frame);
        cv::Mat dstMat;
        bool warning = false;
        std::string plateResult = "";
        AIDataResponse::AIEvent aiEvent;

        for (auto &modelPool : modelPools_) {
            if (modelPool->streamId != streamId) {
                continue;
            }

            if (modelPool->count % 3 != 0) {
                modelPool->count++;
                return;
            }

            if (modelPool->first_ok) {
                modelPool->rkpool[modelPool->rknnFlag % 6]->ori_img = srcMat;
                modelPool->futs.push(modelPool->pool_->enqueue(&rknn_lite::interf, &(*modelPool->rkpool[modelPool->rknnFlag % 6])));
            } else {
                modelPool->futs.pop();
                dstMat = modelPool->rkpool[modelPool->rknnFlag % 6]->ori_img;
                warning = modelPool->rkpool[modelPool->rknnFlag % 6]->warning;
                plateResult = modelPool->rkpool[modelPool->rknnFlag % 6]->plateResult;
                modelPool->rkpool[modelPool->rknnFlag % 6]->ori_img = srcMat;
                modelPool->futs.push(modelPool->pool_->enqueue(&rknn_lite::interf, &(*modelPool->rkpool[modelPool->rknnFlag % 6])));

                if (warning) {
                    // 获取当前时间
                    std::time_t now = std::time(nullptr);

                    // 转换为本地时间结构
                    std::tm* localTime = std::localtime(&now);

                    // 创建用于存储格式化时间的字符数组
                    char buffer[80], buffer_day[80];

                    // 格式化时间字符串: 年-月-日 时:分:秒
                    std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", localTime);
                    std::strftime(buffer_day, sizeof(buffer_day), "%Y-%m-%d", localTime);

//                    std::string dirPath = "/root/data/" + std::string(buffer_day);
                    std::string dirPath = AppConfig::getDirPath();
                    if (!dirExists(dirPath)) {
                        Logger::info("目录不存在，正在创建...");
                        if (createDirRecursive(dirPath)) {
                            Logger::info("目录创建成功！");
                        } else {
                            Logger::error("目录创建失败！");
                        }
                    } else {
                        Logger::debug("目录已存在！");
                    }

                    std::string fileName;
                    if (modelPool->modelType == 1) {
                        fileName = "/" + std::string(buffer_day) + "/plate_" + std::string(buffer) + ".jpg";
                        aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Plate);
                    } else if (modelPool->modelType == 3) {
                        fileName = "/" + std::string(buffer_day) + "/fire_smoke_" + std::string(buffer) + ".jpg";
                        aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Fire);
                    } else if (modelPool->modelType == 4) {
                        fileName = "/" + std::string(buffer_day) + "/person_" + std::string(buffer) + ".jpg";
                        aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Person);
                    } else if (modelPool->modelType == 5) {
                        fileName = "/" + std::string(buffer_day) + "/uav_" + std::string(buffer) + ".jpg";
                        aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_UAV);
                    } else {
                        fileName = "/" + std::string(buffer_day) + "/warning_" + std::string(buffer) + ".jpg";
                        aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Person);
                    }

                    std::string filePath = dirPath + fileName;

                    // Logger::info("filename is: "+ fileName);
                    cv::imwrite(filePath, dstMat);

                    uuid_t uuid;
                    char uuid_str[37];

                    uuid_generate(uuid);
                    uuid_unparse_lower(uuid, uuid_str);

                    aiEvent.set_event_id(uuid_str);
                    aiEvent.set_image_path(fileName);
                    AIDataResponse::PlateParam* plateParam = aiEvent.mutable_plate_param();
                    plateParam->set_plate_number(plateResult);

                    std::string serialized_message;
                    if (!aiEvent.SerializeToString(&serialized_message)) {
                        Logger::error("序列化消息失败");
                    }

                    publishSystemStatus("main_server", serialized_message);
                }

                Logger::info("AI识别处理结束，请做后续相关处理");
//                publishSystemStatus("main_server", dstMat);
//                    cv::imwrite("./test.jpg", dstMat);
            }

            modelPool->rknnFlag++;
            modelPool->count++;
            if (modelPool->rknnFlag >= 6) {
                if (modelPool->first_ok) {
                    modelPool->first_ok = false;
                }
                modelPool->rknnFlag = 0;
            }

            if (modelPool->count == 3) {
                modelPool->count = 0;
            }
        }
    } catch (const std::exception& e) {
        Logger::error("ai service is failed, please check model..., error info: " + std::string(e.what()));
    }
//    cv::imwrite("D:\\project\\C++\\my\\ffmpeg_push_pull\\cmake-build-debug/test.jpg", dstMat);
}

void Application::processDelayFrameAI(const std::string &streamId, const AVFrame *frame, int64_t pts, int fps) {
    //    return;
    // 检查帧是否有效
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    try {
        cv::Mat srcMat = AVFrameToMat(frame);
        cv::Mat dstMat, segAddMask;
        bool warning = false;
        std::string plateResult = "";
        AIDataResponse::AIEvent aiEvent;

        for (auto &modelPool : singleModelPools_) {
            if (modelPool->streamId != streamId) {
                continue;
            }

            if (modelPool->count % fps != 0) {
                modelPool->count++;
                return;
            }

            modelPool->singleRKModel->ori_img = srcMat;
            modelPool->singleRKModel->startValue = 0.0;
            modelPool->singleRKModel->endValue = 1.6;
            modelPool->singleRKModel->interf();
            dstMat = modelPool->singleRKModel->ori_img;
            warning = modelPool->singleRKModel->warning;
            plateResult = modelPool->singleRKModel->plateResult;

            if (modelPool->modelType == 7) {
                segAddMask = modelPool->singleRKModel->SrcAddMask;
            }

            Logger::debug("各个状态结果: "+ std::to_string(warning) + " 时间次数: " + std::to_string(warningFlag) + " 时间数量: " + std::to_string(timeCount));

            if (warning && !warningFlag) {

                // 获取当前时间
                std::time_t now = std::time(nullptr);

                // 转换为本地时间结构
                std::tm* localTime = std::localtime(&now);

                // 创建用于存储格式化时间的字符数组
                char buffer[80], buffer_day[80];

                // 格式化时间字符串: 年-月-日 时:分:秒
                std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", localTime);
                std::strftime(buffer_day, sizeof(buffer_day), "%Y-%m-%d", localTime);

//                    std::string dirPath = "/root/data/" + std::string(buffer_day);
                std::string dirPath = AppConfig::getDirPath() + "/" + std::string(buffer_day);
                std::string tempPath = AppConfig::getDirPath();
                if (!dirExists(dirPath)) {
                    Logger::info("目录不存在，正在创建...");
                    if (createDirRecursive(dirPath)) {
                        Logger::info("目录创建成功！");
                    } else {
                        Logger::error("目录创建失败！");
                    }
                } else {
                    Logger::debug("目录已存在！");
                }

                std::string fileName;
                if (modelPool->modelType == 1) {
                    fileName = "/" + std::string(buffer_day) + "/plate_" + std::string(buffer) + ".jpg";
                    aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Plate);
                } else if (modelPool->modelType == 3) {
                    fileName = "/" + std::string(buffer_day) + "/fire_smoke_" + std::string(buffer) + ".jpg";
                    aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Fire);
                } else if (modelPool->modelType == 4) {
                    fileName = "/" + std::string(buffer_day) + "/person_" + std::string(buffer) + ".jpg";
                    aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Person);
                } else if (modelPool->modelType == 5) {
                    fileName = "/" + std::string(buffer_day) + "/uav_" + std::string(buffer) + ".jpg";
                    aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_UAV);
                } else if (modelPool->modelType == 6) {
                    fileName = "/" + std::string(buffer_day) + "/meter_" + std::string(buffer) + ".jpg";
                    aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Person);
                } else if (modelPool->modelType == 7) {
                    fileName = "/" + std::string(buffer_day) + "/crack_" + std::string(buffer) + ".jpg";
                    aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Person);
                } else {
                    fileName = "/" + std::string(buffer_day) + "/warning_" + std::string(buffer) + ".jpg";
                    aiEvent.set_event_type(AIDataResponse::AIEvent_EventType_Person);
                }

                std::string filePath = tempPath + fileName;

                // Logger::info("filename is: "+ fileName);

                if (modelPool->modelType == 7) {
                    cv::imwrite(filePath, segAddMask);
                } else {
                    cv::imwrite(filePath, dstMat);
                }

                uuid_t uuid;
                char uuid_str[37];

                uuid_generate(uuid);
                uuid_unparse_lower(uuid, uuid_str);

                aiEvent.set_event_id(uuid_str);
                aiEvent.set_image_path(fileName);
                AIDataResponse::PlateParam* plateParam = aiEvent.mutable_plate_param();
                plateParam->set_plate_number(plateResult);

                std::string serialized_message;
                if (!aiEvent.SerializeToString(&serialized_message)) {
                    Logger::error("序列化消息失败");
                }

                publishSystemStatus("main_server", serialized_message);

                // 报警以后，间隔时长进行报警
                warningFlag = true;
                timeCount = 1;
            } else if (warningFlag && warning) {

                Logger::info("AI处理结束, 处于报警间隔期间...");

                timeCount++;
                if (timeCount > 5) {
                    // 重置状态 - 根据需求可以选择是否重置
                    warningFlag = false;
                    timeCount = 0;
                }

            } else {
                Logger::info("AI处理结束, 无报警发生...");
            }

            modelPool->count++;
            if (modelPool->count == fps) {
                modelPool->count = 0;
            }

            Logger::info("AI识别处理结束，请做后续相关处理");
//            publishSystemStatus("main_server", dstMat);
//            cv::imwrite("./test.jpg", dstMat);
        }
    } catch (const std::exception& e) {
        Logger::error("ai service is failed, please check model..., error info: " + std::string(e.what()));
    }
//    cv::imwrite("D:\\project\\C++\\my\\ffmpeg_push_pull\\cmake-build-debug/test.jpg", dstMat);
}

void Application::test_model() {
    cv::Mat srcMat = cv::imread("./test3.jpg", 1);
    cv::Mat dstMat;

    char *modelNmae = "./model/yolov8-plate.rknn";

    auto rkmodel = std::make_unique<rknn_lite>(modelNmae, 0, 1);

//    rknn_lite *rkmodel = new rknn_lite(modelNmae, 0, 1);

    rkmodel->ori_img = srcMat;
    rkmodel->interf();
}