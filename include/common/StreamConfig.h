#ifndef STREAM_CONFIG_H
#define STREAM_CONFIG_H

#include <string>
#include <map>
#include <vector>
#include "nlohmann/json.hpp"  // 直接包含整个json.hpp

/**
 * @brief 流处理配置结构体
 * 包含输入输出流的各种参数配置
 */
struct StreamConfig {
    // 配置ID
    std::string id;

    // 输入配置
    std::string inputUrl;

    // 输出配置
    std::string outputUrl;
    std::string outputFormat;
    bool pushEnabled = true;

    // 编码配置
    std::string videoCodec;
    std::string audioCodec;
    int videoBitrate = 2000000;  // 2Mbps默认值
    int audioBitrate = 128000;   // 128kbps默认值

    // 延迟优化配置
    bool lowLatencyMode = true;
    int keyframeInterval = 30;
    int bufferSize = 1000000;    // 1MB默认缓冲区

    // 硬件加速配置
    bool enableHardwareAccel = true;
    std::string hwaccelType;     // 例如 "cuda", "qsv", "vaapi"等

    // 分辨率设置（若需要转码）
    int width = 0;               // 0表示保持原始分辨率
    int height = 0;

    // 其他高级设置
    std::map<std::string, std::string> extraOptions;

    /**
     * @brief 创建默认配置
     * @return 默认配置实例
     */
    static StreamConfig createDefault();

    /**
     * @brief 从JSON创建配置
     * @param j JSON对象
     * @return 配置实例
     */
    static StreamConfig fromJson(const nlohmann::json& j);

    /**
     * @brief 将配置转换为JSON
     * @return JSON对象
     */
    nlohmann::json toJson() const;

    /**
     * @brief 验证配置有效性
     * @return 配置是否有效
     */
    bool validate() const;

    /**
     * @brief 将配置转换为字符串
     * @return 配置的字符串表示（JSON格式）
     */
    std::string toString() const;

    /**
     * @brief 获取重连尝试次数
     * @return 最大重连尝试次数，如果未设置则返回默认值5
     */
    int getMaxReconnectAttempts() const {
        auto it = extraOptions.find("maxReconnectAttempts");
        if (it != extraOptions.end()) {
            try {
                return std::stoi(it->second);
            } catch (...) {
                return 5; // 默认值
            }
        }
        return 5; // 默认值
    }

    /**
     * @brief 获取无数据超时时间（毫秒）
     * @return 无数据超时时间，如果未设置则返回默认值10000ms
     */
    int getNoDataTimeout() const {
        auto it = extraOptions.find("noDataTimeout");
        if (it != extraOptions.end()) {
            try {
                return std::stoi(it->second);
            } catch (...) {
                return 10000; // 默认值
            }
        }
        return 10000; // 默认值
    }

    /**
     * @brief 检查是否启用自动重连
     * @return 是否启用重连
     */
    bool isReconnectEnabled() const {
        return getMaxReconnectAttempts() > 0;
    }

    /**
     * @brief 检查是否为此流启用看门狗
     * @return 是否启用看门狗
     */
    bool isWatchdogEnabled() const {
        auto it = extraOptions.find("watchdogEnabled");
        if (it != extraOptions.end()) {
            return (it->second == "true" || it->second == "1");
        }
        return true; // 默认启用
    }

    /**
     * @brief 获取看门狗失败阈值
     * @return 失败次数阈值，如果未设置则返回默认值3
     */
    int getWatchdogFailThreshold() const {
        auto it = extraOptions.find("watchdogFailThreshold");
        if (it != extraOptions.end()) {
            try {
                return std::stoi(it->second);
            } catch (...) {
                return 3; // 默认值
            }
        }
        return 3; // 默认值
    }
};

/**
 * @brief 应用配置类
 * 包含整个应用程序的配置
 */
class AppConfig {
public:
    /**
     * @brief 从配置文件加载应用配置
     * @param configFilePath 配置文件路径
     * @return 成功加载返回true
     */
    static bool loadFromFile(const std::string& configFilePath);

    /**
     * @brief 保存配置到文件
     * @param configFilePath 配置文件路径
     * @return 成功保存返回true
     */
    static bool saveToFile(const std::string& configFilePath);

    /**
     * @brief 获取日志配置
     */
    static bool getLogToFile();
    static std::string getLogFilePath();
    static int getLogLevel();

    /**
     * @brief 获取线程池配置
     */
    static int getThreadPoolSize();

    /**
     * @brief 获取额外选项
     */
    static const std::map<std::string, std::string>& getExtraOptions();

    /**
     * @brief 获取看门狗配置
     */
    static bool getUseWatchdog();
    static int getWatchdogInterval();

    /**
     * @brief 获取全部流配置
     */
    static const std::vector<StreamConfig>& getStreamConfigs();

    /**
     * @brief 添加流配置
     * @param config 要添加的配置
     */
    static void addStreamConfig(const StreamConfig& config);

    /**
     * @brief 通过ID查找流配置
     * @param id 配置ID
     * @return 找到的配置，如果未找到则返回默认配置
     */
    static StreamConfig findStreamConfigById(const std::string& id);

    /**
     * @brief 更新流配置
     * @param config 新配置
     * @return 更新成功返回true
     */
    static bool updateStreamConfig(const StreamConfig& config);

    /**
     * @brief 删除流配置
     * @param id 配置ID
     * @return 删除成功返回true
     */
    static bool removeStreamConfig(const std::string& id);

private:
    static std::vector<StreamConfig> streamConfigs;
    static bool logToFile;
    static std::string logFilePath;
    static int logLevel;
    static int threadPoolSize;
    static std::map<std::string, std::string> extraOptions;
    static bool useWatchdog;
    static int watchdogInterval;
};

#endif // STREAM_CONFIG_H