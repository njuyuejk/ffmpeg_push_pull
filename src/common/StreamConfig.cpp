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
    config.extraOptions["autoStart"] = "true";
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
        else {
            // 额外选项
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

    for (const auto& [key, value] : extraOptions) {
        ss << key << "=" << value << "\n";
    }

    return ss.str();
}

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
    return true;
}

bool AppConfig::saveToFile(const std::string& configFilePath) {
    std::ofstream file(configFilePath);
    if (!file.is_open()) {
        return false;
    }

    // 写入常规部分
    file << "[general]\n";
    file << "logToFile=" << (logToFile ? "true" : "false") << "\n";
    file << "logFilePath=" << logFilePath << "\n";
    file << "logLevel=" << logLevel << "\n";
    file << "threadPoolSize=" << threadPoolSize << "\n";

    // 写入额外的应用程序选项
    file << "# Application settings\n";
    file << "monitorInterval=30\n";  // 监控间隔（秒）
    file << "autoRestartStreams=true\n";  // 是否自动重启出错的流

    for (const auto& [key, value] : extraOptions) {
        if (key != "monitorInterval" && key != "autoRestartStreams") {
            file << key << "=" << value << "\n";
        }
    }

    file << "\n";

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