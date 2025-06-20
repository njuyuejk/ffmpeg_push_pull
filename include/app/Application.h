#ifndef APPLICATION_H
#define APPLICATION_H

#include "ffmpeg_base/MultiStreamManager.h"
#include "common/StreamConfig.h"
#include "common/Watchdog.h"
#include "mqtt/mqtt_sync_client.h"
#include "http/httplib.h"
#include "mqtt/PTZControlHandler.h"
#include "mqtt/AIDataResponse.pb.h"
#include "AIService/rknnPool.h"
#include <string>
#include <memory>

struct ModelPoolEntry {
    std::vector<std::unique_ptr<rknn_lite>> rkpool; // 模型集合
    std::unique_ptr<ThreadPool> pool_; // 线程池
    std::queue<std::future<bool>> futs; // 任务函数
    std::string streamId; // 视频流名称
    int count; // 抽帧计数
    int modelType; // 模型类型
    int rknnFlag; // 模型数量
    bool first_ok; // 模型第一次初始化标识
};

struct SingleModelEntry{
    std::unique_ptr<rknn_lite> singleRKModel; // 单个模型
    std::string streamId; // 视频流名称
    int count; // 抽帧计数
    int modelType; // 模型类型
    bool isEnabled; //模型可用
    bool warningFlag = false;                 // 此模型的警告状态
    int timeCount = 0;                        // 此模型的时间计数器
    std::map<std::string, std::string> params; // 模型特定参数
};

/**
 * @brief 应用程序类
 * 管理应用程序生命周期和流管理，完全基于配置文件
 */
class Application {
public:
    /**
     * @brief 获取应用程序实例（单例模式）
     * @return 应用程序实例的引用
     */
    static Application& getInstance();

    /**
     * @brief 禁止拷贝构造
     */
    Application(const Application&) = delete;

    /**
     * @brief 禁止赋值操作
     */
    Application& operator=(const Application&) = delete;

    /**
     * @brief 初始化应用程序
     * @param configFilePath 配置文件路径，默认为"config.ini"
     * @return 初始化是否成功
     */
    bool initialize(const std::string& configFilePath = "config.ini");

    /**
     * @brief 运行应用程序
     * @return 返回代码
     */
    int run();

    /**
     * @brief 处理信号
     * @param signal 信号值
     */
    void handleSignal(int signal);

    /**
     * @brief 清理应用程序资源
     */
    void cleanup();
private:
    /**
     * @brief 私有构造函数（单例模式）
     */
    Application();

    /**
     * @brief 列出所有流
     */
    void listStreams();

    /**
     * @brief 启动所有配置的流
     */
    void startAllStreams();

    /**
     * @brief 停止所有运行中的流
     */
    void stopAllStreams();

    /**
     * @brief 监控并管理流状态
     */
    void monitorStreams();

    /**
 * @brief 设置自定义帧处理回调
 */
    void setupCustomFrameProcessing();

    /**
     * @brief MQTT初始化和清理
     * @return
     */
    bool initializeMQTTClients();
    void cleanupMQTTClients();

    void registerTopicHandler(const std::string& serverName, const std::string& topic,
                                           std::function<void(const std::string&, const std::string&, const std::string&)> handler);

    void registerTopicHandler(const std::string& serverName, const std::string& topic,
                              std::function<void(const std::string&, const std::string&, MQTTClient_message&)> handler);

    /**
     * @brief MQTT消息处理
     * @param serverName
     * @param topic
     * @param payload
     */
    void handleMQTTMessage(const std::string& serverName, const std::string& topic, const std::string& payload);

    void handleMQTTMessage(const std::string& serverName, const std::string& topic, MQTTClient_message& message);

    void handlePTZControl(const std::string& serverName, const std::string& topic, const std::string& payload);

    void handlePTZControl(const std::string& serverName, const std::string& topic, MQTTClient_message& message);

    void handleAIEnabledControl(const std::string& serverName, const std::string& topic, const std::string& payload);

    /**
     * @brief mqtt消息发布
     * @param serverName
     */
    void publishSystemStatus(const std::string& serverName, const std::string& topic, const std::string& payload);

    /*
     *@brief 定时发送AI算法状态信息
     * */
    void publishAIStatus(const std::string& serverName);

    /**
     * @brief 向所有MQTT服务端发送消息
     * @param topic
     * @param payload
     * @param qos
     * @return
     */
    bool publishToAllServers(const std::string& topic, const std::string& payload, int qos = 0);

    /**
 * @brief 监控MQTT连接状态
 */
    void monitorMQTTConnections();

    /**
     * @brief 配置MQTT客户端监控参数
     */
    void configureMQTTMonitoring();

    /**
     * @brief 初始化模型
     */
    std::vector<std::unique_ptr<rknn_lite>> initModel(int modelType);

    std::unique_ptr<rknn_lite> initSingleModel(int modelType,  const std::map<std::string, std::string>& params);

    /**
     * @brief 视频帧的AI实时处理
     * @param streamId
     * @param frame
     * @param pts
     */
    void processFrameAI(const std::string& streamId, const AVFrame* frame, int64_t pts, int fps);

    void processDelayFrameAI(const std::string& streamId, const AVFrame* frame, int64_t pts, int fps);

    void test_model();

    void handleModelWarning(SingleModelEntry* model, const cv::Mat& dstMat, const std::string& plateResult, const std::string& targetValue);

private:
    // 成员变量
    bool running_;                           // 运行状态
    std::string configFilePath_;             // 配置文件路径
    std::unique_ptr<MultiStreamManager> streamManager_; // 流管理器
    int monitorIntervalSeconds_;             // 监控间隔（秒）
    bool autoRestartStreams_;                // 是否自动重启出错的流

    // 看门狗相关
    std::unique_ptr<Watchdog> watchdog_;     // 看门狗
    bool useWatchdog_;                       // 是否使用看门狗
    int watchdogIntervalSeconds_;            // 看门狗检查间隔（秒）

    int periodicReconnectInterval_; // 周期性重连间隔（秒）
    int64_t lastPeriodicReconnectTime_ = 0; // 上次周期性重连时间

    std::unique_ptr<httplib::Client> httpClient_;
    bool isHttpConnect_;

    // 使用 MQTTClientManager 替代直接映射
    MQTTClientManager& mqttManager = MQTTClientManager::getInstance();

    // MQTT监控相关
    int mqttHealthCheckInterval_ = 30; // MQTT健康检查间隔（秒）
    bool mqttMonitoringEnabled_ = true; // 是否启用MQTT监控
    int64_t lastMQTTHealthCheckTime_ = 0; // 上次MQTT健康检查时间

    // 存储每个主题处理程序的映射
    struct TopicHandler {
        std::string serverName;
        std::string topic;
        std::function<void(const std::string&, const std::string&, const std::string&)> handler;
    };
    std::vector<TopicHandler> topicHandlers;

    struct TopicMsgHandler {
        std::string serverName;
        std::string topic;
        std::function<void(const std::string&, const std::string&, MQTTClient_message&)> handler;
    };
    std::vector<TopicMsgHandler> topicMsgHandlers;

    // AI识别相关服务
    std::vector<std::unique_ptr<ModelPoolEntry>> modelPools_;

    std::vector<std::unique_ptr<SingleModelEntry>> singleModelPools_;
    int timeCount = 0;
    bool warningFlag = false;

    // 添加线程相关成员
    std::unique_ptr<std::thread> mqttAIStatusThread;
    std::atomic<bool> mqttAIStatusStarted{false};

};

/**
 * @brief 信号处理函数
 * @param signal 信号值
 */
void signalHandlerWrapper(int signal);

#endif // APPLICATION_H