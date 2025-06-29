#ifndef STREAM_CONFIG_H
#define STREAM_CONFIG_H

#include <string>
#include <map>
#include <vector>
#include "nlohmann/json.hpp"  // 直接包含整个json.hpp

/**
 * @brief 跟踪配置结构体
 */
struct TrackingConfig {
    std::string trackerType = "CSRT";        // 跟踪器类型：CSRT, KCF, MOSSE
    int detectionInterval = 10;              // AI检测间隔（帧数）
    int maxTargets = 15;                     // 最大跟踪目标数
    int maxLostFrames = 30;                  // 目标丢失最大帧数
    int minDetectionConfidence = 50;         // 最小检测置信度(%)
    bool enableTrajectory = true;            // 是否显示轨迹
    int trajectoryLength = 50;               // 轨迹点数量
    float overlapThreshold = 0.3f;           // 重叠阈值
    bool enableVisualization = true;         // 是否启用可视化
    bool enableCallback = true;              // 是否启用回调
    int callbackInterval = 1;                // 回调间隔（帧数）

    // JSON序列化/反序列化方法
    static TrackingConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

/**
 * @brief AI模型配置结构体
 */
struct AIModelConfig {
    std::string modelPath;                   // 模型文件路径
    float confidenceThreshold = 0.5f;        // 置信度阈值
    float nmsThreshold = 0.45f;              // NMS阈值
    int inputWidth = 640;                    // 输入宽度
    int inputHeight = 640;                   // 输入高度
    std::vector<std::string> classNames;     // 类别名称列表
    bool enableGPU = false;                  // 是否启用GPU加速
    std::map<std::string, std::string> customParams; // 自定义参数

    // JSON序列化/反序列化方法
    static AIModelConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

/**
 * @brief 跟踪流配置结构体
 */
struct TrackingStreamConfig {
    std::string id;                          // 流ID

    // 输入配置
    struct InputConfig {
        std::string url;                     // 输入URL
        bool lowLatencyMode = true;          // 低延迟模式
        std::map<std::string, std::string> extraOptions; // 额外选项

        static InputConfig fromJson(const nlohmann::json& j);
        nlohmann::json toJson() const;
    } input;

    // 输出配置
    struct OutputConfig {
        std::string url;                     // 输出URL
        std::string format = "flv";          // 输出格式
        int videoBitrate = 4000000;          // 视频码率
        int audioBitrate = 128000;           // 音频码率
        bool lowLatencyMode = true;          // 低延迟模式
        int keyframeInterval = 30;           // 关键帧间隔
        std::map<std::string, std::string> extraOptions; // 额外选项

        static OutputConfig fromJson(const nlohmann::json& j);
        nlohmann::json toJson() const;
    } output;

    AIModelConfig aiModel;                   // AI模型配置
    TrackingConfig tracking;                 // 跟踪配置
    bool autoStart = false;                  // 是否自动启动
    bool enabled = true;                     // 是否启用

    // JSON序列化/反序列化方法
    static TrackingStreamConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;

    // 验证配置有效性
    bool validate() const;
};

struct ModelConfig {
    int modelType = 0;            // 模型类型
    bool enabled = true;          // 是否启用该模型
    std::map<std::string, std::string> modelParams;  // 模型特定参数

    // JSON序列化/反序列化方法
    static ModelConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

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
    bool aiEnabled = false;
    bool isLocalFile = false;

    std::vector<ModelConfig> models;  // 多个模型配置

    int modelType = 1;

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
};

/**
 * @brief MQTT 订阅配置
 */
struct MQTTSubscriptionConfig {
    std::string topic;
    int qos;
    std::string handlerId;

    MQTTSubscriptionConfig() : qos(0) {}

    static MQTTSubscriptionConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

/**
 * @brief MQTT 服务器配置
 */
struct MQTTServerConfig {
    std::string name;
    std::string brokerUrl;
    std::string clientId;
    std::string username;
    std::string password;
    bool cleanSession;
    int keepAliveInterval;
    std::vector<MQTTSubscriptionConfig> subscriptions;
    bool autoReconnect;
    int reconnectInterval;

    MQTTServerConfig() : cleanSession(true), keepAliveInterval(60),
                         autoReconnect(true), reconnectInterval(5) {}

    static MQTTServerConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

/**
 * @brief HTTP 服务器配置
 */
struct HTTPServerConfig {
    std::string host;
    int port;
    int connectionTimeout;

    HTTPServerConfig() : host("127.0.0.1"), port(9000),
                         connectionTimeout(5) {}

    static HTTPServerConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

/**
 * @brief 全局跟踪配置
 */
struct GlobalTrackingConfig {
    bool enabled = false;                    // 是否启用跟踪功能
    std::string defaultTrackerType = "CSRT"; // 默认跟踪器类型
    int defaultDetectionInterval = 10;       // 默认检测间隔
    int maxTargetsPerStream = 20;           // 每个流的最大目标数
    int maxLostFrames = 30;                 // 最大丢失帧数
    int minDetectionConfidence = 50;        // 最小检测置信度(%)
    bool enableVisualization = true;        // 是否启用可视化
    int trackingThreadPriority = 0;         // 跟踪线程优先级（0=normal, 1=high）

    static GlobalTrackingConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
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
    static std::string getDirPath();

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

    static const std::vector<MQTTServerConfig>& getMQTTServers(); // 获取所有MQTT服务器配置

    /**
     * @brief 获取HTTP服务器配置
     * @return HTTP服务器配置
     */
    static const HTTPServerConfig& getHTTPServerConfig();

    // 新增：跟踪相关配置方法

    /**
     * @brief 获取全局跟踪配置
     */
    static const GlobalTrackingConfig& getGlobalTrackingConfig();

    /**
     * @brief 设置全局跟踪配置
     */
    static void setGlobalTrackingConfig(const GlobalTrackingConfig& config);

    /**
     * @brief 获取所有跟踪流配置
     */
    static const std::vector<TrackingStreamConfig>& getTrackingStreamConfigs();

    /**
     * @brief 添加跟踪流配置
     */
    static void addTrackingStreamConfig(const TrackingStreamConfig& config);

    /**
     * @brief 通过ID查找跟踪流配置
     */
    static TrackingStreamConfig findTrackingStreamConfigById(const std::string& id);

    /**
     * @brief 更新跟踪流配置
     */
    static bool updateTrackingStreamConfig(const TrackingStreamConfig& config);

    /**
     * @brief 删除跟踪流配置
     */
    static bool removeTrackingStreamConfig(const std::string& id);

    /**
     * @brief 检查跟踪功能是否启用
     */
    static bool isTrackingEnabled();

private:
    static std::vector<StreamConfig> streamConfigs;
    static bool logToFile;
    static std::string logFilePath;
    static int logLevel;
    static int threadPoolSize;
    static std::map<std::string, std::string> extraOptions;
    static bool useWatchdog;
    static int watchdogInterval;

    static std::string dirPath;

    static std::vector<MQTTServerConfig> mqttServers; // MQTT服务器配置列表
    static HTTPServerConfig httpServerConfig; // HTTP服务器配置

    // 新增：跟踪相关静态成员
    static GlobalTrackingConfig globalTrackingConfig;  // 全局跟踪配置
    static std::vector<TrackingStreamConfig> trackingStreamConfigs; // 跟踪流配置列表
};

#endif // STREAM_CONFIG_H