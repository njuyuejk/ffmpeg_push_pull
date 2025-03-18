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
#include "mqttClient.h"
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

    /**
     * @brief 订阅主题
     * @param topic 主题名称
     * @param qos 服务质量
     * @param callback 消息回调函数
     * @return 是否成功订阅
     */
    bool subscribe(const std::string& topic, int qos, MessageCallback callback);

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

    // 连接断开回调函数
    std::function<void(const std::string&)> connectionLostCallback;

    // 静态回调函数，用于MQTT库回调
    static int messageArrivedCallback(void* context, char* topicName, int topicLen, MQTTClient_message* message);
    static void staticconnectionLostCallback(void* context, char* cause);

    // 尝试重新连接
    bool reconnect();
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
     * @brief 析构函数
     */
    ~MQTTClientManager();

private:
    // 私有构造函数
    MQTTClientManager() = default;

    // 禁止拷贝和赋值
    MQTTClientManager(const MQTTClientManager&) = delete;
    MQTTClientManager& operator=(const MQTTClientManager&) = delete;

    // 锁用于保证线程安全
    std::mutex mutex;

    // MQTT客户端映射表
    std::map<std::string, std::shared_ptr<MQTTClientWrapper>> clients;
};

#endif //FFMPEG_PULL_PUSH_MQTT_SYNC_CLIENT_H