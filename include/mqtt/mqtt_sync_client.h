//
// Created by YJK on 2025/3/17.
//

#ifndef FFMPEG_PULL_PUSH_MQTT_SYNC_CLIENT_H
#define FFMPEG_PULL_PUSH_MQTT_SYNC_CLIENT_H


#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include "MQTTClient.h"
#include "logger/Logger.h"


/**
 * @brief MQTT客户端包装类
 */
class MQTTClientWrapper {
public:
    /**
     * @brief 构造函数
     * @param brokerUrl MQTT代理URL
     * @param clientId 客户端ID
     * @param username 用户名（可选）
     * @param password 密码（可选）
     * @param cleanSession 是否清除会话
     * @param keepAliveInterval 保活间隔
     */
    MQTTClientWrapper(const std::string& brokerUrl = "",
                      const std::string& clientId = "",
                      const std::string& username = "",
                      const std::string& password = "",
                      bool cleanSession = true,
                      int keepAliveInterval = 60);

    /**
     * @brief 析构函数
     */
    ~MQTTClientWrapper();

    /**
     * @brief 初始化MQTT客户端
     * @param brokerUrl MQTT代理URL
     * @param clientId 客户端ID
     * @param username 用户名（可选）
     * @param password 密码（可选）
     * @param cleanSession 是否清除会话
     * @param keepAliveInterval 保活间隔
     * @return 是否成功初始化
     */
    bool initialize(const std::string& brokerUrl, const std::string& clientId,
                    const std::string& username = "", const std::string& password = "",
                    bool cleanSession = true, int keepAliveInterval = 60);

    /**
     * @brief 连接到MQTT代理
     * @return 是否成功连接
     */
    bool connect();

    /**
     * @brief 断开连接
     */
    void disconnect();

    /**
     * @brief 检查是否已连接
     * @return 是否已连接
     */
    bool isConnected();

    /**
     * @brief 消息回调函数类型
     */
    using MessageCallback = std::function<void(const std::string&, const std::string&)>;

    using MessageCallback1 = std::function<void(const std::string&, MQTTClient_message&)>;

    /**
     * @brief 订阅主题
     * @param topic 主题名称
     * @param qos 服务质量
     * @param callback 消息回调函数
     * @return 是否成功订阅
     */
    bool subscribe(const std::string& topic, int qos, MessageCallback callback);

    bool subscribe(const std::string& topic, int qos, MessageCallback1 callback);

    /**
     * @brief 取消订阅主题
     * @param topic 主题名称
     * @return 是否成功取消订阅
     */
    bool unsubscribe(const std::string& topic);

    /**
     * @brief 发布消息
     * @param topic 主题名称
     * @param payload 消息内容
     * @param qos 服务质量
     * @param retained 是否保留
     * @return 是否成功发布
     */
    bool publish(const std::string& topic, const std::string& payload, int qos = 0, bool retained = false);

    /**
     * @brief 设置连接断开回调函数
     * @param callback 回调函数
     */
    void setConnectionLostCallback(std::function<void(const std::string&)> callback);

    /**
     * @brief 销毁客户端实例
     */
    void cleanup();

    /**
     * @brief 获取MQTT代理URL
     * @return MQTT代理URL
     */
    std::string getBrokerUrl() const { return brokerUrl; }

    /**
     * @brief 获取客户端ID
     * @return 客户端ID
     */
    std::string getClientId() const { return clientId; }

    /**
    * @brief 检查客户端健康状态
    * @return 是否健康
    */
    bool checkHealth();

    /**
     * @brief 获取重连尝试次数
     * @return 重连尝试次数
     */
    int getReconnectAttempts() const { return reconnectAttempts; }

    /**
     * @brief 获取上次重连时间
     * @return 上次重连时间（秒，Unix时间戳）
     */
    int64_t getLastReconnectTime() const { return lastReconnectTime; }

    /**
     * @brief 获取上次连接断开时间
     * @return 上次连接断开时间（秒，Unix时间戳）
     */
    int64_t getLastDisconnectTime() const { return lastDisconnectTime; }

    /**
     * @brief 获取连接状态持续时间（秒）
     * @return 如果已连接，返回连接持续时间；如果断开，返回断开持续时间
     */
    int64_t getStatusDuration() const;

    /**
     * @brief 强制重连
     * @return 是否重连成功
     */
    bool forceReconnect();

    /**
     * @brief 重置重连计数器
     */
    void resetReconnectAttempts() { reconnectAttempts = 0; }

    /**
     * @brief 设置自动重连策略
     * @param enable 是否启用自动重连
     * @param maxAttempts 最大重试次数，0表示无限制
     * @param initialDelayMs 初始重连延迟（毫秒）
     * @param maxDelayMs 最大重连延迟（毫秒）
     */
    void setReconnectPolicy(bool enable, int maxAttempts = 0,
                            int initialDelayMs = 1000, int maxDelayMs = 30000);

private:
    // MQTT客户端实例
    MQTTClient client;

    // 锁用于保证线程安全
    std::mutex mutex;

    // 连接状态
    bool connected;

    // 初始化状态
    bool initialized;

    // 连接参数
    std::string brokerUrl;
    std::string clientId;
    std::string username;
    std::string password;
    bool cleanSession;
    int keepAliveInterval;

    // 订阅主题的回调函数映射
    std::map<std::string, MessageCallback> topicCallbacks;
    std::map<std::string, MessageCallback1> topicMessageCallbacks;

    // 连接断开回调函数
    std::function<void(const std::string&)> connectionLostCallback;

    // 重连相关参数
    std::atomic<int> reconnectAttempts{0};
    std::atomic<int64_t> lastReconnectTime{0};
    std::atomic<int64_t> lastDisconnectTime{0};
    std::atomic<int64_t> connectionStartTime{0};
    std::atomic<bool> autoReconnect{true};
    int maxReconnectAttempts{0};  // 0表示无限制
    int initialReconnectDelayMs{1000};
    int maxReconnectDelayMs{30000};

    // 静态回调函数，用于MQTT库回调
    static int messageArrivedCallback(void* context, char* topicName, int topicLen, MQTTClient_message* message);
    static void staticconnectionLostCallback(void* context, char* cause);

    // 尝试重新连接
    bool reconnect();

    // 计算当前重连延迟（指数退避）
    int calculateReconnectDelay() const;
};

/**
 * @brief MQTT客户端管理器类
 * 用于管理多个MQTT客户端连接
 */
class MQTTClientManager {
public:
    /**
     * @brief 获取单例实例
     * @return MQTTClientManager单例对象
     */
    static MQTTClientManager& getInstance();

    /**
     * @brief 创建新的MQTT客户端
     * @param clientName 客户端名称（用于标识客户端）
     * @param brokerUrl MQTT代理URL
     * @param clientId MQTT客户端ID
     * @param username 用户名（可选）
     * @param password 密码（可选）
     * @param cleanSession 是否清除会话
     * @param keepAliveInterval 保活间隔
     * @return 是否成功创建
     */
    bool createClient(const std::string& clientName,
                      const std::string& brokerUrl,
                      const std::string& clientId,
                      const std::string& username = "",
                      const std::string& password = "",
                      bool cleanSession = true,
                      int keepAliveInterval = 60);

    /**
     * @brief 获取MQTT客户端
     * @param clientName 客户端名称
     * @return MQTT客户端的共享指针，如果未找到则返回nullptr
     */
    std::shared_ptr<MQTTClientWrapper> getClient(const std::string& clientName);

    /**
     * @brief 删除MQTT客户端
     * @param clientName 客户端名称
     * @return 是否成功删除
     */
    bool removeClient(const std::string& clientName);

    /**
     * @brief 获取所有客户端名称
     * @return 客户端名称列表
     */
    std::vector<std::string> listClients();

    /**
     * @brief 销毁所有客户端
     */
    void cleanup();

    /**
     * @brief 检查所有客户端的健康状态并尝试恢复
     * @return 健康状态的客户端数量
     */
    int checkAndRecoverClients();

    /**
     * @brief 设置MQTT连接健康检查的间隔时间
     * @param intervalSeconds 健康检查间隔（秒）
     */
    void setHealthCheckInterval(int intervalSeconds);

    /**
     * @brief 获取MQTT连接健康检查的间隔时间
     * @return 健康检查间隔（秒）
     */
    int getHealthCheckInterval() const { return healthCheckIntervalSeconds; }

    /**
     * @brief 启动周期性健康检查
     */
    void startPeriodicHealthCheck();

    /**
     * @brief 停止周期性健康检查
     */
    void stopPeriodicHealthCheck();

    /**
     * @brief 设置全局重连策略
     * @param enable 是否启用自动重连
     * @param maxAttempts 最大重试次数，0表示无限制
     * @param initialDelayMs 初始重连延迟（毫秒）
     * @param maxDelayMs 最大重连延迟（毫秒）
     */
    void setGlobalReconnectPolicy(bool enable, int maxAttempts = 0,
                                  int initialDelayMs = 1000, int maxDelayMs = 30000);

    /**
     * @brief 析构函数
     */
    ~MQTTClientManager();

private:
    // 私有构造函数
    MQTTClientManager() : healthCheckIntervalSeconds(30),
                          healthCheckRunning(false) {}

    // 禁止拷贝和赋值
    MQTTClientManager(const MQTTClientManager&) = delete;
    MQTTClientManager& operator=(const MQTTClientManager&) = delete;

    // 锁用于保证线程安全
    std::mutex mutex;

    // MQTT客户端映射表
    std::map<std::string, std::shared_ptr<MQTTClientWrapper>> clients;

    // 健康检查相关
    int healthCheckIntervalSeconds;
    std::atomic<bool> healthCheckRunning;
    std::thread healthCheckThread;

    // 健康检查线程函数
    void healthCheckLoop();
};

#endif //FFMPEG_PULL_PUSH_MQTT_SYNC_CLIENT_H