#include "common/StreamConfig.h"
#include "logger/Logger.h"
#include <fstream>
#include <sstream>
#include <utility>
#include <algorithm>
#include <random>

// 使用nlohmann/json库
using json = nlohmann::json;

namespace {
    // 生成随机字符串作为ID
    std::string generateRandomId() {
        static const char alphanum[] =
                "0123456789"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz";

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

        std::string id = "stream_";
        for (int i = 0; i < 8; ++i) {
            id += alphanum[dis(gen)];
        }

        return id;
    }
}

// 静态成员初始化
std::vector<StreamConfig> AppConfig::streamConfigs;
bool AppConfig::logToFile = false;
std::string AppConfig::logFilePath = "ffmpeg_stream_processor.log";
int AppConfig::logLevel = 1; // INFO
int AppConfig::threadPoolSize = 4;
std::map<std::string, std::string> AppConfig::extraOptions;
bool AppConfig::useWatchdog = true;
int AppConfig::watchdogInterval = 5;
std::string AppConfig::dirPath = "/root/data";

// 初始化静态成员
std::vector<MQTTServerConfig> AppConfig::mqttServers;
HTTPServerConfig AppConfig::httpServerConfig;


StreamConfig StreamConfig::createDefault() {
    StreamConfig config;
    config.id = generateRandomId();
    config.inputUrl = "rtsp://example.com/stream";
    config.outputUrl = "rtmp://example.com/live/stream";
    config.outputFormat = "flv";
    config.pushEnabled = true;
    config.aiEnabled = false;
    config.isLocalFile = false;
    config.trackingEnabled = false;
    config.modelType = 1;
    config.videoCodec = "libx264";
    config.audioCodec = "aac";
    config.lowLatencyMode = true;
    config.keyframeInterval = 30;
    config.bufferSize = 1000000;
    config.enableHardwareAccel = true;
    config.hwaccelType = "auto";

    // 设置默认的重连参数
    config.extraOptions["autoStart"] = "true";
    config.extraOptions["maxReconnectAttempts"] = "5";
    config.extraOptions["noDataTimeout"] = "10000";  // 10秒无数据超时
    config.extraOptions["input_reconnect"] = "1";
    config.extraOptions["input_reconnect_streamed"] = "1";
    config.extraOptions["input_reconnect_delay_max"] = "5";

    // 设置看门狗相关参数
    config.extraOptions["watchdogEnabled"] = "true";  // 默认为每个流启用看门狗
    config.extraOptions["watchdogFailThreshold"] = "3";  // 失败3次后触发恢复

    return config;
}

StreamConfig StreamConfig::fromJson(const json& j) {
    StreamConfig config = createDefault();

    // 基本配置
    if (j.contains("id") && j["id"].is_string()) config.id = j["id"];
    if (j.contains("inputUrl") && j["inputUrl"].is_string()) config.inputUrl = j["inputUrl"];
    if (j.contains("outputUrl") && j["outputUrl"].is_string()) config.outputUrl = j["outputUrl"];
    if (j.contains("outputFormat") && j["outputFormat"].is_string()) config.outputFormat = j["outputFormat"];
    if (j.contains("pushEnabled") && j["pushEnabled"].is_boolean()) config.pushEnabled = j["pushEnabled"];
    if (j.contains("aiEnabled") && j["aiEnabled"].is_boolean()) config.aiEnabled = j["aiEnabled"];
    if (j.contains("isLocalFile") && j["isLocalFile"].is_boolean()) config.isLocalFile = j["isLocalFile"];
    if (j.contains("trackingEnabled") && j["trackingEnabled"].is_boolean()) config.trackingEnabled = j["trackingEnabled"];
    if (j.contains("modelType") && j["modelType"].is_number()) config.modelType = j["modelType"];
    if (j.contains("videoCodec") && j["videoCodec"].is_string()) config.videoCodec = j["videoCodec"];
    if (j.contains("audioCodec") && j["audioCodec"].is_string()) config.audioCodec = j["audioCodec"];

    // 数值配置
    if (j.contains("videoBitrate") && j["videoBitrate"].is_number()) config.videoBitrate = j["videoBitrate"];
    if (j.contains("audioBitrate") && j["audioBitrate"].is_number()) config.audioBitrate = j["audioBitrate"];
    if (j.contains("keyframeInterval") && j["keyframeInterval"].is_number()) config.keyframeInterval = j["keyframeInterval"];
    if (j.contains("bufferSize") && j["bufferSize"].is_number()) config.bufferSize = j["bufferSize"];
    if (j.contains("width") && j["width"].is_number()) config.width = j["width"];
    if (j.contains("height") && j["height"].is_number()) config.height = j["height"];

    // 布尔配置
    if (j.contains("lowLatencyMode") && j["lowLatencyMode"].is_boolean()) config.lowLatencyMode = j["lowLatencyMode"];
    if (j.contains("enableHardwareAccel") && j["enableHardwareAccel"].is_boolean()) config.enableHardwareAccel = j["enableHardwareAccel"];

    // 硬件加速类型
    if (j.contains("hwaccelType") && j["hwaccelType"].is_string()) config.hwaccelType = j["hwaccelType"];

    // 处理模型配置
    config.models.clear();

    // 新格式: 模型配置数组
    if (j.contains("models") && j["models"].is_array()) {
        for (const auto& modelJson : j["models"]) {
            config.models.push_back(ModelConfig::fromJson(modelJson));
        }
    }
        // 旧格式兼容: 单一模型类型
    else if (j.contains("modelType") && j["modelType"].is_number()) {
        ModelConfig modelCfg;
        modelCfg.modelType = j["modelType"];
        modelCfg.enabled = true;
        config.models.push_back(modelCfg);
    }
        // 旧格式兼容: 模型类型数组
    else if (j.contains("modelTypes") && j["modelTypes"].is_array()) {
        for (const auto& type : j["modelTypes"]) {
            if (type.is_number()) {
                ModelConfig modelCfg;
                modelCfg.modelType = type.get<int>();
                modelCfg.enabled = true;
                config.models.push_back(modelCfg);
            }
        }
    }

    // 解析额外选项
    if (j.contains("extraOptions") && j["extraOptions"].is_object()) {
        for (auto& [key, value] : j["extraOptions"].items()) {
            if (value.is_string()) {
                config.extraOptions[key] = value;
            } else if (value.is_number_integer()) {
                config.extraOptions[key] = std::to_string(value.get<int>());
            } else if (value.is_boolean()) {
                config.extraOptions[key] = value.get<bool>() ? "true" : "false";
            }
        }
    }

    return config;
}

json StreamConfig::toJson() const {
    json j;

    // 基本配置
    j["id"] = id;
    j["inputUrl"] = inputUrl;
    j["outputUrl"] = outputUrl;
    j["outputFormat"] = outputFormat;
    j["pushEnabled"] = pushEnabled;
    j["aiEnabled"] = aiEnabled;
    j["isLocalFile"] = isLocalFile;
    j["trackingEnabled"] = trackingEnabled;
    j["modelType"] = modelType;
    j["videoCodec"] = videoCodec;
    j["audioCodec"] = audioCodec;
    j["videoBitrate"] = videoBitrate;
    j["audioBitrate"] = audioBitrate;
    j["lowLatencyMode"] = lowLatencyMode;
    j["keyframeInterval"] = keyframeInterval;
    j["bufferSize"] = bufferSize;
    j["enableHardwareAccel"] = enableHardwareAccel;
    j["hwaccelType"] = hwaccelType;
    j["width"] = width;
    j["height"] = height;

    // 添加模型配置数组
    nlohmann::json modelsArray = nlohmann::json::array();
    for (const auto& model : models) {
        modelsArray.push_back(model.toJson());
    }
    j["models"] = modelsArray;

    // 额外选项
    json extraOptionsJson;
    for (const auto& [key, value] : extraOptions) {
        // 尝试将数值字符串转换为实际数值
        if (key == "maxReconnectAttempts" || key == "noDataTimeout" ||
            key == "watchdogFailThreshold" || key.find("_") != std::string::npos) {
            try {
                extraOptionsJson[key] = std::stoi(value);
                continue;
            } catch (...) {
                // 转换失败，按字符串处理
            }
        }

        // 处理布尔值
        if (value == "true" || value == "false") {
            extraOptionsJson[key] = (value == "true");
        } else {
            extraOptionsJson[key] = value;
        }
    }
    j["extraOptions"] = extraOptionsJson;

    return j;
}

bool StreamConfig::validate() const {
    // 检查输入 URL 是否为空（必须提供）
    if (inputUrl.empty()) {
        return false;
    }

    // 仅在推流启用时检查输出 URL 和格式
    if (pushEnabled) {
        if (outputUrl.empty() || outputFormat.empty()) {
            return false;
        }
    }

    // 验证重连相关参数
    if (extraOptions.find("maxReconnectAttempts") != extraOptions.end()) {
        try {
            int attempts = std::stoi(extraOptions.at("maxReconnectAttempts"));
            if (attempts < 0) {
                LOGGER_WARNING("Invalid maxReconnectAttempts value (must be >= 0): " + extraOptions.at("maxReconnectAttempts"));
                return false;
            }
        } catch (const std::exception& e) {
            LOGGER_WARNING("Invalid maxReconnectAttempts format: " + extraOptions.at("maxReconnectAttempts"));
            return false;
        }
    }

    if (extraOptions.find("noDataTimeout") != extraOptions.end()) {
        try {
            int timeout = std::stoi(extraOptions.at("noDataTimeout"));
            if (timeout < 1000) { // 至少1秒
                LOGGER_WARNING("Invalid noDataTimeout value (must be >= 1000 ms): " + extraOptions.at("noDataTimeout"));
                return false;
            }
        } catch (const std::exception& e) {
            LOGGER_WARNING("Invalid noDataTimeout format: " + extraOptions.at("noDataTimeout"));
            return false;
        }
    }

    // 验证看门狗相关参数
    if (extraOptions.find("watchdogFailThreshold") != extraOptions.end()) {
        try {
            int threshold = std::stoi(extraOptions.at("watchdogFailThreshold"));
            if (threshold < 1) {
                LOGGER_WARNING("Invalid watchdogFailThreshold value (must be >= 1): " + extraOptions.at("watchdogFailThreshold"));
                return false;
            }
        } catch (const std::exception& e) {
            LOGGER_WARNING("Invalid watchdogFailThreshold format: " + extraOptions.at("watchdogFailThreshold"));
            return false;
        }
    }

    return true;
}

std::string StreamConfig::toString() const {
    return toJson().dump(2); // 格式化的JSON输出，缩进2个空格
}

// HTTP Server Config implementation
HTTPServerConfig HTTPServerConfig::fromJson(const nlohmann::json& j) {
    HTTPServerConfig config;

    if (j.contains("host") && j["host"].is_string())
        config.host = j["host"];

    if (j.contains("port") && j["port"].is_number_integer())
        config.port = j["port"];

    if (j.contains("connection_timeout") && j["connection_timeout"].is_number_integer())
        config.connectionTimeout = j["connection_timeout"];

    return config;
}

nlohmann::json HTTPServerConfig::toJson() const {
    nlohmann::json j;
    j["host"] = host;
    j["port"] = port;
    j["connection_timeout"] = connectionTimeout;
    return j;
}

// AppConfig 相关方法
bool AppConfig::loadFromFile(const std::string& configFilePath) {
    std::ifstream file(configFilePath);
    if (!file.is_open()) {
        return false;
    }

    try {
        // 解析JSON
        json configJson;
        file >> configJson;
        file.close();

        // 清除现有配置
        streamConfigs.clear();
        extraOptions.clear();

        // 加载常规设置
        if (configJson.contains("general")) {
            auto& general = configJson["general"];

            if (general.contains("logToFile") && general["logToFile"].is_boolean())
                logToFile = general["logToFile"];

            if (general.contains("logFilePath") && general["logFilePath"].is_string())
                logFilePath = general["logFilePath"];

            if (general.contains("logLevel") && general["logLevel"].is_number_integer())
                logLevel = general["logLevel"];

            if (general.contains("threadPoolSize") && general["threadPoolSize"].is_number_integer())
                threadPoolSize = general["threadPoolSize"];

            if (general.contains("useWatchdog") && general["useWatchdog"].is_boolean())
                useWatchdog = general["useWatchdog"];

            if (general.contains("watchdogInterval") && general["watchdogInterval"].is_number_integer())
                watchdogInterval = general["watchdogInterval"];

            if (general.contains("dirPath") && general["dirPath"].is_string())
                dirPath = general["dirPath"];

            // 加载HTTP服务器配置
            if (general.contains("http_server") && general["http_server"].is_object()) {
                httpServerConfig = HTTPServerConfig::fromJson(general["http_server"]);
                LOGGER_INFO("Loaded HTTP server configuration: " + httpServerConfig.host + ":" +
                             std::to_string(httpServerConfig.port));
            }

            // 加载其他额外选项
            if (general.contains("extraOptions") && general["extraOptions"].is_object()) {
                for (auto& [key, value] : general["extraOptions"].items()) {
                    if (value.is_string()) {
                        extraOptions[key] = value;
                    } else if (value.is_number_integer()) {
                        extraOptions[key] = std::to_string(value.get<int>());
                    } else if (value.is_boolean()) {
                        extraOptions[key] = value.get<bool>() ? "true" : "false";
                    }
                }
            }
        }

        // 加载流配置
        if (configJson.contains("streams") && configJson["streams"].is_array()) {
            for (auto& streamJson : configJson["streams"]) {
                StreamConfig config = StreamConfig::fromJson(streamJson);
                if (config.validate()) {
                    streamConfigs.push_back(config);
                }
            }
        }

        if (configJson.contains("mqtt_servers") && configJson["mqtt_servers"].is_array()) {
            mqttServers.clear(); // 清除现有的MQTT服务器配置

            for (auto& serverJson : configJson["mqtt_servers"]) {
                MQTTServerConfig serverConfig = MQTTServerConfig::fromJson(serverJson);
                mqttServers.push_back(serverConfig);
            }

            LOGGER_INFO("Loaded " + std::to_string(mqttServers.size()) + " MQTT server configurations");
        }

        // 输出加载信息
        LOGGER_INFO("Loaded " + std::to_string(streamConfigs.size()) + " stream configurations");
        LOGGER_INFO("Global watchdog settings: useWatchdog=" + std::string(useWatchdog ? "true" : "false") +
                     ", interval=" + std::to_string(watchdogInterval) + "s");

        for (const auto& config : streamConfigs) {
            LOGGER_DEBUG("Loaded stream: " + config.id + " (" + config.inputUrl + " -> " + config.outputUrl + ")");

            // 记录重连和看门狗相关配置
            if (config.extraOptions.find("maxReconnectAttempts") != config.extraOptions.end()) {
                LOGGER_DEBUG("  maxReconnectAttempts: " + config.extraOptions.at("maxReconnectAttempts"));
            }
            if (config.extraOptions.find("noDataTimeout") != config.extraOptions.end()) {
                LOGGER_DEBUG("  noDataTimeout: " + config.extraOptions.at("noDataTimeout") + " ms");
            }
            if (config.extraOptions.find("watchdogEnabled") != config.extraOptions.end()) {
                LOGGER_DEBUG("  watchdogEnabled: " + config.extraOptions.at("watchdogEnabled"));
            }
            if (config.extraOptions.find("watchdogFailThreshold") != config.extraOptions.end()) {
                LOGGER_DEBUG("  watchdogFailThreshold: " + config.extraOptions.at("watchdogFailThreshold"));
            }
        }

        return true;
    }
    catch (const json::exception& e) {
        LOGGER_ERROR("JSON parsing error: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        LOGGER_ERROR("Error loading config file: " + std::string(e.what()));
        return false;
    }
}

bool AppConfig::saveToFile(const std::string& configFilePath) {
    try {
        json configJson;

        // 创建general部分
        json general;
        general["logToFile"] = logToFile;
        general["logFilePath"] = logFilePath;
        general["logLevel"] = logLevel;
        general["threadPoolSize"] = threadPoolSize;
        general["useWatchdog"] = useWatchdog;
        general["watchdogInterval"] = watchdogInterval;
        general["dirPath"] = dirPath;

        // 添加HTTP服务器配置
        general["http_server"] = httpServerConfig.toJson();

        // 添加一些常见的应用设置
        general["monitorInterval"] = 30;
        general["autoRestartStreams"] = true;

        // 添加额外选项
        json extraOptionsJson;
        for (const auto& [key, value] : extraOptions) {
            // 尝试转换数字
            try {
                if (key == "monitorInterval" || key == "periodicReconnectInterval") {
                    extraOptionsJson[key] = std::stoi(value);
                    continue;
                }
            } catch (...) {}

            // 处理布尔值
            if (value == "true" || value == "false") {
                extraOptionsJson[key] = (value == "true");
            } else {
                extraOptionsJson[key] = value;
            }
        }

        if (!extraOptionsJson.empty()) {
            general["extraOptions"] = extraOptionsJson;
        }

        configJson["general"] = general;

        // 添加流配置
        json streamsJson = json::array();
        for (const auto& config : streamConfigs) {
            streamsJson.push_back(config.toJson());
        }
        configJson["streams"] = streamsJson;

        // 写入文件
        std::ofstream file(configFilePath);
        if (!file.is_open()) {
            return false;
        }

        file << configJson.dump(2); // 缩进2个空格的格式化输出
        file.close();

        return true;
    }
    catch (const json::exception& e) {
        LOGGER_ERROR("JSON error while saving config: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        LOGGER_ERROR("Error saving config file: " + std::string(e.what()));
        return false;
    }
}

bool AppConfig::getLogToFile() {
    return logToFile;
}

std::string AppConfig::getLogFilePath() {
    return logFilePath;
}

int AppConfig::getLogLevel() {
    return logLevel;
}

int AppConfig::getThreadPoolSize() {
    return threadPoolSize;
}

const std::map<std::string, std::string>& AppConfig::getExtraOptions() {
    return extraOptions;
}

const std::vector<StreamConfig>& AppConfig::getStreamConfigs() {
    return streamConfigs;
}

bool AppConfig::getUseWatchdog() {
    return useWatchdog;
}

int AppConfig::getWatchdogInterval() {
    return watchdogInterval;
}

std::string AppConfig::getDirPath() {
    return dirPath;
}

void AppConfig::addStreamConfig(const StreamConfig& config) {
    streamConfigs.push_back(config);
}

StreamConfig AppConfig::findStreamConfigById(const std::string& id) {
    auto it = std::find_if(streamConfigs.begin(), streamConfigs.end(),
                           [&id](const StreamConfig& config) {
                               return config.id == id;
                           });

    if (it != streamConfigs.end()) {
        return *it;
    }

    return StreamConfig::createDefault();
}

bool AppConfig::updateStreamConfig(const StreamConfig& config) {
    auto it = std::find_if(streamConfigs.begin(), streamConfigs.end(),
                           [&config](const StreamConfig& c) {
                               return c.id == config.id;
                           });

    if (it != streamConfigs.end()) {
        *it = config;
        return true;
    }

    return false;
}

bool AppConfig::removeStreamConfig(const std::string& id) {
    auto it = std::find_if(streamConfigs.begin(), streamConfigs.end(),
                           [&id](const StreamConfig& config) {
                               return config.id == id;
                           });

    if (it != streamConfigs.end()) {
        streamConfigs.erase(it);
        return true;
    }

    return false;
}

// MQTT配置获取器
const std::vector<MQTTServerConfig>& AppConfig::getMQTTServers() {
    return mqttServers;
}

// HTTP服务器配置获取器
const HTTPServerConfig& AppConfig::getHTTPServerConfig() {
    return httpServerConfig;
}

// MQTTSubscriptionConfig实现
MQTTSubscriptionConfig MQTTSubscriptionConfig::fromJson(const nlohmann::json& j) {
    MQTTSubscriptionConfig config;

    if (j.contains("topic") && j["topic"].is_string())
        config.topic = j["topic"];

    if (j.contains("qos") && j["qos"].is_number_integer())
        config.qos = j["qos"];

    if (j.contains("handler_id") && j["handler_id"].is_string())
        config.handlerId = j["handler_id"];

    return config;
}

nlohmann::json MQTTSubscriptionConfig::toJson() const {
    nlohmann::json j;
    j["topic"] = topic;
    j["qos"] = qos;

    if (!handlerId.empty())
        j["handler_id"] = handlerId;

    return j;
}

// MQTTServerConfig实现
MQTTServerConfig MQTTServerConfig::fromJson(const nlohmann::json& j) {
    MQTTServerConfig config;

    if (j.contains("name") && j["name"].is_string())
        config.name = j["name"];

    if (j.contains("broker_url") && j["broker_url"].is_string())
        config.brokerUrl = j["broker_url"];

    if (j.contains("client_id") && j["client_id"].is_string())
        config.clientId = j["client_id"];

    if (j.contains("username") && j["username"].is_string())
        config.username = j["username"];

    if (j.contains("password") && j["password"].is_string())
        config.password = j["password"];

    if (j.contains("clean_session") && j["clean_session"].is_boolean())
        config.cleanSession = j["clean_session"];

    if (j.contains("keep_alive_interval") && j["keep_alive_interval"].is_number_integer())
        config.keepAliveInterval = j["keep_alive_interval"];

    if (j.contains("auto_reconnect") && j["auto_reconnect"].is_boolean())
        config.autoReconnect = j["auto_reconnect"];

    if (j.contains("reconnect_interval") && j["reconnect_interval"].is_number_integer())
        config.reconnectInterval = j["reconnect_interval"];

    // 解析订阅配置
    if (j.contains("subscriptions") && j["subscriptions"].is_array()) {
        for (auto& subJson : j["subscriptions"]) {
            config.subscriptions.push_back(MQTTSubscriptionConfig::fromJson(subJson));
        }
    }

    return config;
}

nlohmann::json MQTTServerConfig::toJson() const {
    nlohmann::json j;
    j["name"] = name;
    j["broker_url"] = brokerUrl;
    j["client_id"] = clientId;
    j["username"] = username;
    j["password"] = password;
    j["clean_session"] = cleanSession;
    j["keep_alive_interval"] = keepAliveInterval;
    j["auto_reconnect"] = autoReconnect;
    j["reconnect_interval"] = reconnectInterval;

    // 添加订阅配置
    nlohmann::json subs = nlohmann::json::array();
    for (const auto& sub : subscriptions) {
        subs.push_back(sub.toJson());
    }
    j["subscriptions"] = subs;

    return j;
}

ModelConfig ModelConfig::fromJson(const nlohmann::json& j) {
    ModelConfig config;

    if (j.contains("modelType") && j["modelType"].is_number()) {
        config.modelType = j["modelType"];
    }

    if (j.contains("enabled") && j["enabled"].is_boolean()) {
        config.enabled = j["enabled"];
    }

    // 解析模型特定参数
    if (j.contains("params") && j["params"].is_object()) {
        for (auto& [key, value] : j["params"].items()) {
            if (value.is_string()) {
                config.modelParams[key] = value.get<std::string>();
            } else if (value.is_number()) {
                config.modelParams[key] = std::to_string(value.get<double>());
            } else if (value.is_boolean()) {
                config.modelParams[key] = value.get<bool>() ? "true" : "false";
            }
        }
    }

    return config;
}

nlohmann::json ModelConfig::toJson() const {
    nlohmann::json j;
    j["modelType"] = modelType;
    j["enabled"] = enabled;

    // 添加模型特定参数
    nlohmann::json params;
    for (const auto& [key, value] : modelParams) {
        // 尝试将字符串转换为其他类型
        if (value == "true" || value == "false") {
            params[key] = (value == "true");
        } else {
            try {
                // 尝试转换为数字
                double numValue = std::stod(value);
                // 检查是否为整数
                if (numValue == std::floor(numValue)) {
                    params[key] = static_cast<int>(numValue);
                } else {
                    params[key] = numValue;
                }
            } catch (...) {
                // 非数字，保持为字符串
                params[key] = value;
            }
        }
    }
    j["params"] = params;

    return j;
}