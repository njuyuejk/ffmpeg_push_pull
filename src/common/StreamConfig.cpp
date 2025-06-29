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

// 新增：跟踪相关静态成员初始化
GlobalTrackingConfig AppConfig::globalTrackingConfig;
std::vector<TrackingStreamConfig> AppConfig::trackingStreamConfigs;

// TrackingConfig 实现
TrackingConfig TrackingConfig::fromJson(const nlohmann::json& j) {
    TrackingConfig config;

    if (j.contains("tracker_type") && j["tracker_type"].is_string()) {
        config.trackerType = j["tracker_type"];
    }

    if (j.contains("detection_interval") && j["detection_interval"].is_number()) {
        config.detectionInterval = j["detection_interval"];
    }

    if (j.contains("max_targets") && j["max_targets"].is_number()) {
        config.maxTargets = j["max_targets"];
    }

    if (j.contains("max_lost_frames") && j["max_lost_frames"].is_number()) {
        config.maxLostFrames = j["max_lost_frames"];
    }

    if (j.contains("min_detection_confidence") && j["min_detection_confidence"].is_number()) {
        config.minDetectionConfidence = j["min_detection_confidence"];
    }

    if (j.contains("enable_trajectory") && j["enable_trajectory"].is_boolean()) {
        config.enableTrajectory = j["enable_trajectory"];
    }

    if (j.contains("trajectory_length") && j["trajectory_length"].is_number()) {
        config.trajectoryLength = j["trajectory_length"];
    }

    if (j.contains("overlap_threshold") && j["overlap_threshold"].is_number()) {
        config.overlapThreshold = j["overlap_threshold"];
    }

    if (j.contains("enable_visualization") && j["enable_visualization"].is_boolean()) {
        config.enableVisualization = j["enable_visualization"];
    }

    if (j.contains("enable_callback") && j["enable_callback"].is_boolean()) {
        config.enableCallback = j["enable_callback"];
    }

    if (j.contains("callback_interval") && j["callback_interval"].is_number()) {
        config.callbackInterval = j["callback_interval"];
    }

    return config;
}

nlohmann::json TrackingConfig::toJson() const {
    nlohmann::json j;

    j["tracker_type"] = trackerType;
    j["detection_interval"] = detectionInterval;
    j["max_targets"] = maxTargets;
    j["max_lost_frames"] = maxLostFrames;
    j["min_detection_confidence"] = minDetectionConfidence;
    j["enable_trajectory"] = enableTrajectory;
    j["trajectory_length"] = trajectoryLength;
    j["overlap_threshold"] = overlapThreshold;
    j["enable_visualization"] = enableVisualization;
    j["enable_callback"] = enableCallback;
    j["callback_interval"] = callbackInterval;

    return j;
}

// AIModelConfig 实现
AIModelConfig AIModelConfig::fromJson(const nlohmann::json& j) {
    AIModelConfig config;

    if (j.contains("model_path") && j["model_path"].is_string()) {
        config.modelPath = j["model_path"];
    }

    if (j.contains("confidence_threshold") && j["confidence_threshold"].is_number()) {
        config.confidenceThreshold = j["confidence_threshold"];
    }

    if (j.contains("nms_threshold") && j["nms_threshold"].is_number()) {
        config.nmsThreshold = j["nms_threshold"];
    }

    if (j.contains("input_width") && j["input_width"].is_number()) {
        config.inputWidth = j["input_width"];
    }

    if (j.contains("input_height") && j["input_height"].is_number()) {
        config.inputHeight = j["input_height"];
    }

    if (j.contains("class_names") && j["class_names"].is_array()) {
        for (const auto& name : j["class_names"]) {
            if (name.is_string()) {
                config.classNames.push_back(name);
            }
        }
    }

    if (j.contains("enable_gpu") && j["enable_gpu"].is_boolean()) {
        config.enableGPU = j["enable_gpu"];
    }

    if (j.contains("custom_params") && j["custom_params"].is_object()) {
        for (auto& [key, value] : j["custom_params"].items()) {
            if (value.is_string()) {
                config.customParams[key] = value;
            } else if (value.is_number()) {
                config.customParams[key] = std::to_string(value.get<double>());
            } else if (value.is_boolean()) {
                config.customParams[key] = value.get<bool>() ? "true" : "false";
            }
        }
    }

    return config;
}

nlohmann::json AIModelConfig::toJson() const {
    nlohmann::json j;

    j["model_path"] = modelPath;
    j["confidence_threshold"] = confidenceThreshold;
    j["nms_threshold"] = nmsThreshold;
    j["input_width"] = inputWidth;
    j["input_height"] = inputHeight;
    j["class_names"] = classNames;
    j["enable_gpu"] = enableGPU;

    nlohmann::json customParamsJson;
    for (const auto& [key, value] : customParams) {
        // 尝试转换为其他类型
        if (value == "true" || value == "false") {
            customParamsJson[key] = (value == "true");
        } else {
            try {
                double numValue = std::stod(value);
                if (numValue == std::floor(numValue)) {
                    customParamsJson[key] = static_cast<int>(numValue);
                } else {
                    customParamsJson[key] = numValue;
                }
            } catch (...) {
                customParamsJson[key] = value;
            }
        }
    }
    j["custom_params"] = customParamsJson;

    return j;
}

// TrackingStreamConfig::InputConfig 实现
TrackingStreamConfig::InputConfig TrackingStreamConfig::InputConfig::fromJson(const nlohmann::json& j) {
    InputConfig config;

    if (j.contains("url") && j["url"].is_string()) {
        config.url = j["url"];
    }

    if (j.contains("low_latency_mode") && j["low_latency_mode"].is_boolean()) {
        config.lowLatencyMode = j["low_latency_mode"];
    }

    if (j.contains("extra_options") && j["extra_options"].is_object()) {
        for (auto& [key, value] : j["extra_options"].items()) {
            if (value.is_string()) {
                config.extraOptions[key] = value;
            } else if (value.is_number()) {
                config.extraOptions[key] = std::to_string(value.get<double>());
            } else if (value.is_boolean()) {
                config.extraOptions[key] = value.get<bool>() ? "true" : "false";
            }
        }
    }

    return config;
}

nlohmann::json TrackingStreamConfig::InputConfig::toJson() const {
    nlohmann::json j;

    j["url"] = url;
    j["low_latency_mode"] = lowLatencyMode;

    nlohmann::json extraOptionsJson;
    for (const auto& [key, value] : extraOptions) {
        if (value == "true" || value == "false") {
            extraOptionsJson[key] = (value == "true");
        } else {
            try {
                double numValue = std::stod(value);
                if (numValue == std::floor(numValue)) {
                    extraOptionsJson[key] = static_cast<int>(numValue);
                } else {
                    extraOptionsJson[key] = numValue;
                }
            } catch (...) {
                extraOptionsJson[key] = value;
            }
        }
    }
    j["extra_options"] = extraOptionsJson;

    return j;
}

// TrackingStreamConfig::OutputConfig 实现
TrackingStreamConfig::OutputConfig TrackingStreamConfig::OutputConfig::fromJson(const nlohmann::json& j) {
    OutputConfig config;

    if (j.contains("url") && j["url"].is_string()) {
        config.url = j["url"];
    }

    if (j.contains("format") && j["format"].is_string()) {
        config.format = j["format"];
    }

    if (j.contains("video_bitrate") && j["video_bitrate"].is_number()) {
        config.videoBitrate = j["video_bitrate"];
    }

    if (j.contains("audio_bitrate") && j["audio_bitrate"].is_number()) {
        config.audioBitrate = j["audio_bitrate"];
    }

    if (j.contains("low_latency_mode") && j["low_latency_mode"].is_boolean()) {
        config.lowLatencyMode = j["low_latency_mode"];
    }

    if (j.contains("keyframe_interval") && j["keyframe_interval"].is_number()) {
        config.keyframeInterval = j["keyframe_interval"];
    }

    if (j.contains("extra_options") && j["extra_options"].is_object()) {
        for (auto& [key, value] : j["extra_options"].items()) {
            if (value.is_string()) {
                config.extraOptions[key] = value;
            } else if (value.is_number()) {
                config.extraOptions[key] = std::to_string(value.get<double>());
            } else if (value.is_boolean()) {
                config.extraOptions[key] = value.get<bool>() ? "true" : "false";
            }
        }
    }

    return config;
}

nlohmann::json TrackingStreamConfig::OutputConfig::toJson() const {
    nlohmann::json j;

    j["url"] = url;
    j["format"] = format;
    j["video_bitrate"] = videoBitrate;
    j["audio_bitrate"] = audioBitrate;
    j["low_latency_mode"] = lowLatencyMode;
    j["keyframe_interval"] = keyframeInterval;

    nlohmann::json extraOptionsJson;
    for (const auto& [key, value] : extraOptions) {
        if (value == "true" || value == "false") {
            extraOptionsJson[key] = (value == "true");
        } else {
            try {
                double numValue = std::stod(value);
                if (numValue == std::floor(numValue)) {
                    extraOptionsJson[key] = static_cast<int>(numValue);
                } else {
                    extraOptionsJson[key] = numValue;
                }
            } catch (...) {
                extraOptionsJson[key] = value;
            }
        }
    }
    j["extra_options"] = extraOptionsJson;

    return j;
}

// TrackingStreamConfig 实现
TrackingStreamConfig TrackingStreamConfig::fromJson(const nlohmann::json& j) {
    TrackingStreamConfig config;

    if (j.contains("id") && j["id"].is_string()) {
        config.id = j["id"];
    }

    if (j.contains("input") && j["input"].is_object()) {
        config.input = InputConfig::fromJson(j["input"]);
    }

    if (j.contains("output") && j["output"].is_object()) {
        config.output = OutputConfig::fromJson(j["output"]);
    }

    if (j.contains("ai_model") && j["ai_model"].is_object()) {
        config.aiModel = AIModelConfig::fromJson(j["ai_model"]);
    }

    if (j.contains("tracking") && j["tracking"].is_object()) {
        config.tracking = TrackingConfig::fromJson(j["tracking"]);
    }

    if (j.contains("auto_start") && j["auto_start"].is_boolean()) {
        config.autoStart = j["auto_start"];
    }

    if (j.contains("enabled") && j["enabled"].is_boolean()) {
        config.enabled = j["enabled"];
    }

    return config;
}

nlohmann::json TrackingStreamConfig::toJson() const {
    nlohmann::json j;

    j["id"] = id;
    j["input"] = input.toJson();
    j["output"] = output.toJson();
    j["ai_model"] = aiModel.toJson();
    j["tracking"] = tracking.toJson();
    j["auto_start"] = autoStart;
    j["enabled"] = enabled;

    return j;
}

bool TrackingStreamConfig::validate() const {
    // 检查ID
    if (id.empty()) {
        LOGGER_ERROR("跟踪流配置ID不能为空");
        return false;
    }

    // 检查输入URL
    if (input.url.empty()) {
        LOGGER_ERROR("跟踪流输入URL不能为空");
        return false;
    }

    // 检查输出URL
    if (output.url.empty()) {
        LOGGER_ERROR("跟踪流输出URL不能为空");
        return false;
    }

    // 检查AI模型路径
    if (aiModel.modelPath.empty()) {
        LOGGER_WARNING("跟踪流AI模型路径为空，将无法进行AI检测");
    }

    // 检查跟踪参数合理性
    if (tracking.detectionInterval < 1 || tracking.detectionInterval > 100) {
        LOGGER_ERROR("检测间隔必须在1-100帧之间");
        return false;
    }

    if (tracking.maxTargets < 1 || tracking.maxTargets > 100) {
        LOGGER_ERROR("最大目标数必须在1-100之间");
        return false;
    }

    if (tracking.minDetectionConfidence < 0 || tracking.minDetectionConfidence > 100) {
        LOGGER_ERROR("最小检测置信度必须在0-100之间");
        return false;
    }

    return true;
}

// GlobalTrackingConfig 实现
GlobalTrackingConfig GlobalTrackingConfig::fromJson(const nlohmann::json& j) {
    GlobalTrackingConfig config;

    if (j.contains("enabled") && j["enabled"].is_boolean()) {
        config.enabled = j["enabled"];
    }

    if (j.contains("default_tracker_type") && j["default_tracker_type"].is_string()) {
        config.defaultTrackerType = j["default_tracker_type"];
    }

    if (j.contains("default_detection_interval") && j["default_detection_interval"].is_number()) {
        config.defaultDetectionInterval = j["default_detection_interval"];
    }

    if (j.contains("max_targets_per_stream") && j["max_targets_per_stream"].is_number()) {
        config.maxTargetsPerStream = j["max_targets_per_stream"];
    }

    if (j.contains("max_lost_frames") && j["max_lost_frames"].is_number()) {
        config.maxLostFrames = j["max_lost_frames"];
    }

    if (j.contains("min_detection_confidence") && j["min_detection_confidence"].is_number()) {
        config.minDetectionConfidence = j["min_detection_confidence"];
    }

    if (j.contains("enable_visualization") && j["enable_visualization"].is_boolean()) {
        config.enableVisualization = j["enable_visualization"];
    }

    if (j.contains("tracking_thread_priority") && j["tracking_thread_priority"].is_number()) {
        config.trackingThreadPriority = j["tracking_thread_priority"];
    }

    return config;
}

nlohmann::json GlobalTrackingConfig::toJson() const {
    nlohmann::json j;

    j["enabled"] = enabled;
    j["default_tracker_type"] = defaultTrackerType;
    j["default_detection_interval"] = defaultDetectionInterval;
    j["max_targets_per_stream"] = maxTargetsPerStream;
    j["max_lost_frames"] = maxLostFrames;
    j["min_detection_confidence"] = minDetectionConfidence;
    j["enable_visualization"] = enableVisualization;
    j["tracking_thread_priority"] = trackingThreadPriority;

    return j;
}

StreamConfig StreamConfig::createDefault() {
    StreamConfig config;
    config.id = generateRandomId();
    config.inputUrl = "rtsp://example.com/stream";
    config.outputUrl = "rtmp://example.com/live/stream";
    config.outputFormat = "flv";
    config.pushEnabled = true;
    config.aiEnabled = false;
    config.isLocalFile = false;
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
        trackingStreamConfigs.clear(); // 新增：清除跟踪流配置

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

            // 新增：加载全局跟踪配置
            if (general.contains("tracking") && general["tracking"].is_object()) {
                globalTrackingConfig = GlobalTrackingConfig::fromJson(general["tracking"]);
                LOGGER_INFO("Loaded global tracking configuration, enabled: " +
                            std::string(globalTrackingConfig.enabled ? "true" : "false"));
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

        // 加载普通流配置
        if (configJson.contains("streams") && configJson["streams"].is_array()) {
            for (auto& streamJson : configJson["streams"]) {
                StreamConfig config = StreamConfig::fromJson(streamJson);
                if (config.validate()) {
                    streamConfigs.push_back(config);
                }
            }
        }

        // 新增：加载跟踪流配置
        if (configJson.contains("tracking_streams") && configJson["tracking_streams"].is_array()) {
            for (auto& trackingStreamJson : configJson["tracking_streams"]) {
                TrackingStreamConfig config = TrackingStreamConfig::fromJson(trackingStreamJson);
                if (config.validate()) {
                    trackingStreamConfigs.push_back(config);
                }
            }

            LOGGER_INFO("Loaded " + std::to_string(trackingStreamConfigs.size()) + " tracking stream configurations");
        }

        // 加载MQTT服务器配置
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

        // 新增：输出跟踪配置信息
        if (globalTrackingConfig.enabled) {
            LOGGER_INFO("Tracking enabled with " + std::to_string(trackingStreamConfigs.size()) +
                        " tracking streams configured");
        }

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

        // 新增：添加全局跟踪配置
        general["tracking"] = globalTrackingConfig.toJson();

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

        // 添加普通流配置
        json streamsJson = json::array();
        for (const auto& config : streamConfigs) {
            streamsJson.push_back(config.toJson());
        }
        configJson["streams"] = streamsJson;

        // 新增：添加跟踪流配置
        json trackingStreamsJson = json::array();
        for (const auto& config : trackingStreamConfigs) {
            trackingStreamsJson.push_back(config.toJson());
        }
        configJson["tracking_streams"] = trackingStreamsJson;

        // 添加MQTT服务器配置
        json mqttServersJson = json::array();
        for (const auto& config : mqttServers) {
            mqttServersJson.push_back(config.toJson());
        }
        configJson["mqtt_servers"] = mqttServersJson;

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

// 新增：跟踪相关配置方法实现

const GlobalTrackingConfig& AppConfig::getGlobalTrackingConfig() {
    return globalTrackingConfig;
}

void AppConfig::setGlobalTrackingConfig(const GlobalTrackingConfig& config) {
    globalTrackingConfig = config;
}

const std::vector<TrackingStreamConfig>& AppConfig::getTrackingStreamConfigs() {
    return trackingStreamConfigs;
}

void AppConfig::addTrackingStreamConfig(const TrackingStreamConfig& config) {
    trackingStreamConfigs.push_back(config);
}

TrackingStreamConfig AppConfig::findTrackingStreamConfigById(const std::string& id) {
    auto it = std::find_if(trackingStreamConfigs.begin(), trackingStreamConfigs.end(),
                           [&id](const TrackingStreamConfig& config) {
                               return config.id == id;
                           });

    if (it != trackingStreamConfigs.end()) {
        return *it;
    }

    // 返回默认的跟踪流配置
    TrackingStreamConfig defaultConfig;
    defaultConfig.id = id;
    return defaultConfig;
}

bool AppConfig::updateTrackingStreamConfig(const TrackingStreamConfig& config) {
    auto it = std::find_if(trackingStreamConfigs.begin(), trackingStreamConfigs.end(),
                           [&config](const TrackingStreamConfig& c) {
                               return c.id == config.id;
                           });

    if (it != trackingStreamConfigs.end()) {
        *it = config;
        return true;
    }

    return false;
}

bool AppConfig::removeTrackingStreamConfig(const std::string& id) {
    auto it = std::find_if(trackingStreamConfigs.begin(), trackingStreamConfigs.end(),
                           [&id](const TrackingStreamConfig& config) {
                               return config.id == id;
                           });

    if (it != trackingStreamConfigs.end()) {
        trackingStreamConfigs.erase(it);
        return true;
    }

    return false;
}

bool AppConfig::isTrackingEnabled() {
    return globalTrackingConfig.enabled;
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