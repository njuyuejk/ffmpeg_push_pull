#include "common/StreamConfig.h"
#include "logger/Logger.h"
#include <fstream>
#include <sstream>
#include <utility>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <random>

// JSON解析库 (可以使用第三方库如nlohmann/json，这里为简化使用自定义解析)
namespace {
    // 简单的字符串解析函数
    std::pair<std::string, std::string> parseKeyValue(const std::string& line) {
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // 去除前后空格
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            return {key, value};
        }
        return {"", ""};
    }

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

StreamConfig StreamConfig::createDefault() {
    StreamConfig config;
    config.id = generateRandomId();
    config.inputUrl = "rtsp://example.com/stream";
    config.outputUrl = "rtmp://example.com/live/stream";
    config.outputFormat = "flv";
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

StreamConfig StreamConfig::fromString(const std::string& configStr) {
    StreamConfig config = createDefault();

    std::istringstream ss(configStr);
    std::string line;

    while (std::getline(ss, line)) {
        // 跳过注释和空行
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        auto [key, value] = parseKeyValue(line);
        if (key.empty()) continue;

        if (key == "id") config.id = value;
        else if (key == "inputUrl") config.inputUrl = value;
        else if (key == "outputUrl") config.outputUrl = value;
        else if (key == "outputFormat") config.outputFormat = value;
        else if (key == "videoCodec") config.videoCodec = value;
        else if (key == "audioCodec") config.audioCodec = value;
        else if (key == "videoBitrate") config.videoBitrate = std::stoi(value);
        else if (key == "audioBitrate") config.audioBitrate = std::stoi(value);
        else if (key == "lowLatencyMode") config.lowLatencyMode = (value == "true" || value == "1");
        else if (key == "keyframeInterval") config.keyframeInterval = std::stoi(value);
        else if (key == "bufferSize") config.bufferSize = std::stoi(value);
        else if (key == "enableHardwareAccel") config.enableHardwareAccel = (value == "true" || value == "1");
        else if (key == "hwaccelType") config.hwaccelType = value;
        else if (key == "width") config.width = std::stoi(value);
        else if (key == "height") config.height = std::stoi(value);
            // 处理重连相关的配置选项
        else if (key == "maxReconnectAttempts" ||
                 key == "noDataTimeout" ||
                 key == "input_reconnect" ||
                 key == "input_reconnect_streamed" ||
                 key == "input_reconnect_delay_max") {
            // 显式处理这些选项，确保它们被正确记录到日志
            config.extraOptions[key] = value;
            Logger::debug("Setting reconnection parameter: " + key + "=" + value);
        }
            // 处理看门狗相关的配置选项
        else if (key == "watchdogEnabled" ||
                 key == "watchdogFailThreshold") {
            config.extraOptions[key] = value;
            Logger::debug("Setting watchdog parameter: " + key + "=" + value);
        }
        else {
            // 其他额外选项
            config.extraOptions[key] = value;
        }
    }

    return config;
}

bool StreamConfig::validate() const {
    // 基本验证
    if (inputUrl.empty() || outputUrl.empty()) {
        return false;
    }

    // 验证重连相关参数
    if (extraOptions.find("maxReconnectAttempts") != extraOptions.end()) {
        try {
            int attempts = std::stoi(extraOptions.at("maxReconnectAttempts"));
            if (attempts < 0) {
                Logger::warning("Invalid maxReconnectAttempts value (must be >= 0): " + extraOptions.at("maxReconnectAttempts"));
                return false;
            }
        } catch (const std::exception& e) {
            Logger::warning("Invalid maxReconnectAttempts format: " + extraOptions.at("maxReconnectAttempts"));
            return false;
        }
    }

    if (extraOptions.find("noDataTimeout") != extraOptions.end()) {
        try {
            int timeout = std::stoi(extraOptions.at("noDataTimeout"));
            if (timeout < 1000) { // 至少1秒
                Logger::warning("Invalid noDataTimeout value (must be >= 1000 ms): " + extraOptions.at("noDataTimeout"));
                return false;
            }
        } catch (const std::exception& e) {
            Logger::warning("Invalid noDataTimeout format: " + extraOptions.at("noDataTimeout"));
            return false;
        }
    }

    // 验证看门狗相关参数
    if (extraOptions.find("watchdogFailThreshold") != extraOptions.end()) {
        try {
            int threshold = std::stoi(extraOptions.at("watchdogFailThreshold"));
            if (threshold < 1) {
                Logger::warning("Invalid watchdogFailThreshold value (must be >= 1): " + extraOptions.at("watchdogFailThreshold"));
                return false;
            }
        } catch (const std::exception& e) {
            Logger::warning("Invalid watchdogFailThreshold format: " + extraOptions.at("watchdogFailThreshold"));
            return false;
        }
    }

    // 更多验证规则可以根据需要添加

    return true;
}

std::string StreamConfig::toString() const {
    std::ostringstream ss;

    ss << "id=" << id << "\n";
    ss << "inputUrl=" << inputUrl << "\n";
    ss << "outputUrl=" << outputUrl << "\n";
    ss << "outputFormat=" << outputFormat << "\n";
    ss << "videoCodec=" << videoCodec << "\n";
    ss << "audioCodec=" << audioCodec << "\n";
    ss << "videoBitrate=" << videoBitrate << "\n";
    ss << "audioBitrate=" << audioBitrate << "\n";
    ss << "lowLatencyMode=" << (lowLatencyMode ? "true" : "false") << "\n";
    ss << "keyframeInterval=" << keyframeInterval << "\n";
    ss << "bufferSize=" << bufferSize << "\n";
    ss << "enableHardwareAccel=" << (enableHardwareAccel ? "true" : "false") << "\n";
    ss << "hwaccelType=" << hwaccelType << "\n";
    ss << "width=" << width << "\n";
    ss << "height=" << height << "\n";

    // 优先输出重要的参数，以便在配置文件中更容易找到
    // 重连参数
    if (extraOptions.find("maxReconnectAttempts") != extraOptions.end()) {
        ss << "maxReconnectAttempts=" << extraOptions.at("maxReconnectAttempts") << "\n";
    }
    if (extraOptions.find("noDataTimeout") != extraOptions.end()) {
        ss << "noDataTimeout=" << extraOptions.at("noDataTimeout") << "\n";
    }

    // 看门狗参数
    if (extraOptions.find("watchdogEnabled") != extraOptions.end()) {
        ss << "watchdogEnabled=" << extraOptions.at("watchdogEnabled") << "\n";
    }
    if (extraOptions.find("watchdogFailThreshold") != extraOptions.end()) {
        ss << "watchdogFailThreshold=" << extraOptions.at("watchdogFailThreshold") << "\n";
    }

    // 其他所有额外选项
    for (const auto& [key, value] : extraOptions) {
        // 跳过已经输出的重要参数
        if (key != "maxReconnectAttempts" && key != "noDataTimeout" &&
            key != "watchdogEnabled" && key != "watchdogFailThreshold") {
            ss << key << "=" << value << "\n";
        }
    }

    return ss.str();
}

// AppConfig 相关方法
bool AppConfig::loadFromFile(const std::string& configFilePath) {
    std::ifstream file(configFilePath);
    if (!file.is_open()) {
        return false;
    }

    // 清除现有配置
    streamConfigs.clear();
    extraOptions.clear();

    std::string line;
    std::string section;
    std::ostringstream currentConfigStr;

    bool inStreamSection = false;

    while (std::getline(file, line)) {
        // 去除前后空格
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // 跳过注释和空行
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // 检查是否是节标题 [section]
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            // 如果之前在流节中，保存当前配置
            if (inStreamSection) {
                StreamConfig config = StreamConfig::fromString(currentConfigStr.str());
                if (config.validate()) {
                    streamConfigs.push_back(config);
                }
                currentConfigStr.str("");
                currentConfigStr.clear();
            }

            section = line.substr(1, line.length() - 2);
            inStreamSection = (section.find("stream") == 0);
            continue;
        }

        // 解析键值对
        auto [key, value] = parseKeyValue(line);
        if (key.empty()) continue;

        if (section == "general") {
            if (key == "logToFile") logToFile = (value == "true" || value == "1");
            else if (key == "logFilePath") logFilePath = value;
            else if (key == "logLevel") logLevel = std::stoi(value);
            else if (key == "threadPoolSize") threadPoolSize = std::stoi(value);
            else if (key == "useWatchdog") useWatchdog = (value == "true" || value == "1");
            else if (key == "watchdogInterval") watchdogInterval = std::stoi(value);
            else {
                // 其他全局选项保存到额外选项中
                extraOptions[key] = value;
            }
        } else if (inStreamSection) {
            // 在流节中，收集配置行
            currentConfigStr << line << "\n";
        }
    }

    // 不要忘记处理最后一个流配置
    if (inStreamSection) {
        StreamConfig config = StreamConfig::fromString(currentConfigStr.str());
        if (config.validate()) {
            streamConfigs.push_back(config);
        }
    }

    file.close();

    // 输出加载信息
    Logger::info("Loaded " + std::to_string(streamConfigs.size()) + " stream configurations");
    Logger::info("Global watchdog settings: useWatchdog=" + std::string(useWatchdog ? "true" : "false") +
                 ", interval=" + std::to_string(watchdogInterval) + "s");

    for (const auto& config : streamConfigs) {
        Logger::debug("Loaded stream: " + config.id + " (" + config.inputUrl + " -> " + config.outputUrl + ")");

        // 记录重连和看门狗相关配置
        if (config.extraOptions.find("maxReconnectAttempts") != config.extraOptions.end()) {
            Logger::debug("  maxReconnectAttempts: " + config.extraOptions.at("maxReconnectAttempts"));
        }
        if (config.extraOptions.find("noDataTimeout") != config.extraOptions.end()) {
            Logger::debug("  noDataTimeout: " + config.extraOptions.at("noDataTimeout") + " ms");
        }
        if (config.extraOptions.find("watchdogEnabled") != config.extraOptions.end()) {
            Logger::debug("  watchdogEnabled: " + config.extraOptions.at("watchdogEnabled"));
        }
        if (config.extraOptions.find("watchdogFailThreshold") != config.extraOptions.end()) {
            Logger::debug("  watchdogFailThreshold: " + config.extraOptions.at("watchdogFailThreshold"));
        }
    }

    return true;
}

bool AppConfig::saveToFile(const std::string& configFilePath) {
    std::ofstream file(configFilePath);
    if (!file.is_open()) {
        return false;
    }

    // 写入常规部分
    file << "#===========================================================\n";
    file << "# FFmpeg Stream Processor Configuration\n";
    file << "#===========================================================\n\n";

    file << "[general]\n";
    file << "# 日志配置\n";
    file << "logToFile=" << (logToFile ? "true" : "false") << "\n";
    file << "logFilePath=" << logFilePath << "\n";
    file << "logLevel=" << logLevel << "\n\n";

    file << "# 线程池配置\n";
    file << "threadPoolSize=" << threadPoolSize << "\n\n";

    file << "# 应用程序设置\n";
    file << "monitorInterval=30\n";  // 监控间隔（秒）
    file << "autoRestartStreams=true\n\n";  // 是否自动重启出错的流

    file << "# 看门狗配置\n";
    file << "useWatchdog=" << (useWatchdog ? "true" : "false") << "\n";
    file << "watchdogInterval=" << watchdogInterval << "  # 看门狗检查间隔（秒）\n\n";

    // 写入其他全局选项
    for (const auto& [key, value] : extraOptions) {
        if (key != "monitorInterval" && key != "autoRestartStreams" &&
            key != "useWatchdog" && key != "watchdogInterval") {
            file << key << "=" << value << "\n";
        }
    }

    file << "\n";
    file << "#===========================================================\n";
    file << "# 流配置\n";
    file << "#===========================================================\n\n";

    // 写入每个流配置
    for (size_t i = 0; i < streamConfigs.size(); ++i) {
        file << "[stream" << i << "]\n";
        file << streamConfigs[i].toString() << "\n";
    }

    file.close();
    return true;
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